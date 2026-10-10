#include "talcite/digest.hpp"

#include <algorithm>
#include <cstddef>
#include <mutex>

#include "talcite/trouble.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <bcrypt.h>

#elif defined(__linux__)

#include <cerrno>
#include <cstring>
#include <fcntl.h>

#include <linux/if_alg.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

namespace talcite {

namespace {

// How many bytes each digest produces. The only cryptographic number in this file; the rest is
// calls to the system.
std::size_t wide(Sum which) {
    switch (which) {
        case Sum::md5: return 16;
        case Sum::sha1: return 20;
        case Sum::sha512: return 64;
        default: return 32;
    }
}

}

#if defined(__linux__) && !defined(_WIN32)

namespace {

// The kernel's name of the algorithm, plain or as HMAC.
const char* named(Sum which, bool keyed) {
    switch (which) {
        case Sum::md5: return keyed ? "hmac(md5)" : "md5";
        case Sum::sha1: return keyed ? "hmac(sha1)" : "sha1";
        case Sum::sha512: return keyed ? "hmac(sha512)" : "sha512";
        default: return keyed ? "hmac(sha256)" : "sha256";
    }
}

}

// The Linux kernel exposes its cryptography as a socket: the AF_ALG family, the algorithm name in
// the address, the key through setsockopt, the content through write and the digest through read.
// For a network library this is unusually natural: the kernel is asked just as for any other
// socket, and not a single line of the algorithm itself is written here.
//
// A system call filter in a container can block the AF_ALG family, and then there are simply no
// digests. summing() says so plainly instead of pretending.
//
// A digest in installments: the socket lives as long as the `Digest`, every installment goes with
// `MSG_MORE`, and the end is an empty send without that flag. A 40 GB file is thus digested in
// pieces, not after being read in whole.
struct Digest::Held {
    Sum which = Sum::sha256;
    int one = -1;
    int two = -1;
    bool over = false;
    std::unique_ptr<Digesting> spare;

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

    // Closes the operation socket and the algorithm socket.
    ~Held() {
        if (two >= 0) ::close(two);
        if (one >= 0) ::close(one);
    }

    // Sends all `much` bytes, repeating after short sends and EINTR; throws `Trouble` on failure.
    void sent(const std::byte* from, std::size_t much, int flags) const {
        std::size_t at = 0;

        do {
            const long went = ::send(two, from + at, much - at, flags);

            if (went < 0) {
                if (errno == EINTR) continue;

                throw Trouble{"Cannot feed the digest.", errno};
            }

            at += static_cast<std::size_t>(went);
        } while (at < much);
    }
};

namespace {

// Opens the algorithm socket for the digest (as HMAC with `key`, when one is given) and accepts
// the operation socket from it. Throws `Trouble` when the kernel has no AF_ALG or does not know
// the algorithm.
std::unique_ptr<Digest::Held> begun(Sum which, const Bytes* key) {
    auto made = std::make_unique<Digest::Held>();

    made->which = which;

    sockaddr_alg where{};

    where.salg_family = AF_ALG;

    std::strncpy(reinterpret_cast<char*>(where.salg_type), "hash", sizeof where.salg_type - 1);
    std::strncpy(reinterpret_cast<char*>(where.salg_name), named(which, key != nullptr),
                 sizeof where.salg_name - 1);

    // The digest sockets must not leak into a child program.
    made->one = ::socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (made->one < 0) throw Trouble{"The system provides no digests.", errno};

    if (::bind(made->one, reinterpret_cast<sockaddr*>(&where), sizeof where) < 0)
        throw Trouble{"The system does not know this digest.", errno};

    if (key != nullptr &&
        ::setsockopt(made->one, SOL_ALG, ALG_SET_KEY, key->data(), static_cast<socklen_t>(key->size())) < 0)
        throw Trouble{"Cannot set the key.", errno};

    made->two = ::accept4(made->one, nullptr, nullptr, SOCK_CLOEXEC);

    if (made->two < 0) throw Trouble{"Cannot start the digest.", errno};

    return made;
}

// Feeds one installment, with `MSG_MORE` so that the kernel waits for more.
void fed(Digest::Held& held, const Bytes& what) {
    if (!what.empty()) held.sent(what.data(), what.size(), MSG_MORE);
}

// Ends the content with an empty send and reads the whole digest back.
Bytes finished(Digest::Held& held) {
    held.sent(nullptr, 0, 0);

    Bytes made(wide(held.which));

    std::size_t got = 0;

    while (got < made.size()) {
        const long read = ::recv(held.two, made.data() + got, made.size() - got, 0);

        if (read < 0) {
            if (errno == EINTR) continue;

            throw Trouble{"Cannot get the digest.", errno};
        }

        if (read == 0) throw Trouble{"The digest broke off halfway."};

        got += static_cast<std::size_t>(read);
    }

    return made;
}

}

#elif defined(_WIN32)

namespace {

// CNG requires opening an algorithm provider, and that is expensive, so the eight handles are
// opened once and kept until the program ends. The handles themselves are stateless and may be
// used from many threads; the state of a computation lives in a separate hash object, created for
// every `Digest`.
struct Providers {
    BCRYPT_ALG_HANDLE plain[4]{};
    BCRYPT_ALG_HANDLE keyed[4]{};
    bool ready = false;
};

// The provider handles, opened on first use; `ready` is false when any of them failed to open.
Providers& providers() {
    static Providers only;
    static std::once_flag once;

    std::call_once(once, [] {
        const LPCWSTR names[4] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM,
                                  BCRYPT_SHA512_ALGORITHM};

        bool ready = true;

        for (int at = 0; at < 4; ++at) {
            if (::BCryptOpenAlgorithmProvider(&only.plain[at], names[at], nullptr, 0) < 0)
                ready = false;

            if (::BCryptOpenAlgorithmProvider(&only.keyed[at], names[at], nullptr,
                                              BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
                ready = false;
        }

        only.ready = ready;
    });

    return only;
}

}

struct Digest::Held {
    Sum which = Sum::sha256;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool over = false;
    std::unique_ptr<Digesting> spare;

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

    // Destroys the CNG hash object.
    ~Held() {
        if (hash != nullptr) ::BCryptDestroyHash(hash);
    }
};

namespace {

// Creates a CNG hash object for the digest (as HMAC with `key`, when one is given). Throws
// `Trouble` when the providers could not be opened or the object cannot be created.
std::unique_ptr<Digest::Held> begun(Sum which, const Bytes* key) {
    Providers& how = providers();

    if (!how.ready) throw Trouble{"The system provides no digests."};

    auto made = std::make_unique<Digest::Held>();

    made->which = which;

    BCRYPT_ALG_HANDLE alg =
        key != nullptr ? how.keyed[static_cast<int>(which)] : how.plain[static_cast<int>(which)];

    const NTSTATUS why = ::BCryptCreateHash(
        alg, &made->hash, nullptr, 0,
        key != nullptr ? reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key->data())) : nullptr,
        key != nullptr ? static_cast<ULONG>(key->size()) : 0, 0);

    if (why < 0) {
        made->hash = nullptr;

        throw Trouble{"Cannot start the digest.", static_cast<int>(why)};
    }

    return made;
}

// The length is a `ULONG`, so an installment over 4 GB goes in several calls.
void fed(Digest::Held& held, const Bytes& what) {
    for (std::size_t at = 0; at < what.size();) {
        const auto much = static_cast<ULONG>(std::min<std::size_t>(what.size() - at, 0x40000000u));

        const NTSTATUS why =
            ::BCryptHashData(held.hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(what.data() + at)), much, 0);

        if (why < 0) throw Trouble{"Cannot feed the digest.", static_cast<int>(why)};

        at += much;
    }
}

// Finishes the hash object and returns the digest.
Bytes finished(Digest::Held& held) {
    Bytes made(wide(held.which));

    const NTSTATUS why = ::BCryptFinishHash(held.hash, reinterpret_cast<PUCHAR>(made.data()),
                                            static_cast<ULONG>(made.size()), 0);

    if (why < 0) throw Trouble{"Cannot get the digest.", static_cast<int>(why)};

    return made;
}

}

#else

struct Digest::Held {
    Sum which = Sum::sha256;
    bool over = false;
    std::unique_ptr<Digesting> spare;
};

namespace {

// Without a system source of digests there are none; always throws `Trouble`.
std::unique_ptr<Digest::Held> begun(Sum, const Bytes*) { throw Trouble{"The system provides no digests."}; }

void fed(Digest::Held&, const Bytes&) {}

// Never reached, because `begun` always throws; returns zeros of the digest's length.
Bytes finished(Digest::Held& held) { return Bytes(wide(held.which)); }

}

#endif

namespace {

// A spare for when the system gives no digests (`spared`). The Linux kernel is sometimes without
// AF_ALG (a stripped-down virtual machine kernel, a system call filter in a container), and then
// the program that assembles the application can supply digests from another system source,
// usually libcrypto, which carries its TLS anyway (the openssl adapter). The spare is used only
// when the system refused; the system is asked once.
std::mutex spare_guard;
std::shared_ptr<DigestProvider> spare_held;

// The spare digest provider given to `spared`, or null.
std::shared_ptr<DigestProvider> spare() {
    const std::lock_guard<std::mutex> kept{spare_guard};

    return spare_held;
}

// Whether the system digests work, checked once with a trial SHA-1 of nothing.
bool system_ready() {
    static const bool once = [] {
        try {
            const std::unique_ptr<Digest::Held> held = begun(Sum::sha1, nullptr);

            return finished(*held).size() == 20;
        } catch (const std::exception&) {
            return false;
        }
    }();

    return once;
}

// The state of a new digest: from the system when it works, otherwise from the spare. Throws
// `Trouble` when neither can compute it.
std::unique_ptr<Digest::Held> made(Sum which, const Bytes* key) {
    if (system_ready()) return begun(which, key);

    const std::shared_ptr<DigestProvider> other = spare();

    if (!other) throw Trouble{"The system provides no digests."};

    auto held = std::make_unique<Digest::Held>();

    held->which = which;
    held->spare = other->started(which, key);

    if (!held->spare) throw Trouble{"The spare digests do not know this one."};

    return held;
}

}

// Sets the spare digest provider used when the system has none; null removes it.
void spared(std::shared_ptr<DigestProvider> one) {
    const std::lock_guard<std::mutex> kept{spare_guard};

    spare_held = std::move(one);
}

// Starts a plain digest of the given kind.
Digest::Digest(Sum which) : held_(made(which, nullptr)) {}

// Starts an HMAC of the given kind, with `key`.
Digest::Digest(Sum which, const Bytes& key) : held_(made(which, &key)) {}

Digest::~Digest() = default;

Digest::Digest(Digest&& other) noexcept = default;

Digest& Digest::operator=(Digest&& other) noexcept = default;

// Feeds the next installment of the content; throws `Trouble` once the digest is finished.
void Digest::feed(const Bytes& what) {
    if (!held_ || held_->over) throw Trouble{"The digest is finished."};

    if (held_->spare) held_->spare->feed(what);
    else fed(*held_, what);
}

// Finishes the digest and returns it; after that the digest takes nothing more. Throws `Trouble`
// when called twice or when the spare gives a result of the wrong length.
Bytes Digest::done() {
    if (!held_ || held_->over) throw Trouble{"The digest is finished."};

    held_->over = true;

    if (held_->spare) {
        Bytes made = held_->spare->done();

        if (made.size() != wide(held_->which)) throw Trouble{"The spare digest has the wrong length."};

        return made;
    }

    return finished(*held_);
}

// Whether digests exist at all: from the system (asked once; on Linux the answer depends on the
// kernel and the system call filter, and those do not change while the program runs) or from the
// spare, if one was given.
bool summing() { return system_ready() || spare() != nullptr; }

// The digest of `what`, in one call.
Bytes summed(Sum which, const Bytes& what) {
    Digest made{which};

    made.feed(what);

    return made.done();
}

// The HMAC of `what` with `key`, in one call.
Bytes stamped(Sum which, const Bytes& key, const Bytes& what) {
    Digest made{which, key};

    made.feed(what);

    return made.done();
}

// HKDF (RFC 5869) on the system's HMAC: extraction (`salt` as the key, the secret as the content)
// and expansion in blocks T(i) = HMAC(PRK, T(i-1) | info | i). The algorithm is not written here:
// it is calls to `stamped` in a loop, and the byte count is limited, as in the RFC, to 255 digest
// blocks. The RFC says to replace an empty salt with zeros of the digest's length; HMAC pads the
// key with zeros to the block length, so an empty salt and those zeros are the same key and
// nothing needs replacing here.
Bytes derived(Sum which, const Bytes& salt, const Bytes& secret, const Bytes& info, std::size_t much) {
    const std::size_t block = wide(which);

    if (much > 255 * block) throw Trouble{"HKDF cannot give that many bytes."};

    const Bytes key = stamped(which, salt, secret);

    Bytes made;
    Bytes last;

    for (std::size_t round = 1; made.size() < much; ++round) {
        Bytes input = last;

        input.insert(input.end(), info.begin(), info.end());
        input.push_back(static_cast<std::byte>(round));

        last = stamped(which, key, input);

        made.insert(made.end(), last.begin(), last.end());
    }

    made.resize(much);

    return made;
}

}

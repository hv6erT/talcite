#include "../summing.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#include "tal/trouble.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/if_alg.h>
#include <sys/random.h>
#include <sys/socket.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <stdlib.h>
#define TAL_RANDOM_ARC4 1
#endif
#endif

// The system's side of cryptography, ported from talcite's digest.cpp and random.cpp: digests and
// HMAC from the kernel's AF_ALG on Linux and from CNG on Windows, and randomness from the system's
// own source, never from a generator of the program. Other systems give no digests here yet.
namespace tal::crypto::summing {

// How many bytes the digest `which` gives.
std::size_t wide(Sum which) {
    switch (which) {
        case Sum::md5: return 16;
        case Sum::sha1: return 20;
        case Sum::sha256: return 32;
        case Sum::sha512: return 64;
    }

    return 32;
}

#if defined(__linux__)

namespace {

// The kernel's name of the digest `which`, plain or as HMAC (`keyed`).
const char* named(Sum which, bool keyed) {
    switch (which) {
        case Sum::md5: return keyed ? "hmac(md5)" : "md5";
        case Sum::sha1: return keyed ? "hmac(sha1)" : "sha1";
        case Sum::sha512: return keyed ? "hmac(sha512)" : "sha512";
        case Sum::sha256: break;
    }

    return keyed ? "hmac(sha256)" : "sha256";
}

// A digest of the kernel, asked as a socket: the algorithm in the address, the key through an
// option, the content sent in pieces marked that more comes, the digest read at the end, which
// ends the content too.
class Kernel final : public Machine {
public:
    // The sockets of the algorithm and of the operation, and the width of the digest.
    Kernel(int algorithm, int operation, std::size_t width) : algorithm_(algorithm), operation_(operation), width_(width) {}

    // Closes both sockets.
    ~Kernel() override {
        (void)::close(operation_);
        (void)::close(algorithm_);
    }

    Kernel(const Kernel&) = delete;
    Kernel& operator=(const Kernel&) = delete;

    // Sends all of `what`, saying that more comes, after short sends and interruptions.
    void feed(std::span<const std::byte> what) override {
        while (!what.empty()) {
            const ssize_t much = ::send(operation_, what.data(), what.size(), MSG_MORE);

            if (much < 0 && errno == EINTR) continue;

            if (much < 0) throw Trouble{Kind::broken, "Cannot feed the digest.", {}, errno};

            what = what.subspan(static_cast<std::size_t>(much));
        }
    }

    // Reads the digest; the kernel ends the content it was told more would come to.
    Bytes done() override {
        Bytes made(width_);
        std::size_t got = 0;

        while (got < made.size()) {
            const ssize_t much = ::recv(operation_, made.data() + got, made.size() - got, 0);

            if (much < 0 && errno == EINTR) continue;

            if (much <= 0) throw Trouble{Kind::broken, "Cannot get the digest.", {}, much < 0 ? errno : 0};

            got += static_cast<std::size_t>(much);
        }

        return made;
    }

private:
    int algorithm_;
    int operation_;
    std::size_t width_;
};

}

// The kernel's digest of the kind `which`, an HMAC with `key` when one is given; none when the
// kernel has no AF_ALG at all, as in a container whose filter blocks it.
std::unique_ptr<Machine> system(Sum which, Key key) {
    const int algorithm = ::socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (algorithm < 0) return nullptr;

    sockaddr_alg where{};

    where.salg_family = AF_ALG;

    std::strncpy(reinterpret_cast<char*>(where.salg_type), "hash", sizeof where.salg_type - 1);
    std::strncpy(reinterpret_cast<char*>(where.salg_name), named(which, key.has_value()), sizeof where.salg_name - 1);

    const auto refused = [algorithm](const char* said) {
        const int why = errno;

        (void)::close(algorithm);

        throw Trouble{Kind::unsupported, said, {}, why};
    };

    if (::bind(algorithm, reinterpret_cast<sockaddr*>(&where), sizeof where) < 0) refused("The system does not know this digest.");

    if (key.has_value() && ::setsockopt(algorithm, SOL_ALG, ALG_SET_KEY, key->data(), static_cast<socklen_t>(key->size())) < 0)
        refused("The system takes no such key.");

    const int operation = ::accept4(algorithm, nullptr, nullptr, SOCK_CLOEXEC);

    if (operation < 0) refused("The system cannot start the digest.");

    return std::make_unique<Kernel>(algorithm, operation, wide(which));
}

#elif defined(_WIN32)

namespace {

// The providers of CNG, opened once, plain and for HMAC, for the four digests in the order of
// `Sum`; stateless and shared by all threads. None when any could not be opened.
struct Providers {
    BCRYPT_ALG_HANDLE plain[4]{};
    BCRYPT_ALG_HANDLE keyed[4]{};
    bool ready = false;
};

// The providers, opened at the first digest.
Providers& providers() {
    static Providers only;
    static std::once_flag once;

    std::call_once(once, [] {
        const LPCWSTR names[4] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA512_ALGORITHM};
        bool ready = true;

        for (int at = 0; at < 4; ++at) {
            if (BCryptOpenAlgorithmProvider(&only.plain[at], names[at], nullptr, 0) < 0) ready = false;
            if (BCryptOpenAlgorithmProvider(&only.keyed[at], names[at], nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) ready = false;
        }

        only.ready = ready;
    });

    return only;
}

// A digest of CNG: one hash object for its whole life.
class Hashed final : public Machine {
public:
    // The hash object `hash`, of a digest `width` bytes wide.
    Hashed(BCRYPT_HASH_HANDLE hash, std::size_t width) : hash_(hash), width_(width) {}

    // Destroys the hash object.
    ~Hashed() override { (void)BCryptDestroyHash(hash_); }

    Hashed(const Hashed&) = delete;
    Hashed& operator=(const Hashed&) = delete;

    // Takes in `what`, in calls of at most 1 GiB, since CNG takes the length as a ULONG.
    void feed(std::span<const std::byte> what) override {
        while (!what.empty()) {
            const auto much = static_cast<ULONG>(std::min<std::size_t>(what.size(), 0x40000000U));
            const NTSTATUS why = BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(what.data())), much, 0);

            if (why < 0) throw Trouble{Kind::broken, "Cannot feed the digest.", {}, static_cast<int>(why)};

            what = what.subspan(much);
        }
    }

    // The digest.
    Bytes done() override {
        Bytes made(width_);
        const NTSTATUS why = BCryptFinishHash(hash_, reinterpret_cast<PUCHAR>(made.data()), static_cast<ULONG>(made.size()), 0);

        if (why < 0) throw Trouble{Kind::broken, "Cannot get the digest.", {}, static_cast<int>(why)};

        return made;
    }

private:
    BCRYPT_HASH_HANDLE hash_;
    std::size_t width_;
};

}

// CNG's digest of the kind `which`, an HMAC with `key` when one is given; none when the providers
// could not be opened.
std::unique_ptr<Machine> system(Sum which, Key key) {
    Providers& how = providers();

    if (!how.ready) return nullptr;

    const auto at = static_cast<std::size_t>(which);
    BCRYPT_HASH_HANDLE hash = nullptr;
    const NTSTATUS why = BCryptCreateHash(key.has_value() ? how.keyed[at] : how.plain[at], &hash, nullptr, 0,
                                          key.has_value() ? reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key->data())) : nullptr,
                                          key.has_value() ? static_cast<ULONG>(key->size()) : 0, 0);

    if (why < 0) throw Trouble{Kind::unsupported, "The system cannot start the digest.", {}, static_cast<int>(why)};

    return std::make_unique<Hashed>(hash, wide(which));
}

#else

// No digests of the system here yet.
std::unique_ptr<Machine> system(Sum which, Key key) {
    (void)which;
    (void)key;

    return nullptr;
}

#endif

}

namespace tal::crypto {

#if !defined(_WIN32) && !defined(TAL_RANDOM_ARC4)

namespace {

// Fills `made` from /dev/urandom.
[[maybe_unused]] void urandom(Bytes& made) {
    int handle = -1;

    do {
        handle = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (handle < 0 && errno == EINTR);

    if (handle < 0) throw Trouble{Kind::unsupported, "The system gives no randomness.", {}, errno};

    std::size_t at = 0;

    while (at < made.size()) {
        const ssize_t got = ::read(handle, made.data() + at, made.size() - at);

        if (got < 0 && errno == EINTR) continue;

        if (got <= 0) {
            const int why = got < 0 ? errno : 0;

            (void)::close(handle);

            throw Trouble{Kind::broken, "The system gives no randomness.", {}, why};
        }

        at += static_cast<std::size_t>(got);
    }

    (void)::close(handle);
}

}

#endif

// `much` bytes of the system's cryptographic randomness: BCryptGenRandom on Windows, getrandom on
// Linux (/dev/urandom on a kernel without it), arc4random_buf on macOS and the BSDs, /dev/urandom
// elsewhere. Never a generator of the program, whose state a few of its outputs would give away.
Bytes random(std::size_t much) {
    Bytes made(much);

#ifdef _WIN32
    for (std::size_t at = 0; at < made.size();) {
        const auto part = static_cast<ULONG>(std::min<std::size_t>(made.size() - at, 0x40000000U));
        const NTSTATUS why = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(made.data() + at), part, BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        if (why < 0) throw Trouble{Kind::broken, "The system gives no randomness.", {}, static_cast<int>(why)};

        at += part;
    }
#elif defined(__linux__)
    std::size_t at = 0;

    while (at < made.size()) {
        const ssize_t got = ::getrandom(made.data() + at, made.size() - at, 0);

        if (got < 0 && errno == EINTR) continue;

        if (got < 0 && errno == ENOSYS) {
            urandom(made);

            return made;
        }

        if (got < 0) throw Trouble{Kind::broken, "The system gives no randomness.", {}, errno};

        at += static_cast<std::size_t>(got);
    }
#elif defined(TAL_RANDOM_ARC4)
    ::arc4random_buf(made.data(), made.size());
#else
    urandom(made);
#endif

    return made;
}

}

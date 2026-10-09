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

// Ile bajtów oddaje każdy skrót. Jedyna liczba o kryptografii, jaka stoi w tym pliku — reszta to
// wołania do systemu.
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

const char* named(Sum which, bool keyed) {
    switch (which) {
        case Sum::md5: return keyed ? "hmac(md5)" : "md5";
        case Sum::sha1: return keyed ? "hmac(sha1)" : "sha1";
        case Sum::sha512: return keyed ? "hmac(sha512)" : "sha512";
        default: return keyed ? "hmac(sha256)" : "sha256";
    }
}

}

// Jądro Linuksa wystawia swoją kryptografię **jako gniazdo**: rodzina AF_ALG, nazwa algorytmu
// w adresie, klucz przez setsockopt, treść przez write, skrót przez read. Dla biblioteki sieciowej
// to wyjątkowo naturalne — pytamy jądro tak samo jak o każde inne gniazdo, i nie piszemy ani jednej
// linii samego algorytmu.
//
// Rodzinę AF_ALG potrafi zablokować filtr wywołań systemowych w kontenerze, i wtedy skrótów po
// prostu nie ma. Mówimy o tym wprost przez summing(), zamiast udawać.
//
// Skrót ratami: gniazdo żyje tyle, co `Digest`, każda rata idzie z `MSG_MORE`, a koniec to pusta
// wysyłka bez tej flagi. Plik 40 GB liczy się więc kawałkami, a nie po wczytaniu w całości.
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

    ~Held() {
        if (two >= 0) ::close(two);
        if (one >= 0) ::close(one);
    }

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

std::unique_ptr<Digest::Held> begun(Sum which, const Bytes* key) {
    auto made = std::make_unique<Digest::Held>();

    made->which = which;

    sockaddr_alg where{};

    where.salg_family = AF_ALG;

    std::strncpy(reinterpret_cast<char*>(where.salg_type), "hash", sizeof where.salg_type - 1);
    std::strncpy(reinterpret_cast<char*>(where.salg_name), named(which, key != nullptr),
                 sizeof where.salg_name - 1);

    // Gniazda liczące skrót nie mają trafiać do programu potomnego.
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

void fed(Digest::Held& held, const Bytes& what) {
    if (!what.empty()) held.sent(what.data(), what.size(), MSG_MORE);
}

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

// CNG każe otworzyć dostawcę algorytmu, a to jest drogie — więc otwieramy osiem uchwytów raz
// i trzymamy do końca programu. Same uchwyty są bezstanowe i wolno ich używać z wielu wątków;
// stan liczenia siedzi w osobnym obiekcie skrótu, tworzonym na każdy `Digest`.
struct Providers {
    BCRYPT_ALG_HANDLE plain[4]{};
    BCRYPT_ALG_HANDLE keyed[4]{};
    bool ready = false;
};

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

    ~Held() {
        if (hash != nullptr) ::BCryptDestroyHash(hash);
    }
};

namespace {

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

// `ULONG` na długość — rata ponad 4 GB idzie w kilku podejściach.
void fed(Digest::Held& held, const Bytes& what) {
    for (std::size_t at = 0; at < what.size();) {
        const auto much = static_cast<ULONG>(std::min<std::size_t>(what.size() - at, 0x40000000u));

        const NTSTATUS why =
            ::BCryptHashData(held.hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(what.data() + at)), much, 0);

        if (why < 0) throw Trouble{"Cannot feed the digest.", static_cast<int>(why)};

        at += much;
    }
}

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

std::unique_ptr<Digest::Held> begun(Sum, const Bytes*) { throw Trouble{"The system provides no digests."}; }

void fed(Digest::Held&, const Bytes&) {}

Bytes finished(Digest::Held& held) { return Bytes(wide(held.which)); }

}

#endif

namespace {

// **Zapas, gdy system skrótów nie daje** (`spared`). Jądro Linuksa bywa bez AF_ALG — okrojone jądro
// maszyny wirtualnej, filtr wywołań w kontenerze — i wtedy program, który składa aplikację, może
// podać skróty z innego źródła systemu, zwykle z libcrypto, która i tak niesie jego TLS
// (przejściówka `openssl`). Zapas idzie w ruch tylko wtedy, gdy system odmówił — system pytamy raz.
std::mutex spare_guard;
std::shared_ptr<DigestProvider> spare_held;

std::shared_ptr<DigestProvider> spare() {
    const std::lock_guard<std::mutex> kept{spare_guard};

    return spare_held;
}

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

void spared(std::shared_ptr<DigestProvider> one) {
    const std::lock_guard<std::mutex> kept{spare_guard};

    spare_held = std::move(one);
}

Digest::Digest(Sum which) : held_(made(which, nullptr)) {}

Digest::Digest(Sum which, const Bytes& key) : held_(made(which, &key)) {}

Digest::~Digest() = default;

Digest::Digest(Digest&& other) noexcept = default;

Digest& Digest::operator=(Digest&& other) noexcept = default;

void Digest::feed(const Bytes& what) {
    if (!held_ || held_->over) throw Trouble{"The digest is finished."};

    if (held_->spare) held_->spare->feed(what);
    else fed(*held_, what);
}

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

// Czy skróty w ogóle są: od systemu (pytanego raz — na Linuksie odpowiedź zależy od jądra i filtra
// wywołań, a te się w trakcie biegu nie zmieniają) albo z zapasu, jeśli go podano.
bool summing() { return system_ready() || spare() != nullptr; }

Bytes summed(Sum which, const Bytes& what) {
    Digest made{which};

    made.feed(what);

    return made.done();
}

Bytes stamped(Sum which, const Bytes& key, const Bytes& what) {
    Digest made{which, key};

    made.feed(what);

    return made.done();
}

// **HKDF** (RFC 5869) na HMAC z systemu: wyciągnięcie (`salt` jako klucz, sekret jako treść) i
// rozwinięcie blokami `T(i) = HMAC(PRK, T(i-1) | info | i)`. Algorytmu nie piszemy — to trzy
// wołania `stamped` w pętli; liczba bajtów ograniczona jak w RFC do 255 bloków skrótu. Pustą sól RFC
// każe zastąpić zerami o długości skrótu — HMAC dopełnia klucz zerami do długości bloku, więc pusta
// sól i te zera to ten sam klucz i nic tu podmieniać nie trzeba.
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

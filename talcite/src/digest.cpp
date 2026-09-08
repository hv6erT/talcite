#include "talcite/digest.hpp"

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
        default: return 32;
    }
}

#if defined(__linux__) && !defined(_WIN32)

const char* named(Sum which, bool keyed) {
    switch (which) {
        case Sum::md5: return keyed ? "hmac(md5)" : "md5";
        case Sum::sha1: return keyed ? "hmac(sha1)" : "sha1";
        default: return keyed ? "hmac(sha256)" : "sha256";
    }
}

// Jądro Linuksa wystawia swoją kryptografię **jako gniazdo**: rodzina AF_ALG, nazwa algorytmu
// w adresie, klucz przez setsockopt, treść przez write, skrót przez read. Dla biblioteki sieciowej
// to wyjątkowo naturalne — pytamy jądro tak samo jak o każde inne gniazdo, i nie piszemy ani jednej
// linii samego algorytmu.
//
// Rodzinę AF_ALG potrafi zablokować filtr wywołań systemowych w kontenerze, i wtedy skrótów po
// prostu nie ma. Mówimy o tym wprost przez summing(), zamiast udawać.
Bytes asking(Sum which, const Bytes* key, const Bytes& what) {
    sockaddr_alg where{};

    where.salg_family = AF_ALG;

    std::strncpy(reinterpret_cast<char*>(where.salg_type), "hash", sizeof where.salg_type - 1);
    std::strncpy(reinterpret_cast<char*>(where.salg_name), named(which, key != nullptr),
                 sizeof where.salg_name - 1);

    const int one = ::socket(AF_ALG, SOCK_SEQPACKET, 0);

    if (one < 0) throw net::Trouble{"System nie daje skrótów.", errno};

    // Gniazda liczące skrót nie mają trafiać do programu potomnego.
    ::fcntl(one, F_SETFD, ::fcntl(one, F_GETFD, 0) | FD_CLOEXEC);

    struct Closing {
        int one;
        int two = -1;

        ~Closing() {
            if (two >= 0) ::close(two);

            ::close(one);
        }
    } closing{one};

    if (::bind(one, reinterpret_cast<sockaddr*>(&where), sizeof where) < 0)
        throw net::Trouble{"System nie zna tego skrótu.", errno};

    if (key != nullptr &&
        ::setsockopt(one, SOL_ALG, ALG_SET_KEY, key->data(), static_cast<socklen_t>(key->size())) <
            0)
        throw net::Trouble{"Nie mogę podać klucza.", errno};

    closing.two = ::accept(one, nullptr, nullptr);

    if (closing.two < 0) throw net::Trouble{"Nie mogę zacząć liczyć skrótu.", errno};

    ::fcntl(closing.two, F_SETFD, ::fcntl(closing.two, F_GETFD, 0) | FD_CLOEXEC);

    std::size_t at = 0;

    do {
        const long went = ::send(closing.two, what.data() + at, what.size() - at,
                                 at + 1 < what.size() ? MSG_MORE : 0);

        if (went < 0) {
            if (errno == EINTR) continue;

            throw net::Trouble{"Nie mogę podać treści do skrótu.", errno};
        }

        at += static_cast<std::size_t>(went);
    } while (at < what.size());

    Bytes made(wide(which));

    std::size_t got = 0;

    while (got < made.size()) {
        const long read = ::recv(closing.two, made.data() + got, made.size() - got, 0);

        if (read < 0) {
            if (errno == EINTR) continue;

            throw net::Trouble{"Nie mogę odebrać skrótu.", errno};
        }

        if (read == 0) throw net::Trouble{"Skrót urwał się w połowie."};

        got += static_cast<std::size_t>(read);
    }

    return made;
}

#elif defined(_WIN32)

// CNG każe otworzyć dostawcę algorytmu, a to jest drogie — więc otwieramy sześć uchwytów raz
// i trzymamy do końca programu. Same uchwyty są bezstanowe i wolno ich używać z wielu wątków;
// stan liczenia siedzi w osobnym obiekcie skrótu, tworzonym na każde wywołanie.
struct Providers {
    BCRYPT_ALG_HANDLE plain[3]{};
    BCRYPT_ALG_HANDLE keyed[3]{};
    bool ready = false;
};

Providers& providers() {
    static Providers only;
    static std::once_flag once;

    std::call_once(once, [] {
        const LPCWSTR names[3] = {BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM,
                                  BCRYPT_SHA256_ALGORITHM};

        bool ready = true;

        for (int at = 0; at < 3; ++at) {
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

Bytes asking(Sum which, const Bytes* key, const Bytes& what) {
    Providers& how = providers();

    if (!how.ready) throw net::Trouble{"System nie daje skrótów."};

    BCRYPT_ALG_HANDLE alg =
        key != nullptr ? how.keyed[static_cast<int>(which)] : how.plain[static_cast<int>(which)];

    BCRYPT_HASH_HANDLE hash = nullptr;

    NTSTATUS why = ::BCryptCreateHash(
        alg, &hash, nullptr, 0,
        key != nullptr ? reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key->data())) : nullptr,
        key != nullptr ? static_cast<ULONG>(key->size()) : 0, 0);

    if (why < 0) throw net::Trouble{"Nie mogę zacząć liczyć skrótu.", static_cast<int>(why)};

    struct Closing {
        BCRYPT_HASH_HANDLE one;

        ~Closing() { ::BCryptDestroyHash(one); }
    } const closing{hash};

    if (!what.empty()) {
        why = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(what.data())),
                               static_cast<ULONG>(what.size()), 0);

        if (why < 0) throw net::Trouble{"Nie mogę podać treści do skrótu.", static_cast<int>(why)};
    }

    Bytes made(wide(which));

    why = ::BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(made.data()),
                             static_cast<ULONG>(made.size()), 0);

    if (why < 0) throw net::Trouble{"Nie mogę odebrać skrótu.", static_cast<int>(why)};

    return made;
}

#else

Bytes asking(Sum, const Bytes*, const Bytes&) {
    throw net::Trouble{"System nie daje skrótów."};
}

#endif

}

// Czy ten system w ogóle daje skróty. Pytamy raz i zapamiętujemy: na Linuksie odpowiedź zależy od
// filtra wywołań systemowych, a ten się w trakcie biegu nie zmienia.
bool summing() {
    static bool once = [] {
        try {
            return summed(Sum::sha1, Bytes{}).size() == 20;
        } catch (const std::exception&) {
            return false;
        }
    }();

    return once;
}

Bytes summed(Sum which, const Bytes& what) { return asking(which, nullptr, what); }

Bytes stamped(Sum which, const Bytes& key, const Bytes& what) {
    return asking(which, &key, what);
}

}

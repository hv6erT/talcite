#include "talcite/random.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>

#include "talcite/trouble.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <bcrypt.h>

#else

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__) && !defined(TALCITE_GENERIC_POSIX)
#include <sys/random.h>
#define TALCITE_RANDOM_GETRANDOM 1
#elif (defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
       defined(__DragonFly__)) && !defined(TALCITE_GENERIC_POSIX)
#include <stdlib.h>
#define TALCITE_RANDOM_ARC4 1
#endif

#endif

namespace talcite {

// Losowość bierzemy **wprost od systemu**, a nie przez `std::random_device`. Standard nie obiecuje,
// że tamto urządzenie jest losowe — wolno mu być zwykłym generatorem deterministycznym, a MinGW
// przez lata oddawał nim ten sam ciąg przy każdym uruchomieniu. Z wnętrza programu sprawdzić się
// tego nie da: `entropy()` bywa zerowe także tam, gdzie źródło jest prawdziwe. Systemowe źródło
// jest za to jedno na system i dobrze opisane:
//
// - Windows: `BCryptGenRandom` z generatorem wybranym przez system — ten sam CNG, który daje skróty,
//   więc nic nowego się nie linkuje;
// - Linux: getrandom, który czeka tylko raz, na zasilenie puli po starcie, a potem nie czeka nigdy;
//   jądro starsze niż 3.17 go nie zna i wtedy czytamy /dev/urandom;
// - macOS i BSD: arc4random_buf, który nie zawodzi;
// - reszta POSIX-u: /dev/urandom.
//
// **Generatora pseudolosowego tu nie ma i nie może być.** `mt19937` powstał do symulacji: jego stan
// odtwarza się z trzystu dwunastu kolejnych wyników. Kto zobaczy tyle liczb, zna wszystkie następne
// — a numer transakcji STUN jest **jedynym dowodem tożsamości odpowiedzi**.

namespace {

#ifndef _WIN32

[[maybe_unused]] void urandom(Bytes& made) {
    int handle = -1;

    do {
        handle = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (handle < 0 && errno == EINTR);

    if (handle < 0) throw Trouble{"The system gave no randomness.", errno};

    std::size_t at = 0;

    while (at < made.size()) {
        const ssize_t got = ::read(handle, made.data() + at, made.size() - at);

        if (got < 0 && errno == EINTR) continue;

        if (got <= 0) {
            const int why = got < 0 ? errno : 0;

            ::close(handle);

            throw Trouble{"The system gave no randomness.", why};
        }

        at += static_cast<std::size_t>(got);
    }

    ::close(handle);
}

#endif

}

Bytes random(std::size_t much) {
    Bytes made(much);

    if (much == 0) return made;

#ifdef _WIN32
    // `ULONG` na długość — prośba ponad 4 GB idzie w kilku podejściach.
    for (std::size_t at = 0; at < made.size();) {
        const auto part = static_cast<ULONG>(std::min<std::size_t>(made.size() - at, 0x40000000u));

        const NTSTATUS why = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(made.data() + at), part,
                                               BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        if (why < 0) throw Trouble{"The system gave no randomness.", static_cast<int>(why)};

        at += part;
    }
#elif defined(TALCITE_RANDOM_GETRANDOM)
    // getrandom bez flag oddaje najwyżej 32 MiB naraz i potrafi oddać mniej, niż poproszono — stąd
    // pętla dobierająca resztę.
    std::size_t at = 0;

    while (at < made.size()) {
        const ssize_t got = ::getrandom(made.data() + at, made.size() - at, 0);

        if (got < 0) {
            if (errno == EINTR) continue;

            if (errno == ENOSYS) {
                urandom(made);

                return made;
            }

            throw Trouble{"The system gave no randomness.", errno};
        }

        at += static_cast<std::size_t>(got);
    }
#elif defined(TALCITE_RANDOM_ARC4)
    ::arc4random_buf(made.data(), made.size());
#else
    urandom(made);
#endif

    return made;
}

}

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

// Randomness is taken directly from the system, not through std::random_device. The standard does
// not promise that that device is random: it may be an ordinary deterministic generator, and MinGW
// for years returned the same sequence from it at every run. This cannot be checked from inside a
// program: entropy() is sometimes zero even where the source is real. The system source, on the
// other hand, is one per system and well documented:
//
// - Windows: `BCryptGenRandom` with the generator chosen by the system, the same CNG that provides
//   the hashes, so nothing new is linked;
// - Linux: getrandom, which waits only once, for the pool to be seeded after boot, and then never
//   waits; a kernel older than 3.17 does not know it, and then we read /dev/urandom;
// - macOS and BSD: arc4random_buf, which does not fail;
// - the rest of POSIX: /dev/urandom.
//
// There is no pseudorandom generator here and there must not be one. mt19937 was made for
// simulations: its state can be reconstructed from 312 consecutive outputs. Whoever sees that many
// numbers knows all the following ones, and the STUN transaction number is the only proof of an
// answer's identity.

namespace {

#ifndef _WIN32

// Fills `made` from /dev/urandom; throws `Trouble` when it cannot be opened or read.
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

// `much` bytes from the system's cryptographic randomness; throws `Trouble` when the system gives
// none.
Bytes random(std::size_t much) {
    Bytes made(much);

    if (much == 0) return made;

#ifdef _WIN32
    // The length is a ULONG, so a request above 4 GB goes in several rounds.
    for (std::size_t at = 0; at < made.size();) {
        const auto part = static_cast<ULONG>(std::min<std::size_t>(made.size() - at, 0x40000000u));

        const NTSTATUS why = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(made.data() + at), part,
                                               BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        if (why < 0) throw Trouble{"The system gave no randomness.", static_cast<int>(why)};

        at += part;
    }
#elif defined(TALCITE_RANDOM_GETRANDOM)
    // getrandom without flags returns at most 32 MiB at a time and can return less than requested,
    // hence the loop that fetches the rest.
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

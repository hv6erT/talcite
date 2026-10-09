#pragma once

#ifndef _WIN32

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace talcite::sealed {

// **Deskryptory zamykane przy uruchomieniu programu potomnego — od urodzenia.** Ustawienie
// `FD_CLOEXEC` osobnym `fcntl` zostawia szparę: drugi wątek, który akurat uruchamia program
// (`Child` biegnie na puli), dziedziczy deskryptor sprzed ustawienia. Dla rury dziecka znaczyło to,
// że jej koniec trzymał także drugi, równoległy program — i pierwszy nie widział końca wejścia,
// dopóki drugi żył. Linux i BSD dają znacznik w samym wywołaniu; macOS nie ma `pipe2`
// ani `SOCK_CLOEXEC`, więc tam zostaje dawna droga.

inline bool piped(int both[2], bool quiet) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return ::pipe2(both, O_CLOEXEC | (quiet ? O_NONBLOCK : 0)) == 0;
#else
    if (::pipe(both) != 0) return false;

    for (int at = 0; at < 2; ++at) {
        ::fcntl(both[at], F_SETFD, ::fcntl(both[at], F_GETFD, 0) | FD_CLOEXEC);

        if (quiet) ::fcntl(both[at], F_SETFL, ::fcntl(both[at], F_GETFL, 0) | O_NONBLOCK);
    }

    return true;
#endif
}

inline int socketed(int family, int type) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    return ::socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
    const int made = ::socket(family, type, 0);

    if (made < 0) return made;

    ::fcntl(made, F_SETFL, ::fcntl(made, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);

    return made;
#endif
}

inline int accepted(int one, sockaddr* told, socklen_t* much) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK) && !defined(__APPLE__)
    return ::accept4(one, told, much, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    const int made = ::accept(one, told, much);

    if (made < 0) return made;

    ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);
    ::fcntl(made, F_SETFL, ::fcntl(made, F_GETFL, 0) | O_NONBLOCK);

    return made;
#endif
}

}

#endif

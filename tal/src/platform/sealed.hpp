#pragma once

#ifndef _WIN32

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tal::sealed {

// Descriptors that are closed on exec of a child program from the moment they are created.
// Setting `FD_CLOEXEC` with a separate `fcntl` leaves a gap: another thread that happens to be
// starting a program (`Child` runs on the pool) inherits the descriptor from before the flag is
// set. For a child's pipe this means a second, parallel program also holds its end, and the first
// does not see the end of its input while the second lives. Linux and the BSDs take the flag in
// the call itself; macOS has neither `pipe2` nor `SOCK_CLOEXEC`, so there the separate `fcntl`
// remains.

// Creates a pipe whose both ends are close-on-exec, and non-blocking when `quiet`; false on
// failure.
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

// Creates a non-blocking, close-on-exec socket; returns the descriptor, or a negative value on
// failure.
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

// Accepts a connection as a non-blocking, close-on-exec descriptor; returns it, or a negative
// value on failure.
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

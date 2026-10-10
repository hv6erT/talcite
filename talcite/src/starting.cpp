#include "starting.hpp"

#ifdef _WIN32
#include <winsock2.h>

#include <mutex>
#else
#include <cerrno>
#endif

namespace talcite::net {

#ifdef _WIN32

namespace {

std::once_flag once;

}

// Winsock has to be started before the first call of anything, getaddrinfo included. We do it once
// and never call WSACleanup: the library does not know who else uses sockets, and shutting down at
// the wrong moment breaks someone else's code. Winsock cleans up after itself when the process
// ends anyway.
void starting() {
    std::call_once(once, [] {
        WSADATA data{};

        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

// The error code of the last failed socket call on this thread.
int amiss() { return WSAGetLastError(); }

#else

void starting() {}

// The error code of the last failed socket call (`errno` outside Windows).
int amiss() { return errno; }

#endif

}

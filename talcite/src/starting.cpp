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

// Winsock trzeba obudzić przed pierwszym wywołaniem czegokolwiek, łącznie z getaddrinfo. Robimy
// to raz i **nie wołamy WSACleanup** — biblioteka nie wie, kto jeszcze z gniazd korzysta, a
// zamknięcie w złej chwili wywraca cudzy kod. Winsock i tak sprząta po sobie przy końcu procesu.
void starting() {
    std::call_once(once, [] {
        WSADATA data{};

        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

int amiss() { return WSAGetLastError(); }

#else

void starting() {}

int amiss() { return errno; }

#endif

}

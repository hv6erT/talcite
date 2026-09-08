#include "talcite/trouble.hpp"

#include <utility>

#ifdef _WIN32
#include <winsock2.h>

#include <windows.h>
#else
#include <cstring>
#endif

namespace talcite::net {

// Kłopot **nie tłumaczy** numeru sam z siebie. Kusiło, ale numery przychodzą z trzech różnych
// światów — errno, WSA i EAI — i ta sama liczba znaczy w nich co innego. Zdanie składa ten, kto
// wie, skąd wziął numer; od zamiany numeru na zdanie jest spoken().
Trouble::Trouble(const std::string& said, int code, Address where)
    : std::runtime_error(said), where_(std::move(where)), code_(code) {}

// Numer błędu bez zdania obok jest bezużyteczny w dzienniku, a zdanie od systemu jest w jego
// języku — i tak ma być, bo to system się skarży, nie my.
std::string spoken(int code) {
    if (code == 0) return {};

#ifdef _WIN32
    char* said = nullptr;

    const DWORD much = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&said), 0, nullptr);

    if (much == 0 || said == nullptr) return std::to_string(code);

    std::string made{said, much};

    LocalFree(said);

    while (!made.empty() && (made.back() == '\r' || made.back() == '\n' || made.back() == ' '))
        made.pop_back();

    return made;
#else
    return std::strerror(code);
#endif
}

}

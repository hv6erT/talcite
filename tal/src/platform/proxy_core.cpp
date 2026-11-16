#include "../httping.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winhttp.h>

#include "tal/text.hpp"
#endif

// The proxy the system is set to: the user's setting of Windows, as WinHTTP reads it; its fixed
// proxy alone, since a script (PAC) and finding one by itself are not followed. Other systems keep
// it in the environment, which `Proxy::from_environment` reads first.
namespace tal::net::httping {

#ifdef _WIN32

// The fixed proxy of the user's setting, with its exceptions; nothing when none is set.
std::optional<Proxy> systemic() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};

    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) return std::nullopt;

    std::optional<Proxy> made;

    if (config.lpszProxy != nullptr && *config.lpszProxy != L'\0')
        made = windowsy(text::narrow(config.lpszProxy), config.lpszProxyBypass != nullptr ? text::narrow(config.lpszProxyBypass) : std::string{});

    if (config.lpszAutoConfigUrl != nullptr) (void)GlobalFree(config.lpszAutoConfigUrl);
    if (config.lpszProxy != nullptr) (void)GlobalFree(config.lpszProxy);
    if (config.lpszProxyBypass != nullptr) (void)GlobalFree(config.lpszProxyBypass);

    return made;
}

#else

// Nothing beyond the environment.
std::optional<Proxy> systemic() { return std::nullopt; }

#endif

}

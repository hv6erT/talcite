#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "tal/text.hpp"

#ifdef _WIN32

#include <windows.h>

#endif

// The language of the system, as a proposal to the program: the program chooses its own language
// (`text::choose`), and may start from what the person set for the whole system.

namespace tal::text {

#ifdef _WIN32

// The language of the person's Windows, as its locale names it ("pl-PL").
std::optional<Language> proposed() {
    wchar_t named[LOCALE_NAME_MAX_LENGTH] = {};

    if (GetUserDefaultLocaleName(named, LOCALE_NAME_MAX_LENGTH) == 0) return std::nullopt;

    const std::string tag = narrow(named);

    if (tag.empty()) return std::nullopt;

    return Language{tag};
}

#else

// The language of the locale the person set, as POSIX looks for it: LC_ALL, then LC_MESSAGES,
// then LANG, the first that is set. "pl_PL.UTF-8@euro" is "pl-PL"; the C and POSIX locales name
// no language.
std::optional<Language> proposed() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* given = std::getenv(name);

        if (given == nullptr || *given == '\0') continue;

        std::string_view said{given};

        said = said.substr(0, said.find_first_of(".@"));

        if (said.empty() || said == "C" || said == "POSIX") return std::nullopt;

        return Language{said};
    }

    return std::nullopt;
}

#endif

}

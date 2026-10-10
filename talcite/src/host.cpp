#include "talcite/host.hpp"

#include <cstdlib>

#include "talcite/fs.hpp"

#ifdef _WIN32

#include <windows.h>

// CommandLineToArgvW: the arguments in wide form, without conversion to the ANSI code page.
#include <shellapi.h>

#endif

namespace talcite {

namespace {

// Whether the text consists only of lowercase ASCII letters.
bool lower_letters(std::string_view said) {
    for (const char one : said)
        if (one < 'a' || one > 'z') return false;

    return true;
}

// Whether the text looks like a region: two uppercase ASCII letters ("PL") or three digits (a UN
// M.49 code such as "419").
bool region_like(std::string_view said) {
    if (said.size() == 2) return said[0] >= 'A' && said[0] <= 'Z' && said[1] >= 'A' && said[1] <= 'Z';

    if (said.size() == 3) {
        for (const char one : said)
            if (one < '0' || one > '9') return false;

        return true;
    }

    return false;
}

}

// Turns the POSIX form ("pl_PL.UTF-8", "de_DE@euro", "pt_BR") into the tag the rest of the program
// speaks: "pl-PL". The encoding and the variant after the dot and the at sign say nothing about the
// language, so they are dropped. A language is two or three lowercase letters; a text that does not
// look like that means no language, including "C" and "POSIX", which really are no language (not
// English), so they need no guard of their own. Better not to guess than to pick a language file
// that does not exist; the caller knows what to choose when there is none.
std::string language_of(std::string_view posix) {
    const std::string_view bare = posix.substr(0, posix.find_first_of(".@"));

    const std::size_t cut = bare.find('_');

    const std::string_view tongue = bare.substr(0, cut);

    if (tongue.size() < 2 || tongue.size() > 3 || !lower_letters(tongue)) return {};

    if (cut == std::string_view::npos) return std::string{tongue};

    const std::string_view region = bare.substr(cut + 1);

    if (!region_like(region)) return std::string{tongue};

    return std::string{tongue} + '-' + std::string{region};
}

// The language a person speaks with the system. On Windows that is the user interface language, not
// the number and date format: someone with Polish dates and an English Windows wants the program in
// English. On POSIX it is the first one set among `LC_ALL`, `LC_MESSAGES` and `LANG`, in this
// order, as the C library itself reads them. Empty when nothing is set.
std::string language() {
#ifdef _WIN32

    wchar_t named[LOCALE_NAME_MAX_LENGTH] = {};

    const LCID which = MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT);

    if (LCIDToLocaleName(which, named, LOCALE_NAME_MAX_LENGTH, 0) <= 0) return {};

    return fs::to_string(fs::path{std::wstring{named}});

#else

    for (const char* one : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* told = std::getenv(one); told != nullptr && *told != '\0') return language_of(told);

    return {};

#endif
}

// The program's arguments in UTF-8. On Windows `main` receives them converted to the ANSI code
// page, so a name with Polish letters arrives with question marks; the real ones are kept alongside
// in wide form, and we take them from there. Elsewhere it is simply `argv`.
std::vector<std::string> arguments(int count, char** given) {
    std::vector<std::string> made;

#ifdef _WIN32

    int many = 0;

    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &many);

    if (wide != nullptr) {
        for (int at = 0; at < many; ++at) made.push_back(fs::to_string(fs::path{std::wstring{wide[at]}}));

        LocalFree(wide);

        return made;
    }

#endif

    for (int at = 0; at < count; ++at) made.emplace_back(given[at]);

    return made;
}

}

#include "talcite/host.hpp"

#include <cstdlib>

#include "talcite/fs.hpp"

#ifdef _WIN32

#include <windows.h>

// CommandLineToArgvW — argumenty w postaci szerokiej, bez przepisania na stronę kodową ANSI.
#include <shellapi.h>

#endif

namespace talcite {

namespace {

bool lower_letters(std::string_view said) {
    for (const char one : said)
        if (one < 'a' || one > 'z') return false;

    return true;
}

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

// Zapis POSIX-owy (`pl_PL.UTF-8`, `de_DE@euro`, `pt_BR`) na znacznik, którym mówi reszta programu:
// `pl-PL`. Kodowanie i odmiana po kropce i małpie nie mówią nic o języku, więc odpadają. Język to
// dwie albo trzy małe litery; napis, który tak nie wygląda, jest brakiem języka — także `C`
// i `POSIX`, które brakiem są naprawdę (nie angielskim), więc osobnej straży nie potrzebują.
// Lepiej nie zgadywać, niż wybrać plik języka, którego nie ma; wołający wie, co wybrać przy braku.
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

// Język, którym człowiek rozmawia z systemem. Na Windows to język interfejsu użytkownika, a nie
// format liczb i dat — ktoś z polskimi datami i angielskim Windowsem chce programu po angielsku.
// Na POSIX-ie pierwsza ustawiona z `LC_ALL`, `LC_MESSAGES` i `LANG`, w tej kolejności, jak czyta je
// sama biblioteka C.
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

// Argumenty programu w UTF-8. Na Windows `main` dostaje je przepisane na stronę kodową ANSI, więc
// „Filmy Ździśka" przychodzi z pytajnikami — prawdziwe stoją obok, w postaci szerokiej, i stamtąd je
// bierzemy. Gdzie indziej to po prostu `argv`.
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

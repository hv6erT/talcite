#include "proving.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

// CommandLineToArgvW — argumenty w postaci szerokiej, bez przepisania na stronę kodową ANSI.
#include <shellapi.h>
#endif

namespace proving {

namespace {

std::string called;

[[nodiscard]] std::string_view bared(std::string_view file) {
    const std::size_t at = file.find_last_of("/\\");

    return at == std::string_view::npos ? file : file.substr(at + 1);
}

}

std::vector<Trial>& gathered() {
    static std::vector<Trial> held;

    return held;
}

const std::string& mine() { return called; }

void fell(std::string_view file, int line, const std::string& said) {
    throw Failed{std::string{bared(file)} + ":" + std::to_string(line) + "  " + said};
}

}

namespace {

using proving::Trial;

[[nodiscard]] std::string_view bared(std::string_view file) {
    const std::size_t at = file.find_last_of("/\\");

    return at == std::string_view::npos ? file : file.substr(at + 1);
}

void said(std::FILE* into, const std::string& what) {
    if (!what.empty()) std::fwrite(what.data(), 1, what.size(), into);

    std::fflush(into);
}

#ifdef _WIN32

std::string narrowly(const std::wstring& what) {
    if (what.empty()) return {};

    const int much = WideCharToMultiByte(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()),
                                         nullptr, 0, nullptr, nullptr);

    std::string made(static_cast<std::size_t>(much), '\0');

    WideCharToMultiByte(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()), made.data(), much,
                        nullptr, nullptr);

    return made;
}

std::wstring widely(const std::string& what) {
    if (what.empty()) return {};

    const int much =
        MultiByteToWideChar(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()), nullptr, 0);

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()), made.data(), much);

    return made;
}

#endif

// **Argumenty czytamy szeroko, nie wąsko.** Windows podaje `main` argumenty przepisane na stronę
// kodową ANSI, więc „dzień" przychodzi tam z pytajnikiem w środku — i próba porównująca bajty
// obwiniałaby uruchamianie za coś, co zepsuł odbiór. Prawdziwe argumenty stoją obok, w postaci
// szerokiej; bierzemy je stamtąd i przepisujemy na UTF-8, w którym rozmawia reszta talcite.
std::vector<std::string> worded(int count, char** given) {
    std::vector<std::string> made;

#ifdef _WIN32

    int many = 0;

    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &many);

    if (wide != nullptr) {
        for (int at = 0; at < many; ++at) made.push_back(narrowly(std::wstring{wide[at]}));

        LocalFree(wide);

        return made;
    }

#endif

    for (int at = 0; at < count; ++at) made.emplace_back(given[at]);

    return made;
}

std::string around(const std::string& name) {
#ifdef _WIN32

    std::wstring room(32767, L'\0');

    const DWORD much =
        GetEnvironmentVariableW(widely(name).c_str(), room.data(), static_cast<DWORD>(room.size()));

    if (much == 0) return "<brak>";

    room.resize(much);

    return narrowly(room);

#else

    const char* told = std::getenv(name.c_str());

    return told == nullptr ? std::string{"<brak>"} : std::string{told};

#endif
}

std::string here() {
#ifdef _WIN32

    return narrowly(std::filesystem::current_path().wstring());

#else

    return std::filesystem::current_path().string();

#endif
}

// **Program pomocniczy siedzi w testach, nie w bibliotece** — ta sama reguła, co przy serwerach.
// Binarka testowa umie udawać cudzy program i uruchamia samą siebie, więc sprawdzenie uruchamiania
// nie zależy od tego, czy na maszynie stoi akurat powłoka, `echo` czy `cmd`.
//
// Wołane **przed** czymkolwiek innym w `main`, żeby udawanie nie ciągnęło za sobą zbierania prób.
int pretending(const std::vector<std::string>& given) {
#ifdef _WIN32

    // Bez tego Windows zamieniłby każdy `\n` na `\r\n` i porównanie bajtów przestałoby cokolwiek
    // znaczyć.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);

#endif

    const std::string what = given.empty() ? std::string{} : given[0];
    const std::string rest = given.size() > 1 ? given[1] : std::string{};

    if (what == "said") {
        said(stdout, rest);

        return 0;
    }

    if (what == "moaned") {
        said(stderr, rest);

        return 0;
    }

    if (what == "both") {
        said(stdout, "wyjście");
        said(stderr, "błąd");

        return 0;
    }

    if (what == "echo") {
        char room[4096];

        for (;;) {
            const std::size_t got = std::fread(room, 1, sizeof(room), stdin);

            if (got == 0) break;

            std::fwrite(room, 1, got, stdout);
        }

        std::fflush(stdout);

        return 0;
    }

    if (what == "much") {
        // Więcej, niż mieści rura. Czytający, który zapomniałby czytać obu stron naraz, stanie
        // tutaj i nigdy się nie doczeka.
        const std::string one(1000, 'x');

        for (int at = 0; at < 300; ++at) {
            said(stdout, one);
            said(stderr, one);
        }

        return 0;
    }

    if (what == "code") return rest.empty() ? 0 : std::atoi(rest.c_str());

    if (what == "sleeping") {
        std::this_thread::sleep_for(std::chrono::milliseconds{rest.empty() ? 0 : std::atoi(rest.c_str())});

        return 0;
    }

    if (what == "where") {
        said(stdout, here());

        return 0;
    }

    if (what == "around") {
        said(stdout, rest.empty() ? std::string{"<brak>"} : around(rest));

        return 0;
    }

    if (what == "args") {
        std::string made;

        for (std::size_t at = 1; at < given.size(); ++at) {
            made += given[at];
            made += '\n';
        }

        said(stdout, made);

        return 0;
    }

    std::fprintf(stderr, "Nieznane udawanie: %s\n", what.c_str());

    return 2;
}

}

int main(int count, char** given) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    const std::vector<std::string> words = worded(count, given);

    if (!words.empty()) proving::called = words[0];

    if (words.size() > 1 && words[1] == "--pretending")
        return pretending(std::vector<std::string>{words.begin() + 2, words.end()});

    const int many = static_cast<int>(words.size());

    std::string only;
    std::string chosen;
    bool listing = false;

    for (int at = 1; at < many; ++at) {
        const std::string& one = words[static_cast<std::size_t>(at)];

        if (one == "--list") listing = true;
        else if (one == "--file" && at + 1 < many) chosen = words[static_cast<std::size_t>(++at)];
        else if (one == "--only" && at + 1 < many) only = words[static_cast<std::size_t>(++at)];
        else {
            std::cerr << "Użycie: talcite-test [--list] [--file <plik>] [--only <nazwa>]\n";
            return 2;
        }
    }

    std::vector<Trial>& held = proving::gathered();

    if (listing) {
        for (const Trial& one : held) std::cout << bared(one.file) << "  " << one.named << "\n";

        return 0;
    }

    std::size_t passed = 0;
    std::size_t ran = 0;

    for (const Trial& one : held) {
        if (!only.empty() && one.named != only) continue;
        if (!chosen.empty() && bared(one.file) != chosen) continue;

        ++ran;

        try {
            one.run();
            ++passed;
            std::cout << "  ok    " << one.named << "\n";
        } catch (const proving::Failed& why) {
            std::cout << "  ŹLE   " << one.named << "\n        " << why.what() << "\n";
        } catch (const std::exception& why) {
            std::cout << "  ŹLE   " << one.named << "\n        rzuciło: " << why.what() << "\n";
        }
    }

    std::cout << "\nPrzeszło " << passed << " z " << ran << ".\n";

    return passed == ran ? 0 : 1;
}

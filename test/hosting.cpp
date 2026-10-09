#include "proving.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "talcite/fs.hpp"
#include "talcite/host.hpp"

using namespace talcite;

namespace {

namespace stood = std::filesystem;

// Zmienna środowiska podstawiona na czas jednej próby i oddana po niej — pusta wartość to brak.
class Planted {
public:
    Planted(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = was;

        put(value);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    ~Planted() { put(was_ ? was_->c_str() : ""); }

private:
    void put(const char* value) const {
#ifdef _WIN32
        (void)::_putenv_s(name_, value);
#else
        if (*value == '\0') ::unsetenv(name_);
        else ::setenv(name_, value, 1);
#endif
    }

    const char* name_;
    std::optional<std::string> was_;
};

// Katalog na czas jednej próby, sprzątany po sobie.
class Nook {
public:
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-host-" + std::to_string(++counted) + "-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this))};

        std::error_code ignored;
        stood::remove_all(where_, ignored);
        stood::create_directories(where_, ignored);
    }

    ~Nook() {
        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Nook(const Nook&) = delete;
    Nook& operator=(const Nook&) = delete;

    [[nodiscard]] const stood::path& where() const { return where_; }

private:
    stood::path where_;
};

}

// Zapis POSIX-owy na znacznik: kodowanie i odmiana odpadają, podkreślnik staje się łącznikiem.
TRIAL("Język: zapis POSIX na znacznik") {
    same(language_of("pl_PL.UTF-8"), std::string{"pl-PL"});
    same(language_of("de_DE@euro"), std::string{"de-DE"});
    same(language_of("pt_BR"), std::string{"pt-BR"});
    same(language_of("es_419.UTF-8"), std::string{"es-419"});
    same(language_of("fil_PH"), std::string{"fil-PH"});
    same(language_of("en"), std::string{"en"});
}

// `C` i `POSIX` to brak języka, nie angielski; to, co na język nie wygląda, też jest brakiem.
TRIAL("Język: C, POSIX i napisy obce to brak języka") {
    same(language_of(""), std::string{});
    same(language_of("C"), std::string{});
    same(language_of("C.UTF-8"), std::string{});
    same(language_of("POSIX"), std::string{});
    same(language_of("P"), std::string{});
    same(language_of("polski"), std::string{});
    same(language_of("PL_pl"), std::string{});
    same(language_of("pl_pl"), std::string{"pl"});
    same(language_of("pl_P1"), std::string{"pl"});
    same(language_of("pl_PLX"), std::string{"pl"});
    same(language_of("es_41"), std::string{"es"});
}

#ifndef _WIN32

// Pierwsza ustawiona wygrywa, w porządku biblioteki C: LC_ALL, LC_MESSAGES, LANG.
TRIAL("Język: LC_ALL przed LC_MESSAGES przed LANG") {
    const Planted all{"LC_ALL", ""};
    const Planted messages{"LC_MESSAGES", ""};
    const Planted lang{"LANG", "pl_PL.UTF-8"};

    same(language(), std::string{"pl-PL"});

    {
        const Planted told{"LC_MESSAGES", "de_DE.UTF-8"};

        same(language(), std::string{"de-DE"});

        const Planted over{"LC_ALL", "C"};

        same(language(), std::string{});
    }
}

// Zmienna ustawiona, ale pusta, to jak nieustawiona — POSIX każe wtedy pytać następnej.
TRIAL("Język: pusta zmienna przepuszcza do następnej") {
    const Planted all{"LC_ALL", ""};
    const Planted messages{"LC_MESSAGES", ""};
    const Planted lang{"LANG", "pl_PL.UTF-8"};

    ::setenv("LC_ALL", "", 1);
    ::setenv("LC_MESSAGES", "", 1);

    same(language(), std::string{"pl-PL"});
}

TRIAL("Język: bez zmiennych nie ma języka") {
    const Planted all{"LC_ALL", ""};
    const Planted messages{"LC_MESSAGES", ""};
    const Planted lang{"LANG", ""};

    same(language(), std::string{});
}

// Poza Windows argumenty to po prostu `argv`, w tej samej kolejności.
TRIAL("Argumenty: poza Windows to argv") {
    char first[] = "twa";
    char second[] = "Filmy Ździśka";
    char* given[] = {first, second};

    const std::vector<std::string> made = arguments(2, given);

    same(made.size(), std::size_t{2});
    same(made[1], std::string{"Filmy Ździśka"});
}

// Folder znany z `user-dirs.dirs`: od `$HOME` i bezwzględny, w cudzysłowie albo bez.
TRIAL("Foldery: wpisy z user-dirs.dirs") {
    const Nook nook;

    const stood::path house = nook.where() / "dom";

    stood::create_directories(house / "Pobrane");
    stood::create_directories(house / "Wideo");
    stood::create_directories(nook.where() / "muzyka");
    stood::create_directories(house / ".config");

    std::ofstream{house / ".config" / "user-dirs.dirs"}
        << "# komentarz\n"
        << "XDG_DOWNLOAD_DIR=\"$HOME/Pobrane\"\n"
        << "XDG_VIDEOS_DIR=$HOME/Wideo/\n"
        << "XDG_MUSIC_DIR=\"" << (nook.where() / "muzyka").string() << "\"\n";

    const Planted home{"HOME", house.c_str()};
    const Planted config{"XDG_CONFIG_HOME", ""};

    same(fs::folder(fs::Known::home).await(), std::optional<stood::path>{house});
    same(fs::folder(fs::Known::downloads).await(), std::optional<stood::path>{house / "Pobrane"});
    same(fs::folder(fs::Known::videos).await(), std::optional<stood::path>{house / "Wideo"});
    same(fs::folder(fs::Known::music).await(), std::optional<stood::path>{nook.where() / "muzyka"});
}

// Wpis równy domowi znaczy „wyłączony"; bez wpisu i przy wpisie krzywym zostaje nazwa zwyczajowa
// w domu — o ile stoi.
TRIAL("Foldery: wyłączony, zwyczajowy i nieistniejący") {
    const Nook nook;

    const stood::path house = nook.where() / "dom";

    stood::create_directories(house / "Desktop");
    stood::create_directories(house / "Documents");
    stood::create_directories(house / "Pictures");
    stood::create_directories(house / "Downloads");
    stood::create_directories(nook.where() / "konfiguracja");

    std::ofstream{nook.where() / "konfiguracja" / "user-dirs.dirs"}
        << "XDG_DOCUMENTS_DIR=\"$HOME\"\n"
        << "XDG_PICTURES_DIR=\"$HOME/\"\n"
        << "XDG_DOWNLOAD_DIR=\"Pobrane\"\n";

    const Planted home{"HOME", house.c_str()};
    const Planted config{"XDG_CONFIG_HOME", (nook.where() / "konfiguracja").c_str()};

    same(fs::folder(fs::Known::desktop).await(), std::optional<stood::path>{house / "Desktop"});
    same(fs::folder(fs::Known::documents).await(), std::optional<stood::path>{});
    same(fs::folder(fs::Known::pictures).await(), std::optional<stood::path>{});
    same(fs::folder(fs::Known::downloads).await(), std::optional<stood::path>{house / "Downloads"});
    same(fs::folder(fs::Known::music).await(), std::optional<stood::path>{});
}

// XDG_CONFIG_HOME względna nie liczy się — specyfikacja każe ją wtedy pominąć.
TRIAL("Foldery: względna XDG_CONFIG_HOME jest pominięta") {
    const Nook nook;

    const stood::path house = nook.where() / "dom";

    stood::create_directories(house / ".config");
    stood::create_directories(house / "Kino");

    std::ofstream{house / ".config" / "user-dirs.dirs"} << "XDG_VIDEOS_DIR=\"$HOME/Kino\"\n";

    const Planted home{"HOME", house.c_str()};
    const Planted config{"XDG_CONFIG_HOME", "konfiguracja"};

    same(fs::folder(fs::Known::videos).await(), std::optional<stood::path>{house / "Kino"});
}

// Bez domu nie ma żadnego — także wtedy, gdy w katalogu bieżącym stoi `Downloads`: nazwa zwyczajowa
// liczy się od domu, a nie od miejsca, w którym program akurat stoi.
TRIAL("Foldery: bez domu nie ma żadnego") {
    const Nook nook;

    stood::create_directories(nook.where() / "Downloads");
    stood::create_directories(nook.where() / ".config");

    std::error_code why;

    const stood::path was = stood::current_path(why);

    stood::current_path(nook.where(), why);

    {
        const Planted home{"HOME", ""};
        const Planted config{"XDG_CONFIG_HOME", ""};

        same(fs::folder(fs::Known::home).await(), std::optional<stood::path>{});
        same(fs::folder(fs::Known::downloads).await(), std::optional<stood::path>{});
    }

    stood::current_path(was, why);
}

#else

TRIAL("Język: Windows mówi znacznikiem z łącznikiem") {
    const std::string made = language();

    ought(made.empty() || made.find('-') != std::string::npos);
}

TRIAL("Foldery: dom to profil użytkownika") {
    same(fs::folder(fs::Known::home).await(), std::optional<stood::path>{fs::home()});
}

#endif

// Folder znany zawsze jest katalogiem, jeśli w ogóle jest.
TRIAL("Foldery: każdy podany folder to katalog") {
    for (const fs::Known one : {fs::Known::home, fs::Known::desktop, fs::Known::documents, fs::Known::downloads,
                                fs::Known::music, fs::Known::pictures, fs::Known::videos}) {
        const std::optional<stood::path> made = fs::folder(one).await();

        if (made) ought(stood::is_directory(*made));
    }
}

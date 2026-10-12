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

// An environment variable replaced for the duration of one trial and restored after it; an empty
// value means unset.
class Planted {
public:
    // Remembers the variable's current value and sets the new one.
    Planted(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = was;

        put(value);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    // Restores the remembered value, or unsets the variable if it had none.
    ~Planted() { put(was_ ? was_->c_str() : ""); }

private:
    // Sets the variable to the value, or unsets it when the value is empty.
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

// A directory for the duration of one trial, which cleans up after itself.
class Nook {
public:
    // Creates a fresh, empty directory with a name unique to this process and object.
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-host-" + std::to_string(++counted) + "-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this))};

        std::error_code ignored;
        stood::remove_all(where_, ignored);
        stood::create_directories(where_, ignored);
    }

    // Removes the directory with everything in it, ignoring errors.
    ~Nook() {
        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Nook(const Nook&) = delete;
    Nook& operator=(const Nook&) = delete;

    // The path of the directory.
    [[nodiscard]] const stood::path& where() const { return where_; }

private:
    stood::path where_;
};

}

// A POSIX locale name to a tag: the encoding and the modifier are dropped, the underscore becomes a
// hyphen.
TRIAL("Language: a POSIX locale name to a tag") {
    same(language_of("pl_PL.UTF-8"), std::string{"pl-PL"});
    same(language_of("de_DE@euro"), std::string{"de-DE"});
    same(language_of("pt_BR"), std::string{"pt-BR"});
    same(language_of("es_419.UTF-8"), std::string{"es-419"});
    same(language_of("fil_PH"), std::string{"fil-PH"});
    same(language_of("en"), std::string{"en"});
}

// `C` and `POSIX` mean no language, not English; anything that does not look like a language means
// none as well.
TRIAL("Language: C, POSIX and foreign strings mean no language") {
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

// The first variable that is set wins, in the order of the C library: LC_ALL, LC_MESSAGES, LANG.
TRIAL("Language: LC_ALL before LC_MESSAGES before LANG") {
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

// A variable that is set but empty counts as unset: POSIX says to ask the next one then.
TRIAL("Language: an empty variable passes on to the next one") {
    const Planted all{"LC_ALL", ""};
    const Planted messages{"LC_MESSAGES", ""};
    const Planted lang{"LANG", "pl_PL.UTF-8"};

    ::setenv("LC_ALL", "", 1);
    ::setenv("LC_MESSAGES", "", 1);

    same(language(), std::string{"pl-PL"});
}

TRIAL("Language: without the variables there is no language") {
    const Planted all{"LC_ALL", ""};
    const Planted messages{"LC_MESSAGES", ""};
    const Planted lang{"LANG", ""};

    same(language(), std::string{});
}

// Outside Windows the arguments are simply `argv`, in the same order.
TRIAL("Arguments: outside Windows they are argv") {
    char first[] = "twa";
    char second[] = "Filmy Ździśka";
    char* given[] = {first, second};

    const std::vector<std::string> made = arguments(2, given);

    same(made.size(), std::size_t{2});
    same(made[1], std::string{"Filmy Ździśka"});
}

// A folder known from user-dirs.dirs: relative to `$HOME` and absolute, quoted or not.
TRIAL("Folders: entries from user-dirs.dirs") {
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

    same(fs::folder(fs::Known::home).get(), std::optional<stood::path>{house});
    same(fs::folder(fs::Known::downloads).get(), std::optional<stood::path>{house / "Pobrane"});
    same(fs::folder(fs::Known::videos).get(), std::optional<stood::path>{house / "Wideo"});
    same(fs::folder(fs::Known::music).get(), std::optional<stood::path>{nook.where() / "muzyka"});
}

// An entry equal to the home directory means "disabled"; without an entry, and with a malformed
// one, the customary name in the home directory applies, provided it exists.
TRIAL("Folders: disabled, customary and missing") {
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

    same(fs::folder(fs::Known::desktop).get(), std::optional<stood::path>{house / "Desktop"});
    same(fs::folder(fs::Known::documents).get(), std::optional<stood::path>{});
    same(fs::folder(fs::Known::pictures).get(), std::optional<stood::path>{});
    same(fs::folder(fs::Known::downloads).get(), std::optional<stood::path>{house / "Downloads"});
    same(fs::folder(fs::Known::music).get(), std::optional<stood::path>{});
}

// A relative XDG_CONFIG_HOME does not count: the specification says to ignore it then.
TRIAL("Folders: a relative XDG_CONFIG_HOME is ignored") {
    const Nook nook;

    const stood::path house = nook.where() / "dom";

    stood::create_directories(house / ".config");
    stood::create_directories(house / "Kino");

    std::ofstream{house / ".config" / "user-dirs.dirs"} << "XDG_VIDEOS_DIR=\"$HOME/Kino\"\n";

    const Planted home{"HOME", house.c_str()};
    const Planted config{"XDG_CONFIG_HOME", "konfiguracja"};

    same(fs::folder(fs::Known::videos).get(), std::optional<stood::path>{house / "Kino"});
}

// Without a home directory there is no folder at all, also when `Downloads` exists in the current
// directory: the customary name counts from home, not from wherever the program happens to be.
TRIAL("Folders: without a home directory there are none") {
    const Nook nook;

    stood::create_directories(nook.where() / "Downloads");
    stood::create_directories(nook.where() / ".config");

    std::error_code why;

    const stood::path was = stood::current_path(why);

    stood::current_path(nook.where(), why);

    {
        const Planted home{"HOME", ""};
        const Planted config{"XDG_CONFIG_HOME", ""};

        same(fs::folder(fs::Known::home).get(), std::optional<stood::path>{});
        same(fs::folder(fs::Known::downloads).get(), std::optional<stood::path>{});
    }

    stood::current_path(was, why);
}

#else

TRIAL("Language: Windows gives a tag with a hyphen") {
    const std::string made = language();

    ought(made.empty() || made.find('-') != std::string::npos);
}

TRIAL("Folders: home is the user profile") {
    same(fs::folder(fs::Known::home).get(), std::optional<stood::path>{fs::home()});
}

#endif

// A known folder is always a directory, if it exists at all.
TRIAL("Folders: every folder returned is a directory") {
    for (const fs::Known one : {fs::Known::home, fs::Known::desktop, fs::Known::documents, fs::Known::downloads,
                                fs::Known::music, fs::Known::pictures, fs::Known::videos}) {
        const std::optional<stood::path> made = fs::folder(one).get();

        if (made) ought(stood::is_directory(*made));
    }
}

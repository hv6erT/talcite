#include "talcite/login.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "talcite/fs.hpp"
#include "talcite/trouble.hpp"

#ifdef _WIN32

#include <windows.h>

#include "launching.hpp"

#endif

// **Program przy logowaniu** — dla demona, który ma wstać po ponownym uruchomieniu komputera, póki
// ma coś do roboty, bez instalatora i bez praw administratora. Każdy system ma na to miejsce
// użytkownika: Windows — wartość w kluczu `Run` (HKCU), Linux i reszta POSIX-u — plik `.desktop`
// w `$XDG_CONFIG_HOME/autostart` (XDG Autostart, który czyta każde środowisko pulpitu), macOS —
// plik w `~/Library/LaunchAgents`. Wpis ma nazwę; drugi `enrol` pod tą samą nazwą go podmienia,
// `withdraw` wpisu, którego nie ma, to nie kłopot.
//
// Nazwa to litery, cyfry, kropka, kreska i podkreślenie — staje się nazwą pliku albo wartości.
// Windows uruchamia wpis bez katalogu roboczego i bez własnego środowiska, więc polecenie, które
// ich chce, dostaje tam odmowę zamiast cichego pominięcia. XDG niesie środowisko przez `env`.
namespace talcite::login {

namespace {

namespace stood = std::filesystem;

void named(const std::string& name) {
    const bool fine = !name.empty() && name.size() <= 64 && name.front() != '.' &&
                      std::all_of(name.begin(), name.end(), [](char one) {
                          return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') ||
                                 one == '.' || one == '-' || one == '_';
                      });

    if (!fine) throw Trouble{"A login entry takes a name of letters, digits, dots, dashes and underscores."};
}

template <typename Fn>
auto pooled(Fn what) {
    using Made = decltype(what());

    return Promise<Made>{[what = std::move(what)](auto resolve, auto reject) mutable {
        try {
            if constexpr (std::is_void_v<Made>) {
                what();
                resolve();
            } else {
                resolve(what());
            }
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

#ifdef _WIN32

constexpr const wchar_t* run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HKEY opened(REGSAM access) {
    HKEY made = nullptr;

    const LSTATUS told = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0, access, nullptr, &made, nullptr);

    if (told != ERROR_SUCCESS) throw Trouble{"Cannot open the login programs of this user.", static_cast<int>(told)};

    return made;
}

struct Key {
    HKEY held;

    explicit Key(REGSAM access) : held(opened(access)) {}

    ~Key() { RegCloseKey(held); }

    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

std::wstring wide(const std::string& name) { return fs::from_string(name).wstring(); }

#else

#ifdef __APPLE__

stood::path folder() { return fs::home() / "Library" / "LaunchAgents"; }

stood::path file_of(const std::string& name) { return folder() / (name + ".plist"); }

std::string escaped(std::string_view text) {
    std::string made;

    for (const char one : text) {
        if (one == '&') made += "&amp;";
        else if (one == '<') made += "&lt;";
        else if (one == '>') made += "&gt;";
        else made += one;
    }

    return made;
}

std::string entry(const std::string& name, const Command& what) {
    std::string made = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                       "<plist version=\"1.0\">\n<dict>\n";

    made += "  <key>Label</key>\n  <string>" + escaped(name) + "</string>\n";
    made += "  <key>ProgramArguments</key>\n  <array>\n";
    made += "    <string>" + escaped(fs::to_string(what.program)) + "</string>\n";

    for (const std::string& one : what.arguments) made += "    <string>" + escaped(one) + "</string>\n";

    made += "  </array>\n  <key>RunAtLoad</key>\n  <true/>\n";

    if (!what.where.empty()) made += "  <key>WorkingDirectory</key>\n  <string>" + escaped(fs::to_string(what.where)) + "</string>\n";

    if (!what.environment.empty()) {
        made += "  <key>EnvironmentVariables</key>\n  <dict>\n";

        for (const Variable& one : what.environment)
            made += "    <key>" + escaped(one.name) + "</key>\n    <string>" + escaped(one.value) + "</string>\n";

        made += "  </dict>\n";
    }

    return made + "</dict>\n</plist>\n";
}

#else

// XDG: `$XDG_CONFIG_HOME`, gdy jest ścieżką bezwzględną, inaczej `~/.config`.
stood::path folder() {
    if (const char* given = std::getenv("XDG_CONFIG_HOME"); given != nullptr && *given == '/')
        return stood::path{given} / "autostart";

    return fs::home() / ".config" / "autostart";
}

stood::path file_of(const std::string& name) { return folder() / (name + ".desktop"); }

// Wartość typu „string" pliku `.desktop`: ukośnik i znaki końca wiersza przez ucieczki.
std::string valued(std::string_view text) {
    std::string made;

    for (const char one : text) {
        if (one == '\\') made += "\\\\";
        else if (one == '\t') made += "\\t";
        else made += one;
    }

    return made;
}

// Argument w `Exec` według specyfikacji Desktop Entry: zwykły idzie, jak jest; z którymkolwiek
// znakiem zastrzeżonym — w cudzysłowie, a w nim ukośnik, cudzysłów, grawis i dolar dostają ukośnik.
// Procent zawsze podwojony (`%%`), bo pojedynczy to pole do podstawienia. Potem całość jako wartość
// „string" — stąd cztery ukośniki za jeden. Końca wiersza nie da się zapisać wcale.
std::string argument(std::string_view one) {
    if (one.find_first_of("\r\n") != std::string_view::npos || one.find('\0') != std::string_view::npos)
        throw Trouble{"A login entry cannot carry a line break in an argument."};

    std::string doubled;

    for (const char each : one) {
        if (each == '%') doubled += "%%";
        else doubled += each;
    }

    const bool plain = !doubled.empty() && doubled.find_first_of(" \t\"'\\><~|&;$*?#()`") == std::string::npos;

    if (plain) return valued(doubled);

    std::string made = "\"";

    for (const char each : doubled) {
        if (each == '"' || each == '`' || each == '$' || each == '\\') made += '\\';

        made += each;
    }

    return valued(made + "\"");
}

std::string entry(const std::string& name, const Command& what) {
    std::string line;

    if (!what.environment.empty()) {
        line += "env";

        for (const Variable& one : what.environment) line += " " + argument(one.name + "=" + one.value);

        line += " ";
    }

    line += argument(fs::to_string(what.program));

    for (const std::string& one : what.arguments) line += " " + argument(one);

    std::string made = "[Desktop Entry]\nType=Application\nName=" + valued(name) + "\nExec=" + line + "\n";

    if (!what.where.empty()) {
        const std::string where = fs::to_string(what.where);

        if (where.find_first_of("\r\n") != std::string::npos) throw Trouble{"A login entry cannot carry a line break in its folder."};

        made += "Path=" + valued(where) + "\n";
    }

    return made + "Terminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n";
}

#endif

// Zapis obok i przemianowanie: środowisko pulpitu nie przeczyta nigdy połowy wpisu.
void written(const stood::path& where, const std::string& text) {
    std::error_code why;

    stood::create_directories(where.parent_path(), why);

    if (why) throw Trouble{"Cannot create the folder for login programs.", why.value()};

    const stood::path beside = stood::path{where}.concat(".part");

    {
        std::ofstream into{beside, std::ios::binary | std::ios::trunc};

        into << text;
        into.close();

        if (!into) throw Trouble{"Cannot write the login entry."};
    }

    stood::rename(beside, where, why);

    if (why) {
        stood::remove(beside, why);

        throw Trouble{"Cannot write the login entry."};
    }
}

#endif

}

Promise<bool> enrolled(std::string name) {
    return pooled([name = std::move(name)] {
        named(name);

#ifdef _WIN32
        const Key key{KEY_QUERY_VALUE};

        const LSTATUS told = RegQueryValueExW(key.held, wide(name).c_str(), nullptr, nullptr, nullptr, nullptr);

        if (told == ERROR_FILE_NOT_FOUND) return false;

        if (told != ERROR_SUCCESS) throw Trouble{"Cannot read the login programs of this user.", static_cast<int>(told)};

        return true;
#else
        std::error_code why;

        const bool there = stood::exists(file_of(name), why);

        if (why) throw Trouble{"Cannot read the login programs of this user.", why.value()};

        return there;
#endif
    });
}

Promise<void> enrol(std::string name, Command what) {
    return pooled([name = std::move(name), what = std::move(what)] {
        named(name);

        if (what.program.empty()) throw Trouble{"A login entry needs a program."};

#ifdef _WIN32
        if (!what.where.empty() || !what.environment.empty())
            throw Trouble{"Windows starts login programs without a working folder or environment of their own."};

        const std::wstring line = launched(what);

        const Key key{KEY_SET_VALUE};

        const LSTATUS told = RegSetValueExW(key.held, wide(name).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(line.c_str()),
                                            static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)));

        if (told != ERROR_SUCCESS) throw Trouble{"Cannot write the login entry.", static_cast<int>(told)};
#else
        written(file_of(name), entry(name, what));
#endif
    });
}

Promise<void> withdraw(std::string name) {
    return pooled([name = std::move(name)] {
        named(name);

#ifdef _WIN32
        const Key key{KEY_SET_VALUE};

        const LSTATUS told = RegDeleteValueW(key.held, wide(name).c_str());

        if (told != ERROR_SUCCESS && told != ERROR_FILE_NOT_FOUND)
            throw Trouble{"Cannot remove the login entry.", static_cast<int>(told)};
#else
        std::error_code why;

        stood::remove(file_of(name), why);

        // Brak pliku to dla `remove` nie błąd, tylko fałsz — i tak ma być.
        if (why) throw Trouble{"Cannot remove the login entry.", why.value()};
#endif
    });
}

}

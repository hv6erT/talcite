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

// Programs started at login, for a daemon that has to come back after the computer restarts while
// it still has work to do, without an installer and without administrator rights. Every system
// has a per-user place for this: on Windows a value under the Run key (HKCU), on Linux and the
// rest of POSIX a .desktop file in $XDG_CONFIG_HOME/autostart (XDG Autostart, read by every
// desktop environment), on macOS a file in ~/Library/LaunchAgents. An entry has a name; a second
// `enrol` under the same name replaces it, and a `withdraw` of an entry that does not exist is no
// trouble.
//
// A name consists of letters, digits, dots, dashes and underscores, because it becomes a file
// name or a value name. Windows starts an entry without a working folder and without an
// environment of its own, so a command that wants them is refused there instead of silently
// losing them. XDG carries the environment through env.
namespace talcite::login {

namespace {

namespace stood = std::filesystem;

// Checks that a name is fit to become a file or value name: 1 to 64 letters, digits, dots, dashes
// and underscores, not starting with a dot. Throws `Trouble` otherwise.
void named(const std::string& name) {
    const bool fine = !name.empty() && name.size() <= 64 && name.front() != '.' &&
                      std::all_of(name.begin(), name.end(), [](char one) {
                          return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') ||
                                 one == '.' || one == '-' || one == '_';
                      });

    if (!fine) throw Trouble{"A login entry takes a name of letters, digits, dots, dashes and underscores."};
}

// Wraps blocking work in a promise run on the I/O lane, resolved with the work's result or rejected
// with the exception it throws.
template <typename Fn>
auto pooled(Fn what) {
    using Made = decltype(what());

    return run(io(), [what = std::move(what)]() mutable -> Made {
        if constexpr (std::is_void_v<Made>) {
            what();
            return;
        } else {
            return what();
        }
    });
}

#ifdef _WIN32

constexpr const wchar_t* run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// Opens (creating if needed) the Run key of the current user with the given access; throws
// `Trouble` with the registry status on failure.
HKEY opened(REGSAM access) {
    HKEY made = nullptr;

    const LSTATUS told = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0, access, nullptr, &made, nullptr);

    if (told != ERROR_SUCCESS) throw Trouble{"Cannot open the login programs of this user.", static_cast<int>(told)};

    return made;
}

struct Key {
    HKEY held;

    explicit Key(REGSAM access) : held(opened(access)) {}

    // Closes the registry key.
    ~Key() { RegCloseKey(held); }

    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

// The UTF-16 form of a UTF-8 name, as the registry functions take it.
std::wstring wide(const std::string& name) { return fs::from_string(name).wstring(); }

#else

#ifdef __APPLE__

// The folder of the user's launch agents.
stood::path folder() { return fs::home() / "Library" / "LaunchAgents"; }

// The property list file of the entry with the given name.
stood::path file_of(const std::string& name) { return folder() / (name + ".plist"); }

// Escapes text for an XML property list: &, < and > become entities.
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

// The launch agent property list for the command: label, program arguments, RunAtLoad, and the
// working folder and environment when given.
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

// The XDG autostart folder: under $XDG_CONFIG_HOME when it is an absolute path, otherwise under
// ~/.config.
stood::path folder() {
    if (const char* given = std::getenv("XDG_CONFIG_HOME"); given != nullptr && *given == '/')
        return stood::path{given} / "autostart";

    return fs::home() / ".config" / "autostart";
}

// The desktop entry file of the entry with the given name.
stood::path file_of(const std::string& name) { return folder() / (name + ".desktop"); }

// A value of type "string" in a .desktop file: the backslash and the tab are written as escapes.
std::string valued(std::string_view text) {
    std::string made;

    for (const char one : text) {
        if (one == '\\') made += "\\\\";
        else if (one == '\t') made += "\\t";
        else made += one;
    }

    return made;
}

// An argument in Exec according to the Desktop Entry specification: a plain one goes as it is; one
// with any reserved character goes in double quotes, inside which the backslash, double quote,
// backtick and dollar get a backslash. A percent sign is always doubled (%%), because a single one
// is a field code. Then the whole is written as a "string" value, hence four backslashes for one.
// A line break cannot be written at all and throws `Trouble`.
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

// The desktop entry for the command. The environment goes in front of the program through env, the
// working folder into Path; a line break in the folder throws `Trouble`.
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

// Writes the entry file next to its place and renames it over, so the desktop environment never
// reads half an entry. Failures throw `Trouble`.
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

// Whether an entry with the given name exists, checked on the I/O lane.
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

// Registers the command to start at login under the given name, replacing an entry of the same
// name. On Windows a working folder or environment is refused with `Trouble`.
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

// Removes the entry with the given name; a missing entry is not an error.
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

        // A missing file is not an error for `remove` but a false result, which is what we want.
        if (why) throw Trouble{"Cannot remove the login entry.", why.value()};
#endif
    });
}

}

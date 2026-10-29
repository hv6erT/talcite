#include "proving.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include "talcite/bytes.hpp"
#include "talcite/claim.hpp"
#include "talcite/fs.hpp"
#include "talcite/login.hpp"
#include "talcite/shield.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace talcite;

// A directory for the user alone and a single-instance lock: the sharing daemon in twa keeps its
// local socket in the first and checks with the second whether it is already running.
namespace {

namespace stood = std::filesystem;

// A path for a lock file in the temporary directory, with nothing left there from an earlier run.
stood::path spot(const std::string& name) {
    const stood::path made = fs::temp() / ("talcite-blokada-" + name);

    std::error_code why;

    stood::remove(made, why);

    return made;
}

#ifndef _WIN32

// Whether `where` is a real directory (not a symlink) owned by us and closed to group and others.
bool private_folder(const stood::path& where) {
    struct stat told{};

    return ::lstat(where.c_str(), &told) == 0 && S_ISDIR(told.st_mode) && told.st_uid == ::geteuid() &&
           (told.st_mode & 077) == 0;
}

// An environment variable for the duration of one trial, and back.
struct Varied {
    // Sets `name` to `value` (or unsets it for `nullptr`), remembering the previous value.
    Varied(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = std::string{was};

        if (value != nullptr) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }

    // Restores the previous value, or unsets the variable if it had none.
    ~Varied() {
        if (was_) ::setenv(name_, was_->c_str(), 1);
        else ::unsetenv(name_);
    }

    Varied(const Varied&) = delete;
    Varied& operator=(const Varied&) = delete;

private:
    const char* name_;
    std::optional<std::string> was_;
};

#endif

}

TRIAL("Runtime directory: exists and is ours alone") {
    const fs::path made = fs::runtime().get();

    ought(!made.empty());
    ought(stood::is_directory(made));

#ifndef _WIN32
    ought(private_folder(made));
#endif
}

#ifndef _WIN32

// An XDG_RUNTIME_DIR that is not private (open to everyone) is not taken; our own directory in the
// temporary directory remains, and its too loose permissions are narrowed.
TRIAL("Runtime directory: a foreign or open XDG_RUNTIME_DIR is not taken") {
    const stood::path open = fs::temp() / "talcite-ulotny-otwarty";

    std::error_code why;

    stood::create_directories(open, why);
    ::chmod(open.c_str(), 0777);

    const Varied runtime{"XDG_RUNTIME_DIR", open.c_str()};

    const stood::path own = fs::temp() / ("talcite-" + std::to_string(::geteuid()));

    if (stood::is_directory(own, why)) ::chmod(own.c_str(), 0755);

    const fs::path made = fs::runtime().get();

    same(made, own);
    ought(private_folder(made));

    // A private XDG_RUNTIME_DIR wins.
    ::chmod(open.c_str(), 0700);

    same(fs::runtime().get(), open);
}

// Someone else's directory (in XDG_RUNTIME_DIR, or created before us in the temporary directory
// under our name) is not taken: the first gives way to our own, the second is a refusal. Only an
// administrator can give a directory to someone else, so without those rights the trial skips.
TRIAL("Runtime directory: someone else's directory is not ours") {
    const stood::path room = fs::temp() / "talcite-ulotny-cudzy";

    std::error_code why;

    stood::remove_all(room, why);
    stood::create_directories(room / "xdg", why);
    ::chmod((room / "xdg").c_str(), 0700);

    if (::chown((room / "xdg").c_str(), 65534, 65534) != 0) {
        std::cout << "        without administrator rights a directory cannot be given away, trial skipped\n";

        return;
    }

    const Varied temporary{"TMPDIR", room.c_str()};
    const Varied runtime{"XDG_RUNTIME_DIR", (room / "xdg").c_str()};

    const stood::path own = room / ("talcite-" + std::to_string(::geteuid()));

    same(fs::runtime().get(), own);

    stood::remove_all(own, why);
    stood::create_directories(own, why);
    ::chmod(own.c_str(), 0700);

    ought(::chown(own.c_str(), 65534, 65534) == 0);

    threw(fs::runtime().get());

    stood::remove_all(room, why);
}

#endif

// A second lock on the same file (also in the same program, because POSIX locks the open file
// description and Windows the file without sharing) gets nothing: "already running" is an answer,
// not a refusal. After release the next one takes it.
TRIAL("Lock: one instance, nothing for the second, and after release the next one") {
    const stood::path where = spot("jedna");

    std::optional<Claim> first = Claim::taken(where).get();

    ought(first.has_value() && first->held());
    ought(first.has_value() && first->where() == where);

    const std::optional<Claim> second = Claim::taken(where).get();

    ought(!second.has_value());

    if (first) first->release();

    ought(first.has_value() && !first->held());

    const std::optional<Claim> third = Claim::taken(where).get();

    ought(third.has_value() && third->held());
}

// A copy of the handle holds the same lock: releasing one copy does not release the others; only
// the last one to go does.
TRIAL("Lock: the last copy of the handle holds it") {
    const stood::path where = spot("kopie");

    std::optional<Claim> one = Claim::taken(where).get();

    ought(one.has_value());

    if (!one) return;

    {
        const Claim copy = *one;

        one.reset();

        ought(!Claim::taken(where).get().has_value());
    }

    ought(Claim::taken(where).get().has_value());
}

// The holder removes the file while it holds the lock: a lock no longer needed goes away without
// a moment in which someone else could take it under the old name. After the release nothing is
// left, and the next claim is an ordinary one.
TRIAL("Lock: the holder removes the file while holding it") {
    const stood::path where = spot("usuniecie");

    std::optional<Claim> first = Claim::taken(where).get();

    ought(first.has_value());

    if (!first) return;

    fs::erase(where).get();

    first->release();

    std::error_code why;

    ought(!stood::exists(where, why));

    const std::optional<Claim> next = Claim::taken(where).get();

    ought(next.has_value() && next->held());
}

// A place where the file cannot be created is a refusal, not nothing, because nobody runs there.
TRIAL("Lock: a missing directory is a refusal, not already running") {
    threw(Claim::taken(fs::temp() / "talcite-nie-ma-takiego-katalogu" / "blokada").get());
}

#ifndef _WIN32

// A symlink in place of the lock file is not followed, so someone else's link cannot choose the
// file we lock.
TRIAL("Lock: does not follow a symlink") {
    const stood::path where = spot("dowiazanie");
    const stood::path aim = spot("dowiazanie-cel");

    std::error_code why;

    stood::create_symlink(aim, where, why);

    threw(Claim::taken(where).get());
    ought(!stood::exists(aim, why));
}

#endif

// A secret for this user alone: on Windows the system encrypts it and it comes back the same, and
// encrypted it is not in the clear; elsewhere the bytes stay as they are (a file for the owner
// alone protects them).
TRIAL("Secret: the same there and back, and not in the clear on Windows") {
    const Bytes plain = bytes("słowo-sekret 48 bitów");

    const Bytes kept = shielded(plain);

    same(unshielded(kept), plain);

    if (shielding()) {
        ought(kept != plain);

        Bytes broken = kept;

        broken[broken.size() / 2] = static_cast<std::byte>(static_cast<unsigned char>(broken[broken.size() / 2]) ^ 0x5a);

        threw((void)unshielded(broken));
    } else {
        ought(kept == plain);
    }

    same(unshielded(shielded(Bytes{})), Bytes{});
}

// A program at login: the daemon comes up after a restart as long as it shares something.
// Linux: a .desktop file in $XDG_CONFIG_HOME/autostart (here a substituted directory); Windows: a
// value in the user's Run key (under Wine, its own registry).
TRIAL("Login: an entry is added, replaced and removed, and a malformed name is a refusal") {
#ifdef _WIN32
    const std::string name = "talcite-proba";

    login::withdraw(name).get();

    ought(!login::enrolled(name).get());

    // A program every Windows has, where this system keeps it: the entry names a program that must
    // exist, and a guessed path (C:\Windows\notepad.exe) did not on the machine of the first run.
    std::wstring system(MAX_PATH, L'\0');

    system.resize(GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size())));

    const std::wstring program = system + L"\\cmd.exe";

    Command what;

    what.program = program;
    what.arguments = {"a b", "c"};

    login::enrol(name, what).get();

    ought(login::enrolled(name).get());

    std::wstring told(512, L'\0');
    DWORD much = static_cast<DWORD>(told.size() * sizeof(wchar_t));

    same(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"talcite-proba",
                      RRF_RT_REG_SZ, nullptr, told.data(), &much),
         LSTATUS{ERROR_SUCCESS});

    told.resize(much / sizeof(wchar_t) - 1);

    ought(told == program + L" \"a b\" c");

    // Windows offers neither a working directory nor an environment here: a refusal, not a silent
    // omission.
    Command elsewhere = what;

    elsewhere.where = system;

    threw(login::enrol(name, elsewhere).get());

    login::withdraw(name).get();
    login::withdraw(name).get();

    ought(!login::enrolled(name).get());
#else
    const stood::path root = fs::temp() / "talcite-logowanie";

    std::error_code why;

    stood::remove_all(root, why);

    const Varied config{"XDG_CONFIG_HOME", root.c_str()};

    const std::string name = "talcite-proba";

    ought(!login::enrolled(name).get());

    login::withdraw(name).get();

    Command what;

    what.program = "/opt/twa dir/twad";
    what.arguments = {"--a", "b c", "100%", "x$y", "back\\slash"};
    what.where = "/tmp/w";
    what.environment = {Variable{"A", "b"}};

    login::enrol(name, what).get();

    ought(login::enrolled(name).get());

    const stood::path file = root / "autostart" / "talcite-proba.desktop";

    const auto inside = [&file] {
        std::ifstream input{file, std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };

    same(inside(), std::string{"[Desktop Entry]\nType=Application\nName=talcite-proba\n"
                               "Exec=env A=b \"/opt/twa dir/twad\" --a \"b c\" 100%% \"x\\\\$y\" \"back\\\\\\\\slash\"\n"
                               "Path=/tmp/w\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"});

    // A second entry under the same name replaces the first, leaving no trace of the file in
    // progress.
    Command plain;

    plain.program = "/usr/bin/twad";

    login::enrol(name, plain).get();

    same(inside(), std::string{"[Desktop Entry]\nType=Application\nName=talcite-proba\nExec=/usr/bin/twad\n"
                               "Terminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"});
    ought(!stood::exists(stood::path{file}.concat(".part")));

    // A line end in an argument cannot be written: a refusal, and the old entry stays.
    Command broken = plain;

    broken.arguments = {"a\nb"};

    threw(login::enrol(name, broken).get());
    ought(login::enrolled(name).get());

    login::withdraw(name).get();
    login::withdraw(name).get();

    ought(!login::enrolled(name).get());
    ought(!stood::exists(file));

    // Without XDG_CONFIG_HOME (or with a relative path): ~/.config.
    const Varied home{"HOME", root.c_str()};
    const Varied relative{"XDG_CONFIG_HOME", "wzgledna"};

    login::enrol(name, plain).get();

    ought(stood::exists(root / ".config" / "autostart" / "talcite-proba.desktop"));

    login::withdraw(name).get();
#endif

    for (const char* bad : {"", ".ukryta", "z/ukosnikiem", "spacja w nazwie", "ąę"}) {
        threw(login::enrolled(bad).get());
        threw(login::withdraw(bad).get());
    }

    Command some;

    some.program = fs::temp() / "talcite-program";

    threw(login::enrol(std::string(65, 'a'), some).get());
    threw(login::enrol(name, Command{}).get());

    ought(!login::enrolled(name).get());
}

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

// **Katalog tylko dla użytkownika i blokada jednej kopii** (S1): demon udostępniania w twa trzyma
// w pierwszym gniazdo lokalne, a drugą sprawdza, czy już działa.
namespace {

namespace stood = std::filesystem;

stood::path spot(const std::string& name) {
    const stood::path made = fs::temp() / ("talcite-blokada-" + name);

    std::error_code why;

    stood::remove(made, why);

    return made;
}

#ifndef _WIN32

bool private_folder(const stood::path& where) {
    struct stat told{};

    return ::lstat(where.c_str(), &told) == 0 && S_ISDIR(told.st_mode) && told.st_uid == ::geteuid() &&
           (told.st_mode & 077) == 0;
}

// Zmienna środowiska na czas jednej próby i z powrotem.
struct Varied {
    Varied(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = std::string{was};

        if (value != nullptr) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }

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

TRIAL("Katalog ulotny: istnieje i jest tylko nasz") {
    const fs::path made = fs::runtime().await();

    ought(!made.empty());
    ought(stood::is_directory(made));

#ifndef _WIN32
    ought(private_folder(made));
#endif
}

#ifndef _WIN32

// XDG_RUNTIME_DIR, który nie jest prywatny — otwarty dla wszystkich — nie jest brany; zostaje
// własny katalog w katalogu tymczasowym, a jego zbyt luźne prawa zwężamy.
TRIAL("Katalog ulotny: cudzy albo otwarty XDG_RUNTIME_DIR nie jest brany") {
    const stood::path open = fs::temp() / "talcite-ulotny-otwarty";

    std::error_code why;

    stood::create_directories(open, why);
    ::chmod(open.c_str(), 0777);

    const Varied runtime{"XDG_RUNTIME_DIR", open.c_str()};

    const stood::path own = fs::temp() / ("talcite-" + std::to_string(::geteuid()));

    if (stood::is_directory(own, why)) ::chmod(own.c_str(), 0755);

    const fs::path made = fs::runtime().await();

    same(made, own);
    ought(private_folder(made));

    // Prywatny XDG_RUNTIME_DIR wygrywa.
    ::chmod(open.c_str(), 0700);

    same(fs::runtime().await(), open);
}

// Cudzy katalog — w XDG_RUNTIME_DIR albo założony przed nami w katalogu tymczasowym pod naszą
// nazwą — nie jest brany: ten pierwszy ustępuje własnemu, ten drugi jest odmową. Oddać katalog
// komuś innemu umie tylko administrator, więc bez tych praw próba się pomija.
TRIAL("Katalog ulotny: cudzy katalog nie jest nasz") {
    const stood::path room = fs::temp() / "talcite-ulotny-cudzy";

    std::error_code why;

    stood::remove_all(room, why);
    stood::create_directories(room / "xdg", why);
    ::chmod((room / "xdg").c_str(), 0700);

    if (::chown((room / "xdg").c_str(), 65534, 65534) != 0) {
        std::cout << "        bez praw administratora nie da się oddać katalogu — próba pominięta\n";

        return;
    }

    const Varied temporary{"TMPDIR", room.c_str()};
    const Varied runtime{"XDG_RUNTIME_DIR", (room / "xdg").c_str()};

    const stood::path own = room / ("talcite-" + std::to_string(::geteuid()));

    same(fs::runtime().await(), own);

    stood::remove_all(own, why);
    stood::create_directories(own, why);
    ::chmod(own.c_str(), 0700);

    ought(::chown(own.c_str(), 65534, 65534) == 0);

    threw(fs::runtime().await());

    stood::remove_all(room, why);
}

#endif

// Druga blokada tego samego pliku — także w tym samym programie, bo POSIX blokuje opis otwarcia,
// a Windows plik bez dzielenia — dostaje pustkę: „już działa" to odpowiedź, nie odmowa. Po
// zwolnieniu bierze ją następny.
TRIAL("Blokada: jedna kopia, pustka dla drugiej, a po zwolnieniu następna") {
    const stood::path where = spot("jedna");

    std::optional<Claim> first = Claim::taken(where).await();

    ought(first.has_value() && first->held());
    ought(first.has_value() && first->where() == where);

    const std::optional<Claim> second = Claim::taken(where).await();

    ought(!second.has_value());

    if (first) first->release();

    ought(first.has_value() && !first->held());

    const std::optional<Claim> third = Claim::taken(where).await();

    ought(third.has_value() && third->held());
}

// Kopia uchwytu trzyma tę samą blokadę: zwolnienie jednej kopii nie zwalnia pozostałych, dopiero
// ostatnia, która zginie.
TRIAL("Blokada: trzyma ją ostatnia kopia uchwytu") {
    const stood::path where = spot("kopie");

    std::optional<Claim> one = Claim::taken(where).await();

    ought(one.has_value());

    if (!one) return;

    {
        const Claim copy = *one;

        one.reset();

        ought(!Claim::taken(where).await().has_value());
    }

    ought(Claim::taken(where).await().has_value());
}

// Miejsce, w którym pliku założyć nie można, to odmowa — nie pustka, bo nikt tam nie działa.
TRIAL("Blokada: brak katalogu to odmowa, nie „już działa”") {
    threw(Claim::taken(fs::temp() / "talcite-nie-ma-takiego-katalogu" / "blokada").await());
}

#ifndef _WIN32

// Dowiązanie w miejscu pliku blokady nie jest przechodzone — cudze dowiązanie nie wskaże pliku,
// który zablokujemy.
TRIAL("Blokada: dowiązania nie przechodzi") {
    const stood::path where = spot("dowiazanie");
    const stood::path aim = spot("dowiazanie-cel");

    std::error_code why;

    stood::create_symlink(aim, where, why);

    threw(Claim::taken(where).await());
    ought(!stood::exists(aim, why));
}

#endif

// Sekret tylko dla tego użytkownika: na Windows szyfruje go system i wraca ten sam, a zaszyfrowany
// nie jest jawny; gdzie indziej bajty zostają, jakie są (chroni je plik tylko dla właściciela).
TRIAL("Sekret: tam i z powrotem ten sam, a na Windows nie jawnie") {
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

// **Program przy logowaniu** (S1): demon wstaje po ponownym uruchomieniu, póki coś udostępnia.
// Linux: plik `.desktop` w `$XDG_CONFIG_HOME/autostart` — tu podstawiony katalog; Windows: wartość
// w kluczu `Run` użytkownika — pod Wine to jego własny rejestr.
TRIAL("Logowanie: wpis staje, podmienia się i schodzi, a nazwa spoza kształtu to odmowa") {
#ifdef _WIN32
    const std::string name = "talcite-proba";

    login::withdraw(name).await();

    ought(!login::enrolled(name).await());

    Command what;

    what.program = L"C:\\windows\\notepad.exe";
    what.arguments = {"a b", "c"};

    login::enrol(name, what).await();

    ought(login::enrolled(name).await());

    std::wstring told(512, L'\0');
    DWORD much = static_cast<DWORD>(told.size() * sizeof(wchar_t));

    same(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"talcite-proba",
                      RRF_RT_REG_SZ, nullptr, told.data(), &much),
         LSTATUS{ERROR_SUCCESS});

    told.resize(much / sizeof(wchar_t) - 1);

    ought(told == L"C:\\windows\\notepad.exe \"a b\" c");

    // Katalogu roboczego ani środowiska Windows tu nie da — odmowa, nie ciche pominięcie.
    Command elsewhere = what;

    elsewhere.where = L"C:\\windows";

    threw(login::enrol(name, elsewhere).await());

    login::withdraw(name).await();
    login::withdraw(name).await();

    ought(!login::enrolled(name).await());
#else
    const stood::path root = fs::temp() / "talcite-logowanie";

    std::error_code why;

    stood::remove_all(root, why);

    const Varied config{"XDG_CONFIG_HOME", root.c_str()};

    const std::string name = "talcite-proba";

    ought(!login::enrolled(name).await());

    login::withdraw(name).await();

    Command what;

    what.program = "/opt/twa dir/twad";
    what.arguments = {"--a", "b c", "100%", "x$y", "back\\slash"};
    what.where = "/tmp/w";
    what.environment = {Variable{"A", "b"}};

    login::enrol(name, what).await();

    ought(login::enrolled(name).await());

    const stood::path file = root / "autostart" / "talcite-proba.desktop";

    const auto inside = [&file] {
        std::ifstream input{file, std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };

    same(inside(), std::string{"[Desktop Entry]\nType=Application\nName=talcite-proba\n"
                               "Exec=env A=b \"/opt/twa dir/twad\" --a \"b c\" 100%% \"x\\\\$y\" \"back\\\\\\\\slash\"\n"
                               "Path=/tmp/w\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"});

    // Drugi wpis pod tą samą nazwą podmienia pierwszy, bez śladu po pliku w toku.
    Command plain;

    plain.program = "/usr/bin/twad";

    login::enrol(name, plain).await();

    same(inside(), std::string{"[Desktop Entry]\nType=Application\nName=talcite-proba\nExec=/usr/bin/twad\n"
                               "Terminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n"});
    ought(!stood::exists(stood::path{file}.concat(".part")));

    // Końca wiersza w argumencie nie da się zapisać — odmowa, a stary wpis zostaje.
    Command broken = plain;

    broken.arguments = {"a\nb"};

    threw(login::enrol(name, broken).await());
    ought(login::enrolled(name).await());

    login::withdraw(name).await();
    login::withdraw(name).await();

    ought(!login::enrolled(name).await());
    ought(!stood::exists(file));

    // Bez XDG_CONFIG_HOME (albo ze ścieżką względną) — `~/.config`.
    const Varied home{"HOME", root.c_str()};
    const Varied relative{"XDG_CONFIG_HOME", "wzgledna"};

    login::enrol(name, plain).await();

    ought(stood::exists(root / ".config" / "autostart" / "talcite-proba.desktop"));

    login::withdraw(name).await();
#endif

    for (const char* bad : {"", ".ukryta", "z/ukosnikiem", "spacja w nazwie", "ąę"}) {
        threw(login::enrolled(bad).await());
        threw(login::withdraw(bad).await());
    }

    Command some;

    some.program = fs::temp() / "talcite-program";

    threw(login::enrol(std::string(65, 'a'), some).await());
    threw(login::enrol(name, Command{}).await());

    ought(!login::enrolled(name).await());
}

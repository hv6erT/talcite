#include "proving.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "talcite/share.hpp"

#include "fscore.hpp"
#include "sharing.hpp"

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

class Nook {
public:
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-share-" + std::to_string(++counted) + "-" +
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

// --- Adres --------------------------------------------------------------------------------------

TRIAL("Udział: cztery zapisy jednego miejsca dają to samo") {
    for (const char* said : {"\\\\NAS\\filmy\\HD", "//NAS/filmy/HD", "smb://NAS/filmy/HD", "SMB://NAS/filmy/HD/",
                             "  \\\\NAS/filmy\\HD  "}) {
        const std::optional<fs::Share> one = fs::addressed(said);

        ought(one.has_value());

        if (!one) continue;

        same(one->server, std::string{"NAS"});
        same(one->name, std::string{"filmy"});
        same(one->rest, std::string{"HD"});
        same(one->user, std::string{});
    }
}

TRIAL("Udział: adres IP, głębsza reszta i kody procentowe") {
    const std::optional<fs::Share> one = fs::addressed("smb://192.168.1.10/filmy/Filmy%20HD/2024");

    ought(one.has_value());

    if (!one) return;

    same(one->server, std::string{"192.168.1.10"});
    same(one->name, std::string{"filmy"});
    same(one->rest, std::string{"Filmy HD/2024"});

    // W UNC procent jest zwykłym znakiem nazwy.
    const std::optional<fs::Share> two = fs::addressed("\\\\nas\\100%20\\x");

    ought(two.has_value());

    if (two) same(two->name, std::string{"100%20"});

    // Krzywy kod zostaje, jak stoi.
    const std::optional<fs::Share> three = fs::addressed("smb://nas/a%2/b%zz");

    ought(three.has_value());

    if (three) {
        same(three->name, std::string{"a%2"});
        same(three->rest, std::string{"b%zz"});
    }
}

TRIAL("Udział: użytkownik z URL-a, hasło z adresu odpada") {
    const std::optional<fs::Share> one = fs::addressed("smb://hubert@nas/filmy");

    ought(one.has_value());

    if (one) {
        same(one->user, std::string{"hubert"});
        same(one->server, std::string{"nas"});
        same(one->name, std::string{"filmy"});
    }

    const std::optional<fs::Share> two = fs::addressed("smb://DOM%5Chubert:tajne@nas/filmy");

    ought(two.has_value());

    if (two) {
        same(two->user, std::string{"DOM\\hubert"});
        ought(fs::written(*two).find("tajne") == std::string::npos);
    }

    // Małpa w ścieżce po serwerze to część nazwy, nie użytkownik.
    const std::optional<fs::Share> three = fs::addressed("smb://nas/filmy/@eaDir");

    ought(three.has_value());

    if (three) {
        same(three->user, std::string{});
        same(three->rest, std::string{"@eaDir"});
    }
}

TRIAL("Udział: sam serwer i adresy, które udziałem nie są") {
    for (const char* said : {"smb://nas", "smb://nas/", "\\\\nas", "\\\\nas\\", "//nas"}) {
        const std::optional<fs::Share> one = fs::addressed(said);

        ought(one.has_value());

        if (!one) continue;

        same(one->server, std::string{"nas"});
        same(one->name, std::string{});
        same(one->rest, std::string{});
    }

    for (const char* said : {"", "   ", "C:\\Filmy", "/mnt/filmy", "filmy", "\\filmy", "smb://", "\\\\", "//", "http://nas/x"})
        ought(!fs::addressed(said).has_value());
}

TRIAL("Udział: zapis do pokazania w zapisie systemu") {
    fs::Share one{"NAS", "filmy", "Filmy HD/2024", "hubert"};

#ifdef _WIN32
    same(fs::written(one), std::string{"\\\\NAS\\filmy\\Filmy HD\\2024"});
    same(fs::written(fs::Share{"NAS", "", "", ""}), std::string{"\\\\NAS"});
#else
    same(fs::written(one), std::string{"smb://hubert@NAS/filmy/Filmy HD/2024"});
    same(fs::written(fs::Share{"NAS", "", "", ""}), std::string{"smb://NAS"});
    same(fs::written(fs::Share{"NAS", "filmy", "", ""}), std::string{"smb://NAS/filmy"});
#endif

    // Zapis wraca tym samym udziałem.
    const std::optional<fs::Share> back = fs::addressed(fs::written(one));

    ought(back.has_value());

    if (back) {
        same(back->server, one.server);
        same(back->name, one.name);
        same(back->rest, one.rest);
    }
}

// --- Odmowy -------------------------------------------------------------------------------------

TRIAL("Udział: kody Windows po ludzku") {
    same(fs::refused_by_windows(0), fs::Refused::none);

    for (const std::uint32_t code : {5U, 86U, 1244U, 1326U, 1327U, 1330U, 1331U, 1385U, 2202U})
        same(fs::refused_by_windows(code), fs::Refused::login);

    for (const std::uint32_t code : {67U, 2310U}) same(fs::refused_by_windows(code), fs::Refused::missing);

    for (const std::uint32_t code : {51U, 53U, 59U, 64U, 121U, 1203U, 1222U, 1231U, 1232U})
        same(fs::refused_by_windows(code), fs::Refused::network);

    same(fs::refused_by_windows(1219), fs::Refused::conflict);

    for (const std::uint32_t code : {50U, 1200U, 1204U}) same(fs::refused_by_windows(code), fs::Refused::unsupported);

    same(fs::refused_by_windows(87), fs::Refused::other);
}

TRIAL("Udział: zdania gio po ludzku") {
    same(fs::refused_by_gio("gio: smb://nas/filmy/: Failed to mount Windows share: Permission denied"),
         fs::Refused::login);
    same(fs::refused_by_gio("Password required for share filmy on nas\nUser [hubert]: Domain [WORKGROUP]: Password: \n"
                            "gio: smb://nas/filmy/: Password dialog cancelled"),
         fs::Refused::login);
    // Samo przerwanie pytania, bez słowa o haśle.
    same(fs::refused_by_gio("gio: smb://nas/filmy/: Operation was cancelled"), fs::Refused::login);
    same(fs::refused_by_gio("gio: smb://nas/kino/: Failed to mount Windows share: No such file or directory"),
         fs::Refused::missing);
    same(fs::refused_by_gio("gio: smb://nas/: Failed to retrieve share list from server: Connection timed out"),
         fs::Refused::network);
    same(fs::refused_by_gio("gio: smb://nas/filmy/: Failed to mount Windows share: No route to host"),
         fs::Refused::network);
    same(fs::refused_by_gio("gio: smb://nas/filmy/: The specified location is not supported"), fs::Refused::unsupported);
    same(fs::refused_by_gio("gio: smb://nas/: The specified location is not mounted"), fs::Refused::unsupported);
    // Bez GVfs (sam `gio`, jak w kontenerze bez D-Bus).
    same(fs::refused_by_gio("gio: smb://127.0.0.1/x: volume doesn?t implement mount"), fs::Refused::unsupported);
    same(fs::refused_by_gio("gio: smb://127.0.0.1/: Operation not supported"), fs::Refused::unsupported);
    same(fs::refused_by_gio("gio: coś zupełnie innego"), fs::Refused::other);
}

// --- GVfs ---------------------------------------------------------------------------------------

TRIAL("Udział: montowanie GVfs po serwerze i udziale, bez wielkości liter") {
    const std::vector<std::string> names{"sftp:host=nas", "smb-share:server=nas,share=muzyka",
                                         "smb-share:domain=WORKGROUP,server=nas,share=filmy,user=ola",
                                         "smb-share:server=nas,share=filmy,user=hubert"};

    same(fs::mounted_as(names, fs::Share{"NAS", "Filmy", "", ""}),
         std::optional<std::string>{"smb-share:domain=WORKGROUP,server=nas,share=filmy,user=ola"});
    same(fs::mounted_as(names, fs::Share{"nas", "filmy", "", "hubert"}),
         std::optional<std::string>{"smb-share:server=nas,share=filmy,user=hubert"});
    same(fs::mounted_as(names, fs::Share{"nas", "muzyka", "", ""}),
         std::optional<std::string>{"smb-share:server=nas,share=muzyka"});
    ought(!fs::mounted_as(names, fs::Share{"nas", "kino", "", ""}).has_value());
    ought(!fs::mounted_as(names, fs::Share{"nas2", "filmy", "", ""}).has_value());

    // Wartość z kodem procentowym, jak GVfs zapisuje spacje.
    same(fs::mounted_as({"smb-share:server=nas,share=moje%20filmy"}, fs::Share{"nas", "Moje filmy", "", ""}),
         std::optional<std::string>{"smb-share:server=nas,share=moje%20filmy"});
}

// Nazwa katalogu GVfs w drugą stronę: serwer, udział i użytkownik, z kodami procentowymi. Inne
// katalogi GVfs (sftp, sam serwer) udziałem nie są.
TRIAL("Udział: katalog GVfs mówi, jaki to udział") {
    const std::optional<fs::Share> one = fs::gvfs_named("smb-share:domain=WORKGROUP,server=nas,share=moje%20filmy,user=ola");

    ought(one.has_value());

    if (one) {
        same(one->server, std::string{"nas"});
        same(one->name, std::string{"moje filmy"});
        same(one->user, std::string{"ola"});
        same(one->rest, std::string{});
    }

    ought(!fs::gvfs_named("sftp:host=nas").has_value());
    ought(!fs::gvfs_named("smb-server:server=nas").has_value());
    ought(!fs::gvfs_named("smb-share:server=nas").has_value());
    ought(!fs::gvfs_named("smb-share:share=filmy").has_value());
}

TRIAL("Udział: wejście dla gio — użytkownik, domena, hasło") {
    same(fs::answers("hubert", "tajne"), std::string{"hubert\n\ntajne\n"});
    same(fs::answers("DOM\\hubert", "tajne"), std::string{"hubert\nDOM\ntajne\n"});
    same(fs::answers("DOM;hubert", "x"), std::string{"hubert\nDOM\nx\n"});
}

TRIAL("Udział: wykaz z gio list bez ukrytych") {
    same(fs::listed("filmy\nmuzyka/\nIPC$\nprint$\n\n  Zdjęcia rodzinne  \n"),
         (std::vector<std::string>{"filmy", "muzyka", "Zdjęcia rodzinne"}));
    same(fs::listed(""), std::vector<std::string>{});
}

TRIAL("Udział: hasło zerowane w miejscu") {
    std::string secret = "tajne hasło";

    const char* at = secret.data();

    fs::wiped(secret);

    ought(secret.empty());
    same(at[0], '\0');
    same(at[4], '\0');
}

#ifndef _WIN32

TRIAL("Udział: zamontowany w GVfs wchodzi bez gio, z resztą ścieżki") {
    const Nook nook;

    std::error_code ignored;

    stood::create_directories(nook.where() / "gvfs" / "smb-share:server=nas,share=filmy" / "HD", ignored);

    const Planted runtime{"XDG_RUNTIME_DIR", nook.where().c_str()};

    const fs::Reached made = fs::reach(fs::Share{"NAS", "filmy", "HD", ""}).await();

    same(made.refused, fs::Refused::none);
    same(made.where, nook.where() / "gvfs" / "smb-share:server=nas,share=filmy" / "HD");
}

// Ścieżka w katalogu GVfs leży na udziale — z resztą pod nim; ścieżka obok GVfs, sam katalog GVfs
// i katalog, który udziałem nie jest — nie.
TRIAL("Udział: shared rozpoznaje ścieżkę w GVfs") {
    const Nook nook;

    const Planted runtime{"XDG_RUNTIME_DIR", nook.where().c_str()};

    const stood::path mounted = nook.where() / "gvfs" / "smb-share:server=nas,share=filmy,user=hubert";

    const std::optional<fs::Share> deep = fs::shared(mounted / "HD" / "film.mkv");

    ought(deep.has_value());

    if (deep) {
        same(deep->server, std::string{"nas"});
        same(deep->name, std::string{"filmy"});
        same(deep->user, std::string{"hubert"});
        same(deep->rest, std::string{"HD/film.mkv"});
        same(fs::written(*deep), std::string{"smb://hubert@nas/filmy/HD/film.mkv"});
    }

    const std::optional<fs::Share> top = fs::shared(mounted);

    ought(top.has_value() && top->rest.empty());

    ought(!fs::shared(nook.where() / "gvfs").has_value());
    ought(!fs::shared(nook.where() / "gvfs" / "sftp:host=nas" / "x").has_value());
    ought(!fs::shared(nook.where() / "inny" / "smb-share:server=nas,share=filmy").has_value());
    ought(!fs::shared("/home/hubert").has_value());
}

TRIAL("Udział: bez XDG_RUNTIME_DIR nie ma GVfs i gio się nie woła") {
    const Nook nook;

    // Podróbka `gio` na PATH zostawia ślad — bez GVfs nie ma jej po co wołać.
    {
        std::ofstream script{nook.where() / "gio"};

        script << "#!/bin/sh\ntouch \"" << nook.where().string() << "/called\"\n";
    }

    stood::permissions(nook.where() / "gio", stood::perms::owner_all);

    const char* was = std::getenv("PATH");
    const Planted path{"PATH", (nook.where().string() + ":" + (was != nullptr ? was : "/usr/bin:/bin")).c_str()};

    for (const char* value : {"", "pusty"}) {
        const Planted runtime{"XDG_RUNTIME_DIR", "x"};

        // Pusta wartość to też brak — `Planted` pustą zdejmuje, więc stawiamy ją wprost.
        if (*value == '\0') ::setenv("XDG_RUNTIME_DIR", "", 1);
        else ::unsetenv("XDG_RUNTIME_DIR");

        same(fs::reach(fs::Share{"nas", "filmy", "", ""}).await().refused, fs::Refused::unsupported);
        same(fs::connect(fs::Share{"nas", "filmy", "", ""}, "hubert", "tajne", false).await().refused,
             fs::Refused::unsupported);
    }

    ought(!stood::exists(nook.where() / "called"));
    ought(!fs::keeps_logins());
}

#else

TRIAL("Udział: Windows pamięta logowania") { ought(fs::keeps_logins()); }

// Na Windows udział ma ścieżkę UNC: shared czyta ją jak adres, a ścieżka z literą dysku udziałem nie jest.
TRIAL("Udział: shared rozpoznaje ścieżkę UNC") {
    const std::optional<fs::Share> one = fs::shared(fs::path{L"\\\\nas\\filmy\\HD"});

    ought(one.has_value());

    if (one) {
        same(one->server, std::string{"nas"});
        same(one->name, std::string{"filmy"});
        same(one->rest, std::string{"HD"});
    }

    ought(!fs::shared(fs::path{L"C:\\Filmy"}).has_value());
}

#endif

// --- Zmiana samej wielkości liter ----------------------------------------------------------------

TRIAL("Udział: zmiana samej wielkości liter to ta sama nazwa w tym samym katalogu") {
    ought(fs::core::recased("kat/heat.mkv", "kat/Heat.mkv"));
    ought(fs::core::recased("kat/żółw", "kat/ŻÓŁW"));
    ought(fs::core::recased("kat/Ёлка", "kat/ёЛКА"));
    ought(!fs::core::recased("kat/heat.mkv", "kat/heat.mkv"));
    ought(!fs::core::recased("kat/heat.mkv", "kat/Heat.avi"));
    ought(!fs::core::recased("kat/heat.mkv", "kat/Heat.mkv2"));
    ought(!fs::core::recased("kat/heat.mkv2", "kat/Heat.mkv"));
    ought(!fs::core::recased("kat/heat.mkv", "inny/Heat.mkv"));
}

// Serwer, który zmianę wielkości przyjmuje bez słowa i nic nie robi: pierwsze sprawdzenie spisu
// mówi „nie ma", więc idą dwa kroki przez nazwę obok.
TRIAL("Udział: zignorowana zmiana wielkości idzie dwoma krokami") {
    std::vector<std::pair<std::string, std::string>> moves;

    const auto move = [&moves](const fs::path& one, const fs::path& two) {
        moves.emplace_back(one.filename().string(), two.filename().string());

        return true;
    };

    ought(fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv", move, [](const fs::path&) { return false; }));

    same(moves, (std::vector<std::pair<std::string, std::string>>{{"heat.mkv", "Heat.mkv.~case"},
                                                                    {"Heat.mkv.~case", "Heat.mkv"}}));

    // Serwer, który zrobił, co trzeba — bez kroków.
    moves.clear();

    ought(fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv", move, [](const fs::path&) { return true; }));
    ought(moves.empty());
}

TRIAL("Udział: drugi krok nieudany cofa pierwszy") {
    std::vector<std::pair<std::string, std::string>> moves;

    const auto move = [&moves](const fs::path& one, const fs::path& two) {
        moves.emplace_back(one.filename().string(), two.filename().string());

        return moves.size() != 2;
    };

    ought(!fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv", move, [](const fs::path&) { return false; }));

    same(moves, (std::vector<std::pair<std::string, std::string>>{{"heat.mkv", "Heat.mkv.~case"},
                                                                    {"Heat.mkv.~case", "Heat.mkv"},
                                                                    {"Heat.mkv.~case", "heat.mkv"}}));

    // Pierwszy nieudany — nic więcej.
    moves.clear();

    ought(!fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv",
                                     [&moves](const fs::path& one, const fs::path& two) {
                                         moves.emplace_back(one.filename().string(), two.filename().string());
                                         return false;
                                     },
                                     [](const fs::path&) { return false; }));
    same(moves.size(), std::size_t{1});
}

TRIAL("Udział: zmiana wielkości na dysku daje nazwę w spisie") {
    const Nook nook;

    {
        std::ofstream{nook.where() / "heat.txt"} << "x";
    }

    fs::rename(nook.where() / "heat.txt", nook.where() / "Heat.txt").await();

    std::vector<std::string> named;
    std::error_code ignored;

    for (const auto& one : stood::directory_iterator{nook.where(), ignored}) named.push_back(one.path().filename().string());

    same(named, std::vector<std::string>{"Heat.txt"});
}

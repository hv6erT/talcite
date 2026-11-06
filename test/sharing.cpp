#include "proving.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "talcite/share.hpp"

#include "fscore.hpp"
#include "sharing.hpp"

using namespace talcite;

namespace {

namespace stood = std::filesystem;

// An environment variable replaced for the length of one trial and restored after it; an empty
// value means the variable is absent.
class Planted {
public:
    // Remembers the current value of `name`, then sets it to `value`.
    Planted(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = was;

        put(value);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    // Restores the remembered value, or removes the variable if it had none.
    ~Planted() { put(was_ ? was_->c_str() : ""); }

private:
    // Sets the variable to `value`, or removes it when `value` is empty.
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

// A fresh, empty temporary directory for one trial, removed with everything in it at the end.
class Nook {
public:
    // Picks a name unique to this process and object and creates the directory empty.
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-share-" + std::to_string(++counted) + "-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this))};

        std::error_code ignored;
        stood::remove_all(where_, ignored);
        stood::create_directories(where_, ignored);
    }

    // Removes the directory with its contents, ignoring errors.
    ~Nook() {
        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Nook(const Nook&) = delete;
    Nook& operator=(const Nook&) = delete;

    // The directory's path.
    [[nodiscard]] const stood::path& where() const { return where_; }

private:
    stood::path where_;
};

}

// --- Address ------------------------------------------------------------------------------------

TRIAL("Share: four spellings of one place give the same thing") {
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

TRIAL("Share: an IP address, a deeper rest and percent codes") {
    const std::optional<fs::Share> one = fs::addressed("smb://192.168.1.10/filmy/Filmy%20HD/2024");

    ought(one.has_value());

    if (!one) return;

    same(one->server, std::string{"192.168.1.10"});
    same(one->name, std::string{"filmy"});
    same(one->rest, std::string{"Filmy HD/2024"});

    // In UNC a percent sign is an ordinary character of the name.
    const std::optional<fs::Share> two = fs::addressed("\\\\nas\\100%20\\x");

    ought(two.has_value());

    if (two) same(two->name, std::string{"100%20"});

    // A malformed code stays as it is.
    const std::optional<fs::Share> three = fs::addressed("smb://nas/a%2/b%zz");

    ought(three.has_value());

    if (three) {
        same(three->name, std::string{"a%2"});
        same(three->rest, std::string{"b%zz"});
    }
}

TRIAL("Share: the user comes from the URL, a password in the address is dropped") {
    const std::optional<fs::Share> one = fs::addressed("smb://guest@nas/filmy");

    ought(one.has_value());

    if (one) {
        same(one->user, std::string{"guest"});
        same(one->server, std::string{"nas"});
        same(one->name, std::string{"filmy"});
    }

    const std::optional<fs::Share> two = fs::addressed("smb://DOM%5Cguest:tajne@nas/filmy");

    ought(two.has_value());

    if (two) {
        same(two->user, std::string{"DOM\\guest"});
        ought(fs::written(*two).find("tajne") == std::string::npos);
    }

    // An at sign in the path after the server is part of a name, not a user.
    const std::optional<fs::Share> three = fs::addressed("smb://nas/filmy/@eaDir");

    ought(three.has_value());

    if (three) {
        same(three->user, std::string{});
        same(three->rest, std::string{"@eaDir"});
    }
}

TRIAL("Share: a bare server, and addresses that are not shares") {
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

TRIAL("Share: the spelling shown in the system's notation") {
    fs::Share one{"NAS", "filmy", "Filmy HD/2024", "guest"};

#ifdef _WIN32
    same(fs::written(one), std::string{"\\\\NAS\\filmy\\Filmy HD\\2024"});
    same(fs::written(fs::Share{"NAS", "", "", ""}), std::string{"\\\\NAS"});
#else
    same(fs::written(one), std::string{"smb://guest@NAS/filmy/Filmy HD/2024"});
    same(fs::written(fs::Share{"NAS", "", "", ""}), std::string{"smb://NAS"});
    same(fs::written(fs::Share{"NAS", "filmy", "", ""}), std::string{"smb://NAS/filmy"});
#endif

    // The spelling parses back to the same share.
    const std::optional<fs::Share> back = fs::addressed(fs::written(one));

    ought(back.has_value());

    if (back) {
        same(back->server, one.server);
        same(back->name, one.name);
        same(back->rest, one.rest);
    }
}

// --- Refusals -----------------------------------------------------------------------------------

TRIAL("Share: Windows codes in plain terms") {
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

TRIAL("Share: gio messages in plain terms") {
    same(fs::refused_by_gio("gio: smb://nas/filmy/: Failed to mount Windows share: Permission denied"),
         fs::Refused::login);
    same(fs::refused_by_gio("Password required for share filmy on nas\nUser [guest]: Domain [WORKGROUP]: Password: \n"
                            "gio: smb://nas/filmy/: Password dialog cancelled"),
         fs::Refused::login);
    // A cancelled prompt alone, with no word about a password.
    same(fs::refused_by_gio("gio: smb://nas/filmy/: Operation was cancelled"), fs::Refused::login);
    same(fs::refused_by_gio("gio: smb://nas/kino/: Failed to mount Windows share: No such file or directory"),
         fs::Refused::missing);
    same(fs::refused_by_gio("gio: smb://nas/: Failed to retrieve share list from server: Connection timed out"),
         fs::Refused::network);
    same(fs::refused_by_gio("gio: smb://nas/filmy/: Failed to mount Windows share: No route to host"),
         fs::Refused::network);
    same(fs::refused_by_gio("gio: smb://nas/filmy/: The specified location is not supported"), fs::Refused::unsupported);
    same(fs::refused_by_gio("gio: smb://nas/: The specified location is not mounted"), fs::Refused::unsupported);
    // Without GVfs (bare `gio`, as in a container without D-Bus).
    same(fs::refused_by_gio("gio: smb://127.0.0.1/x: volume doesn?t implement mount"), fs::Refused::unsupported);
    same(fs::refused_by_gio("gio: smb://127.0.0.1/: Operation not supported"), fs::Refused::unsupported);
    same(fs::refused_by_gio("gio: coś zupełnie innego"), fs::Refused::other);
}

// --- GVfs ---------------------------------------------------------------------------------------

TRIAL("Share: a GVfs mount is found by server and share, ignoring case") {
    const std::vector<std::string> names{"sftp:host=nas", "smb-share:server=nas,share=muzyka",
                                         "smb-share:domain=WORKGROUP,server=nas,share=filmy,user=ola",
                                         "smb-share:server=nas,share=filmy,user=guest"};

    same(fs::mounted_as(names, fs::Share{"NAS", "Filmy", "", ""}),
         std::optional<std::string>{"smb-share:domain=WORKGROUP,server=nas,share=filmy,user=ola"});
    same(fs::mounted_as(names, fs::Share{"nas", "filmy", "", "guest"}),
         std::optional<std::string>{"smb-share:server=nas,share=filmy,user=guest"});
    same(fs::mounted_as(names, fs::Share{"nas", "muzyka", "", ""}),
         std::optional<std::string>{"smb-share:server=nas,share=muzyka"});
    ought(!fs::mounted_as(names, fs::Share{"nas", "kino", "", ""}).has_value());
    ought(!fs::mounted_as(names, fs::Share{"nas2", "filmy", "", ""}).has_value());

    // A value with a percent code, as GVfs writes spaces.
    same(fs::mounted_as({"smb-share:server=nas,share=moje%20filmy"}, fs::Share{"nas", "Moje filmy", "", ""}),
         std::optional<std::string>{"smb-share:server=nas,share=moje%20filmy"});
}

// A GVfs directory name the other way round: server, share and user, with percent codes. Other
// GVfs directories (sftp, a bare server) are not shares.
TRIAL("Share: a GVfs directory tells which share it is") {
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

TRIAL("Share: input for gio is user, domain, password") {
    same(fs::answers("guest", "tajne"), std::string{"guest\n\ntajne\n"});
    same(fs::answers("DOM\\guest", "tajne"), std::string{"guest\nDOM\ntajne\n"});
    same(fs::answers("DOM;guest", "x"), std::string{"guest\nDOM\nx\n"});
}

TRIAL("Share: the gio list listing without hidden shares") {
    same(fs::listed("filmy\nmuzyka/\nIPC$\nprint$\n\n  Zdjęcia rodzinne  \n"),
         (std::vector<std::string>{"filmy", "muzyka", "Zdjęcia rodzinne"}));
    same(fs::listed(""), std::vector<std::string>{});
}

// A wiped secret is empty and keeps its storage, so it was not copied elsewhere on the way. That
// the storage holds zeros is `secret::wiped`'s, which wiping.cpp checks: past the end of an
// emptied string or vector nothing can be read without undefined behavior, and the address
// sanitizer of MSVC reports such a read of a vector as a container overflow.
TRIAL("Share: the password is emptied in place") {
    std::string secret = "tajne hasło, dłuższe niż bufor w samym napisie";

    const char* at = secret.data();
    const std::size_t room = secret.capacity();

    fs::wiped(secret);

    ought(secret.empty());
    ought(secret.data() == at);
    same(secret.capacity(), room);
}

TRIAL("Share: secret bytes are emptied in place") {
    Bytes secret = bytes("wspólny sekret");

    const std::byte* at = secret.data();
    const std::size_t room = secret.capacity();

    fs::wiped(secret);

    ought(secret.empty());
    ought(secret.data() == at);
    same(secret.capacity(), room);
}

#ifndef _WIN32

TRIAL("Share: one mounted in GVfs is entered without gio, with the rest of the path") {
    const Nook nook;

    std::error_code ignored;

    stood::create_directories(nook.where() / "gvfs" / "smb-share:server=nas,share=filmy" / "HD", ignored);

    const Planted runtime{"XDG_RUNTIME_DIR", nook.where().c_str()};

    const fs::Reached made = fs::reach(fs::Share{"NAS", "filmy", "HD", ""}).get();

    same(made.refused, fs::Refused::none);
    same(made.where, nook.where() / "gvfs" / "smb-share:server=nas,share=filmy" / "HD");
}

// A path inside a GVfs directory lies on a share, with the rest below it; a path beside GVfs, the
// GVfs directory itself and a directory that is not a share do not.
TRIAL("Share: shared recognizes a path in GVfs") {
    const Nook nook;

    const Planted runtime{"XDG_RUNTIME_DIR", nook.where().c_str()};

    const stood::path mounted = nook.where() / "gvfs" / "smb-share:server=nas,share=filmy,user=guest";

    const std::optional<fs::Share> deep = fs::shared(mounted / "HD" / "film.mkv");

    ought(deep.has_value());

    if (deep) {
        same(deep->server, std::string{"nas"});
        same(deep->name, std::string{"filmy"});
        same(deep->user, std::string{"guest"});
        same(deep->rest, std::string{"HD/film.mkv"});
        same(fs::written(*deep), std::string{"smb://guest@nas/filmy/HD/film.mkv"});
    }

    const std::optional<fs::Share> top = fs::shared(mounted);

    ought(top.has_value() && top->rest.empty());

    ought(!fs::shared(nook.where() / "gvfs").has_value());
    ought(!fs::shared(nook.where() / "gvfs" / "sftp:host=nas" / "x").has_value());
    ought(!fs::shared(nook.where() / "inny" / "smb-share:server=nas,share=filmy").has_value());
    ought(!fs::shared("/home/guest").has_value());
}

TRIAL("Share: without XDG_RUNTIME_DIR there is no GVfs and gio is not called") {
    const Nook nook;

    // A fake `gio` on PATH leaves a trace; without GVfs there is no reason to call it.
    {
        std::ofstream script{nook.where() / "gio"};

        script << "#!/bin/sh\ntouch \"" << nook.where().string() << "/called\"\n";
    }

    stood::permissions(nook.where() / "gio", stood::perms::owner_all);

    const char* was = std::getenv("PATH");
    const Planted path{"PATH", (nook.where().string() + ":" + (was != nullptr ? was : "/usr/bin:/bin")).c_str()};

    for (const char* value : {"", "pusty"}) {
        const Planted runtime{"XDG_RUNTIME_DIR", "x"};

        // An empty value also means absent. `Planted` removes an empty value, so we set it directly.
        if (*value == '\0') ::setenv("XDG_RUNTIME_DIR", "", 1);
        else ::unsetenv("XDG_RUNTIME_DIR");

        same(fs::reach(fs::Share{"nas", "filmy", "", ""}).get().refused, fs::Refused::unsupported);
        same(fs::connect(fs::Share{"nas", "filmy", "", ""}, "guest", "tajne", false).get().refused,
             fs::Refused::unsupported);
    }

    ought(!stood::exists(nook.where() / "called"));
    ought(!fs::keeps_logins());
}

#else

TRIAL("Share: Windows remembers logins") { ought(fs::keeps_logins()); }

// On Windows a share has a UNC path: shared reads it like an address, and a path with a drive
// letter is not a share.
TRIAL("Share: shared recognizes a UNC path") {
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

// --- Changing only the case of letters -----------------------------------------------------------

// Paths are made from text through `from_string`: a `path` built from `const char*` reads the bytes
// in the ANSI code page (MSVC, libc++ under MinGW), so "żółw" in UTF-8 would be something else.
TRIAL("Share: changing only the case is the same name in the same directory") {
    const auto recased = [](std::string_view from, std::string_view to) {
        return fs::core::recased(fs::from_string(from), fs::from_string(to));
    };

    ought(recased("kat/heat.mkv", "kat/Heat.mkv"));
    ought(recased("kat/żółw", "kat/ŻÓŁW"));
    ought(recased("kat/Ёлка", "kat/ёЛКА"));
    ought(!recased("kat/heat.mkv", "kat/heat.mkv"));
    ought(!recased("kat/heat.mkv", "kat/Heat.avi"));
    ought(!recased("kat/heat.mkv", "kat/Heat.mkv2"));
    ought(!recased("kat/heat.mkv2", "kat/Heat.mkv"));
    ought(!recased("kat/heat.mkv", "inny/Heat.mkv"));
}

// A server that accepts a case change without complaint and does nothing: the first look at the
// listing says the new name is not there, so two steps go through a name beside it.
TRIAL("Share: an ignored case change takes two steps") {
    std::vector<std::pair<std::string, std::string>> moves;

    const auto move = [&moves](const fs::path& one, const fs::path& two) {
        moves.emplace_back(one.filename().string(), two.filename().string());

        return true;
    };

    ought(fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv", move, [](const fs::path&) { return false; }));

    same(moves, (std::vector<std::pair<std::string, std::string>>{{"heat.mkv", "Heat.mkv.~case"},
                                                                    {"Heat.mkv.~case", "Heat.mkv"}}));

    // A server that did what it should: no steps.
    moves.clear();

    ought(fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv", move, [](const fs::path&) { return true; }));
    ought(moves.empty());
}

TRIAL("Share: a failed second step undoes the first") {
    std::vector<std::pair<std::string, std::string>> moves;

    const auto move = [&moves](const fs::path& one, const fs::path& two) {
        moves.emplace_back(one.filename().string(), two.filename().string());

        return moves.size() != 2;
    };

    ought(!fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv", move, [](const fs::path&) { return false; }));

    same(moves, (std::vector<std::pair<std::string, std::string>>{{"heat.mkv", "Heat.mkv.~case"},
                                                                    {"Heat.mkv.~case", "Heat.mkv"},
                                                                    {"Heat.mkv.~case", "heat.mkv"}}));

    // A failed first step: nothing more.
    moves.clear();

    ought(!fs::core::respelled_twice("kat/heat.mkv", "kat/Heat.mkv",
                                     [&moves](const fs::path& one, const fs::path& two) {
                                         moves.emplace_back(one.filename().string(), two.filename().string());
                                         return false;
                                     },
                                     [](const fs::path&) { return false; }));
    same(moves.size(), std::size_t{1});
}

TRIAL("Share: a case change on disk gives the name in the listing") {
    const Nook nook;

    {
        std::ofstream{nook.where() / "heat.txt"} << "x";
    }

    fs::rename(nook.where() / "heat.txt", nook.where() / "Heat.txt").get();

    std::vector<std::string> named;
    std::error_code ignored;

    for (const auto& one : stood::directory_iterator{nook.where(), ignored}) named.push_back(one.path().filename().string());

    same(named, std::vector<std::string>{"Heat.txt"});
}

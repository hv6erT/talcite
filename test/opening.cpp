#include "proving.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "desktops.hpp"
#include "faking.hpp"
#include "talcite/desktop.hpp"
#include "talcite/loop.hpp"

using namespace talcite;

// Opening files: what the desktop reads from text and bytes (`desktops`), tried on written
// fixtures, and the system desktop on the paths that start nothing. On Linux the system desktop
// reads a whole XDG tree made for the trial, behind a fake console without a session, so it lists
// programs and decides without a window ever opening. On Windows it reads the real cmd.exe and
// notepad.exe and a zone written next to a file.

namespace {

namespace stood = std::filesystem;

// The bytes of the text, as file heads are given.
std::vector<unsigned char> bytes_of(std::string_view text) { return {text.begin(), text.end()}; }

// The first bytes of a PE executable of the given subsystem: the MZ header pointing to the PE
// signature at 0x80, a file header, and an optional header of the given magic.
std::vector<unsigned char> pe(std::uint16_t subsystem, std::uint16_t magic = 0x10B) {
    std::vector<unsigned char> made(0x200, 0);

    made[0] = 'M';
    made[1] = 'Z';
    made[0x3C] = 0x80;
    made[0x80] = 'P';
    made[0x81] = 'E';

    const std::size_t optional = 0x80 + 24;

    made[optional] = static_cast<unsigned char>(magic & 0xFF);
    made[optional + 1] = static_cast<unsigned char>(magic >> 8);
    made[optional + 68] = static_cast<unsigned char>(subsystem & 0xFF);
    made[optional + 69] = static_cast<unsigned char>(subsystem >> 8);

    return made;
}

// The command line of the entry with the given Exec for /tmp/a b.txt, or nothing.
std::optional<std::vector<std::string>> line_of(std::string exec, std::string icon = {}) {
    desktops::Entry one;

    one.name = "Edytor";
    one.exec = std::move(exec);
    one.icon = std::move(icon);

    return desktops::expanded(one, "/tmp/a b.txt", "/usr/share/applications/edytor.desktop");
}

// The value the promise ends with, turning the loop until it does, for at most five seconds; an
// error comes back as the exception it is.
template <typename T>
T answered(Loop& loop, Promise<T> asked) {
    Promise<Outcome<T>> waiting = std::move(asked).outcome();

    for (int at = 0; at < 500 && !waiting.ready(); ++at) {
        (void)loop.turn();
        (void)loop.bell().wait(std::chrono::milliseconds{10});
    }

    if (!waiting.ready()) throw proving::Failed{"no answer in five seconds"};

    return std::move(waiting).get().value();
}

}

TRIAL("desktops: globs2 lines give weight, type, pattern and case") {
    const auto made = desktops::globs("# a comment\n50:text/plain:*.txt\n80:text/x-readme:README:cs\nzle\n:text/x:*.x\n10:image/png:*.png\n");

    same(made.size(), std::size_t{3});
    same(made[0].weight, 50);
    same(made[0].type, std::string{"text/plain"});
    same(made[0].pattern, std::string{"*.txt"});
    ought(!made[0].cased);
    same(made[1].pattern, std::string{"README"});
    ought(made[1].cased);
    same(made[2].type, std::string{"image/png"});
}

TRIAL("desktops: the type goes by weight, then by the longest pattern, case aside") {
    const auto known = desktops::globs("50:text/plain:*.txt\n50:application/gzip:*.gz\n50:application/x-compressed-tar:*.tar.gz\n"
                                       "60:text/x-nfo:*.nfo\n40:text/x-other:*.nfo\n50:text/x-readme:README:cs\n50:text/x-c:*.[ch]\n"
                                       "50:text/x-gone:__NOGLOBS__\n50:text/x-letter:*.[!0-9]x\n");

    same(desktops::type_of("film.NFO", known), std::optional<std::string>{"text/x-nfo"});
    same(desktops::type_of("kopia.tar.gz", known), std::optional<std::string>{"application/x-compressed-tar"});
    same(desktops::type_of("kopia.gz", known), std::optional<std::string>{"application/gzip"});
    same(desktops::type_of("README", known), std::optional<std::string>{"text/x-readme"});
    same(desktops::type_of("readme", known), std::optional<std::string>{});
    same(desktops::type_of("main.c", known), std::optional<std::string>{"text/x-c"});
    same(desktops::type_of("main.h", known), std::optional<std::string>{"text/x-c"});
    same(desktops::type_of("main.cc", known), std::optional<std::string>{});
    same(desktops::type_of("__NOGLOBS__", known), std::optional<std::string>{});
    same(desktops::type_of("plik.bx", known), std::optional<std::string>{"text/x-letter"});
    same(desktops::type_of("plik.1x", known), std::optional<std::string>{});
}

TRIAL("desktops: subclasses, mimeapps.list and mimeinfo.cache") {
    const auto up = desktops::parents("text/x-nfo text/plain\ntext/x-c text/plain\ntext/x-c text/x-source\n");

    same(up.at("text/x-c").size(), std::size_t{2});
    same(up.at("text/x-nfo").front(), std::string{"text/plain"});

    const auto listed = desktops::associations(
        "[Default Applications]\ntext/plain=edytor.desktop;vim.desktop;\n\n[Added Associations]\ntext/plain = kate.desktop;\n"
        "[Removed Associations]\ntext/plain=gedit.desktop\n[Inna grupa]\ntext/plain=nic.desktop\n");

    same(listed.defaults.at("text/plain"), (std::vector<std::string>{"edytor.desktop", "vim.desktop"}));
    same(listed.added.at("text/plain"), (std::vector<std::string>{"kate.desktop"}));
    same(listed.removed.at("text/plain"), (std::vector<std::string>{"gedit.desktop"}));

    const auto cache = desktops::cached("[MIME Cache]\nimage/png=gimp.desktop;eog.desktop;\n");

    same(cache.at("image/png").size(), std::size_t{2});
}

TRIAL("desktops: a desktop entry in the locale's language, with its keys") {
    const std::string text = "[Desktop Entry]\nType=Application\nName=Text Editor\nName[pl]=Edytor\nName[pl_PL]=Edytor tekstu\n"
                             "Exec=edytor %F\nTryExec=edytor\nIcon=edytor\nMimeType=text/plain;text/x-c;\nTerminal=false\n"
                             "NoDisplay=true\n\n[Desktop Action nowe]\nName=Nowe okno\nExec=edytor --new\n";

    const auto polish = desktops::entry(text, "pl_PL.UTF-8");

    ought(polish.has_value());
    same(polish->name, std::string{"Edytor tekstu"});
    same(polish->exec, std::string{"edytor %F"});
    same(polish->try_exec, std::string{"edytor"});
    same(polish->types, (std::vector<std::string>{"text/plain", "text/x-c"}));
    ought(polish->application);
    ought(!polish->shown);
    ought(!polish->hidden);
    ought(!polish->terminal);

    same(desktops::entry(text, "pl_CH")->name, std::string{"Edytor"});
    same(desktops::entry(text, "C.UTF-8")->name, std::string{"Text Editor"});
    same(desktops::entry(text, "")->name, std::string{"Text Editor"});

    const auto other = desktops::entry("[Desktop Entry]\nType=Link\nName=A\\sB\nHidden=true\nTerminal=true\n", "pl");

    ought(!other->application);
    ought(other->hidden);
    ought(other->terminal);
    same(other->name, std::string{"A B"});

    ought(!desktops::entry("[Desktop Action x]\nName=A\n", "pl").has_value());

    const auto reversed = desktops::entry("[Desktop Entry]\nName[pl_PL]=Najlepsza\nName[pl]=Gorsza\nName=Ogólna\n", "pl_PL");

    same(reversed->name, std::string{"Najlepsza"});
}

TRIAL("desktops: the Exec line takes its field codes and quotes") {
    using Line = std::optional<std::vector<std::string>>;

    same(line_of("edytor %f"), (Line{{"edytor", "/tmp/a b.txt"}}));
    same(line_of("edytor %U --wait"), (Line{{"edytor", "/tmp/a b.txt", "--wait"}}));
    same(line_of("edytor --file=%f"), (Line{{"edytor", "--file=/tmp/a b.txt"}}));
    same(line_of("edytor %i %c %k 100%%", "ikona"),
         (Line{{"edytor", "--icon", "ikona", "Edytor", "/usr/share/applications/edytor.desktop", "100%", "/tmp/a b.txt"}}));
    same(line_of("edytor %i %d %m %u"), (Line{{"edytor", "/tmp/a b.txt"}}));
    same(line_of("\"/opt/mój edytor/run\" \"a \\\"b\\\" \\$c \\\\d\" %f"), (Line{{"/opt/mój edytor/run", "a \"b\" $c \\d", "/tmp/a b.txt"}}));
    same(line_of("edytor"), (Line{{"edytor", "/tmp/a b.txt"}}));
    same(line_of("\"edytor"), Line{});
    same(line_of("  "), Line{});

    desktops::Entry alone;

    alone.exec = "program %f --file=%f --x";

    same(desktops::expanded(alone, "", "/x.desktop"), (Line{{"program", "--x"}}));
}

TRIAL("desktops: the XDG places, most important first, desktops before the common list") {
    desktops::Environment given;

    given.home = "/home/h";
    given.current = "KDE:Plasma";

    const auto plain = desktops::places(given);

    same(plain.lists.front(), stood::path{"/home/h/.config/kde-mimeapps.list"});
    same(plain.lists[1], stood::path{"/home/h/.config/plasma-mimeapps.list"});
    same(plain.lists[2], stood::path{"/home/h/.config/mimeapps.list"});
    same(plain.lists[5], stood::path{"/etc/xdg/mimeapps.list"});
    same(plain.lists.back(), stood::path{"/usr/share/applications/mimeapps.list"});
    same(plain.lists.size(), std::size_t{15});
    same(plain.applications, (std::vector<stood::path>{"/home/h/.local/share/applications", "/usr/local/share/applications", "/usr/share/applications"}));
    same(plain.mime.front(), stood::path{"/home/h/.local/share/mime"});

    given.config_home = "/k";
    given.data_dirs = "/d1:wzgledny:/d2";
    given.data_home = "wzgledny";
    given.current.reset();

    const auto set = desktops::places(given);

    same(set.lists.front(), stood::path{"/k/mimeapps.list"});
    same(set.applications, (std::vector<stood::path>{"/home/h/.local/share/applications", "/d1/applications", "/d2/applications"}));
}

TRIAL("desktops: the subsystem of a Windows executable decides how it runs") {
    same(desktops::subsystem(pe(2)), std::optional<std::uint16_t>{2});
    same(desktops::subsystem(pe(3, 0x20B)), std::optional<std::uint16_t>{3});
    same(desktops::subsystem(pe(3, 0x107)), std::optional<std::uint16_t>{});

    std::vector<unsigned char> cut = pe(3);

    cut.resize(0x80 + 24 + 40);

    same(desktops::subsystem(cut), std::optional<std::uint16_t>{});
    same(desktops::subsystem(bytes_of("MZ")), std::optional<std::uint16_t>{});
    same(desktops::subsystem(bytes_of(std::string(0x100, 'x'))), std::optional<std::uint16_t>{});

    std::vector<unsigned char> distant = pe(2);

    distant[0x3C] = 0xFF;
    distant[0x3D] = 0xFF;

    same(desktops::subsystem(distant), std::optional<std::uint16_t>{});

    ought(desktops::windows_runnable("exe", pe(2)) == Runnable::window);
    ought(desktops::windows_runnable("EXE", pe(3)) == Runnable::console);
    ought(desktops::windows_runnable("exe", {}) == Runnable::window);
    ought(desktops::windows_runnable("com", {}) == Runnable::console);
    ought(desktops::windows_runnable("Bat", {}) == Runnable::script);
    ought(desktops::windows_runnable("cmd", {}) == Runnable::script);
    ought(desktops::windows_runnable("ps1", {}) == Runnable::script);
    ought(desktops::windows_runnable("txt", pe(3)) == Runnable::no);
    ought(desktops::windows_runnable("", {}) == Runnable::no);
}

TRIAL("desktops: on POSIX only the execute bit with a program's bytes runs") {
    const std::vector<unsigned char> elf{0x7F, 'E', 'L', 'F', 2, 1};
    const std::vector<unsigned char> macho{0xCF, 0xFA, 0xED, 0xFE};

    ought(desktops::posix_runnable(elf, true) == Runnable::console);
    ought(desktops::posix_runnable(elf, false) == Runnable::no);
    ought(desktops::posix_runnable(macho, true) == Runnable::console);
    ought(desktops::posix_runnable(bytes_of("#!/bin/sh\necho\n"), true) == Runnable::script);
    ought(desktops::posix_runnable(bytes_of("#!/bin/sh\n"), false) == Runnable::no);
    ought(desktops::posix_runnable(bytes_of("zwykly tekst"), true) == Runnable::no);
    ought(desktops::posix_runnable({}, true) == Runnable::no);
}

TRIAL("desktops: the zone of a download, and text told from bytes") {
    ought(desktops::zone_of("[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://example.org/\r\n") == Origin::internet);
    ought(desktops::zone_of("[ZoneTransfer]\nzoneid = 4\n") == Origin::internet);
    ought(desktops::zone_of("[ZoneTransfer]\nZoneId=2\n") == Origin::here);
    ought(desktops::zone_of("[Inna]\nZoneId=3\n") == Origin::here);
    ought(desktops::zone_of("[ZoneTransfer]\nZoneId=3x\n") == Origin::here);
    ought(desktops::zone_of("") == Origin::here);

    ought(desktops::textual(bytes_of("zażółć gęślą jaźń\n")));
    ought(desktops::textual({}));
    ought(desktops::textual(bytes_of("koniec w pół znaku \xC5")));
    ought(!desktops::textual(bytes_of(std::string{"a\0b", 3})));
    ought(!desktops::textual(bytes_of("\xC0\xAF")));
    ought(!desktops::textual(bytes_of("\xC5" "a")));
}

TRIAL("Desktop: the system desktop refuses its window where it has none") {
#ifndef _WIN32
    Loop loop;

    const auto line = std::make_shared<faking::Terminal>();
    const Seam<Desktop> system = desktop(Seam<Console>{line});

    bool refused = false;

    try {
        (void)answered(loop, system.provider().chosen("/tmp/plik.txt"));
    } catch (const Trouble&) {
        refused = true;
    }

    ought(refused);
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)

namespace {

// A whole XDG tree for a trial, in the temporary directory, removed afterwards: configuration,
// data with MIME and applications folders, and a folder of files to open.
class Tree {
public:
    // Creates the tree talcite-desktop-<pid> with the given desktop entries.
    Tree() {
        where_ = stood::temp_directory_path() / ("talcite-desktop-" + std::to_string(::getpid()));

        std::error_code ignored;

        stood::remove_all(where_, ignored);
        stood::create_directories(where_ / "config", ignored);
        stood::create_directories(where_ / "share" / "mime", ignored);
        stood::create_directories(where_ / "share" / "applications" / "kde4", ignored);
        stood::create_directories(where_ / "home" / ".local" / "share" / "applications", ignored);
        stood::create_directories(where_ / "files", ignored);

        put("share/mime/globs2", "50:text/x-nfo:*.nfo\n50:text/plain:*.txt\n50:image/png:*.png\n50:text/x-log:*.log\n");
        put("share/mime/subclasses", "text/x-nfo text/plain\n");
        put("share/applications/edytor.desktop", "[Desktop Entry]\nType=Application\nName=Edytor\nExec=edytor %f\nMimeType=text/plain;\n");
        put("share/applications/vim.desktop", "[Desktop Entry]\nType=Application\nName=Vim\nExec=vim %F\nTerminal=true\nMimeType=text/plain;\n");
        put("share/applications/ukryty.desktop", "[Desktop Entry]\nType=Application\nName=Ukryty\nExec=ukryty %f\nNoDisplay=true\nMimeType=text/plain;\n");
        put("share/applications/brak.desktop", "[Desktop Entry]\nType=Application\nName=Brak\nExec=brak %f\nTryExec=/nie/ma/takiego\nMimeType=text/plain;\n");
        put("share/applications/kde4/kate.desktop", "[Desktop Entry]\nType=Application\nName=Kate\nExec=kate %U\nMimeType=text/x-nfo;\n");
        put("share/applications/usuniety.desktop", "[Desktop Entry]\nType=Application\nName=Usunięty\nExec=usuniety %f\nMimeType=text/plain;\n");
        put("share/applications/mimeinfo.cache",
            "[MIME Cache]\ntext/plain=vim.desktop;ukryty.desktop;usuniety.desktop;brak.desktop;edytor.desktop;\ntext/x-nfo=kde4-kate.desktop;\n");
        put("home/.local/share/applications/wlasny.desktop", "[Desktop Entry]\nType=Application\nName=Własny\nExec=wlasny\nMimeType=text/plain;\n");
        put("config/mimeapps.list", "[Default Applications]\ntext/plain=nie-ma.desktop;edytor.desktop\n[Removed Associations]\ntext/plain=usuniety.desktop;\n");
        put("files/film.nfo", "opis filmu\n");
        put("files/obraz.bin", std::string{"\x89PNG\0\0", 6});
        put("files/skrypt", "#!/bin/sh\necho\n");
        put("files/skrypt-bez-x", "#!/bin/sh\necho\n");
        put("files/start.desktop", "[Desktop Entry]\nType=Application\nName=Start\nExec=start-programu\n");
        put("files/obcy.desktop", "[Desktop Entry]\nType=Application\nName=Obcy\nExec=obcy\n");

        stood::permissions(where_ / "files" / "skrypt", stood::perms::owner_all, ignored);
        stood::permissions(where_ / "files" / "start.desktop", stood::perms::owner_all, ignored);
    }

    // Removes the tree.
    ~Tree() {
        std::error_code ignored;

        stood::remove_all(where_, ignored);
    }

    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;

    // Writes the file at the path within the tree.
    void put(const std::string& at, const std::string& text) const {
        std::ofstream into{where_ / at, std::ios::binary};

        into << text;
    }

    // A path within the tree.
    [[nodiscard]] stood::path at(const std::string& inside) const { return where_ / inside; }

    // A console of this tree, without a graphical session unless `session`.
    [[nodiscard]] std::shared_ptr<faking::Terminal> console(bool session = false) const {
        auto made = std::make_shared<faking::Terminal>();

        made->environment["HOME"] = (where_ / "home").string();
        made->environment["XDG_CONFIG_HOME"] = (where_ / "config").string();
        made->environment["XDG_CONFIG_DIRS"] = (where_ / "nie-ma").string();
        made->environment["XDG_DATA_DIRS"] = (where_ / "share").string();
        made->environment["LANG"] = "pl_PL.UTF-8";
        made->environment["PATH"] = "/usr/bin:/bin";

        if (session) made->environment["DISPLAY"] = ":0";

        return made;
    }

private:
    stood::path where_;
};

// The ids of the applications, in order.
std::vector<std::string> ids_of(const std::vector<Application>& made) {
    std::vector<std::string> ids;

    for (const Application& one : made) ids.push_back(one.id);

    return ids;
}

}

TRIAL("Desktop: the programs of a type, default first, from the lists, caches and parent types") {
    Loop loop;
    const Tree tree;

    const Seam<Desktop> system = desktop(Seam<Console>{tree.console()});

    const std::vector<Application> text = answered(loop, system.provider().applications(tree.at("files/notatki.txt")));

    same(ids_of(text), (std::vector<std::string>{"edytor.desktop", "wlasny.desktop", "vim.desktop"}));
    ought(text[0].preferred);
    ought(!text[1].preferred);
    ought(text[2].terminal);
    same(text[1].name, std::string{"Własny"});

    const std::vector<Application> nfo = answered(loop, system.provider().applications(tree.at("files/film.nfo")));

    same(ids_of(nfo), (std::vector<std::string>{"kde4-kate.desktop", "edytor.desktop", "wlasny.desktop", "vim.desktop"}));
    same(nfo[0].name, std::string{"Kate"});
    ought(nfo[0].preferred);

    same(answered(loop, system.provider().applications(tree.at("files/obraz.bin"))).size(), std::size_t{0});

    const std::vector<Application> log = answered(loop, system.provider().applications(tree.at("files/dziennik.log")));

    same(ids_of(log), ids_of(text));
}

TRIAL("Desktop: opening without a session, a terminal program, a type nobody opens") {
    Loop loop;
    const Tree tree;

    const Seam<Desktop> system = desktop(Seam<Console>{tree.console()});

    ought(answered(loop, system.provider().open(tree.at("files/film.nfo"))) == Started::sessionless);
    ought(answered(loop, system.provider().open(tree.at("files/obraz.bin"))) == Started::unassociated);
    ought(answered(loop, system.provider().open_by(tree.at("files/film.nfo"), "edytor")) == Started::sessionless);

    const Application vim{"Vim", "vim.desktop", false, true};

    ought(answered(loop, system.provider().open_with(tree.at("files/film.nfo"), vim)) == Started::terminal);

    const std::optional<Command> made = answered(loop, system.provider().command(tree.at("files/film.nfo"), vim));

    ought(made.has_value());
    same(made->program, stood::path{"vim"});
    same(made->arguments, (std::vector<std::string>{tree.at("files/film.nfo").string()}));
    same(made->where, tree.at("files"));

    tree.put("config/mimeapps.list", "[Default Applications]\ntext/plain=vim.desktop\n");

    ought(answered(loop, system.provider().open(tree.at("files/notatki.txt"))) == Started::terminal);
    ought(answered(loop, system.provider().open(tree.at("files/film.nfo"))) == Started::sessionless);

    bool gone = false;

    try {
        (void)answered(loop, system.provider().open_with(tree.at("files/film.nfo"), Application{"Brak", "brak.desktop", false, false}));
    } catch (const Trouble&) {
        gone = true;
    }

    ought(gone);
}

TRIAL("Desktop: what runs on Linux, and a launcher with the execute bit") {
    Loop loop;
    const Tree tree;

    const Seam<Desktop> system = desktop(Seam<Console>{tree.console()});

    ought(answered(loop, system.provider().runnable(tree.at("files/skrypt"))) == Runnable::script);
    ought(answered(loop, system.provider().runnable(tree.at("files/skrypt-bez-x"))) == Runnable::no);
    ought(answered(loop, system.provider().runnable(tree.at("files/film.nfo"))) == Runnable::no);
    ought(answered(loop, system.provider().runnable(tree.at("files"))) == Runnable::no);
    ought(answered(loop, system.provider().runnable("/bin/sh")) == Runnable::console);
    ought(answered(loop, system.provider().runnable(tree.at("files/start.desktop"))) == Runnable::window);
    ought(answered(loop, system.provider().runnable(tree.at("files/obcy.desktop"))) == Runnable::no);
    ought(answered(loop, system.provider().origin(tree.at("files/skrypt"))) == Origin::here);

    ought(answered(loop, system.provider().open(tree.at("files/start.desktop"))) == Started::sessionless);

    const std::string launcher = tree.at("files/start.desktop").string();
    const std::optional<Command> made = answered(loop, system.provider().command(tree.at("files/start.desktop"), Application{"Start", launcher, false, false}));

    ought(made.has_value());
    same(made->program, stood::path{"start-programu"});
    ought(made->arguments.empty());
}

#endif

#ifdef _WIN32

TRIAL("Desktop: cmd.exe is a console program, notepad.exe one with windows, a download is from the internet") {
    Loop loop;

    const Seam<Desktop> system = desktop(Seam<Console>{std::make_shared<faking::Terminal>()});

    wchar_t folder[MAX_PATH] = {};

    (void)GetSystemDirectoryW(folder, MAX_PATH);

    const stood::path windows{folder};

    ought(answered(loop, system.provider().runnable(windows / "cmd.exe")) == Runnable::console);
    ought(answered(loop, system.provider().runnable(windows / "notepad.exe")) == Runnable::window);

    const stood::path file = stood::temp_directory_path() / "talcite-strefa.bat";

    {
        std::ofstream into{file, std::ios::binary};

        into << "echo\r\n";
    }

    ought(answered(loop, system.provider().runnable(file)) == Runnable::script);
    ought(answered(loop, system.provider().origin(file)) == Origin::here);

    {
        std::ofstream into{stood::path{file.wstring() + L":Zone.Identifier"}, std::ios::binary};

        into << "[ZoneTransfer]\r\nZoneId=3\r\n";
    }

    ought(answered(loop, system.provider().origin(file)) == Origin::internet);

    std::error_code ignored;

    stood::remove(file, ignored);
}

#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/console.hpp"
#include "tal/desktop.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "desktops.hpp"
#include "tal/testing.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

// The desktop of the system: what it reads from text and bytes (`desktops`), tried on written
// text; on Linux the whole desktop over an XDG tree on a disk in memory, a scripted console and
// scripted programs, so that it lists programs, decides and starts them without a window ever
// opening; on Windows the real cmd.exe and notepad.exe and a zone written next to a file; and the
// scripted desktop that the trials of programs stand on.

namespace {

using tal::Application;
using tal::Command;
using tal::Mode;
using tal::Opened;
using tal::Origin;
using tal::Path;
using tal::Started;
using tal::desktops::Runnable;

// The first bytes of a PE executable of the given subsystem: the MZ header pointing to the PE
// signature at 0x80, a file header, and an optional header of the given magic.
tal::Bytes pe(std::uint16_t subsystem, std::uint16_t magic = 0x10B) {
    tal::Bytes made(0x200, std::byte{0});

    made[0] = std::byte{'M'};
    made[1] = std::byte{'Z'};
    made[0x3C] = std::byte{0x80};
    made[0x80] = std::byte{'P'};
    made[0x81] = std::byte{'E'};

    const std::size_t optional = 0x80 + 24;

    made[optional] = static_cast<std::byte>(magic & 0xFFU);
    made[optional + 1] = static_cast<std::byte>(magic >> 8U);
    made[optional + 68] = static_cast<std::byte>(subsystem & 0xFFU);
    made[optional + 69] = static_cast<std::byte>(subsystem >> 8U);

    return made;
}

// The command line of the entry with the given Exec for /tmp/a b.txt, or nothing.
std::optional<std::vector<std::string>> line_of(std::string exec, std::string icon = {}) {
    tal::desktops::Entry one;

    one.name = "Edytor";
    one.exec = std::move(exec);
    one.icon = std::move(icon);

    return tal::desktops::expanded(one, "/tmp/a b.txt", "/usr/share/applications/edytor.desktop");
}

// The ids of the applications, in order.
std::vector<std::string> ids_of(const std::vector<Application>& made) {
    std::vector<std::string> ids;

    for (const Application& one : made) ids.push_back(one.id);

    return ids;
}

}

TRIAL("desktops: globs2 lines give weight, type, pattern and case") {
    const auto made = tal::desktops::globs("# a comment\n50:text/plain:*.txt\n80:text/x-readme:README:cs\nzle\n:text/x:*.x\n10:image/png:*.png\n");

    SAME(made.size(), std::size_t{3});
    SAME(made[0].weight, 50);
    SAME(made[0].type, std::string{"text/plain"});
    SAME(made[0].pattern, std::string{"*.txt"});
    OUGHT(!made[0].cased);
    SAME(made[1].pattern, std::string{"README"});
    OUGHT(made[1].cased);
    SAME(made[2].type, std::string{"image/png"});
}

TRIAL("desktops: the type goes by weight, then by the longest pattern, case aside") {
    const auto known = tal::desktops::globs("50:text/plain:*.txt\n50:application/gzip:*.gz\n50:application/x-compressed-tar:*.tar.gz\n"
                                            "60:text/x-nfo:*.nfo\n40:text/x-other:*.nfo\n50:text/x-readme:README:cs\n50:text/x-c:*.[ch]\n"
                                            "50:text/x-gone:__NOGLOBS__\n50:text/x-letter:*.[!0-9]x\n50:text/x-first:*.qq\n50:text/x-second:*.q?\n");

    SAME(tal::desktops::type_of("film.NFO", known), std::optional<std::string>{"text/x-nfo"});
    SAME(tal::desktops::type_of("kopia.tar.gz", known), std::optional<std::string>{"application/x-compressed-tar"});
    SAME(tal::desktops::type_of("kopia.gz", known), std::optional<std::string>{"application/gzip"});
    SAME(tal::desktops::type_of("README", known), std::optional<std::string>{"text/x-readme"});
    SAME(tal::desktops::type_of("readme", known), std::optional<std::string>{});
    SAME(tal::desktops::type_of("main.c", known), std::optional<std::string>{"text/x-c"});
    SAME(tal::desktops::type_of("main.h", known), std::optional<std::string>{"text/x-c"});
    SAME(tal::desktops::type_of("main.cc", known), std::optional<std::string>{});
    SAME(tal::desktops::type_of("__NOGLOBS__", known), std::optional<std::string>{});
    SAME(tal::desktops::type_of("plik.bx", known), std::optional<std::string>{"text/x-letter"});
    SAME(tal::desktops::type_of("plik.1x", known), std::optional<std::string>{});
    SAME(tal::desktops::type_of("plik.qq", known), std::optional<std::string>{"text/x-first"});
}

TRIAL("desktops: subclasses, mimeapps.list and mimeinfo.cache") {
    const auto up = tal::desktops::parents("text/x-nfo text/plain\ntext/x-c text/plain\ntext/x-c text/x-source\n");

    SAME(up.at("text/x-c").size(), std::size_t{2});
    SAME(up.at("text/x-nfo").front(), std::string{"text/plain"});

    const auto listed = tal::desktops::associations(
        "[Default Applications]\ntext/plain=edytor.desktop;vim.desktop;\n\n[Added Associations]\ntext/plain = kate.desktop;\n"
        "[Removed Associations]\ntext/plain=gedit.desktop\n[Inna grupa]\ntext/plain=nic.desktop\n");

    SAME(listed.defaults.at("text/plain"), (std::vector<std::string>{"edytor.desktop", "vim.desktop"}));
    SAME(listed.added.at("text/plain"), (std::vector<std::string>{"kate.desktop"}));
    SAME(listed.removed.at("text/plain"), (std::vector<std::string>{"gedit.desktop"}));

    const auto cache = tal::desktops::cached("[MIME Cache]\nimage/png=gimp.desktop;eog.desktop;\n");

    SAME(cache.at("image/png").size(), std::size_t{2});
}

TRIAL("desktops: a desktop entry in the locale's language, with its keys") {
    const std::string text = "[Desktop Entry]\nType=Application\nName=Text Editor\nName[pl]=Edytor\nName[pl_PL]=Edytor tekstu\n"
                             "Exec=edytor %F\nTryExec=edytor\nIcon=edytor\nMimeType=text/plain;text/x-c;\nTerminal=false\n"
                             "NoDisplay=true\n\n[Desktop Action nowe]\nName=Nowe okno\nExec=edytor --new\n";

    const auto polish = tal::desktops::entry(text, "pl_PL.UTF-8");

    OUGHT(polish.has_value());
    SAME(polish->name, std::string{"Edytor tekstu"});
    SAME(polish->exec, std::string{"edytor %F"});
    SAME(polish->try_exec, std::string{"edytor"});
    SAME(polish->types, (std::vector<std::string>{"text/plain", "text/x-c"}));
    OUGHT(polish->application);
    OUGHT(!polish->shown);
    OUGHT(!polish->hidden);
    OUGHT(!polish->terminal);

    SAME(tal::desktops::entry(text, "pl_CH")->name, std::string{"Edytor"});
    SAME(tal::desktops::entry(text, "C.UTF-8")->name, std::string{"Text Editor"});
    SAME(tal::desktops::entry(text, "")->name, std::string{"Text Editor"});

    const auto other = tal::desktops::entry("[Desktop Entry]\nType=Link\nName=A\\sB\nHidden=true\nTerminal=true\n", "pl");

    OUGHT(!other->application);
    OUGHT(other->hidden);
    OUGHT(other->terminal);
    SAME(other->name, std::string{"A B"});

    OUGHT(!tal::desktops::entry("[Desktop Action x]\nName=A\n", "pl").has_value());

    const auto reversed = tal::desktops::entry("[Desktop Entry]\nName[pl_PL]=Najlepsza\nName[pl]=Gorsza\nName=Ogólna\n", "pl_PL");

    SAME(reversed->name, std::string{"Najlepsza"});
}

TRIAL("desktops: the Exec line takes its field codes and quotes") {
    using Line = std::optional<std::vector<std::string>>;

    SAME(line_of("edytor %f"), (Line{{"edytor", "/tmp/a b.txt"}}));
    SAME(line_of("edytor %U --wait"), (Line{{"edytor", "/tmp/a b.txt", "--wait"}}));
    SAME(line_of("edytor --file=%f"), (Line{{"edytor", "--file=/tmp/a b.txt"}}));
    SAME(line_of("edytor %i %c %k 100%%", "ikona"),
         (Line{{"edytor", "--icon", "ikona", "Edytor", "/usr/share/applications/edytor.desktop", "100%", "/tmp/a b.txt"}}));
    SAME(line_of("edytor %i %d %m %u"), (Line{{"edytor", "/tmp/a b.txt"}}));
    SAME(line_of("\"/opt/mój edytor/run\" \"a \\\"b\\\" \\$c \\\\d\" %f"), (Line{{"/opt/mój edytor/run", "a \"b\" $c \\d", "/tmp/a b.txt"}}));
    SAME(line_of("edytor"), (Line{{"edytor", "/tmp/a b.txt"}}));
    SAME(line_of("\"edytor"), Line{});
    SAME(line_of("  "), Line{});

    tal::desktops::Entry alone;

    alone.exec = "program %f --file=%f --x";

    SAME(tal::desktops::expanded(alone, "", "/x.desktop"), (Line{{"program", "--x"}}));
}

TRIAL("desktops: the XDG places, most important first, desktops before the common list") {
    tal::desktops::Environment given;

    given.home = "/home/h";
    given.current = "KDE:Plasma";

    const auto plain = tal::desktops::places(given);

    SAME(plain.lists.front(), Path{"/home/h/.config/kde-mimeapps.list"});
    SAME(plain.lists[1], Path{"/home/h/.config/plasma-mimeapps.list"});
    SAME(plain.lists[2], Path{"/home/h/.config/mimeapps.list"});
    SAME(plain.lists[5], Path{"/etc/xdg/mimeapps.list"});
    SAME(plain.lists.back(), Path{"/usr/share/applications/mimeapps.list"});
    SAME(plain.lists.size(), std::size_t{15});
    SAME(plain.applications, (std::vector<Path>{Path{"/home/h/.local/share/applications"}, Path{"/usr/local/share/applications"}, Path{"/usr/share/applications"}}));
    SAME(plain.mime.front(), Path{"/home/h/.local/share/mime"});

    given.config_home = "/k";
    given.data_dirs = "/d1:wzgledny:/d2";
    given.data_home = "wzgledny";
    given.current.reset();

    const auto set = tal::desktops::places(given);

    SAME(set.lists.front(), Path{"/k/mimeapps.list"});
    SAME(set.applications, (std::vector<Path>{Path{"/home/h/.local/share/applications"}, Path{"/d1/applications"}, Path{"/d2/applications"}}));
}

// A PowerShell script is no program, as Windows opens one in Notepad.
TRIAL("desktops: the subsystem of a Windows executable decides how it runs") {
    SAME(tal::desktops::subsystem(pe(2)), std::optional<std::uint16_t>{2});
    SAME(tal::desktops::subsystem(pe(3, 0x20B)), std::optional<std::uint16_t>{3});
    SAME(tal::desktops::subsystem(pe(3, 0x107)), std::optional<std::uint16_t>{});

    tal::Bytes cut = pe(3);

    cut.resize(0x80 + 24 + 70);

    SAME(tal::desktops::subsystem(cut), std::optional<std::uint16_t>{3});

    cut.resize(0x80 + 24 + 69);

    SAME(tal::desktops::subsystem(cut), std::optional<std::uint16_t>{});
    SAME(tal::desktops::subsystem(tal::bytes("MZ")), std::optional<std::uint16_t>{});
    SAME(tal::desktops::subsystem(tal::bytes(std::string(0x100, 'x'))), std::optional<std::uint16_t>{});

    tal::Bytes distant = pe(2);

    distant[0x3C] = std::byte{0xFF};
    distant[0x3D] = std::byte{0xFF};

    SAME(tal::desktops::subsystem(distant), std::optional<std::uint16_t>{});

    OUGHT(tal::desktops::windows_runnable("exe", pe(2)) == Runnable::window);
    OUGHT(tal::desktops::windows_runnable("EXE", pe(3)) == Runnable::console);
    OUGHT(tal::desktops::windows_runnable("exe", {}) == Runnable::window);
    OUGHT(tal::desktops::windows_runnable("com", {}) == Runnable::console);
    OUGHT(tal::desktops::windows_runnable("Bat", {}) == Runnable::script);
    OUGHT(tal::desktops::windows_runnable("cmd", {}) == Runnable::script);
    OUGHT(tal::desktops::windows_runnable("ps1", {}) == Runnable::no);
    OUGHT(tal::desktops::windows_runnable("txt", pe(3)) == Runnable::no);
    OUGHT(tal::desktops::windows_runnable("", {}) == Runnable::no);
}

TRIAL("desktops: on POSIX only the execute bit with a program's bytes runs") {
    const tal::Bytes elf = tal::bytes("\x7F" "ELF\x02\x01");

    OUGHT(tal::desktops::posix_runnable(elf, true) == Runnable::console);
    OUGHT(tal::desktops::posix_runnable(elf, false) == Runnable::no);
    for (const char* magic : {"\xFE\xED\xFA\xCE", "\xFE\xED\xFA\xCF", "\xCE\xFA\xED\xFE", "\xCF\xFA\xED\xFE", "\xCA\xFE\xBA\xBE"})
        OUGHT(tal::desktops::posix_runnable(tal::bytes(magic), true) == Runnable::console);
    OUGHT(tal::desktops::posix_runnable(tal::bytes("#!/bin/sh\necho\n"), true) == Runnable::script);
    OUGHT(tal::desktops::posix_runnable(tal::bytes("#!/bin/sh\n"), false) == Runnable::no);
    OUGHT(tal::desktops::posix_runnable(tal::bytes("zwykly tekst"), true) == Runnable::no);
    OUGHT(tal::desktops::posix_runnable({}, true) == Runnable::no);
}

TRIAL("desktops: the zone of a download, and text told from bytes") {
    OUGHT(tal::desktops::zone_of("[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://example.org/\r\n") == Origin::internet);
    OUGHT(tal::desktops::zone_of("[ZoneTransfer]\nzoneid = 4\n") == Origin::internet);
    OUGHT(tal::desktops::zone_of("[ZoneTransfer]\nZoneId=2\n") == Origin::here);
    OUGHT(tal::desktops::zone_of("[Inna]\nZoneId=3\n") == Origin::here);
    OUGHT(tal::desktops::zone_of("[ZoneTransfer]\nZoneId=3x\n") == Origin::here);
    OUGHT(tal::desktops::zone_of("") == Origin::here);

    OUGHT(tal::desktops::textual(tal::bytes("zażółć gęślą jaźń\n")));
    OUGHT(tal::desktops::textual({}));
    OUGHT(tal::desktops::textual(tal::bytes("koniec w pół znaku \xC5")));
    OUGHT(!tal::desktops::textual(tal::bytes(std::string{"a\0b", 3})));
    OUGHT(!tal::desktops::textual(tal::bytes("\xC0\xAF")));
    OUGHT(!tal::desktops::textual(tal::bytes("\xC5" "a")));
    OUGHT(!tal::desktops::textual(tal::bytes("\xED\xA0\x80")));
    OUGHT(!tal::desktops::textual(tal::bytes("\xE0\x80\xAF")));
}

// The scripted desktop answers as set and notes what it was asked to open.
TRIAL("ScriptedDesktop: answers as set, requests noted") {
    tal::Loop loop;
    tal::ScriptedDesktop desktop;
    Command run;

    run.program = Path{"/a/run"};
    run.mode = Mode::terminal;
    desktop.programs[Path{"/a/run"}] = run;
    desktop.downloaded.insert(Path{"/a/run"});
    desktop.offered[".txt"] = {Application{"Edytor", "edytor", true, false}};

    OUGHT(tal::wait(loop, desktop.program(Path{"/a/run"})).has_value());
    OUGHT(!tal::wait(loop, desktop.program(Path{"/a/b.txt"})).has_value());
    OUGHT(tal::wait(loop, desktop.origin(Path{"/a/run"})) == Origin::internet);
    OUGHT(tal::wait(loop, desktop.origin(Path{"/a/b.txt"})) == Origin::here);
    SAME(ids_of(tal::wait(loop, desktop.applications(Path{"/a/B.TXT"}))), std::vector<std::string>{"edytor"});
    SAME(ids_of(tal::wait(loop, desktop.applications_for(".TXT"))), std::vector<std::string>{"edytor"});
    OUGHT(tal::wait(loop, desktop.applications(Path{"/a/run"})).empty());

    desktop.answer = Opened{Started::unassociated, {}};

    OUGHT(tal::wait(loop, desktop.open(Path{"/a/b.txt"})).started == Started::unassociated);
    OUGHT(tal::wait(loop, desktop.open_with(Path{"/a/b.txt"}, Application{"Edytor", "edytor", true, false})).started == Started::unassociated);
    OUGHT(tal::wait(loop, desktop.open_by(Path{"/a/b.txt"}, Path{"/bin/ed"})).started == Started::unassociated);

    desktop.refusal = tal::Trouble{tal::Kind::denied, "Nie."};

    bool refused = false;

    try {
        (void)tal::wait(loop, desktop.chosen(Path{"/a/b.txt"}));
    } catch (const tal::Trouble& why) {
        refused = why.kind() == tal::Kind::denied;
    }

    OUGHT(refused);
    SAME(desktop.requests.size(), std::size_t{4});
    OUGHT(desktop.requests[0].way == tal::ScriptedDesktop::Way::open);
    OUGHT(desktop.requests[1].way == tal::ScriptedDesktop::Way::with);
    SAME(desktop.requests[1].with, std::string{"edytor"});
    OUGHT(desktop.requests[2].way == tal::ScriptedDesktop::Way::by);
    SAME(desktop.requests[2].with, std::string{"/bin/ed"});
    OUGHT(desktop.requests[3].way == tal::ScriptedDesktop::Way::chosen);
    SAME(desktop.requests[3].file, Path{"/a/b.txt"});
}

#ifndef _WIN32

namespace {

// A whole XDG tree on a disk in memory, behind a scripted console, starting scripted programs:
// configuration, data with MIME and applications folders, and a folder of files to open. The
// programs the trials start (edytor, kate, start-programu, /bin/ed) end at once.
struct Tree {
    tal::Loop loop;
    std::shared_ptr<tal::fs::Memory> memory = std::make_shared<tal::fs::Memory>();
    std::shared_ptr<tal::ScriptedConsole> console = std::make_shared<tal::ScriptedConsole>();
    std::shared_ptr<tal::Scripted> processes = std::make_shared<tal::Scripted>(loop);
    tal::Port<tal::Desktop> desktop = tal::desktop(tal::Port<tal::Console>{console}, tal::Port<tal::Processes>{processes}, tal::Port<tal::fs::Files>{memory});

    // The tree, without a graphical session unless `session`.
    explicit Tree(bool session = false) {
        for (const char* one : {"/x/config", "/x/share/mime", "/x/share/applications/kde4", "/x/home/.local/share/applications", "/x/files"})
            tal::wait(loop, memory->make(Path{one}));

        put("/x/share/mime/globs2", "50:text/x-nfo:*.nfo\n50:text/plain:*.txt\n50:image/png:*.png\n50:text/x-log:*.log\n50:image/x-raw:*.raw\n50:text/x-readme:README\n");
        put("/x/share/mime/subclasses", "text/x-nfo text/plain\ntext/x-log application/octet-stream\n");
        put("/x/share/applications/edytor.desktop", "[Desktop Entry]\nType=Application\nName=Edytor\nExec=edytor %f\nMimeType=text/plain;\n");
        put("/x/share/applications/vim.desktop", "[Desktop Entry]\nType=Application\nName=Vim\nExec=vim %F\nTerminal=true\nMimeType=text/plain;\n");
        put("/x/share/applications/ukryty.desktop", "[Desktop Entry]\nType=Application\nName=Ukryty\nExec=ukryty %f\nNoDisplay=true\nMimeType=text/plain;\n");
        put("/x/share/applications/brak.desktop", "[Desktop Entry]\nType=Application\nName=Brak\nExec=brak %f\nTryExec=/nie/ma/takiego\nMimeType=text/plain;\n");
        put("/x/share/applications/jest.desktop", "[Desktop Entry]\nType=Application\nName=Jest\nExec=jest %f\nTryExec=jest\nMimeType=image/png;\n");
        put("/x/share/applications/kde4/kate.desktop", "[Desktop Entry]\nType=Application\nName=Kate\nExec=kate %U\nMimeType=text/x-nfo;\n");
        put("/x/share/applications/usuniety.desktop", "[Desktop Entry]\nType=Application\nName=Usunięty\nExec=usuniety %f\nMimeType=text/plain;\n");
        put("/x/share/applications/skasowany.desktop", "[Desktop Entry]\nType=Application\nName=Skasowany\nExec=skasowany %f\nHidden=true\nMimeType=text/plain;\n");
        put("/x/share/applications/odnosnik.desktop", "[Desktop Entry]\nType=Link\nName=Odnośnik\nExec=odnosnik %f\nMimeType=text/plain;\n");
        put("/x/share/applications/hex.desktop", "[Desktop Entry]\nType=Application\nName=Hex\nExec=hex %f\nMimeType=application/octet-stream;\n");
        put("/x/share/applications/mimeinfo.cache",
            "[MIME Cache]\ntext/plain=vim.desktop;ukryty.desktop;usuniety.desktop;skasowany.desktop;odnosnik.desktop;brak.desktop;edytor.desktop;\n"
            "text/x-nfo=kde4-kate.desktop;\nimage/png=jest.desktop;\napplication/octet-stream=hex.desktop;\n");
        put("/x/home/.local/share/applications/wlasny.desktop", "[Desktop Entry]\nType=Application\nName=Własny\nExec=wlasny\nMimeType=text/plain;\n");
        put("/x/home/.local/share/applications/czytaj.txt", "[Desktop Entry]\nType=Application\nName=Czytaj\nExec=czytaj\nMimeType=text/plain;\n");
        put("/x/home/.local/share/applications/stare/nowy.desktop", "[Desktop Entry]\nType=Application\nName=Nowy\nExec=nowy\nMimeType=text/x-log;\n");
        put("/x/config/mimeapps.list", "[Default Applications]\ntext/plain=nie-ma.desktop;edytor.desktop\n[Removed Associations]\ntext/plain=usuniety.desktop;\n");
        put("/x/files/film.nfo", "opis filmu\n");
        put("/x/files/obraz.bin", std::string{"\x89PNG\0\0", 6});
        put("/x/files/skrypt", "#!/bin/sh\necho\n", 0755);
        put("/x/files/skrypt-bez-x", "#!/bin/sh\necho\n");
        put("/x/files/program", "\x7F" "ELF\x02\x01", 0100);
        put("/x/files/grupy", "\x7F" "ELF\x02\x01", 0010);
        put("/x/files/inni", "\x7F" "ELF\x02\x01", 0001);
        put("/x/files/skrypt:Zone.Identifier", "[ZoneTransfer]\nZoneId=3\n");
        put("/x/files/start.desktop", "[Desktop Entry]\nType=Application\nName=Start\nExec=start-programu\n", 0700);
        put("/x/files/okno.desktop", "[Desktop Entry]\nType=Application\nName=Okno\nExec=okno --tak\nTerminal=true\n", 0700);
        put("/x/files/obcy.desktop", "[Desktop Entry]\nType=Application\nName=Obcy\nExec=obcy\n");
        put("/x/files/pusty.desktop", "[Desktop Entry]\nType=Link\nName=Pusty\nExec=pusty\n", 0700);
        put("/x/bin/jest", "#!/bin/sh\n", 0755);
        tal::wait(loop, memory->link(Path{"/x/files/skrot"}, Path{"/x/files/skrypt"}));
        tal::wait(loop, memory->link(Path{"/x/files/start-skrot.desktop"}, Path{"/x/files/start.desktop"}));

        console->environment["HOME"] = "/x/home";
        console->environment["XDG_CONFIG_HOME"] = "/x/config";
        console->environment["XDG_CONFIG_DIRS"] = "/x/nie-ma";
        console->environment["XDG_DATA_DIRS"] = "/x/share";
        console->environment["LANG"] = "pl_PL.UTF-8";
        console->environment["PATH"] = "/usr/bin::/x/bin";

        if (session) console->environment["DISPLAY"] = ":0";

        for (const char* one : {"edytor", "kate", "start-programu", "/bin/ed"})
            processes->add(one, [](tal::Running&) -> tal::Task<int> { co_return 0; });
    }

    // Writes the file at `where` with `text` and `rights`.
    void put(const char* where, std::string_view text, std::uint16_t rights = 0644) {
        tal::wait(loop, memory->make(Path{where}.parent()));
        tal::wait(loop, memory->write(Path{where}, tal::bytes(text), tal::fs::Writing{.rights = rights}));
    }

    // The value of the task, turning the loop until it ends.
    template <typename T>
    T run(tal::Task<T> work) {
        return tal::wait(loop, std::move(work));
    }

    // The kind of trouble the task fails with, or none.
    template <typename T>
    std::optional<tal::Kind> refused(tal::Task<T> work) {
        try {
            (void)tal::wait(loop, std::move(work));
        } catch (const tal::Trouble& why) {
            return why.kind();
        }

        return std::nullopt;
    }
};

}

// The default program comes first and preferred, then the others by type: the lists' defaults,
// the user's applications without a cache (read from their desktop files), the caches; a program
// removed, deleted, hidden from lists or whose TryExec is missing is left out, and so is a file of
// the user's that is no desktop file; a program in a subfolder of the user's has the folder in its
// id; a parent type adds its programs after the type's own, but the type of all bytes adds none; a
// name no glob knows is text by its first bytes, or bytes; a type with no program has none.
TRIAL("Desktop: the programs of a type, default first, from the lists, caches and parent types") {
    Tree tree;

    const std::vector<Application> text = tree.run(tree.desktop->applications(Path{"/x/files/notatki.txt"}));

    SAME(ids_of(text), (std::vector<std::string>{"edytor.desktop", "wlasny.desktop", "vim.desktop"}));
    OUGHT(text[0].preferred);
    OUGHT(!text[1].preferred);
    OUGHT(!text[2].preferred);
    OUGHT(text[2].terminal);
    OUGHT(!text[0].terminal);
    SAME(text[0].name, std::string{"Edytor"});
    SAME(text[1].name, std::string{"Własny"});

    const std::vector<Application> nfo = tree.run(tree.desktop->applications(Path{"/x/files/film.nfo"}));

    SAME(ids_of(nfo), (std::vector<std::string>{"kde4-kate.desktop", "edytor.desktop", "wlasny.desktop", "vim.desktop"}));
    SAME(nfo[0].name, std::string{"Kate"});
    OUGHT(nfo[0].preferred);

    SAME(ids_of(tree.run(tree.desktop->applications(Path{"/x/files/obraz.png"}))), std::vector<std::string>{"jest.desktop"});
    SAME(ids_of(tree.run(tree.desktop->applications(Path{"/x/files/obraz.bin"}))), std::vector<std::string>{"hex.desktop"});
    OUGHT(tree.run(tree.desktop->applications(Path{"/x/files/surowe.raw"})).empty());
    SAME(ids_of(tree.run(tree.desktop->applications(Path{"/x/files/skrypt-bez-x"}))), ids_of(text));
    std::vector<std::string> logs{"stare-nowy.desktop"};

    for (const std::string& id : ids_of(text)) logs.push_back(id);

    SAME(ids_of(tree.run(tree.desktop->applications(Path{"/x/files/dziennik.log"}))), logs);
    OUGHT(tree.run(tree.desktop->applications(Path{"/x/files"})).empty());

    tree.put("/x/bin/jest", "#!/bin/sh\n", 0644);

    OUGHT(tree.run(tree.desktop->applications(Path{"/x/files/obraz.png"})).empty());
}

// The programs of an extension are those of a file of that extension, found by the globs alone,
// in any case of letters; an extension no glob knows, a bare dot and a word without its dot have
// none.
TRIAL("Desktop: the programs of an extension, without a file") {
    Tree tree;

    SAME(ids_of(tree.run(tree.desktop->applications_for(".txt"))), ids_of(tree.run(tree.desktop->applications(Path{"/x/files/notatki.txt"}))));
    SAME(ids_of(tree.run(tree.desktop->applications_for(".NFO"))), (std::vector<std::string>{"kde4-kate.desktop", "edytor.desktop", "wlasny.desktop", "vim.desktop"}));

    for (const std::string none : {".xyz", ".", "txt", "README", ""}) OUGHT(tree.run(tree.desktop->applications_for(none)).empty());
}

// Without a session nothing opens in a window, but a terminal program comes back with its command
// line, and a type nobody opens says so; a program no longer installed is missing.
TRIAL("Desktop: opening without a session, a terminal program, a type nobody opens") {
    Tree tree;

    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/film.nfo"})).started == Started::sessionless);
    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/surowe.raw"})).started == Started::unassociated);
    OUGHT(tree.run(tree.desktop->open_by(Path{"/x/files/film.nfo"}, Path{"/bin/ed"})).started == Started::sessionless);
    OUGHT(tree.run(tree.desktop->open_with(Path{"/x/files/film.nfo"}, Application{"Edytor", "edytor.desktop", false, false})).started == Started::sessionless);

    const Opened vim = tree.run(tree.desktop->open_with(Path{"/x/files/film.nfo"}, Application{"Vim", "vim.desktop", false, true}));

    OUGHT(vim.started == Started::terminal);
    SAME(vim.command.program, Path{"vim"});
    SAME(vim.command.arguments, (std::vector<std::string>{"/x/files/film.nfo"}));
    SAME(vim.command.folder, Path{"/x/files"});
    OUGHT(vim.command.mode == Mode::terminal);

    tree.put("/x/config/mimeapps.list", "[Default Applications]\ntext/plain=vim.desktop\n");

    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/notatki.txt"})).started == Started::terminal);
    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/film.nfo"})).started == Started::sessionless);
    OUGHT(tree.refused(tree.desktop->open_with(Path{"/x/files/film.nfo"}, Application{"Brak", "brak.desktop", false, false})) == tal::Kind::missing);
    OUGHT(tree.refused(tree.desktop->chosen(Path{"/x/files/film.nfo"})) == tal::Kind::unsupported);
    OUGHT(tree.processes->started().empty());
}

// With a session the program is let loose in the file's folder with the file; a program named by
// the user, too; over ssh there is no session.
TRIAL("Desktop: opening with a session lets the program loose") {
    Tree tree{true};

    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/film.nfo"})).started == Started::opened);
    OUGHT(tree.run(tree.desktop->open_with(Path{"/x/files/notatki.txt"}, Application{"Edytor", "edytor.desktop", true, false})).started == Started::opened);
    OUGHT(tree.run(tree.desktop->open_by(Path{"/x/files/film.nfo"}, Path{"/bin/ed"})).started == Started::opened);

    const std::vector<Command> started = tree.processes->started();

    SAME(started.size(), std::size_t{3});
    SAME(started[0].program, Path{"kate"});
    SAME(started[0].arguments, (std::vector<std::string>{"/x/files/film.nfo"}));
    SAME(started[0].folder, Path{"/x/files"});
    OUGHT(started[0].mode == Mode::detached);
    SAME(started[1].program, Path{"edytor"});
    SAME(started[2].program, Path{"/bin/ed"});
    SAME(started[2].arguments, (std::vector<std::string>{"/x/files/film.nfo"}));
    SAME(started[2].folder, Path{"/x/files"});
    OUGHT(started[2].mode == Mode::detached);

    tree.console->environment["SSH_CONNECTION"] = "10.0.0.1 22 10.0.0.2 22";

    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/film.nfo"})).started == Started::sessionless);

    tree.console->environment.erase("SSH_CONNECTION");
    tree.console->environment["SSH_TTY"] = "/dev/pts/0";

    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/film.nfo"})).started == Started::sessionless);

    tree.console->environment.erase("SSH_TTY");
    tree.console->environment.erase("DISPLAY");
    tree.console->environment["WAYLAND_DISPLAY"] = "wayland-0";

    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/film.nfo"})).started == Started::opened);
}

// A program has its execute bit and the bytes of one; it runs on a terminal in its folder. A
// launcher (a .desktop file anyone may run) runs its Exec line, let loose or on a terminal as it
// says. A file is here wherever it came from, a stream of Windows being only a name here.
TRIAL("Desktop: what runs on Linux, and a launcher with the execute bit") {
    Tree tree;

    const std::optional<Command> script = tree.run(tree.desktop->program(Path{"/x/files/skrypt"}));

    OUGHT(script.has_value());
    SAME(script->program, Path{"/x/files/skrypt"});
    SAME(script->folder, Path{"/x/files"});
    OUGHT(script->arguments.empty());
    OUGHT(script->mode == Mode::terminal);

    for (const char* runs : {"/x/files/program", "/x/files/grupy", "/x/files/inni", "/x/files/skrot"}) {
        const std::optional<Command> binary = tree.run(tree.desktop->program(Path{runs}));

        OUGHT(binary.has_value());
        OUGHT(binary->mode == Mode::terminal);
    }

    for (const char* none : {"/x/files/skrypt-bez-x", "/x/files/film.nfo", "/x/files", "/x/files/nie-ma", "/x/files/obcy.desktop", "/x/files/pusty.desktop"})
        OUGHT(!tree.run(tree.desktop->program(Path{none})).has_value());

    const std::optional<Command> start = tree.run(tree.desktop->program(Path{"/x/files/start.desktop"}));

    OUGHT(start.has_value());
    OUGHT(tree.run(tree.desktop->program(Path{"/x/files/start-skrot.desktop"})).has_value());
    SAME(start->program, Path{"start-programu"});
    OUGHT(start->arguments.empty());
    SAME(start->folder, Path{"/x/files"});
    OUGHT(start->mode == Mode::detached);

    const std::optional<Command> window = tree.run(tree.desktop->program(Path{"/x/files/okno.desktop"}));

    OUGHT(window.has_value());
    SAME(window->arguments, std::vector<std::string>{"--tak"});
    OUGHT(window->mode == Mode::terminal);

    OUGHT(tree.run(tree.desktop->origin(Path{"/x/files/skrypt"})) == Origin::here);
    OUGHT(!tree.run(tree.desktop->program(Path{"/x/files/skrypt:Zone.Identifier"})).has_value());
    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/start.desktop"})).started == Started::sessionless);
    OUGHT(tree.run(tree.desktop->open(Path{"/x/files/okno.desktop"})).started == Started::terminal);
    OUGHT(tree.refused(tree.desktop->open(Path{"/x/files/pusty.desktop"})) == tal::Kind::malformed);

    Tree lively{true};

    OUGHT(lively.run(lively.desktop->open(Path{"/x/files/start.desktop"})).started == Started::opened);
    SAME(lively.processes->started().at(0).program, Path{"start-programu"});
    SAME(lively.processes->started().at(0).folder, Path{"/x/files"});
}

#endif

#ifdef _WIN32

// cmd.exe runs on a terminal, notepad.exe on its own, a batch file through cmd.exe /c; a file with
// the mark of a download is from the internet; the programs of an extension are those of a file of
// it, which need not be there. The mark is a stream of the file, which Windows cannot put in place
// by renaming, so it is written in place.
TRIAL("Desktop: cmd.exe is a console program, notepad.exe one with windows, a download is from the internet") {
    tal::Loop loop;
    const tal::Port<tal::Desktop> desktop = tal::desktop(tal::Port<tal::Console>{std::make_shared<tal::ScriptedConsole>()}, tal::Port<tal::Processes>{},
                                                         tal::Port<tal::fs::Files>{});
    wchar_t folder[MAX_PATH] = {};

    (void)GetSystemDirectoryW(folder, MAX_PATH);

    const Path windows{tal::text::narrow(folder)};
    const std::optional<Command> console = tal::wait(loop, desktop->program(windows / "cmd.exe"));
    const std::optional<Command> window = tal::wait(loop, desktop->program(windows / "notepad.exe"));

    OUGHT(console.has_value() && console->mode == Mode::terminal);
    OUGHT(window.has_value() && window->mode == Mode::detached);
    SAME(window->folder, windows);

    const Path temporary = *tal::wait(loop, tal::fs::known(tal::fs::Known::temp));

    SAME(ids_of(tal::wait(loop, desktop->applications_for(".txt"))), ids_of(tal::wait(loop, desktop->applications(temporary / "tal-nie-ma.txt"))));
    OUGHT(tal::wait(loop, desktop->applications_for("txt")).empty());

    const Path file = temporary / "tal-strefa.bat";

    tal::wait(loop, tal::fs::write(file, tal::bytes("echo\r\n")));

    const std::optional<Command> batch = tal::wait(loop, desktop->program(file));

    OUGHT(batch.has_value());
    SAME(batch->program, Path{"cmd.exe"});
    SAME(batch->arguments.size(), std::size_t{2});
    SAME(batch->arguments[0], std::string{"/c"});
    OUGHT(batch->arguments[1].find('\\') != std::string::npos && batch->arguments[1].find('/') == std::string::npos);
    OUGHT(tal::wait(loop, desktop->origin(file)) == Origin::here);

    tal::wait(loop, tal::fs::write(Path{std::string{file.stored()} + ":Zone.Identifier"}, tal::bytes("[ZoneTransfer]\r\nZoneId=3\r\n"),
                                   tal::fs::Writing{.atomic = false}));

    OUGHT(tal::wait(loop, desktop->origin(file)) == Origin::internet);
    OUGHT(!tal::wait(loop, desktop->program(temporary / "tal-nie-ma.ps1")).has_value());

    tal::wait(loop, tal::fs::remove(file));
}

#endif

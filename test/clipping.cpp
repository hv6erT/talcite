#include "tal/testing.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "faking.hpp"
#include "talcite/clipboard.hpp"
#include "talcite/fs.hpp"

using namespace talcite;

namespace {

// A clipboard on a fake terminal reached over ssh. On POSIX there is one way anyway (OSC 52), but
// on Windows only ssh sends the clipboard to the terminal. Thanks to this the same trials run on
// every system, and none reaches the system clipboard, which would overwrite what the user has
// copied.
struct Wired {
    std::shared_ptr<faking::Terminal> line = faking::terminal();
    Seam<Clipboard> kept;

    // Marks the fake terminal as an ssh session and makes a clipboard on it.
    Wired() {
        line->environment["SSH_CONNECTION"] = "10.0.0.1 22 10.0.0.2 22";

        kept = clipboard(Seam<Console>{line});
    }
};

// What copying `what` writes to the terminal.
std::string spouted(const std::string& what) {
    Wired one;

    (void)one.kept.provider().copy(what);

    return one.line->written;
}

}

// --- What the program remembers ------------------------------------------------------------------

TRIAL("Clipboard: what was copied comes back on paste") {
    Wired one;

    OUGHT(one.kept.provider().copy("ala ma kota"));

    const std::optional<std::string> got = one.kept.provider().paste();

    OUGHT(got.has_value());
    SAME(*got, std::string{"ala ma kota"});
}

TRIAL("Clipboard: emptiness is not content") {
    Wired one;

    OUGHT(one.kept.provider().copy(""));
    OUGHT(!one.kept.provider().paste().has_value());
}

// The threshold applies only to the way through the terminal: OSC 52 is sometimes silently cut,
// so it is better to say "did not go" than to paste a stub posing as the whole. Whatever the
// terminal says, our copy stays complete and comes back in full.
TRIAL("Clipboard: content over the threshold stays with us in full") {
    Wired one;

    const std::string big(200 * 1024, 'x');

    OUGHT(!one.kept.provider().copy(big));

    const std::optional<std::string> got = one.kept.provider().paste();

    OUGHT(got.has_value());
    SAME(got->size(), big.size());
}

TRIAL("Clipboard: multibyte characters pass unchanged") {
    Wired one;

    const std::string what = "z\xc4\x85\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87";

    OUGHT(one.kept.provider().copy(what));

    const std::optional<std::string> got = one.kept.provider().paste();

    OUGHT(got.has_value());
    SAME(got.value_or(""), what);
}

// Our own copy belongs to the clipboard, not to the process: two clipboards on two consoles do
// not see each other's copies. Only the default one is shared.
TRIAL("Clipboard: two clipboards do not share a copy") {
    Wired one;
    Wired two;

    OUGHT(one.kept.provider().copy("abc"));
    OUGHT(!two.kept.provider().paste().has_value());
}

TRIAL("Clipboard: there is one default per process") {
    OUGHT(&clipboard().provider() == &clipboard().provider());
}

// --- What goes to the terminal -------------------------------------------------------------------

TRIAL("Clipboard: OSC 52 goes out with the content in base64") {
    SAME(spouted("abc"), std::string{"\x1b]52;c;YWJj\x07"});
}

// Padding counts to the character: a terminal that gets base64 without padding may reject the
// whole thing.
TRIAL("Clipboard: base64 pads for all three remainders") {
    SAME(spouted("a"), std::string{"\x1b]52;c;YQ==\x07"});
    SAME(spouted("ab"), std::string{"\x1b]52;c;YWI=\x07"});
    SAME(spouted("abc"), std::string{"\x1b]52;c;YWJj\x07"});
    SAME(spouted("abcd"), std::string{"\x1b]52;c;YWJjZA==\x07"});
}

TRIAL("Clipboard: base64 does not lose non-ASCII bytes") {
    SAME(spouted("\xc4\x85"), std::string{"\x1b]52;c;xIU=\x07"});
}

TRIAL("Clipboard: over the threshold nothing goes out") {
    SAME(spouted(std::string(200 * 1024, 'x')), std::string{});
}

// The threshold is inclusive: exactly that much still goes out.
TRIAL("Clipboard: content equal to the threshold still goes out") {
    Wired one;

    OUGHT(one.kept.provider().copy(std::string(64 * 1024, 'x')));
    OUGHT(!one.line->written.empty());
}

// --- Files --------------------------------------------------------------------------------------

namespace {

// An absolute path under the temporary directory, absolute on every system, which a list of files
// on the clipboard requires.
std::filesystem::path rooted(const std::string& name) { return std::filesystem::temp_directory_path() / name; }

// Two files, cut or copied.
Files two(Handing how) {
    Files made;

    made.paths = {rooted("a.mkv"), rooted("b")};
    made.how = how;

    return made;
}

}

TRIAL("Clipboard: a clipboard that knows only text takes no files") {
    faking::Clipboard one;

    OUGHT(!one.give(two(Handing::copy)));
    OUGHT(!one.taken().has_value());
}

// Over ssh the system clipboard is not the user's, so files stay in our own copy: `give` says they
// did not reach the system, yet a paste in the same program gets them back, cut included, and a
// text paste gets their paths.
TRIAL("Clipboard: over ssh files stay in our own copy, with their paths as text") {
    Wired one;

    OUGHT(!one.kept.provider().give(two(Handing::cut)));

    const std::optional<Files> got = one.kept.provider().taken();

    OUGHT(got.has_value());
    OUGHT(got.has_value() && got->paths == two(Handing::cut).paths);
    OUGHT(got.has_value() && got->how == Handing::cut);

    SAME(one.kept.provider().paste().value_or(""),
         fs::to_string(rooted("a.mkv")) + "\n" + fs::to_string(rooted("b")));
    SAME(one.line->written, std::string{});
}

// One clipboard, one content: copied text replaces the files, and an empty list takes files away
// together with their text, but leaves text that came after them.
TRIAL("Clipboard: copied text replaces the files, and an empty list takes only files away") {
    Wired one;

    OUGHT(!one.kept.provider().give(two(Handing::copy)));
    OUGHT(one.kept.provider().copy("abc"));
    OUGHT(!one.kept.provider().taken().has_value());

    (void)one.kept.provider().give({});

    SAME(one.kept.provider().paste().value_or(""), std::string{"abc"});

    (void)one.kept.provider().give(two(Handing::copy));
    (void)one.kept.provider().give({});

    OUGHT(!one.kept.provider().taken().has_value());
    OUGHT(!one.kept.provider().paste().has_value());
}

// Another process does not share our working directory, so a relative path would point it
// elsewhere: the whole list is refused and what was there stays.
TRIAL("Clipboard: a relative path refuses the whole list and changes nothing") {
    Wired one;

    (void)one.kept.provider().give(two(Handing::cut));

    Files wrong;

    wrong.paths = {rooted("c"), std::filesystem::path{"relative.txt"}};

    OUGHT(!one.kept.provider().give(wrong));

    const std::optional<Files> got = one.kept.provider().taken();

    OUGHT(got.has_value() && got->paths == two(Handing::cut).paths);
}

#ifndef _WIN32

namespace {

// A pretend clipboard program: a directory with wl-copy at the front of PATH. It pretends either
// to write to a file or to refuse; PATH and the directory go back as they were.
class Helper {
public:
    // Writes the script `named`, which saves its input when `works` (otherwise exits with 1) and,
    // when `stuck`, sleeps five seconds first, and puts its directory at the front of PATH.
    explicit Helper(bool works, const std::string& named = "wl-copy", bool stuck = false) {
        where_ = std::filesystem::temp_directory_path() / ("talcite-schowek-" + std::to_string(::getpid()));

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
        std::filesystem::create_directories(where_, ignored);

        {
            std::ofstream making{where_ / named, std::ios::binary};

            making << "#!/bin/sh\n" << (stuck ? "sleep 5\n" : "")
                   << (works ? "cat > \"" + (where_ / "dostał").string() + "\"\n" : "exit 1\n");
        }

        std::filesystem::permissions(where_ / named, std::filesystem::perms::owner_all, ignored);

        const char* was = std::getenv("PATH");

        was_ = was == nullptr ? std::string{} : std::string{was};

        ::setenv("PATH", (where_.string() + ":" + was_).c_str(), 1);
    }

    // Restores PATH and removes the directory.
    ~Helper() {
        ::setenv("PATH", was_.c_str(), 1);

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
    }

    Helper(const Helper&) = delete;
    Helper& operator=(const Helper&) = delete;

    // What the script received, or an empty string.
    [[nodiscard]] std::string got() const {
        std::ifstream reading{where_ / "dostał", std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{reading}, std::istreambuf_iterator<char>{}};
    }

private:
    std::filesystem::path where_;
    std::string was_;
};

}

// In a local graphical session the copy goes through the session's clipboard program, since the
// VTE family of terminals does not know OSC 52. When the program refuses, OSC 52 remains.
TRIAL("Clipboard: locally the copy goes through the session's clipboard program") {
    {
        const Helper helper{true};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        OUGHT(kept.provider().copy("zażółć"));

        SAME(helper.got(), std::string{"zażółć"});
        SAME(line->written, std::string{});
    }

    {
        const Helper helper{false};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        OUGHT(kept.provider().copy("abc"));

        SAME(line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }

    // Under X: xclip, and without it xsel.
    for (const std::string named : {"xclip", "xsel"}) {
        const Helper helper{true, named};

        auto line = faking::terminal();

        line->environment["DISPLAY"] = ":0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        OUGHT(kept.provider().copy("pod X"));

        SAME(helper.got(), std::string{"pod X"});
        SAME(line->written, std::string{});
    }

    // A program that got stuck gives way to OSC 52 after a second and a half.
    {
        const Helper helper{true, "wl-copy", true};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        const auto began = std::chrono::steady_clock::now();

        OUGHT(kept.provider().copy("abc"));

        OUGHT(std::chrono::steady_clock::now() - began < std::chrono::seconds{4});
        SAME(line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }

    // Over ssh the clipboard program of that machine is not the user's clipboard.
    {
        const Helper helper{true};

        Wired one;

        one.line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        OUGHT(one.kept.provider().copy("abc"));

        SAME(helper.got(), std::string{});
        SAME(one.line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }
}

namespace {

// A pretend Wayland session: wl-copy and wl-paste at the front of PATH. wl-copy saves its arguments
// and its input; wl-paste prints the offer of the asked type, a file named after the type, and
// fails like the real one when there is none. PATH and the directory go back as they were.
class Session {
public:
    // Writes both scripts; with `stuck`, wl-paste sleeps five seconds first.
    explicit Session(bool stuck = false) {
        where_ = std::filesystem::temp_directory_path() / ("talcite-session-" + std::to_string(::getpid()));

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
        std::filesystem::create_directories(where_, ignored);

        const std::string at = where_.string();

        scripted("wl-copy", "printf '%s' \"$*\" > \"" + at + "/args\"\ncat > \"" + at + "/given\"\n");
        scripted("wl-paste", std::string{stuck ? "sleep 5\n" : ""} + "offer=\"" + at + "/offer-$(printf '%s' \"$3\" | tr / _)\"\n" +
                                 "[ -f \"$offer\" ] || exit 1\ncat \"$offer\"\n");

        const char* was = std::getenv("PATH");

        was_ = was == nullptr ? std::string{} : std::string{was};

        ::setenv("PATH", (at + ":" + was_).c_str(), 1);

        line = faking::terminal();
        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        kept = clipboard(Seam<Console>{line});
    }

    // Restores PATH and removes the directory.
    ~Session() {
        ::setenv("PATH", was_.c_str(), 1);

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Makes the clipboard offer the content under the type, as another program would.
    void offer(const std::string& type, const std::string& content) const {
        std::string named = type;

        for (char& one : named)
            if (one == '/') one = '_';

        std::ofstream making{where_ / ("offer-" + named), std::ios::binary};

        making << content;
    }

    // What the last wl-copy got as arguments, or an empty string.
    [[nodiscard]] std::string args() const { return read("args"); }

    // What the last wl-copy got as input, or an empty string.
    [[nodiscard]] std::string given() const { return read("given"); }

    std::shared_ptr<faking::Terminal> line;
    Seam<Clipboard> kept;

private:
    // Writes an executable script under the name.
    void scripted(const std::string& named, const std::string& body) const {
        {
            std::ofstream making{where_ / named, std::ios::binary};

            making << "#!/bin/sh\n" << body;
        }

        std::error_code ignored;

        std::filesystem::permissions(where_ / named, std::filesystem::perms::owner_all, ignored);
    }

    // The content of a file in the directory, or an empty string.
    [[nodiscard]] std::string read(const std::string& named) const {
        std::ifstream reading{where_ / named, std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{reading}, std::istreambuf_iterator<char>{}};
    }

    std::filesystem::path where_;
    std::string was_;
};

}

// Files go to the session's clipboard as text/uri-list, the type file managers read: file URIs with
// every byte outside the unreserved characters escaped, multibyte letters too, lines ended by CR
// LF.
TRIAL("Clipboard: files go to the session's clipboard program as a uri-list") {
    const Session one;

    Files what;

    what.paths = {"/tmp/a b/za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87.mkv", "/srv/100%"};

    OUGHT(one.kept.provider().give(what));

    SAME(one.args(), std::string{"--type text/uri-list"});
    SAME(one.given(), std::string{"file:///tmp/a%20b/za%C5%BC%C3%B3%C5%82%C4%87.mkv\r\nfile:///srv/100%25\r\n"});
    SAME(one.line->written, std::string{});

    (void)one.kept.provider().give({});

    SAME(one.args(), std::string{"--clear"});
}

// A uri-list from another program: comments and lines naming no local file are skipped, the host
// may be empty or localhost, the line ends may be CR LF or LF, and the escapes come back as bytes.
// Such a list is a copy.
TRIAL("Clipboard: a uri-list from another program is read as copied files") {
    const Session one;

    one.offer("text/uri-list", "# from a file manager\r\nfile:///tmp/a%20b\r\nfile://localhost/srv/z%C5%BC\n"
                               "file://elsewhere/x\r\nhttps://example.com/y\r\nfile:///bad%2\r\nfile:///nul%00\r\n");

    const std::optional<Files> got = one.kept.provider().taken();

    OUGHT(got.has_value());

    if (!got.has_value()) return;

    SAME(got->paths.size(), std::size_t{2});
    OUGHT(got->paths.size() == 2 && got->paths[0] == std::filesystem::path{"/tmp/a b"} &&
          got->paths[1] == std::filesystem::path{"/srv/z\xc5\xbc"});
    OUGHT(got->how == Handing::copy);
}

// GNOME's file managers mark a cut in their own type, "cut" or "copy" above the URIs.
TRIAL("Clipboard: a cut from a GNOME file manager is read as cut") {
    const Session one;

    one.offer("x-special/gnome-copied-files", "cut\nfile:///tmp/a\nfile:///tmp/b");
    one.offer("text/uri-list", "file:///tmp/a\r\nfile:///tmp/b\r\n");

    std::optional<Files> got = one.kept.provider().taken();

    OUGHT(got.has_value() && got->how == Handing::cut && got->paths.size() == 2);

    one.offer("x-special/gnome-copied-files", "copy\nfile:///tmp/a\nfile:///tmp/b");

    got = one.kept.provider().taken();

    OUGHT(got.has_value() && got->how == Handing::copy && got->paths.size() == 2);
}

// The uri-list cannot say "cut", so our own cut is recognized by its list: the same paths as we cut
// paste as cut, any other list as a copy.
TRIAL("Clipboard: our own cut comes back as cut, another list as a copy") {
    const Session one;

    Files what;

    what.paths = {"/tmp/a", "/tmp/b"};
    what.how = Handing::cut;

    OUGHT(one.kept.provider().give(what));

    one.offer("text/uri-list", one.given());

    std::optional<Files> got = one.kept.provider().taken();

    OUGHT(got.has_value() && got->how == Handing::cut && got->paths == what.paths);

    one.offer("text/uri-list", "file:///tmp/a\r\n");

    got = one.kept.provider().taken();

    OUGHT(got.has_value() && got->how == Handing::copy && got->paths.size() == 1);
}

// The session's clipboard answers, not our older copy: when it holds something else, there is
// nothing to paste, and a clipboard program that hangs is killed within the limit: each of the two
// readings waits a second and a half, while the program would sleep five seconds through each.
TRIAL("Clipboard: a session clipboard without files gives none, even after our own") {
    {
        const Session one;

        OUGHT(one.kept.provider().give(two(Handing::copy)));
        OUGHT(!one.kept.provider().taken().has_value());
    }

    {
        const Session one{true};

        one.offer("text/uri-list", "file:///tmp/a\r\n");

        const auto began = std::chrono::steady_clock::now();

        OUGHT(!one.kept.provider().taken().has_value());
        OUGHT(std::chrono::steady_clock::now() - began < std::chrono::seconds{8});
    }
}

#endif

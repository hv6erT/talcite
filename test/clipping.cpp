#include "proving.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "faking.hpp"
#include "talcite/clipboard.hpp"

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

    ought(one.kept.provider().copy("ala ma kota"));

    const std::optional<std::string> got = one.kept.provider().paste();

    ought(got.has_value());
    same(*got, std::string{"ala ma kota"});
}

TRIAL("Clipboard: emptiness is not content") {
    Wired one;

    ought(one.kept.provider().copy(""));
    ought(!one.kept.provider().paste().has_value());
}

// The threshold applies only to the way through the terminal: OSC 52 is sometimes silently cut,
// so it is better to say "did not go" than to paste a stub posing as the whole. Whatever the
// terminal says, our copy stays complete and comes back in full.
TRIAL("Clipboard: content over the threshold stays with us in full") {
    Wired one;

    const std::string big(200 * 1024, 'x');

    ought(!one.kept.provider().copy(big));

    const std::optional<std::string> got = one.kept.provider().paste();

    ought(got.has_value());
    same(got->size(), big.size());
}

TRIAL("Clipboard: multibyte characters pass unchanged") {
    Wired one;

    const std::string what = "z\xc4\x85\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87";

    ought(one.kept.provider().copy(what));

    const std::optional<std::string> got = one.kept.provider().paste();

    ought(got.has_value());
    same(got.value_or(""), what);
}

// Our own copy belongs to the clipboard, not to the process: two clipboards on two consoles do
// not see each other's copies. Only the default one is shared.
TRIAL("Clipboard: two clipboards do not share a copy") {
    Wired one;
    Wired two;

    ought(one.kept.provider().copy("abc"));
    ought(!two.kept.provider().paste().has_value());
}

TRIAL("Clipboard: there is one default per process") {
    ought(&clipboard().provider() == &clipboard().provider());
}

// --- What goes to the terminal -------------------------------------------------------------------

TRIAL("Clipboard: OSC 52 goes out with the content in base64") {
    same(spouted("abc"), std::string{"\x1b]52;c;YWJj\x07"});
}

// Padding counts to the character: a terminal that gets base64 without padding may reject the
// whole thing.
TRIAL("Clipboard: base64 pads for all three remainders") {
    same(spouted("a"), std::string{"\x1b]52;c;YQ==\x07"});
    same(spouted("ab"), std::string{"\x1b]52;c;YWI=\x07"});
    same(spouted("abc"), std::string{"\x1b]52;c;YWJj\x07"});
    same(spouted("abcd"), std::string{"\x1b]52;c;YWJjZA==\x07"});
}

TRIAL("Clipboard: base64 does not lose non-ASCII bytes") {
    same(spouted("\xc4\x85"), std::string{"\x1b]52;c;xIU=\x07"});
}

TRIAL("Clipboard: over the threshold nothing goes out") {
    same(spouted(std::string(200 * 1024, 'x')), std::string{});
}

// The threshold is inclusive: exactly that much still goes out.
TRIAL("Clipboard: content equal to the threshold still goes out") {
    Wired one;

    ought(one.kept.provider().copy(std::string(64 * 1024, 'x')));
    ought(!one.line->written.empty());
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

        ought(kept.provider().copy("zażółć"));

        same(helper.got(), std::string{"zażółć"});
        same(line->written, std::string{});
    }

    {
        const Helper helper{false};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        ought(kept.provider().copy("abc"));

        same(line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }

    // Under X: xclip, and without it xsel.
    for (const std::string named : {"xclip", "xsel"}) {
        const Helper helper{true, named};

        auto line = faking::terminal();

        line->environment["DISPLAY"] = ":0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        ought(kept.provider().copy("pod X"));

        same(helper.got(), std::string{"pod X"});
        same(line->written, std::string{});
    }

    // A program that got stuck gives way to OSC 52 after a second and a half.
    {
        const Helper helper{true, "wl-copy", true};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        const auto began = std::chrono::steady_clock::now();

        ought(kept.provider().copy("abc"));

        ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{4});
        same(line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }

    // Over ssh the clipboard program of that machine is not the user's clipboard.
    {
        const Helper helper{true};

        Wired one;

        one.line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        ought(one.kept.provider().copy("abc"));

        same(helper.got(), std::string{});
        same(one.line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }
}

#endif

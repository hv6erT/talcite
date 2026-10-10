#include "talcite/clipboard.hpp"

#include "talcite/encoding.hpp"

#include <cstddef>
#include <memory>
#include <utility>

#ifdef _WIN32

#include <windows.h>

#else

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

#include <unistd.h>

#include "talcite/child.hpp"
#include "talcite/time.hpp"

#endif

namespace talcite {

namespace {

// How many bytes of content may be sent in OSC 52. Terminals cut long strings silently, each at its
// own threshold, and none announces it, so above ours we send nothing: "it did not work" is better
// than a stub posing as the whole. Inside the program the copy stays complete.
constexpr std::size_t brim = 64 * 1024;

constexpr const char* letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 with padding, because that is what terminals expect. Twenty lines instead of a dependency:
// this is not cryptography, only rewriting three bytes as four characters.
std::string sixed(std::string_view what) {
    std::string made;

    made.reserve((what.size() + 2) / 3 * 4);

    std::size_t at = 0;

    while (at + 2 < what.size()) {
        const auto one = static_cast<unsigned char>(what[at]);
        const auto two = static_cast<unsigned char>(what[at + 1]);
        const auto three = static_cast<unsigned char>(what[at + 2]);

        made.push_back(letters[one >> 2]);
        made.push_back(letters[((one & 0x03) << 4) | (two >> 4)]);
        made.push_back(letters[((two & 0x0F) << 2) | (three >> 6)]);
        made.push_back(letters[three & 0x3F]);

        at += 3;
    }

    if (at + 1 == what.size()) {
        const auto one = static_cast<unsigned char>(what[at]);

        made.push_back(letters[one >> 2]);
        made.push_back(letters[(one & 0x03) << 4]);
        made.push_back('=');
        made.push_back('=');
    } else if (at + 2 == what.size()) {
        const auto one = static_cast<unsigned char>(what[at]);
        const auto two = static_cast<unsigned char>(what[at + 1]);

        made.push_back(letters[one >> 2]);
        made.push_back(letters[((one & 0x03) << 4) | (two >> 4)]);
        made.push_back(letters[(two & 0x0F) << 2]);
        made.push_back('=');
    }

    return made;
}

// Asks the terminal to put the text into the clipboard of its own machine; false when the text is
// above `brim`. `c` is the system clipboard; we leave the primary selection (`p`, the middle mouse
// button) alone: one clipboard, one content.
//
// Inside tmux this gets through only with set-clipboard on. We deliberately do not wrap it in tmux
// passthrough: on newer versions the wrapping breaks what works without it, and the tmux version
// cannot be learned from here other than by guessing.
bool sent(Console& out, std::string_view what) {
    if (what.size() > brim) return false;

    out.write("\x1b]52;c;" + sixed(what) + "\x07");

    return true;
}

// Over ssh the clipboard of this process is the clipboard of a machine other than the one the user
// sits at. Then the only correct way is to ask the terminal. The environment is read from the
// console, because the console knows where the user sits, and thanks to that a test can pretend to
// be ssh on any system.
bool remote(const Console& from) {
    return from.variable("SSH_CONNECTION").has_value() || from.variable("SSH_TTY").has_value();
}

#ifndef _WIN32

// Whether the program is in PATH, searched the same way the shell does.
bool present(const std::string& named) {
    const char* whole = std::getenv("PATH");

    if (whole == nullptr) return false;

    const std::string said{whole};

    for (std::size_t at = 0; at <= said.size();) {
        const std::size_t next = std::min(said.find(':', at), said.size());

        const std::string one = said.substr(at, next - at);

        if (!one.empty() && ::access((one + "/" + named).c_str(), X_OK) == 0) return true;

        at = next + 1;
    }

    return false;
}

// The graphical session's clipboard through its own program: `wl-copy` under Wayland, `xclip` or
// `xsel` under X, `pbcopy` on macOS. OSC 52 stays as the fallback: GNOME Terminal and the rest of
// the VTE family do not know it, so a copy would not reach the clipboard there at all. Over ssh
// this way is closed: the clipboard program of that machine is not the user's clipboard.
//
// Each of them leaves behind a process that guards the clipboard; it runs through the shell with
// output to /dev/null, so that this process does not hold our pipes and does not delay the end.
std::optional<Command> helped(const Console& from) {
    std::string line;

    if (from.variable("WAYLAND_DISPLAY").has_value() && present("wl-copy")) line = "exec wl-copy";
    else if (from.variable("DISPLAY").has_value() && present("xclip")) line = "exec xclip -selection clipboard -in";
    else if (from.variable("DISPLAY").has_value() && present("xsel")) line = "exec xsel --clipboard --input";
#ifdef __APPLE__
    else if (present("pbcopy")) line = "exec pbcopy";
#endif

    if (line.empty()) return std::nullopt;

    Command made;

    made.program = "/bin/sh";
    made.arguments = {"-c", line + " >/dev/null 2>&1"};

    return made;
}

// Hands the content to the clipboard program; true when it finished without an error. We wait at
// most a second and a half: a program that got stuck gives way to OSC 52 instead of freezing the
// loop.
bool handed(const Command& what, std::string_view said) {
    try {
        const Child one = Child::start(what).await();

        Promise<std::size_t> writing = one.write(bytes(std::string{said}));

        (void)writing.await();

        one.close_input();

        Promise<Exit> ending = one.wait();
        Promise<void> limit = Time::limit(std::chrono::milliseconds{1500});

        try {
            Promise<void>::race(ending, limit).await();
        } catch (...) {
            one.kill();

            return false;
        }

        limit.cancel();

        const Exit made = ending.await();

        return made.ended == Ended::normal && made.code == 0;
    } catch (...) {
        return false;
    }
}

#endif

#ifdef _WIN32

// The Windows clipboard keeps lines separated by a pair (CR LF), and the document by a single
// character. Without the conversion, three copied lines would be one in Notepad.
std::wstring widened(std::string_view what) {
    std::string full;

    full.reserve(what.size() + what.size() / 16);

    for (const char one : what) {
        if (one == '\n') full.push_back('\r');

        full.push_back(one);
    }

    return to_wstring(full);
}

// The clipboard's UTF-16 text as UTF-8, with CR LF pairs turned back into single line feeds.
std::string narrowed(const wchar_t* what) {
    if (what == nullptr) return {};

    const std::string full = to_string(std::wstring_view{what});

    std::string made;

    made.reserve(full.size());

    for (std::size_t at = 0; at < full.size(); ++at) {
        if (full[at] == '\r' && at + 1 < full.size() && full[at + 1] == '\n') continue;

        made.push_back(full[at]);
    }

    return made;
}

// Opens the clipboard. Opening can fail because another process holds the clipboard at the moment.
// A few immediate retries are enough; sleeping is ruled out, because it would freeze the event
// loop.
bool opened() {
    for (int which = 0; which < 8; ++which)
        if (OpenClipboard(nullptr) != 0) return true;

    return false;
}

#endif

#ifdef _WIN32

// Hands the text to the Windows system clipboard; true when it took it.
bool handed(std::string_view what) {
    const std::wstring wide = widened(what);

    if (!opened()) return false;

    // From here on every way out has to close the clipboard: left open, it blocks the clipboard of
    // the whole desktop, not just ours.
    if (EmptyClipboard() == 0) {
        CloseClipboard();

        return false;
    }

    const std::size_t much = (wide.size() + 1) * sizeof(wchar_t);

    const HGLOBAL room = GlobalAlloc(GMEM_MOVEABLE, much);

    if (room == nullptr) {
        CloseClipboard();

        return false;
    }

    auto* said = static_cast<wchar_t*>(GlobalLock(room));

    if (said == nullptr) {
        GlobalFree(room);
        CloseClipboard();

        return false;
    }

    for (std::size_t at = 0; at < wide.size(); ++at) said[at] = wide[at];

    said[wide.size()] = L'\0';

    GlobalUnlock(room);

    // After a successful hand-over the memory belongs to the system, and freeing it would damage
    // the clipboard. After a failed one it is still ours, and we have to free it.
    const bool went = SetClipboardData(CF_UNICODETEXT, room) != nullptr;

    if (!went) GlobalFree(room);

    CloseClipboard();

    return went;
}

// The text on the Windows clipboard; an empty inner value for an empty clipboard. Failing to open
// is different from an empty clipboard: then the outer value is empty and the caller reaches for
// its own copy.
std::optional<std::optional<std::string>> taken() {
    if (!opened()) return std::nullopt;

    const HANDLE room = GetClipboardData(CF_UNICODETEXT);

    if (room == nullptr) {
        CloseClipboard();

        return std::optional<std::string>{};
    }

    const auto* said = static_cast<const wchar_t*>(GlobalLock(room));

    std::string made = narrowed(said);

    if (said != nullptr) GlobalUnlock(room);

    CloseClipboard();

    if (made.empty()) return std::optional<std::string>{};

    return std::optional<std::string>{std::move(made)};
}

#endif

// The clipboard of the machine the user sits at. On Windows, locally, the system one, and over ssh
// the terminal; on POSIX the graphical session's program, with the terminal as the fallback.
class System final : public Clipboard {
public:
    explicit System(Seam<Console> with) : console_(std::move(with)) {}

    // The name of this clipboard in messages.
    [[nodiscard]] std::string_view name() const override { return "system clipboard"; }

    // Copies the text: keeps it as our own copy, then hands it to the system clipboard or the
    // clipboard program, or else asks the terminal. Returns whether it reached a clipboard.
    [[nodiscard]] bool copy(std::string_view what) override {
        mine_.assign(what);

        Console& out = console_.provider();

#ifdef _WIN32

        if (!remote(out)) return handed(what);

#else

        if (!remote(out))
            if (const std::optional<Command> helper = helped(out); helper && handed(*helper, what)) return true;

#endif

        return sent(out, what);
    }

    // The text to paste: on Windows, locally, from the system clipboard; otherwise our own last
    // copy, or nothing.
    [[nodiscard]] std::optional<std::string> paste() override {
#ifdef _WIN32

        if (!remote(console_.provider()))
            if (std::optional<std::optional<std::string>> got = taken(); got.has_value())
                return std::move(*got);

#endif

        if (mine_.empty()) return std::nullopt;

        return mine_;
    }

private:
    Seam<Console> console_;

    // Our own last copy. On POSIX it is the whole answer of `paste`, because we do not ask the
    // terminal for the clipboard: its answer would pour into the program as keys, as long as the
    // decoder does not know OSC. On Windows it is a fallback in case the system clipboard refuses
    // to open.
    //
    // This is not a private clipboard posing as the system one: it is one variable so that cut and
    // paste within the same program works everywhere. Pasting from outside is done by the user with
    // the terminal's key and arrives as a `PasteEvent`, so that path is not affected.
    std::string mine_;
};

}

// The clipboard of the user's machine. One per process, because our own copy has to be shared: what
// is cut in one editor is pasted in another. Whoever wants a separate one (a test, a second
// terminal) takes the version with a console.
Seam<Clipboard> clipboard() {
    static const Seam<Clipboard> shared{std::make_shared<System>(console())};

    return shared;
}

// A separate clipboard that talks through the given console, with its own copy.
Seam<Clipboard> clipboard(Seam<Console> with) {
    return Seam<Clipboard>{std::make_shared<System>(std::move(with))};
}

}

#include "talcite/clipboard.hpp"

#include "talcite/base64.hpp"
#include "talcite/encoding.hpp"
#include "talcite/fs.hpp"

#include "remoting.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32

#include <cstring>

#include <windows.h>

#include <shlobj.h>

#else

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

#include <unistd.h>

#include "talcite/child.hpp"
#include "talcite/time.hpp"
#include "hexing.hpp"
#include "netting.hpp"

#endif

namespace talcite {

namespace {

// How many bytes of content may be sent in OSC 52. Terminals cut long strings silently, each at its
// own threshold, and none announces it, so above ours we send nothing: "it did not work" is better
// than a stub posing as the whole. Inside the program the copy stays complete.
constexpr std::size_t brim = 64 * 1024;

// Asks the terminal to put the text into the clipboard of its own machine; false when the text is
// above `brim`. `c` is the system clipboard; we leave the primary selection (`p`, the middle mouse
// button) alone: one clipboard, one content.
//
// Inside tmux this gets through only with set-clipboard on. We deliberately do not wrap it in tmux
// passthrough: on newer versions the wrapping breaks what works without it, and the tmux version
// cannot be learned from here other than by guessing.
bool sent(Console& out, std::string_view what) {
    if (what.size() > brim) return false;

    out.write("\x1b]52;c;" + base64(std::as_bytes(std::span{what})) + "\x07");

    return true;
}

// The files as text, one path per line: what a text paste gets after files were copied, the same
// as on the Windows clipboard, where the paths go beside the list of files.
std::string spelled(const Files& what) {
    std::string made;

    for (const fs::path& one : what.paths) {
        if (!made.empty()) made.push_back('\n');

        made += fs::to_string(one);
    }

    return made;
}

// Whether the files can leave the process: every path absolute. Another program does not share our
// working directory, so a relative path would point it somewhere else.
bool portable(const Files& what) {
    for (const fs::path& one : what.paths)
        if (!one.is_absolute()) return false;

    return true;
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

// A shell line as a command, for the clipboard programs.
Command shelled(const std::string& line) {
    Command made;

    made.program = "/bin/sh";
    made.arguments = {"-c", line};

    return made;
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

    return shelled(line + " >/dev/null 2>&1");
}

// Hands the content to the clipboard program; true when it finished without an error. We wait at
// most a second and a half: a program that got stuck gives way to OSC 52 instead of freezing the
// loop.
bool handed(const Command& what, std::string_view said) {
    try {
        const Child one = Child::start(what).get();

        (void)one.write(bytes(std::string{said})).get();

        one.close_input();

        Exit made;

        try {
            made = one.wait().timeout(std::chrono::milliseconds{1500}).get();
        } catch (...) {
            one.kill();

            return false;
        }

        return made.ended == Ended::normal && made.code == 0;
    } catch (...) {
        return false;
    }
}

// The clipboard programs that carry a content type, which a list of files needs: wl-copy with
// wl-paste under Wayland, xclip under X. xsel and pbcopy carry only text, so with them files stay
// in our own copy.
enum class Typed : std::uint8_t { none, wayland, x };

// Which typed clipboard program serves the session, in the same order as `helped`.
Typed typed(const Console& from) {
    if (from.variable("WAYLAND_DISPLAY").has_value() && present("wl-copy") && present("wl-paste"))
        return Typed::wayland;

    if (from.variable("DISPLAY").has_value() && present("xclip")) return Typed::x;

    return Typed::none;
}

// The command that puts a list of files on the clipboard as text/uri-list, or with `clearing`
// takes the clipboard's content away. The program stays behind to serve the clipboard, so its
// output goes to /dev/null, as in `helped`.
Command writing(Typed by, bool clearing) {
    if (by == Typed::wayland)
        return shelled(clearing ? "exec wl-copy --clear >/dev/null 2>&1"
                                : "exec wl-copy --type text/uri-list >/dev/null 2>&1");

    return shelled("exec xclip -selection clipboard -t text/uri-list -in >/dev/null 2>&1");
}

// The command that prints the clipboard's content of the given type. A type the clipboard does not
// offer ends the program with an error, which is how we learn the clipboard holds something else.
Command reading(Typed by, const std::string& type) {
    if (by == Typed::wayland) return shelled("exec wl-paste --no-newline --type " + type + " 2>/dev/null");

    return shelled("exec xclip -selection clipboard -t " + type + " -out 2>/dev/null");
}

// What the clipboard program printed, when it ended without an error within a second and a half,
// the limit of `handed`; nothing otherwise. A stuck program is killed rather than left to freeze
// the loop, and so is one printing more than any list of files could need.
std::optional<std::string> fetched(const Command& what) {
    constexpr std::size_t most = 4 * 1024 * 1024;

    try {
        const Child one = Child::start(what).get();

        one.close_input();

        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{1500};

        std::string made;

        for (;;) {
            Bytes piece;

            try {
                piece = one.read(64 * 1024).timeout(net::left(until)).get();
            } catch (...) {
                one.kill();

                return std::nullopt;
            }

            if (piece.empty()) break;

            if (made.size() + piece.size() > most) {
                one.kill();

                return std::nullopt;
            }

            made += text(piece);
        }

        Exit ended;

        try {
            ended = one.wait().timeout(net::left(until)).get();
        } catch (...) {
            one.kill();

            return std::nullopt;
        }

        if (ended.ended != Ended::normal || ended.code != 0) return std::nullopt;

        return made;
    } catch (...) {
        return std::nullopt;
    }
}

// Whether a byte goes into a file URI as it is: the unreserved characters of RFC 3986 and the slash
// between names. Every other byte, also each byte of a multibyte character, goes as %XX, the way
// file managers write it.
bool plain(unsigned char one) {
    return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '-' ||
           one == '.' || one == '_' || one == '~' || one == '/';
}

// An absolute path as a file URI with an empty host.
std::string located(const fs::path& where) {
    std::string made = "file://";

    for (const char each : where.native()) {
        const auto one = static_cast<unsigned char>(each);

        if (plain(one)) {
            made.push_back(each);

            continue;
        }

        hex::escaped(one, made);
    }

    return made;
}

// The path of a file URI on this machine: file:///path or file://localhost/path, with the %XX
// escapes decoded. Another host, another scheme, a broken escape or an escaped NUL give nothing:
// none of them names a local file we could copy.
std::optional<fs::path> unlocated(std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    constexpr std::string_view local = "localhost";

    if (uri.substr(0, scheme.size()) != scheme) return std::nullopt;

    uri.remove_prefix(scheme.size());

    if (uri.substr(0, local.size()) == local) uri.remove_prefix(local.size());

    if (uri.empty() || uri.front() != '/') return std::nullopt;

    std::string made;

    for (std::size_t at = 0; at < uri.size(); ++at) {
        if (uri[at] != '%') {
            made.push_back(uri[at]);

            continue;
        }

        if (at + 2 >= uri.size()) return std::nullopt;

        const int high = hex::digit(uri[at + 1]);
        const int low = hex::digit(uri[at + 2]);

        if (high < 0 || low < 0 || (high == 0 && low == 0)) return std::nullopt;

        made.push_back(static_cast<char>((high << 4) | low));

        at += 2;
    }

    return fs::path{made};
}

// The local files of a text/uri-list (RFC 2483): one URI per line, ended by CR LF or by LF alone,
// with lines starting with # as comments. Lines that name no local file are skipped.
std::vector<fs::path> listed(std::string_view text) {
    std::vector<fs::path> made;

    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());

        std::string_view line = text.substr(0, end);

        text.remove_prefix(std::min(end + 1, text.size()));

        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.remove_suffix(1);

        if (line.empty() || line.front() == '#') continue;

        if (std::optional<fs::path> one = unlocated(line)) made.push_back(std::move(*one));
    }

    return made;
}

// The URIs of the files, each line ended by CR LF, as RFC 2483 wants.
std::string uris(const Files& what) {
    std::string made;

    for (const fs::path& one : what.paths) made += located(one) + "\r\n";

    return made;
}

// The files the clipboard program reads, or nothing when the clipboard holds no files. GNOME's file
// managers mark a cut in x-special/gnome-copied-files ("cut" or "copy" on the first line, the URIs
// below); a plain text/uri-list is a copy, unless it is exactly the list we cut ourselves (`ours`),
// because we cannot mark a cut beside it: the program carries one type at a time.
std::optional<Files> gathered(Typed by, const std::optional<Files>& ours) {
    if (const std::optional<std::string> got = fetched(reading(by, "x-special/gnome-copied-files"))) {
        const std::size_t end = std::min(got->find('\n'), got->size());

        const std::string_view verb = std::string_view{*got}.substr(0, end);

        Files made;

        made.paths = listed(std::string_view{*got}.substr(std::min(end + 1, got->size())));
        made.how = verb == "cut" ? Handing::cut : Handing::copy;

        if ((verb == "cut" || verb == "copy") && !made.paths.empty()) return made;
    }

    const std::optional<std::string> got = fetched(reading(by, "text/uri-list"));

    if (!got.has_value()) return std::nullopt;

    Files made;

    made.paths = listed(*got);

    if (made.paths.empty()) return std::nullopt;

    if (ours.has_value() && ours->paths == made.paths) made.how = ours->how;

    return made;
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

// Global memory holding a copy of the bytes, ready to be handed to the clipboard; null when the
// system has no memory to give.
HGLOBAL lent(const void* from, std::size_t much) {
    const HGLOBAL room = GlobalAlloc(GMEM_MOVEABLE, much);

    if (room == nullptr) return nullptr;

    void* at = GlobalLock(room);

    if (at == nullptr) {
        GlobalFree(room);

        return nullptr;
    }

    std::memcpy(at, from, much);

    GlobalUnlock(room);

    return room;
}

// Hands the memory to the open clipboard under the format; true when the clipboard took it. After a
// successful hand-over the memory belongs to the system, and freeing it would damage the clipboard.
// After a failed one it is still ours, and we free it.
bool placed(UINT format, HGLOBAL room) {
    if (room == nullptr) return false;

    if (SetClipboardData(format, room) != nullptr) return true;

    GlobalFree(room);

    return false;
}

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

    const bool went = placed(CF_UNICODETEXT, lent(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t)));

    CloseClipboard();

    return went;
}

// The text on the Windows clipboard; an empty inner value for an empty clipboard. Failing to open
// is different from an empty clipboard: then the outer value is empty and the caller reaches for
// its own copy.
std::optional<std::optional<std::string>> written() {
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

// The clipboard format in which Explorer says whether files were copied or cut: a DWORD with
// DROPEFFECT_COPY or DROPEFFECT_MOVE. The name is fixed, the number is given by the system.
UINT effected() { return RegisterClipboardFormatW(L"Preferred DropEffect"); }

// A path for another program: with backslashes, the separator every Windows program understands.
std::wstring preferred(const fs::path& one) {
    fs::path made = one;

    made.make_preferred();

    return made.native();
}

// The files as a CF_HDROP block: the DROPFILES header, then the wide paths, each ended by NUL, and
// one more NUL closing the list.
std::vector<std::byte> packed(const Files& what) {
    std::wstring names;

    for (const fs::path& one : what.paths) {
        names += preferred(one);
        names.push_back(L'\0');
    }

    names.push_back(L'\0');

    DROPFILES head{};

    head.pFiles = sizeof(DROPFILES);
    head.fWide = TRUE;

    std::vector<std::byte> made(sizeof(DROPFILES) + names.size() * sizeof(wchar_t));

    std::memcpy(made.data(), &head, sizeof(DROPFILES));
    std::memcpy(made.data() + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));

    return made;
}

// The names in a stretch of a CF_HDROP block: separated by NUL and closed by an empty name. A name
// the block ends in the middle of is not a name, so it is left out.
template <typename Unit>
std::vector<std::basic_string<Unit>> split(std::basic_string_view<Unit> all) {
    std::vector<std::basic_string<Unit>> made;

    while (!all.empty()) {
        const std::size_t end = all.find(Unit{0});

        if (end == std::basic_string_view<Unit>::npos || end == 0) break;

        made.emplace_back(all.substr(0, end));

        all.remove_prefix(end + 1);
    }

    return made;
}

// A name in the ANSI code page, which a CF_HDROP block from an old program may carry, as UTF-16.
std::wstring ansi(const std::string& one) {
    const int much = MultiByteToWideChar(CP_ACP, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);

    if (much <= 0) return {};

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    (void)MultiByteToWideChar(CP_ACP, 0, one.data(), static_cast<int>(one.size()), made.data(), much);

    return made;
}

// The paths in a CF_HDROP block of `much` bytes, wide or in the ANSI code page as its header says.
// The block comes from another process, so nothing is read beyond its size, whatever the header
// claims.
std::vector<fs::path> unpacked(const std::byte* block, std::size_t much) {
    std::vector<fs::path> made;

    if (much < sizeof(DROPFILES)) return made;

    DROPFILES head{};

    std::memcpy(&head, block, sizeof(DROPFILES));

    if (head.pFiles < sizeof(DROPFILES) || head.pFiles >= much) return made;

    const std::byte* names = block + head.pFiles;
    const std::size_t left = much - head.pFiles;

    if (head.fWide != FALSE) {
        std::wstring all(left / sizeof(wchar_t), L'\0');

        std::memcpy(all.data(), names, all.size() * sizeof(wchar_t));

        for (std::wstring& one : split(std::wstring_view{all})) made.emplace_back(std::move(one));
    } else {
        const std::string all(reinterpret_cast<const char*>(names), left);

        for (const std::string& one : split(std::string_view{all}))
            if (std::wstring wide = ansi(one); !wide.empty()) made.emplace_back(std::move(wide));
    }

    return made;
}

// Puts the files on the Windows clipboard the way Explorer does: CF_HDROP, the Preferred DropEffect
// (copy, or move for a cut) and, for programs that paste text, the paths one per line. True when
// the list and its effect were taken. An empty list takes the content away, but only while it is
// files: text copied by someone since then stays.
bool dropped(const Files& what) {
    if (!opened()) return false;

    if (what.paths.empty()) {
        const bool went = IsClipboardFormatAvailable(CF_HDROP) == 0 || EmptyClipboard() != 0;

        CloseClipboard();

        return went;
    }

    if (EmptyClipboard() == 0) {
        CloseClipboard();

        return false;
    }

    const std::vector<std::byte> block = packed(what);

    const DWORD effect = what.how == Handing::cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;

    const std::wstring wide = widened(spelled(what));

    const bool filed = placed(CF_HDROP, lent(block.data(), block.size()));
    const bool marked = filed && placed(effected(), lent(&effect, sizeof(effect)));

    if (filed) (void)placed(CF_UNICODETEXT, lent(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t)));

    CloseClipboard();

    return filed && marked;
}

// Whether the Preferred DropEffect on the open clipboard says move without copy, which is how
// Explorer marks a cut. Without the format the files were copied.
bool moved() {
    const HANDLE room = GetClipboardData(effected());

    if (room == nullptr || GlobalSize(room) < sizeof(DWORD)) return false;

    const void* at = GlobalLock(room);

    if (at == nullptr) return false;

    DWORD effect = 0;

    std::memcpy(&effect, at, sizeof(DWORD));

    GlobalUnlock(room);

    return (effect & DROPEFFECT_MOVE) != 0 && (effect & DROPEFFECT_COPY) == 0;
}

// The files on the Windows clipboard and whether they were cut. As with `written`, the outer value
// is empty when the clipboard would not open, and the inner one when it holds no files.
std::optional<std::optional<Files>> dragged() {
    if (!opened()) return std::nullopt;

    const HANDLE room = GetClipboardData(CF_HDROP);

    if (room == nullptr) {
        CloseClipboard();

        return std::optional<Files>{};
    }

    Files made;

    if (const auto* block = static_cast<const std::byte*>(GlobalLock(room)); block != nullptr) {
        made.paths = unpacked(block, GlobalSize(room));

        GlobalUnlock(room);
    }

    if (moved()) made.how = Handing::cut;

    CloseClipboard();

    if (made.paths.empty()) return std::optional<Files>{};

    return std::optional<Files>{std::move(made)};
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
        files_.reset();

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
            if (std::optional<std::optional<std::string>> got = written(); got.has_value())
                return std::move(*got);

#endif

        if (mine_.empty()) return std::nullopt;

        return mine_;
    }

    // Puts the files on the clipboard as copied or cut, as one content with their paths as text: on
    // Windows, locally, the system clipboard; on POSIX the session's clipboard program when it
    // carries content types (`typed`); our own copy in any case. True when the files reached the
    // system clipboard, false when only our copy holds them. A relative path refuses the whole list
    // and changes nothing (`portable`). An empty list takes away files given before, and only
    // files: text copied since then stays.
    [[nodiscard]] bool give(const Files& what) override {
        if (!portable(what)) return false;

        if (what.paths.empty()) {
            if (files_.has_value()) mine_.clear();

            files_.reset();
        } else {
            files_ = what;
            mine_ = spelled(what);
        }

        Console& out = console_.provider();

        if (remote(out)) return false;

#ifdef _WIN32

        return dropped(what);

#else

        const Typed by = typed(out);

        if (by == Typed::none) return false;

        return handed(writing(by, what.paths.empty()), uris(what));

#endif
    }

    // The files to paste and how. Locally the system clipboard answers: on Windows its CF_HDROP,
    // cut when Explorer's effect says move; on POSIX what the clipboard program reads (`gathered`).
    // A system clipboard that holds something else gives nothing, because our older copy must not
    // paste over what the user has copied since. Our own copy answers only when the system
    // clipboard is out of reach: over ssh, without a typed clipboard program, or when Windows will
    // not open it.
    [[nodiscard]] std::optional<Files> taken() override {
        Console& out = console_.provider();

        if (!remote(out)) {
#ifdef _WIN32

            if (std::optional<std::optional<Files>> got = dragged(); got.has_value()) return std::move(*got);

#else

            if (const Typed by = typed(out); by != Typed::none) return gathered(by, files_);

#endif
        }

        return files_;
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

    // Our own last list of files, which `taken` gives when the system clipboard is out of reach. On
    // POSIX it also tells our own cut from a copy, which the uri-list on the clipboard cannot
    // carry.
    std::optional<Files> files_;
};

}

// A clipboard that knows only text takes no files.
bool Clipboard::give(const Files&) { return false; }

// A clipboard that knows only text has no files to give.
std::optional<Files> Clipboard::taken() { return std::nullopt; }

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

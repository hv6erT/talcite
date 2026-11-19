#include "tal/clipboard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "clipboarding.hpp"
#include "locating.hpp"

#ifdef _WIN32
#include <windows.h>

#include <shlobj.h>
#endif

namespace tal {

namespace {

using namespace std::chrono_literals;

// How many bytes of text may go out in OSC 52. Terminals cut long strings silently, each at its own
// threshold, so above ours nothing goes: "it did not work" is better than a stub posing as the
// whole. The program's own copy stays whole.
constexpr std::size_t brim = 64 * 1024;

// How long a clipboard program may take; a stuck one is killed and the next way tried, rather than
// left to hold the program.
constexpr std::chrono::milliseconds patience = 1500ms;

// The most a clipboard program may print: more than any list of files needs.
constexpr std::size_t most = 4 * 1024 * 1024;

// The exit status of a shell line that found none of its programs.
constexpr int lacking = 127;

// Whether the program runs over ssh: then the clipboard programs and the system clipboard of this
// machine are not the user's, and only the terminal reaches the user's clipboard.
bool remote(const Console& from) { return from.variable("SSH_CONNECTION").has_value() || from.variable("SSH_TTY").has_value(); }

// `what` in base64, for OSC 52.
std::string coded(std::string_view what) {
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string made;

    for (std::size_t at = 0; at < what.size(); at += 3) {
        const std::size_t much = std::min<std::size_t>(3, what.size() - at);
        std::uint32_t held = 0;

        for (std::size_t one = 0; one < 3; ++one)
            held = (held << 8) | (one < much ? static_cast<unsigned char>(what[at + one]) : 0U);

        for (std::size_t one = 0; one < 4; ++one) made.push_back(one <= much ? digits[(held >> (18 - 6 * one)) & 0x3F] : '=');
    }

    return made;
}

// Asks the terminal to put `what` into the clipboard of the machine the user sits at; false above
// `brim`. Only the clipboard (`c`), not the primary selection: one clipboard, one content. Inside
// tmux it goes through only with set-clipboard on; wrapping it in tmux's passthrough breaks newer
// versions, and the version cannot be learned from here.
bool sent(Console& out, std::string_view what) {
    if (what.size() > brim) return false;

    out.write("\x1b]52;c;" + coded(what) + "\x07");

    return true;
}

// The paths one to a line: what a text paste gets after files were given.
std::string spelled(const Handed& what) {
    std::string made;

    for (const Path& one : what.paths) {
        if (!made.empty()) made.push_back('\n');

        made += one.text();
    }

    return made;
}

// Whether every path is absolute: another program does not share our working folder.
bool portable(const Handed& what) {
    return std::all_of(what.paths.begin(), what.paths.end(), [](const Path& one) { return one.absolute(); });
}

// A shell line as a command.
Command shelled(std::string line) {
    Command made;

    made.program = Path{"/bin/sh"};
    made.arguments = {"-c", std::move(line)};

    return made;
}

// The shell line that hands text to the graphical session's clipboard program: wl-copy under
// Wayland, xclip or xsel under X, pbcopy on macOS, each only when present; it ends with `lacking`
// when none is. Nothing when the session has no clipboard program to look for. Each program stays
// behind to serve the clipboard, so its output goes to /dev/null and holds none of our pipes.
std::optional<std::string> copier(const Console& from) {
    std::string line;

    if (from.variable("WAYLAND_DISPLAY")) line += "command -v wl-copy >/dev/null 2>&1 && exec wl-copy >/dev/null 2>&1\n";

    if (from.variable("DISPLAY")) {
        line += "command -v xclip >/dev/null 2>&1 && exec xclip -selection clipboard -in >/dev/null 2>&1\n";
        line += "command -v xsel >/dev/null 2>&1 && exec xsel --clipboard --input >/dev/null 2>&1\n";
    }

#ifdef __APPLE__
    line += "command -v pbcopy >/dev/null 2>&1 && exec pbcopy >/dev/null 2>&1\n";
#endif

    if (line.empty()) return std::nullopt;

    return line + "exit " + std::to_string(lacking);
}

// The shell line that hands a list of files to the session's clipboard program that carries a type
// (wl-copy with wl-paste under Wayland, xclip under X), or with `clearing` takes the content away.
// It ends with `lacking` when none is present.
std::string giver(const Console& from, bool clearing) {
    std::string line;

    if (from.variable("WAYLAND_DISPLAY"))
        line += std::string{"command -v wl-copy >/dev/null 2>&1 && command -v wl-paste >/dev/null 2>&1 && exec wl-copy "} +
                (clearing ? "--clear" : "--type text/uri-list") + " >/dev/null 2>&1\n";

    if (from.variable("DISPLAY")) line += "command -v xclip >/dev/null 2>&1 && exec xclip -selection clipboard -t text/uri-list -in >/dev/null 2>&1\n";

    return line + "exit " + std::to_string(lacking);
}

// The shell line that prints the clipboard's content of the type `type` through the same programs
// as `giver`. A type the clipboard does not hold ends it with an error; none of the programs ends
// it with `lacking`.
std::string reader(const Console& from, const std::string& type) {
    std::string line;

    if (from.variable("WAYLAND_DISPLAY"))
        line += "command -v wl-copy >/dev/null 2>&1 && command -v wl-paste >/dev/null 2>&1 && exec wl-paste --no-newline --type " + type +
                " 2>/dev/null\n";

    if (from.variable("DISPLAY")) line += "command -v xclip >/dev/null 2>&1 && exec xclip -selection clipboard -t " + type + " -out 2>/dev/null\n";

    return line + "exit " + std::to_string(lacking);
}

// The URIs of the files, each line ended by CR LF.
std::string uris(const Handed& what) {
    std::string made;

    for (const Path& one : what.paths) made += locating::located(one) + "\r\n";

    return made;
}

// What a clipboard program did: whether any was present, and what it printed when it ended well.
struct Fetched {
    bool present = true;
    std::optional<std::string> text;
};

// Feeds `input` to `child`, closes its input and waits for its end.
Task<Exit> fed(Process child, std::string input) {
    co_await child.input().write(bytes(input));
    child.input().close();

    co_return co_await child.ended();
}

// Runs the shell line `line` with `input`; true when it ended well within `patience`. A program
// that is stuck is killed; one that cannot start is a failure.
Task<bool> handed(std::shared_ptr<Processes> processes, std::string line, std::string input) {
    Process child;

    try {
        child = co_await processes->start(shelled(std::move(line)));
    } catch (const Trouble&) {
        co_return false;
    }

    try {
        const Exit made = co_await within(patience, fed(child, std::move(input)));

        co_return made.ended == Exit::Ended::normal && made.code == 0;
    } catch (const Trouble&) {
        child.kill();

        co_return false;
    }
}

// What `child` prints, up to `most` bytes, and how it ended.
Task<Fetched> gathered(Process child) {
    std::string made;

    while (true) {
        const Bytes piece = co_await child.output().read(64 * 1024);

        if (piece.empty()) break;

        if (made.size() + piece.size() > most) throw Trouble{Kind::full, "The clipboard program printed too much."};

        made += text::of(piece);
    }

    const Exit ended = co_await child.ended();

    if (ended.ended == Exit::Ended::normal && ended.code == lacking) co_return Fetched{false, std::nullopt};

    if (ended.ended != Exit::Ended::normal || ended.code != 0) co_return Fetched{};

    co_return Fetched{true, std::move(made)};
}

// What the shell line `line` prints, as `gathered` tells it, within `patience`; a program stuck or
// printing too much is killed and gives nothing.
Task<Fetched> fetched(std::shared_ptr<Processes> processes, std::string line) {
    Process child;

    try {
        child = co_await processes->start(shelled(std::move(line)));
    } catch (const Trouble&) {
        co_return Fetched{};
    }

    child.input().close();

    try {
        co_return co_await within(patience, gathered(child));
    } catch (const Trouble&) {
        child.kill();

        co_return Fetched{};
    }
}

#ifdef _WIN32

// The Windows clipboard ends lines with CR LF and the program with LF; without the change three
// copied lines would be one in Notepad.
std::wstring widened(std::string_view what) {
    std::string full;

    for (const char one : what) {
        if (one == '\n') full.push_back('\r');

        full.push_back(one);
    }

    return text::wide(full);
}

// The clipboard's UTF-16 text as UTF-8, CR LF turned back into LF.
std::string narrowed(const wchar_t* what) {
    if (what == nullptr) return {};

    const std::string full = text::narrow(std::wstring_view{what});
    std::string made;

    for (std::size_t at = 0; at < full.size(); ++at) {
        if (full[at] == '\r' && at + 1 < full.size() && full[at + 1] == '\n') continue;

        made.push_back(full[at]);
    }

    return made;
}

// Opens the clipboard, which another process may hold for a moment: a few immediate tries, since
// sleeping would hold the program.
bool opened() {
    for (int which = 0; which < 8; ++which)
        if (OpenClipboard(nullptr) != 0) return true;

    return false;
}

// Global memory holding a copy of `much` bytes from `from`, or null when the system has none.
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

// Hands `room` to the open clipboard as `format`; true when it took it, and then the memory is the
// system's. Refused, the memory is still ours and freed.
bool placed(UINT format, HGLOBAL room) {
    if (room == nullptr) return false;

    if (SetClipboardData(format, room) != nullptr) return true;

    GlobalFree(room);

    return false;
}

// Puts the text on the system clipboard; true when it took it. Every way out closes the clipboard:
// left open, it blocks the clipboard of the whole desktop.
bool copied(std::string_view what) {
    const std::wstring wide = widened(what);

    if (!opened()) return false;

    if (EmptyClipboard() == 0) {
        CloseClipboard();

        return false;
    }

    const bool went = placed(CF_UNICODETEXT, lent(wide.c_str(), (wide.size() + 1) * sizeof(wchar_t)));

    CloseClipboard();

    return went;
}

// The text on the system clipboard, an empty inner value when it holds none; nothing when it would
// not open, and then the program's own copy answers.
std::optional<std::optional<std::string>> pasted() {
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

// The format in which Explorer tells a cut from a copy: a DWORD with DROPEFFECT_MOVE or _COPY.
UINT effected() { return RegisterClipboardFormatW(L"Preferred DropEffect"); }

// A path as other programs take it: wide, with backslashes.
std::wstring preferred(const Path& one) {
    std::wstring made = text::wide(one.text());

    std::replace(made.begin(), made.end(), L'/', L'\\');

    return made;
}

// The files as a CF_HDROP block: the DROPFILES head, the wide paths each ended by NUL, one more
// NUL.
std::vector<std::byte> packed(const Handed& what) {
    std::wstring names;

    for (const Path& one : what.paths) {
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

// The names in a stretch of a CF_HDROP block, separated by NUL and closed by an empty one; a name
// the block cuts off is left out.
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

// A name in the ANSI code page, which an old program's CF_HDROP block may carry, as UTF-8.
std::string ansi(const std::string& one) {
    const int much = MultiByteToWideChar(CP_ACP, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);

    if (much <= 0) return {};

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    (void)MultiByteToWideChar(CP_ACP, 0, one.data(), static_cast<int>(one.size()), made.data(), much);

    return text::narrow(made);
}

// The paths in a CF_HDROP block of `much` bytes, wide or ANSI as its head says. The block comes
// from another process, so nothing past its size is read, whatever the head claims.
std::vector<Path> unpacked(const std::byte* block, std::size_t much) {
    std::vector<Path> made;

    if (much < sizeof(DROPFILES)) return made;

    DROPFILES head{};

    std::memcpy(&head, block, sizeof(DROPFILES));

    if (head.pFiles < sizeof(DROPFILES) || head.pFiles >= much) return made;

    const std::byte* names = block + head.pFiles;
    const std::size_t left = much - head.pFiles;

    if (head.fWide != FALSE) {
        std::wstring all(left / sizeof(wchar_t), L'\0');

        std::memcpy(all.data(), names, all.size() * sizeof(wchar_t));

        for (const std::wstring& one : split(std::wstring_view{all})) made.emplace_back(text::narrow(one));
    } else {
        const std::string all(reinterpret_cast<const char*>(names), left);

        for (const std::string& one : split(std::string_view{all}))
            if (std::string named = ansi(one); !named.empty()) made.emplace_back(named);
    }

    return made;
}

// Puts the files on the system clipboard as Explorer does: CF_HDROP, the drop effect (move for a
// cut) and the paths as text. True when the list and its effect were taken. An empty list takes the
// content away, but only while it is files.
bool dropped(const Handed& what) {
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

// Whether the drop effect on the open clipboard says move without copy: Explorer's mark of a cut.
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

// The files on the system clipboard and whether they were cut; an empty inner value when it holds
// none, nothing when it would not open.
std::optional<std::optional<Handed>> dragged() {
    if (!opened()) return std::nullopt;

    const HANDLE room = GetClipboardData(CF_HDROP);

    if (room == nullptr) {
        CloseClipboard();

        return std::optional<Handed>{};
    }

    Handed made;

    if (const auto* block = static_cast<const std::byte*>(GlobalLock(room)); block != nullptr) {
        made.paths = unpacked(block, GlobalSize(room));
        GlobalUnlock(room);
    }

    if (moved()) made.how = Handing::cut;

    CloseClipboard();

    if (made.paths.empty()) return std::optional<Handed>{};

    return std::optional<Handed>{std::move(made)};
}

// The clipboard of Windows itself, as `Kept` reaches it.
class Native final : public clipboarding::Board {
public:
    // As `copied`.
    [[nodiscard]] bool copy(std::string_view what) override { return copied(what); }

    // As `pasted`.
    [[nodiscard]] std::optional<std::optional<std::string>> paste() override { return pasted(); }

    // As `dropped`.
    [[nodiscard]] bool give(const Handed& what) override { return dropped(what); }

    // As `dragged`.
    [[nodiscard]] std::optional<std::optional<Handed>> take() override { return dragged(); }
};

#endif

// What the system clipboard keeps between calls: its ports, the clipboard of the system itself
// when it has one (`board`, Windows'; none on POSIX, where the session's clipboard programs stand
// for it), and the program's own copy, which answers when the system's clipboard is out of reach,
// so that cutting and pasting inside the program always works.
struct Kept {
    Port<Console> console;
    Port<Processes> processes;
    std::shared_ptr<clipboarding::Board> board;
    std::optional<std::string> mine;
    std::optional<Handed> files;
};

// Copies `what`: keeps it as the own copy, then, locally, hands it to the system's clipboard
// (`board`) or the session's clipboard program, else asks the terminal. Whether it reached one.
Task<bool> copying(std::shared_ptr<Kept> kept, std::string what) {
    kept->mine = what;
    kept->files.reset();

    Console& out = *kept->console;

    if (!remote(out)) {
        if (kept->board) co_return kept->board->copy(what);

        if (const std::optional<std::string> line = copier(out); line && co_await handed(kept->processes.shared(), *line, what))
            co_return true;
    }

    co_return sent(out, what);
}

// The text to paste: locally the system clipboard's (`board`), where it opens; otherwise the own
// copy. The terminal is not asked: its answer would pour into the program as keys, and a paste from
// outside comes from the terminal's own key as a paste event.
Task<std::optional<std::string>> pasting(std::shared_ptr<Kept> kept) {
    if (kept->board && !remote(*kept->console))
        if (std::optional<std::optional<std::string>> got = kept->board->paste(); got) co_return std::move(*got);

    co_return kept->mine;
}

// Gives the files as copied or cut, their paths also as text: locally to the system's clipboard
// (`board`) or to a clipboard program that carries types, and to the own copy always. True
// when they reached the system's clipboard. A relative path refuses the whole list and changes
// nothing. An empty list takes away files given before, and only files.
Task<bool> giving(std::shared_ptr<Kept> kept, Handed what) {
    if (!portable(what)) co_return false;

    if (what.paths.empty()) {
        if (kept->files) kept->mine.reset();

        kept->files.reset();
    } else {
        kept->files = what;
        kept->mine = spelled(what);
    }

    Console& out = *kept->console;

    if (remote(out)) co_return false;

    if (kept->board) co_return kept->board->give(what);

    co_return co_await handed(kept->processes.shared(), giver(out, what.paths.empty()), uris(what));
}

// The files to paste and how. Locally the system's clipboard answers: `board` (on Windows its
// CF_HDROP), else what the typed clipboard program reads, GNOME's mark of a cut first, then the
// plain list (a copy, unless it is exactly the list the program cut itself). A system clipboard
// that holds something else gives nothing: an older own copy must not paste over what was copied
// since. The own copy answers only when the system's clipboard is out of reach.
Task<std::optional<Handed>> taking(std::shared_ptr<Kept> kept) {
    Console& out = *kept->console;

    if (remote(out)) co_return kept->files;

    if (kept->board) {
        if (std::optional<std::optional<Handed>> got = kept->board->take(); got) co_return std::move(*got);

        co_return kept->files;
    }

    const Fetched marked = co_await fetched(kept->processes.shared(), reader(out, "x-special/gnome-copied-files"));

    if (!marked.present) co_return kept->files;

    if (marked.text) {
        const std::size_t end = std::min(marked.text->find('\n'), marked.text->size());
        const std::string_view verb = std::string_view{*marked.text}.substr(0, end);
        Handed made;

        made.paths = locating::listed(std::string_view{*marked.text}.substr(std::min(end + 1, marked.text->size())));
        made.how = verb == "cut" ? Handing::cut : Handing::copy;

        if ((verb == "cut" || verb == "copy") && !made.paths.empty()) co_return made;
    }

    const Fetched plain = co_await fetched(kept->processes.shared(), reader(out, "text/uri-list"));

    if (!plain.text) co_return std::nullopt;

    Handed made;

    made.paths = locating::listed(*plain.text);

    if (made.paths.empty()) co_return std::nullopt;

    if (kept->files && kept->files->paths == made.paths) made.how = kept->files->how;

    co_return made;
}

// The clipboard of the machine the user sits at, through `Kept`.
class System final : public Clipboard {
public:
    System(Port<Console> console, Port<Processes> processes, std::shared_ptr<clipboarding::Board> board)
        : kept_(std::make_shared<Kept>(Kept{std::move(console), std::move(processes), std::move(board), std::nullopt, std::nullopt})) {}

    // Its name in messages.
    [[nodiscard]] std::string_view name() const override { return "system"; }

    // As `copying`.
    [[nodiscard]] Task<bool> copy(std::string what) override { return copying(kept_, std::move(what)); }

    // As `pasting`.
    [[nodiscard]] Task<std::optional<std::string>> paste() override { return pasting(kept_); }

    // As `giving`.
    [[nodiscard]] Task<bool> give(Handed what) override { return giving(kept_, std::move(what)); }

    // As `taking`.
    [[nodiscard]] Task<std::optional<Handed>> taken() override { return taking(kept_); }

private:
    std::shared_ptr<Kept> kept_;
};

}

// The system clipboard over the default console and processes.
std::shared_ptr<Clipboard> Clipboard::builtin() { return std::make_shared<System>(Port<Console>{}, Port<Processes>{}, clipboarding::native()); }

// A clipboard that carries no files: giving them fails.
Task<bool> Clipboard::give(Handed what) {
    (void)what;

    co_return false;
}

// A clipboard that carries no files: there are none to take.
Task<std::optional<Handed>> Clipboard::taken() { co_return std::nullopt; }

// The clipboard of the machine the user sits at: the system's on Windows, the session's clipboard
// program on POSIX, the terminal through `console` as the last way to copy, and the program's own
// copy for pasting inside it. Programs run through `processes`.
Port<Clipboard> clipboard(Port<Console> console, Port<Processes> processes) {
    return clipboarding::clipboard(std::move(console), std::move(processes), clipboarding::native());
}

// The clipboard of Windows itself; none elsewhere, where the session's clipboard programs stand for
// it.
std::shared_ptr<clipboarding::Board> clipboarding::native() {
#ifdef _WIN32
    return std::make_shared<Native>();
#else
    return nullptr;
#endif
}

// The clipboard of the machine the user sits at with `board` as the system's own clipboard, or
// with none: the trials give the logic of both systems a board of their own, and never the real
// one.
Port<Clipboard> clipboarding::clipboard(Port<Console> console, Port<Processes> processes, std::shared_ptr<Board> board) {
    return Port<Clipboard>{std::make_shared<System>(std::move(console), std::move(processes), std::move(board))};
}

// Keeps `what` as copied and, when it takes copies, as what a paste gives.
Task<bool> ScriptedClipboard::copy(std::string what) {
    copied.push_back(what);

    if (takes) held = std::move(what);

    co_return takes;
}

// What it holds.
Task<std::optional<std::string>> ScriptedClipboard::paste() { co_return held; }

// Keeps the files when it takes them.
Task<bool> ScriptedClipboard::give(Handed what) {
    if (takes) files = std::move(what);

    co_return takes;
}

// The files it holds.
Task<std::optional<Handed>> ScriptedClipboard::taken() { co_return files; }

}

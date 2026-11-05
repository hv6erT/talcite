#include "talcite/terminal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/skin.hpp"

#include "xterming.hpp"

// A terminal in a node: a program in a pseudoterminal (`Pty`), its screen (`Emulator`), and the
// node that draws that screen, gives the program its keys, its pastes and its mouse, and tells the
// tree what the program told the terminal (`FolderEvent`, `MarkEvent`, `SwitchEvent`,
// `EndEvent`). What the program writes is read on the pool and fed to the screen on
// the loop, which then draws a new frame; what goes to the program goes in the order it was given,
// one write after another. The end comes after what the program wrote, never before it, and after
// the end the terminal tells nothing more.
namespace talcite::ui {

namespace {

// How much one read asks for: a screenful of a busy program in one go.
constexpr std::size_t chunk = 64 * 1024;

// How long the end waits, after the program's exit, for the rest of what it wrote. Output and exit
// come on their own threads, so the exit may come first. When the program was the terminal's only
// user, the end of the output follows at once, and the end goes then. A program it left behind
// may hold the terminal open for as long as it likes; this much later the end goes anyway.
constexpr std::chrono::milliseconds lingering{500};

// Appends text to bytes.
void added(Bytes& into, std::string_view said) {
    for (const char each : said) into.push_back(static_cast<std::byte>(each));
}

}

// A terminal for the program `what`, with a screen of `size` and `kept` lines of history. It does
// not start the program: `run` does, once the node stands in a tree that can wait for it.
Terminal::Terminal(Command what, Size size, std::size_t kept) : command_(std::move(what)), emulator_(size, kept) {
    role("talcite.terminal");
}

// Hangs the terminal up as it goes, as closing a terminal window does: a shell left without its
// terminal ends, and with it what it ran in the foreground.
Terminal::~Terminal() {
    if (pty_.held()) pty_.hang_up();
}

// The terminal takes the focus: its keys are the program's.
bool Terminal::focusable() const { return true; }

// As much room as it gets: a terminal fills its place.
Size Terminal::wants(Size room) const { return room; }

// Starts the program in a pseudoterminal of the screen's size, once; then reads what it writes and
// waits for its end, which comes once both are over (`exited`, `drained`). A program that cannot be
// started ends at once, with `Ended::lost`.
void Terminal::run() {
    if (running_ || starting_) return;

    starting_ = true;

    const std::weak_ptr<Node> weak = weak_from_this();

    started_ = Pty::start(command_, emulator_.size())
                   .here()
                   .then([weak](Pty got) {
                       const std::shared_ptr<Node> kept = weak.lock();

                       if (!kept) return;

                       auto& self = static_cast<Terminal&>(*kept);

                       self.pty_ = std::move(got);
                       self.running_ = true;
                       self.starting_ = false;
                       self.exit_.reset();
                       self.drained_ = false;

                       self.reading_ = self.listened();

                       self.waiting_ = self.pty_.wait().here().then([weak](const Exit& exit) {
                           if (const std::shared_ptr<Node> alive = weak.lock()) static_cast<Terminal&>(*alive).exited(exit);
                       });

                       if (!self.outbox_.empty()) self.flushed();
                   })
                   .recover([weak](const std::exception&) {
                       if (const std::shared_ptr<Node> kept = weak.lock()) {
                           auto& self = static_cast<Terminal&>(*kept);

                           self.starting_ = false;

                           self.ended(Exit{Ended::lost, 0, 0});
                       }
                   });
}

// Reads what the program writes, one chunk; the chunk comes to the loop and goes to the screen
// there (`heard`), and the next read takes this one's place in `reading_`. A read that returned the
// next one would make a chain of every chunk ever read, held until the output ends. The end of the
// output, or a read that fails, drains the terminal (`drained`).
Promise<void> Terminal::listened() {
    const std::weak_ptr<Node> weak = weak_from_this();

    return pty_.read(chunk)
        .here()
        .then([weak](const Bytes& got) {
            const std::shared_ptr<Node> kept = weak.lock();

            if (!kept) return;

            auto& self = static_cast<Terminal&>(*kept);

            if (got.empty()) {
                self.drained();

                return;
            }

            self.heard(got);

            self.reading_ = self.listened();
        })
        .recover([weak](const std::exception&) {
            if (const std::shared_ptr<Node> alive = weak.lock()) static_cast<Terminal&>(*alive).drained();
        });
}

// A chunk of what the program wrote: onto the screen, the terminal's answers back to the program,
// and what the program told the terminal up the tree. New output brings the view back from the
// history to the live screen. After the end, what a program left behind still writes only goes onto
// the screen: nobody answers it, and the tree has already heard the end.
void Terminal::heard(const Bytes& got) {
    const std::string was = emulator_.folder();

    Fed fed = emulator_.feed(got);

    scrolled_ = 0;

    changed();

    if (!running_) return;

    if (!fed.replies.empty()) write(std::move(fed.replies));

    if (fed.switched) {
        SwitchEvent told{emulator_.modes().alternate};

        (void)trigger(told);
    }

    if (fed.moved && emulator_.folder() != was) {
        FolderEvent told{emulator_.folder()};

        (void)trigger(told);
    }

    for (const ShellMark& one : fed.marks) {
        if (one.kind == Marked::prompt) submitted_ = false;

        MarkEvent told{one};

        (void)trigger(told);
    }
}

// The program has exited. Its end goes once what it wrote has been read (`drained`), or after a
// while (`lingering`) when something it left behind keeps the terminal open.
void Terminal::exited(const Exit& exit) {
    exit_ = exit;

    if (drained_) {
        ended(exit);

        return;
    }

    const std::weak_ptr<Node> weak = weak_from_this();

    lingering_ = later(lingering).here().then([weak] {
        const std::shared_ptr<Node> kept = weak.lock();

        if (!kept) return;

        auto& self = static_cast<Terminal&>(*kept);

        if (self.running_) self.ended(*self.exit_);
    });
}

// The output is over: its end was read, or reading it failed. A program that has already exited
// ends now; otherwise its exit brings the end.
void Terminal::drained() {
    drained_ = true;

    if (exit_ && running_) ended(*exit_);
}

// The program has ended: the terminal no longer runs, and the tree hears of it.
void Terminal::ended(const Exit& exit) {
    running_ = false;
    submitted_ = false;

    EndEvent told{exit};

    (void)trigger(told);
}

// Text for the program, as if typed: a command, a key the shell knows. Before the program runs it
// waits, and goes when it does.
void Terminal::send(std::string_view text) {
    Bytes made;

    added(made, text);

    write(std::move(made));
}

// A command line for the shell: the line and Enter. Until the shell marks its next prompt, it is
// busy with it (`idle`), even a shell that marks no command's start.
void Terminal::submit(std::string_view line) {
    std::string made{line};

    made += '\r';

    submitted_ = true;
    scrolled_ = 0;

    changed();

    send(made);
}

// Whether the shell waits at its prompt with nothing given to it since: it marked the prompt (OSC
// 133), no command line went since (`submit`, or Enter typed into the terminal at the prompt), and
// it marked no command's start. Only then may someone else type for it.
bool Terminal::idle() const { return running_ && emulator_.waiting() && !submitted_; }

// Bytes for the program, behind what waits to go. One write goes at a time, so the program gets the
// bytes in their order, however the pool runs its writers.
void Terminal::write(Bytes what) {
    outbox_.insert(outbox_.end(), what.begin(), what.end());

    if (!writing_ && running_) flushed();
}

// Sends what waits, and again whatever came meanwhile, until nothing waits. A write that fails (the
// program is gone) drops what waits.
void Terminal::flushed() {
    if (outbox_.empty() || !running_) return;

    writing_ = true;

    const std::weak_ptr<Node> weak = weak_from_this();

    // A chain of writes, each with what waited when the one before it ended.
    struct Pouring {
        // The next write, or the end of the chain.
        static Promise<void> next(std::weak_ptr<Node> weak) {
            const std::shared_ptr<Node> kept = weak.lock();

            if (!kept) return ready();

            auto& self = static_cast<Terminal&>(*kept);

            if (self.outbox_.empty() || !self.running_) {
                self.writing_ = false;

                return ready();
            }

            Bytes going = std::move(self.outbox_);

            self.outbox_.clear();

            return self.pty_.write(std::move(going)).here().then([weak](std::size_t) { return next(weak); });
        }
    };

    sending_ = Pouring::next(weak).recover([weak](const std::exception&) {
        if (const std::shared_ptr<Node> kept = weak.lock()) {
            auto& self = static_cast<Terminal&>(*kept);

            self.outbox_.clear();
            self.writing_ = false;
        }
    });
}

// Hangs the terminal up, as closing its window does; the end comes as `EndEvent`.
void Terminal::hang_up() {
    if (pty_.held()) pty_.hang_up();
}

// The bytes xterm writes for a key, in the program's modes: the cursor keys and the keypad in
// application mode when the program asked for it, Alt as a leading escape, Ctrl with a letter as
// its control character. A key xterm has no bytes for gives none.
Bytes Terminal::keyed(Key key, const Modes& modes) {
    Bytes made;

    const Mods mods = key.mods();

    if (const char32_t letter = key.letter(); letter != 0) {
        if (mods.alt) added(made, "\x1b");

        if (mods.ctrl) {
            const char32_t lower = letter >= U'A' && letter <= U'Z' ? letter - U'A' + U'a' : letter;

            if (lower >= U'a' && lower <= U'z') made.push_back(static_cast<std::byte>(lower - U'a' + 1));
            else if (lower == U' ' || lower == U'@' || lower == U'2') made.push_back(std::byte{0});
            else if (lower == U'[' || lower == U'3') made.push_back(std::byte{0x1B});
            else if (lower == U'\\' || lower == U'4') made.push_back(std::byte{0x1C});
            else if (lower == U']' || lower == U'5') made.push_back(std::byte{0x1D});
            else if (lower == U'^' || lower == U'6') made.push_back(std::byte{0x1E});
            else if (lower == U'_' || lower == U'7' || lower == U'/') made.push_back(std::byte{0x1F});
            else if (lower == U'?' || lower == U'8') made.push_back(std::byte{0x7F});
            else added(made, utf8::encode(letter));

            return made;
        }

        added(made, utf8::encode(letter));

        return made;
    }

    if (const std::string named = xterm::named(key.code(), xterm::modifier(mods.shift, mods.alt, mods.ctrl), modes.cursor_keys);
        !named.empty()) {
        added(made, named);

        return made;
    }

    // F13–F20 are F1–F8 with Shift, as xterm counts them.
    const auto code = static_cast<int>(key.code());
    const auto first = static_cast<int>(Code::f13);

    if (code >= first && code <= static_cast<int>(Code::f20)) {
        Mods shifted = mods;

        shifted.shift = true;

        return keyed(Key{static_cast<Code>(static_cast<int>(Code::f1) + code - first), shifted}, modes);
    }

    return made;
}

// A key for the program. Shift with Page Up and Page Down walks the history instead, while the
// primary screen is shown, and any other key brings the view back to the live screen first. A key
// without bytes (a key of the screen, not of the terminal) goes up the tree.
bool Terminal::handle(KeyEvent& what) {
    const Key key = what.key();
    const Mods mods = key.mods();

    if (mods.shift && !mods.ctrl && !mods.alt && (key.code() == Code::pageup || key.code() == Code::pagedown) &&
        !emulator_.modes().alternate) {
        const int step = std::max(emulator_.size().high - 1, 1);

        scrolled_ = std::clamp(scrolled_ + (key.code() == Code::pageup ? step : -step), 0, deepest());

        changed();

        return true;
    }

    Bytes made = keyed(key, emulator_.modes());

    if (made.empty() || !running_) return false;

    scrolled_ = 0;

    changed();

    // Enter at the prompt gives the shell its command line, as `submit` does.
    if (key.code() == Code::enter && emulator_.waiting()) submitted_ = true;

    write(std::move(made));

    return true;
}

// A paste goes to the program as typed text, between the markers of a bracketed paste when the
// program asked for them, so it can tell a paste from typing. Line ends go as carriage returns, as
// Enter does.
bool Terminal::handle(PasteEvent& what) {
    if (!running_) return false;

    std::string text;

    for (std::size_t at = 0; at < what.text().size(); ++at) {
        const char one = what.text()[at];

        if (one == '\r' && at + 1 < what.text().size() && what.text()[at + 1] == '\n') continue;

        text += one == '\n' ? '\r' : one;
    }

    Bytes made;

    if (emulator_.modes().paste) {
        added(made, "\x1b[200~");
        added(made, text);
        added(made, "\x1b[201~");
    } else {
        added(made, text);
    }

    scrolled_ = 0;

    changed();

    write(std::move(made));

    return true;
}

// The mouse goes to a program that asked for it, in xterm's SGR encoding: presses and releases
// always, drags when it asked for them, every move when it asked for that. Otherwise the mouse is
// the screen's.
bool Terminal::handle(MouseEvent& what) {
    const Modes& modes = emulator_.modes();

    if (!running_ || modes.mouse == Mousing::none || !modes.sgr_mouse) return false;

    int button = xterm::mouse_none;

    switch (what.button()) {
        case Mouse::left: button = xterm::mouse_left; break;
        case Mouse::middle: button = xterm::mouse_middle; break;
        case Mouse::right: button = xterm::mouse_right; break;
        default: break;
    }

    if (what.press() == Press::move) {
        const bool dragging = what.button() != Mouse::none && what.button() != Mouse::back && what.button() != Mouse::forward;

        if (modes.mouse == Mousing::clicks || (modes.mouse == Mousing::drags && !dragging)) return true;

        button += xterm::mouse_motion;
    }

    button += xterm::mouse_mods(what.mods().shift, what.mods().alt, what.mods().ctrl);

    Bytes made;

    added(made, xterm::sgr(button, local(what.where()), what.press() == Press::up));

    write(std::move(made));

    return true;
}

// The wheel: to a program that asked for the mouse, as its wheel buttons; on the alternate screen
// of one that did not, as the arrow keys, which is how full-screen programs like less are scrolled
// in a terminal window; otherwise it walks the history.
bool Terminal::handle(WheelEvent& what) {
    if (what.axis() != Axis::vertical || what.steps() == 0) return false;

    const Modes& modes = emulator_.modes();
    const bool up = what.steps() < 0;
    const int many = std::abs(what.steps());

    if (running_ && modes.mouse != Mousing::none && modes.sgr_mouse) {
        const Spot at = local(what.where());

        Bytes made;

        for (int turn = 0; turn < many; ++turn) added(made, xterm::sgr(up ? xterm::wheel_up : xterm::wheel_down, at, false));

        write(std::move(made));

        return true;
    }

    if (modes.alternate) {
        if (!running_) return false;

        Bytes made;

        for (int turn = 0; turn < many * 3; ++turn) {
            const Bytes one = keyed(Key{up ? Code::up : Code::down}, modes);

            made.insert(made.end(), one.begin(), one.end());
        }

        write(std::move(made));

        return true;
    }

    scrolled_ = std::clamp(scrolled_ + (up ? 3 : -3) * many, 0, deepest());

    changed();

    return true;
}

// Tells a program that asked for it that the terminal has the focus.
bool Terminal::handle(FocusEvent&) {
    if (running_ && emulator_.modes().focus) send("\x1b[I");

    return false;
}

// Tells a program that asked for it that the terminal has lost the focus.
bool Terminal::handle(UnfocusEvent&) {
    if (running_ && emulator_.modes().focus) send("\x1b[O");

    return false;
}

// The row under the last one shown: the bottom of the screen, or the row where the prompt the shell
// waits at begins, when the prompt is not to be shown, so the picture ends just above it.
int Terminal::bottom() const {
    const int high = emulator_.size().high;

    if (prompt_shown_) return high;

    return emulator_.prompt().value_or(high);
}

// How far back the view can be walked: until the oldest line of the history stands at the top. A
// hidden prompt lifts the picture by the rows under it, so as many rows of history are shown
// already.
int Terminal::deepest() const {
    return std::max(static_cast<int>(emulator_.history()) - (emulator_.size().high - bottom()), 0);
}

// A click takes the focus, as a click into a terminal window does; a program that asked for the
// mouse has had the click already (`MouseEvent`).
bool Terminal::handle(ClickEvent&) {
    focus();

    return true;
}

// The cursor's shape: the one the program asked for (DECSCUSR), as long as it asks for one, and
// otherwise the one this terminal was given (`caret_shape`), as a terminal window keeps its own
// setting for programs that ask for nothing.
CaretShape Terminal::caret_shape() const {
    const CaretShape asked = emulator_.modes().caret;

    return asked.form != Caret::usual ? asked : Node::caret_shape();
}

// The cursor of the program, while the live screen is shown with the cursor in it and the program
// has not hidden it.
std::optional<Spot> Terminal::caret() const {
    if (!focused() || scrolled_ != 0 || !emulator_.modes().cursor) return {};

    const int shift = emulator_.size().high - bottom();
    const Spot at = emulator_.cursor();

    if (at.y + shift >= emulator_.size().high) return {};

    return Spot{at.x, at.y + shift};
}

// The node's size is the terminal's: a node laid out larger or smaller gives the screen and the
// program the new size first, since the program has to fit its picture to what is seen.
void Terminal::layout(Size given) {
    if (given.wide > 0 && given.high > 0 && given != emulator_.size()) {
        emulator_.resize(given);

        if (pty_.held()) pty_.resize(given);
    }
}

// Draws the screen. The rows drawn end at `bottom` and come from the history above it as far as
// needed, and as far as the view has been walked back. The default colors are those of the node's
// role.
void Terminal::paint(Canvas& onto) const {
    const Size given = onto.size();

    const Style base = skin().style(role());

    onto.fill({{0, 0}, given}, base);

    const int high = emulator_.size().high;
    const int first = bottom() - high - scrolled_;

    for (int y = 0; y < high; ++y) {
        const Emulator::Line& line = emulator_.line(first + y);

        for (int x = 0; x < static_cast<int>(line.size()); ++x) {
            const Cell& one = line[static_cast<std::size_t>(x)];

            if (one.letter == 0) continue;

            onto.put({x, y}, one.letter, Style::merge(base, one.style));
        }
    }
}

}

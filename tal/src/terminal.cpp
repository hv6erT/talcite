#include "tal/terminal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/text.hpp"

#include "controlling.hpp"
#include "xterming.hpp"

// A terminal in a node: a program in a pseudoterminal, started through the port of processes, its
// screen (`Emulator`), and the view that draws the screen, gives the program its keys, its pastes
// and its mouse, lets the person walk back through the history and select text, and tells the tree
// what the program told the terminal (`Moved`, `Marked`, `Switched`, `Titled`, `Rang`, `Finished`).
// What the program writes is read in the terminal's scope and fed to the screen on the loop, which
// draws the frame once for all that came; what goes to the program goes in the order it was given,
// one write after another. The end comes after what the program wrote, never before it, and after
// the end the terminal tells nothing more.
namespace tal {

namespace {

// How much one read asks for: a screenful of a busy program in one go.
constexpr std::size_t chunk = 64 * 1024;

// How long the end waits, after the program's exit, for the rest of what it wrote. Output and exit
// come on their own ways, so the exit may come first. When the program was the terminal's only
// user, the end of the output follows at once, and the end goes then. A program it left behind may
// hold the terminal open for as long as it likes; this much later the end goes anyway.
constexpr std::chrono::milliseconds lingering{500};

// How long a program that draws (synchronized output, mode 2026) may hold the picture back.
constexpr std::chrono::milliseconds holding{200};

// The most bytes waiting to go to the program.
constexpr std::size_t most_waiting = std::size_t{1} << 20;

// How many rows a step of the wheel walks the history or moves a full-screen program.
constexpr int wheeled = 3;

// Appends text to bytes.
void added(Bytes& into, std::string_view said) {
    for (const char each : said) into.push_back(static_cast<std::byte>(each));
}

// Whether the first character of `cluster` belongs to a word.
bool wordy(std::string_view cluster) {
    std::size_t at = 0;
    const std::optional<char32_t> one = text::decoded(cluster, at);

    return one && text::in_word(*one);
}

}

// The bytes xterm writes for a key, in the program's modes: the cursor keys and the keypad in
// application mode when the program asked for it, Alt as a leading escape, Ctrl with a letter as
// its control character. F13–F20 are F1–F8 with Shift, as xterm counts them. A key xterm has no
// bytes for gives none.
Bytes keyed(Key key, const Modes& modes) {
    Bytes made;
    const Mods mods = key.mods();

    if (const char32_t letter = key.letter(); letter != 0) {
        std::string spelled;

        text::encoded(spelled, letter);

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
            else added(made, spelled);

            return made;
        }

        added(made, spelled);

        return made;
    }

    if (const std::string named = xterm::named(key.code(), xterm::modifier(mods.shift, mods.alt, mods.ctrl), modes.cursor_keys); !named.empty()) {
        added(made, named);

        return made;
    }

    const auto code = static_cast<int>(key.code());
    const auto first = static_cast<int>(Code::f13);

    if (code >= first && code <= static_cast<int>(Code::f20)) {
        Mods shifted = mods;

        shifted.shift = true;

        return keyed(Key{static_cast<Code>(static_cast<int>(Code::f1) + code - first), shifted}, modes);
    }

    return made;
}

// A terminal for the program `program`, in the class "terminal", with `kept` lines of history; the
// screen takes the size the command gives until the layout gives its own. It does not start the
// program: `run` does. Keys, pastes and the mouse are its default actions, so a program's own
// bindings above it come first; the focus coming and going is told to a program that asked. Files
// dropped on it while its program runs reach the program as the paste the outer terminal wrote,
// before the nodes above see the drop.
Terminal::Terminal(Command program, std::size_t kept, Port<Processes> processes, Port<Clipboard> clipboard)
    : command_(std::move(program)),
      emulator_(Size{command_.columns, command_.rows}, kept),
      processes_(std::move(processes)),
      clipboard_(std::move(clipboard)) {
    command_.mode = Mode::terminal;

    classes({"terminal"});
    focusable(true);

    hooks_.add(on<Changed>([this](Changed& what) {
        if (!what.of(prompt)) return;

        scrolled_ = std::clamp(scrolled_, 0, deepest());
        scrolled();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) { typed(what); }));

    hooks_.add(otherwise<PasteEvent>([this](PasteEvent& what) {
        if (running_) {
            (void)paste(what.text);
            what.take();
        }
    }));

    hooks_.add(on<DropEvent>([this](DropEvent& what) {
        if (&what.target() != this || !running_) return;

        (void)paste(what.text);
        what.take();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) { moused(what); }));

    hooks_.add(on<Focused>([this](Focused&) {
        if (emulator_.modes().focus) send("\x1b[I");
    }));

    hooks_.add(on<Blurred>([this](Blurred&) {
        if (emulator_.modes().focus) send("\x1b[O");
    }));
}

// Hangs the terminal up as it goes, as closing a terminal window does: a shell left without its
// terminal ends, and with it what it ran.
Terminal::~Terminal() {
    if (process_.held()) process_.hang_up();
}

// Starts the program in a pseudoterminal of the screen's size, once; then reads what it writes and
// waits for its end, which comes once both are over (`exited`, `drained`). A program that cannot be
// started ends at once, as lost.
void Terminal::run() {
    if (running_ || starting_ || process_.held()) return;

    starting_ = true;
    scope_.run(started());
}

// The start itself: the program, its reading and its waiting, and what was given to it meanwhile.
Task<void> Terminal::started() {
    Command what = command_;
    const Size asked = emulator_.size();

    what.columns = asked.wide;
    what.rows = asked.high;

    std::optional<Process> made;

    try {
        made = co_await processes_->start(std::move(what));
    } catch (const std::exception&) {
        made.reset();
    }

    starting_ = false;

    if (!made) {
        ended(Exit{Exit::Ended::lost, 0, 0});

        co_return;
    }

    process_ = std::move(*made);
    running_ = true;

    if (emulator_.size() != asked) process_.resize(emulator_.size().wide, emulator_.size().high);

    scope_.run(listened());
    scope_.run(waited());

    if (!outbox_.empty() && !writing_) {
        writing_ = true;
        scope_.run(poured());
    }
}

// Reads what the program writes, a chunk at a time, onto the screen (`heard`). The end of the
// output, or a read that fails, drains the terminal (`drained`).
Task<void> Terminal::listened() {
    try {
        for (;;) {
            const Bytes got = co_await process_.output().read(chunk);

            if (got.empty()) break;

            heard(got);
        }
    } catch (const std::exception&) {
    }

    drained();
}

// Waits for the program's exit (`exited`); a wait that fails counts as lost.
Task<void> Terminal::waited() {
    Exit made{Exit::Ended::lost, 0, 0};

    try {
        made = co_await process_.ended();
    } catch (const std::exception&) {
    }

    exited(made);
}

// The end, `lingering` after the exit, when the output has not ended by then.
Task<void> Terminal::lingered() {
    co_await after(lingering);

    if (running_ && exit_) ended(*exit_);
}

// Sends what waits, and again whatever came meanwhile, until nothing waits. A write that fails (the
// program no longer reads) loses what it carried.
Task<void> Terminal::poured() {
    try {
        while (!outbox_.empty() && running_) {
            Bytes going = std::move(outbox_);

            outbox_.clear();

            co_await process_.input().write(std::move(going));
        }
    } catch (const std::exception&) {
    }

    writing_ = false;
}

// The picture held back for a drawing program: shown when the program has not finished drawing
// `holding` after it began (`which` counts the holds, so that a hold ended and begun anew is not
// cut short by the timer of the one before).
Task<void> Terminal::held(std::uint64_t which) {
    co_await after(holding);

    if (!holding_ || which != hold_) co_return;

    holding_ = false;
    repaint();
}

// A chunk of what the program wrote: onto the screen, the terminal's answers back to the program,
// and what the program told the terminal up the tree. New output brings the view back from the
// history to the live screen, and the selection, which stays by its text, along. A program drawing
// in synchronized output holds the picture back (`held`). After the end, what a program left
// behind still writes only goes onto the screen: nobody answers it, and the tree has already heard
// the end.
void Terminal::heard(const Bytes& got) {
    const bool alternate = emulator_.modes().alternate;
    Fed fed = emulator_.feed(got);

    scrolled_ = 0;

    if (fed.switched || emulator_.modes().alternate != alternate) unselect();

    if (emulator_.modes().synchronized) {
        if (!holding_) {
            holding_ = true;
            scope_.run(held(++hold_));
        }
    } else {
        holding_ = false;
        repaint();
    }

    scrolled();

    if (!running_) return;

    if (!fed.replies.empty()) (void)write(std::move(fed.replies));

    if (fed.switched) {
        Switched told{emulator_.modes().alternate};

        notified(told);
    }

    if (fed.moved) {
        Moved told{emulator_.folder()};

        notified(told);
    }

    if (fed.titled) {
        Titled told{emulator_.title()};

        notified(told);
    }

    if (fed.bells > 0) {
        Rang told;

        notified(told);
    }

    for (const Staged& one : fed.stages) {
        if (one.stage == Stage::prompt) submitted_ = false;

        Marked told{one};

        notified(told);
    }
}

// The program has exited. Its end goes once what it wrote has been read (`drained`), or after a
// while (`lingering`) when something it left behind keeps the terminal open.
void Terminal::exited(Exit exit) {
    exit_ = exit;

    if (drained_) {
        ended(exit);

        return;
    }

    scope_.run(lingered());
}

// The output is over: its end was read, or reading it failed. A program that has already exited
// ends now; otherwise its exit brings the end.
void Terminal::drained() {
    drained_ = true;

    if (exit_ && running_) ended(*exit_);
}

// The program has ended: the terminal no longer runs, and the tree hears of it.
void Terminal::ended(Exit exit) {
    running_ = false;
    outbox_.clear();

    Finished told{exit};

    notified(told);
}

// Text for the program, as if typed: a command, a key the shell knows. Before the program runs it
// waits, and goes when it does.
void Terminal::send(std::string_view text) {
    Bytes made;

    added(made, text);

    (void)write(std::move(made));
}

// A command line for the shell: the line and Enter. Until the shell marks its next prompt, it is
// busy with it (`idle`), even a shell that marks no command's start.
void Terminal::submit(std::string_view line) {
    std::string made{line};

    made += '\r';

    submitted_ = true;
    scrolled_ = 0;

    repaint();
    scrolled();
    send(made);
}

// Text for the program as pasted: between the markers of a bracketed paste when the program asked
// for them, so it can tell a paste from typing, and line ends as carriage returns, as Enter gives
// them. The escapes and the other controls but the tab and the line end are taken out, so a paste
// cannot end its brackets early or give the program a key. A paste the queue to the program has no
// room for goes not at all; false then, and when the program does not run.
bool Terminal::paste(std::string_view text) {
    if (!running_) return false;

    std::string clean;

    for (std::size_t at = 0; at < text.size(); ++at) {
        const char one = text[at];

        if (one == '\r' && at + 1 < text.size() && text[at + 1] == '\n') continue;

        if (one == '\n' || one == '\r') clean += '\r';
        else if (one == '\t' || (static_cast<unsigned char>(one) >= 0x20 && one != 0x7F)) clean += one;
    }

    Bytes made;

    if (emulator_.modes().paste) added(made, "\x1b[200~");

    added(made, clean);

    if (emulator_.modes().paste) added(made, "\x1b[201~");

    if (!write(std::move(made))) return false;

    scrolled_ = 0;

    repaint();
    scrolled();

    return true;
}

// Whether the shell waits at its prompt with nothing given to it since: it marked the prompt (OSC
// 133), no command line went since (`submit`, or Enter typed into the terminal at the prompt), and
// it marked no command's start. Only then may someone else type for it.
bool Terminal::idle() const { return running_ && emulator_.waiting() && !submitted_; }

// Hangs the terminal up, as closing its window does; the end comes as `Finished`.
void Terminal::hang_up() {
    if (process_.held()) process_.hang_up();
}

// Bytes for the program, behind what waits to go; false when they would pass the bound of what
// may wait, and then nothing of them goes. One write goes at a time, so the program gets the bytes
// in their order.
bool Terminal::write(Bytes what) {
    if (outbox_.size() + what.size() > most_waiting) return false;

    outbox_.insert(outbox_.end(), what.begin(), what.end());

    if (!writing_ && running_) {
        writing_ = true;
        scope_.run(poured());
    }

    return true;
}

// A key for the program. Shift with Page Up and Page Down walks the history instead, while the
// primary screen is shown, and any other key brings the view back to the live screen first. A key
// without bytes (a key of the screen, not of the terminal) goes up the tree.
void Terminal::typed(KeyEvent& what) {
    const Key key = what.key;
    const Mods mods = key.mods();

    if (mods.shift && !mods.ctrl && !mods.alt && (key.code() == Code::pageup || key.code() == Code::pagedown) && !emulator_.modes().alternate) {
        const int step = std::max(emulator_.size().high - 1, 1);

        what.take();
        slid(key.code() == Code::pageup ? step : -step);

        return;
    }

    Bytes made = tal::keyed(key, emulator_.modes());

    if (made.empty() || !running_) return;

    what.take();

    // Enter at the prompt gives the shell its command line, as `submit` does.
    if (key.code() == Code::enter && emulator_.waiting()) submitted_ = true;

    if (!write(std::move(made))) return;

    if (scrolled_ != 0) {
        scrolled_ = 0;
        repaint();
        scrolled();
    }
}

// Walks the history by `rows` rows, back when positive, within what it holds.
void Terminal::slid(long long rows) {
    const int was = scrolled_;

    scrolled_ = static_cast<int>(std::clamp<long long>(scrolled_ + rows, 0, deepest()));

    if (scrolled_ == was) return;

    repaint();
    scrolled();
}

// The mouse. A program that asked for it gets it (`reported`), but with Shift held it is the
// screen's, as in xterm. The screen's mouse selects: a press begins, a drag extends, a double click
// takes a word; any press takes the focus. Ctrl with a click on a link tells `Followed`, which the
// document opens when nobody takes it. The wheel goes to a program that asked for the mouse, as
// the arrow keys to a full-screen program that did not, and otherwise walks the history.
void Terminal::moused(MouseEvent& what) {
    const Modes& modes = emulator_.modes();
    const bool asked = running_ && modes.mouse != Mousing::none && modes.sgr_mouse;

    if (what.press == Press::wheel) {
        if (what.axis != Axis::vertical || what.steps == 0) return;

        what.take();

        const bool up = what.steps > 0;
        const int many = std::abs(what.steps);

        if (asked) {
            Bytes made;

            for (int turn = 0; turn < many; ++turn) added(made, xterm::sgr(up ? xterm::wheel_up : xterm::wheel_down, what.at, false));

            (void)write(std::move(made));

            return;
        }

        if (modes.alternate) {
            if (!running_) return;

            Bytes made;

            for (int turn = 0; turn < many * wheeled; ++turn) {
                const Bytes one = tal::keyed(Key{up ? Code::up : Code::down}, modes);

                made.insert(made.end(), one.begin(), one.end());
            }

            (void)write(std::move(made));

            return;
        }

        slid(static_cast<long long>(up ? wheeled : -wheeled) * many);

        return;
    }

    if (what.press == Press::down) focus();

    if (asked && !what.mods.shift) {
        what.take();
        reported(what);

        return;
    }

    if (what.button != MouseButton::left) return;

    what.take();

    if (what.press == Press::click && what.mods.ctrl) {
        const Emulator::Row row = emulator_.row(first() + what.at.y);
        const std::size_t x = static_cast<std::size_t>(std::max(what.at.x, 0));
        const std::string_view address = x < row.cells.size() ? emulator_.address(row.cells[x].link) : std::string_view{};

        unselect();

        if (address.empty()) return;

        Followed one{std::string{address}};

        notified(one);

        controlling::unheeded(*this, one);

        return;
    }

    if (what.press == Press::down) {
        anchor_ = head_ = placed(what.at);
        selecting_ = true;
        capture();
        repaint();
    } else if (what.press == Press::move && selecting_) {
        head_ = placed(what.at);
        repaint();
    } else if (what.press == Press::up) {
        selecting_ = false;
    } else if (what.press == Press::double_click) {
        worded(placed(what.at));
        repaint();
    }
}

// A mouse event for a program that asked for the mouse, in xterm's SGR encoding: presses and
// releases always, drags when it asked for them, every move when it asked for that. The clicks the
// screen adds after a release say nothing new.
void Terminal::reported(const MouseEvent& what) {
    const Modes& modes = emulator_.modes();
    int button = xterm::mouse_none;

    switch (what.button) {
        case MouseButton::left: button = xterm::mouse_left; break;
        case MouseButton::middle: button = xterm::mouse_middle; break;
        case MouseButton::right: button = xterm::mouse_right; break;
        default: break;
    }

    if (what.press == Press::click || what.press == Press::double_click) return;

    if (what.press == Press::move) {
        const bool dragging = button != xterm::mouse_none;

        if (modes.mouse == Mousing::clicks || (modes.mouse == Mousing::drags && !dragging)) return;

        button += xterm::mouse_motion;
    } else if (button == xterm::mouse_none) {
        return;
    }

    button += xterm::mouse_mods(what.mods.shift, what.mods.alt, what.mods.ctrl);

    Bytes made;

    added(made, xterm::sgr(button, what.at, what.press == Press::up));

    (void)write(std::move(made));
}

// The row under the last one shown: the bottom of the screen, or the row where the prompt the shell
// waits at begins, when the prompt is not to be shown, so the picture ends just above it.
int Terminal::bottom() const {
    const int high = emulator_.size().high;

    if (*prompt) return high;

    return emulator_.prompt().value_or(high);
}

// How far back the view can be walked: until the oldest line of the history stands at the top. A
// hidden prompt lifts the picture by the rows under it, so as many rows of history are shown
// already.
int Terminal::deepest() const { return std::max(static_cast<int>(emulator_.history()) - (emulator_.size().high - bottom()), 0); }

// The row of the screen (below zero, of the history) the picture begins with.
int Terminal::first() const { return bottom() - emulator_.size().high - scrolled_; }

// The place of the cell `at` of the picture by the row's number from the start (`scrolled`), so
// that it stays with its text while output scrolls the screen.
Terminal::At Terminal::placed(Spot at) const {
    const int row = first() + std::clamp(at.y, 0, emulator_.size().high - 1);
    const long long counted = static_cast<long long>(emulator_.scrolled()) + row;

    return At{static_cast<std::uint64_t>(std::max<long long>(counted, 0)), std::clamp(at.x, 0, emulator_.size().wide - 1)};
}

// The selection in reading order, from its first cell to its last, both in it; none without one.
std::optional<std::pair<Terminal::At, Terminal::At>> Terminal::span() const {
    if (!anchor_ || !head_) return std::nullopt;

    return *anchor_ <= *head_ ? std::pair{*anchor_, *head_} : std::pair{*head_, *anchor_};
}

// Selects the word at `at`: the characters of words on its row around it, or the cell alone when
// it holds none; the second half of a wide character goes with the character.
void Terminal::worded(At at) {
    const long long row = static_cast<long long>(at.row) - static_cast<long long>(emulator_.scrolled());
    const std::vector<Cell> cells = emulator_.row(static_cast<int>(row)).cells;
    const auto letters = [&cells](int x) {
        const std::string& cluster = cells[static_cast<std::size_t>(x)].cluster;

        return cluster.empty() || wordy(cluster);
    };

    int from = at.column;
    int upto = at.column;

    if (letters(at.column)) {
        while (from > 0 && letters(from - 1)) --from;

        while (upto + 1 < static_cast<int>(cells.size()) && letters(upto + 1)) ++upto;
    }

    anchor_ = At{at.row, from};
    head_ = At{at.row, upto};
    selecting_ = false;
}

// The selected text: the rows from its first cell to its last, a row's spaces at its end left out
// and the rows joined by line ends, except where a row went on in the next one. What scrolled out
// of the history is gone from it. Nothing without a selection.
std::string Terminal::selected() const {
    const auto chosen = span();

    if (!chosen) return {};

    const auto [from, upto] = *chosen;
    const long long top = static_cast<long long>(emulator_.scrolled());
    const long long oldest = -static_cast<long long>(emulator_.history());
    std::string made;

    for (std::uint64_t line = from.row; line <= upto.row; ++line) {
        const long long row = static_cast<long long>(line) - top;

        if (row < oldest || row >= emulator_.size().high) continue;

        const Emulator::Row got = emulator_.row(static_cast<int>(row));
        const int start = line == from.row ? from.column : 0;
        const int end = line == upto.row ? upto.column : static_cast<int>(got.cells.size()) - 1;
        std::string said;

        for (int x = start; x <= end && x < static_cast<int>(got.cells.size()); ++x) said += got.cells[static_cast<std::size_t>(x)].cluster;

        const bool going = got.wrapped && line != upto.row;

        if (!going)
            while (!said.empty() && said.back() == ' ') said.pop_back();

        made += said;

        if (!going && line != upto.row) made += '\n';
    }

    return made;
}

// Takes the selection away.
void Terminal::unselect() {
    if (!anchor_ && !head_) return;

    anchor_.reset();
    head_.reset();
    selecting_ = false;
    repaint();
}

// Copies the selected text to the clipboard; false without a selection or when the clipboard
// refused it. The text is taken at once, so the terminal may go while the copy runs.
Task<bool> Terminal::copy() {
    const std::string what = selected();
    const Port<Clipboard> board = clipboard_;

    if (what.empty()) co_return false;

    co_return co_await board->copy(what);
}

// How many rows there are to see: the history and the screen down to its bottom (`bottom`), at
// least a screenful.
std::size_t Terminal::extent() const {
    return std::max<std::size_t>(emulator_.history() + static_cast<std::size_t>(bottom()), static_cast<std::size_t>(emulator_.size().high));
}

// How many rows the view shows: the screen's.
std::size_t Terminal::window() const { return static_cast<std::size_t>(emulator_.size().high); }

// The row of all (`extent`) the view shows first.
std::size_t Terminal::top() const { return static_cast<std::size_t>(deepest() - scrolled_); }

// Shows the rows from `row` on, as far as the history and the screen reach.
void Terminal::top(std::size_t row) { slid(static_cast<long long>(deepest() - scrolled_) - static_cast<long long>(std::min<std::size_t>(row, static_cast<std::size_t>(deepest())))); }

// The least room: the layout gives the terminal its size, as to a list.
Size Terminal::measure(Limits room) const { return room.least; }

// The terminal's size is the inside's: a new one goes to the screen and to the program first,
// since the program has to fit its picture to what is seen. The selection goes, its rows numbered
// anew.
void Terminal::arrange(Area inside) {
    const Size given = inside.size;

    if (given.wide <= 0 || given.high <= 0 || given == emulator_.size()) return;

    emulator_.resize(given);

    if (process_.held() && running_) process_.resize(emulator_.size().wide, emulator_.size().high);

    anchor_.reset();
    head_.reset();
    selecting_ = false;
    scrolled_ = std::clamp(scrolled_, 0, deepest());

    scrolled();
}

// Draws the screen. The rows drawn end at `bottom` and come from the history above it as far as
// needed, and as far as the view has been walked back. A color the program left as the default is
// the terminal's own, laid by the fill beneath; the selection is drawn reversed. A cell with a link
// carries its address, so a terminal around the program may follow it.
void Terminal::paint(Canvas& onto) const {
    const Look base = look();
    const int high = emulator_.size().high;
    const int start = first();
    const auto chosen = span();
    const long long top = static_cast<long long>(emulator_.scrolled());

    onto.fill(Area{{0, 0}, onto.size()}, base);

    for (int y = 0; y < high; ++y) {
        const Emulator::Row row = emulator_.row(start + y);
        const auto line = static_cast<std::uint64_t>(std::max<long long>(top + start + y, 0));

        for (int x = 0; x < static_cast<int>(row.cells.size());) {
            const Cell& cell = row.cells[static_cast<std::size_t>(x)];

            if (cell.cluster.empty()) {
                ++x;

                continue;
            }

            Look made = cell.look;

            if (chosen && top + start + y >= 0 && chosen->first <= At{line, x} && At{line, x} <= chosen->second) made.reverse = !made.reverse;

            const int wide = std::max(onto.put({x, y}, cell.cluster, made), 1);

            if (cell.link != 0)
                if (const std::string_view address = emulator_.address(cell.link); !address.empty()) onto.link(Area{{x, y}, {wide, 1}}, address);

            x += wide;
        }
    }
}

// The cursor of the program, while the live screen is shown and the program has not hidden it, in
// the form the program asked for; the document leaves out a cursor under a hidden prompt, outside
// the terminal.
std::optional<Caret> Terminal::caret() const {
    if (!focused() || scrolled_ != 0 || !emulator_.modes().cursor) return std::nullopt;

    const int shift = emulator_.size().high - bottom();
    const Spot at = emulator_.cursor();

    return Caret{Spot{at.x, at.y + shift}, emulator_.modes().form, emulator_.modes().blink};
}

// Tells the view's bar that the history, the window or the view moved.
void Terminal::scrolled() {
    Scrolled one;

    notified(one);
}

}

#include "tal/emulator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/style.hpp"
#include "tal/text.hpp"

#include "hexing.hpp"
#include "palette.hpp"

// The screen of a terminal, as a program in it sees the screen: the bytes it writes (text, control
// characters and escape sequences of xterm, ECMA-48 and the DEC terminals) turn into rows of cells,
// a cursor and the terminal's modes. This is pure logic, without input or output: whoever holds a
// pseudoterminal feeds it what the program wrote (`feed`) and draws the rows; what the program
// asked of the terminal (answers to its questions, the bell, the title, the folder the shell stands
// in, the stages of a shell's prompt) comes back from each feeding (`Fed`).
//
// The parser follows the state machine of the DEC terminals: text, an escape sequence, a control
// sequence (CSI) with its parameters, and strings (OSC, which carries the title, the folder, the
// links and the stages; DCS, APC, PM and SOS, which are read and dropped). What it does not know it
// skips whole, so an unknown sequence never shows up as letters. The two screens are those of
// xterm: the primary one with its history, and the alternate one that full-screen programs take
// and give back.
//
// What a program writes is hostile text: every count it gives and everything it makes the screen
// keep has a bound, so that neither memory nor the time of a byte depends on it beyond the size of
// the screen, and nothing it wrote is ever sent back to it.
namespace tal {

namespace {

// The highest value of one parameter; what is larger is taken as this, so that a long run of
// digits cannot overflow.
constexpr int most = 65535;

// How many parameters of a control sequence are kept, and how many parts of one parameter
// (38:2::r:g:b has six); the rest are read and dropped.
constexpr std::size_t most_parameters = 32;
constexpr std::size_t most_parts = 16;

// How many intermediate characters a sequence may have; one with more is read and dropped whole.
constexpr std::size_t most_intermediates = 2;

// The longest string (OSC) that counts; a longer one is read and dropped whole.
constexpr std::size_t most_string = 4096;

// The longest address of a link (OSC 8).
constexpr std::size_t most_address = 2048;

// The most bytes one cell's cluster holds: the marks a program joins to a letter beyond it are
// left out.
constexpr std::size_t most_cluster = 32;

// The most bytes the addresses of all links take together.
constexpr std::size_t most_links = std::size_t{1} << 20;

// The largest side of the screen.
constexpr int most_side = 1024;

// The DEC special graphics, the line-drawing set full-screen programs switch to with `ESC ( 0`:
// the characters from `_` to `~` become the corners, lines and symbols below.
constexpr std::u32string_view graphics = U" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";

// A character through the DEC special graphics, when that set is in use.
char32_t drawn(char32_t one, bool special) {
    if (!special || one < U'_' || one > U'~') return one;

    return graphics[static_cast<std::size_t>(one - U'_')];
}

// Appends text to bytes for an answer.
void told(Bytes& into, std::string_view said) {
    for (const char each : said) into.push_back(static_cast<std::byte>(each));
}

// A side of the screen within its bounds.
int sided(int one) { return std::clamp(one, 1, most_side); }

// Whether `one` is a cell nothing was written to: a space without colors or a link.
bool vacant(const Cell& one) { return one == Cell{}; }

// Whether `one` holds the first half of a wide character.
bool wide(const Cell& one) { return !one.cluster.empty() && text::width(one.cluster) == 2; }

// A line of the screen: its cells, whether it goes on in the next line because the text wrapped
// there (`wrapped`), and whether its last cell is only the room a wide character left when it went
// on to the next line (`spilled`).
struct Line {
    std::vector<Cell> cells;
    bool wrapped = false;
    bool spilled = false;
};

// A run of cells of one look and one link in a packed line.
struct Streak {
    std::uint32_t count = 0;
    Look look;
    std::uint32_t link = 0;
};

// A line of the history, packed: the clusters one after another, the size of each in bytes (0 for
// the second half of a wide character), and the runs of looks; the vacant cells at its end are
// left out. A cell of the screen weighs 56 bytes, a cell of plain text here one or two.
struct Packed {
    std::string text;
    std::vector<std::uint8_t> sizes;
    std::vector<Streak> runs;
    bool wrapped = false;
    bool spilled = false;
};

// A line of `wide` vacant cells.
Line blank(int wide) { return Line{std::vector<Cell>(static_cast<std::size_t>(wide)), false, false}; }

// Leaves no half of a wide character alone in `one`: a second half without its first, or a first
// half without its second, becomes a space with its look.
void mended(Line& one) {
    std::vector<Cell>& cells = one.cells;

    for (std::size_t at = 0; at < cells.size(); ++at) {
        if (cells[at].cluster.empty()) {
            if (at == 0 || !wide(cells[at - 1])) cells[at].cluster = " ";

            continue;
        }

        if (wide(cells[at]) && (at + 1 >= cells.size() || !cells[at + 1].cluster.empty())) cells[at].cluster = " ";
    }
}

// `one` packed for the history.
Packed packed(const Line& one) {
    Packed made;
    std::size_t end = one.cells.size();

    while (end > 0 && vacant(one.cells[end - 1])) --end;

    made.sizes.reserve(end);

    for (std::size_t at = 0; at < end; ++at) {
        const Cell& cell = one.cells[at];

        made.text += cell.cluster;
        made.sizes.push_back(static_cast<std::uint8_t>(cell.cluster.size()));

        if (!made.runs.empty() && made.runs.back().look == cell.look && made.runs.back().link == cell.link) ++made.runs.back().count;
        else made.runs.push_back(Streak{1, cell.look, cell.link});
    }

    made.wrapped = one.wrapped;
    made.spilled = one.spilled;

    return made;
}

// The line `one` packed, back as `wide` cells: vacant ones after its text, or its text cut there.
Line unpacked(const Packed& one, int wide) {
    Line made{{}, one.wrapped, one.spilled};
    std::size_t from = 0;
    std::size_t cell = 0;

    made.cells.reserve(std::max(one.sizes.size(), static_cast<std::size_t>(wide)));

    for (const Streak& run : one.runs)
        for (std::uint32_t turn = 0; turn < run.count; ++turn, ++cell) {
            const std::size_t size = one.sizes[cell];

            made.cells.push_back(Cell{one.text.substr(from, size), run.look, 0, run.link});
            from += size;
        }

    made.cells.resize(static_cast<std::size_t>(wide));
    mended(made);

    return made;
}

// Whether nothing was written to `one`.
bool vacant(const Line& one) { return std::all_of(one.cells.begin(), one.cells.end(), [](const Cell& each) { return vacant(each); }); }

// A place carried through a new layout: the row and column, and whether the cursor waited there for
// a wrap.
struct Carried {
    std::size_t row = 0;
    int column = 0;
    bool waiting = false;
};

// The text of lines laid out anew in rows of `wide` cells, each row packed once it is finished, so
// that the history never stands unpacked whole.
class Flow {
public:
    explicit Flow(int wide) : wide_(wide), row_(blank(wide)) {}

    // The rows finished so far.
    [[nodiscard]] std::vector<Packed>& rows() { return rows_; }

    // Puts `cell` on the row being filled, two cells wide with `half` after it when `half` is
    // given, going on to the next row when it does not fit; a wide character with one cell left
    // leaves that cell as room. On rows of one cell a wide character becomes a space. The place
    // where it starts.
    Carried put(const Cell& cell, const Cell* half) {
        const int much = half != nullptr && wide_ >= 2 ? 2 : 1;

        if (column_ + much > wide_) {
            if (much == 2 && column_ == wide_ - 1) row_.spilled = true;

            next();
        }

        const Carried made{rows_.size(), column_, false};

        if (half != nullptr && much == 1) {
            row_.cells[static_cast<std::size_t>(column_)] = Cell{" ", cell.look, 0, cell.link};
        } else {
            row_.cells[static_cast<std::size_t>(column_)] = cell;

            if (much == 2) row_.cells[static_cast<std::size_t>(column_ + 1)] = *half;
        }

        column_ += much;

        return made;
    }

    // The place `beyond` cells past the text put so far, over blanks, with the rows up to it. A
    // place at the end of a row is its last column, waiting for a wrap, as the cursor waits after a
    // row it filled; no row is begun for it.
    Carried past(std::size_t beyond) {
        const auto wide = static_cast<std::size_t>(wide_);
        std::size_t total = static_cast<std::size_t>(column_) + beyond;

        if (total > 0 && total % wide == 0) {
            for (; total > wide; total -= wide) next();

            column_ = wide_;

            return Carried{rows_.size(), wide_ - 1, true};
        }

        for (; total >= wide; total -= wide) next();

        column_ = static_cast<int>(total);

        return Carried{rows_.size(), column_, false};
    }

    // Ends the line: the row being filled is finished and goes on in no other.
    void ended() {
        rows_.push_back(packed(row_));
        row_ = blank(wide_);
        column_ = 0;
    }

private:
    // The row being filled is finished and goes on in the next one.
    void next() {
        row_.wrapped = true;
        ended();
    }

    int wide_ = 1;
    Line row_;
    int column_ = 0;
    std::vector<Packed> rows_;
};

}

// The whole state of the screen and of the parser.
struct Emulator::Held {
    enum class State : std::uint8_t { ground, escape, intermediate, csi, osc, ignored };

    struct Saved {
        int x = 0;
        int y = 0;
        bool pending = false;
        Look look;
        bool concealed = false;
        bool origin = false;
        bool special = false;
        bool spare = false;
        bool shifted = false;
    };

    Held(Size given, std::size_t lines);

    [[nodiscard]] std::vector<Line>& screen() { return modes.alternate ? alternate : primary; }

    [[nodiscard]] const std::vector<Line>& screen() const { return modes.alternate ? alternate : primary; }

    [[nodiscard]] Cell erased() const;

    void step(char32_t one, Fed& fed);

    void control(char32_t one, Fed& fed);

    void print(char32_t one);

    void joined(char32_t one);

    void broke(int at);

    void index();

    void reverse();

    void newline();

    void wrapped();

    void scroll_up(int many);

    void scroll_down(int many);

    void remembered(Line one);

    void escaped(char32_t final);

    void parameter(char32_t one);

    [[nodiscard]] int number(std::size_t at, int fallback) const;

    void dispatched(char32_t final, Fed& fed);

    void reported(bool asked_private, Fed& fed) const;

    void moded(bool on, Fed& fed);

    [[nodiscard]] std::optional<Color> colored(std::size_t& at) const;

    void graphic();

    void erase_display(int how);

    void erase_line(int how);

    void inserted_lines(int many);

    void deleted_lines(int many);

    void tab(int many);

    void placed(int across, int down);

    void saved();

    void restored();

    void reset();

    void switched(bool to, bool clearing, Fed& fed);

    void stringed(Fed& fed);

    void foldered(const std::string& said);

    void linked(std::string_view said);

    void marked(const std::string& said, Fed& fed);

    [[nodiscard]] std::uint32_t interned(std::string_view address);

    void swept();

    [[nodiscard]] std::uint64_t first() const { return dropped + history.size(); }

    void resize(Size given);

    void heightened(int high);

    void reflowed(Size given);

    Size size;
    std::size_t kept = 0;
    std::vector<Line> primary;
    std::vector<Line> alternate;
    std::deque<Packed> history;
    std::uint64_t dropped = 0;
    int x = 0;
    int y = 0;
    bool pending = false;
    Look look;
    bool concealed = false;
    std::uint32_t link = 0;
    int top = 0;
    int bottom = 0;
    std::vector<bool> tabs;
    Modes modes;
    bool special = false;
    bool spare = false;
    bool shifted = false;
    Saved primary_saved;
    Saved alternate_saved;
    State state = State::ground;
    std::vector<std::vector<int>> parameters;
    bool fresh = true;
    bool dropping = false;
    std::string intermediates;
    bool overlong = false;
    char prefix = 0;
    std::string gathered;
    bool gathered_escape = false;
    bool gathered_long = false;
    bool osc = false;
    std::string partial;
    char32_t last = 0;
    Spot printed{-1, -1};
    int printed_wide = 0;
    std::string title;
    std::string folder;
    std::optional<Stage> stage;
    std::optional<std::uint64_t> prompt;
    std::vector<std::string> addresses;
    std::vector<std::uint32_t> unused;
    std::map<std::string, std::uint32_t, std::less<>> known;
    std::size_t link_bytes = 0;
};

// An empty screen of `size`, its sides within 1 and 1024, with the cursor in the top left corner,
// and room for `kept` lines of history above it.
Emulator::Held::Held(Size given, std::size_t lines) : size{sided(given.wide), sided(given.high)}, kept(lines) {
    primary.assign(static_cast<std::size_t>(size.high), blank(size.wide));
    alternate = primary;
    bottom = size.high - 1;
    tabs.assign(static_cast<std::size_t>(size.wide), false);

    for (int at = 8; at < size.wide; at += 8) tabs[static_cast<std::size_t>(at)] = true;
}

// The cell an erase leaves: a space in the default colors, with the background in use, as xterm
// erases (back color erase).
Cell Emulator::Held::erased() const {
    Cell made;

    made.look.paper = look.paper;

    return made;
}

// One character through the state machine. The controls of the C1 set (U+0080 to U+009F) do
// nothing and show nothing, as in terminals that read UTF-8.
void Emulator::Held::step(char32_t one, Fed& fed) {
    if (one >= 0x80 && one <= 0x9F) return;

    // In a string an escape ends it and starts an escape sequence: `ESC \`, the terminator, does
    // nothing more, and anything else goes on as its own sequence.
    if (state == State::osc || state == State::ignored) {
        if (gathered_escape) {
            gathered_escape = false;

            stringed(fed);

            state = State::escape;
            intermediates.clear();
            overlong = false;

            escaped(one);

            return;
        }

        if (one == 0x1B) {
            gathered_escape = true;

            return;
        }

        if (one == 0x07) {
            stringed(fed);

            state = State::ground;

            return;
        }

        if (one == 0x18 || one == 0x1A) {
            gathered.clear();
            state = State::ground;

            return;
        }

        if (state == State::osc && one >= 0x20 && one != 0x7F && !gathered_long) {
            if (gathered.size() + 4 > most_string) {
                gathered_long = true;
                gathered.clear();
            } else {
                text::encoded(gathered, one);
            }
        }

        return;
    }

    if (one == 0x1B) {
        state = State::escape;
        intermediates.clear();
        overlong = false;

        return;
    }

    // Cancel and substitute break off whatever sequence was under way.
    if (one == 0x18 || one == 0x1A) {
        state = State::ground;

        return;
    }

    if (one < 0x20) {
        control(one, fed);

        return;
    }

    if (one == 0x7F) return;

    switch (state) {
        case State::ground: print(one); return;
        case State::escape:
        case State::intermediate: escaped(one); return;
        case State::csi:
            if ((one >= U'0' && one <= U'9') || one == U';' || one == U':') {
                parameter(one);

                return;
            }

            if (one >= U'<' && one <= U'?') {
                if (parameters.empty() && fresh && prefix == 0 && intermediates.empty()) prefix = static_cast<char>(one);

                return;
            }

            if (one >= 0x20 && one <= 0x2F) {
                if (intermediates.size() < most_intermediates) intermediates += static_cast<char>(one);
                else overlong = true;

                return;
            }

            if (one >= 0x40 && one <= 0x7E) {
                state = State::ground;

                if (!overlong) dispatched(one, fed);

                intermediates.clear();
                overlong = false;

                return;
            }

            return;
        case State::osc:
        case State::ignored: return;
    }
}

// A digit, a semicolon or a colon of a control sequence's parameters. A semicolon starts the next
// parameter and a colon the next part of one; parameters and parts past their bounds are read and
// dropped (`dropping`).
void Emulator::Held::parameter(char32_t one) {
    const auto started = [this] {
        dropping = parameters.size() >= most_parameters;

        if (!dropping) parameters.emplace_back(1, 0);
    };

    if (one == U';') {
        if (fresh) started();

        fresh = true;

        return;
    }

    if (fresh) {
        started();
        fresh = false;
    }

    if (one == U':') {
        if (dropping) return;

        if (parameters.back().size() < most_parts) parameters.back().push_back(0);
        else dropping = true;

        return;
    }

    if (dropping) return;

    int& value = parameters.back().back();

    value = std::min(value * 10 + static_cast<int>(one - U'0'), most);
}

// A control character, wherever it comes: in the middle of a sequence it acts at once, and the
// sequence goes on.
void Emulator::Held::control(char32_t one, Fed& fed) {
    switch (one) {
        case 0x07: ++fed.bells; return;
        case 0x08:
            pending = false;

            if (x > 0) --x;

            return;
        case 0x09: tab(1); return;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            index();

            if (modes.newline) x = 0;

            return;
        case 0x0D:
            x = 0;
            pending = false;

            return;
        case 0x0E: shifted = true; return;
        case 0x0F: shifted = false; return;
        default: return;
    }
}

// A printable character at the cursor. A character waiting for a wrap (the cursor stood past the
// last column) first goes to the next line, which the line it leaves goes on in; one that takes two
// cells and has one left goes there too, leaving that cell as room. In insert mode what stands to
// the right moves along. The cursor stops at the last column and waits there for the next
// character, as xterm does, so a line exactly as wide as the screen leaves no empty line under it.
void Emulator::Held::print(char32_t one) {
    one = drawn(one, shifted ? spare : special);

    const int much = text::width(one);

    if (much == 0) {
        joined(one);

        return;
    }

    if (pending && modes.wrap) wrapped();

    if (much == 2 && x == size.wide - 1) {
        if (!modes.wrap || size.wide < 2) return;

        broke(x);
        screen()[static_cast<std::size_t>(y)].cells[static_cast<std::size_t>(x)] = erased();
        screen()[static_cast<std::size_t>(y)].spilled = true;

        wrapped();
    }

    Line& row = screen()[static_cast<std::size_t>(y)];

    if (modes.insert) {
        for (int at = size.wide - 1; at >= x + much; --at) row.cells[static_cast<std::size_t>(at)] = row.cells[static_cast<std::size_t>(at - much)];

        row.wrapped = row.spilled = false;
    }

    broke(x);

    if (much == 2) broke(x + 1);

    Cell made{concealed ? std::string{" "} : std::string{}, look, 0, link};

    if (!concealed) text::encoded(made.cluster, one);

    row.cells[static_cast<std::size_t>(x)] = made;

    if (much == 2) row.cells[static_cast<std::size_t>(x + 1)] = Cell{concealed ? std::string{" "} : std::string{}, look, 0, link};

    if (modes.insert) mended(row);

    printed = Spot{x, y};
    printed_wide = much;
    last = one;
    x += much;

    if (x >= size.wide) {
        x = size.wide - 1;
        pending = modes.wrap;
    }
}

// A character that takes no cell (a combining mark, a joiner, a selector) joins the cluster printed
// last, while the cursor stands right after it, or on it at the last column, when the two stay one
// cluster of the same width, as a terminal measures it, and the cluster stays within its bound;
// otherwise it is left out.
void Emulator::Held::joined(char32_t one) {
    const int end = printed.x + printed_wide;

    if (printed.x < 0 || printed.y != y || (end != x && !(end >= size.wide && x == size.wide - 1))) return;

    Cell& cell = screen()[static_cast<std::size_t>(y)].cells[static_cast<std::size_t>(printed.x)];

    if (cell.cluster.empty() || cell.cluster == " ") return;

    std::string made = cell.cluster;

    text::encoded(made, one);

    if (made.size() > most_cluster || text::after(made, 0) != made.size() || text::width(made) != text::width(cell.cluster)) return;

    cell.cluster = std::move(made);
}

// A cell about to be written over: when it is half of a wide character, the other half becomes a
// space, so no half character stays on the screen.
void Emulator::Held::broke(int at) {
    if (at < 0 || at >= size.wide) return;

    std::vector<Cell>& cells = screen()[static_cast<std::size_t>(y)].cells;
    const auto here = static_cast<std::size_t>(at);

    if (cells[here].cluster.empty() && at > 0) {
        cells[here - 1].cluster = " ";
        cells[here].cluster = " ";
    } else if (wide(cells[here]) && at + 1 < size.wide && cells[here + 1].cluster.empty()) {
        cells[here + 1].cluster = " ";
    }
}

// A line down, scrolling the region when the cursor stands on its last line.
void Emulator::Held::index() {
    pending = false;

    if (y == bottom) scroll_up(1);
    else if (y < size.high - 1) ++y;
}

// A line up, scrolling the region down when the cursor stands on its first line.
void Emulator::Held::reverse() {
    pending = false;

    if (y == top) scroll_down(1);
    else if (y > 0) --y;
}

// The start of the next line.
void Emulator::Held::newline() {
    x = 0;

    index();
}

// The start of the next line after the text wrapped: the line left goes on in it.
void Emulator::Held::wrapped() {
    screen()[static_cast<std::size_t>(y)].wrapped = true;

    newline();
}

// Scrolls the region up: its top lines leave, empty ones come in at its bottom. Lines that leave
// the top of the whole primary screen go into the history.
void Emulator::Held::scroll_up(int many) {
    many = std::clamp(many, 0, bottom - top + 1);

    std::vector<Line>& lines = screen();

    for (int turn = 0; turn < many; ++turn) {
        Line gone = std::move(lines[static_cast<std::size_t>(top)]);

        lines.erase(lines.begin() + top);
        lines.insert(lines.begin() + bottom, Line{std::vector<Cell>(static_cast<std::size_t>(size.wide), erased()), false, false});

        if (!modes.alternate && top == 0) remembered(std::move(gone));
    }
}

// Scrolls the region down: empty lines come in at its top, its bottom lines leave.
void Emulator::Held::scroll_down(int many) {
    many = std::clamp(many, 0, bottom - top + 1);

    std::vector<Line>& lines = screen();

    for (int turn = 0; turn < many; ++turn) {
        lines.erase(lines.begin() + bottom);
        lines.insert(lines.begin() + top, Line{std::vector<Cell>(static_cast<std::size_t>(size.wide), erased()), false, false});
    }
}

// A line into the history, the oldest leaving when it is full; a line that leaves counts in
// `dropped`, so that rows keep their numbers from the start.
void Emulator::Held::remembered(Line one) {
    if (kept == 0) {
        ++dropped;

        return;
    }

    history.push_back(packed(one));

    while (history.size() > kept) {
        history.pop_front();
        ++dropped;
    }
}

// The final character of an escape sequence, after its intermediates.
void Emulator::Held::escaped(char32_t final) {
    if (final >= 0x20 && final <= 0x2F) {
        if (intermediates.size() < most_intermediates) intermediates += static_cast<char>(final);
        else overlong = true;

        state = State::intermediate;

        return;
    }

    state = State::ground;

    if (overlong) {
        intermediates.clear();
        overlong = false;

        return;
    }

    if (!intermediates.empty()) {
        // The character sets: `ESC ( 0` takes the line-drawing set for G0, `ESC ( B` gives ASCII
        // back; the same with `)` for G1. `ESC # 8` fills the screen with E, the DEC alignment
        // test.
        if (intermediates == "(") special = final == U'0';
        else if (intermediates == ")") spare = final == U'0';
        else if (intermediates == "#" && final == U'8') {
            for (Line& row : screen()) row = Line{std::vector<Cell>(static_cast<std::size_t>(size.wide), Cell{"E", Look{}, 0, 0}), false, false};
        }

        intermediates.clear();

        return;
    }

    switch (final) {
        case U'[':
            state = State::csi;
            parameters.clear();
            prefix = 0;
            fresh = true;
            dropping = false;

            return;
        case U']':
            state = State::osc;
            gathered.clear();
            gathered_long = false;
            osc = true;

            return;
        case U'P':
        case U'X':
        case U'^':
        case U'_':
            state = State::ignored;
            gathered.clear();
            gathered_long = false;
            osc = false;

            return;
        case U'7': saved(); return;
        case U'8': restored(); return;
        case U'D': index(); return;
        case U'E': newline(); return;
        case U'M': reverse(); return;
        case U'H': tabs[static_cast<std::size_t>(x)] = true; return;
        case U'c': reset(); return;
        case U'=': modes.keypad = true; return;
        case U'>': modes.keypad = false; return;
        default: return;
    }
}

// The value of a parameter of a control sequence; a missing one or zero is the fallback, as the
// sequences define it.
int Emulator::Held::number(std::size_t at, int fallback) const {
    if (at >= parameters.size() || parameters[at].front() == 0) return fallback;

    return parameters[at].front();
}

// A control sequence.
void Emulator::Held::dispatched(char32_t final, Fed& fed) {
    const int one = number(0, 1);

    if (intermediates == "$" && final == U'p' && (prefix == '?' || prefix == 0)) {
        reported(prefix == '?', fed);

        return;
    }

    if (prefix == '?') {
        if (intermediates.empty() && (final == U'h' || final == U'l')) moded(final == U'h', fed);

        return;
    }

    if (prefix == '>') {
        // The secondary device attributes: a VT220-like terminal of the first version.
        if (intermediates.empty() && final == U'c') told(fed.replies, "\x1b[>1;10;0c");

        return;
    }

    if (prefix != 0) return;

    if (intermediates == "!" && final == U'p') {
        // A soft reset: the modes, the region and the look, not the screen.
        modes.insert = false;
        modes.origin = false;
        modes.wrap = true;
        modes.cursor = true;
        modes.cursor_keys = false;
        modes.keypad = false;
        top = 0;
        bottom = size.high - 1;
        look = Look{};
        concealed = false;
        special = spare = shifted = false;

        return;
    }

    if (intermediates == " " && final == U'q') {
        // The cursor's form the program asks for (DECSCUSR): 0 the terminal's usual one, 1 and 2 a
        // block, 3 and 4 an underline, 5 and 6 a bar, the odd ones blinking.
        const int asked = number(0, 0);

        if (asked >= 1 && asked <= 6) {
            modes.form = asked <= 2 ? Caret::Form::block : asked <= 4 ? Caret::Form::underline : Caret::Form::bar;
            modes.blink = asked % 2 == 1;
        } else {
            modes.form = Caret::Form::usual;
            modes.blink = true;
        }

        return;
    }

    // Anything else with an intermediate the screen does not keep, so it is read and dropped.
    if (!intermediates.empty()) return;

    const int high = size.high;
    const int wide = size.wide;
    const int origin = modes.origin ? top : 0;

    switch (final) {
        case U'@': {
            Line& row = screen()[static_cast<std::size_t>(y)];
            const int many = std::min(one, wide - x);

            row.cells.erase(row.cells.end() - many, row.cells.end());
            row.cells.insert(row.cells.begin() + x, static_cast<std::size_t>(many), erased());
            row.wrapped = row.spilled = false;
            mended(row);
            pending = false;

            return;
        }
        case U'A': placed(x, std::max(y - one, y >= top ? top : 0)); return;
        case U'B':
        case U'e': placed(x, std::min(y + one, y <= bottom ? bottom : high - 1)); return;
        case U'C':
        case U'a': placed(std::min(x + one, wide - 1), y); return;
        case U'D': placed(std::max(x - one, 0), y); return;
        case U'E': placed(0, std::min(y + one, y <= bottom ? bottom : high - 1)); return;
        case U'F': placed(0, std::max(y - one, y >= top ? top : 0)); return;
        case U'G':
        case U'`': placed(std::clamp(one - 1, 0, wide - 1), y); return;
        case U'H':
        case U'f': {
            const int row = std::clamp(number(0, 1) - 1 + origin, origin, modes.origin ? bottom : high - 1);

            placed(std::clamp(number(1, 1) - 1, 0, wide - 1), row);

            return;
        }
        case U'I': tab(one); return;
        case U'J': erase_display(number(0, 0)); return;
        case U'K': erase_line(number(0, 0)); return;
        case U'L': inserted_lines(one); return;
        case U'M': deleted_lines(one); return;
        case U'P': {
            Line& row = screen()[static_cast<std::size_t>(y)];
            const int many = std::min(one, wide - x);

            row.cells.erase(row.cells.begin() + x, row.cells.begin() + x + many);
            row.cells.insert(row.cells.end(), static_cast<std::size_t>(many), erased());
            row.wrapped = row.spilled = false;
            mended(row);
            pending = false;

            return;
        }
        case U'S': scroll_up(one); return;
        case U'T': scroll_down(one); return;
        case U'X': {
            Line& row = screen()[static_cast<std::size_t>(y)];

            for (int at = x; at < std::min(x + one, wide); ++at) row.cells[static_cast<std::size_t>(at)] = erased();

            if (x + one >= wide) row.wrapped = row.spilled = false;

            mended(row);
            pending = false;

            return;
        }
        case U'Z':
            for (int turn = 0; turn < one && x > 0; ++turn) {
                --x;

                while (x > 0 && !tabs[static_cast<std::size_t>(x)]) --x;
            }

            pending = false;

            return;
        case U'b':
            if (last != 0)
                for (int turn = 0; turn < std::min(one, wide * high); ++turn) print(last);

            return;
        case U'c':
            // The primary device attributes: a VT220 with color (22), which is what programs need
            // to hear to use colors and the line-drawing set.
            if (number(0, 0) == 0) told(fed.replies, "\x1b[?62;22c");

            return;
        case U'd': placed(x, std::clamp(one - 1 + origin, 0, high - 1)); return;
        case U'g':
            if (number(0, 0) == 0) tabs[static_cast<std::size_t>(x)] = false;
            else if (number(0, 0) == 3) std::fill(tabs.begin(), tabs.end(), false);

            return;
        case U'h':
        case U'l': {
            const bool on = final == U'h';

            for (std::size_t at = 0; at < parameters.size(); ++at) {
                if (number(at, 0) == 4) modes.insert = on;
                if (number(at, 0) == 20) modes.newline = on;
            }

            return;
        }
        case U'm': graphic(); return;
        case U'n':
            if (number(0, 0) == 5) told(fed.replies, "\x1b[0n");

            if (number(0, 0) == 6) told(fed.replies, "\x1b[" + std::to_string(y - origin + 1) + ";" + std::to_string(x + 1) + "R");

            return;
        case U'r': {
            const int first = number(0, 1) - 1;
            const int end = std::min(number(1, high), high) - 1;

            if (first < end) {
                top = first;
                bottom = end;

                placed(0, modes.origin ? top : 0);
            }

            return;
        }
        case U's': saved(); return;
        case U'u': restored(); return;
        default: return;
    }
}

// The answer to a question about a mode (DECRQM), private (`CSI ? Ps $ p`) or of ANSI
// (`CSI Ps $ p`): 1 when it is set, 2 when it is not, 0 for a mode the screen does not know.
void Emulator::Held::reported(bool asked_private, Fed& fed) const {
    const int which = number(0, 0);
    std::optional<bool> on;

    if (asked_private) {
        switch (which) {
            case 1: on = modes.cursor_keys; break;
            case 6: on = modes.origin; break;
            case 7: on = modes.wrap; break;
            case 25: on = modes.cursor; break;
            case 47:
            case 1047:
            case 1049: on = modes.alternate; break;
            case 1000: on = modes.mouse == Mousing::clicks; break;
            case 1002: on = modes.mouse == Mousing::drags; break;
            case 1003: on = modes.mouse == Mousing::moves; break;
            case 1004: on = modes.focus; break;
            case 1006: on = modes.sgr_mouse; break;
            case 2004: on = modes.paste; break;
            case 2026: on = modes.synchronized; break;
            default: break;
        }
    } else if (which == 4) {
        on = modes.insert;
    } else if (which == 20) {
        on = modes.newline;
    }

    const int setting = !on ? 0 : *on ? 1 : 2;

    told(fed.replies, std::string{"\x1b["} + (asked_private ? "?" : "") + std::to_string(which) + ";" + std::to_string(setting) + "$y");
}

// The private modes, `CSI ? Ps h` and `CSI ? Ps l`, several in one sequence.
void Emulator::Held::moded(bool on, Fed& fed) {
    for (std::size_t at = 0; at < parameters.size(); ++at) {
        switch (number(at, 0)) {
            case 1: modes.cursor_keys = on; break;
            case 6:
                modes.origin = on;

                placed(0, on ? top : 0);

                break;
            case 7: modes.wrap = on; break;
            case 25: modes.cursor = on; break;
            case 47: switched(on, false, fed); break;
            case 1047: switched(on, true, fed); break;
            case 1048:
                if (on) saved();
                else restored();

                break;
            case 1049:
                if (on) {
                    saved();
                    switched(true, true, fed);
                } else {
                    switched(false, false, fed);
                    restored();
                }

                break;
            case 1000: modes.mouse = on ? Mousing::clicks : Mousing::none; break;
            case 1002: modes.mouse = on ? Mousing::drags : Mousing::none; break;
            case 1003: modes.mouse = on ? Mousing::moves : Mousing::none; break;
            case 1004: modes.focus = on; break;
            case 1006: modes.sgr_mouse = on; break;
            case 2004: modes.paste = on; break;
            case 2026: modes.synchronized = on; break;
            default: break;
        }
    }
}

// The color of an extended color parameter at `at`: `5;n` from the palette of 256, `2;r;g;b` by its
// components, written either way xterm takes them, with semicolons or with colons; the default
// color for one it cannot read. `at` moves past what it used.
std::optional<Color> Emulator::Held::colored(std::size_t& at) const {
    const std::vector<int>& group = parameters[at];
    const auto component = [](int one) { return static_cast<std::uint8_t>(std::min(one, 255)); };
    const auto indexed = [](int which) { return which <= 255 ? std::optional<Color>{palette::indexed(which)} : std::nullopt; };

    // With colons everything is in one parameter: 38:5:n or 38:2::r:g:b (the empty one is the color
    // space, which may also be left out).
    if (group.size() > 1) {
        if (group[1] == 5 && group.size() > 2) return indexed(group[2]);

        if (group[1] == 2) {
            const std::size_t from = group.size() >= 6 ? 3 : 2;

            if (group.size() >= from + 3) return Color{component(group[from]), component(group[from + 1]), component(group[from + 2])};
        }

        return std::nullopt;
    }

    const auto value = [this](std::size_t which) { return which < parameters.size() ? parameters[which].front() : 0; };

    if (value(at + 1) == 5) {
        const int which = value(at + 2);

        at += 2;

        return indexed(which);
    }

    if (value(at + 1) == 2) {
        const Color made{component(value(at + 2)), component(value(at + 3)), component(value(at + 4))};

        at += 4;

        return made;
    }

    return std::nullopt;
}

// The look of what is written next (SGR). An empty sequence is a reset. Colors 0–7 and their bright
// halves come from the palette of 256; the default colors stay empty, so whoever draws the screen
// gives them its own. An underline of any kind (4:3 is curly) is an underline.
void Emulator::Held::graphic() {
    if (parameters.empty()) {
        look = Look{};
        concealed = false;

        return;
    }

    for (std::size_t at = 0; at < parameters.size(); ++at) {
        const int one = parameters[at].front();

        switch (one) {
            case 0:
                look = Look{};
                concealed = false;

                break;
            case 1: look.bold = true; break;
            case 2: look.faint = true; break;
            case 3: look.italic = true; break;
            case 4: look.under = parameters[at].size() < 2 || parameters[at][1] != 0; break;
            case 7: look.reverse = true; break;
            case 8: concealed = true; break;
            case 9: look.crossed = true; break;
            case 21: look.under = true; break;
            case 22: look.bold = look.faint = false; break;
            case 23: look.italic = false; break;
            case 24: look.under = false; break;
            case 27: look.reverse = false; break;
            case 28: concealed = false; break;
            case 29: look.crossed = false; break;
            case 38: look.ink = colored(at); break;
            case 39: look.ink.reset(); break;
            case 48: look.paper = colored(at); break;
            case 49: look.paper.reset(); break;
            default:
                if (one >= 30 && one <= 37) look.ink = palette::indexed(one - 30);
                else if (one >= 40 && one <= 47) look.paper = palette::indexed(one - 40);
                else if (one >= 90 && one <= 97) look.ink = palette::indexed(one - 90 + 8);
                else if (one >= 100 && one <= 107) look.paper = palette::indexed(one - 100 + 8);

                break;
        }
    }
}

// Erases in the display: from the cursor to the end (0), from the start to the cursor (1), the
// whole screen (2), or the history (3).
void Emulator::Held::erase_display(int how) {
    std::vector<Line>& lines = screen();

    pending = false;

    if (how == 3) {
        dropped += history.size();
        history.clear();

        return;
    }

    for (int at = 0; at < size.high; ++at)
        if (how == 2 || (how == 0 && at > y) || (how == 1 && at < y))
            lines[static_cast<std::size_t>(at)] = Line{std::vector<Cell>(static_cast<std::size_t>(size.wide), erased()), false, false};

    if (how == 0 || how == 1) erase_line(how);
}

// Erases in the line: from the cursor to its end (0), from its start to the cursor (1), or the
// whole line (2). A line erased to its end goes on in no other.
void Emulator::Held::erase_line(int how) {
    Line& row = screen()[static_cast<std::size_t>(y)];

    pending = false;

    const int from = how == 0 ? x : 0;
    const int to = how == 1 ? x + 1 : size.wide;

    for (int at = from; at < std::min(to, size.wide); ++at) row.cells[static_cast<std::size_t>(at)] = erased();

    if (to >= size.wide) row.wrapped = row.spilled = false;

    mended(row);
}

// Inserts empty lines at the cursor, within the scroll region; the region's last lines leave.
void Emulator::Held::inserted_lines(int many) {
    if (y < top || y > bottom) return;

    const int was = top;

    top = y;

    scroll_down(many);

    top = was;
    x = 0;
    pending = false;
}

// Deletes lines at the cursor, within the scroll region; empty ones come in at its bottom. The
// lines deleted go nowhere: they are not the top of the screen leaving.
void Emulator::Held::deleted_lines(int many) {
    if (y < top || y > bottom) return;

    std::vector<Line>& lines = screen();

    many = std::clamp(many, 0, bottom - y + 1);

    for (int turn = 0; turn < many; ++turn) {
        lines.erase(lines.begin() + y);
        lines.insert(lines.begin() + bottom, Line{std::vector<Cell>(static_cast<std::size_t>(size.wide), erased()), false, false});
    }

    x = 0;
    pending = false;
}

// To the next tab stop, `many` times, or to the last column.
void Emulator::Held::tab(int many) {
    pending = false;

    for (int turn = 0; turn < many; ++turn) {
        ++x;

        while (x < size.wide - 1 && !tabs[static_cast<std::size_t>(x)]) ++x;

        if (x >= size.wide - 1) {
            x = size.wide - 1;

            return;
        }
    }
}

// The cursor to a cell of the screen; it waits for no wrap there.
void Emulator::Held::placed(int across, int down) {
    x = std::clamp(across, 0, size.wide - 1);
    y = std::clamp(down, 0, size.high - 1);
    pending = false;
}

// Saves the cursor (DECSC): its place, the look, the origin mode and the character sets, one set
// for each screen.
void Emulator::Held::saved() {
    Saved& into = modes.alternate ? alternate_saved : primary_saved;

    into = Saved{x, y, pending, look, concealed, modes.origin, special, spare, shifted};
}

// Restores what `saved` kept; without a save, the top left corner and the default look.
void Emulator::Held::restored() {
    const Saved& from = modes.alternate ? alternate_saved : primary_saved;

    placed(from.x, from.y);

    pending = from.pending && from.x == size.wide - 1;
    look = from.look;
    concealed = from.concealed;
    modes.origin = from.origin;
    special = from.special;
    spare = from.spare;
    shifted = from.shifted;
}

// A full reset (RIS): empty screens, the history kept, the modes and the look as at the start.
void Emulator::Held::reset() {
    primary.assign(static_cast<std::size_t>(size.high), blank(size.wide));
    alternate = primary;

    modes = Modes{};
    look = Look{};
    concealed = false;
    link = 0;
    special = spare = shifted = false;
    top = 0;
    bottom = size.high - 1;
    primary_saved = alternate_saved = Saved{};

    std::fill(tabs.begin(), tabs.end(), false);

    for (int at = 8; at < size.wide; at += 8) tabs[static_cast<std::size_t>(at)] = true;

    placed(0, 0);
}

// To the alternate screen and back. `clearing` empties the alternate screen on the way in; the
// primary one waits under it unchanged.
void Emulator::Held::switched(bool to, bool clearing, Fed& fed) {
    if (modes.alternate == to) return;

    if (to && clearing) alternate.assign(static_cast<std::size_t>(size.high), blank(size.wide));

    modes.alternate = to;
    fed.switched = true;
    pending = false;
    printed = Spot{-1, -1};
}

// The end of a string. An OSC carries the title (0 and 2), the folder the shell stands in (7, as an
// address; 9;9, as a path, the way Windows Terminal takes it from cmd and PowerShell), a link (8)
// and the stages of a shell's prompt (133). The other strings are dropped, and so is one longer
// than its bound, of which nothing was kept.
void Emulator::Held::stringed(Fed& fed) {
    const std::string said = std::move(gathered);

    gathered.clear();

    if (!osc) return;

    const std::size_t semicolon = said.find(';');
    const std::string_view kind = std::string_view{said}.substr(0, semicolon);
    const std::string rest = semicolon == std::string::npos ? std::string{} : said.substr(semicolon + 1);

    if (kind == "0" || kind == "2") {
        title = rest;
        fed.titled = true;

        return;
    }

    if (kind == "7") {
        const std::string was = folder;

        foldered(rest);

        fed.moved = folder != was;

        return;
    }

    if (kind == "8") {
        linked(rest);

        return;
    }

    if (kind == "9" && rest.starts_with("9;")) {
        std::string path = rest.substr(2);

        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);

        fed.moved = path != folder;
        folder = path;

        return;
    }

    if (kind == "133") marked(rest, fed);
}

// The folder from an address `file://host/path`, its escapes undone. On Windows the path comes as
// `/C:/...`, and the slash before the drive goes.
void Emulator::Held::foldered(const std::string& said) {
    constexpr std::string_view scheme = "file://";

    if (!said.starts_with(scheme)) return;

    const std::size_t slash = said.find('/', scheme.size());

    if (slash == std::string::npos) return;

    std::string path = hex::unescaped(std::string_view{said}.substr(slash));

    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);

    folder = path;
}

// A link (OSC 8, `params;address`): the cells written next carry its address, until a link with
// no address ends it. An address longer than its bound, or with anything but the printable
// characters of ASCII an address is made of, gives no link.
void Emulator::Held::linked(std::string_view said) {
    const std::size_t semicolon = said.find(';');

    link = 0;

    if (semicolon == std::string_view::npos) return;

    const std::string_view address = said.substr(semicolon + 1);

    if (address.empty() || address.size() > most_address) return;

    if (!std::all_of(address.begin(), address.end(), [](char one) { return one >= 0x20 && one <= 0x7E; })) return;

    link = interned(address);
}

// The number of the link to `address`, the same for the same address. When the addresses would
// pass their bound, those no cell holds any longer go first; when that is not enough, there is no
// link (0) and the text stays without it.
std::uint32_t Emulator::Held::interned(std::string_view address) {
    if (const auto found = known.find(address); found != known.end()) return found->second;

    if (link_bytes + address.size() > most_links) swept();

    if (link_bytes + address.size() > most_links) return 0;

    std::uint32_t made = 0;

    if (!unused.empty()) {
        made = unused.back();
        unused.pop_back();
        addresses[made - 1] = std::string{address};
    } else {
        addresses.emplace_back(address);
        made = static_cast<std::uint32_t>(addresses.size());
    }

    known.emplace(std::string{address}, made);
    link_bytes += address.size();

    return made;
}

// Forgets the addresses of links no cell holds, on either screen or in the history. The cells
// written next carry none meanwhile, since `linked` ends the link before it asks for a new one.
void Emulator::Held::swept() {
    std::vector<bool> used(addresses.size() + 1, false);
    const auto use = [&used](std::uint32_t one) {
        if (one < used.size()) used[one] = true;
    };

    for (const std::vector<Line>* lines : {&primary, &alternate})
        for (const Line& line : *lines)
            for (const Cell& cell : line.cells) use(cell.link);

    for (const Packed& line : history)
        for (const Streak& run : line.runs) use(run.link);

    for (std::uint32_t one = 1; one <= addresses.size(); ++one) {
        if (used[one] || addresses[one - 1].empty()) continue;

        link_bytes -= addresses[one - 1].size();
        known.erase(addresses[one - 1]);
        addresses[one - 1].clear();
        unused.push_back(one);
    }
}

// A stage of a shell's prompt (OSC 133): A where the prompt begins, B where the command line does,
// C where the command's output does, D with the exit code when it ends. The screen keeps where the
// last prompt began, so the prompt can be left out of the picture while the shell waits there.
void Emulator::Held::marked(const std::string& said, Fed& fed) {
    if (said.empty()) return;

    Staged made;

    switch (said.front()) {
        case 'A':
            made.stage = Stage::prompt;

            if (!modes.alternate) prompt = first() + static_cast<std::uint64_t>(y);

            break;
        case 'B': made.stage = Stage::input; break;
        case 'C': made.stage = Stage::output; break;
        case 'D':
            made.stage = Stage::done;

            if (said.size() > 2 && said[1] == ';') {
                int code = 0;
                bool any = false;

                for (std::size_t at = 2; at < said.size() && said[at] >= '0' && said[at] <= '9'; ++at) {
                    code = std::min(code * 10 + (said[at] - '0'), most);
                    any = true;
                }

                if (any) made.code = code;
            }

            break;
        default: return;
    }

    stage = made.stage;
    fed.stages.push_back(made);
}

// A new size, its sides within 1 and 1024. A new width lays the text of the primary screen and of
// the history out anew (`reflowed`); a new height alone keeps the lines (`heightened`). The
// alternate screen is cut or widened at its right and grows and shrinks at its bottom: its program
// draws it anew at once anyway. The scroll region goes back to the whole screen.
void Emulator::Held::resize(Size given) {
    given = Size{sided(given.wide), sided(given.high)};

    if (given == size) return;

    printed = Spot{-1, -1};

    for (Line& one : alternate) {
        one.cells.resize(static_cast<std::size_t>(given.wide));
        one.wrapped = one.spilled = false;
        mended(one);
    }

    alternate.resize(static_cast<std::size_t>(given.high), blank(given.wide));

    if (given.wide != size.wide) reflowed(given);
    else heightened(given.high);

    tabs.resize(static_cast<std::size_t>(given.wide), false);

    for (int at = (size.wide + 7) / 8 * 8; at < given.wide; at += 8) tabs[static_cast<std::size_t>(at)] = true;

    size = given;
    top = 0;
    bottom = size.high - 1;

    if (modes.alternate) placed(x, y);
}

// A new height of the primary screen, its width the same. A lower screen first drops the empty
// lines under its cursor and then sends its top lines into the history, so the cursor and what is
// around it stay in sight; a higher one takes lines back from the history, so what scrolled away
// comes back, as in xterm. While a full-screen program holds the alternate screen, the primary one
// does the same around the cursor saved for it.
void Emulator::Held::heightened(int high) {
    int& row = modes.alternate ? primary_saved.y : y;
    int excess = size.high - high;

    while (excess > 0 && static_cast<int>(primary.size()) - 1 > row && vacant(primary.back())) {
        primary.pop_back();
        --excess;
    }

    for (; excess > 0; --excess, --row) {
        remembered(std::move(primary.front()));
        primary.erase(primary.begin());
    }

    for (int room = high - static_cast<int>(primary.size()); room > 0 && !history.empty(); --room, ++row) {
        primary.insert(primary.begin(), unpacked(history.back(), size.wide));
        history.pop_back();
    }

    primary.resize(static_cast<std::size_t>(high), blank(size.wide));
    row = std::clamp(row, 0, high - 1);
}

// A new width of the primary screen: the text of the screen and of the history laid out anew. A
// line that went on in the next one (`wrapped`) joins it, and the text of each line so joined fills
// rows of the new width; a line's vacant cells at its end are no text. The cursor, the cursor saved
// for the primary screen and the row where the prompt began stay by the same character; a cursor
// past the text keeps its distance from it. Then the height is taken as `heightened` takes it: the
// rows from the one where the screen began, without the empty ones under the cursor, and as many
// more from the history as the screen has room for.
void Emulator::Held::reflowed(Size given) {
    const bool own = !modes.alternate;
    const std::size_t start = history.size();
    std::vector<Carried> places{{start + static_cast<std::size_t>(own ? y : primary_saved.y), own ? x : primary_saved.x, own ? pending : primary_saved.pending},
                              {start + static_cast<std::size_t>(primary_saved.y), primary_saved.x, primary_saved.pending},
                              {start, 0, false}};
    const std::uint64_t base = dropped;
    const bool prompted = prompt && *prompt >= base && *prompt - base < start + primary.size();

    if (prompted) places.push_back(Carried{static_cast<std::size_t>(*prompt - base), 0, false});

    std::vector<Carried> moved(places.size());
    Flow flow{given.wide};
    std::deque<Packed> old = std::move(history);

    history.clear();

    const std::size_t count = old.size() + primary.size();
    const auto line = [&](std::size_t at) { return at < old.size() ? unpacked(old[at], size.wide) : std::move(primary[at - old.size()]); };

    // The places wanted in the line being laid out: which, the index of its cell in the line's
    // text, whether the cursor waited there.
    struct Wanted {
        std::size_t which = 0;
        std::size_t index = 0;
        bool waiting = false;
    };

    const auto settled = [&](const Wanted& one, Carried got) {
        if (one.waiting) {
            if (got.column >= given.wide - 1) {
                got.column = given.wide - 1;
                got.waiting = true;
            } else {
                ++got.column;
            }
        }

        moved[one.which] = got;
    };

    for (std::size_t at = 0; at < count;) {
        std::vector<Wanted> wanted;
        std::size_t index = 0;
        bool going = true;

        for (; going && at < count; ++at) {
            const Line one = line(at);

            going = one.wrapped && at + 1 < count;

            std::size_t end = one.cells.size();

            if (going && one.spilled && end > 0) --end;

            if (!going)
                while (end > 0 && vacant(one.cells[end - 1])) --end;

            for (std::size_t which = 0; which < places.size(); ++which)
                if (places[which].row == at)
                    wanted.push_back(Wanted{which, index + static_cast<std::size_t>(std::max(places[which].column, 0)), places[which].waiting});

            for (std::size_t cell = 0; cell < end; ++cell, ++index) {
                if (one.cells[cell].cluster.empty()) continue;

                const bool two = wide(one.cells[cell]) && cell + 1 < end && one.cells[cell + 1].cluster.empty();
                const Carried got = flow.put(one.cells[cell], two ? &one.cells[cell + 1] : nullptr);

                for (const Wanted& each : wanted) {
                    if (each.index == index) settled(each, got);
                    else if (two && each.index == index + 1) settled(each, Carried{got.row, given.wide >= 2 ? got.column + 1 : got.column, false});
                }
            }
        }

        // A place past the text: a waiting cursor stands after its cell, which `past` says.
        const auto after = [](const Wanted& one) { return one.index + (one.waiting ? 1 : 0); };

        std::sort(wanted.begin(), wanted.end(), [&after](const Wanted& one, const Wanted& two) { return after(one) < after(two); });

        for (const Wanted& each : wanted) {
            if (each.index < index) continue;

            moved[each.which] = flow.past(after(each) - index);
            index = after(each);
        }

        flow.ended();
    }

    std::vector<Packed>& rows = flow.rows();
    const std::size_t cursor = moved[0].row;
    std::size_t from = moved[2].row;
    std::size_t shown = rows.size() - from;
    const auto high = static_cast<std::size_t>(given.high);

    while (shown > high && rows.size() - 1 > cursor && rows.back().sizes.empty()) {
        rows.pop_back();
        --shown;
    }

    if (shown > high) from += shown - high;

    while (rows.size() - from < high && from > 0) --from;

    for (std::size_t at = 0; at < from; ++at) history.push_back(std::move(rows[at]));

    dropped = base;

    if (kept == 0) {
        dropped += history.size();
        history.clear();
    }

    while (history.size() > kept) {
        history.pop_front();
        ++dropped;
    }

    primary.clear();

    for (std::size_t at = from; at < rows.size() && primary.size() < high; ++at) primary.push_back(unpacked(rows[at], given.wide));

    primary.resize(high, blank(given.wide));

    const auto row = [&](const Carried& one) { return static_cast<int>(std::min<std::size_t>(one.row >= from ? one.row - from : 0, high - 1)); };
    const auto column = [&](const Carried& one) { return std::clamp(one.column, 0, given.wide - 1); };
    const Carried& saved_place = own ? moved[1] : moved[0];

    primary_saved.y = row(saved_place);
    primary_saved.x = column(saved_place);
    primary_saved.pending = saved_place.waiting;

    if (own) {
        y = row(moved[0]);
        x = column(moved[0]);
        pending = moved[0].waiting && modes.wrap;
    }

    if (prompted) prompt = base + moved[3].row;
}

// An emulator of an empty screen of `size`, its sides within 1 and 1024, keeping `kept` lines of
// history.
Emulator::Emulator(Size size, std::size_t kept) : held_(std::make_unique<Held>(size, kept)) {}

// Nothing to release beyond the state it holds.
Emulator::~Emulator() = default;

// Takes the state of `other`.
Emulator::Emulator(Emulator&& other) noexcept = default;

// Takes the state of `other` in place of its own.
Emulator& Emulator::operator=(Emulator&& other) noexcept = default;

// Feeds what the program wrote. Text in UTF-8 may break off in the middle of a character at the end
// of one chunk and go on in the next, so the unfinished bytes wait for it (`text::span`). What is
// not UTF-8 becomes one replacement character for each maximal subpart, and the byte that broke a
// character off is read anew.
Fed Emulator::feed(std::span<const std::byte> bytes) {
    Held& held = *held_;
    Fed fed;

    for (const std::byte each : bytes) {
        const auto one = static_cast<unsigned char>(each);

        for (bool anew = true; anew;) {
            anew = false;

            if (held.partial.empty() && one < 0x80) {
                held.step(one, fed);

                break;
            }

            held.partial.push_back(static_cast<char>(one));

            const text::Span got = text::span(held.partial);

            if (got.kind == text::Span::Kind::partial) break;

            if (got.kind == text::Span::Kind::whole) {
                std::size_t at = 0;

                held.step(text::decoded(held.partial, at).value_or(U'\uFFFD'), fed);
                held.partial.clear();

                break;
            }

            anew = got.size < held.partial.size();

            held.partial.clear();
            held.step(U'\uFFFD', fed);
        }
    }

    return fed;
}

// A new size, its sides within 1 and 1024 (`Held::resize`).
void Emulator::resize(Size size) { held_->resize(size); }

// The number of columns and rows.
Size Emulator::size() const { return held_->size; }

// How many lines of history lie above the screen. The alternate screen has none: a full-screen
// program draws its whole picture itself.
std::size_t Emulator::history() const { return held_->modes.alternate ? 0 : held_->history.size(); }

// A row of the screen, from 0 at the top, or of the history above it, from -1 for the newest, as
// many cells as the screen is wide. A row outside both is a row of vacant cells.
Emulator::Row Emulator::row(int at) const {
    const Held& held = *held_;

    if (at >= 0) {
        if (at >= held.size.high) return Row{std::vector<Cell>(static_cast<std::size_t>(held.size.wide)), false};

        const Line& one = held.screen()[static_cast<std::size_t>(at)];

        return Row{one.cells, one.wrapped};
    }

    const std::size_t back = static_cast<std::size_t>(-static_cast<long long>(at));

    if (back > history()) return Row{std::vector<Cell>(static_cast<std::size_t>(held.size.wide)), false};

    Line one = unpacked(held.history[held.history.size() - back], held.size.wide);

    return Row{std::move(one.cells), one.wrapped};
}

// Where the cursor stands on the screen.
Spot Emulator::cursor() const { return {held_->x, held_->y}; }

// The modes the program set.
const Modes& Emulator::modes() const { return held_->modes; }

// The title the program gave (OSC 0 or 2).
const std::string& Emulator::title() const { return held_->title; }

// The folder the shell said it stands in (OSC 7 or 9;9).
const std::string& Emulator::folder() const { return held_->folder; }

// The address of the link `link` that cells carry, or nothing for 0 or a link no cell holds any
// longer.
std::string_view Emulator::address(std::uint32_t link) const {
    if (link == 0 || link > held_->addresses.size()) return {};

    return held_->addresses[link - 1];
}

// Whether the shell in the terminal waits at its prompt: it marked the prompt (OSC 133) and has not
// started a command since.
bool Emulator::waiting() const { return held_->stage == Stage::prompt || held_->stage == Stage::input; }

// The row on the screen where the prompt the shell waits at begins, when it waits and that row is
// on the screen. Whoever shows the terminal under a line of its own for typing commands can leave
// that row and those under it out, so the prompt is not shown twice.
std::optional<int> Emulator::prompt() const {
    const Held& held = *held_;

    if (!waiting() || !held.prompt || held.modes.alternate || *held.prompt < held.first()) return std::nullopt;

    const std::uint64_t row = *held.prompt - held.first();

    if (row >= static_cast<std::uint64_t>(held.size.high)) return std::nullopt;

    return static_cast<int>(row);
}

}

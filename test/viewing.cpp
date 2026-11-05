#include "proving.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/event.hpp"
#include "talcite/feed.hpp"
#include "talcite/loop.hpp"
#include "talcite/promise.hpp"
#include "talcite/root.hpp"
#include "talcite/viewer.hpp"

using namespace talcite;

namespace {

// A root to hang the viewer on.
class Sheet : public ui::Root {};


// A row of the page as a string, with spaces trimmed on the right; otherwise every comparison would
// have to add padding up to the width of the window.
std::string said(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        made += spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// A viewer holding `what` in memory, hung on `sheet`.
std::shared_ptr<ui::Viewer> shown(Sheet& sheet, const std::string& what) {
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(bytes(what));

    sheet.children().add(one);

    return one;
}

// An asynchronous source in memory, like `File::read`, only without a disk.
Feed fed(const Bytes& raw) {
    return [raw](std::size_t at, std::size_t most) {
        Bytes made;

        for (std::size_t one = at; one < raw.size() && one < at + most; ++one) made.push_back(raw[one]);

        return ready(made);
    };
}

// Turns the loop and paints until the condition becomes true, as `Term` does between frames;
// false if it does not within 4000 turns.
template <typename Fn>
bool turned(Loop& loop, Sheet& sheet, ui::Size size, Fn until) {
    for (int tries = 0; tries < 4000; ++tries) {
        (void)sheet.render({{0, 0}, size});

        if (until()) return true;

        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    return false;
}

// A text of `much` numbered lines.
std::string many(int much) {
    std::string made;

    for (int one = 0; one < much; ++one) made += "wiersz " + std::to_string(one) + "\n";

    return made;
}

}

// --- What the widget knows before layout ---------------------------------------------------------

TRIAL("Viewer: an empty one carries nothing") {
    const ui::Viewer one;

    ought(!one.held());
    ought(!one.first().has_value());
    ought(!one.lines().has_value());
    ought(!one.at().has_value());
    ought(!one.whole().has_value());
}

TRIAL("Viewer: a document in memory gives the full set of values at once") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    ought(one->held());
    same(*one->first(), std::size_t{0});
    same(*one->lines(), std::size_t{3});
    same(*one->at(), std::size_t{0});
    same(*one->whole(), std::size_t{8});
}

// The page holds the document, and a new one replaces the previous one entirely, including its row
// count and its place.
TRIAL("Viewer: a new document replaces the previous one entirely") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    one->at(std::size_t{5});

    same(*one->first(), std::size_t{2});
    same(*one->at(), std::size_t{5});

    one->hold(bytes("x\ny"));

    same(*one->lines(), std::size_t{2});
    same(*one->first(), std::size_t{0});
    same(*one->at(), std::size_t{0});
    same(*one->whole(), std::size_t{3});
}

// What can no longer be written: a bare page, and a document given to the page from outside.
static_assert(!std::is_default_constructible_v<ui::Page>);

// --- Drawing -------------------------------------------------------------------------------------

TRIAL("Viewer: rows land one below another") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi\ntrzeci");

    const Snapshot made = sheet.render({{0, 0}, {20, 4}});

    same(said(made, 0), std::string{"pierwszy"});
    same(said(made, 1), std::string{"drugi"});
    same(said(made, 2), std::string{"trzeci"});
    same(said(made, 3), std::string{});
}

TRIAL("Viewer: a tab expands to eight columns") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\tc");

    const Snapshot made = sheet.render({{0, 0}, {20, 1}});

    same(said(made, 0), std::string{"ab      c"});
}

// Wrapping is the default, so a long row takes several rows of the screen.
TRIAL("Viewer: wrapping splits a long row by columns") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc");

    ought(one->wrapped());

    const Snapshot made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"aaaa"});
    same(said(made, 1), std::string{"bbbb"});
    same(said(made, 2), std::string{"cccc"});
}

TRIAL("Viewer: without wrapping a row is cut off") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi");

    one->wrapped(false);

    const Snapshot made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"aaaa"});
    same(said(made, 1), std::string{"drug"});
}

// A Chinese character takes two cells, so cutting by bytes or by characters would let it out of the
// page. At a width of five two of them fit and one column is left.
TRIAL("Viewer: cutting counts columns, not characters") {
    Sheet sheet;

    const auto one = shown(sheet, "\xe4\xbd\xa0\xe4\xbd\xa0\xe4\xbd\xa0");

    const Snapshot made = sheet.render({{0, 0}, {5, 3}});

    same(said(made, 0), std::string{"\xe4\xbd\xa0\xe4\xbd\xa0"});
    same(said(made, 1), std::string{"\xe4\xbd\xa0"});
}

// --- Scrolling -----------------------------------------------------------------------------------

TRIAL("Viewer: a step down and up returns to the same place") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->slide(5);
    same(*one->first(), std::size_t{5});

    one->slide(-5);
    same(*one->first(), std::size_t{0});
}

// The wheel under the mouse moves the view three rows a notch, down and back, as in the list and
// the scroller; the wheel goes through the root to the node under the mouse, as from the terminal.
TRIAL("Viewer: the wheel moves the view three rows a notch") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(sheet.dispatch(down));
    same(*one->first(), std::size_t{3});
    same(said(sheet.render({{0, 0}, {40, 10}}), 0), std::string{"wiersz 3"});

    ui::WheelEvent up{{1, 1}, 1};

    ought(sheet.dispatch(up));
    same(*one->first(), std::size_t{0});
}

TRIAL("Viewer: at the start one cannot go back") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    (void)sheet.render({{0, 0}, {40, 4}});

    one->slide(-5);

    same(*one->first(), std::size_t{0});
}

// A step down stops when the last row stands at the bottom of the screen, not at the top with
// emptiness below it. A document shorter than the screen does not scroll at all, and a jump to a
// row past the end lands on the last page.
TRIAL("Viewer: we do not go past the last page") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    (void)sheet.render({{0, 0}, {40, 4}});

    one->slide(1000);

    same(*one->first(), std::size_t{6});

    one->first(1000);

    same(*one->first(), std::size_t{6});

    Sheet other;

    const auto brief = shown(other, many(3));

    (void)other.render({{0, 0}, {40, 4}});

    brief->slide(1000);

    same(*brief->first(), std::size_t{0});
}

// With wrapping one row of the document is several steps of the screen, so a step does not move
// the row number every time.
TRIAL("Viewer: with wrapping a step goes by pieces") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi");

    (void)sheet.render({{0, 0}, {4, 2}});

    one->slide(1);
    same(*one->first(), std::size_t{0});

    one->slide(1);
    same(*one->first(), std::size_t{0});

    one->slide(1);
    same(*one->first(), std::size_t{1});

    // Five pieces on two screen rows: there is nowhere further to go.
    one->slide(1);
    same(*one->first(), std::size_t{1});
}

TRIAL("Viewer: a page moves by the window's height") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->focus();

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.dispatch(down));

    same(*one->first(), std::size_t{10});
}

// `slide` says whether the picture moved, and notifies only then. At the end of the document and
// with zero there is nothing to report.
TRIAL("Viewer: slide returns whether it moved, and notifies only then") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    int heard = 0;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    ought(one->slide(3));
    same(heard, 1);

    ought(!one->slide(0));
    same(heard, 1);

    ought(one->slide(-10));
    same(*one->first(), std::size_t{0});
    same(heard, 2);

    ought(!one->slide(-1));
    same(heard, 2);

    one->end();
    heard = 0;

    ought(!one->slide(1));
    same(heard, 0);
}

// The page's own keys and wheel move it by hand, a call from the program does not.
TRIAL("Viewer: a move says whether it came by hand") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    one->focus();

    (void)sheet.render({{0, 0}, {40, 10}});

    std::vector<bool> heard;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent& what) {
        heard.push_back(what.by_hand());

        return false;
    });

    ui::KeyEvent down{ui::Code::down};
    ui::WheelEvent wheel{{1, 1}, -1};

    ought(sheet.dispatch(down));
    ought(sheet.dispatch(wheel));

    same(heard, (std::vector<bool>{true, true}));

    heard.clear();

    ought(one->slide(2));
    one->first(0);
    one->end();

    same(heard, (std::vector<bool>{false, false, false}));
}

TRIAL("Viewer: end shows the last page") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->end();

    same(*one->first(), std::size_t{90});

    const Snapshot made = sheet.render({{0, 0}, {40, 10}});

    same(said(made, 9), std::string{"wiersz 99"});
}

TRIAL("Viewer: a jump to a row is clipped to the document") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->first(1000);

    same(*one->first(), std::size_t{9});
}

TRIAL("Viewer: a jump by byte lands in the right row") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    one->at(5);

    same(*one->first(), std::size_t{2});
    same(*one->at(), std::size_t{5});
}

// The same byte means something different in the two modes, so switching has to stay in the same
// row of the document rather than jump to a random place.
TRIAL("Viewer: switching wrapping does not move the row") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi\ntrzeci");

    (void)sheet.render({{0, 0}, {4, 4}});

    one->first(1);
    one->wrapped(false);

    same(*one->first(), std::size_t{1});

    const Snapshot made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"drug"});
}

// Scrolling counts the pieces and drawing builds them, with one walker. Random documents with tabs,
// double-width and combining characters and rows that are multiples of the width: after every
// step a short screen has to show the same stretch as a tall one from the top. If the counting
// drifted from the cutting by even one piece, a step would land one row off, and the end of the
// document would not stand at the bottom.
TRIAL("Viewer: stepping by pieces agrees with what is drawn") {
    std::mt19937 random{7};

    const std::vector<std::string> signs{"a", "b", " ", "\t", "\xe4\xbd\xa0", "e\xcc\x81", "\xcc\x81"};

    const auto picked = [&random](int much) { return static_cast<int>(random() % static_cast<unsigned>(much)); };

    for (int round = 0; round < 60; ++round) {
        std::string text;

        const int count = 1 + picked(12);

        for (int line = 0; line < count; ++line) {
            const int much = picked(25);

            for (int one = 0; one < much; ++one) text += signs[static_cast<std::size_t>(picked(7))];

            text += '\n';
        }

        text += "Z";

        const int wide = 3 + picked(8);
        const int high = 2 + picked(4);

        Sheet tall;

        const auto whole = shown(tall, text);

        whole->scrollbar(false);

        const Snapshot all = tall.render({{0, 0}, {wide, 1000}});

        int total = 0;

        while (said(all, total) != "Z") ++total;

        ++total;

        const int last = std::max(total - high, 0);

        Sheet sheet;

        const auto one = shown(sheet, text);

        one->scrollbar(false);

        const auto agreed = [&](int from) {
            const Snapshot made = sheet.render({{0, 0}, {wide, high}});

            for (int y = 0; y < high; ++y) same(said(made, y), said(all, from + y));
        };

        agreed(0);

        for (int step = 1; step <= total; ++step) {
            one->slide(1);

            agreed(std::min(step, last));
        }

        for (int step = last - 1; step >= 0; --step) {
            one->slide(-1);

            agreed(step);
        }

        one->end();

        agreed(last);

        one->first(0);
        one->slide(high);

        agreed(std::min(high, last));
    }
}

// Moving about a document that fits in one window of the spool never goes back to the source: the
// rows are read from the window, whatever the movement.
TRIAL("Viewer: movement within a window does not go back to the source") {
    std::string text;

    for (int at = 0; at < 40; ++at) text += std::string(at % 3 == 0 ? 25 : 3, 'x') + "\n";

    const Bytes raw = bytes(text);

    int calls = 0;

    Loop loop;
    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>();

    one->hold([&calls, raw](std::size_t at, std::size_t most) {
        ++calls;

        return fed(raw)(at, most);
    },
              raw.size());

    sheet.children().add(one);

    one->scrollbar(false);

    ought(turned(loop, sheet, {10, 6}, [&one] { return one->shown() > 0; }));

    const int was = calls;

    for (int step = 0; step < 16; ++step) one->slide(4);

    for (int step = 0; step < 16; ++step) one->slide(-5);

    one->first(39);
    one->end();

    (void)sheet.render({{0, 0}, {10, 6}});

    same(calls, was);
}

// A frame reads only what it shows. A line of four megabytes, wrapped, shows its first rows, and the
// source is asked for one window, not for the line; scrolling down through it asks for the windows
// it passes, no more.
TRIAL("Viewer: a frame of a very long line reads only what it shows") {
    const Bytes raw = bytes(std::string(4u * 1024 * 1024, 'x'));

    int calls = 0;

    Loop loop;
    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>();

    one->hold([&calls, raw](std::size_t at, std::size_t most) {
        ++calls;

        return fed(raw)(at, most);
    },
              raw.size());

    sheet.children().add(one);

    one->scrollbar(false);

    ought(turned(loop, sheet, {40, 10}, [&sheet] { return said(sheet.render({{0, 0}, {40, 10}}), 9) == std::string(40, 'x'); }));
    same(calls, 1);

    for (int step = 0; step < 100; ++step) one->slide(10);

    ought(turned(loop, sheet, {40, 10}, [&sheet] { return said(sheet.render({{0, 0}, {40, 10}}), 9) == std::string(40, 'x'); }));
    same(*one->at(), std::size_t{100 * 10 * 40});
    ought(calls <= 2);
}

// A source that has not answered yet stops clipping to the end: a walk of the screen that runs into
// a missing window does not know where the end is, so it clips nothing. Here the first window holds
// eight rows and the next one never comes.
TRIAL("Viewer: clipping to the end stops on a window that waits") {
    const std::string head = many(8);

    auto waiting = std::make_shared<std::vector<Settle<Bytes>>>();

    Loop loop;
    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>();

    one->hold([waiting](std::size_t, std::size_t) {
        auto [made, settle] = promised<Bytes>();

        waiting->push_back(std::move(settle));

        return std::move(made);
    },
              head.size() + 100000, bytes(head));

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {10, 5}});

    one->first(6);

    same(*one->first(), std::size_t{6});

    one->slide(1);

    same(*one->first(), std::size_t{7});
}

// A tab after the cut, alone at the end of a row, is a screen row of spaces, not emptiness that can
// be left uncounted. At a width of ten it expands to the eighth column and stays as a tail.
TRIAL("Viewer: a tab after the cut takes its own screen row") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaaaaaaaa\t\nZ");

    const Snapshot made = sheet.render({{0, 0}, {10, 3}});

    same(said(made, 0), std::string{"aaaaaaaaaa"});
    same(said(made, 1), std::string{});
    same(said(made, 2), std::string{"Z"});

    one->end();

    same(said(sheet.render({{0, 0}, {10, 3}}), 2), std::string{"Z"});
    same(*one->first(), std::size_t{0});
}

// `End` stands on the last piece of the last row: the wrapped tail of the document stands at the
// bottom of the screen, not its start at the top.
TRIAL("Viewer: end with a wrapped last row shows its tail") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nabcdefghijkl");

    (void)sheet.render({{0, 0}, {4, 2}});

    one->end();

    const Snapshot made = sheet.render({{0, 0}, {4, 2}});

    same(said(made, 0), std::string{"efgh"});
    same(said(made, 1), std::string{"ijkl"});
}

// Before the page gets its allotment there is no width and nothing to cut: a step goes by rows of
// the document, as without wrapping.
TRIAL("Viewer: before layout a step goes by rows of the document") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaaaaaa\nb\nc");

    one->slide(1);

    same(*one->first(), std::size_t{1});
}

// --- Scrollbar -----------------------------------------------------------------------------------

// The scrollbar takes one column on the right, so the content gets that much less, and that shows
// in wrapping, because a long row breaks one cell earlier.
TRIAL("Viewer: the scrollbar takes a column from the content") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbb");

    one->scrollbar(true);

    ought(one->scrollbar());

    const Snapshot made = sheet.render({{0, 0}, {5, 4}});

    // The last column belongs to the scrollbar, so we compare only the start of the row.
    same(said(made, 0).substr(0, 4), std::string{"aaaa"});
    same(said(made, 1).substr(0, 4), std::string{"bbbb"});
}

// Without the scrollbar the same content fits in five columns.
TRIAL("Viewer: without the scrollbar the content gets the whole width") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbb");

    ought(!one->scrollbar());

    const Snapshot made = sheet.render({{0, 0}, {5, 4}});

    same(said(made, 0), std::string{"aaaab"});
    same(said(made, 1), std::string{"bbb"});
}

// A scrollbar "when needed": a page that fits whole has no scrollbar and wraps to the whole width;
// a page longer than the window, also through wrapping or by a single row, has one.
TRIAL("Viewer: a when-needed scrollbar appears only with a page longer than the window") {
    const auto barred = [](const std::string& text, ui::Size size) {
        Sheet sheet;

        const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true, true});

        one->hold(bytes(text));
        sheet.children().add(one);

        const Snapshot made = sheet.render({{0, 0}, size});

        const char32_t edge = made.at({size.wide - 1, 0}).letter;

        return std::pair{edge == U'█' || edge == U'│', said(made, 0)};
    };

    const auto [fits, first] = barred("aaaabbbb", {5, 4});

    ought(!fits);
    same(first, std::string{"aaaab"});

    ought(!barred("a\nb\nc\nd", {5, 4}).first);
    ought(barred("a\nb\nc\nd\ne", {5, 4}).first);
    ought(barred("aaaabbbbccccdddde", {5, 4}).first);

    // A page scrolled to the end still has the scrollbar: the rest fits, but the start is above the
    // window.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true, true});

        one->hold(bytes("a\nb\nc\nd\ne\nf"));
        sheet.children().add(one);

        (void)sheet.render({{0, 0}, {5, 4}});

        one->focus();

        ui::KeyEvent end{ui::Code::end};

        ought(sheet.dispatch(end));

        const Snapshot made = sheet.render({{0, 0}, {5, 4}});

        same(made.at({4, 0}).letter, U'│');
        same(made.at({4, 3}).letter, U'█');
    }

    // A source that does not yet have the later rows also gives a scrollbar: the page does not know
    // it is the end, though the four rows that have arrived fill the window exactly.
    {
        const std::string head = "w0\nw1\nw2\nw3\n";

        auto waiting = std::make_shared<std::vector<Settle<Bytes>>>();

        Loop loop;
        Sheet sheet;

        const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true, true});

        one->hold([waiting](std::size_t, std::size_t) {
            auto [made, settle] = promised<Bytes>();

            waiting->push_back(std::move(settle));

            return std::move(made);
        },
                  head.size() + 100, bytes(head));
        sheet.children().add(one);

        same(sheet.render({{0, 0}, {5, 4}}).at({4, 0}).letter, U'█');
    }
}

// The scrollbar stands in the last column, from the top to the bottom.
TRIAL("Viewer: the scrollbar stands at the right edge") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    one->scrollbar(true);

    const Snapshot made = sheet.render({{0, 0}, {20, 5}});

    ought(made.at({19, 0}).letter != U' ');
    ought(made.at({19, 4}).letter != U' ');
}

// While the row count is unknown, the scrollbar goes by bytes, and its window is the span of what
// stands on the screen: from the start of the first visible row to the start of the row after the
// last. Ten rows of ten thousand bytes out of twenty is half the document, and half the track.
TRIAL("Viewer: a byte scrollbar measures its window by what is visible") {
    Loop loop;
    Sheet sheet;

    std::string text;

    for (int one = 0; one < 20; ++one) text += std::string(10000, static_cast<char>('a' + one)) + "\n";

    const Bytes raw = bytes(text);
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    one->wrapped(false);
    one->scrollbar(true);
    sheet.children().add(one);

    ought(turned(loop, sheet, {20, 10}, [&] { return said(sheet.render({{0, 0}, {20, 10}}), 9).starts_with("j"); }));
    ought(!one->lines().has_value());

    const Snapshot made = sheet.render({{0, 0}, {20, 10}});
    const char32_t block = sheet.skin().glyph("talcite.scroll.thumb");

    int thumb = 0;

    for (int y = 0; y < 10; ++y)
        if (made.at({19, y}).letter == block) ++thumb;

    ought(thumb >= 3);
}

// Intentions waiting for a window belong to the document, not to the page: a new document does
// not inherit them. Otherwise a jump by byte issued on the old file would be carried out on the new
// one as soon as its first window arrived, here at a byte far past the end of the new document,
// that is, on its last rows.
TRIAL("Viewer: a new document does not inherit the old one's intentions") {
    Loop loop;
    Sheet sheet;

    const std::string text = many(30000);
    const Bytes raw = bytes(text);
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 10}});

    one->end();
    one->at(text.find("wiersz 20000\n"));

    ought(!one->lines().has_value());

    const Bytes other = bytes(many(30));

    one->hold(fed(other), other.size());

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0) == "wiersz 0"; }));

    for (int turns = 0; turns < 50; ++turns) {
        (void)loop.bell().wait(std::chrono::milliseconds{2});
        (void)loop.turn();
        (void)sheet.render({{0, 0}, {40, 10}});
    }

    same(*one->first(), std::size_t{0});
}

// --- Marks ---------------------------------------------------------------------------------------

// A mark is a range of bytes within a row and a role from the skin. The built-in role
// `talcite.viewer.mark` reverses, so it is visible without any colors.
TRIAL("Viewer: a mark repaints exactly its range") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefgh");

    one->marks().add(ui::Mark{0, 2, 5, "talcite.viewer.mark"});

    const Snapshot made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({4, 0}).style.reverse);
    ought(!made.at({5, 0}).style.reverse);

    // The letters stay in place; only the style changes.
    same(said(made, 0), std::string{"abcdefgh"});
}

// A two-column character takes two cells, and in the row it is one three-byte character. A range
// covering it has to repaint both columns.
TRIAL("Viewer: a mark on a double-width character takes both columns") {
    Sheet sheet;

    const auto one = shown(sheet, "a\xe4\xbd\xa0" "b");

    one->marks().add(ui::Mark{0, 1, 4, "talcite.viewer.mark"});

    const Snapshot made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({0, 0}).style.reverse);
    ought(made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(!made.at({3, 0}).style.reverse);
}

// A tab is a single byte but takes several columns, and they all share its fate.
TRIAL("Viewer: a mark on a tab takes the whole expansion") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\tc");

    one->marks().add(ui::Mark{0, 2, 3, "talcite.viewer.mark"});

    const Snapshot made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({7, 0}).style.reverse);
    ought(!made.at({8, 0}).style.reverse);
}

// The set has to be ordered, or else `next` does not know which one is next.
TRIAL("Viewer: marks are ordered by row and by start") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->marks().add(ui::Mark{5, 0, 1, "a"});
    one->marks().add(ui::Mark{1, 4, 5, "b"});
    one->marks().add(ui::Mark{1, 0, 1, "c"});

    same(one->marks().many(), std::size_t{3});
    same(one->marks()[0].role, std::string{"c"});
    same(one->marks()[1].role, std::string{"b"});
    same(one->marks()[2].role, std::string{"a"});
}

TRIAL("Viewer: replacing the whole set sorts and clears the current one") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->marks().all({ui::Mark{7, 0, 1, "a"}, ui::Mark{2, 0, 1, "b"}});

    same(one->marks().many(), std::size_t{2});
    same(one->marks()[0].line, std::size_t{2});
    ought(!one->which().has_value());

    one->marks().clear();

    same(one->marks().many(), std::size_t{0});
}

// --- Stepping through marks ----------------------------------------------------------------------

TRIAL("Viewer: without marks there is nowhere to jump") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    ought(!one->step(1));
    ought(!one->step(-1));
    ought(!one->which().has_value());
}

TRIAL("Viewer: a jump takes the first mark not before the screen") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{3, 0, 1, "a"}, ui::Mark{50, 0, 1, "b"}});

    ought(one->step(1));
    same(*one->which(), std::size_t{0});

    // Row 3 was visible, so the picture did not move.
    same(*one->first(), std::size_t{0});
}

TRIAL("Viewer: a jump to a mark off the screen scrolls down") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{50, 0, 1, "a"}});

    ought(one->step(1));
    same(*one->which(), std::size_t{0});
    same(*one->first(), std::size_t{41});
}

TRIAL("Viewer: after the last mark there is no next one") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{2, 0, 1, "a"}, ui::Mark{4, 0, 1, "b"}});

    ought(one->step(1));
    ought(one->step(1));
    ought(!one->step(1));

    same(*one->which(), std::size_t{1});
}

TRIAL("Viewer: backward returns through the same marks") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{2, 0, 1, "a"}, ui::Mark{4, 0, 1, "b"}});

    ought(one->step(1));
    ought(one->step(1));
    ought(one->step(-1));

    same(*one->which(), std::size_t{0});

    ought(!one->step(-1));
}

// A scrolling jump has to notify once, even if the movement consists of two parts.
TRIAL("Viewer: a jump notifies with a single event") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{50, 0, 1, "a"}});

    int heard = 0;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    ought(one->step(1));

    same(heard, 1);
}

// --- Visible range -------------------------------------------------------------------------------

// `shown()` tells how many rows of the document are visible, and that is not the window's height
// when wrapping spreads one row over several screen rows.
TRIAL("Viewer: the visible range counts rows of the document, not of the screen") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    same(one->shown(), std::size_t{10});

    Sheet inna;

    const auto two = shown(inna, "aaaabbbbcccc\ndrugi\ntrzeci");

    (void)inna.render({{0, 0}, {4, 4}});

    same(two->shown(), std::size_t{2});
}

// --- Keys and events -----------------------------------------------------------------------------

TRIAL("Viewer: takes focus and arrows, and lets function keys through") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    ought(one->focusable());

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    ought(sheet.dispatch(down));
    same(*one->first(), std::size_t{1});

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.dispatch(f2));
}

// The event goes up, so a listener on an ancestor catches the viewer's movement. That is exactly
// how the top bar learns that it has to redraw.
TRIAL("Viewer: a shift notifies the ancestor") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    int heard = 0;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    one->slide(1);

    same(heard, 1);

    one->slide(1000);

    same(heard, 2);
}

// Standing still is not movement: at the end of the document the wheel has nothing to report.
TRIAL("Viewer: no movement notifies nobody") {
    Sheet sheet;

    const auto one = shown(sheet, many(3));

    (void)sheet.render({{0, 0}, {40, 10}});

    int heard = 0;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    one->slide(-1);

    same(heard, 0);
}

// The walk cursor is dropped lazily. The set of marks has no pointer to the page, so it has no way
// to tell it that it changed; after a change "the next mark" means something else, so the cursor
// stops making sense. The page checks this at the next question, the same way the history clips
// its list.
TRIAL("Viewer: adding a mark drops the walk cursor") {
    Sheet sheet;

    const auto one = shown(sheet, many(40));

    one->marks().add(ui::Mark{2, 0, 3, "moja"});
    one->marks().add(ui::Mark{9, 0, 3, "moja"});

    (void)sheet.render({{0, 0}, {30, 6}});

    ought(one->step(1));
    ought(one->which().has_value());

    one->marks().add(ui::Mark{20, 0, 3, "moja"});

    ought(!one->which().has_value());
}

// The set keeps marks ordered by their place in the document, whatever order they arrived in;
// otherwise "next" would have no way to tell which one is next.
TRIAL("Viewer: marks are ordered by place, not by the order of adding") {
    Sheet sheet;

    const auto one = shown(sheet, many(40));

    one->marks().add(ui::Mark{9, 0, 3, "moja"});
    one->marks().add(ui::Mark{2, 0, 3, "moja"});
    one->marks().add(ui::Mark{2, 5, 7, "moja"});

    same(one->marks().many(), std::size_t{3});
    same(one->marks()[0].line, std::size_t{2});
    same(one->marks()[0].from, std::size_t{0});
    same(one->marks()[1].from, std::size_t{5});
    same(one->marks()[2].line, std::size_t{9});

    std::size_t counted = 0;

    for (const ui::Mark& said : one->marks()) {
        (void)said;

        ++counted;
    }

    same(counted, std::size_t{3});
}

// `MarkedMixin` is a base, not just a field of the page. A foreign document widget gets both the set
// of marks and the walk cursor with its lazy dropping, without writing a single line.
TRIAL("MarkedMixin: a foreign widget gets marks and a cursor without writing anything") {
    class Mine : public ui::MarkedMixin<ui::Node> {
    public:
        // The only thing the widget adds: its own way of pointing at the current mark. `topmost()`
        // and `reach()` have default behavior in the base, so a widget that scrolls nothing gets
        // the walk through marks without writing anything.
        void stand(std::size_t given) { which(given); }

        using ui::MarkedMixin<ui::Node>::step;
    };

    Mine one;

    ought(!one.which().has_value());

    one.marks().add(ui::Mark{9, 0, 3, "moja"});
    one.marks().add(ui::Mark{2, 0, 3, "moja"});

    same(one.marks().many(), std::size_t{2});
    same(one.marks()[0].line, std::size_t{2});

    one.stand(1);

    same(*one.which(), std::size_t{1});

    // Changing the set drops the cursor, lazily, at the next question.
    one.marks().add(ui::Mark{20, 0, 3, "moja"});

    ought(!one.which().has_value());

    // The walk is in the base too. Without a cursor, `step` forward takes the first mark that does
    // not lie before the screen, and a widget that scrolls nothing has its screen from row zero, so
    // it takes the very first one.
    ought(one.step(1));
    same(*one.which(), std::size_t{0});

    ought(one.step(1));
    same(*one.which(), std::size_t{1});

    ought(one.step(-1));
    same(*one.which(), std::size_t{0});

    // It does not go outside the set, and nothing moves when it tries.
    ought(!one.step(-1));
    same(*one.which(), std::size_t{0});

    // A step larger than one skips over marks, not rows.
    ought(one.step(2));
    same(*one.which(), std::size_t{2});
}

// --- Asynchronous source -------------------------------------------------------------------------

// A viewer on a `Feed` does not wait for bytes: the first frame is empty, because the window is
// still on its way, and after a turn of the loop the rows are there, with no "refresh me", because
// `Term` paints after every callback from the loop.
TRIAL("Viewer: an asynchronous source paints emptiness, and rows after a turn of the loop") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(100));
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    const Snapshot first = sheet.render({{0, 0}, {40, 10}});

    same(said(first, 0), std::string{});
    ought(!one->lines().has_value());

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0) == "wiersz 0"; }));

    const Snapshot made = sheet.render({{0, 0}, {40, 10}});

    same(said(made, 9), std::string{"wiersz 9"});
    same(*one->at(), std::size_t{0});
    same(*one->whole(), raw.size());
}

// A warmed-up spool. Whoever read the header already has the first window: given with the source,
// it lets the first frame have rows at once, without a turn of the loop (the source's promise has
// no way to return before one, so rows in the first frame can only come from the head). The source
// is asked only for what the head does not have, from its end upward, by fetching ahead and when
// scrolling; the row count stays unknown, because the document is longer than what we have.
TRIAL("Viewer: a source with a head paints rows at once, and the source is asked only past the head") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(100));
    const auto one = std::make_shared<ui::Viewer>();
    const auto asked = std::make_shared<std::vector<std::size_t>>();

    const Feed feed = [raw, asked](std::size_t at, std::size_t most) {
        asked->push_back(at);

        return fed(raw)(at, most);
    };

    // The head: the first 40 bytes, four rows and a bit of the fifth.
    const Bytes head(raw.begin(), raw.begin() + 40);

    one->hold(feed, raw.size(), head);
    sheet.children().add(one);

    const Snapshot first = sheet.render({{0, 0}, {40, 2}});

    same(said(first, 0), std::string{"wiersz 0"});
    same(said(first, 1), std::string{"wiersz 1"});
    ought(!one->lines().has_value());
    same(*one->whole(), raw.size());

    // Past the head: scrolling row by row stops at the end of the head until the window arrives from
    // the loop, and every question to the source starts past the head.
    ought(turned(loop, sheet, {40, 2}, [&] {
        one->slide(1);

        return said(sheet.render({{0, 0}, {40, 2}}), 0) == "wiersz 8";
    }));
    ought(!asked->empty());

    for (const std::size_t at : *asked) ought(at >= 40);

    // A head longer than the document does not lie about the length: it is cut to the document.
    Sheet second;

    const auto other = std::make_shared<ui::Viewer>();
    const Bytes tiny = bytes("ab\ncd\n");
    Bytes longer = tiny;

    longer.insert(longer.end(), tiny.begin(), tiny.end());

    other->hold(feed, tiny.size(), longer);
    second.children().add(other);

    (void)second.render({{0, 0}, {40, 4}});
    other->end();

    ought(turned(loop, second, {40, 4}, [&] { return other->lines().has_value(); }));
    same(*other->lines(), std::size_t{2});
}

// `End` on a source nobody has walked through yet is an intention: the row count arrives window by
// window, and once it is there the jump finishes by itself, from the spool's handle, not from a
// second press.
TRIAL("Viewer: End on an asynchronous source finishes once the row count arrives") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(30000));
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 10}});

    one->end();

    ought(!one->lines().has_value());
    same(*one->first(), std::size_t{0});

    ought(turned(loop, sheet, {40, 10}, [&] { return one->lines().has_value() && *one->first() == 29990; }));
    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 9) == "wiersz 29999"; }));
}

// A jump by byte far beyond the window is also an intention: the spool reaches the byte once it has
// the windows, and the viewer stands on the right row without being called again.
TRIAL("Viewer: a jump by byte on an asynchronous source finishes from the handle") {
    Loop loop;
    Sheet sheet;

    const std::string text = many(30000);
    const Bytes raw = bytes(text);
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 10}});

    const std::size_t byte = text.find("wiersz 20000\n");

    one->at(byte);

    ought(turned(loop, sheet, {40, 10}, [&] { return *one->first() == 20000; }));
    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0) == "wiersz 20000"; }));
    same(*one->at(), byte);
}

// The scrollbar of a page counted in bytes: a document from a source whose rows nobody has counted
// yet has a byte scrollbar, and a thumb dragged to the end jumps to a byte, not to the row with that
// number.
TRIAL("Viewer: the thumb of a byte scrollbar jumps to a byte") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(100));
    const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{.scrollbar = true});

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0).rfind("wiersz 0", 0) == 0; }));
    ought(!one->lines().has_value());

    ui::MouseEvent grab{{39, 0}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent pull{{39, 20}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent drop{{39, 20}, ui::Press::up, ui::Mouse::left};

    (void)sheet.dispatch(grab);
    (void)sheet.dispatch(pull);
    (void)sheet.dispatch(drop);

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0).rfind("wiersz 9", 0) == 0; }));
}

// --- Long lines ----------------------------------------------------------------------------------

// Without wrapping the view goes sideways: the arrows to the sides move it a column, the rows show
// the columns from the first one shown, a shorter line shows nothing, and Home goes back to the
// first column.
TRIAL("Viewer: without wrapping the view scrolls sideways") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefghij\nxy");

    one->wrapped(false);
    one->focus();

    (void)sheet.render({{0, 0}, {4, 2}});

    one->left(3);

    Snapshot made = sheet.render({{0, 0}, {4, 2}});

    same(said(made, 0), std::string{"defg"});
    same(said(made, 1), std::string{});

    ui::KeyEvent right{ui::Code::right};

    ought(sheet.dispatch(right));
    same(one->left(), std::size_t{4});
    same(said(sheet.render({{0, 0}, {4, 2}}), 0), std::string{"efgh"});

    ui::KeyEvent home{ui::Code::home};

    ought(sheet.dispatch(home));
    same(one->left(), std::size_t{0});
    same(said(sheet.render({{0, 0}, {4, 2}}), 0), std::string{"abcd"});
}

// A tab or a double-width character cut by the left edge shows its visible part as spaces, and the
// columns after it stand where they belong.
TRIAL("Viewer: a character cut by the left edge shows its visible part") {
    Sheet sheet;

    const auto one = shown(sheet, "\tX\na\xe6\xbc\xa2Y");

    one->wrapped(false);

    (void)sheet.render({{0, 0}, {4, 2}});

    one->left(6);

    const Snapshot made = sheet.render({{0, 0}, {4, 2}});

    same(said(made, 0), std::string{"  X"});

    one->left(2);

    same(said(sheet.render({{0, 0}, {4, 2}}), 1), std::string{" Y"});
}

// A jump to a byte deep in a long wrapped line lands on the row holding it, not on the start of the
// line, and End stands on the last row of the line.
TRIAL("Viewer: a jump into a long wrapped line lands on its row") {
    Sheet sheet;

    std::string text;

    for (int at = 0; at < 10000; ++at) text += static_cast<char>('a' + at % 26);

    const auto one = shown(sheet, text);

    one->scrollbar(false);

    (void)sheet.render({{0, 0}, {10, 3}});

    one->at(std::size_t{5005});

    same(*one->at(), std::size_t{5000});
    same(said(sheet.render({{0, 0}, {10, 3}}), 0), text.substr(5000, 10));

    one->end();

    const Snapshot made = sheet.render({{0, 0}, {10, 3}});

    same(said(made, 2), text.substr(9990, 10));
    same(*one->at(), std::size_t{9970});
}

// The scrollbar counts lines while lines are short, and bytes in a document of long lines, where
// a line scrollbar would stand still over the whole length of each of them.
TRIAL("Viewer: the scrollbar counts bytes in a document of long lines") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true});

    one->hold(bytes(std::string(5000, 'a') + "\n" + std::string(5000, 'b') + "\n" + std::string(5000, 'c')));

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {11, 5}});

    one->focus();

    // The thumb dragged to the middle of the track jumps to the middle of the document in bytes:
    // into the line of b, not to its start.
    ui::MouseEvent grab{{10, 0}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent pull{{10, 2}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent drop{{10, 2}, ui::Press::up, ui::Mouse::left};

    (void)sheet.dispatch(grab);
    (void)sheet.dispatch(pull);
    (void)sheet.dispatch(drop);

    same(*one->first(), std::size_t{1});
    ought(*one->at() > 5001);
}

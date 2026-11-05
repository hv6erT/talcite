#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/diff.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};

// The text of row `y` of the view, without trailing spaces.
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

// A row of the view without the open fold sign at its end, for rows of an unfolded hunk whose text
// is the point of the trial.
std::string bare(const Snapshot& one, int y) {
    std::string made = said(one, y);

    if (const std::string sign = "▾"; made.ends_with(sign)) made.erase(made.size() - sign.size());

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// The text of a side outside its marks.
std::string kept(const ui::DiffSide& one) {
    std::string made;
    std::size_t at = 0;

    for (const ui::DiffSpan& span : one.marked) {
        made += one.text.substr(at, span.from - at);
        at = span.upto;
    }

    return made + one.text.substr(at);
}

// The text of each mark of a side, in brackets.
std::string marked(const ui::DiffSide& one) {
    std::string made;

    for (const ui::DiffSpan& span : one.marked) made += "[" + one.text.substr(span.from, span.upto - span.from) + "]";

    return made;
}

// The same words as in the library: the trial computes the longest common subsequence with a
// plain table and checks that Myers found it.
std::vector<std::string> words(std::string_view text) {
    const auto wordy = [](unsigned char sign) {
        return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z') ||
               sign >= 0x80;
    };

    std::vector<std::string> made;

    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = at + 1;

        if (wordy(static_cast<unsigned char>(text[at])))
            while (end < text.size() && wordy(static_cast<unsigned char>(text[end]))) ++end;

        made.emplace_back(text.substr(at, end - at));
        at = end;
    }

    return made;
}

// The length of the longest common subsequence of two word lists, by dynamic programming.
std::size_t common(const std::vector<std::string>& one, const std::vector<std::string>& two) {
    std::vector<std::vector<std::size_t>> made(one.size() + 1, std::vector<std::size_t>(two.size() + 1, 0));

    for (std::size_t x = 1; x <= one.size(); ++x)
        for (std::size_t y = 1; y <= two.size(); ++y)
            made[x][y] = one[x - 1] == two[y - 1] ? made[x - 1][y - 1] + 1 : std::max(made[x - 1][y], made[x][y - 1]);

    return made[one.size()][two.size()];
}

// A row comparing `left` with `right`, with the marks the library computes.
ui::DiffRow row(std::string_view left, std::string_view right) {
    auto [one, two] = ui::diffed(left, right);

    return ui::DiffRow{std::move(one), std::move(two)};
}

// A hunk without a title from the given rows, with a check box when `tickable`.
ui::DiffHunk hunk(std::vector<ui::DiffRow> rows, bool tickable = true) {
    return ui::DiffHunk{{}, std::move(rows), tickable, {}};
}

// A section heading: a hunk with a title, no rows and no check box.
ui::DiffHunk heading(std::string title) { return ui::DiffHunk{std::move(title), {}, false, {}}; }

// A source that hands out the given hunks at once, at most `most` from `from` on.
ui::DiffSource sourced(std::vector<ui::DiffHunk> all) {
    return [all = std::move(all)](std::size_t from, std::size_t most) {
        std::vector<ui::DiffHunk> made;

        for (std::size_t one = from; one < all.size() && made.size() < most; ++one) made.push_back(all[one]);

        return std::optional{made};
    };
}

// Two sections: two hunks under the first heading, one under the second.
std::vector<ui::DiffHunk> planned() {
    return {heading("Pewne (2)"),
            hunk({row("a.mkv", "A (2000)/A (2000).mkv"), row("a.srt", "A (2000)/A (2000).srt")}),
            hunk({row("b.mkv", "B (2001)/B (2001).mkv")}),
            heading("Kolizje (1)"),
            hunk({row("c.mkv", "C (2002)/")}, false)};
}

// A two-column viewer of the given hunks, added to `sheet`.
std::shared_ptr<ui::DiffViewer> shown(Sheet& sheet, std::vector<ui::DiffHunk> all) {
    const auto one = std::make_shared<ui::DiffViewer>(ui::DiffShape{false, ui::Elide::middle});

    const std::size_t much = all.size();

    one->source(sourced(std::move(all)), much);

    sheet.children().add(one);

    return one;
}

// Sends a key to the sheet; whether anybody took it.
bool pressed(Sheet& sheet, ui::Key key) {
    ui::KeyEvent what{key};

    return sheet.dispatch(what);
}

}

// --- Difference of texts -------------------------------------------------------------------------

TRIAL("Diff: equal texts have no differences") {
    const auto [one, two] = ui::diffed("Heat (1995).mkv", "Heat (1995).mkv");

    ought(one.marked.empty());
    ought(two.marked.empty());
    same(one.text, std::string{"Heat (1995).mkv"});
}

TRIAL("Diff: a move into a directory highlights only the directory") {
    const auto [one, two] = ui::diffed("a.mkv", "Dir/a.mkv");

    ought(one.marked.empty());
    same(marked(two), std::string{"[Dir/]"});
}

TRIAL("Diff: the difference goes by words, not by letters") {
    const auto [one, two] = ui::diffed("The.Matrix.1999.1080p.BluRay.mkv", "The Matrix (1999).mkv");

    same(kept(one), kept(two));
    ought(marked(one).find("1080p.BluRay") != std::string::npos);
    ought(kept(one).find("Matrix") != std::string::npos);
    ought(kept(one).find("1999") != std::string::npos);
}

TRIAL("Diff: letters with diacritics belong to the word") {
    const auto [one, two] = ui::diffed("zażółć", "zażółć gęślą");

    ought(one.marked.empty());
    same(marked(two), std::string{"[ gęślą]"});
}

TRIAL("Diff: an empty side means all of the other one") {
    const auto [one, two] = ui::diffed("", "abc def");

    ought(one.marked.empty());
    same(marked(two), std::string{"[abc def]"});

    const auto [three, four] = ui::diffed("abc", "");

    same(marked(three), std::string{"[abc]"});
    ought(four.marked.empty());
}

// Myers finds the longest common subsequence: on random texts of a few words the unmarked parts
// of both sides are equal, and the number of kept words is exactly the length of the common
// subsequence computed with a table.
TRIAL("Diff: the kept words are the longest common subsequence") {
    std::mt19937 gen(3);

    const std::vector<std::string> pieces{"a", "b", "cd", "ef", ".", " ", "(", ")", "1999", "ż"};

    for (int round = 0; round < 500; ++round) {
        std::string left;
        std::string right;

        const int much_left = static_cast<int>(gen() % 12);
        const int much_right = static_cast<int>(gen() % 12);

        for (int one = 0; one < much_left; ++one) left += pieces[gen() % pieces.size()];
        for (int one = 0; one < much_right; ++one) right += pieces[gen() % pieces.size()];

        const auto [one, two] = ui::diffed(left, right);

        same(kept(one), kept(two));
        same(words(kept(one)).size() <= common(words(left), words(right)), true);
        same(words(left).size() - words(kept(one)).size() + words(right).size() - words(kept(two)).size(),
             words(left).size() + words(right).size() - 2 * common(words(left), words(right)));

        for (const ui::DiffSpan& span : one.marked) ought(span.from < span.upto && span.upto <= left.size());
        for (std::size_t at = 1; at < one.marked.size(); ++at) ought(one.marked[at - 1].upto < one.marked[at].from);
    }
}

// Too many differences: the middle between the common start and end is marked in full.
TRIAL("Diff: with many differences the whole middle is marked") {
    std::string left = "X ";
    std::string right = "X ";

    for (int one = 0; one < 400; ++one) {
        left += "a" + std::to_string(one) + " ";
        right += "b" + std::to_string(one) + " ";
    }

    left += "Y";
    right += "Y";

    const auto [one, two] = ui::diffed(left, right);

    same(one.marked.size(), std::size_t{1});
    same(one.marked.front().from, std::size_t{2});
    same(one.marked.front().upto, left.size() - 2);
    same(two.marked.size(), std::size_t{1});
    same(kept(one), std::string{"X  Y"});
}

// --- View ----------------------------------------------------------------------------------------

TRIAL("DiffViewer: an empty one carries nothing") {
    const ui::DiffViewer one;

    same(one.hunks(), std::size_t{0});
    ought(!one.which().has_value());
    ought(!one.unified().has_value());
    ought(one.scrollbar());
}

// The scroll bar is there only when there is something to scroll: a plan that fits entirely has
// none; a plan one row longer has one. The column for the bar stays, so that the sides do not jump.
TRIAL("DiffViewer: a scroll bar only when the plan does not fit") {
    const auto barred = [](std::vector<ui::DiffHunk> all, int high) {
        Sheet sheet;

        const auto one = std::make_shared<ui::DiffViewer>();
        const std::size_t much = all.size();

        one->source(sourced(std::move(all)), much);
        sheet.children().add(one);

        const Snapshot made = sheet.render({{0, 0}, {70, high}});

        for (int y = 0; y < high; ++y)
            if (made.at({69, y}).letter != U' ') return true;

        return false;
    };

    // The plan: two section headings and three hunks (the first folded), five rows.
    ought(!barred(planned(), 5));
    ought(!barred(planned(), 12));
    ought(barred(planned(), 4));

    std::vector<ui::DiffHunk> long_plan;

    for (int one = 0; one < 30; ++one) long_plan.push_back(hunk({row("x" + std::to_string(one), "y")}));

    ought(barred(long_plan, 10));

    // At the end of a long plan the rest fits, but the start is above the window, so the bar
    // stays.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::DiffViewer>();
        const std::size_t much = long_plan.size();

        one->source(sourced(long_plan), much);
        sheet.children().add(one);

        (void)sheet.render({{0, 0}, {70, 10}});

        one->focus();

        ought(pressed(sheet, ui::Code::end));

        const Snapshot made = sheet.render({{0, 0}, {70, 10}});

        bool bar = false;

        for (int y = 0; y < 10; ++y) bar = bar || made.at({69, y}).letter != U' ';

        ought(bar);
    }

    // A source that makes us wait for further hunks gives a bar too.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::DiffViewer>();

        one->source(
            [](std::size_t from, std::size_t) -> std::optional<std::vector<ui::DiffHunk>> {
                if (from >= 2) return std::nullopt;

                return std::vector<ui::DiffHunk>{hunk({row("a", "b")}), hunk({row("c", "d")})};
            },
            5);
        sheet.children().add(one);

        const Snapshot made = sheet.render({{0, 0}, {70, 10}});

        bool bar = false;

        for (int y = 0; y < 10; ++y) bar = bar || made.at({69, y}).letter != U' ';

        ought(bar);
    }
}

// A header with the sides' titles, a section title in its own row, check boxes at the hunks that
// have them, and "+N" with the closed fold sign at a folded hunk with several rows; unfolded, the
// hunk carries the open sign instead, and a hunk of one row carries none. The cursor stands on the first hunk with
// rows, not on a section heading.
TRIAL("DiffViewer: two sides, sections, check boxes and folding") {
    Sheet sheet;

    const auto one = shown(sheet, planned());

    one->titles("Było", "Będzie");

    const Snapshot made = sheet.render({{0, 0}, {70, 8}});

    same(one->which(), std::optional<std::size_t>{1});

    ought(said(made, 0).find("Było") != std::string::npos);
    ought(said(made, 0).find("Będzie") != std::string::npos);
    ought(said(made, 1).find("Pewne (2)") != std::string::npos);
    ought(said(made, 2).rfind("[ ] a.mkv", 0) == 0);
    ought(said(made, 2).find("+1 ▸") != std::string::npos);
    ought(said(made, 2).find("A (2000)/A (2000).mkv") != std::string::npos);
    ought(said(made, 3).rfind("[ ] b.mkv", 0) == 0);
    ought(said(made, 3).find("▸") == std::string::npos && said(made, 3).find("▾") == std::string::npos);
    ought(said(made, 4).find("Kolizje (1)") != std::string::npos);
    ought(said(made, 5).rfind("    c.mkv", 0) == 0);
    same(said(made, 6), std::string{});

    one->opened(1, true);

    const Snapshot open = sheet.render({{0, 0}, {70, 8}});

    ought(said(open, 2).find("+1") == std::string::npos);
    ought(said(open, 2).find("▾") != std::string::npos);
    ought(said(open, 3).find("a.srt") != std::string::npos);
    ought(said(open, 3).find("A (2000).srt") != std::string::npos);
    ought(one->opened(1));
    ought(!one->opened(2));
}

// When narrow, the view switches to one column by itself: the removed side with a minus, the added
// one with a plus, an equal row once, without a sign. Either view can be forced on request.
TRIAL("DiffViewer: a narrow view switches to one column") {
    Sheet sheet;

    const auto one = shown(sheet, {hunk({row("a.mkv", "A/a.mkv"), row("same.txt", "same.txt")}, false)});

    one->opened(0, true);

    const Snapshot narrow = sheet.render({{0, 0}, {40, 5}});

    same(bare(narrow, 0), std::string{"- a.mkv"});
    same(said(narrow, 1), std::string{"+ A/a.mkv"});
    same(said(narrow, 2), std::string{"  same.txt"});

    one->unified(false);

    const Snapshot split = sheet.render({{0, 0}, {40, 5}});

    ought(said(split, 0).rfind("a.mkv", 0) == 0);
    ought(said(split, 0).find("A/a.mkv") != std::string::npos);

    one->unified(true);

    same(one->unified(), std::optional<bool>{true});

    const Snapshot wide = sheet.render({{0, 0}, {120, 5}});

    same(bare(wide, 0), std::string{"- a.mkv"});

    one->unified(std::nullopt);

    ought(said(sheet.render({{0, 0}, {120, 5}}), 0).find("A/a.mkv") != std::string::npos);
}

// Space toggles the box of the hunk under the cursor and notifies; on a hunk without a box it does
// nothing. + and - set the whole section of the cursor, and only that one.
TRIAL("DiffViewer: space, plus and minus toggle the boxes") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all = planned();

    all.push_back(hunk({row("d.mkv", "D/d.mkv")}));

    const auto one = shown(sheet, all);

    (void)sheet.render({{0, 0}, {70, 10}});

    one->focus();

    int heard = 0;

    const ui::Hook heard_pick = sheet.on<ui::PickEvent>([&heard](ui::PickEvent&) {
        ++heard;

        return false;
    });

    ought(pressed(sheet, ui::Key{U' '}));
    ought(one->chosen().contains(1));
    same(heard, 1);

    ought(pressed(sheet, ui::Key{U'+'}));
    ought(one->chosen().contains(2));
    ought(!one->chosen().contains(5));
    same(heard, 2);

    ought(pressed(sheet, ui::Key{U'+'}));
    same(heard, 2);

    one->which(4);

    ought(pressed(sheet, ui::Key{U' '}));
    ought(!one->chosen().contains(4));
    same(heard, 2);

    ought(pressed(sheet, ui::Key{U'+'}));
    ought(one->chosen().contains(5));
    ought(one->chosen().contains(1));
    same(one->chosen().many(), std::size_t{3});

    one->which(1);

    ought(pressed(sheet, ui::Key{U'-'}));
    same(one->chosen().many(), std::size_t{1});
    ought(one->chosen().contains(5));

    ought(said(sheet.render({{0, 0}, {70, 10}}), 5).rfind("[x] d.mkv", 0) == 0);
}

// The arrows move through hunks with rows, skipping headings; Ctrl with an arrow jumps through
// sections; the side arrows fold and unfold. Tab and Enter go up.
TRIAL("DiffViewer: keys move through hunks and sections") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all = planned();

    all.push_back(hunk({row("d.mkv", "D/d.mkv")}));

    const auto one = shown(sheet, all);

    (void)sheet.render({{0, 0}, {70, 10}});

    one->focus();

    int moved = 0;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&moved](ui::ScrollEvent&) {
        ++moved;

        return false;
    });

    ought(pressed(sheet, ui::Code::down));
    same(one->which(), std::optional<std::size_t>{2});
    same(moved, 1);

    ought(pressed(sheet, ui::Code::down));
    same(one->which(), std::optional<std::size_t>{4});

    ought(pressed(sheet, ui::Code::up));
    same(one->which(), std::optional<std::size_t>{2});

    ought(pressed(sheet, ui::Key{ui::Code::down, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{4});

    ought(pressed(sheet, ui::Key{ui::Code::down, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{4});

    one->which(5);

    ought(pressed(sheet, ui::Key{ui::Code::up, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{4});

    ought(pressed(sheet, ui::Key{ui::Code::up, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{1});

    ought(pressed(sheet, ui::Code::end));
    same(one->which(), std::optional<std::size_t>{5});

    ought(pressed(sheet, ui::Code::home));
    same(one->which(), std::optional<std::size_t>{1});

    // At either end the keys stay with the viewer, as with a list: in the plan window, an arrow
    // pressed once too often must not carry the focus off to a button.
    ought(pressed(sheet, ui::Code::up));
    ought(pressed(sheet, ui::Code::home));
    same(one->which(), std::optional<std::size_t>{1});

    one->which(5);

    ought(pressed(sheet, ui::Code::down));
    ought(pressed(sheet, ui::Code::end));
    same(one->which(), std::optional<std::size_t>{5});

    one->which(1);

    ought(pressed(sheet, ui::Code::right));
    ought(one->opened(1));

    ought(pressed(sheet, ui::Code::left));
    ought(!one->opened(1));

    ought(!pressed(sheet, ui::Code::tab));
    ought(!pressed(sheet, ui::Code::enter));
    ought(!pressed(sheet, ui::Key{U'x', ui::Mods{.alt = true}}));
}

// The viewer's own keys and wheel move it by hand, a call from the program does not.
TRIAL("DiffViewer: a move says whether it came by hand") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all = planned();

    for (int at = 0; at < 6; ++at) all.push_back(hunk({row("d.mkv", "D/d.mkv")}));

    const auto one = shown(sheet, all);

    (void)sheet.render({{0, 0}, {70, 6}});

    one->focus();

    std::vector<bool> heard;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent& what) {
        heard.push_back(what.by_hand());

        return false;
    });

    ought(pressed(sheet, ui::Code::down));

    ui::WheelEvent wheel{{2, 2}, -1};

    ought(sheet.dispatch(wheel));

    same(heard, (std::vector<bool>{true, true}));

    heard.clear();

    one->which(0);

    same(heard, (std::vector<bool>{false}));
}

// A long list: the cursor going down drags the view along, and the end lands at the bottom of the
// screen, not at the top with emptiness below. The wheel moves the view alone.
TRIAL("DiffViewer: the view follows the cursor and does not go past the end") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all;

    for (int one = 0; one < 30; ++one) all.push_back(hunk({row("f" + std::to_string(one), "F" + std::to_string(one))}));

    const auto one = shown(sheet, all);

    one->unified(false);

    (void)sheet.render({{0, 0}, {40, 5}});

    one->focus();

    for (int step = 0; step < 7; ++step) ought(pressed(sheet, ui::Code::down));

    same(one->which(), std::optional<std::size_t>{7});

    const Snapshot seen = sheet.render({{0, 0}, {40, 5}});

    ought(said(seen, 4).rfind("[ ] f7", 0) == 0);
    ought(said(seen, 0).rfind("[ ] f3", 0) == 0);

    ought(pressed(sheet, ui::Code::end));

    const Snapshot last = sheet.render({{0, 0}, {40, 5}});

    ought(said(last, 4).rfind("[ ] f29", 0) == 0);
    ought(said(last, 0).rfind("[ ] f25", 0) == 0);

    ought(pressed(sheet, ui::Code::home));

    ui::WheelEvent wheel{{0, 0}, -2};

    ought(sheet.dispatch(wheel));
    same(one->which(), std::optional<std::size_t>{0});
    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f6", 0) == 0);

    one->which(29);

    ui::WheelEvent more{{0, 0}, -2};

    ought(!sheet.dispatch(more));
}

// Line numbers stand at the sides that carry them, aligned to the largest visible one. An empty
// side (a row added or removed) stays empty.
TRIAL("DiffViewer: line numbers and empty sides") {
    Sheet sheet;

    ui::DiffSide nine{"dziewięć", {}, 9};
    ui::DiffSide ten{"dziesięć", {}, 10};
    ui::DiffSide added{"nowy", {ui::DiffSpan{0, 4}}, 11};

    const auto one = shown(sheet, {hunk({ui::DiffRow{nine, nine}, ui::DiffRow{ten, std::nullopt},
                                         ui::DiffRow{std::nullopt, added}},
                                        false)});

    one->opened(0, true);
    one->unified(false);

    const Snapshot made = sheet.render({{0, 0}, {60, 4}});

    ought(said(made, 0).rfind(" 9 dziewięć", 0) == 0);
    ought(said(made, 1).rfind("10 dziesięć", 0) == 0);
    ought(said(made, 2).find("11 nowy") != std::string::npos);
    ought(said(made, 2).rfind("  ", 0) == 0);

    one->unified(true);

    const Snapshot flat = sheet.render({{0, 0}, {60, 4}});

    same(bare(flat, 0), std::string{" 9  9   dziewięć"});
    same(said(flat, 1), std::string{"10    - dziesięć"});
    same(said(flat, 2), std::string{"   11 + nowy"});
}

// Eliding in the middle keeps the marks with their letters: after eliding, the tail still shows as
// added, and the start, shared with the other side, does not.
TRIAL("DiffViewer: eliding in the middle keeps the marks with their letters") {
    Sheet sheet;

    const std::string long_one = "wspolny/poczatek/bardzo/dlugiej/sciezki/az/do/pliku.mkv";

    const auto one = shown(sheet, {hunk({row("pliku.mkv", long_one)}, false)});

    one->unified(true);

    const Snapshot made = sheet.render({{0, 0}, {24, 2}});

    const std::string line = said(made, 1);

    ought(line.find("…") != std::string::npos || line.find("~") != std::string::npos);
    ought(line.find("pliku.mkv") != std::string::npos);

    // The start of the added path is highlighted, because the first side has nothing there; the
    // tail is not, because it is shared.
    const Cell head = made.at({2, 1});
    const Cell tail = made.at({23, 1});

    ought(head.style.bold);
    ought(!tail.style.bold);
}

// A source that makes us wait leaves an empty view; after `refresh()` what arrived is visible.
TRIAL("DiffViewer: a source that waits, and refreshing") {
    Sheet sheet;

    bool ready = false;

    const auto one = std::make_shared<ui::DiffViewer>(ui::DiffShape{false, ui::Elide::middle});

    one->source(
        [&ready](std::size_t, std::size_t) -> std::optional<std::vector<ui::DiffHunk>> {
            if (!ready) return {};

            return std::vector<ui::DiffHunk>{hunk({row("a", "b")})};
        },
        1);

    sheet.children().add(one);

    ought(!one->which().has_value());
    same(said(sheet.render({{0, 0}, {40, 3}}), 0), std::string{});

    ready = true;

    one->refresh();

    ought(said(sheet.render({{0, 0}, {40, 3}}), 0).rfind("[ ] - a", 0) == 0);
    same(one->which(), std::optional<std::size_t>{0});
}

// A click puts the cursor on the hunk under it, and a click on the box toggles it. A click on a
// section heading does nothing.
TRIAL("DiffViewer: a click moves the cursor and toggles the box") {
    Sheet sheet;

    const auto one = shown(sheet, planned());

    (void)sheet.render({{0, 0}, {70, 8}});

    ui::ClickEvent row_click{{10, 2}, ui::Mouse::left};

    ought(sheet.dispatch(row_click));
    same(one->which(), std::optional<std::size_t>{2});
    ought(one->chosen().empty());

    ui::ClickEvent box{{1, 1}, ui::Mouse::left};

    ought(sheet.dispatch(box));
    same(one->which(), std::optional<std::size_t>{1});
    ought(one->chosen().contains(1));

    ui::ClickEvent title{{1, 0}, ui::Mouse::left};

    ought(sheet.dispatch(title));
    same(one->which(), std::optional<std::size_t>{1});
}

// A new source clears the choices and the unfolding; fewer hunks trim the choices when painting.
TRIAL("DiffViewer: a new source starts clean") {
    Sheet sheet;

    const auto one = shown(sheet, planned());

    one->chosen().add(1);
    one->opened(1, true);

    one->source(sourced(planned()), 5);

    ought(one->chosen().empty());
    ought(!one->opened(1));

    one->chosen().add(4);

    one->source(sourced({hunk({row("a", "b")})}), 1);

    one->chosen().add(0);

    (void)sheet.render({{0, 0}, {40, 3}});

    same(one->chosen().many(), std::size_t{1});
    same(one->hunks(), std::size_t{1});
}

// A letter whose second byte is 0x80 (U+0100) still belongs to the word: a word that differs
// entirely is highlighted entirely, not from the middle of a letter.
TRIAL("Diff: a letter with the byte 0x80 does not split a word") {
    const auto [one, two] = ui::diffed("\xc4\x80" "a", "\xc4\x80" "b");

    same(marked(one), std::string{"[\xc4\x80" "a]"});
    same(marked(two), std::string{"[\xc4\x80" "b]"});
}

// A middle longer than the limit is marked in full, however few differences there are; here only
// the first and the last words differ, so there is no common start or end.
TRIAL("Diff: a very long middle is marked in full") {
    std::string middle;

    for (int one = 0; one < 3000; ++one) middle += " w" + std::to_string(one);

    const auto [one, two] = ui::diffed("A" + middle + " B", "C" + middle + " D");

    same(one.marked.size(), std::size_t{1});
    same(one.marked.front().from, std::size_t{0});
    same(one.marked.front().upto, one.text.size());

    const auto [three, four] = ui::diffed("A w1 w2 B", "C w1 w2 D");

    same(marked(three), std::string{"[A][B]"});
    same(marked(four), std::string{"[C][D]"});
}

// A hunk with a title and rows (this is how a hunk of a file comparison looks: @@ ... @@ and rows
// under it) shows the title in its own row; an entirely empty hunk is an empty row the cursor
// passes over; a tab expands to the next multiple of eight; a mark in the middle of a text is
// highlighted only where it is.
TRIAL("DiffViewer: a title above rows, an empty hunk, a tab and a mark in the middle") {
    Sheet sheet;

    ui::DiffHunk titled{"@@ -1,2 +1,2 @@", {row("x a", "x b a"), row("t\tu", "t\tu")}, false, {}};

    const auto one = shown(sheet, {titled, ui::DiffHunk{}, hunk({row("k", "k")}, false)});

    one->opened(0, true);
    one->unified(true);

    const Snapshot made = sheet.render({{0, 0}, {40, 8}});

    same(said(made, 0), std::string{"@@ -1,2 +1,2 @@"});
    same(bare(made, 1), std::string{"- x a"});
    same(said(made, 2), std::string{"+ x b a"});
    same(said(made, 3), std::string{"  t       u"});
    same(said(made, 4), std::string{});
    same(said(made, 5), std::string{"  k"});

    ought(!made.at({2, 2}).style.bold);
    ought(made.at({4, 2}).style.bold);
    ought(!made.at({6, 2}).style.bold);

    one->focus();

    same(one->which(), std::optional<std::size_t>{0});

    ought(pressed(sheet, ui::Code::down));
    same(one->which(), std::optional<std::size_t>{2});
}

// A hunk scrolled halfway by the wheel: moving the cursor onto it puts its start at the top. A
// click where the box would be, at a hunk without one, selects nothing.
TRIAL("DiffViewer: a jump to a hunk scrolled halfway, and a click beside the box") {
    Sheet sheet;

    std::vector<ui::DiffRow> rows;

    for (int one = 0; one < 6; ++one) rows.push_back(row("r" + std::to_string(one), "r" + std::to_string(one)));

    const auto one = shown(sheet, {hunk(rows, false), hunk({row("n", "m")}, false), hunk({row("o", "o")}, false),
                                   hunk({row("p", "p")}, false)});

    one->opened(0, true);
    one->unified(false);

    (void)sheet.render({{0, 0}, {40, 4}});

    ui::WheelEvent wheel{{0, 0}, -1};

    ought(sheet.dispatch(wheel));
    ought(said(sheet.render({{0, 0}, {40, 4}}), 0).rfind("r3", 0) == 0);

    one->which(1);
    one->which(0);

    ought(said(sheet.render({{0, 0}, {40, 4}}), 0).rfind("r0", 0) == 0);

    ui::ClickEvent box{{1, 0}, ui::Mouse::left};

    ought(sheet.dispatch(box));
    ought(one->chosen().empty());
}

// The mouse on the diff scroll bar: the track scrolls by a page, the thumb to a hunk; the view
// moves, the cursor does not. A click on the bar does not put the cursor on the
// row under it.
TRIAL("DiffViewer: the mouse on the scroll bar scrolls the view, not the cursor") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all;

    for (int one = 0; one < 30; ++one) all.push_back(hunk({row("f" + std::to_string(one), "F" + std::to_string(one))}));

    const auto one = std::make_shared<ui::DiffViewer>(ui::DiffShape{true, ui::Elide::middle});

    one->source(sourced(std::move(all)), 30);
    one->unified(false);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 5}});

    one->focus();

    const auto clicked = [&sheet](ui::Spot at) {
        ui::MouseEvent down{at, ui::Press::down, ui::Mouse::left};
        ui::MouseEvent up{at, ui::Press::up, ui::Mouse::left};

        (void)sheet.dispatch(down);
        (void)sheet.dispatch(up);
    };

    clicked({39, 4});

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f5", 0) == 0);

    clicked({39, 3});

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f10", 0) == 0);

    clicked({39, 0});

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f5", 0) == 0);

    ui::MouseEvent grab{{39, 1}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent pull{{39, 9}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent drop{{39, 9}, ui::Press::up, ui::Mouse::left};

    (void)sheet.dispatch(grab);
    (void)sheet.dispatch(pull);
    (void)sheet.dispatch(drop);

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f25", 0) == 0);

    same(one->which(), std::optional<std::size_t>{0});
}

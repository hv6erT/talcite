#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tal/diff.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/key.hpp"
#include "tal/scene.hpp"
#include "tal/views.hpp"

#include "tal/testing.hpp"

// Differences: of two strings by words (what is kept is the longest common subsequence), of two
// texts by lines in hunks with their context, and the view of hunks on a scene, in two columns and
// in one, with its keys, its mouse, its ticks and its folding, following the changes of its model.

namespace {

using namespace std::chrono_literals;

using tal::Changes;
using tal::Code;
using tal::Diff;
using tal::Document;
using tal::Hooks;
using tal::Hunk;
using tal::Key;
using tal::Mark;
using tal::Mods;
using tal::Pair;
using tal::Side;
using tal::Size;
using tal::Spot;
using tal::testing::Scene;

// The text of `one` outside its marks.
std::string kept(const Side& one) {
    std::string made;
    std::size_t at = 0;

    for (const Mark& mark : one.marks) {
        made += one.text.substr(at, mark.from - at);
        at = mark.upto;
    }

    return made + one.text.substr(at);
}

// The text of each mark of `one`, in brackets.
std::string marked(const Side& one) {
    std::string made;

    for (const Mark& mark : one.marks) made += "[" + one.text.substr(mark.from, mark.upto - mark.from) + "]";

    return made;
}

// `text` in words as the library takes them, for a check by another method.
std::vector<std::string> words(std::string_view text) {
    const auto wordy = [](unsigned char sign) { return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z') || sign >= 0x80; };
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

// The length of the longest common subsequence of `one` and `two`, by a table.
std::size_t common(const std::vector<std::string>& one, const std::vector<std::string>& two) {
    std::vector<std::vector<std::size_t>> made(one.size() + 1, std::vector<std::size_t>(two.size() + 1, 0));

    for (std::size_t x = 1; x <= one.size(); ++x)
        for (std::size_t y = 1; y <= two.size(); ++y) made[x][y] = one[x - 1] == two[y - 1] ? made[x - 1][y - 1] + 1 : std::max(made[x - 1][y], made[x][y - 1]);

    return made[one.size()][two.size()];
}

// The rows of `hunks` as text: "=" for both sides equal, "-" for the left alone, "+" for the right
// alone, "~" for both sides that differ, each with its numbers and texts.
std::vector<std::string> spelled(const std::vector<Hunk>& hunks) {
    std::vector<std::string> made;

    for (const Hunk& hunk : hunks) {
        made.push_back("@");

        for (const Pair& row : hunk.rows) {
            const auto side = [](const std::optional<Side>& one) { return one ? std::to_string(one->number.value_or(0)) + ":" + one->text : std::string{}; };
            const char sign = row.left && row.right ? (row.left->marks.empty() && row.right->marks.empty() ? '=' : '~') : row.left ? '-' : '+';

            made.push_back(std::string{sign} + side(row.left) + "|" + side(row.right));
        }
    }

    return made;
}

// The lines of `text` as the comparison takes them.
std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> made;

    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = text.find('\n', at);
        std::string one{text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at)};

        if (!one.empty() && one.back() == '\r') one.pop_back();

        made.push_back(one);

        if (end == std::string_view::npos) break;

        at = end + 1;
    }

    return made;
}

// Hunks held in a vector, which tells what changes.
class Fixed : public tal::Hunks {
public:
    explicit Fixed(std::vector<Hunk> given) : all(std::move(given)) {}

    std::vector<Hunk> all;

    // How many hunks there are.
    std::size_t count() const override { return all.size(); }

    // The hunk `which`.
    Hunk hunk(std::size_t which) const override { return all.at(which); }

    // Inserts `some` at `at` and tells it.
    void insert(std::size_t at, std::vector<Hunk> some) {
        const std::size_t many = some.size();

        all.insert(all.begin() + static_cast<std::ptrdiff_t>(at), some.begin(), some.end());
        told(Changes{Changes::Kind::inserted, at, many});
    }

    // Removes `many` hunks from `at` and tells it.
    void remove(std::size_t at, std::size_t many) {
        all.erase(all.begin() + static_cast<std::ptrdiff_t>(at), all.begin() + static_cast<std::ptrdiff_t>(at + many));
        told(Changes{Changes::Kind::removed, at, many});
    }

    // Replaces all with `some` and tells a reset.
    void reset(std::vector<Hunk> some) {
        all = std::move(some);
        told(Changes{Changes::Kind::reset});
    }
};

// A row of the left and right side compared by words.
Pair row(std::string_view left, std::string_view right) {
    auto [one, two] = tal::diffed(left, right);

    return Pair{std::move(one), std::move(two)};
}

// A hunk of `rows` without a title, with a box when `tickable`.
Hunk hunk(std::vector<Pair> rows, bool tickable = true) { return Hunk{{}, std::move(rows), tickable}; }

// The heading of a section: a title without rows.
Hunk heading(std::string title) { return Hunk{std::move(title), {}, false}; }

// Two sections: two hunks under the first heading, one under the second.
std::vector<Hunk> planned() {
    return {heading("Pewne (2)"),
            hunk({row("a.mkv", "A (2000)/A (2000).mkv"), row("a.srt", "A (2000)/A (2000).srt")}),
            hunk({row("b.mkv", "B (2001)/B (2001).mkv")}),
            heading("Kolizje (1)"),
            hunk({row("c.mkv", "C (2002)/")}, false)};
}

// A view of `hunks` filling a scene of `size`, shown and focused, with what it told.
struct Shown {
    Scene scene;
    Document document{scene.time()};
    std::shared_ptr<Fixed> hunks;
    Diff* diff = nullptr;
    std::vector<std::pair<std::size_t, bool>> ticked;
    std::size_t scrolled = 0;
    Hooks hooks;

    // Builds and shows it.
    explicit Shown(std::vector<Hunk> given, Size size = {70, 8}) : scene({.screen = size}), hunks(std::make_shared<Fixed>(std::move(given))) {
        diff = &document.children().add<Diff>(hunks);
        hooks.add(diff->on<tal::Ticked>([this](tal::Ticked& what) { ticked.emplace_back(what.hunk, what.on); }));
        hooks.add(diff->on<tal::Scrolled>([this](tal::Scrolled&) { ++scrolled; }));
        scene.show(document);
        diff->focus();
        scene.wait(0ms);
    }

    // Presses `key` and lets the scene settle.
    void press(Key key) {
        scene.press(key);
        scene.wait(0ms);
    }

    // The row `y` of the screen without the spaces at its end.
    std::string row(int y) const {
        std::string made = scene.row(y);

        while (!made.empty() && made.back() == ' ') made.pop_back();

        return made;
    }
};

}

TRIAL("diffed: equal strings have no marks; a move into a folder marks the folder alone") {
    const auto [one, two] = tal::diffed("Heat (1995).mkv", "Heat (1995).mkv");

    OUGHT(one.marks.empty() && two.marks.empty());
    SAME(one.text, std::string{"Heat (1995).mkv"});

    const auto [three, four] = tal::diffed("a.mkv", "Dir/a.mkv");

    OUGHT(three.marks.empty());
    SAME(marked(four), std::string{"[Dir/]"});
    SAME(four.marks.front().part, std::string{"came"});

    const auto [five, six] = tal::diffed("Dir/a.mkv", "a.mkv");

    SAME(marked(five), std::string{"[Dir/]"});
    SAME(five.marks.front().part, std::string{"gone"});
    OUGHT(!five.number && !six.number);
}

TRIAL("diffed: by words, not letters, with the letters of other scripts and the byte 0x80 in the word") {
    const auto [one, two] = tal::diffed("The.Matrix.1999.1080p.BluRay.mkv", "The Matrix (1999).mkv");

    SAME(kept(one), kept(two));
    OUGHT(marked(one).find("1080p.BluRay") != std::string::npos);
    OUGHT(kept(one).find("Matrix") != std::string::npos);

    const auto [three, four] = tal::diffed("zażółć", "zażółć gęślą");

    OUGHT(three.marks.empty());
    SAME(marked(four), std::string{"[ gęślą]"});

    const auto [five, six] = tal::diffed("a\x80" "b c", "a\x80" "b d");

    SAME(marked(five), std::string{"[c]"});
    SAME(marked(six), std::string{"[d]"});

    const auto [seven, eight] = tal::diffed("a\x80" "b", "a\x80" "c");

    SAME(marked(seven), std::string{"[a\x80" "b]"});
}

TRIAL("diffed: an empty side means all of the other") {
    const auto [one, two] = tal::diffed("", "abc def");

    OUGHT(one.marks.empty());
    SAME(marked(two), std::string{"[abc def]"});

    const auto [three, four] = tal::diffed("abc", "");

    SAME(marked(three), std::string{"[abc]"});
    OUGHT(four.marks.empty());
}

TRIAL("diffed: what is kept is the longest common subsequence of words") {
    std::mt19937 random{3};
    const std::vector<std::string> pieces{"a", "b", "cd", "ef", ".", " ", "(", ")", "1999", "ż"};

    for (int round = 0; round < 500; ++round) {
        std::string left;
        std::string right;

        for (int one = 0, much = static_cast<int>(random() % 12); one < much; ++one) left += pieces[random() % pieces.size()];
        for (int one = 0, much = static_cast<int>(random() % 12); one < much; ++one) right += pieces[random() % pieces.size()];

        const auto [one, two] = tal::diffed(left, right);

        SAME(kept(one), kept(two));
        SAME(words(kept(one)).size(), common(words(left), words(right)));

        for (const Mark& mark : one.marks) OUGHT(mark.from < mark.upto && mark.upto <= left.size());
        for (std::size_t at = 1; at < one.marks.size(); ++at) OUGHT(one.marks[at - 1].upto < one.marks[at].from);
    }
}

TRIAL("diffed: too many differences, or too many words, mark the whole middle") {
    std::string left = "X ";
    std::string right = "X ";

    for (int one = 0; one < 400; ++one) {
        left += "a" + std::to_string(one) + " ";
        right += "b" + std::to_string(one) + " ";
    }

    left += "Y";
    right += "Y";

    const auto [one, two] = tal::diffed(left, right);

    SAME(one.marks.size(), std::size_t{1});
    SAME(one.marks.front().from, std::size_t{2});
    SAME(one.marks.front().upto, left.size() - 2);
    SAME(kept(one), std::string{"X  Y"});

    std::string long_left = "X a ";
    std::string long_right = "X b ";

    for (int times = 0; times < 2100; ++times) {
        long_left += "w ";
        long_right += "w ";
    }

    long_left += "c Y";
    long_right += "d Y";

    const auto [three, four] = tal::diffed(long_left, long_right);

    SAME(three.marks.size(), std::size_t{1});
    SAME(three.marks.front().from, std::size_t{2});
    SAME(three.marks.front().upto, long_left.size() - 2);
    SAME(four.marks.size(), std::size_t{1});
    SAME(four.marks.front().upto, long_right.size() - 2);

    std::string short_left = "X a ";
    std::string short_right = "X b ";

    for (int times = 0; times < 1000; ++times) {
        short_left += "w ";
        short_right += "w ";
    }

    short_left += "c Y";
    short_right += "d Y";

    const auto [five, six] = tal::diffed(short_left, short_right);

    SAME(five.marks.size(), std::size_t{2});
    SAME(five.marks.front().from, std::size_t{2});
    SAME(five.marks.front().upto, std::size_t{3});
    SAME(five.marks.back().from, short_left.size() - 3);
    SAME(five.marks.back().upto, short_left.size() - 2);
}

TRIAL("compared: equal texts give no hunks, whatever their line ends") {
    OUGHT(tal::compared("", "").empty());
    OUGHT(tal::compared("a\nb\n", "a\nb\n").empty());
    OUGHT(tal::compared("a\r\nb\r\n", "a\nb").empty());
    OUGHT(tal::compared("a\nb", "a\nb\n").empty());
}

TRIAL("compared: a changed line with its context, numbered, its words marked") {
    const std::vector<Hunk> made = tal::compared("1\n2\n3\n4 x\n5\n6\n7\n", "1\n2\n3\n4 y\n5\n6\n7\n", 2);

    SAME(made.size(), std::size_t{1});
    OUGHT(spelled(made) == (std::vector<std::string>{"@", "=2:2|2:2", "=3:3|3:3", "~4:4 x|4:4 y", "=5:5|5:5", "=6:6|6:6"}));
    SAME(marked(*made[0].rows[2].left), std::string{"[x]"});
    SAME(made[0].rows[2].right->marks.front().part, std::string{"came"});
    OUGHT(made[0].title.empty() && !made[0].tickable);

    OUGHT(spelled(tal::compared("a\nb\n", "a\nc\n", 0)) == (std::vector<std::string>{"@", "~2:b|2:c"}));
    OUGHT(spelled(tal::compared("a\nb\n", "a\nc\n", -5)) == (std::vector<std::string>{"@", "~2:b|2:c"}));
}

TRIAL("compared: lines gone and come pair up row by row, the rest stands alone, marked whole") {
    const std::vector<Hunk> made = tal::compared("a\nb\nc\nz\n", "a\nB\nC\nD\nz\n", 1);

    OUGHT(spelled(made) == (std::vector<std::string>{"@", "=1:a|1:a", "~2:b|2:B", "~3:c|3:C", "+|4:D", "=4:z|5:z"}));
    SAME(marked(*made[0].rows[3].right), std::string{"[D]"});
    SAME(made[0].rows[3].right->marks.front().part, std::string{"came"});

    OUGHT(spelled(tal::compared("a\nb\nc\n", "a\nc\n", 1)) == (std::vector<std::string>{"@", "=1:a|1:a", "-2:b|", "=3:c|2:c"}));
    SAME(marked(*tal::compared("a\nb\nc\n", "a\nc\n", 1)[0].rows[1].left), std::string{"[b]"});
    SAME(tal::compared("a\nb\nc\n", "a\nc\n", 1)[0].rows[1].left->marks.front().part, std::string{"gone"});
    OUGHT(spelled(tal::compared("a\n\nc\n", "a\nc\n", 0)) == (std::vector<std::string>{"@", "-2:|"}));
    OUGHT(tal::compared("a\n\nc\n", "a\nc\n", 0)[0].rows[0].left->marks.empty());
    OUGHT(tal::compared("a\nc\n", "a\n\nc\n", 0)[0].rows[0].right->marks.empty());
    OUGHT(spelled(tal::compared("", "x\n", 3)) == (std::vector<std::string>{"@", "+|1:x"}));
}

TRIAL("compared: changes whose context meets share a hunk, others do not") {
    const std::string left = "1\n2\n3\n4\n5\n6\n7\n8\n9\n";

    OUGHT(spelled(tal::compared(left, "1\nX\n3\n4\n5\n6\nY\n8\n9\n", 2)) ==
          (std::vector<std::string>{"@", "=1:1|1:1", "~2:2|2:X", "=3:3|3:3", "=4:4|4:4", "=5:5|5:5", "=6:6|6:6", "~7:7|7:Y", "=8:8|8:8", "=9:9|9:9"}));
    OUGHT(spelled(tal::compared(left, "1\nX\n3\n4\n5\n6\nY\n8\n9\n", 1)) ==
          (std::vector<std::string>{"@", "=1:1|1:1", "~2:2|2:X", "=3:3|3:3", "@", "=6:6|6:6", "~7:7|7:Y", "=8:8|8:8"}));
    SAME(tal::compared(left, "X\n2\n3\n4\n5\n6\n7\n8\nY\n", 3).size(), std::size_t{2});
    SAME(tal::compared(left, "X\n2\n3\n4\n5\n6\n7\n8\nY\n", 4).size(), std::size_t{1});
}

TRIAL("compared: the hunks give both texts back, also past the bound on differences") {
    std::mt19937 random{539};
    const std::vector<std::string> pieces{"a", "b", "c", "", "a b", "ż", "\t"};

    const auto check = [](const std::string& left, const std::string& right) {
        const std::vector<Hunk> made = tal::compared(left, right, std::numeric_limits<int>::max());
        std::vector<std::string> ones;
        std::vector<std::string> twos;
        std::size_t last_one = 0;
        std::size_t last_two = 0;

        for (const Hunk& hunk : made)
            for (const Pair& row : hunk.rows) {
                if (row.left) {
                    ones.push_back(row.left->text);
                    OUGHT(*row.left->number == ++last_one);
                }

                if (row.right) {
                    twos.push_back(row.right->text);
                    OUGHT(*row.right->number == ++last_two);
                }
            }

        if (made.empty()) {
            OUGHT(lines(left) == lines(right));
        } else {
            SAME(made.size(), std::size_t{1});
            OUGHT(ones == lines(left));
            OUGHT(twos == lines(right));
        }
    };

    for (int round = 0; round < 1000; ++round) {
        std::string left;
        std::string right;

        for (int one = 0, much = static_cast<int>(random() % 10); one < much; ++one) left += pieces[random() % pieces.size()] + "\n";
        for (int one = 0, much = static_cast<int>(random() % 10); one < much; ++one) right += pieces[random() % pieces.size()] + "\n";

        check(left, right);
    }

    std::string many_left = "head\n";
    std::string many_right = "head\n";

    for (int one = 0; one < 1500; ++one) {
        many_left += "l" + std::to_string(one) + "\n";
        many_right += "r" + std::to_string(one) + "\n";
    }

    check(many_left + "tail\n", many_right + "tail\n");

    const std::vector<Hunk> made = tal::compared(many_left + "tail\n", many_right + "tail\n", 0);

    SAME(made.size(), std::size_t{1});
    SAME(made[0].rows.size(), std::size_t{1500});
}

TRIAL("Diff: two columns under the titles, boxes, the sign of folding, the current hunk lit") {
    Shown shown{planned()};

    shown.diff->left = "Teraz";
    shown.diff->right = "Potem";
    shown.scene.wait(0ms);

    OUGHT(shown.row(0).starts_with("    Teraz"));
    OUGHT(shown.row(0).find("Potem") != std::string::npos);
    OUGHT(shown.scene.at({0, 0}).look.reverse);
    SAME(shown.row(1), std::string{"    Pewne (2)"});
    OUGHT(shown.row(2).starts_with("[ ] a.mkv"));
    OUGHT(shown.row(2).find("+1 ▸") != std::string::npos);
    OUGHT(shown.row(2).find("│ A (2000)/A (2000).mkv") != std::string::npos);
    OUGHT(shown.row(3).starts_with("[ ] b.mkv"));
    SAME(shown.row(4), std::string{"    Kolizje (1)"});
    OUGHT(shown.row(5).starts_with("    c.mkv"));
    SAME(*shown.diff->current, std::size_t{1});
    OUGHT(shown.scene.at({5, 2}).look.reverse);
    OUGHT(!shown.scene.at({5, 3}).look.reverse);

    const std::optional<Spot> dir = shown.scene.where("A (2000)/");

    OUGHT(dir.has_value() && shown.scene.at(*dir).look.bold);

    shown.diff->open(1, true);
    shown.scene.wait(0ms);
    OUGHT(shown.row(3).starts_with("    a.srt"));
    OUGHT(shown.scene.at({5, 2}).look.reverse);
    OUGHT(!shown.scene.at({5, 3}).look.reverse);

    shown.document.children().add<tal::Label>("x").focus();
    shown.scene.wait(0ms);

    OUGHT(!shown.scene.at({5, 2}).look.reverse);
}

TRIAL("Diff: a narrow view, or one asked for, takes one column with - and +") {
    Shown shown{planned(), {40, 8}};

    SAME(shown.row(0), std::string{"    Pewne (2)"});
    OUGHT(shown.row(1).starts_with("[ ] - a.mkv"));
    OUGHT(shown.row(1).ends_with("+1 ▸"));
    SAME(shown.row(2), std::string{"    + A (2000)/A (2000).mkv"});

    const std::optional<Spot> gone = shown.scene.where("a.mkv");

    OUGHT(gone.has_value() && shown.scene.at(*gone).look.under);

    shown.diff->unified = false;
    shown.scene.wait(0ms);

    OUGHT(shown.row(1).find("│") != std::string::npos);

    Shown wide{planned()};

    wide.diff->unified = true;
    wide.scene.wait(0ms);

    OUGHT(wide.row(1).starts_with("[ ] - a.mkv"));

    wide.diff->left = "L";
    wide.diff->right = "R";
    wide.scene.wait(0ms);

    SAME(wide.row(0), std::string{"    L → R"});

    Shown edge{planned(), {55, 8}};

    OUGHT(edge.row(1).find("│") != std::string::npos);

    Shown under{planned(), {54, 8}};

    OUGHT(under.row(1).starts_with("[ ] - a.mkv"));

    Shown context{tal::compared("x\ny\n", "x\nz\n", 1), {40, 6}};

    OUGHT(context.row(0).starts_with("1 1   x"));

    const std::optional<Spot> sign = context.scene.where("+1 ▸");

    OUGHT(sign.has_value());
    context.scene.click({sign->x + 3, sign->y});
    context.scene.wait(0ms);
    OUGHT(context.diff->opened(0));
}

TRIAL("Diff: the arrows go over hunks with rows, Ctrl over sections, Home and End to the ends") {
    Shown shown{planned()};

    OUGHT(shown.scrolled > 0);
    shown.press(Key{Code::down, Mods{.shift = true}});
    SAME(*shown.diff->current, std::size_t{1});
    shown.press(Key{Code::down});
    SAME(*shown.diff->current, std::size_t{2});
    shown.press(Key{Code::down});
    SAME(*shown.diff->current, std::size_t{4});
    shown.press(Key{Code::down});
    SAME(*shown.diff->current, std::size_t{4});
    shown.press(Key{Code::up});
    SAME(*shown.diff->current, std::size_t{2});
    shown.press(Key{Code::home});
    SAME(*shown.diff->current, std::size_t{1});
    shown.press(Key{Code::end});
    SAME(*shown.diff->current, std::size_t{4});
    shown.press(Key{Code::pageup});
    SAME(*shown.diff->current, std::size_t{1});
    shown.press(Key{Code::pagedown});
    SAME(*shown.diff->current, std::size_t{4});
    shown.press(Key{Code::up, Mods{.ctrl = true}});
    SAME(*shown.diff->current, std::size_t{1});
    shown.press(Key{Code::up, Mods{.ctrl = true}});
    SAME(*shown.diff->current, std::size_t{1});
    shown.press(Key{Code::down, Mods{.ctrl = true}});
    SAME(*shown.diff->current, std::size_t{4});
    shown.press(Key{Code::down, Mods{.ctrl = true}});
    SAME(*shown.diff->current, std::size_t{4});
    shown.diff->current = 2;
    shown.press(Key{Code::up, Mods{.ctrl = true}});
    SAME(*shown.diff->current, std::size_t{1});
}

TRIAL("Diff: a section ends at a heading alone, not at an empty hunk, which still takes a row") {
    Shown shown{{hunk({row("a", "A")}), hunk({row("b", "B")}), Hunk{}, hunk({row("c", "C")}), heading("H"), hunk({row("d", "D")})}};

    SAME(*shown.diff->current, std::size_t{0});
    SAME(shown.row(2), std::string{});
    OUGHT(shown.row(3).starts_with("[ ] c"));

    shown.press(Key{Code::down, Mods{.ctrl = true}});
    SAME(*shown.diff->current, std::size_t{5});

    shown.diff->current = 3;
    shown.press(Key{U'+'});
    OUGHT(shown.diff->marks() == (std::vector<std::size_t>{0, 1, 3}));
    OUGHT(shown.ticked == (std::vector<std::pair<std::size_t, bool>>{{0, true}, {1, true}, {3, true}}));

    shown.press(Key{U'+'});
    SAME(shown.ticked.size(), std::size_t{3});
}

TRIAL("Diff: right opens a hunk, left closes it; Enter and Tab go on to the window") {
    Shown shown{planned()};
    std::size_t entered = 0;

    shown.hooks.add(shown.document.on<tal::KeyEvent>([&entered](tal::KeyEvent& what) {
        if (what.key.code() == Code::enter) ++entered;
    }));

    shown.press(Key{Code::right});
    OUGHT(shown.diff->opened(1));
    shown.press(Key{Code::right});
    OUGHT(shown.diff->opened(1));
    OUGHT(shown.row(1).find("▾") != std::string::npos);
    OUGHT(shown.row(2).starts_with("    a.srt"));
    OUGHT(shown.row(3).starts_with("[ ] b.mkv"));

    shown.press(Key{Code::left});
    OUGHT(!shown.diff->opened(1));
    OUGHT(shown.row(2).starts_with("[ ] b.mkv"));

    shown.press(Key{Code::enter});
    SAME(entered, std::size_t{1});
}

TRIAL("Diff: Space ticks a hunk that can be ticked, + and - the whole section, and each tells Ticked") {
    std::vector<Hunk> all = planned();

    all.push_back(hunk({row("d", "D")}));

    Shown shown{all, {70, 10}};

    shown.press(Key{U' '});
    OUGHT(shown.diff->marked(1));
    OUGHT(shown.row(1).starts_with("[x] a.mkv"));
    OUGHT(shown.ticked == (std::vector<std::pair<std::size_t, bool>>{{1, true}}));

    shown.press(Key{U'+'});
    OUGHT(shown.diff->marks() == (std::vector<std::size_t>{1, 2}));
    OUGHT(shown.ticked.back() == (std::pair<std::size_t, bool>{2, true}));

    shown.press(Key{U'-'});
    OUGHT(shown.diff->marks().empty());

    shown.press(Key{Code::end});
    SAME(*shown.diff->current, std::size_t{5});
    shown.press(Key{U'+'});
    OUGHT(shown.diff->marks() == (std::vector<std::size_t>{5}));

    shown.diff->current = 4;
    shown.press(Key{U' '});
    OUGHT(!shown.diff->marked(4));

    const std::size_t told = shown.ticked.size();

    shown.diff->mark(1, true);
    shown.diff->mark(1, true);
    OUGHT(shown.diff->marks() == (std::vector<std::size_t>{1, 5}));
    shown.diff->unmark();
    SAME(shown.ticked.size(), told);
    OUGHT(shown.diff->marks().empty());
}

TRIAL("Diff: a click makes a hunk current, ticks its box, folds it at its sign; the wheel moves the view") {
    std::vector<Hunk> all = planned();

    for (int one = 0; one < 10; ++one) all.push_back(hunk({row("x" + std::to_string(one), "y" + std::to_string(one))}));

    Shown shown{all, {70, 6}};

    shown.scene.click({20, 2});
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{2});

    shown.scene.click({1, 1});
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{1});
    OUGHT(shown.diff->marked(1));
    OUGHT(shown.ticked == (std::vector<std::pair<std::size_t, bool>>{{1, true}}));

    shown.scene.click({4, 1});
    shown.scene.wait(0ms);
    OUGHT(shown.diff->marked(1));
    SAME(shown.ticked.size(), std::size_t{1});

    shown.scene.click({20, 2}, tal::MouseButton::right);
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{1});

    const std::optional<Spot> sign = shown.scene.where("+1 ▸");

    OUGHT(sign.has_value());
    shown.scene.click({sign->x + 4, sign->y});
    shown.scene.wait(0ms);
    OUGHT(!shown.diff->opened(1));
    shown.scene.click({sign->x + 1, sign->y});
    shown.scene.wait(0ms);
    OUGHT(shown.diff->opened(1));

    shown.scene.click({0, 0});
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{1});

    const std::size_t top = shown.diff->top();

    shown.scene.wheel({10, 2}, -1, tal::Axis::horizontal);
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), top);

    shown.scene.wheel({10, 2}, -1);
    shown.scene.wait(0ms);
    OUGHT(shown.diff->top() > top);
    SAME(*shown.diff->current, std::size_t{1});

    shown.scene.wheel({10, 2}, 10);
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), std::size_t{0});
}

TRIAL("Diff: the view follows the current hunk and never shows past the end") {
    std::vector<Hunk> all;

    for (int one = 0; one < 30; ++one) all.push_back(hunk({row("l" + std::to_string(one), "r" + std::to_string(one)), row("m", "n")}));

    Shown shown{all, {70, 5}};

    SAME(shown.diff->extent(), std::size_t{30});
    SAME(shown.diff->window(), std::size_t{5});

    for (int one = 0; one < 7; ++one) shown.press(Key{Code::down});

    SAME(*shown.diff->current, std::size_t{7});
    OUGHT(shown.row(4).starts_with("[ ] l7"));

    shown.diff->top(100);
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), std::size_t{25});
    OUGHT(shown.row(4).starts_with("[ ] l29"));

    shown.diff->open(29, true);
    shown.scene.wait(0ms);
    OUGHT(shown.row(3).starts_with("[ ] l29"));
    OUGHT(shown.row(4).starts_with("    m"));

    shown.press(Key{Code::home});
    SAME(shown.diff->top(), std::size_t{0});
    OUGHT(shown.scrolled > 0);

    shown.diff->open(1, true);
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), std::size_t{0});
    OUGHT(shown.row(2).starts_with("    m"));

    std::vector<Hunk> tall;
    std::vector<Pair> many;

    for (int one = 0; one < 10; ++one) many.push_back(row("x" + std::to_string(one), "y"));

    for (int one = 0; one < 3; ++one) tall.push_back(hunk({row("l" + std::to_string(one), "r"), row("m", "n")}));

    tall.push_back(hunk(many));

    Shown high{tall, {70, 5}};

    high.diff->open(3, true);
    high.scene.wait(0ms);
    SAME(high.diff->top(), std::size_t{3});
    OUGHT(high.row(0).starts_with("[ ] x0"));
    OUGHT(high.row(4).starts_with("    x4"));

    high.scene.wheel({10, 2}, -1);
    high.scene.wait(0ms);
    OUGHT(high.row(0).starts_with("    x3"));

    high.diff->top(3);
    high.scene.wait(0ms);
    OUGHT(high.row(0).starts_with("[ ] x0"));

    high.scene.wheel({10, 2}, -1);
    high.diff->current = 3;
    high.scene.wait(0ms);
    OUGHT(high.row(0).starts_with("[ ] x0"));

    shown.diff->top(20);
    shown.diff->open(5, true);
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), std::size_t{5});
}

TRIAL("Diff: the view takes an open hunk whole when it fits, and the wheel goes up into one") {
    std::vector<Hunk> all;
    std::vector<Pair> three;

    for (int one = 0; one < 3; ++one) three.push_back(row("t" + std::to_string(one), "u"));

    for (int one = 0; one < 10; ++one) all.push_back(hunk({row("s" + std::to_string(one), "s")}));

    all.push_back(hunk(three));

    for (int one = 0; one < 5; ++one) all.push_back(hunk({row("e" + std::to_string(one), "e")}));

    Shown shown{all, {70, 5}};

    shown.diff->open(10, true);
    shown.diff->top(0);
    shown.diff->current = 10;
    shown.scene.wait(0ms);
    OUGHT(shown.row(2).starts_with("[ ] t0"));
    OUGHT(shown.row(4).starts_with("    t2"));

    std::vector<Hunk> tall{hunk({row("x0", "y"), row("x1", "y"), row("x2", "y"), row("x3", "y"), row("x4", "y")})};

    for (int one = 0; one < 10; ++one) tall.push_back(hunk({row("s" + std::to_string(one), "s")}));

    Shown high{tall, {70, 5}};

    high.diff->open(0, true);
    high.diff->top(1);
    high.scene.wait(0ms);
    high.scene.wheel({10, 2}, 1);
    high.scene.wait(0ms);
    OUGHT(high.row(0).starts_with("    x2"));
}

TRIAL("Diff: line numbers in a gutter as wide as the largest, tabs as spaces, a long side cut") {
    const std::vector<Hunk> made = tal::compared("x\n\ty\n", "x\n\tz\n", 1);
    std::vector<Hunk> all = made;

    all.push_back(hunk({Pair{Side{"left " + std::string(80, 'a'), {}, 10}, Side{"r", {}, 9}}}, false));

    Shown shown{all, {70, 6}};

    OUGHT(shown.row(0).starts_with(" 1 x"));
    OUGHT(shown.row(0).find("+1 ▸") != std::string::npos);

    shown.diff->open(0, true);
    shown.scene.wait(0ms);

    OUGHT(shown.row(1).starts_with(" 2         y"));
    OUGHT(shown.row(2).starts_with("10 left aaaa"));
    OUGHT(shown.row(2).find("…") != std::string::npos);
    OUGHT(shown.row(2).find("│  9 r") != std::string::npos);

    Shown tabs{{hunk({Pair{Side{"ab\tc", {}, {}}, Side{"ż\tx", {}, {}}}}, false)}, {70, 3}};

    OUGHT(tabs.row(0).starts_with("ab      c"));
    OUGHT(tabs.row(0).find("ż       x") != std::string::npos);
}

TRIAL("Diff: the model's changes move the view, the current hunk, the ticks and the open hunks along") {
    std::vector<Hunk> all;

    for (int one = 0; one < 6; ++one) all.push_back(hunk({row("l" + std::to_string(one), "r"), row("m", "n")}));

    Shown shown{all, {70, 10}};

    shown.diff->current = 3;
    shown.diff->mark(3, true);
    shown.diff->open(4, true);
    shown.scene.wait(0ms);

    shown.hunks->insert(1, {hunk({row("new", "new")})});
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{4});
    OUGHT(shown.diff->marked(4) && !shown.diff->marked(3));
    OUGHT(shown.diff->opened(5));

    shown.hunks->remove(0, 2);
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{2});
    OUGHT(shown.diff->marks() == (std::vector<std::size_t>{2}));
    OUGHT(shown.diff->opened(3));

    shown.hunks->remove(2, 1);
    shown.scene.wait(0ms);
    OUGHT(shown.diff->marks().empty());
    SAME(*shown.diff->current, std::size_t{2});

    shown.hunks->reset({heading("H"), hunk({row("a", "b")})});
    shown.scene.wait(0ms);
    OUGHT(shown.diff->marks().empty());
    OUGHT(!shown.diff->opened(1));
    SAME(*shown.diff->current, std::size_t{1});

    shown.hunks->reset({hunk({row("a", "b")}), heading("Z")});
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{0});

    shown.hunks->reset({});
    shown.scene.wait(0ms);
    SAME(shown.diff->extent(), std::size_t{0});
    shown.press(Key{Code::down});
    SAME(shown.row(0), std::string{});
}

TRIAL("Diff: changes at the view's first hunk and at the current one move them; a reset forgets the boxes") {
    std::vector<Hunk> all;

    for (int one = 0; one < 20; ++one) all.push_back(hunk({row("l" + std::to_string(one), "r")}));

    Shown shown{all, {70, 5}};

    shown.diff->top(10);
    shown.diff->current = 12;
    shown.diff->mark(12, true);
    shown.scene.wait(0ms);

    shown.hunks->insert(0, {hunk({row("n", "n")})});
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), std::size_t{11});
    SAME(*shown.diff->current, std::size_t{13});

    shown.hunks->insert(13, {hunk({row("n", "n")})});
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{14});
    OUGHT(shown.diff->marks() == (std::vector<std::size_t>{14}));

    shown.hunks->remove(10, 3);
    shown.scene.wait(0ms);
    SAME(shown.diff->top(), std::size_t{10});
    SAME(*shown.diff->current, std::size_t{11});

    shown.hunks->remove(10, 3);
    shown.scene.wait(0ms);
    SAME(*shown.diff->current, std::size_t{10});

    shown.hunks->reset({hunk({row("a", "b")}, false)});
    shown.scene.wait(0ms);
    OUGHT(shown.row(0).starts_with("a"));
}

TRIAL("Hunks: a handler whose hook went hears nothing more") {
    Fixed model{{hunk({row("a", "b")})}};
    std::size_t heard = 0;

    {
        const tal::Hook hook = model.on<Changes>([&heard](Changes&) { ++heard; });

        model.insert(0, {hunk({row("c", "d")})});
        SAME(heard, std::size_t{1});
    }

    model.insert(0, {hunk({row("e", "f")})});
    SAME(heard, std::size_t{1});
}

TRIAL("Diff: no hunks show nothing and take no key; a model is needed") {
    Shown shown{{}};
    bool reached = false;

    shown.hooks.add(shown.document.on<tal::KeyEvent>([&reached](tal::KeyEvent&) { reached = true; }));
    shown.press(Key{Code::down});

    OUGHT(reached);
    SAME(shown.row(0), std::string{});

    bool threw = false;

    try {
        Diff none{nullptr};
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    OUGHT(threw);
    THREW(shown.diff->open(0, true));
    THREW(shown.diff->mark(0, true));
}

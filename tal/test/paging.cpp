#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/document.hpp"
#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/pages.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"
#include "tal/views.hpp"

#include "quieting.hpp"
#include "tal/testing.hpp"

// Pagers on a scene: a text or a file read in windows, its lines wrapped or cut and moved sideways,
// the view moved by keys, the wheel and the program, never past the last page; marks in the looks
// of their parts, the current one, links followed.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::Code;
using tal::Document;
using tal::Followed;
using tal::Hooks;
using tal::Key;
using tal::Mods;
using tal::Pager;
using tal::Place;
using tal::Scrolled;
using tal::Size;
using tal::Stretch;
using tal::Text;
using tal::Unread;
using tal::testing::Scene;

// Whether `what` throws an exception of type E.
template <typename E, typename F>
bool throws(F what) {
    try {
        what();
    } catch (const E&) {
        return true;
    }

    return false;
}

// A document of `much` numbered lines, each `said` and its number.
std::string many(int much, const std::string& said = "l") {
    std::string made;

    for (int one = 0; one < much; ++one) made += said + std::to_string(one) + "\n";

    return made;
}

// A file in memory, read at once or held until let through; reads from `failing` on fail, and
// every read is noted.
class Held : public tal::fs::Opened {
public:
    explicit Held(std::string what) : raw_(tal::bytes(what)) {}

    std::vector<std::uint64_t> asked;
    std::uint64_t failing = UINT64_MAX;
    bool unsized = false;
    bool stalling = false;
    std::vector<tal::Pending<int>> gates;

    // The bytes from `at`, `most` at most; while `stalling`, once let through.
    tal::Task<Bytes> read(std::uint64_t at, std::size_t most) override {
        asked.push_back(at);

        if (stalling) {
            tal::Pending<int> gate;

            gates.push_back(gate);

            (void)co_await gate.task();
        }

        if (at >= failing) throw tal::Trouble{tal::Kind::broken, "Read failed."};

        Bytes made;

        for (std::uint64_t one = at; one < raw_.size() && one < at + most; ++one) made.push_back(raw_[static_cast<std::size_t>(one)]);

        co_return made;
    }

    // Nothing is written.
    tal::Task<void> write(std::uint64_t at, Bytes what) override {
        (void)at;
        (void)what;

        co_return;
    }

    // The size, or a failure when the file cannot tell it.
    tal::Task<std::uint64_t> size() override {
        if (unsized) throw tal::Trouble{tal::Kind::denied, "No size."};

        co_return raw_.size();
    }

    // Nothing.
    tal::Task<void> flush() override { co_return; }

    // Lets the reads waiting through, and the next ones at once.
    void release() {
        stalling = false;

        for (tal::Pending<int>& one : gates) one.settle(0);

        gates.clear();
    }

private:
    Bytes raw_;
};

// A pager filling a scene of `size`, focused, with what it told; its loop is the program's, unless
// another came first.
struct Paged {
    tal::Loop loop;
    Scene scene;
    Document document{scene.time()};
    Pager* pager = nullptr;
    std::vector<std::string> followed;
    std::vector<std::string> unread;
    int scrolled = 0;
    Hooks hooks;

    // A pager of the text `said`.
    Paged(std::string said, Size size) : scene(size) { shown(document.children().add<Pager>(std::make_shared<Text>(said))); }

    // A pager of the file `file`, its first bytes `head` at hand.
    Paged(const std::shared_ptr<Held>& file, Size size, std::string head = {}) : scene(size) {
        shown(document.children().add<Pager>(tal::fs::File{file}, tal::bytes(head)));
    }

    // Shows `one` and listens to it.
    void shown(Pager& one) {
        pager = &one;
        hooks.add(one.on<Followed>([this](Followed& what) { followed.push_back(what.address); }));
        hooks.add(one.on<Unread>([this](Unread& what) { unread.emplace_back(what.why.what()); }));
        hooks.add(one.on<Scrolled>([this](Scrolled& what) {
            (void)what;

            ++scrolled;
        }));
        scene.show(document);
        one.focus();
        scene.wait(0ms);
    }

    // Lets every read come on the program's loop, and draws.
    void settled() {
        QUIETED(tal::loop());

        scene.wait(0ms);
    }

    // The rows of the screen joined by |.
    std::string rows() {
        std::string made;

        for (int y = 0; y < scene.screen().size().high; ++y) made += (y > 0 ? "|" : "") + scene.row(y);

        return made;
    }
};

}

// The lines one under another, a tab to the next multiple of its width, a line end of \r\n as \n;
// no line after a final line end.
TRIAL("Pager: the lines of a text, and tabs") {
    Paged shown{"a\tb\r\nc\n", {12, 3}};
    Pager& pager = *shown.pager;

    SAME(shown.rows(), std::string{"a       b   |c           |            "});
    SAME(pager.lines(), std::optional<std::size_t>{2});
    SAME(pager.size(), std::optional<std::uint64_t>{6});
    OUGHT(pager.has_class("page"));
    OUGHT(pager.has_class("pager"));

    pager.tab = 4;
    shown.scene.wait(0ms);

    SAME(shown.scene.row(0), std::string{"a   b       "});

    pager.tab = 0;
    shown.scene.wait(0ms);

    SAME(shown.scene.row(0), std::string{"a b         "});
    OUGHT(throws<std::invalid_argument>([] { Pager none{std::shared_ptr<Text>{}}; }));
    OUGHT(throws<std::invalid_argument>([] { Pager none{tal::fs::File{}}; }));
}

// A pager wraps at first. Without wrapping a long line is cut at the edge, and the arrows to the
// sides move the view a column, a wide character cut by the left edge showing as a space; with
// wrapping the line takes rows, and the side arrows are not the page's.
TRIAL("Pager: wrapped, or cut and moved sideways") {
    Paged shown{"abcdefghij\nxy\n\xe6\x97\xa5\xe6\x9c\xac", {4, 4}};
    Pager& pager = *shown.pager;

    OUGHT(*pager.wrap);

    pager.wrap = false;
    shown.scene.wait(0ms);

    SAME(shown.rows(), std::string{"abcd|xy  |\xe6\x97\xa5\xe6\x9c\xac|    "});

    shown.scene.press(Key{Code::right});

    SAME(*pager.shift, std::size_t{1});
    SAME(shown.rows(), std::string{"bcde|y   | \xe6\x9c\xac |    "});

    shown.scene.press(Key{Code::left});
    shown.scene.press(Key{Code::left});

    SAME(*pager.shift, std::size_t{0});

    shown.scene.wheel({0, 0}, -1, tal::Axis::horizontal);

    SAME(*pager.shift, std::size_t{3});

    shown.scene.wheel({0, 0}, 2, tal::Axis::horizontal);

    SAME(*pager.shift, std::size_t{0});

    pager.shift = 2;
    pager.wrap = true;
    shown.scene.wait(0ms);

    SAME(*pager.shift, std::size_t{0});
    SAME(shown.rows(), std::string{"abcd|efgh|ij  |xy  "});

    shown.scene.press(Key{Code::right});
    shown.scene.wheel({0, 0}, -1, tal::Axis::horizontal);

    SAME(*pager.shift, std::size_t{0});
}

// The arrows go a row, the page keys a screen, Home and End to the ends, the wheel three rows a
// step; the view goes no further than the last line at the bottom, and tells `Scrolled` when it
// moves, only then. A line set at the top is the last when past it.
TRIAL("Pager: the view moved by keys, the wheel and the program") {
    Paged shown{many(10), {4, 3}};
    Pager& pager = *shown.pager;
    const int scrolled = shown.scrolled;

    shown.scene.press(Key{Code::down});

    SAME(pager.first(), std::size_t{1});
    SAME(shown.scrolled, scrolled + 1);

    shown.scene.press(Key{Code::pagedown});

    SAME(pager.first(), std::size_t{4});

    shown.scene.press(Key{Code::end});

    SAME(pager.first(), std::size_t{7});
    SAME(shown.rows(), std::string{"l7  |l8  |l9  "});

    const int ended = shown.scrolled;

    shown.scene.press(Key{Code::down});
    shown.scene.press(Key{Code::pagedown});

    SAME(pager.first(), std::size_t{7});
    SAME(shown.scrolled, ended);

    shown.scene.press(Key{Code::pageup});

    SAME(pager.first(), std::size_t{4});

    shown.scene.press(Key{Code::up});

    SAME(pager.first(), std::size_t{3});

    shown.scene.press(Key{Code::home});

    SAME(pager.first(), std::size_t{0});

    shown.scene.press(Key{Code::up});

    SAME(pager.first(), std::size_t{0});

    shown.scene.wheel({0, 0}, -1);

    SAME(pager.first(), std::size_t{3});

    shown.scene.wheel({0, 0}, 5);

    SAME(pager.first(), std::size_t{0});

    pager.first(100);

    SAME(pager.first(), std::size_t{7});

    pager.first(8);

    SAME(pager.first(), std::size_t{7});

    pager.first(2);
    shown.scene.wait(0ms);

    SAME(shown.rows(), std::string{"l2  |l3  |l4  "});
}

// With wrapping a step goes a row of the screen, also inside a line; the byte of the top row is the
// place in the document, and wrapping turned off goes back to the start of the top line.
TRIAL("Pager: wrapped rows stepped one by one") {
    Paged shown{"abcdefghij\nxy", {4, 2}};
    Pager& pager = *shown.pager;

    pager.wrap = true;
    shown.scene.wait(0ms);
    shown.scene.press(Key{Code::down});

    SAME(shown.rows(), std::string{"efgh|ij  "});
    SAME(pager.at(), std::uint64_t{4});

    shown.scene.press(Key{Code::down});
    shown.scene.press(Key{Code::down});

    SAME(shown.rows(), std::string{"ij  |xy  "});
    SAME(pager.at(), std::uint64_t{8});

    shown.scene.press(Key{Code::up});
    shown.scene.press(Key{Code::up});

    SAME(pager.at(), std::uint64_t{0});

    shown.scene.press(Key{Code::end});

    SAME(shown.rows(), std::string{"ij  |xy  "});

    shown.scene.press(Key{Code::up});
    pager.wrap = false;
    shown.scene.wait(0ms);

    SAME(shown.rows(), std::string{"abcd|xy  "});
}

// A jump to a byte shows the row that holds it, without wrapping its line, with wrapping its row;
// the extent and the window of the view count lines, or bytes where lines are long.
TRIAL("Pager: a jump to a byte, and the view in lines or bytes") {
    Paged shown{many(100), {4, 3}};
    Pager& pager = *shown.pager;

    pager.at(25);

    SAME(pager.first(), std::size_t{8});
    SAME(pager.at(), std::uint64_t{24});
    SAME(pager.extent(), std::size_t{100});
    SAME(pager.window(), std::size_t{3});
    SAME(pager.top(), std::size_t{8});

    pager.top(50);

    SAME(pager.first(), std::size_t{50});

    pager.at(100000);

    SAME(pager.first(), std::size_t{97});

    Paged wide{std::string(3000, 'x') + "\nend", {10, 2}};
    Pager& long_one = *wide.pager;

    long_one.wrap = true;
    long_one.at(2005);
    wide.scene.wait(0ms);

    SAME(long_one.at(), std::uint64_t{2000});
    SAME(long_one.extent(), std::size_t{3004});
    SAME(long_one.window(), std::size_t{20});
    SAME(long_one.top(), std::size_t{2000});

    long_one.top(2003);

    SAME(long_one.at(), std::uint64_t{2010});

    long_one.top(2008);

    SAME(long_one.at(), std::uint64_t{2000});

    long_one.top(2000);

    SAME(long_one.at(), std::uint64_t{2000});

    long_one.top(10);

    SAME(long_one.at(), std::uint64_t{10});
}

// A scrollbar beside a pager shows its view and moves it: a press under the thumb goes a page, the
// wheel three lines.
TRIAL("Pager: a scrollbar beside it") {
    Scene scene{{5, 4}};
    Document document{scene.time()};
    tal::Row& row = document.children().add<tal::Row>();
    Pager& pager = row.children().add<Pager>(std::make_shared<Text>(many(40)));
    tal::Scroll& bar = row.children().add<tal::Scroll>(pager);

    pager.share(tal::Share{1, 0});
    scene.show(document);
    scene.wait(0ms);

    SAME(pager.extent(), std::size_t{40});
    SAME(pager.window(), std::size_t{4});

    scene.click({4, 3});

    SAME(pager.first(), std::size_t{4});

    scene.wheel({4, 0}, -1);

    SAME(pager.first(), std::size_t{7});
    OUGHT(bar.has_class("scroll"));
}

// Marks paint their cells in the looks of their parts, the current one in the state `current`; one
// with an address links its cells, and Enter on it or a click on it tells `Followed`. Marks given
// come in order, none current; a mark past the last throws.
TRIAL("Pager: marks, the current one, links followed") {
    Paged shown{"one two\nthree", {10, 2}};
    Pager& pager = *shown.pager;

    shown.document.sheet(tal::StyleSheet::parsed(
        R"({"rules": [{"select": "pager::found", "style": {"bold": true}}, {"select": "pager:current::found", "style": {"under": true}}], "glyphs": {}})"));
    pager.marks({Stretch{Place{1, 0}, Place{1, 5}, "link", "https://x"}, Stretch{Place{0, 4}, Place{0, 7}, "found", {}}});
    shown.scene.wait(0ms);

    SAME(pager.marks().front().part, std::string{"found"});
    OUGHT(shown.scene.at({4, 0}).look.bold);
    OUGHT(shown.scene.at({6, 0}).look.bold);
    OUGHT(!shown.scene.at({3, 0}).look.bold);
    OUGHT(!shown.scene.at({7, 0}).look.bold);
    OUGHT(!shown.scene.at({4, 0}).look.under);
    OUGHT(shown.scene.at({0, 1}).link != 0);
    OUGHT(shown.scene.at({5, 1}).link == 0);
    OUGHT(shown.scene.at({0, 0}).link == 0);

    pager.reach(0);
    shown.scene.wait(0ms);

    OUGHT(shown.scene.at({4, 0}).look.under);

    shown.scene.press(Key{Code::enter});

    OUGHT(shown.followed.empty());

    pager.reach(1);
    shown.scene.press(Key{Code::enter});
    shown.scene.click({2, 1});
    shown.scene.click({6, 1});
    shown.scene.click({2, 1}, tal::MouseButton::right);

    OUGHT(shown.followed == (std::vector<std::string>{"https://x", "https://x"}));
    OUGHT(throws<std::out_of_range>([&pager] { pager.reach(2); }));

    pager.marks({});

    SAME(*pager.current, std::optional<std::size_t>{});

    shown.scene.press(Key{Code::enter});

    SAME(shown.followed.size(), std::size_t{2});
}

// Reaching a mark below the view brings it to the bottom, above to the top, one in view moves
// nothing; without wrapping the view goes sideways as far as shows it.
TRIAL("Pager: reaching marks out of view") {
    Paged shown{many(30) + std::string(20, 'z') + "!", {6, 3}};
    Pager& pager = *shown.pager;

    pager.wrap = false;

    pager.marks({Stretch{Place{5, 0}, Place{5, 1}, "found", {}}, Stretch{Place{20, 0}, Place{20, 2}, "found", {}},
                 Stretch{Place{30, 20}, Place{30, 21}, "found", {}}});
    pager.reach(1);

    SAME(pager.first(), std::size_t{18});

    const int scrolled = shown.scrolled;

    pager.reach(1);

    SAME(pager.first(), std::size_t{18});
    SAME(shown.scrolled, scrolled);

    pager.reach(0);

    SAME(pager.first(), std::size_t{5});

    pager.reach(2);
    shown.scene.wait(0ms);

    SAME(pager.first(), std::size_t{28});
    SAME(*pager.shift, std::size_t{15});
    SAME(shown.scene.row(2), std::string{"zzzzz!"});

    pager.reach(0);

    SAME(*pager.shift, std::size_t{0});
}

// A file shows nothing until its size and its first window come; then its lines, read in windows as
// the view needs them. A head at hand shows at once. End walks the file window by window and stands
// on the last page, the lines counted.
TRIAL("Pager: a file read in windows") {
    const std::string doc = many(30000);
    const auto file = std::make_shared<Held>(doc);
    Paged shown{file, {6, 2}};
    Pager& pager = *shown.pager;

    SAME(shown.rows(), std::string{"      |      "});
    SAME(pager.size(), std::optional<std::uint64_t>{});

    shown.settled();

    SAME(shown.rows(), std::string{"l0    |l1    "});
    SAME(pager.size(), std::optional<std::uint64_t>{doc.size()});
    SAME(pager.lines(), std::optional<std::size_t>{});
    SAME(file->asked.size(), std::size_t{1});

    shown.scene.press(Key{Code::end});
    shown.settled();

    SAME(shown.rows(), std::string{"l29998|l29999"});
    SAME(pager.lines(), std::optional<std::size_t>{30000});
    SAME(pager.first(), std::size_t{29998});

    pager.at(doc.size() / 2);
    shown.settled();

    OUGHT(shown.rows().starts_with("l1"));

    const auto headed = std::make_shared<Held>(doc);
    Paged quick{headed, {6, 2}, doc.substr(0, 100)};

    quick.settled();

    SAME(quick.rows(), std::string{"l0    |l1    "});
    OUGHT(headed->asked.empty() || headed->asked.front() == 100);
}

// A read that fails ends the document where it got and tells `Unread`, and nothing is read past it;
// a file that cannot tell its size shows nothing and tells it too.
TRIAL("Pager: a file that fails") {
    const std::string doc = many(30000);
    const auto file = std::make_shared<Held>(doc);

    file->failing = 64 * 1024;

    Paged shown{file, {6, 2}};

    shown.settled();
    shown.scene.press(Key{Code::end});
    shown.settled();

    OUGHT(shown.unread == (std::vector<std::string>{"Read failed."}));
    OUGHT(shown.pager->lines().has_value());
    OUGHT(*shown.pager->lines() < 30000);
    SAME(shown.pager->size(), std::optional<std::uint64_t>{64 * 1024});

    const std::size_t asked = file->asked.size();

    shown.scene.press(Key{Code::home});
    shown.scene.press(Key{Code::end});
    shown.settled();

    SAME(file->asked.size(), asked);

    const auto blind = std::make_shared<Held>(doc);

    blind->unsized = true;

    Paged none{blind, {6, 2}};

    none.settled();

    OUGHT(none.unread == (std::vector<std::string>{"No size."}));
    SAME(none.pager->size(), std::optional<std::uint64_t>{0});
    SAME(none.rows(), std::string{"      |      "});
}

// A pager of a text follows its changes, the view on its line, the last page when the text got
// shorter; one not shown yet takes its line from the program and shows it once laid out.
TRIAL("Pager: a text that changes, and a view before layout") {
    const auto text = std::make_shared<Text>(many(10));
    Scene scene{{4, 2}};
    Document document{scene.time()};
    Pager& pager = document.children().add<Pager>(text);

    pager.first(3);

    SAME(pager.first(), std::size_t{3});

    scene.show(document);
    scene.wait(0ms);

    SAME(scene.row(0), std::string{"l3  "});

    (void)text->insert(Place{0, 0}, "new\n");
    scene.wait(0ms);

    SAME(pager.first(), std::size_t{3});
    SAME(scene.row(0), std::string{"l2  "});
    SAME(pager.lines(), std::optional<std::size_t>{11});

    text->erase(Place{1, 0}, Place{10, 0});
    scene.wait(0ms);

    SAME(pager.first(), std::size_t{0});
    SAME(scene.row(0), std::string{"new "});
    SAME(scene.row(1), std::string{"l9  "});
}

// A mark on a wide character takes both its columns, on a tab all the tab's, and one cut by the
// edge what shows of it.
TRIAL("Pager: marks over wide characters and tabs") {
    Paged shown{"\xe6\x97\xa5x\ty", {12, 1}};
    Pager& pager = *shown.pager;

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "pager::found", "style": {"bold": true}}], "glyphs": {}})"));
    pager.wrap = false;
    pager.marks({Stretch{Place{0, 0}, Place{0, 3}, "found", {}}, Stretch{Place{0, 4}, Place{0, 5}, "found", {}}});
    shown.scene.wait(0ms);

    OUGHT(shown.scene.at({0, 0}).look.bold);
    OUGHT(shown.scene.at({1, 0}).look.bold);
    OUGHT(!shown.scene.at({2, 0}).look.bold);
    OUGHT(shown.scene.at({3, 0}).look.bold);
    OUGHT(shown.scene.at({7, 0}).look.bold);
    OUGHT(!shown.scene.at({8, 0}).look.bold);

    pager.shift = 1;
    shown.scene.wait(0ms);

    OUGHT(shown.scene.at({0, 0}).look.bold);
    OUGHT(!shown.scene.at({1, 0}).look.bold);
}

// A frame over a line of megabytes reads only the window it shows; until the end is known the view
// counts bytes. Without wrapping, showing the line after it walks the long line to its end, window
// by window, each window read once.
TRIAL("Pager: a line of megabytes in a file") {
    const std::size_t mega = 8U * 1024 * 1024;
    const auto file = std::make_shared<Held>(std::string(mega, 'x') + "\nend\n");
    Paged shown{file, {6, 2}};
    Pager& pager = *shown.pager;

    shown.settled();

    SAME(shown.rows(), std::string{"xxxxxx|xxxxxx"});
    OUGHT(file->asked.size() <= 2);
    SAME(pager.extent(), std::size_t{mega + 5});
    SAME(pager.top(), std::size_t{0});
    SAME(pager.window(), std::size_t{12});

    pager.wrap = false;
    shown.settled();

    SAME(shown.rows(), std::string{"xxxxxx|end   "});
    OUGHT(file->asked.size() <= mega / (64 * 1024) + 4);

    Paged one{std::make_shared<Held>(std::string(mega, 'y')), {6, 1}};

    one.pager->wrap = false;
    one.settled();

    SAME(one.rows(), std::string{"yyyyyy"});
}

// Without wrapping: tabs, a lone \r shown as a sign, \r\n and \r at the end of a file as line ends,
// a wide character left out at the right edge; one wider than a wrapped row stands in it alone, and
// a tab at the end of a full row goes to the next.
TRIAL("Pager: line ends and signs, characters at the edges") {
    Paged shown{"a\tb\nc\rd", {12, 2}};

    shown.pager->wrap = false;
    shown.scene.wait(0ms);

    SAME(shown.rows(), std::string{"a       b   |c\xe2\x90\x8d" "d         "});

    const auto file = std::make_shared<Held>("a\r\nb\r\nc\r");
    Paged ends{file, {4, 4}};

    ends.settled();

    SAME(ends.rows(), std::string{"a   |b   |c   |    "});
    SAME(ends.pager->lines(), std::optional<std::size_t>{3});

    Paged edge{"ab\xe6\x97\xa5", {3, 1}};

    edge.pager->wrap = false;
    edge.scene.wait(0ms);

    SAME(edge.scene.at({2, 0}).cluster, std::string{" "});

    Paged narrow{"\xe6\x97\xa5x", {1, 2}};

    SAME(narrow.scene.row(1), std::string{"x"});

    Paged tabbed{"abcd\tx", {4, 2}};

    SAME(tabbed.rows(), std::string{"abcd|    "});

    tabbed.scene.press(Key{Code::down});

    SAME(tabbed.rows(), std::string{"    |x   "});
}

// A cluster of many marks across the end of a piece read stays whole.
TRIAL("Pager: a cluster across pieces read") {
    std::string cluster = "a";

    for (int one = 0; one < 30; ++one) cluster += "\xcc\x81";

    std::string line;

    for (int one = 0; one < 70; ++one) line += cluster;

    Paged shown{line, {70, 1}};

    for (int x = 60; x < 70; ++x) SAME(shown.scene.at({x, 0}).cluster, cluster);
}

// Without wrapping the keys stop at the last page as with it, Home goes back to the first column,
// and keys with Alt are left to the nodes above.
TRIAL("Pager: without wrapping, at the edges") {
    Paged shown{many(10) + std::string(20, 'w'), {4, 3}};
    Pager& pager = *shown.pager;

    pager.wrap = false;
    shown.scene.press(Key{Code::end});

    SAME(pager.first(), std::size_t{8});

    shown.scene.press(Key{Code::down});

    SAME(pager.first(), std::size_t{8});

    shown.scene.press(Key{Code::right});
    shown.scene.press(Key{Code::right});
    shown.scene.press(Key{Code::home});

    SAME(*pager.shift, std::size_t{0});
    SAME(pager.first(), std::size_t{0});

    shown.scene.press(Key{Code::down, Mods{false, true, false}});

    SAME(pager.first(), std::size_t{0});
}

// The rows of a long wrapped line kept for one tab, one width or one text are not taken for
// another, and a jump to the line it stands in goes to the line's start, as wrapping turned off
// does.
TRIAL("Pager: rows of a long wrapped line kept and forgotten") {
    const auto text = std::make_shared<Text>("\t" + std::string(1000, 'y') + "\nend");
    Scene scene{{10, 2}};
    Document document{scene.time()};
    Pager& pager = document.children().add<Pager>(text);

    scene.show(document);
    pager.focus();
    scene.wait(0ms);

    for (int step = 0; step < 70; ++step) scene.press(Key{Code::down});

    SAME(pager.at(), std::uint64_t{693});

    pager.first(0);

    SAME(pager.at(), std::uint64_t{0});

    for (int step = 0; step < 71; ++step) scene.press(Key{Code::down});

    scene.press(Key{Code::up});

    SAME(pager.at(), std::uint64_t{693});

    pager.tab = 4;
    scene.press(Key{Code::up});

    SAME(pager.at(), std::uint64_t{687});

    scene.resize({15, 2});
    scene.press(Key{Code::up});

    SAME(pager.at(), std::uint64_t{672});

    for (int step = 0; step < 30; ++step) scene.press(Key{Code::down});

    scene.press(Key{Code::up});
    (void)text->insert(Place{0, 0}, "zzzzz");
    scene.wait(0ms);

    SAME(pager.at(), std::uint64_t{0});

    for (int step = 0; step < 70; ++step) scene.press(Key{Code::down});

    scene.press(Key{Code::up});

    SAME(pager.at(), std::uint64_t{988});

    pager.wrap = false;

    SAME(pager.at(), std::uint64_t{0});
}

// The head at hand shows while the reads wait; a view set where the file is not read yet shows once
// it comes, so does a jump to a byte, which then stays where it went; the end reached once is not
// gone to again; Up at the top reads nothing; and a view set on a line read only in part stays.
TRIAL("Pager: a file whose reads wait") {
    const std::string doc = many(300000);
    const auto file = std::make_shared<Held>(doc);

    file->stalling = true;

    Paged shown{file, {8, 2}, doc.substr(0, 100)};
    Pager& pager = *shown.pager;

    shown.settled();

    SAME(shown.rows(), std::string{"l0      |l1      "});

    file->release();
    shown.settled();
    pager.first(200000);
    shown.settled();

    SAME(shown.rows(), std::string{"l200000 |l200001 "});

    pager.at(doc.find("l250000\n"));
    shown.settled();

    SAME(pager.first(), std::size_t{250000});

    shown.scene.press(Key{Code::pagedown});
    shown.settled();

    SAME(pager.first(), std::size_t{250002});

    pager.first(100);
    shown.settled();

    SAME(pager.first(), std::size_t{100});

    shown.scene.press(Key{Code::end});
    shown.settled();

    SAME(pager.first(), std::size_t{299998});

    shown.scene.press(Key{Code::home});
    pager.at(doc.find("l150000\n"));
    shown.settled();

    SAME(pager.first(), std::size_t{150000});

    const auto other = std::make_shared<Held>(doc);
    Paged top{other, {8, 2}};

    top.settled();

    const std::size_t asked = other->asked.size();

    top.scene.press(Key{Code::up});
    top.scene.press(Key{Code::down});
    top.settled();

    SAME(other->asked.size(), asked);

    pager.at(doc.find("l299998\n"));
    shown.settled();

    SAME(pager.lines(), std::optional<std::size_t>{300000});

    shown.scene.press(Key{Code::home});
    shown.settled();
    shown.scene.press(Key{Code::end});
    shown.settled();

    SAME(pager.first(), std::size_t{299998});

    pager.at(doc.find("l1000\n"));
    shown.settled();
    shown.scene.press(Key{Code::end});
    shown.settled();

    SAME(pager.first(), std::size_t{299998});

    const auto cut = std::make_shared<Held>(doc);
    const std::size_t at = doc.find("l20\n");

    cut->stalling = true;

    Paged part{cut, {8, 2}, doc.substr(0, at + 2)};

    part.settled();
    part.pager->first(20);

    SAME(part.pager->first(), std::size_t{20});
}

// A catcher of what the pager leaves: keys and the wheel.
class Catcher : public tal::Column {
public:
    // Hears the wheel left to it.
    Catcher() {
        hooks_.add(otherwise<tal::MouseEvent>([this](tal::MouseEvent& what) {
            if (what.press == tal::Press::wheel) ++wheeled;
        }));
    }

    int wheeled = 0;

private:
    Hooks hooks_;
};

// Marks come in order of their starts; a mark not current is not lit with the focus, and one
// without an address neither links nor follows. A click past the end of a row in a mark with an
// address follows it, also in a framed pager. The wheel that moves the view is the page's, and the
// wheel at an edge is left to the nodes above.
TRIAL("Pager: marks clicked, and the wheel at the edges") {
    Scene scene{{10, 4}};
    Document document{scene.time()};
    Catcher& catcher = document.children().add<Catcher>();
    Pager& pager = catcher.children().add<Pager>(std::make_shared<Text>("one two\nthree\n" + many(10)));
    std::vector<std::string> followed;
    Hooks hooks;

    pager.share(tal::Share{1, 0});
    pager.wrap = false;
    hooks.add(pager.on<Followed>([&followed](Followed& what) { followed.push_back(what.address); }));
    document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "pager:focus::found", "style": {"italic": true}}], "glyphs": {}})"));
    scene.show(document);
    pager.focus();
    pager.marks({Stretch{Place{0, 5}, Place{1, 2}, "found", "https://y"}, Stretch{Place{0, 0}, Place{0, 3}, "found", {}},
                 Stretch{Place{0, 1}, Place{0, 2}, "found", {}}});
    scene.wait(0ms);

    SAME(pager.marks().front().from, (Place{0, 0}));
    SAME(pager.marks()[1].from, (Place{0, 1}));
    OUGHT(!scene.at({0, 0}).look.italic);
    SAME(scene.at({0, 0}).link, std::uint32_t{0});

    scene.click({1, 0});

    OUGHT(followed.empty());

    scene.click({8, 0});

    OUGHT(followed == (std::vector<std::string>{"https://y"}));

    scene.wheel({0, 0}, 1);

    SAME(catcher.wheeled, 1);

    scene.wheel({0, 0}, -1);

    SAME(catcher.wheeled, 1);
    SAME(pager.first(), std::size_t{3});

    scene.wheel({0, 0}, 1, tal::Axis::horizontal);

    SAME(catcher.wheeled, 2);

    pager.first(0);
    pager.add_class("drop");
    scene.wait(0ms);
    scene.click({0, 0});
    scene.click({7, 1});

    SAME(followed.size(), std::size_t{2});
}

// Reaching a mark: just past the right edge moves the view a column; deep in a wrapped line below
// the view brings its row to the bottom, in an earlier row of the line at the top to the top; the
// end of a line in view moves nothing; a mark reached before the pager is laid out shows once it
// is.
TRIAL("Pager: reaching marks in long lines") {
    Paged cut{"abcdefghij", {6, 1}};

    cut.pager->wrap = false;
    cut.pager->marks({Stretch{Place{0, 6}, Place{0, 7}, "found", {}}});
    cut.pager->reach(0);

    SAME(*cut.pager->shift, std::size_t{1});

    Paged wide{std::string(400, 'x') + "\nend", {10, 3}};
    Pager& pager = *wide.pager;

    pager.marks({Stretch{Place{0, 395}, Place{0, 396}, "found", {}}, Stretch{Place{0, 105}, Place{0, 106}, "found", {}}});
    pager.reach(1);

    SAME(pager.at(), std::uint64_t{370});

    pager.reach(0);

    SAME(pager.at(), std::uint64_t{100});

    Paged ends{many(30), {6, 5}};

    ends.pager->wrap = false;
    ends.pager->first(10);
    ends.pager->marks({Stretch{Place{12, 3}, Place{12, 4}, "found", {}}});
    ends.pager->reach(0);

    SAME(ends.pager->first(), std::size_t{10});

    const auto text = std::make_shared<Text>(many(50));
    Scene scene{{6, 3}};
    Document document{scene.time()};
    Pager& later = document.children().add<Pager>(text);

    later.marks({Stretch{Place{40, 0}, Place{40, 1}, "found", {}}});
    later.reach(0);
    scene.show(document);
    scene.wait(0ms);

    SAME(later.first(), std::size_t{38});
}

// Shifted far along a line of megabytes the view reads only around where it stands, step after
// step.
TRIAL("Pager: far along a long line without wrapping") {
    const auto file = std::make_shared<Held>(std::string(8U * 1024 * 1024, 'r') + "\n");
    Paged shown{file, {6, 1}};
    Pager& pager = *shown.pager;

    pager.wrap = false;
    pager.shift = 5000000;
    shown.settled();

    SAME(shown.rows(), std::string{"rrrrrr"});

    const std::size_t asked = file->asked.size();

    shown.scene.press(Key{Code::right});
    shown.scene.press(Key{Code::right});
    shown.settled();

    SAME(*pager.shift, std::size_t{5000002});
    OUGHT(file->asked.size() <= asked + 1);
}

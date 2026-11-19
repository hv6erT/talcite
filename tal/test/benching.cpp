#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tal/app.hpp"
#include "tal/bytes.hpp"
#include "tal/capabilities.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/document.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/geometry.hpp"
#include "tal/json.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/model.hpp"
#include "tal/node.hpp"
#include "tal/pages.hpp"
#include "tal/port.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/text.hpp"
#include "tal/views.hpp"

#include "tal/testing.hpp"

#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define TAL_CALLGRIND 1
#endif

// The measurements of talcite.next's acceptance (E3e), of the views over a large model (E4d) and of
// a pager over a line of 100 MB (E4e2). They run only when `TAL_BENCH` is set; in an ordinary run
// each passes at once. As in twa's test/benching.cpp, each measured stretch prints `BENCH <name>
// <milliseconds per repetition>`, and under callgrind (started with `--instr-atstart=no`) it is the
// only instrumented code, dumped as `<name>/<repetitions>`. The tree is built like twa's screen of
// panels on the terminal of twa's measurement: 200 by 60 cells, a menu bar, two framed panels of
// film rows side by side and a key bar, in the built-in sheet, monochrome like the skin twa was
// measured with.

namespace {

using tal::Area;
using tal::Canvas;
using tal::Code;
using tal::Document;
using tal::Limits;
using tal::Look;
using tal::Node;
using tal::Size;
using tal::Spot;
using tal::State;

// Whether the run is a measurement.
bool benching() { return std::getenv("TAL_BENCH") != nullptr; }

// Runs `work` `times` times as one measured stretch named `name` and prints its time per
// repetition; under callgrind only this stretch is instrumented.
template <typename Work>
void measured(const std::string& name, int times, Work&& work) {
    const std::string dumped = name + "/" + std::to_string(times);
    const auto began = std::chrono::steady_clock::now();

#ifdef TAL_CALLGRIND
    CALLGRIND_ZERO_STATS;
    CALLGRIND_START_INSTRUMENTATION;
#endif

    for (int at = 0; at < times; ++at) work();

#ifdef TAL_CALLGRIND
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS_AT(dumped.c_str());
#endif

    const double each = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / times;

    std::printf("BENCH %s %.4f\n", name.c_str(), each);
    std::fflush(stdout);
}

// One line of text in the look of a part of the node's style, on the whole row.
class Line : public Node {
public:
    Line(std::vector<std::string> names, std::string text, std::string part)
        : text_(std::move(text)), part_(std::move(part)) {
        classes(std::move(names));
    }

protected:
    // One row as wide as the room.
    Size measure(Limits room) const override { return Size{room.most.wide, 1}; }

    // The text from the left, in the part's look.
    void paint(Canvas& onto) const override { (void)onto.put({0, 0}, text_, look(part_)); }

private:
    std::string text_;
    std::string part_;
};

// A row of a panel: a film's name, its size and its date in columns, in the look of the table,
// reversed where the cursor stands.
class Row : public Node {
public:
    explicit Row(int number)
        : name_("Film number " + std::to_string(number) + " (19" + std::to_string(10 + number % 90) + ")"),
          size_(std::to_string(1 + number % 7) + "." + std::to_string(number % 10) + " GB"),
          date_("2026-0" + std::to_string(1 + number % 9) + "-1" + std::to_string(number % 10)) {
        classes({"table"});
    }

    // Puts the cursor on the row or takes it off.
    void cursor(bool on) { state(State::focus, on); }

protected:
    // One row as wide as the room.
    Size measure(Limits room) const override { return Size{room.most.wide, 1}; }

    // The name from the left, the size and the date in columns at the right.
    void paint(Canvas& onto) const override {
        const Look plain = look();
        const int wide = onto.size().wide;

        (void)onto.put({0, 0}, name_, plain);
        (void)onto.put({std::max(0, wide - 22), 0}, size_, plain);
        (void)onto.put({std::max(0, wide - 11), 0}, date_, plain);
    }

private:
    std::string name_;
    std::string size_;
    std::string date_;
};

// A framed panel with its rows one under another, as many as fit.
class Panel : public Node {
public:
    Panel() { classes({"frame"}); }

protected:
    // Each child in its row of the inside.
    void arrange(Area inside) override {
        int y = inside.at.y;

        for (Node& one : children()) one.placed(Area{{inside.at.x, y++}, {inside.size.wide, 1}});
    }
};

// Two panels side by side, halves of the room.
class Split : public Node {
protected:
    // The first child in the left half, the second in the right one.
    void arrange(Area inside) override {
        const int half = inside.size.wide / 2;
        int x = inside.at.x;

        for (Node& one : children()) {
            const int wide = x == inside.at.x ? half : inside.size.wide - half;

            one.placed(Area{{x, inside.at.y}, {wide, inside.size.high}});
            x += wide;
        }
    }
};

// The screen: the menu bar on top, the panels, the key bar at the bottom.
class Screen : public Node {
protected:
    // Bars of one row at the edges, the panels between them.
    void arrange(Area inside) override {
        std::vector<Node*> parts;

        for (Node& one : children()) parts.push_back(&one);

        parts[0]->placed(Area{inside.at, {inside.size.wide, 1}});
        parts[1]->placed(Area{{inside.at.x, inside.at.y + 1}, {inside.size.wide, inside.size.high - 2}});
        parts[2]->placed(Area{{inside.at.x, inside.at.y + inside.size.high - 1}, {inside.size.wide, 1}});
    }
};

// The document of the measurement and the rows of its left panel.
struct Panels {
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    Document document{tal::Port<tal::Clock>{clock}};
    std::vector<Row*> left;

    // Builds the screen with `rows` rows in each panel.
    explicit Panels(int rows) {
        document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "frame", "style": {"border": "thin"}}], "glyphs": {}})"));

        Screen& screen = document.children().add<Screen>();

        (void)screen.children().add<Line>(std::vector<std::string>{"menubar"},
                                          " Left   Files   Commands   Options   Right", std::string{});

        Split& split = screen.children().add<Split>();
        Panel& one = split.children().add<Panel>();
        Panel& two = split.children().add<Panel>();

        for (int at = 0; at < rows; ++at) {
            left.push_back(&one.children().add<Row>(at));
            (void)two.children().add<Row>(rows + at);
        }

        (void)screen.children().add<Line>(std::vector<std::string>{"keybar"},
                                          "1 Help  2 Menu  3 View  4 Edit  5 Copy  6 Move  7 Folder  8 Delete  9 Menu  10 Quit",
                                          std::string{"text"});
    }
};

// An application on a scripted 200 by 60 terminal that takes 24-bit colors and Unicode, showing the
// panels, with `turns` turns made already.
struct Measured {
    std::shared_ptr<tal::ScriptedConsole> line = std::make_shared<tal::ScriptedConsole>();
    Panels panels{56};
    std::unique_ptr<tal::App> app;

    // Makes the application, shows the panels and turns `turns` times.
    explicit Measured(int turns = 3) {
        line->window = Size{200, 60};
        line->environment["COLORTERM"] = "truecolor";

        tal::Setup wish;

        wish.colors = tal::Colors::rgb;
        wish.unicode = true;

        app = std::make_unique<tal::App>(wish, tal::Port<tal::Console>{line}, tal::Port<tal::Clock>{panels.clock});
        app->show(panels.document);

        for (int at = 0; at < turns; ++at) (void)app->turn();
    }
};

// Films numbered from `first`, `many` of them, in three columns (name, size, date) made when asked,
// counting what a view asks for.
class Films : public tal::Model {
public:
    Films(std::size_t many, std::size_t first) : many_(many), first_(first) {}

    std::shared_ptr<std::size_t> asked = std::make_shared<std::size_t>(0);

    // How many films there are.
    std::size_t rows() const override { return many_; }

    // A film's name, size or date.
    tal::Entry entry(std::size_t row, std::size_t column) const override {
        const std::size_t number = first_ + row;
        tal::Entry made;

        ++*asked;

        if (column == 0) made.text = "Film number " + std::to_string(number) + " (19" + std::to_string(10 + number % 90) + ")";
        else if (column == 1) made.text = std::to_string(1 + number % 7) + "." + std::to_string(number % 10) + " GB";
        else made.text = "2026-0" + std::to_string(1 + number % 9) + "-1" + std::to_string(number % 10);

        return made;
    }

    // A film more at `at`.
    void inserted(std::size_t at) {
        ++many_;
        told(tal::Changes{tal::Changes::Kind::inserted, at, 1});
    }

    // The film at `at` changed.
    void touched(std::size_t at) { told(tal::Changes{tal::Changes::Kind::changed, at, 1}); }

private:
    std::size_t many_;
    std::size_t first_;
};

// An application on the terminal of the measurement showing two framed tables of 20 000 films side
// by side, each with its bar, between the menu bar and the key bar; the left table has the focus
// and its cursor stands in the middle.
struct Viewed {
    std::shared_ptr<tal::ScriptedConsole> line = std::make_shared<tal::ScriptedConsole>();
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    Document document{tal::Port<tal::Clock>{clock}};
    std::shared_ptr<Films> films = std::make_shared<Films>(20000, 0);
    tal::Table* left = nullptr;
    std::unique_ptr<tal::App> app;

    // Builds the screen, shows it and turns until it settles.
    Viewed() {
        document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "frame", "style": {"border": "thin"}}], "glyphs": {}})"));

        Screen& screen = document.children().add<Screen>();

        (void)screen.children().add<Line>(std::vector<std::string>{"menubar"}, " Left   Files   Commands   Options   Right", std::string{});

        Split& split = screen.children().add<Split>();
        const std::vector<tal::Heading> columns{{"Name", tal::Share{1, 0}, tal::Edge::start},
                                                {"Size", tal::Share{0, 8}, tal::Edge::end},
                                                {"Date", tal::Share{0, 10}, tal::Edge::start}};

        for (const std::shared_ptr<Films>& one : {films, std::make_shared<Films>(20000, 20000)}) {
            tal::Row& panel = split.children().add<tal::Row>();

            panel.classes({"frame"});

            tal::Table& table = panel.children().add<tal::Table>(one, columns);

            table.share(tal::Share{1, 0});
            (void)panel.children().add<tal::Scroll>(table);

            if (left == nullptr) left = &table;
        }

        (void)screen.children().add<Line>(std::vector<std::string>{"keybar"}, "1 Help  2 Menu  3 View  4 Edit  5 Copy  6 Move  10 Quit",
                                          std::string{"text"});

        line->window = Size{200, 60};
        line->environment["COLORTERM"] = "truecolor";

        tal::Setup wish;

        wish.colors = tal::Colors::rgb;
        wish.unicode = true;

        app = std::make_unique<tal::App>(wish, tal::Port<tal::Console>{line}, tal::Port<tal::Clock>{clock});
        app->show(document);

        for (int at = 0; at < 3; ++at) (void)app->turn();

        left->focus();
        left->current = 10000;
        left->top(9980);

        for (int at = 0; at < 2; ++at) (void)app->turn();
    }

    // Does `work`, turns once and gives what the turn's frame touched (nothing without a frame) and
    // how many entries it asked for.
    template <typename Work>
    std::pair<tal::Tally, std::size_t> turned(Work&& work) {
        const std::uint64_t before = document.framed();

        *films->asked = 0;
        work();
        (void)app->turn();

        return {document.framed() == before ? tal::Tally{} : document.tally(), *films->asked};
    }
};

}

// Views over 20 000 films (E4d): a step of the cursor asks the model for the two rows it lights and
// lays nothing out; a page and the wheel ask for one window and lay nothing out; a change out of
// view and a film added above it ask for nothing and paint nothing, the thumb standing where it
// stood. Outside a measurement the trial checks these.
TRIAL("Bench: views of 20 000 rows") {
    Viewed viewed;
    tal::Table& left = *viewed.left;
    const std::size_t window = left.window();
    const auto pressed = [&viewed](Code code) { return [&viewed, code] { viewed.document.key(tal::Key{code}); }; };

    SAME(window, std::size_t{55});

    const auto [down, down_asked] = viewed.turned(pressed(Code::down));

    SAME(down_asked, std::size_t{6});
    SAME(down.measured, std::uint64_t{0});
    SAME(down.arranged, std::uint64_t{0});

    const auto [page, page_asked] = viewed.turned(pressed(Code::pagedown));

    SAME(page_asked, 3 * window);
    SAME(page.arranged, std::uint64_t{0});

    const auto [wheel, wheel_asked] = viewed.turned([&viewed] { viewed.document.mouse(tal::Press::wheel, tal::MouseButton::none, Spot{20, 20}, {}, -1); });

    SAME(wheel_asked, 3 * window);
    SAME(wheel.arranged, std::uint64_t{0});

    const auto [far, far_asked] = viewed.turned([&viewed] { viewed.films->touched(5); });

    SAME(far_asked, std::size_t{0});
    SAME(far.painted, std::uint64_t{0});

    viewed.line->written.clear();

    const auto [above, above_asked] = viewed.turned([&viewed] { viewed.films->inserted(5); });

    SAME(above_asked, std::size_t{0});
    SAME(above.painted, std::uint64_t{0});

    if (!benching()) return;

    std::printf("TALLY view-down measured=%llu arranged=%llu painted=%llu asked=%zu\n", static_cast<unsigned long long>(down.measured),
                static_cast<unsigned long long>(down.arranged), static_cast<unsigned long long>(down.painted), down_asked);
    std::printf("TALLY view-above painted=%llu bytes=%zu\n", static_cast<unsigned long long>(above.painted), viewed.line->written.size());

    bool back = false;

    measured("view-still", 200, [&viewed] { (void)viewed.app->turn(); });
    measured("view-down", 200, [&viewed, &back] {
        back = !back;
        viewed.document.key(tal::Key{back ? Code::up : Code::down});
        (void)viewed.app->turn();
    });
    measured("view-page", 100, [&viewed, &back] {
        back = !back;
        viewed.document.key(tal::Key{back ? Code::pageup : Code::pagedown});
        (void)viewed.app->turn();
    });
    measured("view-hidden", 200, [&viewed] {
        viewed.films->touched(5);
        (void)viewed.app->turn();
    });
    measured("view-above", 200, [&viewed] {
        viewed.films->inserted(5);
        (void)viewed.app->turn();
    });
}

// A turn of the application in which nothing changed, and a turn after the cursor moved one row:
// two rows change their state, so their style, and only they are painted. Outside a measurement
// the same turns run once each and the trial checks what a cursor's move touches.
TRIAL("Bench: turns of the application over the panels") {
    Measured measured_app;
    tal::App& app = *measured_app.app;
    Panels& panels = measured_app.panels;
    std::size_t row = 0;

    panels.left[row]->cursor(true);

    (void)app.turn();

    // Moves the cursor one row down, around the 55 rows after the first, and turns.
    const auto moved = [&] {
        panels.left[row]->cursor(false);
        row = row % 55 + 1;
        panels.left[row]->cursor(true);

        measured_app.line->written.clear();

        (void)app.turn();
    };

    moved();

    const tal::Tally touched = panels.document.tally();
    const std::size_t sent = measured_app.line->written.size();

    SAME(touched.styled, std::uint64_t{2});
    SAME(touched.measured, std::uint64_t{0});
    SAME(touched.arranged, std::uint64_t{0});
    // The two rows lie next to each other, so their damage is one rectangle: the rows and the four
    // nodes under them (the document, the screen, the split and the panel) are painted once each.
    SAME(touched.painted, std::uint64_t{6});
    OUGHT(sent > 0 && sent < 2 * 200 * 4);

    measured_app.line->written.clear();

    (void)app.turn();

    SAME(measured_app.line->written, std::string{});

    if (!benching()) return;

    std::printf("TALLY cursor-move styled=%llu measured=%llu arranged=%llu painted=%llu bytes=%zu\n",
                static_cast<unsigned long long>(touched.styled), static_cast<unsigned long long>(touched.measured),
                static_cast<unsigned long long>(touched.arranged), static_cast<unsigned long long>(touched.painted), sent);

    measured("frame-still", 200, [&app] { (void)app.turn(); });
    measured("cursor-move", 200, moved);
}

// For information, without a threshold: the first frame of the whole screen, and a new window size
// that lays everything out and draws it again.
TRIAL("Bench: the first frame and a new size") {
    if (!benching()) return;

    Measured fresh{0};

    measured("first-frame", 1, [&fresh] { (void)fresh.app->turn(); });

    Measured measured_app;
    bool wide = false;

    measured("resize", 50, [&measured_app, &wide] {
        wide = !wide;
        measured_app.line->window = wide ? Size{180, 56} : Size{200, 60};
        measured_app.line->written.clear();

        (void)measured_app.app->turn();
    });
}

namespace {

// A file in memory whose reads come at once.
class Memory : public tal::fs::Opened {
public:
    explicit Memory(std::string what) : raw_(tal::bytes(what)) {}

    // The bytes from `at`, `most` at most.
    tal::Task<tal::Bytes> read(std::uint64_t at, std::size_t most) override {
        const auto from = static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(at, raw_.size()));
        const auto upto = static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(at + most, raw_.size()));

        co_return tal::Bytes(raw_.begin() + from, raw_.begin() + upto);
    }

    // Nothing is written.
    tal::Task<void> write(std::uint64_t at, tal::Bytes what) override {
        (void)at;
        (void)what;

        co_return;
    }

    // The size.
    tal::Task<std::uint64_t> size() override { co_return raw_.size(); }

    // Nothing.
    tal::Task<void> flush() override { co_return; }

private:
    tal::Bytes raw_;
};

// A pager of a file filling a screen of 200 by 60 cells, its reads come on the program's loop.
struct Paged {
    tal::testing::Scene scene{{.screen = {200, 60}}};
    tal::Document document{scene.time()};
    tal::Pager* pager = nullptr;

    // A pager of `doc`, wrapping or not.
    Paged(std::string doc, bool wrapping) {
        pager = &document.children().add<tal::Pager>(tal::fs::File{std::make_shared<Memory>(std::move(doc))});
        pager->wrap = wrapping;
        scene.show(document);
        pager->focus();
        settled();
    }

    // Lets the reads come, and draws.
    void settled() {
        while (tal::loop().turn()) {
        }

        scene.wait(std::chrono::milliseconds{0});
    }

    // A step down or up and its frame.
    void stepped(bool back) {
        scene.press(tal::Key{back ? Code::up : Code::down});
        settled();
    }
};

// An editor of a text filling a screen of 200 by 60 cells, its cursor in the middle line.
struct Composed {
    tal::testing::Scene scene{{.screen = {200, 60}}};
    tal::Document document{scene.time()};
    tal::Editor* editor = nullptr;

    // An editor of `lines` lines of 80 letters.
    explicit Composed(std::size_t lines) {
        std::string doc;

        for (std::size_t one = 0; one < lines; ++one) doc += std::string(80, 'a') + "\n";

        editor = &document.children().add<tal::Editor>(std::make_shared<tal::Text>(doc));
        scene.show(document);
        editor->focus();
        editor->line(lines / 2);
        scene.wait(std::chrono::milliseconds{0});
    }

    // Types `key` and draws.
    void typed(tal::Key key) {
        scene.press(key);
        scene.wait(std::chrono::milliseconds{0});
    }
};

}

// A frame of a pager over a line of 100 MB costs what one over short lines filling the same cells
// does: each row reads only what it shows. Stepping down and up, wrapped and not.
TRIAL("Bench: a pager over a line of 100 MB") {
    std::string lines;

    while (lines.size() < 1024 * 1024) lines += std::string(200, 'a') + "\n";

    tal::Loop loop;
    Paged short_one{lines, true};
    Paged long_one{std::string(100U * 1024 * 1024, 'b'), true};
    Paged cut{std::string(100U * 1024 * 1024, 'c'), false};

    SAME(long_one.scene.row(59), std::string(200, 'b'));
    SAME(cut.scene.row(0), std::string(200, 'c'));

    if (!benching()) return;

    bool back = false;

    measured("pager-short", 200, [&short_one, &back] {
        back = !back;
        short_one.stepped(back);
    });
    measured("pager-long", 200, [&long_one, &back] {
        back = !back;
        long_one.stepped(back);
    });
    measured("pager-cut", 200, [&cut, &back] {
        back = !back;
        cut.stepped(back);
    });
}

// A letter typed in an editor of 200 000 lines costs a frame what one in an editor of a hundred
// lines does: the spool follows the change of a line in a few steps of its tree. A line end costs a
// pass over the starts of the lines, as the text moves its lines then anyway.
TRIAL("Bench: typing in an editor of 200 000 lines") {
    tal::Loop loop;
    Composed small{100};
    Composed big{200000};

    small.typed(tal::Key{U'x'});
    big.typed(tal::Key{U'x'});

    SAME(small.editor->cursor->byte, std::size_t{1});
    SAME(big.editor->cursor->line, std::size_t{100000});

    if (!benching()) return;

    bool back = false;
    const auto step = [&back](Composed& one, tal::Key key) {
        back = !back;
        one.typed(back ? tal::Key{Code::backspace} : key);
    };

    measured("editor-small", 200, [&small, &step] { step(small, tal::Key{U'x'}); });
    measured("editor-big", 200, [&big, &step] { step(big, tal::Key{U'x'}); });
    measured("editor-big-enter", 20, [&big, &step] { step(big, tal::Key{Code::enter}); });
}

// The hostile texts the JSON reader is built against cost in proportion to their size: an object of
// half a million names, half a million values in one array, arrays at the limit of depth over and
// over, and numbers of the most digits.
TRIAL("Bench: hostile JSON") {
    std::string names = "{";
    std::string values = "[";
    std::string deep = "[";
    std::string digits = "[";

    for (int one = 0; one < 500000; ++one) {
        names += (one > 0 ? ",\"" : "\"") + std::to_string(static_cast<std::int64_t>(one) * 7919 % 500009) + "\":0";
        values += one > 0 ? ",0" : "0";
    }

    names += "}";
    values += "]";

    for (int one = 0; one < 10000; ++one) deep += (one > 0 ? "," : "") + std::string(63, '[') + std::string(63, ']');

    deep += "]";

    for (int one = 0; one < 10000; ++one) digits += (one > 0 ? ",0." : "0.") + std::string(399, '7');

    digits += "]";

    SAME(tal::json::parsed(names).size(), std::size_t{500000});
    SAME(tal::json::parsed(values).size(), std::size_t{500000});
    SAME(tal::json::parsed(deep).size(), std::size_t{10000});
    SAME(tal::json::parsed(digits).size(), std::size_t{10000});

    if (!benching()) return;

    measured("json-names", 3, [&names] { (void)tal::json::parsed(names); });
    measured("json-values", 3, [&values] { (void)tal::json::parsed(values); });
    measured("json-deep", 3, [&deep] { (void)tal::json::parsed(deep); });
    measured("json-digits", 3, [&digits] { (void)tal::json::parsed(digits); });
}

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
#include "tal/capabilities.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/document.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

#include "tal/testing.hpp"

#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define TAL_CALLGRIND 1
#endif

// The measurements of talcite.next's acceptance (E3e). They run only when `TAL_BENCH` is set; in an
// ordinary run each passes at once. As in twa's test/benching.cpp, each measured stretch prints
// `BENCH <name> <milliseconds per repetition>`, and under callgrind (started with
// `--instr-atstart=no`) it is the only instrumented code, dumped as `<name>/<repetitions>`. The
// tree is built like twa's screen of panels on the terminal of twa's measurement: 200 by 60 cells,
// a menu bar, two framed panels of film rows side by side and a key bar, in the built-in sheet,
// monochrome like the skin twa was measured with.

namespace {

using tal::Area;
using tal::Canvas;
using tal::Document;
using tal::Limits;
using tal::Look;
using tal::Node;
using tal::Size;
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

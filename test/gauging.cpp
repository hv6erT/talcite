#include "proving.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

#include "talcite/encoding.hpp"
#include "talcite/gauge.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/time.hpp"

using namespace talcite;

namespace {

// Reads the first row of a view as text, with trailing spaces removed.
std::string read(const View& one) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, 0}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Renders the gauge in one row of the given width and reads it back as text.
std::string drawn(ui::Gauge& one, int wide) { return read(one.render({{0, 0}, {wide, 1}})); }

}

TRIAL("Gauge: wants the width it was given, and always one row") {
    ui::Gauge one{20};

    same(one.wants({40, 3}), (ui::Size{20, 3 > 1 ? 1 : 3}));
    same(one.wants({12, 3}), (ui::Size{12, 1}));
    same(one.wants({40, 0}), (ui::Size{20, 0}));
    same(one.capacity(), 0);
}

// The fraction is clamped in one place, where it comes in. The inverted comparison also catches
// NaN, which would otherwise pass every ordinary condition.
TRIAL("Gauge: the fraction is clamped to zero and one") {
    ui::Gauge one{20};

    one.part(-3.0);
    same(one.part(), 0.0);

    one.part(7.5);
    same(one.part(), 1.0);

    one.part(std::nan(""));
    same(one.part(), 0.0);

    one.part(0.25);
    same(one.part(), 0.25);
}

TRIAL("Gauge: bar, brackets and the percentage on the right") {
    ui::Gauge one{20};

    same(drawn(one, 20), std::string{"[░░░░░░░░░░░░░]   0%"});

    one.part(0.5);

    same(drawn(one, 20), std::string{"[███████░░░░░░]  50%"});

    one.part(1.0);

    same(drawn(one, 20), std::string{"[█████████████] 100%"});
}

// Full only when finished. Both numbers follow the same rule, so the bar and the percentage never
// say different things, and `100%` means "done", not "almost".
TRIAL("Gauge: a hundred percent only at exactly one") {
    ui::Gauge one{20};

    one.part(0.999);

    same(drawn(one, 20), std::string{"[████████████░]  99%"});

    one.part(1.0);

    same(drawn(one, 20), std::string{"[█████████████] 100%"});
}

TRIAL("Gauge: when tight, the percentage goes first, then the brackets") {
    ui::Gauge one{20};

    one.part(0.5);

    same(drawn(one, 12), std::string{"[███░░]  50%"});
    same(drawn(one, 8), std::string{"[░]  50%"});

    // Below eight cells the percentage has no room.
    same(drawn(one, 7), std::string{"[███░░]"});
    same(drawn(one, 3), std::string{"[░]"});

    // Below three there is no room for the brackets, but the bar still says something.
    same(drawn(one, 2), std::string{"█░"});
    same(drawn(one, 1), std::string{"░"});
    same(drawn(one, 0), std::string{});
}

TRIAL("Gauge: without the percentage the whole width goes to the bar") {
    ui::Gauge one{20};

    one.percent(false);
    one.part(0.5);

    same(drawn(one, 20), std::string{"[█████████░░░░░░░░░]"});
}

// Indeterminate: the block travels back and forth, and there is no percentage, because there is
// nothing to show. `step()` drives the movement, so the trial does not have to wait for a clock.
TRIAL("Gauge: an indeterminate gauge travels and bounces off the edges") {
    ui::Gauge one{20};

    one.part(0.5);
    one.busy(true);

    ought(one.busy());

    same(drawn(one, 20), std::string{"[██░░░░░░░░░░░]"});

    one.step();

    same(drawn(one, 20), std::string{"[░██░░░░░░░░░░]"});

    // Eleven steps bring the block to the right edge.
    for (int which = 1; which < 11; ++which) one.step();

    same(drawn(one, 20), std::string{"[░░░░░░░░░░░██]"});

    one.step();

    same(drawn(one, 20), std::string{"[░░░░░░░░░░██░]"});
}

TRIAL("Gauge: returning from indeterminate shows the remembered fraction") {
    ui::Gauge one{20};

    one.part(0.5);
    one.busy(true);
    one.step();
    one.step();

    one.busy(false);

    ought(!one.busy());
    same(one.part(), 0.5);
    same(drawn(one, 20), std::string{"[███████░░░░░░]  50%"});

    // Turning it on again starts from the left, not from where it stopped.
    one.busy(true);

    same(drawn(one, 20), std::string{"[██░░░░░░░░░░░]"});
}

// A gauge without an owner and without a loop simply does not move by itself, and does not break
// either.
TRIAL("Gauge: an indeterminate gauge without a loop starts no timer and does not break") {
    ui::Gauge one{20};

    one.busy(true);
    one.busy(false);
    one.busy(true);

    same(drawn(one, 20), std::string{"[██░░░░░░░░░░░]"});
}

// The gauge spins by itself. `Time::later` measures the time, `here()` moves the continuation to
// the loop's thread, and `Loop::turn()` runs it: here in the trial by hand, in a program `Term`
// does it.
TRIAL("Gauge: an indeterminate gauge spins by itself on the library's timer") {
    Loop loop;

    const auto one = std::make_shared<ui::Gauge>(20);

    one->busy(true);

    const std::string was = drawn(*one, 20);

    Stopwatch began;

    while (began.spent() < std::chrono::seconds{3}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});

        (void)loop.turn();

        if (drawn(*one, 20) != was) break;
    }

    ought(drawn(*one, 20) != was);

    // Once stopped, it stops ticking: after it is turned off, the loop gets nothing more to do.
    one->busy(false);

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    (void)loop.turn();

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    same(loop.turn(), std::size_t{0});
}

// All skin keys of the gauge grow from one role root, as everywhere else, so replacing the root
// restyles the whole gauge.
TRIAL("Gauge: the role root can be replaced") {
    ui::Root top;

    const auto one = std::make_shared<ui::Gauge>(8);

    same(one->role(), std::string{"talcite.gauge"});

    one->part(0.0);

    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 4, 4, 4};

    these["moj.wskaznik"] = look;

    top.skin(ui::Skin{these});
    one->role("moj.wskaznik");

    const View made = top.render({{0, 0}, {8, 1}});

    same(made.at({1, 0}).style.foreground, (Color{false, 4, 4, 4}));
}

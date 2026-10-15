#include "proving.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/clock.hpp"
#include "talcite/root.hpp"
#include "talcite/timer.hpp"

// The root's clock: appointments once (`after`) and every period (`every`), each kept by a timer
// that takes it back when it goes. Time is a manual clock, so no trial waits.
using namespace talcite;
using namespace std::chrono_literals;

namespace {

// A root on a manual clock, with the clock at hand.
struct Timed {
    ManualClock time;
    ui::Root top{time};
};

// How long until the nearest appointment, or nothing when none waits.
std::optional<std::chrono::milliseconds> waiting(ui::Root& top) {
    const std::optional<Clock::Moment> next = top.clock().next();

    if (!next) return std::nullopt;

    return std::chrono::duration_cast<std::chrono::milliseconds>(*next - top.clock().now());
}

}

TRIAL("Clock: without appointments nothing waits and nothing is called") {
    Timed one;

    ought(!waiting(one.top).has_value());
    ought(!one.top.elapse());
}

TRIAL("Clock: once after a delay, not before, then the timer is spent") {
    Timed one;

    int called = 0;

    const ui::Timer later = one.top.clock().after(100ms, [&called] { ++called; });

    ought(later.active());
    ought(waiting(one.top) == std::optional{100ms});

    one.time.advance(99ms);

    ought(!one.top.elapse());
    same(called, 0);

    one.time.advance(1ms);

    ought(one.top.elapse());
    same(called, 1);
    ought(!later.active());
    ought(!waiting(one.top).has_value());

    one.time.advance(1s);

    ought(!one.top.elapse());
    same(called, 1);
}

// A repeating appointment keeps to its own grid: called at every period, and after a long delay
// once, moving on to the first of its moments still ahead instead of catching up.
TRIAL("Clock: every period on its grid, and once after being late") {
    Timed one;

    int called = 0;

    const ui::Timer beat = one.top.clock().every(100ms, [&called] { ++called; });

    one.time.advance(100ms);
    ought(one.top.elapse());

    one.time.advance(130ms);
    ought(one.top.elapse());

    same(called, 2);
    ought(waiting(one.top) == std::optional{70ms});

    one.time.advance(570ms);
    ought(one.top.elapse());

    same(called, 3);
    ought(waiting(one.top) == std::optional{100ms});
    ought(beat.active());
}

// The timer is the appointment: dropping it, emptying it or stopping it takes the appointment
// back; moving it hands the appointment over; assigning over a full timer takes back the old one.
TRIAL("Clock: a timer that goes takes its appointment back, and moving hands it over") {
    Timed one;

    std::vector<std::string> told;

    {
        const ui::Timer gone = one.top.clock().after(10ms, [&told] { told.push_back("porzucony"); });
    }

    ui::Timer kept = one.top.clock().after(10ms, [&told] { told.push_back("przeniesiony"); });
    ui::Timer moved = std::move(kept);

    ought(!kept.active());
    ought(moved.active());

    ui::Timer replaced = one.top.clock().after(10ms, [&told] { told.push_back("zastąpiony"); });

    replaced = one.top.clock().after(10ms, [&told] { told.push_back("nowy"); });

    ui::Timer stopped = one.top.clock().every(10ms, [&told] { told.push_back("zatrzymany"); });

    stopped.stop();

    ought(!stopped.active());

    one.time.advance(10ms);

    ought(one.top.elapse());
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"przeniesiony"});
    same(told[1], std::string{"nowy"});
}

// Calls come in the order of their moments and, for one moment, in the order the appointments were
// made.
TRIAL("Clock: calls in the order of moments, and of making for one moment") {
    Timed one;

    std::vector<std::string> told;

    const ui::Timer third = one.top.clock().after(30ms, [&told] { told.push_back("trzeci"); });
    const ui::Timer first = one.top.clock().after(10ms, [&told] { told.push_back("pierwszy"); });
    const ui::Timer second = one.top.clock().after(10ms, [&told] { told.push_back("drugi"); });

    ought(waiting(one.top) == std::optional{10ms});

    one.time.advance(50ms);

    ought(one.top.elapse());
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"pierwszy"});
    same(told[1], std::string{"drugi"});
    same(told[2], std::string{"trzeci"});
}

// An appointment made by a call waits for the next turn, even without a delay, so a call that
// makes its successor at once cannot hold the loop; one taken back by an earlier call of the same
// turn is not called.
TRIAL("Clock: made in a call waits for the next turn, taken back in a call is not called") {
    Timed one;

    std::vector<std::string> told;

    ui::Timer chained;
    std::optional<ui::Timer> later;

    const ui::Timer first = one.top.clock().after(10ms, [&] {
        told.push_back("pierwszy");

        chained = one.top.clock().after(0ms, [&told] { told.push_back("następny"); });
        later.reset();
    });

    later.emplace(one.top.clock().after(10ms, [&told] { told.push_back("odwołany"); }));

    one.time.advance(10ms);

    ought(one.top.elapse());
    same(told.size(), std::size_t{1});
    ought(chained.active());

    ought(one.top.elapse());
    same(told.size(), std::size_t{2});
    same(told[1], std::string{"następny"});
}

// A call may take back its own appointment, also a repeating one, by emptying its timer.
TRIAL("Clock: a call that stops its own repeating appointment ends it") {
    Timed one;

    int called = 0;

    ui::Timer beat;

    beat = one.top.clock().every(10ms, [&called, &beat] {
        ++called;

        beat = ui::Timer{};
    });

    one.time.advance(10ms);
    ought(one.top.elapse());

    one.time.advance(10ms);
    ought(!one.top.elapse());

    same(called, 1);
    ought(!waiting(one.top).has_value());
}

// An empty call only wakes the loop: the turn counts as one in which something was due.
TRIAL("Clock: an empty call only wakes") {
    Timed one;

    const ui::Timer frame = one.top.clock().after(5ms, {});

    one.time.advance(5ms);

    ought(one.top.elapse());
    ought(!frame.active());
}

// A timer that outlives its root has nothing to take back.
TRIAL("Clock: a timer outliving its root is spent and goes quietly") {
    ui::Timer left;

    {
        Timed one;

        left = one.top.clock().after(10ms, [] {});

        ought(left.active());
    }

    ought(!left.active());

    left = ui::Timer{};

    ought(!left.active());
}

// A call runs inside a delivery: a node it takes off the tree lives until the calls are over, as
// during an event.
TRIAL("Clock: a node taken off the tree in a call lives until the calls end") {
    Timed one;

    std::weak_ptr<ui::Node> seen;

    {
        const auto child = std::make_shared<ui::Node>();

        seen = child;
        one.top.children().add(child);
    }

    bool alive_in_call = false;

    const ui::Timer take = one.top.clock().after(1ms, [&one] { (void)one.top.children().remove(one.top.children().front().get()); });
    const ui::Timer look = one.top.clock().after(1ms, [&alive_in_call, &seen] { alive_in_call = !seen.expired(); });

    one.time.advance(1ms);

    ought(one.top.elapse());
    ought(alive_in_call);
    ought(seen.expired());
}

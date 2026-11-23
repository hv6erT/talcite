#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include "tal/simulation.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// Simulations (E7e): steps owed by real time at their pace and never more, nothing while paused,
// a single step paused, a change of the program taken in between steps, a step slower than real
// time slowing the simulation down, a step's trouble stopping it, and steps or paces malformed.

namespace {

using namespace std::chrono_literals;

// A falling body: height and speed.
struct Body {
    double height = 100;
    double speed = 0;
    int steps = 0;
};

// One step of the body under gravity, explicit Euler.
void fell(Body& body, double dt) {
    body.speed -= 9.81 * dt;
    body.height += body.speed * dt;
    ++body.steps;
}

}

TRIAL("Simulation: steps owed by real time at their pace, nothing paused, one step paused, a change taken in between steps") {
    tal::Simulation<Body> falling{Body{}, fell, 0.01};

    SAME(falling.latest().steps, 0);

    falling.advance(1s);
    SOON(!falling.busy());
    SAME(falling.steps(), std::uint64_t{100});
    SAME(falling.latest().steps, 100);
    OUGHT(std::abs(falling.time() - 1.0) < 1e-12);
    OUGHT(falling.latest().height < 100);

    falling.pace(2);
    falling.advance(500ms);
    SOON(!falling.busy());
    SAME(falling.steps(), std::uint64_t{200});

    falling.run(false);
    OUGHT(!falling.running());
    falling.advance(1s);
    SOON(!falling.busy());
    SAME(falling.steps(), std::uint64_t{200});

    falling.step();
    SOON(!falling.busy());
    SAME(falling.steps(), std::uint64_t{201});

    falling.changed([](Body& body) { body.height = 1000; });
    SOON(!falling.busy());
    SAME(falling.latest().height, 1000.0);
    SAME(falling.steps(), std::uint64_t{201});

    falling.pace(1);
    falling.run(true);
    falling.advance(5ms);
    SOON(!falling.busy());
    SAME(falling.steps(), std::uint64_t{201});
    falling.advance(5ms);
    SOON(!falling.busy());
    SAME(falling.steps(), std::uint64_t{202});
}

TRIAL("Simulation: a step slower than real time slows the simulation down; a step's trouble stops it; steps and paces malformed") {
    tal::Simulation<int> slow{0, [](int& one, double) {
                                  std::this_thread::sleep_for(100ms);
                                  ++one;
                              },
                              0.1};

    slow.advance(10s);
    SOON(!slow.busy());
    OUGHT(slow.steps() >= 1 && slow.steps() <= 4);
    SAME(slow.latest(), static_cast<int>(slow.steps()));

    tal::Simulation<int> failing{0, [](int& one, double) {
                                     if (++one == 3) throw tal::Trouble{tal::Kind::malformed, "The body fell through the floor."};
                                 },
                                 0.01};

    failing.advance(1s);
    SOON(!failing.busy());
    OUGHT(failing.trouble().has_value());
    OUGHT(!failing.running());
    SAME(failing.steps(), std::uint64_t{2});

    failing.run(true);
    OUGHT(!failing.trouble().has_value());

    THREW((tal::Simulation<int>{0, [](int&, double) {}, 0}));
    THREW((tal::Simulation<int>{0, [](int&, double) {}, std::nan("")}));
    THREW(failing.pace(-1));
}

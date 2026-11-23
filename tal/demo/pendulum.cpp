#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <numbers>
#include <optional>
#include <string>

#include "tal/app.hpp"
#include "tal/document.hpp"
#include "tal/drawing.hpp"
#include "tal/drawn.hpp"
#include "tal/event.hpp"
#include "tal/fonts.hpp"
#include "tal/key.hpp"
#include "tal/simulation.hpp"

// A double pendulum (E7e), the program that shows a simulation in the terminal: two rods of a
// metre and two bodies of a kilogram, integrated by RK4 at steps of a millisecond on the
// simulation's own thread, drawn sixty times a second with the path of the second body behind it
// and its total energy above, which RK4 keeps nearly still (a drift there is the integrator's, not
// the physics'). A body can be dragged with the mouse, the simulation holding it where the pointer
// is. Space pauses, s takes one step, + and - double and halve the pace, q quits.
namespace {

using tal::drawing::Color;
using tal::drawing::Matrix;
using tal::drawing::Path;
using tal::drawing::Pen;
using tal::drawing::Point;

constexpr double g = 9.81;
constexpr double length = 1.0;
constexpr double mass = 1.0;
constexpr std::size_t trail = 600;

// The state of the pendulum: both angles from the vertical, their rates, and the path of the
// second body.
struct Pendulum {
    double first = 2.0;
    double second = 2.6;
    double first_rate = 0;
    double second_rate = 0;
    std::deque<Point> path;
};

// The angular accelerations of the state (θ1, θ2, ω1, ω2), from the equations of Lagrange.
std::pair<double, double> accelerated(double a, double b, double wa, double wb) {
    const double d = a - b;
    const double under = 2 * mass + mass - mass * std::cos(2 * a - 2 * b);
    const double first = (-g * (2 * mass + mass) * std::sin(a) - mass * g * std::sin(a - 2 * b) - 2 * std::sin(d) * mass * (wb * wb * length + wa * wa * length * std::cos(d))) / (length * under);
    const double second = (2 * std::sin(d) * (wa * wa * length * (mass + mass) + g * (mass + mass) * std::cos(a) + wb * wb * length * mass * std::cos(d))) / (length * under);

    return {first, second};
}

// Where the second body is, in metres from the pivot, y down.
Point tip(const Pendulum& state) { return Point{length * std::sin(state.first) + length * std::sin(state.second), length * std::cos(state.first) + length * std::cos(state.second)}; }

// One step of RK4 of `dt` seconds, and the path of the second body.
void stepped(Pendulum& state, double dt) {
    const auto slope = [](double a, double b, double wa, double wb) {
        const auto [aa, ab] = accelerated(a, b, wa, wb);

        return std::array<double, 4>{wa, wb, aa, ab};
    };
    const std::array<double, 4> s{state.first, state.second, state.first_rate, state.second_rate};
    const std::array<double, 4> k1 = slope(s[0], s[1], s[2], s[3]);
    const std::array<double, 4> k2 = slope(s[0] + dt / 2 * k1[0], s[1] + dt / 2 * k1[1], s[2] + dt / 2 * k1[2], s[3] + dt / 2 * k1[3]);
    const std::array<double, 4> k3 = slope(s[0] + dt / 2 * k2[0], s[1] + dt / 2 * k2[1], s[2] + dt / 2 * k2[2], s[3] + dt / 2 * k2[3]);
    const std::array<double, 4> k4 = slope(s[0] + dt * k3[0], s[1] + dt * k3[1], s[2] + dt * k3[2], s[3] + dt * k3[3]);
    std::array<double, 4> next{};

    for (std::size_t one = 0; one < 4; ++one) next[one] = s[one] + dt / 6 * (k1[one] + 2 * k2[one] + 2 * k3[one] + k4[one]);

    state.first = next[0];
    state.second = next[1];
    state.first_rate = next[2];
    state.second_rate = next[3];

    if (state.path.empty() || std::hypot(tip(state).x - state.path.back().x, tip(state).y - state.path.back().y) > 0.01) state.path.push_back(tip(state));

    while (state.path.size() > trail) state.path.pop_front();
}

// The total energy of the state, in joules, the pivot at zero height.
double energy(const Pendulum& s) {
    const double d = s.first - s.second;
    const double kinetic = mass * length * length * s.first_rate * s.first_rate / 2 + mass / 2 * (length * length * s.first_rate * s.first_rate + length * length * s.second_rate * s.second_rate + 2 * length * length * s.first_rate * s.second_rate * std::cos(d));
    const double potential = -(mass + mass) * g * length * std::cos(s.first) - mass * g * length * std::cos(s.second);

    return kinetic + potential;
}

// The matrix from metres to the pixels of a drawing of `size`: the pivot a third down the middle,
// the whole reach of the pendulum within the smaller side.
Matrix placed(tal::media::Size size) {
    const double scale = std::min(size.width, size.height) / (2.4 * 2 * length);

    return Matrix::scaled(scale, scale).then(Matrix::moved(size.width / 2.0, size.height / 2.0));
}

// The angle from `pivot` to `at`, from the vertical down, as the pendulum counts it.
double angle(Point pivot, Point at) { return std::atan2(at.x - pivot.x, at.y - pivot.y); }

}

int main() {
    tal::App app;
    tal::Document document;
    tal::Simulation<Pendulum> pendulum{Pendulum{}, stepped, 0.001};
    std::optional<std::chrono::nanoseconds> last;
    std::optional<int> held;
    const tal::drawing::Font font = tal::drawing::Font::builtin();
    tal::Drawing& drawing = document.children().add<tal::Drawing>([&](Pen& pen, tal::media::Size size, std::chrono::nanoseconds since) {
        if (last) pendulum.advance(since - *last);

        last = since;

        const Pendulum state = pendulum.latest();
        const Matrix world = placed(size);
        const Point first{length * std::sin(state.first), length * std::cos(state.first)};
        const Point second = tip(state);
        Path all;

        all.rectangle(tal::drawing::Rect{Point{0, 0}, static_cast<double>(size.width), static_cast<double>(size.height)});
        pen.fill(all, Color{0.07F, 0.08F, 0.11F, 1});
        pen.save();
        pen.place(world);

        if (state.path.size() > 1) {
            Path path;
            tal::drawing::Stroke thin;

            thin.width = 0.012;
            thin.join = tal::drawing::Join::round;
            path.move(state.path.front());

            for (const Point& one : state.path) path.line(one);

            pen.stroke(path, Color{0.95F, 0.55F, 0.2F, 0.6F}, thin);
        }

        Path rods;
        tal::drawing::Stroke rod;

        rod.width = 0.03;
        rod.cap = tal::drawing::Cap::round;
        rod.join = tal::drawing::Join::round;
        rods.move(Point{0, 0});
        rods.line(first);
        rods.line(second);
        pen.stroke(rods, Color{0.8F, 0.82F, 0.86F, 1}, rod);

        for (const Point& body : {first, second}) {
            Path ball;

            ball.arc(body, 0.08, 0, 2 * std::numbers::pi);
            ball.close();
            pen.fill(ball, Color{0.35F, 0.65F, 1, 1});
        }

        pen.restore();

        char said[96];

        std::snprintf(said, sizeof said, "E = %.6f J   t = %.2f s   pace %.2g%s", energy(state), pendulum.time(), pendulum.pace(), pendulum.running() ? "" : "   paused");

        const double letters = std::max(10.0, size.height / 24.0);

        pen.fill(font.outlined(said, letters, Point{letters / 2, letters / 2}, tal::drawing::Align::left, tal::drawing::Baseline::top), Color{0.9F, 0.9F, 0.9F, 1});
    });

    const tal::Hook mouse = drawing.on<tal::MouseEvent>([&](tal::MouseEvent& event) {
        const std::optional<Point> pointed = drawing.pointed(event);

        if (!pointed) return;

        const std::optional<Matrix> back = placed(drawing.pixels()).inverted();

        if (!back) return;

        const Point at = back->applied(*pointed);
        const Pendulum state = pendulum.latest();
        const Point first{length * std::sin(state.first), length * std::cos(state.first)};

        if (event.press == tal::Press::down) {
            const double to_first = std::hypot(at.x - first.x, at.y - first.y);
            const double to_second = std::hypot(at.x - tip(state).x, at.y - tip(state).y);

            if (std::min(to_first, to_second) < 0.25) held = to_second < to_first ? 2 : 1;
        }

        if (held && (event.press == tal::Press::down || event.press == tal::Press::move)) {
            const int which = *held;

            pendulum.changed([which, at](Pendulum& s) {
                if (which == 1) {
                    s.first = angle(Point{0, 0}, at);
                } else {
                    s.second = angle(Point{length * std::sin(s.first), length * std::cos(s.first)}, at);
                }

                s.first_rate = 0;
                s.second_rate = 0;
            });
        }

        if (event.press == tal::Press::up) held.reset();
    });

    const tal::Hook keys = document.on<tal::KeyEvent>([&](tal::KeyEvent& event) {
        const tal::Key key = event.key;

        if (key == tal::Key{U'q'} || key == tal::Key{tal::Code::f10}) app.stop();

        if (key == tal::Key{U' '}) pendulum.run(!pendulum.running());

        if (key == tal::Key{U's'}) pendulum.step();

        if (key == tal::Key{U'+'}) pendulum.pace(std::min(64.0, pendulum.pace() * 2));

        if (key == tal::Key{U'-'}) pendulum.pace(std::max(1.0 / 64, pendulum.pace() / 2));

        drawing.redraw();
    });

    drawing.rate(60);
    app.title("Double pendulum");
    app.show(document);

    return app.run();
}

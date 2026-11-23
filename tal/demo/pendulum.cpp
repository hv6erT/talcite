#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "tal/app.hpp"
#include "tal/document.hpp"
#include "tal/drawing.hpp"
#include "tal/drawn.hpp"
#include "tal/event.hpp"
#include "tal/fonts.hpp"
#include "tal/fs.hpp"
#include "tal/key.hpp"
#include "tal/path.hpp"
#include "tal/plotting.hpp"
#include "tal/settings.hpp"
#include "tal/simulation.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "pendulum_settings.hpp"

// A double pendulum (E7e), the program that shows a simulation in the terminal: two rods of a metre
// and two bodies of a kilogram, integrated by RK4 at steps of a millisecond on the simulation's own
// thread, drawn sixty times a second with the path of the second body behind it and its total
// energy above, which RK4 keeps nearly still (a drift there is the integrator's, not the physics').
// Beside it (below it, in a window taller than wide) three plots (E8) of the last ten seconds,
// sampled every ten steps on the simulation's thread into traces: the drift of the energy from
// where it started or the pendulum was last let go, both angles in time, and the phase portrait of
// the second rod, its angle against its rate. A body can be dragged with the mouse, the simulation
// holding it where the pointer is. Space pauses, s takes one step, + and - double and halve the
// pace, q quits.
//
// The plots are read with the mouse (E9): over a plot a cursor reads the values under it; a click
// holds the plots still at the time they show (and, as set, the simulation too), a second click or
// Esc lets them go; the wheel draws the time in or out about the cursor of plots held still (of
// plots going on, the stretch of time they show), and a drag moves it. What a click holds (plots,
// both or none, f goes round them), how long the traces keep their samples (seconds) and the step
// of the wheel (in per cent) are settings, read from talcite/tal-pendulum.json in the user's folder
// of settings (in the working folder where there is none, or the file after --settings); a mode
// changed with f is written back when the program ends.
namespace {

using tal::drawing::Color;
using tal::drawing::Matrix;
using tal::drawing::Path;
using tal::drawing::Pen;
using tal::drawing::Point;
using tal::drawing::Rect;

constexpr double g = 9.81;
constexpr double length = 1.0;
constexpr double mass = 1.0;
constexpr std::size_t trail = 600;
constexpr double shown_at_first = 10;
constexpr int every = 10;
constexpr double pi = std::numbers::pi;

// The state of the pendulum: both angles from the vertical, their rates, the path of the second
// body, the time and steps so far, and the energy the drift is measured from.
struct Pendulum {
    double first = 2.0;
    double second = 2.6;
    double first_rate = 0;
    double second_rate = 0;
    std::deque<Point> path;
    double time = 0;
    std::uint64_t steps = 0;
    double reference = 0;
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
    state.time += dt;
    ++state.steps;

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

// Where the parts go in a drawing of `size`: the pendulum, and the three plots beside it in a
// column, or below it where the drawing is taller than wide.
struct Areas {
    Rect swing;
    std::array<Rect, 3> plots;
};

// The areas of a drawing of `size`: the pendulum in a square of the height at the left, at most
// half the width, the plots in the rest one above another; or, taller than wide, the pendulum in a
// square of the width above, at most half the height, and the plots below.
Areas divided(tal::media::Size size) {
    const auto wide = static_cast<double>(size.width);
    const auto high = static_cast<double>(size.height);
    Areas made;

    if (wide >= high) {
        const double side = std::min(high, wide / 2);

        made.swing = Rect{Point{0, 0}, side, high};

        for (std::size_t at = 0; at < 3; ++at) made.plots[at] = Rect{Point{side, high * static_cast<double>(at) / 3}, wide - side, high / 3};
    } else {
        const double side = std::min(wide, high / 2);

        made.swing = Rect{Point{0, 0}, wide, side};

        for (std::size_t at = 0; at < 3; ++at) made.plots[at] = Rect{Point{0, side + (high - side) * static_cast<double>(at) / 3}, wide, (high - side) / 3};
    }

    return made;
}

// The matrix from metres to the pixels of `area`: the pivot in its middle, the whole reach of the
// pendulum within its smaller side.
Matrix placed(const Rect& area) {
    const double scale = std::min(area.wide, area.high) / (2.4 * 2 * length);

    return Matrix::scaled(scale, scale).then(Matrix::moved(area.at.x + area.wide / 2, area.at.y + area.high / 2));
}

// The angle `value` brought to -π to π.
double around(double value) { return std::remainder(value, 2 * pi); }

// The samples of `points` with their angles brought to -π to π, a gap (a point that is not a
// number) where an angle goes round.
std::vector<Point> wrapped(const std::vector<Point>& points) {
    std::vector<Point> made;

    for (const Point& one : points) {
        const double value = around(one.y);

        if (!made.empty() && std::abs(value - made.back().y) > pi) made.push_back(Point{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()});

        made.push_back(Point{one.x, value});
    }

    return made;
}

// The phase portrait from the samples of an angle `angles` and its rate `rates`, taken at the same
// times, of those within `time` (all, without it): the angle brought to -π to π across, the rate
// up, a gap where the angle goes round. The traces are read one after the other, so one may hold
// a sample the other does not have yet.
std::vector<Point> phased(std::vector<Point> angles, std::vector<Point> rates, const std::optional<tal::plotting::Scale>& time) {
    while (!angles.empty() && !rates.empty() && angles.back().x > rates.back().x) angles.pop_back();

    while (!angles.empty() && !rates.empty() && rates.back().x > angles.back().x) rates.pop_back();

    const std::size_t many = std::min(angles.size(), rates.size());
    std::vector<Point> made;

    for (std::size_t at = 0; at < many; ++at) {
        const Point& sample = angles[angles.size() - many + at];

        if (time && (sample.x < time->low() || sample.x > time->high())) continue;

        const double angle = around(sample.y);
        const double rate = rates[rates.size() - many + at].y;

        if (!made.empty() && std::abs(angle - made.back().x) > pi) made.push_back(Point{std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()});

        made.push_back(Point{angle, rate});
    }

    return made;
}

// The traces the simulation samples into, keeping the last `kept` seconds: the drift of the
// energy, both angles and the rate of the second.
struct Traces {
    explicit Traces(double kept) : drift{kept}, first{kept}, second{kept}, second_rate{kept} {}

    tal::plotting::Trace drift;
    tal::plotting::Trace first;
    tal::plotting::Trace second;
    tal::plotting::Trace second_rate;
};

// What a click on a plot holds still: the plots alone, the simulation with them, or nothing.
enum class Freeze { plots, both, none };

// The setting's word for `one`, and the mode a word names (the plots alone for any other).
std::string worded(Freeze one) { return one == Freeze::both ? "both" : one == Freeze::none ? "none" : "plots"; }

Freeze freezing(const std::string& word) { return word == "both" ? Freeze::both : word == "none" ? Freeze::none : Freeze::plots; }

// How the plots are looked at: the plot under the mouse and the pixel it points at, the time held
// still (none while the plots go on), the seconds shown while they go on and the time they last
// showed, where a press began and whether it moved since, and the layouts last drawn.
struct View {
    std::optional<std::size_t> over;
    std::optional<Point> cursor;
    std::optional<tal::plotting::Scale> held;
    double shown = shown_at_first;
    std::optional<tal::plotting::Scale> live;
    std::optional<double> pressed;
    bool moved = false;
    bool stopped = false;
    std::array<tal::plotting::Layout, 3> laid;
};

// The last `shown` seconds up to the last of `points`, none before the first; within the numbers.
std::optional<tal::plotting::Scale> latest(const std::vector<Point>& points, double shown) {
    if (points.empty()) return std::nullopt;

    const double last = points.back().x;
    const double first = std::max(std::numeric_limits<double>::lowest(), last - shown);

    return first < last ? tal::plotting::Scale{first, last} : tal::plotting::Scale{std::nextafter(last, -std::numeric_limits<double>::infinity()), last};
}

// The samples of `state` into `traces`, every so many steps.
void sampled(const Pendulum& state, Traces& traces) {
    if (state.steps % every != 0) return;

    traces.drift.add(state.time, energy(state) - state.reference);
    traces.first.add(state.time, state.first);
    traces.second.add(state.time, state.second);
    traces.second_rate.add(state.time, state.second_rate);
}

// The three plots of `traces` drawn with `pen` in `areas`, their letters of `letters`, over the
// time `view` holds or the last seconds it shows, the plot under the mouse with its cursor; the
// layouts and the time shown kept in `view`.
void plotted(Pen& pen, const tal::drawing::Font& font, const Areas& areas, const Traces& traces, double letters, View& view) {
    using tal::plotting::Plot;
    using tal::plotting::Scale;
    using tal::plotting::Series;

    const Scale angles{-pi, pi};
    std::vector<Point> drifted = traces.drift.points();

    view.live = latest(drifted, view.shown);

    const std::optional<Scale> time = view.held ? view.held : view.live;
    const auto pointing = [&](Plot& plot, std::size_t index) {
        if (view.over == index) plot.cursor = view.cursor;
    };
    Plot drift;
    Series drifting;

    drift.title = "Energy drift";
    drift.x.title = "t [s]";
    drift.y.title = "ΔE [J]";
    drift.x.scale = time;
    drift.legend = tal::plotting::Corner::none;
    drift.letters = letters;
    drifting.points = std::move(drifted);
    drift.series.push_back(std::move(drifting));
    pointing(drift, 0);
    view.laid[0] = drift.drawn(pen, font, areas.plots[0]);

    Plot turns;
    Series one;
    Series two;

    turns.title = "Angles";
    turns.x.title = "t [s]";
    turns.y.title = "θ [rad]";
    turns.x.scale = time;
    turns.y.scale = angles;
    turns.y.grid = true;
    turns.letters = letters;
    one.name = "θ₁";
    one.points = wrapped(traces.first.points());
    two.name = "θ₂";
    two.points = wrapped(traces.second.points());
    turns.series.push_back(std::move(one));
    turns.series.push_back(std::move(two));
    pointing(turns, 1);
    view.laid[1] = turns.drawn(pen, font, areas.plots[1]);

    Plot phase;
    Series portrait;
    Series now;

    phase.title = "Phase of the second rod";
    phase.x.title = "θ₂ [rad]";
    phase.y.title = "ω₂ [rad/s]";
    phase.x.scale = angles;
    phase.legend = tal::plotting::Corner::none;
    phase.letters = letters;
    phase.readout = tal::plotting::Readout::nearest;
    portrait.points = phased(traces.second.points(), traces.second_rate.points(), time);
    portrait.width = 1;
    portrait.color = Color{0.34F, 0.71F, 0.91F, 0.7F};

    for (auto at = portrait.points.rbegin(); at != portrait.points.rend(); ++at)
        if (std::isfinite(at->x)) {
            now.points.push_back(*at);

            break;
        }

    now.line = false;
    now.mark = tal::plotting::Mark::circle;
    now.size = letters * 0.6;
    now.color = Color{0.95F, 0.55F, 0.2F, 1};
    phase.series.push_back(std::move(portrait));
    phase.series.push_back(std::move(now));
    pointing(phase, 2);
    view.laid[2] = phase.drawn(pen, font, areas.plots[2]);
}

// Whether `point` lies within `area`.
bool inside(const Rect& area, Point point) { return point.x >= area.at.x && point.x < area.at.x + area.wide && point.y >= area.at.y && point.y < area.at.y + area.high; }

// The angle from `pivot` to `at`, from the vertical down, as the pendulum counts it.
double angle(Point pivot, Point at) { return std::atan2(at.x - pivot.x, at.y - pivot.y); }

}

int main(int count, char** given) {
    tal::Setup wish;

    wish.hover = true;

    tal::App app{wish};
    std::optional<std::string> asked;

    for (int at = 1; at + 1 < count; ++at)
        if (std::string{given[at]} == "--settings") asked = given[at + 1];

    const auto absolute = [](const std::string& file) {
        const std::u8string whole = std::filesystem::absolute(std::filesystem::path{std::u8string{file.begin(), file.end()}}).u8string();

        return tal::Path{std::string{whole.begin(), whole.end()}};
    };
    std::optional<tal::Path> folder;

    if (!asked) {
        try {
            folder = tal::wait(app.loop(), tal::fs::known(tal::fs::Known::settings));
        } catch (const tal::Trouble&) {
        }
    }

    const tal::Path where = asked ? absolute(*asked) : folder ? *folder / "talcite" / "tal-pendulum.json" : absolute("tal-pendulum.json");
    tal::Settings settings{pendulum_settings::builtin(), where};

    try {
        tal::wait(app.loop(), settings.read());
    } catch (const tal::Trouble&) {
    }

    Freeze freeze = freezing(settings.get(pendulum_settings::plots::freeze));
    const double kept = std::clamp(static_cast<double>(settings.get(pendulum_settings::plots::window)), 10.0, 3600.0);
    const double zoom = std::clamp(static_cast<double>(settings.get(pendulum_settings::plots::zoom)) / 100, 1.01, 10.0);
    tal::Document document;
    Traces traces{kept};
    View view;
    Pendulum start;

    start.reference = energy(start);

    tal::Simulation<Pendulum> pendulum{start,
                                       [&traces](Pendulum& state, double dt) {
                                           stepped(state, dt);
                                           sampled(state, traces);
                                       },
                                       0.001};
    std::optional<std::chrono::nanoseconds> last;
    std::optional<int> held;
    const tal::drawing::Font font = tal::drawing::Font::builtin();
    tal::Drawing& drawing = document.children().add<tal::Drawing>([&](Pen& pen, tal::media::Size size, std::chrono::nanoseconds since) {
        if (last) pendulum.advance(since - *last);

        last = since;

        const Pendulum state = pendulum.latest();
        const Areas areas = divided(size);
        const Matrix world = placed(areas.swing);
        const Point first{length * std::sin(state.first), length * std::cos(state.first)};
        const Point second = tip(state);
        Path all;

        all.rectangle(Rect{Point{0, 0}, static_cast<double>(size.width), static_cast<double>(size.height)});
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

        char said[160];

        std::snprintf(said, sizeof said, "E = %.6f J   t = %.2f s   pace %.2g%s%s   click holds %s", energy(state), pendulum.time(), pendulum.pace(), pendulum.running() ? "" : "   paused", view.held ? "   plots held" : "", worded(freeze).c_str());

        const double letters = std::max(10.0, std::min(areas.swing.high / 24, areas.swing.wide * 0.95 / std::max(1.0, font.measured(said, 1).wide)));

        pen.fill(font.outlined(said, letters, Point{letters / 2, letters / 2}, tal::drawing::Align::left, tal::drawing::Baseline::top), Color{0.9F, 0.9F, 0.9F, 1});
        plotted(pen, font, areas, traces, std::clamp(size.height / 48.0, 10.0, 16.0), view);
    });

    const auto hold = [&](bool on) {
        if (freeze == Freeze::none || on == view.held.has_value()) return;

        if (on) {
            view.held = view.live;

            if (freeze == Freeze::both && pendulum.running()) {
                pendulum.run(false);
                view.stopped = true;
            }
        } else {
            view.held.reset();

            if (view.stopped) pendulum.run(true);

            view.stopped = false;
        }
    };

    // The mouse over a plot: the cursor, a click that holds or lets go, the wheel and a drag along
    // the time of the first two plots.
    const auto looked = [&](const tal::MouseEvent& event, Point pointed, std::size_t index) {
        const tal::plotting::Layout& laid = view.laid[index];
        const bool timed = index < 2;

        view.over = index;
        view.cursor = pointed;

        if (event.press == tal::Press::down && event.button == tal::MouseButton::left) {
            view.pressed = pointed.x;
            view.moved = false;
        } else if (event.press == tal::Press::move && view.pressed) {
            if (std::abs(pointed.x - *view.pressed) > 2) view.moved = true;

            if (view.moved && view.held && timed) {
                try {
                    view.held = view.held->shifted(pointed.x - *view.pressed, laid.data.at.x, laid.data.at.x + laid.data.wide);
                } catch (const tal::Trouble&) {
                }

                view.pressed = pointed.x;
            }
        } else if (event.press == tal::Press::up && view.pressed) {
            if (!view.moved) hold(!view.held);

            view.pressed.reset();
        } else if (event.press == tal::Press::wheel && timed && event.axis == tal::Axis::vertical) {
            const double factor = std::pow(zoom, event.steps);
            const std::optional<Point> value = laid.at(pointed);

            if (view.held && value) {
                try {
                    view.held = view.held->zoomed(value->x, factor);
                } catch (const tal::Trouble&) {
                }
            } else if (!view.held) {
                view.shown = std::clamp(view.shown / factor, 0.1, kept);
            }
        }
    };

    const tal::Hook mouse = drawing.on<tal::MouseEvent>([&](tal::MouseEvent& event) {
        const std::optional<Point> pointed = drawing.pointed(event);

        if (!pointed) return;

        const Areas areas = divided(drawing.pixels());

        if (!held && !inside(areas.swing, *pointed)) {
            view.over.reset();
            view.cursor.reset();

            for (std::size_t index = 0; index < areas.plots.size(); ++index)
                if (inside(areas.plots[index], *pointed)) looked(event, *pointed, index);

            if (event.press == tal::Press::up) view.pressed.reset();

            drawing.redraw();

            return;
        }

        view.over.reset();
        view.cursor.reset();

        const std::optional<Matrix> back = placed(areas.swing).inverted();

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
                s.reference = energy(s);
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

        if (key == tal::Key{U'f'}) {
            hold(false);
            freeze = freeze == Freeze::plots ? Freeze::both : freeze == Freeze::both ? Freeze::none : Freeze::plots;
        }

        if (key == tal::Key{tal::Code::escape}) hold(false);

        drawing.redraw();
    });

    drawing.rate(60);
    app.title("Double pendulum");
    app.show(document);

    const Freeze read = freezing(settings.get(pendulum_settings::plots::freeze));
    const int code = app.run();

    if (freeze != read) {
        settings.set(pendulum_settings::plots::freeze, worded(freeze));

        try {
            tal::wait(app.loop(), tal::fs::make(where.parent()));
            tal::wait(app.loop(), settings.write());
        } catch (const tal::Trouble&) {
        }
    }

    return code;
}

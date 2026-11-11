#include "tal/scene.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tal::testing {

// A scene of `screen` cells for a terminal of `colors`, with or without Unicode, drawing emoji
// `how`; its clock stands still until the scene moves it.
Scene::Scene(Size screen, Colors colors, bool unicode, text::Rendering how)
    : clock_(std::make_shared<ManualClock>()), screen_(screen), colors_(colors), unicode_(unicode), how_(how) {}

// The scene's clock as a document takes it. A document shown in the scene has to be made on it, or
// its timers would not ring when the scene waits.
Port<Clock> Scene::time() const { return Port<Clock>{clock_}; }

// The scene's clock, to read or to move by hand.
ManualClock& Scene::clock() { return *clock_; }

// Shows `one` on the scene's screen and makes its first frame, as an application would.
void Scene::show(Document& one) {
    shown_ = &one;
    last_ = nullptr;

    one.display(screen_, colors_, unicode_, how_);

    settled();
}

// A window of `screen` cells now: the document is laid out in it and drawn again.
void Scene::resize(Size screen) {
    screen_ = screen;

    shown().display(screen_, colors_, unicode_, how_);

    settled();
}

// The key `key`, struck once.
void Scene::press(Key key) {
    shown().key(key);

    settled();
}

// The text as a terminal sends it typed: a key for every character, each settled before the next.
// Text that is not UTF-8 throws std::invalid_argument, since no terminal types it.
void Scene::type(std::string_view text) {
    Document& onto = shown();
    std::size_t at = 0;

    while (at < text.size()) {
        const std::optional<char32_t> one = text::decoded(text, at);

        if (!one) throw std::invalid_argument{"tal::testing::Scene::type: the text is not UTF-8"};

        onto.key(Key{*one});

        settled();
    }
}

// The text pasted at once.
void Scene::paste(std::string text) {
    shown().paste(std::move(text));

    settled();
}

// The button `button` pressed and released in the cell `at`, a click where nothing moved.
void Scene::click(Spot at, MouseButton button) {
    Document& onto = shown();

    onto.mouse(Press::down, button, at);
    onto.mouse(Press::up, button, at);

    settled();
}

// A click in the middle of `one` as it stands on the screen. A node that is not placed on the
// screen of the document shown throws std::logic_error: nobody could click it.
void Scene::click(Node& one, MouseButton button) {
    if (one.document() != &shown()) throw std::logic_error{"tal::testing::Scene::click: the node is not in the document shown"};

    Spot at = one.area().at;

    for (const Node* above = one.parent(); above != nullptr; above = above->parent())
        at = Spot{at.x + above->area().at.x, at.y + above->area().at.y};

    const Size size = one.area().size;
    const Spot middle{at.x + size.wide / 2, at.y + size.high / 2};

    if (size.wide <= 0 || size.high <= 0 || !Area{{0, 0}, screen_}.contains(middle))
        throw std::logic_error{"tal::testing::Scene::click: the node does not stand on the screen"};

    click(middle, button);
}

// Two clicks with the left button in the cell `at`, with no time between them: a double click.
void Scene::twice(Spot at) {
    click(at);
    click(at);
}

// The pointer moved to the cell `at` with no button down, which hovers what lies there.
void Scene::point(Spot at) {
    shown().mouse(Press::move, MouseButton::none, at);

    settled();
}

// The wheel turned by `steps` along `axis` with the pointer in the cell `at`.
void Scene::wheel(Spot at, int steps, Axis axis) {
    shown().mouse(Press::wheel, MouseButton::none, at, {}, steps, axis);

    settled();
}

// The clock moved on by `much`; what fell due rings, and the frame follows.
void Scene::wait(std::chrono::milliseconds much) {
    (void)shown();

    clock_->advance(much);

    settled();
}

// The screen as the last frame left it. Before `show` throws std::logic_error.
const Surface& Scene::screen() const {
    (void)shown();

    return *last_;
}

// The cell at `where` on the screen; outside it throws std::out_of_range.
const Cell& Scene::at(Spot where) const { return screen().at(where); }

// Row `y` of the screen as text, every cell's cluster from the first column to the last, borders
// and spaces included; the second half of a wide character adds nothing. A row outside the screen
// throws std::out_of_range.
std::string Scene::row(int y) const {
    const Surface& made = screen();

    if (y < 0 || y >= made.size().high) throw std::out_of_range{"tal::testing::Scene::row: the row lies outside the screen"};

    std::string said;

    for (int x = 0; x < made.size().wide; ++x) said += made.at({x, y}).cluster;

    return said;
}

// The whole screen as text, row after row, each ended with a new line.
std::string Scene::text() const {
    std::string said;

    for (int y = 0; y < screen().size().high; ++y) said += row(y) + "\n";

    return said;
}

// The cell where `said` first starts on the screen, reading rows from the top and each from the
// left; nothing when no row holds it whole from the start of a cell. Empty text is nowhere.
std::optional<Spot> Scene::where(std::string_view said) const {
    const Surface& made = screen();

    if (said.empty()) return std::nullopt;

    for (int y = 0; y < made.size().high; ++y) {
        std::string line;
        std::vector<int> starts;

        for (int x = 0; x < made.size().wide; ++x) {
            const std::string& cluster = made.at({x, y}).cluster;

            for (std::size_t one = 0; one < cluster.size(); ++one) starts.push_back(one == 0 ? x : -1);

            line += cluster;
        }

        for (std::size_t found = line.find(said); found != std::string::npos; found = line.find(said, found + 1))
            if (starts[found] >= 0) return Spot{starts[found], y};
    }

    return std::nullopt;
}

// Where the document shows its caret and how, or nothing.
std::optional<Caret> Scene::caret() const { return shown().caret(); }

// The document shown; before `show` throws std::logic_error.
Document& Scene::shown() const {
    if (shown_ == nullptr) throw std::logic_error{"tal::testing::Scene: no document shown"};

    return *shown_;
}

// Ends a step as an application ends its turn: the document's timers that are due ring, and a frame
// is made when anything changed, or when none was made yet.
void Scene::settled() {
    Document& onto = shown();

    onto.ring();

    if (last_ == nullptr || onto.due()) last_ = &onto.frame();
}

}

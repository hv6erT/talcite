#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "talcite/box.hpp"
#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/form.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"
#include "talcite/tick.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

namespace {

// Reads one row of a view as text, with trailing spaces removed.
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Tells whether the text appears in any row of the view.
bool shown(const Snapshot& one, const std::string& what) {
    for (int y = 0; y < one.size().high; ++y)
        if (read(one, y).find(what) != std::string::npos) return true;

    return false;
}

// The veil puts the box in the middle of the screen, so the trials count rows from the top edge of
// the frame and compare without the left margin: shape, not position. This returns the row of the
// frame's top edge, or -1 when there is no frame.
int framed(const Snapshot& one) {
    for (int y = 0; y < one.size().high; ++y) {
        const std::string row = read(one, y);

        if (row.find("╔") != std::string::npos || row.find("┌") != std::string::npos) return y;
    }

    return -1;
}

// Reads one row of a view as text, without the leading and trailing spaces.
std::string lined(const Snapshot& one, int y) {
    std::string made = read(one, y);

    made.erase(0, made.find_first_not_of(' '));

    return made;
}

// Dispatches one key press to the root.
void pressed(ui::Root& top, ui::Code code, ui::Mods mods = {}) {
    ui::KeyEvent one{ui::Key{code, mods}};

    (void)top.dispatch(one);
}

// A window with an open form. The content and the buttons go in before opening, because the focus
// is placed at the moment of opening.
struct Opened {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Form> one;

    // Puts the window in the root and creates the form with the given title and shape, not opened
    // yet.
    explicit Opened(std::string title, ui::FormShape style = {}) {
        top.children().add(window);

        one = std::make_shared<ui::Form>(std::move(title), style);
    }

    // Opens the form in the window.
    void open() { window->open(one); }

    // Renders the whole root in an area of the given size.
    Snapshot drawn(ui::Size size) { return top.render({{0, 0}, size}); }
};

}

// --- Shape ---------------------------------------------------------------------------------------

// The title in the frame, the content below it, an empty row, the buttons in the middle, like the
// sort dialog in Midnight Commander. The form paints nothing of its own; everything visible is a
// frame, a column and a row.
TRIAL("Form: title in the frame, content, a gap and the buttons in the middle") {
    Opened made{"Sort"};

    made.one->body()->children().add(std::make_shared<ui::Span>("Name"));
    made.one->body()->children().add(std::make_shared<ui::Span>("Size"));
    (void)made.one->buttons().add("OK");
    (void)made.one->buttons().add("Cancel");
    made.open();

    const Snapshot seen = made.drawn({30, 10});
    const int at = framed(seen);

    ought(at >= 0);
    same(lined(seen, at), std::string{"╔═════ Sort ══════╗"});
    same(lined(seen, at + 1), std::string{"║Name             ║"});
    same(lined(seen, at + 2), std::string{"║Size             ║"});
    same(lined(seen, at + 3), std::string{"║                 ║"});
    same(lined(seen, at + 4), std::string{"║[ OK ] [ Cancel ]║"});
    same(lined(seen, at + 5), std::string{"╚═════════════════╝"});
}

TRIAL("Form: the buttons stand in the middle when the content is wider") {
    Opened made{"Filter"};

    made.one->body()->children().add(std::make_shared<ui::Span>("A long line of text here"));
    (void)made.one->buttons().add("OK");
    made.open();

    const Snapshot seen = made.drawn({40, 10});
    const int at = framed(seen);

    same(lined(seen, at + 1), std::string{"║A long line of text here║"});
    same(lined(seen, at + 3), std::string{"║         [ OK ]         ║"});
}

// More buttons than the width can hold: the row breaks into two lines and none disappears.
TRIAL("Form: buttons that do not fit move down to the next line") {
    Opened made{"Many"};

    made.one->body()->children().add(std::make_shared<ui::Span>("Text"));

    for (const char* each : {"First", "Second", "Third", "Fourth", "Fifth"}) (void)made.one->buttons().add(each);

    made.open();

    const Snapshot seen = made.drawn({30, 12});
    const int at = framed(seen);

    same(lined(seen, at + 3), std::string{"║[ First ] [ Second ]║"});
    same(lined(seen, at + 4), std::string{"║[ Third ] [ Fourth ]║"});
    same(lined(seen, at + 5), std::string{"║     [ Fifth ]      ║"});
    same(lined(seen, at + 6), std::string{"╚════════════════════╝"});
}

// Content that fills the height gets the same room when asked for its size and when painted: the
// form sets aside the gap and the buttons first. A box that wants "as much as I need, but leave a
// row for what is below me" then stands right above the status row, and that right above the gap,
// with no empty rows.
TRIAL("Form: content filling the height stands right above the buttons, with no empty rows") {
    Opened made{"Card"};

    const auto lines = std::make_shared<ui::Column>();

    for (int at = 0; at < 20; ++at) lines->children().add(std::make_shared<ui::Span>("line " + std::to_string(at)));

    const std::weak_ptr<ui::Column> inside = lines;

    const auto bounded = std::make_shared<ui::Box>([inside](ui::Size room) {
        const int most = std::max(room.high - 1, 0);
        const auto kept = inside.lock();

        return ui::Size{room.wide, kept ? std::min(kept->wants({room.wide, most}).high, most) : most};
    });

    bounded->children().add(lines);

    made.one->body()->children().add(bounded);
    made.one->body()->children().add(std::make_shared<ui::Span>("status"));
    (void)made.one->buttons().add("OK");
    made.open();

    const Snapshot seen = made.drawn({30, 12});
    const int at = framed(seen);

    // The room has twelve rows and the shadow takes one: the frame, six rows of content, the
    // status, the gap, the buttons, the frame.
    same(at, 0);
    ought(lined(seen, at + 1).starts_with("║line 0"));
    ought(lined(seen, at + 6).starts_with("║line 5"));
    ought(lined(seen, at + 7).starts_with("║status"));
    ought(lined(seen, at + 8).find_first_not_of("║ ") == std::string::npos);
    ought(lined(seen, at + 9).find("[ OK ]") != std::string::npos);
    ought(lined(seen, at + 10).starts_with("╚"));
}

// The footer is the caller's too: what stands in it (here what a filter window says beside its own
// buttons) comes under the content, an empty row apart, and keeps its place when the room is short;
// the content gets what is left.
TRIAL("Form: the footer stands under the content and keeps its place when the room is short") {
    Opened made{"Filter"};

    const auto lines = std::make_shared<ui::Column>();

    for (int at = 0; at < 20; ++at) lines->children().add(std::make_shared<ui::Span>("line " + std::to_string(at)));

    made.one->body()->children().add(lines);
    made.one->footer()->children().add(std::make_shared<ui::Span>("Matches 2 of 4"));
    made.open();

    const Snapshot seen = made.drawn({30, 8});
    const int at = framed(seen);

    // Eight rows, one for the shadow: the frame, three rows of content, the gap, the footer, the
    // frame.
    same(at, 0);
    ought(lined(seen, at + 1).starts_with("║line 0"));
    ought(lined(seen, at + 3).starts_with("║line 2"));
    ought(lined(seen, at + 4).find_first_not_of("║ ") == std::string::npos);
    ought(lined(seen, at + 5).starts_with("║Matches 2 of 4"));
    ought(lined(seen, at + 6).starts_with("╚"));
}

// Without buttons there is neither their row nor the gap before them: the gap stands only between
// content and footer that both take some height.
TRIAL("Form: without buttons there is no gap and no button row") {
    Opened made{"Note"};

    made.one->body()->children().add(std::make_shared<ui::Span>("Just this"));
    made.open();

    const Snapshot seen = made.drawn({30, 10});
    const int at = framed(seen);

    same(lined(seen, at), std::string{"╔═ Note ══╗"});
    same(lined(seen, at + 1), std::string{"║Just this║"});
    same(lined(seen, at + 2), std::string{"╚═════════╝"});
}

TRIAL("Form: without content there is no gap, only the buttons") {
    Opened made{"Ask"};

    (void)made.one->buttons().add("Yes");
    (void)made.one->buttons().add("No");
    made.open();

    const Snapshot seen = made.drawn({30, 10});
    const int at = framed(seen);

    same(lined(seen, at + 1), std::string{"║[ Yes ] [ No ]║"});
    same(lined(seen, at + 2), std::string{"╚══════════════╝"});
}

TRIAL("Form: the title, frame style and shadow come from the caller") {
    Opened made{"Thin", ui::FormShape{ui::Border::thin, false}};

    (void)made.one->buttons().add("OK");
    made.open();

    same(made.one->title(), std::string{"Thin"});

    made.one->title("Other");

    same(made.one->title(), std::string{"Other"});

    const Snapshot seen = made.drawn({30, 10});

    ought(shown(seen, "┌"));
    ought(shown(seen, "Other"));
    ought(!shown(seen, "Thin"));
}

// The frame computes its width from the content and draws the title in what is left, so a window
// with only `[ OK ]` and the title `Make directory` would cut the title. The form wants no less
// than the title needs.
TRIAL("Form: the window is not narrower than its title") {
    Opened made{"Make directory"};

    (void)made.one->buttons().add("OK");
    made.open();

    const Snapshot seen = made.drawn({40, 10});
    const int at = framed(seen);

    same(lined(seen, at), std::string{"╔═ Make directory ═╗"});
    same(lined(seen, at + 1), std::string{"║      [ OK ]      ║"});
}

// --- Buttons -------------------------------------------------------------------------------------

// `add` returns a full button: its own `on<PickEvent>`, `primary`, `keys`. The form hangs nothing
// on it and does not close by itself; closing is part of the caller's action.
TRIAL("Form: a button does what the caller attached, and the window does not close by itself") {
    Opened made{"Ask"};

    int said = 0;

    const std::shared_ptr<ui::Button> ok = made.one->buttons().add("OK");

    ok->primary(true);
    ok->keys().add(ui::Key{U'O'});

    const ui::Hook heard_pick = ok->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    made.open();

    (void)made.drawn({30, 10});

    same(made.one->buttons().many(), std::size_t{1});

    ui::KeyEvent letter{ui::Key{U'O'}};

    ought(made.top.dispatch(letter));
    same(said, 1);
    ought(!made.window->empty());

    ought(shown(made.drawn({30, 10}), "[< OK >]"));
}

// The form has no roles of its own, so its button stays an ordinary button; roles are handed out
// only by a message, which has a family of roles.
TRIAL("Form: a button keeps the role of an ordinary button") {
    Opened made{"Ask"};

    same(made.one->buttons().add("OK")->role(), std::string{"talcite.button"});
}

// Buttons are reached by number, like the children of a node; a number outside the row is the
// caller's error.
TRIAL("Form: a button is reached by number, and one outside the row throws") {
    Opened made{"Ask"};

    const std::shared_ptr<ui::Button> yes = made.one->buttons().add("Yes");
    const std::shared_ptr<ui::Button> no = made.one->buttons().add("No");

    same(made.one->buttons()[0], yes);
    same(made.one->buttons()[1], no);

    bool threw = false;

    try {
        (void)made.one->buttons()[2];
    } catch (const std::out_of_range&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Form: clear removes the buttons together with their row") {
    Opened made{"Ask"};

    (void)made.one->buttons().add("Yes");
    (void)made.one->buttons().add("No");
    made.open();

    ought(shown(made.drawn({30, 10}), "[ Yes ]"));

    made.one->buttons().clear();

    ought(made.one->buttons().empty());
    ought(!shown(made.drawn({30, 10}), "[ Yes ]"));
}

// --- Focus ---------------------------------------------------------------------------------------

// The focus goes to the first thing that takes it (a field in the content), and Tab goes around the
// content and the buttons in one circle, because it is one veil.
TRIAL("Form: Tab goes around the content and the buttons in one circle") {
    Opened made{"Name"};

    const auto field = std::make_shared<ui::Field>(10);
    const auto tick = std::make_shared<ui::Tick>("Only", false);

    made.one->body()->children().add(field);
    made.one->body()->children().add(tick);

    const std::shared_ptr<ui::Button> ok = made.one->buttons().add("OK");
    const std::shared_ptr<ui::Button> cancel = made.one->buttons().add("Cancel");

    made.open();

    (void)made.drawn({30, 10});

    ought(field->focused());

    pressed(made.top, ui::Code::tab);
    ought(tick->focused());

    pressed(made.top, ui::Code::tab);
    ought(ok->focused());

    pressed(made.top, ui::Code::tab);
    ought(cancel->focused());

    pressed(made.top, ui::Code::tab);
    ought(field->focused());
}

// Escape does not close on its own, as in a message and in every veil.
TRIAL("Form: Esc closes only with a handler") {
    Opened made{"Ask"};

    (void)made.one->buttons().add("OK");
    made.open();

    (void)made.drawn({30, 10});

    pressed(made.top, ui::Code::escape);

    ought(!made.window->empty());

    const std::weak_ptr<ui::Form> weak = made.one;

    const ui::Hook heard_key = made.one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    pressed(made.top, ui::Code::escape);

    ought(made.window->empty());
}

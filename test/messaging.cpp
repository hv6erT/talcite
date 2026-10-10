#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/button.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/message.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"

using namespace talcite;

namespace {

// The text of row `y` of the view, with trailing spaces removed.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Whether the text shows anywhere in the frame. In trials where the box appears and disappears, the
// row stops being fixed, so they check presence, not place.
bool shown(const View& one, const std::string& what) {
    for (int y = 0; y < one.size().high; ++y)
        if (read(one, y).find(what) != std::string::npos) return true;

    return false;
}

// Dispatches a press of the key `code` with modifiers `mods` from the root.
void pressed(ui::Root& top, ui::Code code, ui::Mods mods = {}) {
    ui::KeyEvent one{ui::Key{code, mods}};

    (void)top.dispatch(one);
}

// Dispatches a typed letter from the root.
void typed(ui::Root& top, char32_t letter) {
    ui::KeyEvent one{ui::Key{letter}};

    (void)top.dispatch(one);
}

// One answer in a trial: the text and its hotkey. The library has no such structure (an answer is a
// button), but the trials build dozens of them, and one line apiece reads better than three.
struct Said {
    std::string text;
    char32_t key = 0;
};

// A window with an open message, used as it is for real: buttons before opening, because the focus
// is placed at the moment of opening and does not catch up with what comes later.
struct Opened {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Message> one;

    // Which answer was given and how many times the window closed, both learned the way the
    // application learns them: the action sits on the button, and the veil announces the close.
    int said = -99;
    int closed = 0;

    // Builds the message with the given answers, hangs on each one an action that records it and
    // closes the message, counts the closes, and opens the message in the window.
    Opened(std::string title, std::string text, std::vector<Said> answers = {},
          ui::MessageShape style = {}) {
        top.children().add(window);

        one = std::make_shared<ui::Message>(std::move(title), std::move(text), style);

        for (std::size_t at = 0; at < answers.size(); ++at) {
            const std::shared_ptr<ui::Button> made =
                one->buttons().add(std::move(answers[at].text));

            if (answers[at].key != 0) made->keys().add(answers[at].key);

            const int which = static_cast<int>(at);

            // Closing is part of the answer's action, so the caller calls it; the window does not
            // leave the screen by itself.
            (void)made->on<ui::PickEvent>([this, which](ui::PickEvent&) {
                said = which;

                one->close();

                return true;
            });
        }

        (void)one->on<ui::CloseEvent>([this](ui::CloseEvent&) {
            ++closed;

            return true;
        });

        window->open(one);
    }

    // Renders the whole window at the given size.
    View drawn(ui::Size size) { return top.render({{0, 0}, size}); }
};

}

// --- Shape ------------------------------------------------------------------------------------

TRIAL("Message: title above the content box, buttons in a second box") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    const View seen = made.drawn({30, 12});

    same(read(seen, 0), std::string{"      ╔═════════════╗"});
    same(read(seen, 1), std::string{"      ║    Error    ║"});
    same(read(seen, 2), std::string{"      ║┌───────────┐║"});
    same(read(seen, 3), std::string{"      ║│Nie da się.│║"});
    same(read(seen, 4), std::string{"      ║└───────────┘║"});
    same(read(seen, 5), std::string{"      ║             ║"});
    same(read(seen, 6), std::string{"      ║┌───────────┐║"});
    same(read(seen, 7), std::string{"      ║│  [ OK ]   │║"});
    same(read(seen, 8), std::string{"      ║└───────────┘║"});
    same(read(seen, 9), std::string{"      ╚═════════════╝"});
}

// A single answer is an ordinary button, not the primary one: that is how `[ OK ]` looks in
// Midnight Commander. Enter works anyway, because the focus presses it, not the mark.
TRIAL("Message: a single answer is not primary") {
    Opened made{"Uwaga", "Coś.", {{"OK", U'O'}}};

    ought(!made.one->buttons()[0]->primary());
}

TRIAL("Message: without a title there is no title row") {
    Opened made{std::string{}, "Sama treść.", {{"OK", U'O'}}};

    const View seen = made.drawn({30, 12});

    same(read(seen, 2), std::string{"      ║┌───────────┐║"});
    same(read(seen, 3), std::string{"      ║│Sama treść.│║"});
}

TRIAL("Message: without answers there is no second box") {
    Opened made{"Czekaj", "Przeglądam…"};

    const View seen = made.drawn({30, 10});

    same(read(seen, 1), std::string{"      ╔═════════════╗"});
    same(read(seen, 2), std::string{"      ║   Czekaj    ║"});
    same(read(seen, 3), std::string{"      ║┌───────────┐║"});
    same(read(seen, 4), std::string{"      ║│Przeglądam…│║"});
    same(read(seen, 5), std::string{"      ║└───────────┘║"});
    same(read(seen, 6), std::string{"      ╚═════════════╝"});
}

// Without boxes the message looks like a form: the title on the edge of the frame, the content with
// a cell of breathing room on both sides, the buttons centered below an empty row.
TRIAL("Message: boxes off give the title on the frame and content with breathing room") {
    Opened made{"Bez", "Goła treść.", {{"OK", U'O'}}, ui::MessageShape{ui::Border::twin, false}};

    const View seen = made.drawn({30, 10});

    same(read(seen, 2), std::string{"      ╔════ Bez ════╗"});
    same(read(seen, 3), std::string{"      ║ Goła treść. ║"});
    same(read(seen, 4), std::string{"      ║             ║"});
    same(read(seen, 5), std::string{"      ║   [ OK ]    ║"});
    same(read(seen, 6), std::string{"      ╚═════════════╝"});
    same(made.one->role(), std::string{"talcite.message"});
}

// Long content without boxes wraps in a room narrower by the breathing room, so no word is lost
// behind the frame, and the window wants no more than the room, even a very narrow one.
TRIAL("Message: without boxes long content wraps with breathing room, the window fits the room") {
    Opened made{"T", "jeden dwa trzy cztery pięć sześć siedem osiem", {{"OK", U'O'}},
                ui::MessageShape{ui::Border::twin, false}};

    // At every width the first and the last word show: wrapping is computed in the same room the
    // text then stands in.
    for (const int wide : {20, 21, 22, 23, 24, 25, 26}) {
        const View seen = made.drawn({wide, 14});

        bool first = false;
        bool last = false;

        for (int y = 0; y < 14; ++y) {
            first = first || read(seen, y).find("jeden") != std::string::npos;
            last = last || read(seen, y).find("osiem") != std::string::npos;
        }

        ought(first && last);
    }

    ought(made.one->wants({3, 10}).wide <= 3);
}

// A title changed after construction goes onto the frame, and a long title widens the window
// instead of being cut.
TRIAL("Message: without boxes a new title stands on the frame and widens the window") {
    Opened made{"A", "b", {{"OK", U'O'}}, ui::MessageShape{ui::Border::twin, false}};

    made.one->title("Bardzo długi tytuł");

    same(made.one->title(), std::string{"Bardzo długi tytuł"});

    const View seen = made.drawn({40, 10});

    ought(read(seen, 2).find("═ Bardzo długi tytuł ═") != std::string::npos);
}

// Without content there is no content row and no gap; without buttons, no gap and no row.
TRIAL("Message: without boxes empty content and no buttons leave no holes") {
    Opened bare{"Tytuł", "", {{"OK", U'O'}}, ui::MessageShape{ui::Border::twin, false}};

    const View one = bare.drawn({30, 10});

    int rows = 0;

    for (int y = 0; y < 10; ++y)
        if (!read(one, y).empty()) ++rows;

    same(rows, 3);

    Opened mute{"Tytuł", "Czekam.", {}, ui::MessageShape{ui::Border::twin, false}};

    const View two = mute.drawn({30, 10});

    rows = 0;

    for (int y = 0; y < 10; ++y)
        if (!read(two, y).empty()) ++rows;

    same(rows, 3);
}

TRIAL("Message: buttons stand in the middle and have a gap") {
    Opened made{"Pytanie", "Plik już istnieje.", {{"Tak", U'T'}, {"Nie", U'N'}}};

    const View seen = made.drawn({40, 12});

    same(read(seen, 7), std::string{"        ║│ [ Tak ] [ Nie ]  │║"});
}

// --- Width ------------------------------------------------------------------------------------

TRIAL("Message: long content wraps instead of stretching the window across the screen") {
    Opened made{"Uwaga",
               "To jest bardzo długie zdanie, które nie ma prawa rozepchnąć okna na całą "
               "szerokość ekranu, bo tak się okien nie robi.",
               {{"OK", U'O'}}};

    const View seen = made.drawn({60, 16});

    // Four cells of breathing room on the sides and room for the shadow: the window does not touch
    // the edge.
    ought(read(seen, 1).size() > 40);
    same(seen.at({0, 1}).letter, U' ');
    same(seen.at({59, 1}).letter, U' ');

    // The content went onto several rows instead of being cut.
    ought(read(seen, 4).find("To jest") != std::string::npos);
    ought(!read(seen, 5).empty());
}

TRIAL("Message: short content does not inflate the window") {
    Opened made{"A", "b", {{"OK", U'O'}}};

    const View seen = made.drawn({60, 12});

    // The width is dictated by the widest child, here the row of buttons, not the content.
    same(read(seen, 3), std::string{"                        ║│  b   │║"});
    same(read(seen, 7), std::string{"                        ║│[ OK ]│║"});
}

// --- Answer -----------------------------------------------------------------------------------

// The answer's action lives on the button, just as in a menu item and in a legend field, and
// closing the window is part of it; the message does not close by itself.
TRIAL("Message: an answer does what the caller attached") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}, {"Przerwij", U'P'}}};

    (void)made.drawn({44, 14});

    made.one->buttons()[1]->pick();

    same(made.said, 1);
    ought(made.window->empty());
}

// Escape does not close by itself, because the veil does not take it: a window performing an
// action that the caller cannot cancel would be deciding for the caller. Closing is still announced
// by a separate event, because there is no "close" button to hang an action on; it is the caller's
// handler that starts the close.
TRIAL("Message: Esc closes only with a handler, and the close is announced") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    pressed(made.top, ui::Code::escape);

    same(made.closed, 0);
    ought(!made.window->empty());

    const std::weak_ptr<ui::Message> weak = made.one;

    (void)made.one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    pressed(made.top, ui::Code::escape);

    same(made.said, -99);
    same(made.closed, 1);
    ought(made.window->empty());
}

// Every close is announced, not only the one from Esc, also when the caller's handler calls it.
// Whoever listens to one event learns about both paths.
TRIAL("Message: a close from a handler is announced too") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    made.one->buttons()[0]->pick();

    same(made.said, 0);
    same(made.closed, 1);
}

// Opening puts the focus on the first answer, so Enter presses the one you are on; the primary mark
// decides only when the focus is elsewhere. That is how Midnight Commander does it, and it follows
// from the focused button taking Enter itself.
TRIAL("Message: Enter presses the answer under focus") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons()[1]->primary(true);

    (void)made.drawn({44, 14});

    pressed(made.top, ui::Code::enter);

    same(made.said, 0);
}

// The primary mark presses nothing: it draws the bracket with arrows and that is all. Enter belongs
// to whoever has the focus; when the focus has left the buttons, it presses nothing.
TRIAL("Message: without focus on a button Enter presses nothing") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons()[1]->primary(true);

    (void)made.drawn({44, 14});

    // The focus goes back to the window itself; then the dialog handles the key, not a button.
    made.one->focus();

    pressed(made.top, ui::Code::enter);

    same(made.said, -99);
    same(made.closed, 0);
    ought(!made.window->empty());
}

TRIAL("Message: a hotkey works from the whole window") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    typed(made.top, U'n');

    same(made.said, 1);
}

// The primary mark belongs to the button, not to the message, since it is something the button
// knows about itself. Nobody enforces that there is only one: the dialog takes the first found
// depth-first, and that rule takes the place of enforcement.
TRIAL("Message: the primary answer is marked on the button") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    ought(!made.one->buttons()[0]->primary());
    ought(!made.one->buttons()[1]->primary());

    made.one->buttons()[0]->primary(true);

    ought(made.one->buttons()[0]->primary());
    ought(!made.one->buttons()[1]->primary());

    made.one->buttons()[0]->primary(false);

    ought(!made.one->buttons()[0]->primary());
    ought(!made.one->buttons()[1]->primary());
}

TRIAL("Message: the primary answer is drawn in a bracket with arrows") {
    Opened made{"Pytanie", "Co?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons()[0]->primary(true);

    same(read(made.drawn({40, 12}), 7), std::string{"        ║│[< Tak >] [ Nie ]│║"});
}

// --- Roles ------------------------------------------------------------------------------------

TRIAL("Message: the role's stem is handed out to all the parts") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    same(made.one->role(), std::string{"talcite.message"});

    ui::Styles these;

    Style window{};
    Style title{};
    Style text{};
    Style box{};
    Style button{};

    window.background = Color{false, 1, 1, 1};
    title.foreground = Color{false, 2, 2, 2};
    text.foreground = Color{false, 3, 3, 3};
    box.foreground = Color{false, 4, 4, 4};
    button.foreground = Color{false, 5, 5, 5};

    these["moj.blad"] = window;
    these["moj.blad.title"] = title;
    these["moj.blad.text"] = text;
    these["moj.blad.box"] = box;
    these["moj.blad.button"] = button;

    made.top.skin(ui::Skin{these});
    made.one->role("moj.blad");

    // Opening puts the focus on the button, which then asks for the `.focus` suffix. Here we want
    // to see the resting role, so the focus goes back to the window itself.
    made.one->focus();

    const View seen = made.drawn({30, 12});

    // The window's edge, the title, the box's line, the content and the button, each from its own
    // role.
    same(seen.at({6, 0}).style.background, (Color{false, 1, 1, 1}));
    same(seen.at({11, 1}).style.foreground, (Color{false, 2, 2, 2}));
    same(seen.at({7, 2}).style.foreground, (Color{false, 4, 4, 4}));
    same(seen.at({8, 3}).style.foreground, (Color{false, 3, 3, 3}));
    same(seen.at({10, 7}).style.foreground, (Color{false, 5, 5, 5}));
}

TRIAL("Message: a button under focus takes the suffix of its stem") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    ui::Styles these;

    Style hot{};

    hot.foreground = Color{false, 9, 9, 9};

    these["moj.blad.button.focus"] = hot;

    made.top.skin(ui::Skin{these});
    made.one->role("moj.blad");

    made.one->buttons()[0]->focus();

    same(made.drawn({30, 12}).at({10, 7}).style.foreground, (Color{false, 9, 9, 9}));
}

// --- Construction -----------------------------------------------------------------------------

// The message puts itself together and has no second path by which it could be put together
// wrong, just as the menu bar accepts content only through `content()`.
TRIAL("Message: does not accept a child from outside") {
    ui::Message one{"Tytuł", "Treść"};

    bool threw = false;

    try {
        one.children().add(std::make_shared<ui::Span>("obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Message: content and title can be changed after construction") {
    Opened made{"Stary", "Stara treść.", {{"OK", U'O'}}};

    made.one->title("Nowy");
    made.one->text("Nowa treść.");

    same(made.one->title(), std::string{"Nowy"});
    same(made.one->text(), std::string{"Nowa treść."});

    const View seen = made.drawn({30, 12});

    ought(read(seen, 1).find("Nowy") != std::string::npos);
    ought(read(seen, 3).find("Nowa treść.") != std::string::npos);
    ought(read(seen, 3).find("Stara") == std::string::npos);
}

// Content set after construction shows. The content box stands in the column from the start, and
// whether it is visible follows from its size, so text given later does not end up on a node that
// is nobody's child and never shows.
TRIAL("Message: content set after construction shows") {
    Opened made{"Uwaga", std::string{}, {{"OK", U'O'}}};

    ought(!shown(made.drawn({30, 12}), "Jest treść."));

    made.one->text("Jest treść.");

    ought(shown(made.drawn({30, 12}), "Jest treść."));
}

// The same from the other side: the content box disappears when the content is taken away, and
// leaves behind neither an outline nor a gap row.
TRIAL("Message: content taken away after construction hides the box") {
    Opened made{"Uwaga", "Była treść.", {{"OK", U'O'}}};

    const View was = made.drawn({30, 12});

    made.one->text(std::string{});

    const View seen = made.drawn({30, 12});

    ought(!shown(seen, "Była treść."));
    ought(shown(seen, "[ OK ]"));

    // Without content the window is shorter by the box and by the row between the boxes.
    ought(read(seen, 9).empty());
    ought(!read(was, 9).empty());
}

TRIAL("Message: an answer added after construction works like any other") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}}};

    int seen = 0;

    const std::shared_ptr<ui::Button> more = made.one->buttons().add("Nie");

    more->keys().add(U'n');

    (void)more->on<ui::PickEvent>([&seen](ui::PickEvent&) {
        ++seen;

        return true;
    });

    ought(shown(made.drawn({44, 14}), "[ Nie ]"));

    typed(made.top, U'n');

    same(seen, 1);
}

TRIAL("Message: the first answer added after construction shows the second box") {
    Opened made{"Czekaj", "Przeglądam…"};

    ought(!shown(made.drawn({30, 12}), "[ Stop ]"));

    made.one->buttons().add("Stop");

    ought(shown(made.drawn({30, 12}), "[ Stop ]"));
}

TRIAL("Message: clear() takes the answers away together with the box") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    made.one->buttons().clear();

    same(made.one->buttons().many(), std::size_t{0});

    const View seen = made.drawn({44, 14});

    ought(!shown(seen, "[ Tak ]"));
    ought(shown(seen, "Co robić?"));
}

// An answer without an action does nothing, and that is how it should be. The library does not add
// behavior to a button that the caller did not hang on it, so a freshly added answer does not close
// the window and announces nothing.
TRIAL("Message: an answer without a handler does nothing") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons().clear();

    const std::shared_ptr<ui::Button> only = made.one->buttons().add("Przerwij");

    (void)made.drawn({44, 14});

    only->pick();

    same(made.said, -99);
    same(made.closed, 0);
    ought(!made.window->empty());
}

// A button removed from the window still has its action: it belongs to the caller, and nobody takes
// it away or strips its power. Since that action closes the window, the window closes, even though
// the button no longer stands in the tree; the library decides nothing for anyone here.
TRIAL("Message: a button removed by clear() still does its job") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    const std::shared_ptr<ui::Button> was = made.one->buttons()[0];

    made.one->buttons().clear();

    (void)made.drawn({44, 14});

    was->pick();

    same(made.said, 0);
    same(made.closed, 1);
}

// The answer's role is handed out both when it is added and when the stem changes, in both
// directions, whichever comes first.
TRIAL("Message: an answer added after the stem changes takes its suffix") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    made.one->role("moj.blad");

    const std::shared_ptr<ui::Button> more = made.one->buttons().add("Anuluj");

    same(more->role(), std::string{"moj.blad.button"});

    made.one->role("inny.blad");

    same(more->role(), std::string{"inny.blad.button"});
    same(made.one->buttons()[0]->role(), std::string{"inny.blad.button"});
}

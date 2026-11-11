#include "tal/window.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/key.hpp"
#include "tal/text.hpp"

#include "lettering.hpp"

namespace tal {

namespace {

// Whether `one` is `outer` or lies under it.
bool under(const Node& outer, const Node& one) {
    for (const Node* at = &one; at != nullptr; at = at->parent())
        if (at == &outer) return true;

    return false;
}

// The first node in `root`, itself included, that can take the focus now, in the order of the tree.
Node* taking(Node& root) {
    if (root.focusable()) return &root;

    for (Node& one : root.children())
        if (Node* found = taking(one)) return found;

    return nullptr;
}

}

// A window titled `named`, in the class "window", and a modal one in the class "dialog" too: the
// sheet gives them their frames and shadows. Its bound actions run on their shortcuts and letters,
// and a modal window closes on Esc; these are default actions, so a node inside takes a key first.
Window::Window(std::string named, Modal modal) : title(*this, Effect::layout, std::move(named)), modal_(modal) {
    classes(modal == Modal::yes ? std::vector<std::string>{"window", "dialog"} : std::vector<std::string>{"window"});

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Key key = what.key;

        for (const std::shared_ptr<Action>& one : bound_)
            if (one->enabled() && one->key() && alike(*one->key(), key)) {
                what.take();
                one->run();

                return;
            }

        const Mods mods = key.mods();

        if (key.letter() != 0 && !mods.ctrl)
            for (const std::shared_ptr<Action>& one : bound_)
                if (one->enabled() && lettering::same(key.letter(), one->letter())) {
                    what.take();
                    one->run();

                    return;
                }

        if (modal_ == Modal::yes && key.code() == Code::escape && mods == Mods{}) {
            what.take();
            close();
        }
    }));
}

// Stands the window at `where` in its layers, or, with nothing, in the middle in the size of its
// content.
void Window::seat(std::optional<Area> where) {
    if (where == seat_) return;

    seat_ = where;

    relayout();
}

// Binds `one` to the window: its shortcut and its letter run it while the focus is inside. Bound
// twice, it still runs once, since the first match takes the key.
void Window::bind(std::shared_ptr<Action> one) {
    if (one == nullptr) throw std::invalid_argument{"tal::Window::bind: no action"};

    bound_.push_back(std::move(one));
}

// Closes the window: its layers take it away and give the focus back, then it hears `Closed`. A
// window outside layers only hears `Closed`.
void Window::close() {
    const std::shared_ptr<Node> kept = shared_from_this();

    if (auto* layers = dynamic_cast<Layers*>(parent())) layers->closed(*this);

    Closed told;

    notified(told);
}

// As large as its content, and wide enough for its title on the frame, between two spaces.
Size Window::measure(Limits room) const {
    Size made = Node::measure(room);

    if (!title->empty() && style().border != Border::none) made.wide = std::max(made.wide, text::width(*title, rendering()) + 2);

    return made;
}

// The title on the top edge of the frame, in the middle between two spaces, in the look of the part
// "title"; a window without a frame shows none.
void Window::outline(Canvas& box) const {
    if (title->empty() || style().border == Border::none) return;

    const std::string named = " " + *title + " ";

    (void)box.put({std::max((box.size().wide - text::width(named, rendering())) / 2, 0), 0}, named, look("title"));
}

// Empty layers, in the class "layers". A press of the mouse in a window that is not on top raises
// it, before the press goes on to what lies under it.
Layers::Layers() {
    classes({"layers"});

    hooks_.add(on<MouseEvent>(
        [this](MouseEvent& what) {
            if (what.press != Press::down) return;

            for (Node* at = &what.target(); at != nullptr; at = at->parent()) {
                if (at->parent() != this) continue;

                auto* window = dynamic_cast<Window*>(at);

                if (window != nullptr && window->modal() == Modal::no) raise(*window);

                return;
            }
        },
        Phase::capture));
}

// Opens `one` on top: a modeless window above the other modeless ones and under every modal one, a
// modal window above everything, confining the focus and the mouse. The focus goes into the window
// once it is laid out, when it is the top one; it comes back where it was when the window closes. A
// window already open somewhere throws std::logic_error, and none std::invalid_argument.
Window& Layers::open(std::shared_ptr<Window> one) {
    if (one == nullptr) throw std::invalid_argument{"tal::Layers::open: no window"};

    if (one->parent() != nullptr) throw std::logic_error{"tal::Layers::open: the window is open already"};

    kept();

    Opened made;

    if (document() != nullptr)
        if (Node* focus = document()->focused()) made.returning = focus->weak_from_this();

    const std::size_t at = one->modal() == Modal::yes ? children().size() : modals();
    auto& window = static_cast<Window&>(children().insert(at, std::move(one)));

    made.window = std::static_pointer_cast<Window>(window.shared_from_this());
    made.arranged = window.on<Arranged>([this, &window](Arranged&) {
        Opened* held = record(window);

        if (held == nullptr || held->entered) return;

        held->entered = true;

        if (top() == &window) entered(window);
    });

    opened_.push_back(std::move(made));

    if (window.modal() == Modal::yes && document() != nullptr) document()->confine(&window);

    return window;
}

// The window on top, or none.
Window* Layers::top() const {
    if (children().empty()) return nullptr;

    Node* last = nullptr;

    for (Node& one : children()) last = &one;

    return dynamic_cast<Window*>(last);
}

// Raises the modeless window `one` above the other modeless ones; under an open modal window it
// stays under it, without the focus. Taken out of the tree and put back, it is laid out anew, and
// the focus goes into it then, as on opening. A window that is not one of these layers' modeless
// windows throws std::logic_error.
void Layers::raise(Window& one) {
    if (one.parent() != this || one.modal() == Modal::yes) throw std::logic_error{"tal::Layers::raise: not a modeless window of these layers"};

    if (uppermost(Modal::no, nullptr) == &one) return;

    kept();

    const std::shared_ptr<Node> held = children().remove(one);

    if (Opened* made = record(one)) made->entered = false;

    (void)children().insert(modals(), held);
}

// Raises the modeless window opened after the top one, round the end.
void Layers::next() { stepped(false); }

// Raises the modeless window opened before the top one, round the start.
void Layers::previous() { stepped(true); }

// Places every window: a full one over the whole inside, a seated one at its seat (from the
// inside's corner), the others in the middle in the size of their content, no larger than the
// inside without two columns on each side and a row above and below.
void Layers::arrange(Area inside) {
    for (Node& one : children()) {
        auto* window = dynamic_cast<Window*>(&one);

        if (window != nullptr && *window->full) {
            one.placed(inside);

            continue;
        }

        if (window != nullptr && window->seat()) {
            const Area seat = *window->seat();

            one.placed(Area{{inside.at.x + seat.at.x, inside.at.y + seat.at.y}, seat.size});

            continue;
        }

        const Size most{std::max(inside.size.wide - 4, 0), std::max(inside.size.high - 2, 0)};
        const Size size = one.measured(Limits{{0, 0}, most});

        one.placed(Area{{inside.at.x + (inside.size.wide - size.wide) / 2, inside.at.y + (inside.size.high - size.high) / 2}, size});
    }
}

// Takes away the window `one`, which is closing: the focus is confined to the modal window left on
// top, or freed; a node that asked for the focus meanwhile gets it now, and when none did and the
// focus was in the closing window, it goes back where it was before the window opened (when that
// lies outside the confinement left, it waits there like any request), or else into the window now
// on top.
void Layers::closed(Window& one) {
    const Opened* held = record(one);
    const std::shared_ptr<Node> returning = held != nullptr ? held->returning.lock() : nullptr;

    if (Document* in = document()) {
        const Node* before = in->focused();
        const bool inside = before == nullptr || under(one, *before);

        if (in->confined() == &one) in->confine(uppermost(Modal::yes, &one));

        const Node* after = in->focused();

        if (inside && (after == nullptr || under(one, *after))) {
            if (returning != nullptr && returning->document() == in && !under(one, *returning) && returning->focusable()) {
                returning->focus();
            } else if (Window* next = uppermost(Modal::yes, &one) != nullptr ? uppermost(Modal::yes, &one) : uppermost(Modal::no, &one)) {
                entered(*next);
            }
        }
    }

    std::erase_if(opened_, [&one](const Opened& made) { return made.window.lock().get() == &one; });

    const std::shared_ptr<Node> gone = children().remove(one);
}

// Puts the focus into `one`: where it was when the window last lost it, or on the first node inside
// that takes it.
void Layers::entered(Window& one) {
    if (const Opened* held = record(one))
        if (const std::shared_ptr<Node> was = held->kept.lock(); was != nullptr && under(one, *was) && was->focusable()) {
            was->focus();

            return;
        }

    if (Node* first = taking(one)) first->focus();
}

// Remembers the focus in the window that holds it, for when it comes back on top.
void Layers::kept() {
    if (document() == nullptr || document()->focused() == nullptr) return;

    Node* focus = document()->focused();

    for (Opened& made : opened_)
        if (const std::shared_ptr<Window> window = made.window.lock(); window != nullptr && under(*window, *focus)) made.kept = focus->weak_from_this();
}

// Raises the modeless window opened after (or, `back`, before) the top one, round the end.
void Layers::stepped(bool back) {
    std::vector<Window*> order;

    for (const Opened& made : opened_)
        if (const std::shared_ptr<Window> window = made.window.lock(); window != nullptr && window->modal() == Modal::no) order.push_back(window.get());

    if (order.size() < 2) return;

    const auto found = std::find(order.begin(), order.end(), uppermost(Modal::no, nullptr));
    const std::size_t at = found == order.end() ? 0 : static_cast<std::size_t>(found - order.begin());
    const std::size_t to = back ? (at + order.size() - 1) % order.size() : (at + 1) % order.size();

    raise(*order[to]);
}

// What is remembered about the open window `one`, or nothing.
Layers::Opened* Layers::record(const Window& one) {
    for (Opened& made : opened_)
        if (made.window.lock().get() == &one) return &made;

    return nullptr;
}

// The topmost window that is modal (or modeless, as `modal` says), other than `besides`, or none.
Window* Layers::uppermost(Modal modal, const Window* besides) const {
    Window* made = nullptr;

    for (Node& one : children())
        if (auto* window = dynamic_cast<Window*>(&one); window != nullptr && window != besides && window->modal() == modal) made = window;

    return made;
}

// Where the modal windows begin among the children: after every modeless one.
std::size_t Layers::modals() const {
    std::size_t made = 0;

    for (Node& one : children()) {
        auto* window = dynamic_cast<Window*>(&one);

        if (window != nullptr && window->modal() == Modal::yes) break;

        ++made;
    }

    return made;
}

// A modal message titled `named`, in the class "message": the text, wrapped to the room, over the
// answers as buttons in the middle. An answer runs its action and closes the message, unless a
// handler of the action takes the run; Esc closes it without an answer.
Message::Message(std::string named, std::string text, std::vector<std::shared_ptr<Action>> answers)
    : Window(std::move(named), Modal::yes) {
    add_class("message");

    Column& column = children().add<Column>(1);
    Label& said = column.children().add<Label>(std::move(text));

    said.wrap = true;

    if (answers.empty()) return;

    Row& row = column.children().add<Align>(Axis::horizontal, Edge::middle).children().add<Row>(1);

    for (const std::shared_ptr<Action>& one : answers) {
        (void)row.children().add<Button>(one);

        bind(one);

        answered_.add(one->on<Run>([this](Run&) { close(); }));
    }
}

// A form titled `named`, in the class "form": its body, which takes the room the window has to
// spare, over the row of answers in the middle.
Form::Form(std::string named, Modal modal) : Window(std::move(named), modal) {
    add_class("form");

    Column& column = children().add<Column>(1);

    body_ = &column.children().add<Column>();
    body_->share(Share{1, 0});
    answers_ = &column.children().add<Align>(Axis::horizontal, Edge::middle).children().add<Row>(1);
}

// Adds the answer `one` as a button in the row of answers, bound to the window.
void Form::answer(std::shared_ptr<Action> one) {
    (void)answers_->children().add<Button>(one);

    bind(std::move(one));
}

}

#include "popup.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/modals.hpp"

namespace talcite::ui {

// `Rolled` is a veil that places its box under the given node. Transparency, modality, the shadow
// and returning the focus come from `Modal`, and none of that is repeated here. It does not know
// what a choice, a list or a history is. It gets the node under which it has to stand, and that is
// all, so it serves anyone who needs something rolled out beneath them, without a single branch for
// "and what if this is a choice".
Rolled::Rolled(std::weak_ptr<Node> under) : under_(std::move(under)) {}

// A rolled-out list closes on Escape, and this is the only place where that key closes a veil.
// `Modal` does not take Escape, because closing a window the caller cannot cancel would decide for
// the caller. Here there is no such doubt: nobody outside assembles or sees the rolled-out list; it
// is entirely a widget of the library, just like the arrows in a menu. The rest of the behavior
// comes from `Modal`, including swallowing everything else.
bool Rolled::handle(KeyEvent& what) {
    if (what.key().code() == Code::escape) {
        close();

        return true;
    }

    return Modal::handle(what);
}

// A click beside the rolled-out list rolls it up, as a click beside an open menu does: the list is
// a part of the widget below, not a window waiting for an answer, so the click is not an answer
// either and goes no further. A click on the list hits its rows or its frame, and only one beside it
// lands on the veil itself. The veil closes itself, so this holds also after the widget is gone.
bool Rolled::handle(ClickEvent& what) {
    if (what.target() == this) {
        close();

        return true;
    }

    return Modal::handle(what);
}

// The box stands under its anchor. When there is not enough room below, it goes above; when it
// sticks out to the right, it moves left. Just like the last menu in a menu bar, and for the same
// reason: only the one who has the whole screen knows its edges, and the veil has it.
//
// A node that no longer exists cannot be held on to; then the center is returned, which is the
// behavior of `Modal`. This is not a failure: a widget may be taken off the tree while its list is
// rolled out.
Area Rolled::seat(Size given) const {
    if (children().empty()) return {};

    const std::shared_ptr<Node> under = under_.lock();

    if (!under) return Modal::seat(given);

    const std::optional<Area> where = under->screen_area();

    if (!where.has_value()) return Modal::seat(given);

    // Room for the shadow is subtracted before computing, just as `Modal` does; otherwise the
    // shadow would go off screen, and at the bottom edge there would be no row left for it.
    const int aside = shade() ? 2 : 0;
    const int apart = shade() ? 1 : 0;

    const Size room{std::max(given.wide - aside, 0), std::max(given.high - apart, 0)};

    Size wants = children().front()->wants(room);

    wants.wide = std::min(wants.wide, room.wide);
    wants.high = std::min(wants.high, room.high);

    if (wants.wide <= 0 || wants.high <= 0) return {};

    const int below = where->at.y + where->size.high;

    // Below, if it fits; above, if not; and when it does not fit there either, it stays below and
    // gets cut. The choice of the lesser evil is ours here, because only we know both dimensions
    // at once.
    int y = below;

    if (below + wants.high > room.high) {
        const int above = where->at.y - wants.high;

        if (above >= 0) y = above;
    }

    y = std::clamp(y, 0, std::max(room.high - wants.high, 0));

    const int x = std::clamp(where->at.x, 0, std::max(room.wide - wants.wide, 0));

    return {{x, y}, wants};
}

// The nearest veil window up the tree, or null. The same trick by which a menu item finds its menu
// and a veil its window, only one floor further, because there is usually a form between the
// widget and the window.
Modals* housing(Node* from) {
    for (Node* at = from; at != nullptr; at = at->parent())
        if (Modals* one = dynamic_cast<Modals*>(at)) return one;

    return nullptr;
}

// Rolls the list up, if it is still open, and forgets it.
//
// `Popup` is a rolled-out list as part of a widget. A selection and a history hold one `Popup` and
// differ only in where they take their strings from and what they do with the chosen row. The
// object is not a node and lives in the widget under which it rolls the list out; the widget holds
// it through `unique_ptr`, because the public header cannot show this class. It shows at most
// `rows()` rows at once (eight by default, never fewer than one); a longer list gets a scrollbar.
//
// The rolled-out list belongs to the window, not to us. The window holds it and returns the focus,
// and the veil closes itself on Escape. Holding it strongly, we would be left with a dead handle
// after every closing by someone else's hand; a weak pointer says exactly as much as is true, "this
// one is mine as long as it exists", and `opened()` is true exactly while it does.
void Popup::close() {
    if (const std::shared_ptr<Modal> one = rolled_.lock()) one->close();

    rolled_.reset();
}

// Assembles and opens the rolled-out list: a frame, a list with the items, a veil under the
// widget, an entry in the window. `took` gets the index of the chosen row. No veil window up the
// tree means "there is nowhere to open it" and does nothing; so does an empty list or one that is
// already open.
void Popup::open(const std::vector<std::string>& among, std::size_t which,
                 std::function<void(std::size_t)> took) {
    if (among.empty() || opened()) return;

    Modals* window = housing(under_->parent());

    if (window == nullptr) return;

    // The width comes from the longest item, not from the widget: a rolled-out list has to show
    // the content, not fit the width of the row it came from. The veil trims it to the screen
    // edges anyway.
    int wide = 0;

    for (const std::string& one : among) wide = std::max(wide, measured(one).wide);

    const int high = std::max(std::min(static_cast<int>(among.size()), rows_), 1);
    const bool bar = static_cast<int>(among.size()) > high;

    const auto list = std::make_shared<List>(wide + (bar ? 1 : 0), high, ScrollShape{bar});

    for (const std::string& one : among) list->children().add(std::make_shared<ListItem>(one));

    if (which < among.size()) list->which(which);

    // The choice comes back as an ordinary event, not a callback. `ListItem::pick()` announces a
    // `PickEvent`, and we listen to it on the list we assembled ourselves. `Rolled` takes no part
    // in this and does not need to know why it carries the list.
    //
    // The hooks are this object's: the rolled-out list can outlive the widget if someone takes the
    // widget off the tree while the list is open, and then nothing chooses any more; the veil can
    // still be closed. A new list replaces the hooks of the last one. First closing, then the
    // choice: the widget that gets the row already has its list rolled up.
    const std::weak_ptr<List> theirs = list;

    picked_ = list->on<PickEvent>([this, theirs, took = std::move(took)](PickEvent&) {
        const std::shared_ptr<List> from = theirs.lock();

        if (!from) return true;

        const std::optional<std::size_t> at = from->which();

        close();

        if (at) took(*at);

        return true;
    });

    // A single click on a row chooses it, as in every drop-down list: the list was rolled out to be
    // chosen from, and the double click a plain list asks for would leave the first click doing
    // nothing visible. The row takes the click and the focus first; then it is picked like Enter.
    clicked_ = list->on<ClickEvent>([](ClickEvent& what) {
        auto* row = dynamic_cast<ListItem*>(what.target());

        if (row == nullptr || !row->enabled()) return false;

        row->pick();

        return true;
    });

    const auto frame = std::make_shared<Frame>();

    frame->children().add(list);

    const auto veil = std::make_shared<Rolled>(under_->weak_from_this());

    veil->children().add(frame);

    window->open(veil);

    rolled_ = veil;
}

}

#include "dropping.hpp"

#include <algorithm>
#include <utility>

#include "tal/layout.hpp"
#include "tal/model.hpp"
#include "tal/text.hpp"
#include "tal/views.hpp"

#include "controlling.hpp"

// The list that drops under a node, as a field's history and a selection show theirs: a modal
// window in the class "popup", in the nearest layers above the node, under it or over it. Its rows
// stand in a holder of the classes "drop" and the kind of list, which the sheet frames, beside a
// bar when there are more rows than it shows. Enter, a click or a double click takes a row and
// closes; Esc and a press beside the list close it. Its owner closes it when disabled; an owner
// that is gone, or can no longer take the focus, closes it at the first key or touch of the mouse,
// and nothing is taken.
namespace tal::dropping {

// The rows as a model of one column.
class Drop::Rows : public Model {
public:
    explicit Rows(std::vector<std::string> all) : all_(std::move(all)) {}

    // How many rows.
    [[nodiscard]] std::size_t rows() const override { return all_.size(); }

    // The row `row` as it is.
    [[nodiscard]] Entry entry(std::size_t row, std::size_t) const override {
        Entry made;

        made.text = all_.at(row);

        return made;
    }

    // The rows, to measure them.
    [[nodiscard]] const std::vector<std::string>& all() const { return all_; }

private:
    std::vector<std::string> all_;
};

// The list of the rows, which takes a row at a click.
class Drop::Entries : public List {
public:
    Entries(std::shared_ptr<Rows> rows, Drop& drop) : List(std::move(rows)), drop_(drop) {}

protected:
    // Takes the row clicked.
    void clicked(std::size_t row, int x) override {
        (void)x;

        drop_.chosen(row);
    }

private:
    Drop& drop_;
};

// The frame of the list: as wide as its owner, or as the widest row with the bar when that is
// wider, and as high as the rows it shows.
class Drop::Holder : public Row {
public:
    Holder(const Rows& rows, int owner, std::size_t shown, bool bar) : rows_(rows), owner_(owner), shown_(shown), bar_(bar) {}

protected:
    // The size the frame asks for inside its border; the room bounds it as it bounds every node.
    [[nodiscard]] Size measure(Limits room) const override {
        (void)room;

        const Sides in = controlling::boxed(style());
        int wide = 0;

        for (const std::string& one : rows_.all()) wide = std::max(wide, text::width(one, rendering()));

        wide = std::max(wide + (bar_ ? 1 : 0), owner_ - in.left - in.right);

        return Size{std::max(wide, 1), static_cast<int>(std::min(rows_.rows(), shown_))};
    }

private:
    const Rows& rows_;
    int owner_;
    std::size_t shown_;
    bool bar_;
};

// A list for `user` of `rows`, the row `current` current, showing `shown` rows at most (one at
// least) in a holder of the class `kind` too.
Drop::Drop(Node& user, std::vector<std::string> rows, std::size_t current, std::size_t shown, std::string kind)
    : Window({}, Modal::yes), owner_(user.weak_from_this()) {
    add_class("popup");

    const std::size_t most = std::max<std::size_t>(shown, 1);
    const bool bar = rows.size() > most;
    auto model = std::make_shared<Rows>(std::move(rows));
    Holder& holder = children().add<Holder>(*model, user.area().size.wide, most, bar);
    Entries& entries = holder.children().add<Entries>(model, *this);

    holder.add_class("drop");
    holder.add_class(std::move(kind));
    entries.share(Share{1, 0});
    entries.current = model->rows() == 0 ? 0 : std::min(current, model->rows() - 1);

    if (bar) (void)holder.children().add<Scroll>(entries);

    heard_.add(entries.on<Picked>([this](Picked& what) { chosen(what.row); }));

    heard_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (&what.target() != this || what.press != Press::down) return;

        what.take();
        close();
    }));

    heard_.add(on<KeyEvent>(
        [this](KeyEvent& what) {
            if (owner() != nullptr) return;

            what.take();
            close();
        },
        Phase::capture));

    heard_.add(on<MouseEvent>(
        [this](MouseEvent& what) {
            if (owner() != nullptr) return;

            what.take();
            close();
        },
        Phase::capture));
}

// Opens the list in the nearest layers above its owner, its corner under the owner's, or over the
// owner when there is no room under it; false, opening nothing, without layers or when the owner
// cannot take the focus.
bool Drop::dropped() {
    Node* one = owner();

    if (one == nullptr) return false;

    Layers* layers = nullptr;

    for (Node* at = one->parent(); at != nullptr && layers == nullptr; at = at->parent()) layers = dynamic_cast<Layers*>(at);

    if (layers == nullptr) return false;

    Spot at = one->area().at;

    for (const Node* above = one->parent(); above != nullptr; above = above->parent()) at = Spot{at.x + above->area().at.x, at.y + above->area().at.y};

    const int high = one->area().size.high;

    anchor(layers->local(Spot{at.x, at.y + high}), high);
    (void)layers->open(std::static_pointer_cast<Window>(shared_from_this()));

    return true;
}

// The node the list is for, while it is there and can take the focus; none otherwise.
Node* Drop::owner() const {
    const std::shared_ptr<Node> one = owner_.lock();

    return one != nullptr && one->focusable() ? one.get() : nullptr;
}

// Closes the list and gives its owner the row `row`; only closes when the owner is no more.
void Drop::chosen(std::size_t row) {
    const std::shared_ptr<Node> kept = shared_from_this();
    Node* one = owner();

    close();

    if (one != nullptr) taken(*one, row);
}

}

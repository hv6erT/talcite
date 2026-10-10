#include "talcite/message.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"

#include "amid.hpp"

namespace talcite::ui {

namespace {

// How many cells are left around the box so it does not stick to the screen edge. Four, because
// two on each side read as a margin, while one looks like a mistake.
constexpr int loose = 4;

// A box that disappears together with its content. A frame alone cannot do that: `Frame::wants`
// adds the edges even around a child that wants nothing, so an empty text in a box is not
// emptiness but a two-by-two outline. This node therefore asks the content, not the frame, and
// when the content wants zero rows, it wants zero too, and the column gives it not a single cell.
//
// This keeps the shape of the column fixed: the title, the text box, the gap and the answer box
// stand in it from the start and for good. `text()` and adding buttons are plain setters that
// attach, insert and rebuild nothing, and whether a box is visible follows from the sizes at the
// next frame.
//
// With `boxed` off there is no frame, but the node stays and still disappears together with its
// content, so bare messages behave exactly the same.
class Boxed : public Node {
public:
    // Puts the content into the frame, or straight into this node when there is no frame.
    Boxed(std::shared_ptr<Node> inside, const std::shared_ptr<Frame>& box)
        : inside_(std::move(inside)) {
        if (box == nullptr) {
            children().add(inside_);

            return;
        }

        box->children().add(inside_);

        children().add(box);
    }

    // Nothing when the content wants no rows; otherwise what the frame around it wants.
    [[nodiscard]] Size wants(Size room) const override {
        if (inside_->wants(room).high == 0) return {};

        return Node::wants(room);
    }

private:
    std::shared_ptr<Node> inside_;
};

// The width of the box is decided here. The dialog asks its only child how much it wants and gives
// it that much, so if it asked the frame directly, a message with one long sentence would stretch
// across the whole screen. This node narrows the question, not the answer: the text gets less room
// and so wraps instead of being cut. A short message still wants exactly as much as its characters.
class Capped : public Node {
public:
    // What the content wants when asked with the room narrowed by the margin.
    [[nodiscard]] Size wants(Size room) const override {
        if (children().empty()) return {};

        // On a screen narrower than two margins there is no breathing room: better to show the
        // content tightly than a nice gap around a single column.
        const int wide = room.wide > loose * 2 ? room.wide - loose : room.wide;

        return children().front()->wants({std::max(wide, 1), room.high});
    }
};

// One cell of breathing room on each side of the content, in a message without boxes. A box gives
// it with its edge; without one the text would touch the window frame. It asks the content with
// room two cells narrower and wants what the content wants plus those two, at the content's
// height, so empty content is zero rows and `Gap` sees emptiness.
class Aired : public Node {
public:
    // Holds the given content as its only child.
    explicit Aired(std::shared_ptr<Node> inside) { children().add(std::move(inside)); }

    // The content's wish for room two cells narrower, widened by those two cells.
    [[nodiscard]] Size wants(Size room) const override {
        const Size inside = children().front()->wants({std::max(room.wide - 2, 0), room.high});

        // This exceeds the room only on a screen narrower than the breathing room itself, and
        // there the window frame cuts it.
        return {inside.wide + 2, inside.high};
    }

protected:
    // Paints the content one cell in from the left, transparent elsewhere.
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.clear({{0, 0}, seen.size});

        const Area where{{1, 0}, {std::max(given.wide - 2, 0), given.high}};
        const Area part = Area::crossed(seen, where);

        made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

        return made;
    }
};

}

// A message box with a title, a text and buttons. A message paints nothing of its own. It is an
// assembly of parts that already exist: the frame gives the background and the breathing room,
// the column lays out, the text speaks, the row of buttons asks. All its code is building the
// tree, handing out the roles and passing the answer on.
//
// The layout, from the outside:
//
//     Capped → Frame(stem)           ← background and outline; its edge is the margin
//              └── Column
//                  ├── Span(title)   ← centered, role <stem>.title
//                  ├── Boxed         ← the text box; disappears when there is no text
//                  │   └── Frame(.box) → Span(text)
//                  ├── Gap           ← an empty row; disappears when either box is missing
//                  └── Boxed         ← the answer box; disappears when there are no buttons
//                      └── Frame(.box) → Amid → Row(spacing 1) → buttons
//
// The column is assembled once and never changes. Everything that is or is not visible follows
// from `wants`, so `text()` and adding buttons may be called at any time, also on a window that
// is already open, and nothing has to be rearranged.
//
// Without boxes (`boxed` off) the message looks like a form: the title stands on the top edge of
// the frame, the text has a cell of breathing room on both sides, and the buttons stand centered
// under an empty row:
//
//     Capped → Frame(title, stem)
//              └── Titled          ← no narrower than the title, as in a form
//                  └── Column
//                      ├── Aired → Span(text)
//                      ├── Gap
//                      └── Amid → Row(spacing 1) → buttons
//
// The edge of the outer frame does double duty: it is drawn with the role <stem>, so the skin
// decides whether a line shows in it or only the background color. In Midnight Commander the
// error box has it invisible, and it looks like a column of breathing room, which is exactly the
// same cell. The curtain takes only the shadow from the message's shape.
Message::Message(std::string title, std::string text, MessageShape shape)
    : Dialog(ModalShape{shape.shade}), shape_(shape) {
    const auto capped = std::make_shared<Capped>();

    frame_ = std::make_shared<Frame>(std::string{}, FrameShape{shape_.border});

    const auto column = std::make_shared<Column>();

    title_ = std::make_shared<Span>(std::move(title),
                                    SpanShape{false, Justify::center, Elide::right});

    text_ = std::make_shared<Span>(std::move(text),
                                   SpanShape{true, Justify::center, Elide::none});

    // The spacing between buttons belongs to the row, not to the content, so a button shrunk to
    // zero leaves no hole behind.
    const auto amid = std::make_shared<Amid>();

    amid->children().add(row_);

    // A box around the text and around the buttons: that is how a message looks in Midnight
    // Commander. Whoever prefers bare content turns `boxed` off and gets just the column in a
    // frame.
    const auto boxing = [this]() -> std::shared_ptr<Frame> {
        if (!shape_.boxed) return nullptr;

        auto made = std::make_shared<Frame>(std::string{}, FrameShape{Border::thin});

        boxes_.push_back(made);

        return made;
    };

    if (!shape_.boxed) {
        frame_->title(title_->text());

        const auto said = std::make_shared<Aired>(text_);
        const auto titled = std::make_shared<Titled>(*frame_);

        column->children().add(said);
        column->children().add(std::make_shared<Gap>(*said, *amid));
        column->children().add(amid);

        titled->children().add(column);
        frame_->children().add(titled);
        capped->children().add(frame_);

        children().add(capped);

        role("talcite.message");

        return;
    }

    const auto said = std::make_shared<Boxed>(text_, boxing());
    const auto asks = std::make_shared<Boxed>(amid, boxing());

    column->children().add(title_);
    column->children().add(said);
    column->children().add(std::make_shared<Gap>(*said, *asks));
    column->children().add(asks);

    frame_->children().add(column);
    capped->children().add(frame_);

    children().add(capped);

    // The stem is set last, once the parts are in place, and the setter hands it to all of them.
    role("talcite.message");
}

// The message's title.
const std::string& Message::title() const { return title_->text(); }

// Changes the title; without boxes it also stands on the frame's top edge.
void Message::title(std::string given) {
    title_->text(std::move(given));

    if (!shape_.boxed) frame_->title(title_->text());
}

// The message's text.
const std::string& Message::text() const { return text_->text(); }

// Changes the text; the text box appears or disappears with it at the next frame.
void Message::text(std::string given) { text_->text(std::move(given)); }

// Sets the role stem and hands it out to the parts, after building and after every change,
// because the children keep it themselves. The family has five members: the window itself, the
// title, the boxes inside, the text and the buttons. The skin thus describes one message with five
// entries and does not have to guess anything.
void Message::role(std::string given) {
    Node::role(std::move(given));

    frame_->role(role());
    title_->role(role() + ".title");
    text_->role(role() + ".text");

    for (const std::shared_ptr<Frame>& one : boxes_) one->role(role() + ".box");

    buttons_.role(role() + ".button");
}

}

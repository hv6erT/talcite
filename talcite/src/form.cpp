#include "talcite/form.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/row.hpp"

#include "amid.hpp"

namespace talcite::ui {

namespace {

// Buttons first. The button row and the gap above it are taken out of the room before the content
// is asked, both when asked for the size and when painting. Asking the content first would give it
// the room including the place for the buttons when sizing, but only what it wanted earlier when
// painting. Content that is meant to fill the height (a scrolled page in a box asking "as much as I
// want, but leave room for what is below me") would then get two different rooms, give two
// different answers, and leave empty rows below it.
//
// Children: content, gap, buttons, in this order, as in a column; only the room is computed from
// the bottom.
class Footed : public Node {
public:
    // Content, gap and button row.
    [[nodiscard]] int capacity() const override { return 3; }

    // The content's wish within what the buttons leave, plus the footing; as wide as the wider of
    // the two.
    [[nodiscard]] Size wants(Size room) const override {
        const Size foot = footing(room);
        const Size body = children()[0]->wants({room.wide, std::max(room.high - foot.high, 0)});

        return {std::min(std::max(body.wide, foot.wide), room.wide), std::min(body.high + foot.high, room.high)};
    }

protected:
    // Stacks content, gap and buttons, giving the content only what remains after the footing.
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.clear({{0, 0}, seen.size});

        const Size foot = footing(given);
        const int left = std::max(given.high - foot.high, 0);
        const int body = std::min(children()[0]->wants({given.wide, left}).high, left);
        const int gap = children()[1]->wants({given.wide, given.high}).high;

        int y = 0;

        for (const auto& [at, high] : {std::pair{0, body}, std::pair{1, std::min(gap, given.high - body)},
                                       std::pair{2, std::max(given.high - body - gap, 0)}}) {
            const Area where{{0, y}, {given.wide, high}};
            const Area part = Area::crossed(seen, where);

            made.draw(children()[static_cast<std::size_t>(at)]->render(where, part - where.at), part.at - seen.at);

            y += high;
        }

        return made;
    }

private:
    // The gap and the buttons together: the height of both and the width of the row.
    [[nodiscard]] Size footing(Size room) const {
        const int gap = children()[1]->wants(room).high;
        const Size row = children()[2]->wants({room.wide, std::max(room.high - gap, 0)});

        return {row.wide, std::min(gap + row.high, room.high)};
    }
};

}

// A form is a sibling of the message box: a title, content and a row of buttons, except that the
// content is not text but a slot where the caller puts fields, switches and lists. This is what the
// sort, filter, appearance or directory name dialogs in Midnight Commander look like.
//
// Layout:
//
//     Frame(title, border)
//     └── Titled                ← not narrower than the title
//         └── Footed            ← room counted from the bottom: buttons first, the rest to content
//             ├── Column(body)  ← the caller's slot
//             ├── Gap           ← an empty row; vanishes when there is no content or no buttons
//             └── Amid → Row(1) ← buttons centered, as in the message box
//
// No roles of its own. The frame draws as a frame and a button as a button, so a skin that knows
// `talcite.frame` and `talcite.button` already knows the whole form. The message box has a family
// of roles because it has variants (error, question); a form has no variants and does not pretend
// to.
//
// Escape does not close it: talcite leaves that to the caller, together with the default button
// (described at `Modal`), and the form does not reverse that decision.
Form::Form(std::string title, FormShape shape) : Dialog(ModalShape{shape.shade}), shape_(shape) {
    frame_ = std::make_shared<Frame>(std::move(title), FrameShape{shape_.border});
    body_ = std::make_shared<Column>();
    const auto column = std::make_shared<Footed>();
    const auto amid = std::make_shared<Amid>();

    amid->children().add(row_);

    column->children().add(body_);
    column->children().add(std::make_shared<Gap>(*body_, *row_));
    column->children().add(amid);

    const auto titled = std::make_shared<Titled>(*frame_);

    titled->children().add(column);
    frame_->children().add(titled);

    children().add(frame_);
}

// The title shown in the frame.
const std::string& Form::title() const { return frame_->title(); }

// Replaces the title shown in the frame.
void Form::title(std::string given) { frame_->title(std::move(given)); }

}

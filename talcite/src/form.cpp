#include "talcite/form.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"

namespace talcite::ui {

namespace {

// Content above, a footer below, and the room counted from the bottom: the footer first. The
// footer and the gap above it are taken out of the room before the content is asked, both when
// asked for the size and when painting. Asking the content first would give it the room including
// the place for the footer when sizing, but only what it wanted earlier when painting. Content that
// is meant to fill the height (a scrolled page in a box asking "as much as I want, but leave room
// for what is below me") would then get two different rooms, give two different answers, and leave
// empty rows below it.
//
// The gap stands only between two parts that both take some height, the rule of `Column`: a form
// without content or without anything in its footer has no empty row to separate them.
class Footed : public Node {
public:
    // Footed parts `gap` rows apart; a negative gap counts as zero.
    explicit Footed(int gap) : gap_(std::max(gap, 0)) {}

    // Content and footer.
    [[nodiscard]] int capacity() const override { return 2; }

    // The content's wish within what the footer leaves, plus the gap and the footer; as wide as the
    // wider of the two, within the room.
    [[nodiscard]] Size wants(Size room) const override {
        const Laid parts = laid(room);

        return {std::min(std::max(parts.body.wide, parts.foot.wide), room.wide),
                std::min(parts.body.high + parts.apart + parts.foot.high, room.high)};
    }

protected:
    // Stacks content, gap and footer, giving the content only what remains after the footer and
    // the footer what remains after the content.
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.clear({{0, 0}, seen.size});

        if (children().many() < 2) return made;

        const Laid parts = laid(given);
        const int left = std::max(given.high - parts.apart - parts.foot.high, 0);
        const int body = std::min(parts.body.high, left);
        const int apart = std::min(parts.apart, std::max(given.high - body, 0));

        const Area places[] = {{{0, 0}, {given.wide, body}},
                               {{0, body + apart}, {given.wide, std::max(given.high - body - apart, 0)}}};

        for (std::size_t which = 0; which < 2; ++which) {
            const Area part = Area::crossed(seen, places[which]);

            made.draw(children()[which]->render(places[which], part - places[which].at), part.at - seen.at);
        }

        return made;
    }

private:
    // The sizes of both parts and of the gap between them, for the room `room`.
    struct Laid {
        Size body;
        Size foot;
        int apart = 0;
    };

    // Asks the footer first and the content with what is left. Whether the gap shows is asked of
    // both with the whole room, before either is cut down by the other.
    [[nodiscard]] Laid laid(Size room) const {
        if (children().many() < 2) return {};

        const Node& body = *children()[0];
        const Node& foot = *children()[1];

        Laid made;

        if (body.wants(room).high > 0 && foot.wants(room).high > 0) made.apart = std::min(gap_, std::max(room.high, 0));

        made.foot = foot.wants({room.wide, std::max(room.high - made.apart, 0)});
        made.foot.high = std::min(made.foot.high, std::max(room.high - made.apart, 0));
        made.body = body.wants({room.wide, std::max(room.high - made.apart - made.foot.high, 0)});

        return made;
    }

    int gap_ = 0;
};

}

// A form is a sibling of the message box: a title, content and a row of buttons, except that the
// content is not text but a slot where the caller puts fields, switches and lists. This is what the
// sort, filter, appearance or directory name dialogs in Midnight Commander look like.
//
// Layout:
//
//     Frame(title, border)     ← no narrower than the title, which the frame measures
//     └── Footed(1)            ← room from the bottom: the footer first, the rest to the content
//         ├── Column(body)     ← the caller's slot
//         └── Column(footer)   ← the caller's slot under the content, an empty row above it
//             └── Flow(1)      ← buttons in centered lines, as in the message box
//
// The footer holds the buttons, and anything a window wants to stand under its content with the
// same priority: a window that says something beside its buttons (how many entries pass a filter)
// builds that row itself and puts it in the footer, where the content cannot push it out of the
// window. The buttons stay a `Flow` of their own, so `buttons()` works in every form the same.
//
// No roles of its own. The frame draws as a frame and a button as a button, so a skin that knows
// `talcite.frame` and `talcite.button` already knows the whole form. The message box has a family
// of roles because it has variants (error, question); a form has no variants and does not pretend
// to.
//
// Escape does not close it: talcite leaves that to the caller, together with the default button
// (described at `Modal`), and the form does not reverse that decision.
Form::Form(std::string title, FormShape shape) : Dialog(ModalShape{shape.shade}) {
    frame_ = std::make_shared<Frame>(std::move(title), FrameShape{shape.border});
    body_ = std::make_shared<Column>();
    footer_ = std::make_shared<Column>();

    const auto parts = std::make_shared<Footed>(1);

    footer_->children().add(row_);

    parts->children().add(body_);
    parts->children().add(footer_);

    frame_->children().add(parts);

    children().add(frame_);
}

// The title shown in the frame.
const std::string& Form::title() const { return frame_->title(); }

// Replaces the title shown in the frame.
void Form::title(std::string given) { frame_->title(std::move(given)); }

}

#include "talcite/form.hpp"

#include <memory>
#include <utility>

#include "talcite/column.hpp"
#include "talcite/footed.hpp"
#include "talcite/frame.hpp"

namespace talcite::ui {

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

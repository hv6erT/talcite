#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/controls.hpp"
#include "tal/docs.hpp"
#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/layout.hpp"
#include "tal/leaves.hpp"
#include "tal/pages.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

// What a viewer shows of a document (E6f): its pages, when an engine renders them, and otherwise
// its text version under one line that says why there are no pages. The text version is a Pager
// of `written` text, its links marks that tell `Followed` when chosen; the pages of a PDF are
// read one by one and added to the text as they come, so that the first shows at once and a long
// document never waits whole. A page that cannot be read leaves the sentence of its trouble in
// its place, and the pages after it are read still.
namespace tal {

namespace {

// The text version shown under the line `why`: a column of a wrapped label (class "reason") and
// a pager that takes the rest of the room, in the class "transcript". A link the pager follows
// is told by the transcript too (`Followed`), since the pager is its own.
class Transcript final : public Column {
public:
    // A transcript, empty, under `why`.
    explicit Transcript(std::string why) {
        add_class("transcript");

        Label& said = children().add<Label>(std::move(why));

        said.wrap = true;
        said.add_class("reason");

        pager_ = &children().add<Pager>(text_);
        pager_->share(Share{1, 0});
        followed_ = pager_->on<Followed>([this](Followed& what) {
            Followed again{what.address};

            notified(again);
        });
    }

    // `plain` added after what the transcript holds, an empty line between, its links marked.
    void added(const docs::Plain& plain) {
        Written made = written(plain);

        if (made.text.empty()) return;

        const std::size_t last = text_->lines() - 1;
        const bool first = last == 0 && text_->line(0).empty();
        const std::size_t base = first ? 0 : last + 2;

        text_->insert(Place{last, text_->line(last).size()}, first ? made.text : "\n\n" + made.text);

        for (Stretch& one : made.marks) {
            one.from.line += base;
            one.upto.line += base;
            marks_.push_back(std::move(one));
        }

        pager_->marks(marks_);
    }

    // The pages of `prose` read in order on the program's loop and added, each after the line of
    // its number; a page that cannot be read is the sentence of its trouble.
    void reading(std::shared_ptr<docs::Prose> prose) {
        scope_.emplace();
        scope_->run(read(std::move(prose)));
    }

private:
    // The work of `reading`.
    Task<void> read(std::shared_ptr<docs::Prose> prose) {
        for (std::size_t which = 0; which < prose->pages(); ++which) {
            std::optional<Trouble> failed;
            docs::Plain page;

            try {
                page = co_await prose->page(which);
            } catch (const Trouble& why) {
                failed = why;
            }

            if (failed) {
                docs::Block number;
                docs::Block said;

                number.kind = docs::Block::Kind::page;
                number.text = std::to_string(which + 1);
                said.text = failed->what();
                page.blocks = {std::move(number), std::move(said)};
            }

            added(page);
        }
    }

    std::shared_ptr<Text> text_ = std::make_shared<Text>();
    Pager* pager_ = nullptr;
    Hook followed_;
    std::vector<Stretch> marks_;
    std::optional<Scope> scope_;
};

}

// The text version `plain` under one line `why` saying why it is shown instead of the thing
// itself: a column of a wrapped label and a pager of the text written, its links marked.
std::shared_ptr<Node> transcript(const docs::Plain& plain, std::string why) {
    auto made = std::make_shared<Transcript>(std::move(why));

    made->added(plain);

    return made;
}

// The document read through `from`: its pages in `Leaves` when `renderer` opens it with
// `password`, and when it refuses for want of an engine (`Trouble` unsupported), the text version
// of a PDF under the sentence of that refusal, its pages read on the program's loop as long as
// the node lives. Throws `Trouble`: what the renderer throws but unsupported; when the text
// version cannot be had either (not a PDF, encrypted), the kind of that with both sentences.
Task<std::shared_ptr<Node>> previewed(std::shared_ptr<docs::Renderer> renderer, std::shared_ptr<meta::Reader> from, std::string password) {
    std::optional<Trouble> refused;
    std::shared_ptr<docs::Document> document;

    try {
        document = co_await renderer->opened(from, std::move(password));
    } catch (const Trouble& why) {
        if (why.kind() != Kind::unsupported) throw;

        refused = why;
    }

    if (document) co_return std::make_shared<Leaves>(std::move(document));

    std::optional<Trouble> unread;
    std::shared_ptr<docs::Prose> prose;

    try {
        prose = co_await docs::prose(std::move(from));
    } catch (const Trouble& why) {
        unread = why;
    }

    if (unread) throw Trouble{unread->kind(), std::string{refused->what()} + " " + unread->what()};

    auto made = std::make_shared<Transcript>(refused->what());

    made->reading(std::move(prose));

    co_return made;
}

}

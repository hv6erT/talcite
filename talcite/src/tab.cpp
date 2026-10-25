#include "talcite/tab.hpp"

#include <utility>

#include "talcite/tabs.hpp"

namespace talcite::ui {

// A tab with the name `text`; its page is its only child. The strip with the names belongs to the
// `Tabs` above it, which also says whose page is in the tree: a hidden tab keeps its page aside
// (`shown`), so the page's buttons do not answer their keys and the focus does not walk into it.
Tab::Tab(std::string text) : text_(std::move(text)) {}

// One child: the page.
int Tab::capacity() const { return 1; }

// What the page wants, whether it is in the tree or kept aside, so the tabs keep one size whichever
// is shown.
Size Tab::wants(Size room) const {
    if (!children().empty()) return children().front()->wants(room);

    return page_ ? page_->wants(room) : Size{0, 0};
}

// The page over all the room.
View Tab::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    Node& page = *children().front();
    const Area part = Area::crossed(seen, {{0, 0}, given});

    made.draw(page.render({{0, 0}, given}, part), part.at - seen.at);

    return made;
}

// The tab's key shows its page, as a click on its name does.
void Tab::activate() {
    auto* tabs = dynamic_cast<Tabs*>(parent());

    if (tabs == nullptr) return;

    std::size_t at = 0;

    for (const std::shared_ptr<Node>& one : tabs->children()) {
        if (one.get() == this) return tabs->which(at);

        ++at;
    }
}

// Puts the page into the tree, or keeps it aside; nothing changes when it already is where asked.
void Tab::shown(bool on) {
    if (on && children().empty() && page_) {
        children().add(std::exchange(page_, nullptr));

        return;
    }

    if (!on && !children().empty()) page_ = children().remove(children().front().get());
}

}

#include "tal/menu.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/document.hpp"
#include "tal/key.hpp"
#include "tal/style.hpp"
#include "tal/text.hpp"
#include "tal/window.hpp"

#include "controlling.hpp"
#include "lettering.hpp"
#include "tree.hpp"

namespace tal {

using controlling::boxed;
using controlling::clicking;
using controlling::lined;
using controlling::pressing;
using controlling::spelled;

namespace {

// Where `one` stands on the screen: its area moved by the corners of the nodes above it.
Area screened(const Node& one) {
    Spot at = one.area().at;

    for (const Node* above = one.parent(); above != nullptr; above = above->parent()) at = Spot{at.x + above->area().at.x, at.y + above->area().at.y};

    return Area{at, one.area().size};
}

// The nearest layers above `one`, or none.
Layers* layered(Node& one) {
    for (Node* at = one.parent(); at != nullptr; at = at->parent())
        if (auto* found = dynamic_cast<Layers*>(at)) return found;

    return nullptr;
}

// Whether `key` is held with neither Ctrl nor Alt.
bool bare(Key key) { return !key.mods().ctrl && !key.mods().alt; }

// Whether a cluster of `said` is the letter `letter`.
bool holding(std::string_view said, std::optional<char32_t> letter, text::Rendering how) {
    const std::vector<std::string_view> clusters = text::clusters(said, how);

    return std::any_of(clusters.begin(), clusters.end(), [letter](std::string_view one) { return controlling::lettered(one, letter); });
}

// `said` in at most `wide` columns: whole when it fits, otherwise its start and `dots` after it.
// Narrower than `dots` it is `dots` alone, which the canvas clips.
std::string cut(std::string_view said, int wide, std::string_view dots, text::Rendering how) {
    if (text::width(said, how) <= wide) return std::string{said};

    const int room = wide - text::width(dots, how);
    std::string made;
    int taken = 0;

    for (const std::string_view one : text::clusters(said, how)) {
        const int needs = text::width(one, how);

        if (taken + needs > room) break;

        made += one;
        taken += needs;
    }

    return made + std::string{dots};
}

}

// A position of a menu, in the class "menu": an action (its text, letter, shortcut and whether it
// is enabled follow the action), or the title of a submenu, marked on the right with the glyph
// `menu.more`. The mouse over it gives it the focus, as in Midnight Commander, and a click picks
// it.
class Menu::Item : public Node {
public:
    Item(std::shared_ptr<Action> one, std::string text, std::optional<char32_t> letter, std::shared_ptr<Menu> sub);

    // The text shown: the action's or the submenu's.
    [[nodiscard]] const std::string& shown() const { return action != nullptr ? action->text() : said; }

    // The letter that picks it: the action's or the submenu's.
    [[nodiscard]] std::optional<char32_t> letter() const { return action != nullptr ? action->letter() : marked; }

    std::shared_ptr<Action> action;
    std::string said;
    std::optional<char32_t> marked;
    std::shared_ptr<Menu> below;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    [[nodiscard]] std::string hint() const;

    Hooks hooks_;
};

// A row of a menu holding a line across, in the kind of the menu's frame and the look of its part
// "frame"; the menu draws the joints on its frame and gives it its row.
class Menu::Separator : public Node {
protected:
    void paint(Canvas& onto) const override;
};

// The position for `one` (an action) or, without one, for the submenu `sub` shown as `text` with
// `letter`.
Menu::Item::Item(std::shared_ptr<Action> one, std::string text, std::optional<char32_t> letter, std::shared_ptr<Menu> sub)
    : action(std::move(one)), said(std::move(text)), marked(letter), below(std::move(sub)) {
    classes({"menu"});
    focusable(true);

    if (action != nullptr) {
        enable(action->enabled());

        hooks_.add(action->on<Altered>([this](Altered& what) {
            enable(what.action.enabled());
            relayout();
        }));
    }

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press == Press::move) {
            if (!focused()) focus();

            return;
        }

        if (!clicking(what)) return;

        what.take();

        if (auto* menu = dynamic_cast<Menu*>(parent())) menu->picked(*this);
    }));
}

// What stands on the right: the shortcut of the action, or the glyph `menu.more` of a submenu.
std::string Menu::Item::hint() const {
    if (below != nullptr) return std::string{glyph("menu.more")};

    return action->key() ? to_string(*action->key()) : std::string{};
}

// The text between a column on each side, and the hint two columns further.
Size Menu::Item::measure(Limits room) const {
    (void)room;

    const std::string aside = hint();
    const int wide = text::width(shown(), rendering()) + 2;

    return Size{aside.empty() ? wide : wide + 2 + text::width(aside, rendering()), 1};
}

// The text one column in, its letter in the look of the part "key" (none when disabled, since it
// picks nothing), and the hint against the right edge, a column in.
void Menu::Item::paint(Canvas& onto) const {
    const Look plain = look();
    const std::string aside = hint();

    (void)spelled(onto, {1, 0}, shown(), enabled() ? letter() : std::nullopt, plain, look("key"), rendering());

    if (!aside.empty()) (void)onto.put({onto.size().wide - 1 - text::width(aside, rendering()), 0}, aside, plain);
}

// The line across the whole row.
void Menu::Separator::paint(Canvas& onto) const {
    const auto* menu = dynamic_cast<const Menu*>(parent());
    const Border kind = menu != nullptr ? menu->style().border : Border::none;
    const std::string_view across = glyph("line." + std::string{lined(kind)} + ".across");
    const Look edge = menu != nullptr ? menu->look("frame") : look();

    for (int x = 0; x < onto.size().wide; ++x) (void)onto.put({x, 0}, across, edge);
}

// An empty menu, in the classes "menu" and "box": the sheet gives the box its frame. The box is
// only the ground under its positions and takes no hover, so the sheet's hover look stays on
// them. The arrows go round the enabled positions, Home and End go to the first and the last,
// Enter, the space bar or a position's letter pick it, and the right arrow opens a submenu; these
// are default actions, so the program may take the key before them.
Menu::Menu() {
    classes({"menu", "box"});
    hoverable(false);

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Key key = what.key;
        auto* item = dynamic_cast<Item*>(&what.target());

        if (key.mods() == Mods{}) {
            const Code code = key.code();

            if (code == Code::up || code == Code::down || code == Code::home || code == Code::end) {
                what.take();
                stepped(item, code == Code::down || code == Code::end ? 1 : -1, code == Code::home || code == Code::end);

                return;
            }

            if (code == Code::right && item != nullptr && item->below != nullptr) {
                what.take();
                picked(*item);

                return;
            }
        }

        if (item != nullptr && pressing(key)) {
            what.take();
            picked(*item);

            return;
        }

        if (key.letter() == 0 || !bare(key)) return;

        for (Node& one : children())
            if (auto* found = dynamic_cast<Item*>(&one); found != nullptr && found->focusable() && lettering::same(key.letter(), found->letter())) {
                what.take();
                found->focus();
                picked(*found);

                return;
            }
    }));
}

// Adds a position for `one` at the end.
void Menu::add(std::shared_ptr<Action> one) {
    if (one == nullptr) throw std::invalid_argument{"tal::Menu::add: no action"};

    (void)children().add<Item>(std::move(one), std::string{}, std::nullopt, nullptr);
}

// Adds a position shown as `text` with `letter` that opens a submenu beside it, and gives the
// submenu, empty, to be filled.
Menu& Menu::sub(std::string text, std::optional<char32_t> letter) {
    auto made = std::make_shared<Menu>();

    made->bar_ = bar_;

    (void)children().add<Item>(nullptr, std::move(text), letter, made);

    return *made;
}

// Adds a line between the positions above and below.
void Menu::separator() { (void)children().add<Separator>(); }

// As wide as its widest row and a row for each position and line.
Size Menu::measure(Limits room) const {
    int wide = 0;
    int high = 0;

    for (Node& one : children()) {
        wide = std::max(wide, one.measured(Limits{{0, 0}, room.most}).wide);
        ++high;
    }

    return Size{wide, high};
}

// The rows one under another, each across the whole inside; those below its end get no room.
void Menu::arrange(Area inside) {
    int y = inside.at.y;

    for (Node& one : children()) {
        const bool room = y < inside.at.y + inside.size.high;

        one.placed(Area{{inside.at.x, y}, {inside.size.wide, room ? 1 : 0}});

        if (room) ++y;
    }
}

// The frame again in the look of the part "frame", and the joints of its lines on it.
void Menu::outline(Canvas& box) const {
    if (style().border == Border::none) return;

    const std::string stem = "line." + std::string{lined(style().border)} + ".";
    const auto line = [this, &stem](std::string_view rest) { return glyph(stem + std::string{rest}); };
    const Look edge = look("frame");
    const int wide = box.size().wide;

    box.frame(Area{{0, 0}, box.size()}, Lines{line("upper.left"), line("upper.right"), line("lower.left"), line("lower.right"),
                                             line("across"), line("upright"), line("left.joint"), line("right.joint")},
              edge);

    for (Node& one : children()) {
        if (dynamic_cast<Separator*>(&one) == nullptr || one.area().size.high == 0) continue;

        (void)box.put({0, one.area().at.y}, line("left.joint"), edge);
        (void)box.put({wide - 1, one.area().at.y}, line("right.joint"), edge);
    }
}

// Picks `one`: a submenu opens beside it; an action closes every open menu of the bar first and
// then runs, so that a window it opens stands over the screen, not under a menu.
void Menu::picked(Item& one) {
    if (one.below != nullptr) {
        if (bar_ != nullptr) bar_->dropped(*one.below, one);

        return;
    }

    const std::shared_ptr<Action> action = one.action;

    if (bar_ != nullptr) bar_->close();

    action->run();
}

// Gives the focus to the enabled position `by` (1 or -1) away from `from`, round the ends, or with
// `ends` to the last (1) or the first (-1).
void Menu::stepped(Item* from, int by, bool ends) {
    std::vector<Item*> items;

    for (Node& one : children())
        if (auto* item = dynamic_cast<Item*>(&one); item != nullptr && item->focusable()) items.push_back(item);

    if (items.empty()) return;

    const std::size_t many = items.size();

    if (ends) {
        items[by > 0 ? many - 1 : 0]->focus();

        return;
    }

    const auto here = static_cast<std::size_t>(std::find(items.begin(), items.end(), from) - items.begin());

    items[by > 0 ? (here + 1) % many : (here + many - 1) % many]->focus();
}

// The title of a menu in the bar, in the class "menubar": the text between two spaces, its letter
// in the look of the part "key". While its menu is open it is lit with the state `focus`, though
// the focus is in the menu. A press opens its menu.
class MenuBar::Title : public Node {
public:
    Title(std::string text, std::optional<char32_t> letter);

    // Lights the title, or puts it out.
    void lit(bool on) { state(State::focus, on); }

    std::string said;
    std::optional<char32_t> marked;
    std::shared_ptr<Menu> menu;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    Hooks hooks_;
};

// The window an open menu stands in: modal, in the class "popup" too, which the sheet leaves
// without a frame but with the shadow. A press of the mouse outside the menu comes to it, as to a
// curtain, and goes to the bar; the side arrows the menu leaves step the bar or, in a submenu, the
// left one closes it. Closed, it tells the bar and is gone, since only its layers held it, and its
// menu leaves it then.
class MenuBar::Drop : public Window {
public:
    explicit Drop(MenuBar& owner);

    MenuBar& bar;

private:
    Hooks heard_;
};

// A title shown as `text` with `letter`.
MenuBar::Title::Title(std::string text, std::optional<char32_t> letter) : said(std::move(text)), marked(letter) {
    classes({"menubar"});

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press != Press::down || what.button != MouseButton::left) return;

        auto* bar = dynamic_cast<MenuBar*>(parent());

        if (bar == nullptr) return;

        const std::vector<Title*> all = bar->titles();
        const auto at = std::find(all.begin(), all.end(), this);

        what.take();
        bar->open(static_cast<std::size_t>(at - all.begin()));
    }));
}

// The text between two spaces.
Size MenuBar::Title::measure(Limits room) const {
    (void)room;

    return Size{text::width(said, rendering()) + 2, 1};
}

// The text one column in, its letter in the look of the part "key".
void MenuBar::Title::paint(Canvas& onto) const { (void)spelled(onto, {1, 0}, said, marked, look(), look("key"), rendering()); }

// A drop for `owner`'s menus.
MenuBar::Drop::Drop(MenuBar& owner) : Window({}, Modal::yes), bar(owner) {
    add_class("popup");

    heard_.add(on<Closed>([this](Closed&) { bar.left(*this); }));

    heard_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (&what.target() != this || what.press != Press::down) return;

        what.take();
        bar.outside(what.screen, *this);
    }));

    heard_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Code code = what.key.code();

        if (what.key.mods() != Mods{} || (code != Code::left && code != Code::right)) return;

        what.take();

        if (code == Code::left && bar.drops_.front().lock().get() != this) {
            close();

            return;
        }

        bar.stepped(code == Code::right ? 1 : -1);
    }));
}

// An empty bar, in the class "menubar". The bar is only the ground under its titles and takes no
// hover, so the sheet's hover look stays on them.
MenuBar::MenuBar() {
    classes({"menubar"});
    hoverable(false);
}

// Closes the menus still open, so that none outlives the bar that runs them.
MenuBar::~MenuBar() { shut(0); }

// Adds a title shown as `text` with `letter` at the end, and gives its menu, empty, to be filled.
Menu& MenuBar::add(std::string text, std::optional<char32_t> letter) {
    Title& made = children().add<Title>(std::move(text), letter);

    made.menu = std::make_shared<Menu>();
    made.menu->bar_ = this;

    return *made.menu;
}

// Opens the menu `which` (counted from 0) under its title in the nearest layers above the bar,
// lights the title and closes any menu open before. A menu past the last throws std::out_of_range,
// a bar outside layers std::logic_error.
void MenuBar::open(std::size_t which) {
    const std::vector<Title*> all = titles();

    if (which >= all.size()) throw std::out_of_range{"tal::MenuBar::open: no such menu"};

    Layers* layers = layered(*this);

    if (layers == nullptr) throw std::logic_error{"tal::MenuBar::open: the bar is not in layers"};

    shut(0);

    Title& title = *all[which];
    const Area at = screened(title);
    auto drop = std::make_shared<Drop>(*this);

    (void)drop->children().add(title.menu);
    drop->anchor(layers->local(Spot{at.at.x, at.at.y + 1}));

    root_ = which;
    drops_.push_back(drop);

    for (Title* one : all) one->lit(one == &title);

    (void)layers->open(drop);
}

// Closes every open menu, the submenus first; the focus goes back where it was before.
void MenuBar::close() { shut(0); }

// Whether a menu of the bar is open.
bool MenuBar::opened() const {
    return std::any_of(drops_.begin(), drops_.end(), [](const std::weak_ptr<Drop>& held) { return !held.expired(); });
}

// A row of the titles as wide as they are.
Size MenuBar::measure(Limits room) const {
    int wide = 0;

    for (Node& one : children()) wide += one.measured(Limits{{0, 0}, room.most}).wide;

    return Size{wide, 1};
}

// The titles from the left, each as wide as it wants while there is room.
void MenuBar::arrange(Area inside) {
    int x = inside.at.x;
    const int end = inside.at.x + inside.size.wide;

    for (Node& one : children()) {
        const int wide = std::min(one.measured(Limits{{0, 0}, inside.size}).wide, end - x);

        one.placed(Area{{x, inside.at.y}, {wide, std::min(inside.size.high, 1)}});
        x += wide;
    }
}

// The titles of the bar, in order.
std::vector<MenuBar::Title*> MenuBar::titles() const {
    std::vector<Title*> made;

    for (Node& one : children())
        if (auto* title = dynamic_cast<Title*>(&one)) made.push_back(title);

    return made;
}

// Opens the submenu `one` beside the position `beside` of the menu open on top, in the same layers.
// A submenu picked while no menu of the bar is open (its menu put elsewhere by hand) opens nothing.
void MenuBar::dropped(Menu& one, const Node& beside) {
    const std::shared_ptr<Drop> top = drops_.empty() ? nullptr : drops_.back().lock();
    auto* layers = top != nullptr ? dynamic_cast<Layers*>(top->parent()) : nullptr;

    if (layers == nullptr) return;

    const Area box = screened(*beside.parent());
    auto drop = std::make_shared<Drop>(*this);

    (void)drop->children().add(one.shared_from_this());
    drop->anchor(layers->local(Spot{box.at.x + box.size.wide, screened(beside).at.y - 1}));

    drops_.push_back(drop);

    (void)layers->open(drop);
}

// Closes the open drops from the top down to the one at `from`; each, closed, takes itself off
// (`left`).
void MenuBar::shut(std::size_t from) {
    while (drops_.size() > from) {
        const std::shared_ptr<Drop> top = drops_.back().lock();

        if (top == nullptr) {
            drops_.pop_back();

            continue;
        }

        top->close();
    }
}

// The drop `one` has closed, always the one on top, since only it hears keys and the mouse: it
// leaves the stack, and when none is left the title goes out.
void MenuBar::left(Drop& one) {
    std::erase_if(drops_, [&one](const std::weak_ptr<Drop>& held) { return held.lock().get() == &one; });

    if (!drops_.empty()) return;

    for (Title* title : titles()) title->lit(false);
}

// A press at `screen` outside the menu of the drop `one`: in a menu below it the submenus above
// that one close; on a title its menu opens, or on the open one's all close; anywhere else every
// menu closes.
void MenuBar::outside(Spot screen, Drop& one) {
    const auto mine = std::find_if(drops_.begin(), drops_.end(), [&one](const std::weak_ptr<Drop>& held) { return held.lock().get() == &one; });

    for (auto index = static_cast<std::size_t>(mine - drops_.begin()); index-- > 0;)
        if (const std::shared_ptr<Drop> drop = drops_[index].lock(); drop != nullptr && screened(*drop).contains(screen)) {
            shut(index + 1);

            return;
        }

    const std::vector<Title*> all = titles();

    for (std::size_t index = 0; index < all.size(); ++index) {
        if (!screened(*all[index]).contains(screen)) continue;

        if (index == root_) {
            shut(0);
        } else {
            open(index);
        }

        return;
    }

    shut(0);
}

// Opens the menu `by` (1 or -1) titles away from the open one, round the ends.
void MenuBar::stepped(int by) {
    const std::size_t many = titles().size();

    if (many == 0) return;

    open(by > 0 ? (root_ + 1) % many : (root_ + many - 1) % many);
}

// A key of a key bar, in the class "keybar": a tag and the text of its action, enabled with it. A
// click runs the action without taking the focus, as the bar of Midnight Commander does.
class KeyBar::Item : public Node {
public:
    explicit Item(std::shared_ptr<Action> one);

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    [[nodiscard]] std::optional<char32_t> letter() const;

    std::shared_ptr<Action> action_;
    Hooks hooks_;
};

// A key for `one`.
KeyBar::Item::Item(std::shared_ptr<Action> one) : action_(std::move(one)) {
    classes({"keybar"});
    enable(action_->enabled());

    hooks_.add(action_->on<Altered>([this](Altered& what) {
        enable(what.action.enabled());
        repaint();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (!clicking(what)) return;

        what.take();
        action_->run();
    }));
}

// The width the bar gives it, one row.
Size KeyBar::Item::measure(Limits room) const {
    (void)room;

    return Size{0, 1};
}

// The letter the tag would show: a letter key's, or without a shortcut the action's letter; none
// for an F key or another named key.
std::optional<char32_t> KeyBar::Item::letter() const {
    if (const std::optional<Key> key = action_->key()) {
        if (key->letter() == 0) return std::nullopt;

        return key->letter();
    }

    return action_->letter();
}

// The tag in the look of the part "tag" (the number of an F key, else the letter), then the text in
// the look of the part "text" to the end. A letter that the text holds replaces the tag with its
// mark in the look of the part "key", since the same thing twice in a few cells is noise. A
// disabled key shows all in its own look, without a mark. A text too long ends with the glyph
// `elide`.
void KeyBar::Item::paint(Canvas& onto) const {
    const int wide = onto.size().wide;
    const text::Rendering how = rendering();
    const std::string_view dots = glyph("elide");
    const std::optional<char32_t> mark = letter();
    std::string tag;

    if (const std::optional<Key> key = action_->key(); key && key->code() >= Code::f1 && key->code() <= Code::f15)
        tag = std::to_string(static_cast<int>(key->code()) - static_cast<int>(Code::f1) + 1);
    else if (mark)
        text::encoded(tag, *mark);

    const std::string whole = cut(action_->text(), wide, dots, how);
    const bool marked = mark && holding(whole, mark, how);

    if (marked) tag.clear();

    const Look plain = enabled() ? look("tag") : look();
    const Look named = enabled() ? look("text") : look();
    const int aside = std::min(text::width(tag, how), wide);
    const std::string shown = cut(action_->text(), wide - aside, dots, how);

    onto.fill(Area{{0, 0}, {wide, 1}}, named);

    if (aside > 0) {
        onto.fill(Area{{0, 0}, {aside, 1}}, plain);
        (void)onto.put({0, 0}, cut(tag, aside, {}, how), plain);
    }

    (void)spelled(onto, {aside, 0}, shown, marked && enabled() ? mark : std::nullopt, named, look("key"), how);
}

// An empty key bar, in the class "keybar".
KeyBar::KeyBar() { classes({"keybar"}); }

// Adds a key for `one` at the end.
void KeyBar::add(std::shared_ptr<Action> one) {
    if (one == nullptr) throw std::invalid_argument{"tal::KeyBar::add: no action"};

    (void)children().add<Item>(std::move(one));
}

// Takes every key away.
void KeyBar::clear() { children().clear(); }

// The least width it is given, one row, or no row while it has no keys.
Size KeyBar::measure(Limits room) const { return Size{room.least.wide, children().empty() ? 0 : 1}; }

// The keys share the row evenly; what the division leaves goes one column each to the keys on the
// left.
void KeyBar::arrange(Area inside) {
    const auto many = static_cast<int>(children().size());

    if (many == 0) return;

    const int each = inside.size.wide / many;
    int left = inside.size.wide % many;
    int x = inside.at.x;

    for (Node& one : children()) {
        const int wide = each + (left > 0 ? 1 : 0);

        one.placed(Area{{x, inside.at.y}, {wide, std::min(inside.size.high, 1)}});
        x += wide;

        if (left > 0) --left;
    }
}

// The name of a page, in the class "tab": the text between two spaces, its letter in the look of
// the part "key", with the state `current` while its page is shown. A click shows its page.
class Tabs::Name : public Node {
public:
    Name(std::string text, std::optional<char32_t> letter);

    // Marks the name as the shown page's, or not.
    void lit(bool on) { state(State::current, on); }

    // The columns it wants: the text and a space on each side.
    [[nodiscard]] int wanted() const { return text::width(said, rendering()) + 2; }

    std::string said;
    std::optional<char32_t> marked;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    Hooks hooks_;
};

// A name shown as `text` with `letter`.
Tabs::Name::Name(std::string text, std::optional<char32_t> letter) : said(std::move(text)), marked(letter) {
    classes({"tab"});

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        auto* tabs = dynamic_cast<Tabs*>(parent());

        if (!clicking(what) || tabs == nullptr) return;

        const auto at = std::find(tabs->names_.begin(), tabs->names_.end(), this);

        if (at == tabs->names_.end()) return;

        what.take();
        tabs->current = static_cast<std::size_t>(at - tabs->names_.begin());
    }));
}

// The text between two spaces.
Size Tabs::Name::measure(Limits room) const {
    (void)room;

    return Size{wanted(), 1};
}

// The text one column in, its letter in the look of the part "key".
void Tabs::Name::paint(Canvas& onto) const { (void)spelled(onto, {1, 0}, said, marked, look(), look("key"), rendering()); }

// Pages without any, in the class "tabs". Ctrl+PgUp and Ctrl+PgDn show the page before and after,
// round the ends, and Alt with a name's letter its page; these are default actions, so a node on
// the page may take the key first. A click on an arrow of crowded names steps too.
Tabs::Tabs() {
    classes({"tabs"});

    hooks_.add(on<Changed>([this](Changed& what) {
        if (what.of(current)) switched();
    }));

    hooks_.add(on<Arranged>([this](Arranged&) {
        if (!moving_) return;

        moving_ = false;

        if (pages_.empty()) return;

        if (Node* first = tree::taking(*pages_[shown()])) first->focus();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Key key = what.key;
        const Mods mods = key.mods();

        if (mods == Mods{true, false, false} && (key.code() == Code::pageup || key.code() == Code::pagedown)) {
            what.take();
            turned(key.code() == Code::pagedown ? 1 : -1);

            return;
        }

        if (!mods.alt || mods.ctrl || key.letter() == 0) return;

        for (std::size_t at = 0; at < names_.size(); ++at)
            if (lettering::same(key.letter(), names_[at]->marked)) {
                what.take();
                current = at;

                return;
            }
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (!clicking(what) || &what.target() != this) return;

        const Sides in = boxed(style());
        const int wide = area().size.wide - in.left - in.right;
        const int x = what.at.x - in.left;

        if (what.at.y != in.top || !crowded(wide) || (x != 0 && x != wide - 1)) return;

        what.take();
        turned(x == 0 ? -1 : 1);
    }));
}

// Adds a page with the name `title` and `letter` at the end, and gives it, empty, to be filled.
Column& Tabs::add(std::string title, std::optional<char32_t> letter) {
    Name& name = children().add<Name>(std::move(title), letter);
    Column& page = children().add<Column>();

    names_.push_back(&name);
    pages_.push_back(&page);

    for (std::size_t at = 0; at < names_.size(); ++at) names_[at]->lit(at == shown());

    return page;
}

// The row of names, the line under them, and the largest page, so that the size stays when the
// page changes.
Size Tabs::measure(Limits room) const {
    int names = 0;
    Size most{0, 0};
    const Limits inner{{0, 0}, {room.most.wide, std::max(room.most.high - 2, 0)}};

    for (const Name* one : names_) names += one->wanted();

    for (Column* one : pages_) {
        const Size wants = one->measured(inner);

        most = Size{std::max(most.wide, wants.wide), std::max(most.high, wants.high)};
    }

    return Size{std::max(names, most.wide), most.high + 2};
}

// The names in the middle of the first row (crowded, only the shown one, between the arrows), the
// shown page under the line; the other names and pages stand without room, so that they take
// neither the focus nor the mouse.
void Tabs::arrange(Area inside) {
    const std::size_t at = shown();
    const int wide = inside.size.wide;
    const bool crowd = crowded(wide);
    const int high = std::min(inside.size.high, 1);
    const Area none{inside.at, {0, 0}};
    int total = 0;

    for (const Name* one : names_) total += one->wanted();

    int x = inside.at.x + (wide - total) / 2;

    for (std::size_t index = 0; index < names_.size(); ++index) {
        Name& name = *names_[index];

        if (!crowd) {
            name.placed(Area{{x, inside.at.y}, {name.wanted(), high}});
            x += name.wanted();
        } else if (index == at) {
            const int room = std::min(name.wanted(), std::max(wide - 2, 0));

            name.placed(Area{{inside.at.x + (wide - room) / 2, inside.at.y}, {room, high}});
        } else {
            name.placed(none);
        }
    }

    const Area page{{inside.at.x, inside.at.y + 2}, {wide, std::max(inside.size.high - 2, 0)}};

    for (std::size_t index = 0; index < pages_.size(); ++index) pages_[index]->placed(index == at ? page : Area{page.at, {0, 0}});
}

// The thin line under the names, thick under the shown one, and the arrows `tab.left` and
// `tab.right` at the ends of crowded names.
void Tabs::paint(Canvas& onto) const {
    const int wide = onto.size().wide;
    const Look plain = look();

    if (onto.size().high >= 2) {
        const std::string_view thin = glyph("line.thin.across");
        const std::string_view thick = glyph("line.thick.across");

        for (int x = 0; x < wide; ++x) (void)onto.put({x, 1}, thin, plain);

        if (!names_.empty()) {
            const Area name = names_[shown()]->area();
            const int from = name.at.x - boxed(style()).left;

            for (int x = std::max(from, 0); x < std::min(from + name.size.wide, wide); ++x) (void)onto.put({x, 1}, thick, plain);
        }
    }

    if (!crowded(wide) || wide < 2) return;

    (void)onto.put({0, 0}, glyph("tab.left"), plain);
    (void)onto.put({wide - 1, 0}, glyph("tab.right"), plain);
}

// The page shown: `current`, or the last one when it lies past it.
std::size_t Tabs::shown() const { return pages_.empty() ? 0 : std::min(*current, pages_.size() - 1); }

// Whether the names do not fit in `wide` columns side by side.
bool Tabs::crowded(int wide) const {
    int total = 0;

    for (const Name* one : names_) total += one->wanted();

    return total > wide;
}

// The page changed: the names show which one is current, and a focus on a page now hidden moves to
// the first node of the shown page that takes it, once it is laid out. A shown page with nothing to
// take the focus leaves it where it was.
void Tabs::switched() {
    for (std::size_t at = 0; at < names_.size(); ++at) names_[at]->lit(at == shown());

    const Document* in = document();
    const Node* focus = in != nullptr ? in->focused() : nullptr;

    if (focus == nullptr) return;

    for (std::size_t at = 0; at < pages_.size(); ++at)
        if (at != shown() && tree::under(*pages_[at], *focus)) moving_ = true;
}

// Shows the page `by` (1 or -1) away from the shown one, round the ends.
void Tabs::turned(int by) {
    const std::size_t many = pages_.size();

    if (many == 0) return;

    current = by > 0 ? (shown() + 1) % many : (shown() + many - 1) % many;
}

}

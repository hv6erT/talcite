#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/clipboard.hpp"
#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/fs.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"
#include "tal/undo.hpp"
#include "tal/views.hpp"

namespace tal {

namespace spooling {

class Spool;

}

struct Stretch {
    Place from;
    Place upto;
    std::string part;
    std::string address;

    friend bool operator==(const Stretch&, const Stretch&) = default;
};

struct Range {
    Place from;
    Place upto;

    friend bool operator==(const Range&, const Range&) = default;
};

struct Followed : Event {
    explicit Followed(std::string given) : address(std::move(given)) {}

    std::string address;
};

struct Unread : Event {
    explicit Unread(Trouble given) : why(std::move(given)) {}

    Trouble why;
};

class Page : public Scrollable {
public:
    ~Page() override;

    Property<bool> wrap{*this, Effect::none, false};
    Property<std::size_t> shift{*this, Effect::none, 0};
    Property<int> tab{*this, Effect::none, 8};
    Property<std::optional<std::size_t>> current{*this, Effect::paint, std::nullopt};

    [[nodiscard]] std::optional<std::size_t> lines() const;

    [[nodiscard]] std::size_t first() const;

    void first(std::size_t line);

    void end();

    void marks(std::vector<Stretch> all);

    [[nodiscard]] const std::vector<Stretch>& marks() const;

    void reach(std::size_t mark);

    [[nodiscard]] std::size_t extent() const override;

    [[nodiscard]] std::size_t window() const override;

    [[nodiscard]] std::size_t top() const override;

    void top(std::size_t row) override;

protected:
    Page();

    void hold(std::shared_ptr<spooling::Spool> given);

    [[nodiscard]] std::optional<std::uint64_t> whole() const;

    [[nodiscard]] std::optional<std::uint64_t> at() const;

    void jump(std::uint64_t byte);

    virtual void order(std::uint64_t from, std::size_t most);

    void arrived(std::uint64_t from, Bytes got);

    void edited(std::size_t line);

    void show(Place where);

    void lit(std::vector<Range> all);

    [[nodiscard]] std::optional<Place> under(Spot at);

    [[nodiscard]] std::optional<Spot> spot(Place where) const;

    [[nodiscard]] std::optional<std::size_t> across(Place where);

    [[nodiscard]] std::optional<Place> beyond(Place from, int rows, std::size_t column);

    virtual void keyed(KeyEvent& what);

    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    struct Kept;

    void spread();

    void refreshed();

    bool moved(int rows);

    bool slid(int rows);

    bool revealed(Place where);

    void served();

    void scrolled();

    void followed(std::size_t mark);

    std::unique_ptr<Kept> kept_;
    Hooks hooks_;
};

class Pager : public Page {
public:
    explicit Pager(std::shared_ptr<Text> text);

    explicit Pager(fs::File file, Bytes head = {});

    [[nodiscard]] std::optional<std::uint64_t> size() const;

    [[nodiscard]] std::uint64_t at() const;

    void at(std::uint64_t byte);

protected:
    void order(std::uint64_t from, std::size_t most) override;

private:
    [[nodiscard]] Task<void> opened(Bytes head);

    [[nodiscard]] Task<void> fetched(std::uint64_t from, std::size_t most);

    void reread();

    std::shared_ptr<Text> text_;
    fs::File file_;
    Hook told_;
    std::optional<Scope> scope_;
};

class Editor : public Page {
public:
    explicit Editor(std::shared_ptr<Text> text, std::shared_ptr<Undo> undo = std::make_shared<Undo>(), Port<Clipboard> clipboard = {});

    ~Editor() override;

    Property<Place> cursor{*this, Effect::none, Place{}};
    Property<std::optional<Place>> anchor{*this, Effect::none, std::nullopt};
    Property<bool> readonly{*this, Effect::none, false};

    [[nodiscard]] std::optional<Range> selected() const;

    [[nodiscard]] int column() const;

    void line(std::size_t which);

    void insert(std::string_view what);

    bool undo();

    bool redo();

    [[nodiscard]] Undo& history() const;

    [[nodiscard]] Task<bool> copy();

    [[nodiscard]] Task<bool> cut();

    [[nodiscard]] Task<bool> paste();

    void mark();

    [[nodiscard]] std::optional<Range> block() const;

    void unblock();

    bool copy_block();

    bool move_block();

    bool erase_block();

    bool find(const Comb& how, bool back = false);

    bool replace(const Comb& how, std::string_view with);

    std::size_t replace_all(const Comb& how, std::string_view with);

protected:
    void keyed(KeyEvent& what) override;

    [[nodiscard]] std::optional<Caret> caret() const override;

private:
    struct Held;
    struct Change;

    void heard(const Edited& what);

    bool made(std::unique_ptr<Change> one, bool landing);

    void typed(std::string_view what);

    void erased(bool forth);

    void went(Place to, bool stretching);

    void stood(Place to, std::optional<Place> from);

    void lighted();

    void settled();

    [[nodiscard]] Place aimed(Spot at);

    void dragged(Spot at);

    void worded(Place at);

    void blocked();

    [[nodiscard]] Place fitted(Place at) const;

    [[nodiscard]] Place stepped(Place at, bool forth) const;

    [[nodiscard]] Place word(Place at, bool forth) const;

    [[nodiscard]] int high() const;

    [[nodiscard]] std::optional<Range> hit(const Comb& how, Place from, bool back) const;

    [[nodiscard]] bool editable() const;

    std::unique_ptr<Held> held_;
    Hooks hooks_;
};

}

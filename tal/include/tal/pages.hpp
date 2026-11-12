#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/fs.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"
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

    [[nodiscard]] std::optional<Place> under(Spot at) const;

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

}

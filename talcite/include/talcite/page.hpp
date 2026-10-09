#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/feed.hpp"
#include "talcite/markednode.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite {

class Spool;

}

namespace talcite::ui {

struct Place {
    std::size_t line = 0;
    std::size_t byte = 0;
};

class Page : public MarkedNode<Node> {
public:
    ~Page() override;

    [[nodiscard]] bool held() const;

    [[nodiscard]] std::optional<std::size_t> first() const;

    [[nodiscard]] std::optional<std::size_t> lines() const;

    void first(std::size_t given);

    [[nodiscard]] std::optional<std::size_t> at() const;

    void at(std::size_t given);

    [[nodiscard]] std::optional<std::size_t> whole() const;

    void end();

    bool slide(int lines);

    [[nodiscard]] bool wrapped() const;

    void wrapped(bool given);

    [[nodiscard]] bool scrollbar() const;

    void scrollbar(bool given);

    [[nodiscard]] std::size_t shown() const;

    [[nodiscard]] const std::string& scrollbar_role() const;

    void scrollbar_role(std::string given);

    [[nodiscard]] const std::string& chosen() const;

    void chosen(std::string given);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    Page();

    explicit Page(ScrollShape shape);

    void hold(std::shared_ptr<Spool> given);

    void hold(Roll given, std::size_t lines);

    [[nodiscard]] const std::shared_ptr<Spool>& spool() const;

    [[nodiscard]] int pieces(std::size_t which) const;

    void invalidate();

    bool reveal(std::size_t line);

    [[nodiscard]] std::optional<Spot> spot_of(Place where) const;

    [[nodiscard]] std::optional<Place> place_of(Spot where) const;

    [[nodiscard]] std::size_t topmost() const override;

    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(WheelEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(MouseEvent& what) override;

    bool reach(std::size_t which) override;

private:
    struct Kept;

    void page(int pages);

    bool move_by(int lines);

    void spread(Size given);

    [[nodiscard]] bool gauged(std::size_t& shift, std::size_t& whole,
                              std::size_t& window) const;

    std::unique_ptr<Kept> kept_;
};

}

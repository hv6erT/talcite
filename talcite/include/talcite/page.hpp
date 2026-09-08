#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/feed.hpp"
#include "talcite/markednode.hpp"
#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

struct Place {
    std::size_t line = 0;
    std::size_t byte = 0;
};

class Page : public MarkedNode<Node> {
public:
    Page();

    explicit Page(ScrollStyle style);

    ~Page() override;

    void hold(Roll given, std::size_t lines);

    [[nodiscard]] bool held() const;

    [[nodiscard]] std::optional<std::size_t> first() const;

    [[nodiscard]] virtual std::optional<std::size_t> lines() const;

    void first(std::size_t given);

    void end();

    void slide(int lines);

    void page(int pages);

    [[nodiscard]] bool wrapping() const;

    void wrapping(bool given);

    [[nodiscard]] bool progress() const;

    void progress(bool given);

    [[nodiscard]] std::size_t shown() const;

    [[nodiscard]] const std::string& bar() const;

    void bar(std::string given);

    [[nodiscard]] const std::string& chosen() const;

    void chosen(std::string given);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] int takes() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    [[nodiscard]] virtual std::optional<std::size_t> at() const;

    [[nodiscard]] virtual std::optional<std::size_t> below() const;

    [[nodiscard]] virtual std::optional<std::size_t> whole() const;

    virtual void counting();

    virtual void screen(int high);

    [[nodiscard]] int pieces(std::size_t which) const;

    bool moved(int lines);

    void afresh();

    bool show(std::size_t line);

    [[nodiscard]] std::optional<Spot> spotted(std::size_t line, std::size_t byte) const;

    [[nodiscard]] std::optional<Place> struck(Spot where) const;

    [[nodiscard]] std::size_t topmost() const override;

    void stirred();

    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(WheelEvent& what) override;

    bool react(ClickEvent& what) override;

    bool reach(std::size_t which) override;

private:
    struct Kept;

    void spread(Size given);

    [[nodiscard]] bool gauged(std::size_t& shift, std::size_t& whole,
                              std::size_t& window) const;

    std::unique_ptr<Kept> kept_;
};

}

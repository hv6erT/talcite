#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/chosennode.hpp"
#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

struct Warp {
    std::string title;
    int wide = 0;
    JustifyStyle justify = JustifyStyle::left;
};

struct Knot {
    std::string text;
    std::string role;
};

using Knots = std::vector<Knot>;

using Weft = std::function<std::vector<Knots>(std::size_t which, std::size_t most)>;

class Table : public ChosenNode<Node> {
public:
    Table();

    explicit Table(TableStyle style);

    ~Table() override;

    void hold(Weft given, std::size_t rows);

    [[nodiscard]] bool held() const;

    void again();

    void warps(std::vector<Warp> given);

    [[nodiscard]] std::size_t rows() const;

    [[nodiscard]] std::optional<std::size_t> which() const;

    void which(std::size_t given);

    [[nodiscard]] std::size_t first() const;

    void first(std::size_t given);

    [[nodiscard]] std::size_t shown() const;

    void step(int rows);

    void page(int pages);

    void end();

    [[nodiscard]] bool heading() const;

    void heading(bool given);

    [[nodiscard]] bool progress() const;

    void progress(bool given);

    [[nodiscard]] const std::string& bar() const;

    void bar(std::string given);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] int takes() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(WheelEvent& what) override;

    bool react(ClickEvent& what) override;

private:
    struct Kept;

    void spread(Size given);

    void stirred();

    std::unique_ptr<Kept> kept_;
};

}

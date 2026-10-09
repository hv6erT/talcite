#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/chosen.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

struct TableColumn {
    std::string title;
    int wide = 0;
    Justify justify = Justify::left;
};

struct TableCell {
    std::string text;
    std::string role;
};

using TableRow = std::vector<TableCell>;

using TableSource = std::function<std::vector<TableRow>(std::size_t from, std::size_t most)>;

class Table : public Node {
public:
    Table();

    explicit Table(TableShape shape);

    ~Table() override;

    [[nodiscard]] Chosen& chosen() { return chosen_; }

    [[nodiscard]] const Chosen& chosen() const { return chosen_; }

    void source(TableSource given, std::size_t rows);

    [[nodiscard]] bool sourced() const;

    void refresh();

    void columns(std::vector<TableColumn> given);

    [[nodiscard]] const std::vector<TableColumn>& columns() const;

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

    [[nodiscard]] bool scrollbar() const;

    void scrollbar(bool given);

    [[nodiscard]] const std::string& scrollbar_role() const;

    void scrollbar_role(std::string given);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] std::optional<std::size_t> column_under(Spot where) const;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(WheelEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(DoubleClickEvent& what) override;

    bool handle(MouseEvent& what) override;

private:
    struct Kept;

    [[nodiscard]] std::optional<std::size_t> row_under(Spot where) const;

    void spread(Size given);


    std::unique_ptr<Kept> kept_;
    Chosen chosen_;
};

}

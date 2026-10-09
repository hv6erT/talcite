#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/chosen.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

struct DiffSpan {
    std::size_t from = 0;
    std::size_t upto = 0;

    friend bool operator==(const DiffSpan&, const DiffSpan&) = default;
};

struct DiffSide {
    std::string text;
    std::vector<DiffSpan> marked;
    std::optional<std::size_t> number;
};

struct DiffRow {
    std::optional<DiffSide> left;
    std::optional<DiffSide> right;
};

struct DiffHunk {
    std::string title;
    std::vector<DiffRow> rows;
    bool tickable = false;
    std::string role;
};

using DiffSource = std::function<std::optional<std::vector<DiffHunk>>(std::size_t from, std::size_t most)>;

[[nodiscard]] std::pair<DiffSide, DiffSide> diffed(std::string_view left, std::string_view right);

class DiffViewer : public Node {
public:
    DiffViewer();

    explicit DiffViewer(DiffShape shape);

    ~DiffViewer() override;

    [[nodiscard]] Chosen& chosen() { return chosen_; }

    [[nodiscard]] const Chosen& chosen() const { return chosen_; }

    void source(DiffSource given, std::size_t hunks);

    void refresh();

    void titles(std::string left, std::string right);

    [[nodiscard]] std::size_t hunks() const;

    [[nodiscard]] std::optional<std::size_t> which() const;

    void which(std::size_t given);

    [[nodiscard]] bool opened(std::size_t hunk) const;

    void opened(std::size_t hunk, bool given);

    [[nodiscard]] std::optional<bool> unified() const;

    void unified(std::optional<bool> given);

    [[nodiscard]] bool scrollbar() const;

    void scrollbar(bool given);

    [[nodiscard]] const std::string& scrollbar_role() const;

    void scrollbar_role(std::string given);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(WheelEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(MouseEvent& what) override;

private:
    struct Kept;

    void spread(Size given);

    bool slid(int lines);

    std::unique_ptr<Kept> kept_;
    Chosen chosen_;
};

}

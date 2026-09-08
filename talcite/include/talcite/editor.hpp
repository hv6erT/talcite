#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "talcite/bytes.hpp"
#include "talcite/comb.hpp"
#include "talcite/feed.hpp"
#include "talcite/page.hpp"

namespace talcite::ui {

class Editor : public Page {
public:
    Editor();

    explicit Editor(ScrollStyle style);

    ~Editor() override;

    using Page::hold;

    void hold(Bytes given);

    void hold(Feed given, std::size_t whole);

    [[nodiscard]] bool dirty() const;

    void clean();

    bool save(const Drain& into);

    void settle();

    [[nodiscard]] bool undoable() const;

    [[nodiscard]] bool redoable() const;

    void undo();

    void redo();

    [[nodiscard]] bool readonly() const;

    void readonly(bool given);

    [[nodiscard]] std::size_t cursor() const;

    void cursor(std::size_t given);

    void stretch(std::size_t given);

    [[nodiscard]] std::size_t line() const;

    [[nodiscard]] std::size_t column() const;

    [[nodiscard]] std::size_t anchor() const;

    [[nodiscard]] bool selected() const;

    void select(std::size_t from, std::size_t upto);

    void unselect();

    [[nodiscard]] std::string selection() const;

    bool copy();

    bool cut();

    bool paste();

    void insert(std::string what);

    void back_erase(std::size_t much);

    void front_erase(std::size_t much);

    std::optional<Hit> find(const Comb& how, std::size_t from, bool back);

    std::size_t replace(const Comb& how, const std::string& with, bool all);

    [[nodiscard]] std::optional<std::size_t> at() const override;

    [[nodiscard]] std::optional<std::size_t> whole() const override;

    [[nodiscard]] std::optional<std::size_t> lines() const override;

    [[nodiscard]] std::optional<Spot> caret() const override;

protected:
    [[nodiscard]] std::optional<std::size_t> below() const override;

    void counting() override;

    void screen(int high) override;

    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

    bool react(DoubleClickEvent& what) override;

    bool react(MouseEvent& what) override;

    bool react(PasteEvent& what) override;

private:
    void settled(std::size_t where);

    struct Kept;

    std::unique_ptr<Kept> kept_;
};

}

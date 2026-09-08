#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "talcite/node.hpp"

namespace talcite::ui {

class Field : public Node {
public:
    explicit Field(int wide, std::string text = {});

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given);

    [[nodiscard]] int wide() const { return wide_; }

    void wide(int given) { wide_ = given; }

    [[nodiscard]] bool able() const { return able_; }

    void able(bool given);

    [[nodiscard]] std::size_t cursor() const;

    void cursor(std::size_t given);

    void guess(std::function<std::string(const std::string& what)> given);

    [[nodiscard]] const std::string& guessed() const { return ahead_; }

    void allows(std::function<bool(char32_t one)> given);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] std::optional<Spot> caret() const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

    bool react(PasteEvent& what) override;

private:
    void guessing();

    void insert(const std::string& what);

    std::string text_;
    std::string ahead_;
    std::function<std::string(const std::string& what)> guess_;
    std::function<bool(char32_t one)> allows_;
    std::size_t at_ = 0;
    std::size_t from_ = 0;
    int wide_ = 0;
    bool able_ = true;
};

}

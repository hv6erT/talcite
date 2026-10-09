#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "talcite/enablednode.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Field : public EnabledNode<Node> {
public:
    explicit Field(int wide, std::string text = {});

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given);

    [[nodiscard]] int wide() const { return wide_; }

    void wide(int given) { wide_ = given; }

    [[nodiscard]] std::size_t cursor() const;

    void cursor(std::size_t given);

    void guess(std::function<std::string(const std::string& what)> given);

    [[nodiscard]] const std::string& guessed() const { return ahead_; }

    void allows(std::function<bool(char32_t one)> given);

    [[nodiscard]] bool concealed() const { return concealed_; }

    void concealed(bool on);

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] std::optional<Spot> caret() const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(PasteEvent& what) override;

private:
    void guessing();

    void insert(const std::string& what);

    [[nodiscard]] int spanned(std::size_t from, std::size_t upto) const;

    std::string text_;
    std::string ahead_;
    std::function<std::string(const std::string& what)> guess_;
    std::function<bool(char32_t one)> allows_;
    std::size_t at_ = 0;
    std::size_t from_ = 0;
    int wide_ = 0;
    bool concealed_ = false;
};

}

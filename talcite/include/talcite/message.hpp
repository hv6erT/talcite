#pragma once

#include <memory>
#include <string>
#include <vector>

#include "talcite/dialog.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class Button;
class Frame;
class Row;
class Span;

class Message : public Dialog {
public:
    Message(std::string title, std::string text, MessageStyle style = {});

    [[nodiscard]] const std::string& title() const;

    void title(std::string given);

    [[nodiscard]] const std::string& text() const;

    void text(std::string given);

    using Node::role;

    void role(std::string given) override;

    std::shared_ptr<Button> add(std::string text);

    void clear();

    [[nodiscard]] const std::vector<std::shared_ptr<Button>>& buttons() const { return buttons_; }

private:
    std::shared_ptr<Frame> frame_;
    std::shared_ptr<Span> title_;
    std::shared_ptr<Span> text_;
    std::vector<std::shared_ptr<Frame>> boxes_;
    std::shared_ptr<Row> row_;
    std::vector<std::shared_ptr<Button>> buttons_;
    MessageStyle style_;
};

}

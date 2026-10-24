#pragma once

#include <memory>
#include <string>
#include <vector>

#include "talcite/buttons.hpp"
#include "talcite/dialog.hpp"
#include "talcite/flow.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Frame;
class Span;

class Message : public Dialog {
public:
    Message(std::string title, std::string text, MessageShape shape = {});

    [[nodiscard]] const std::string& title() const;

    void title(std::string given);

    [[nodiscard]] const std::string& text() const;

    void text(std::string given);

    using Node::role;

    void role(std::string given) override;

    [[nodiscard]] Buttons& buttons() { return buttons_; }

    [[nodiscard]] const Buttons& buttons() const { return buttons_; }

private:
    std::shared_ptr<Frame> frame_;
    std::shared_ptr<Span> title_;
    std::shared_ptr<Span> text_;
    std::vector<std::shared_ptr<Frame>> boxes_;
    std::shared_ptr<Flow> row_ = std::make_shared<Flow>(1);
    Buttons buttons_{*row_};
    MessageShape shape_;
};

}

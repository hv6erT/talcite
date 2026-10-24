#pragma once

#include <memory>
#include <string>

#include "talcite/buttons.hpp"
#include "talcite/dialog.hpp"
#include "talcite/flow.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Column;
class Frame;

class Form : public Dialog {
public:
    explicit Form(std::string title, FormShape shape = {});

    [[nodiscard]] const std::string& title() const;

    void title(std::string given);

    [[nodiscard]] const std::shared_ptr<Column>& body() const { return body_; }

    [[nodiscard]] Buttons& buttons() { return buttons_; }

    [[nodiscard]] const Buttons& buttons() const { return buttons_; }

private:
    std::shared_ptr<Frame> frame_;
    std::shared_ptr<Column> body_;
    std::shared_ptr<Flow> row_ = std::make_shared<Flow>(1);
    Buttons buttons_{*row_};
    FormShape shape_;
};

}

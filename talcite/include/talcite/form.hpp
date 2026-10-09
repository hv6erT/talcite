#pragma once

#include <memory>
#include <string>

#include "talcite/buttons.hpp"
#include "talcite/dialog.hpp"
#include "talcite/row.hpp"
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
    std::shared_ptr<Row> row_ = std::make_shared<Row>(RowShape{1, true});
    Buttons buttons_{*row_};
    FormShape shape_;
};

}

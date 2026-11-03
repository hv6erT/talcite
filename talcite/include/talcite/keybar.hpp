#pragma once

#include <memory>
#include <string>
#include <vector>

#include "talcite/node.hpp"
#include "talcite/row.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class KeyBarItem;

class KeyBar : public Node {
public:
    KeyBar();

    std::shared_ptr<KeyBarItem> add(Key key, std::string text);

    [[nodiscard]] const std::vector<std::shared_ptr<KeyBarItem>>& items() const { return items_; }

    void clear();

    [[nodiscard]] Node* content() const;

    void content(std::shared_ptr<Node> given);

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

    bool handle(KeyEvent& what) override;

private:
    [[nodiscard]] int width(int which, int wide) const;

    [[nodiscard]] int along(int which, int wide) const;

    std::shared_ptr<Row> body_;
    std::vector<std::shared_ptr<KeyBarItem>> items_;
};

}

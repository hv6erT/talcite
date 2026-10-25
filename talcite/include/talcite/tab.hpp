#pragma once

#include <memory>
#include <string>

#include "talcite/keyedmixin.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Tabs;

class Tab : public KeyedMixin<Node> {
public:
    explicit Tab(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    void activate() override;

private:
    friend class Tabs;

    void shown(bool on);

    std::string text_;
    std::shared_ptr<Node> page_;
};

}

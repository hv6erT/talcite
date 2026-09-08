#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "talcite/optionsnode.hpp"
#include "talcite/veil.hpp"

namespace talcite::ui {

class Select : public OptionsNode<Node> {
public:
    Select();

    explicit Select(std::vector<std::string> options, std::size_t which = 0);

    [[nodiscard]] int most() const { return most_; }

    void most(int given);

    [[nodiscard]] bool opened() const;

    void open();

    void close();

    using OptionsNode::able;

    void able(bool given) override;

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

private:
    void chose(const std::string& given);

    int most_ = 8;
    std::weak_ptr<Veil> rolled_;
};

}

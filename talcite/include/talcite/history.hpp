#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/field.hpp"
#include "talcite/keyednode.hpp"
#include "talcite/optionsnode.hpp"
#include "talcite/veil.hpp"

namespace talcite::ui {

class History : public KeyedNode<Field> {
public:
    explicit History(int wide, std::string text = {});

    [[nodiscard]] Options& options() { return options_; }

    [[nodiscard]] const Options& options() const { return options_; }

    using Field::able;

    [[nodiscard]] bool able() const override;

    void clear();

    [[nodiscard]] int keeps() const { return keeps_; }

    void keeps(int given);

    [[nodiscard]] int most() const { return most_; }

    void most(int given);

    [[nodiscard]] bool opened() const;

    void open();

    void close();

protected:
    View paint(Size given, Area seen) override;

    void act() override;

    bool react(KeyEvent& what) override;

private:
    void settle();

    void took(std::size_t which);

    void back();

    Options options_;
    std::string typed_;
    std::size_t seen_ = 0;
    std::size_t walked_ = 0;
    int most_ = 8;
    int keeps_ = 60;
    std::weak_ptr<Veil> rolled_;
};

}

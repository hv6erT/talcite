#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/field.hpp"
#include "talcite/keyedmixin.hpp"
#include "talcite/optionsmixin.hpp"

namespace talcite::ui {

class Popup;

class HistoryField : public KeyedMixin<Field> {
public:
    explicit HistoryField(int wide, std::string text = {});

    ~HistoryField() override;

    [[nodiscard]] Options& options() { return options_; }

    [[nodiscard]] const Options& options() const { return options_; }

    void clear();

    [[nodiscard]] int keeps() const { return keeps_; }

    void keeps(int given);

    [[nodiscard]] int rows() const;

    void rows(int given);

    [[nodiscard]] bool opened() const;

    void open();

    void close();

protected:
    View paint(Size given, Area seen) override;

    void activate() override;

    using KeyedMixin::handle;

    bool handle(KeyEvent& what) override;

    bool handle(DisableEvent& what) override;

private:
    void settle();

    void took(std::size_t which);

    void back();

    Options options_;
    std::string typed_;
    std::size_t seen_ = 0;
    std::size_t walked_ = 0;
    int keeps_ = 60;
    std::unique_ptr<Popup> popup_;
};

}

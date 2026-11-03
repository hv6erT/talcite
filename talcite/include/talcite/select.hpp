#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "talcite/enabledmixin.hpp"
#include "talcite/event.hpp"
#include "talcite/optionsmixin.hpp"

namespace talcite::ui {

class Popup;

class Select : public OptionsMixin<EnabledMixin<Node>> {
public:
    Select();

    explicit Select(std::vector<std::string> options, std::size_t which = 0);

    ~Select() override;

    [[nodiscard]] int rows() const;

    void rows(int given);

    [[nodiscard]] bool opened() const;

    void open();

    void close();

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void paint(Canvas& onto) const override;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(DisableEvent& what) override;

private:
    void chose(const std::string& given);

    std::unique_ptr<Popup> popup_;
};

}

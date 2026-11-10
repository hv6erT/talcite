#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "tal/clock.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

namespace tal::testing {

class Scene {
public:
    explicit Scene(Size screen = {80, 24}, Colors colors = Colors::rgb, bool unicode = true, text::Rendering how = {});

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] Port<Clock> time() const;

    [[nodiscard]] ManualClock& clock();

    void show(Document& one);

    void resize(Size screen);

    void press(Key key);

    void type(std::string_view text);

    void paste(std::string text);

    void click(Spot at, Button button = Button::left);

    void click(Node& one, Button button = Button::left);

    void twice(Spot at);

    void point(Spot at);

    void wheel(Spot at, int steps, Axis axis = Axis::vertical);

    void wait(std::chrono::milliseconds much);

    [[nodiscard]] const Surface& screen() const;

    [[nodiscard]] const Cell& at(Spot where) const;

    [[nodiscard]] std::string row(int y) const;

    [[nodiscard]] std::string text() const;

    [[nodiscard]] std::optional<Spot> where(std::string_view said) const;

    [[nodiscard]] std::optional<Caret> caret() const;

private:
    [[nodiscard]] Document& shown() const;

    void settled();

    std::shared_ptr<ManualClock> clock_;
    Size screen_;
    Colors colors_;
    bool unicode_;
    text::Rendering how_;
    Document* shown_ = nullptr;
    const Surface* last_ = nullptr;
};

}

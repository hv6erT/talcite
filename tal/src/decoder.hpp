#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tal/clock.hpp"
#include "tal/event.hpp"
#include "tal/document.hpp"

namespace tal {

struct Struck {
    Key key;

    friend bool operator==(const Struck&, const Struck&) = default;
};

struct Moused {
    Spot where;
    Press press = Press::move;
    MouseButton button = MouseButton::none;
    Mods mods;

    friend bool operator==(const Moused&, const Moused&) = default;
};

struct Wheeled {
    Spot where;
    int steps = 0;
    Axis axis = Axis::vertical;
    Mods mods;

    friend bool operator==(const Wheeled&, const Wheeled&) = default;
};

struct Pasted {
    std::string text;
    bool whole = true;

    friend bool operator==(const Pasted&, const Pasted&) = default;
};

struct Windowed {
    bool in = false;

    friend bool operator==(const Windowed&, const Windowed&) = default;
};

using Heard = std::variant<Struck, Moused, Wheeled, Pasted, Windowed>;

class Decoder {
public:
    Decoder();

    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    [[nodiscard]] std::vector<Heard> fed(std::string_view bytes, std::chrono::steady_clock::time_point now);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> due() const;

    [[nodiscard]] std::vector<Heard> lapsed(std::chrono::steady_clock::time_point now);

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

bool dispatched(const Heard& one, Document& onto);

}

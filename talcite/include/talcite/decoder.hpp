#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "talcite/clock.hpp"
#include "talcite/event.hpp"
#include "talcite/root.hpp"

namespace talcite {

struct Struck {
    ui::Key key;

    friend bool operator==(const Struck&, const Struck&) = default;
};

struct Moused {
    ui::Spot where;
    ui::Press press = ui::Press::move;
    ui::Mouse button = ui::Mouse::none;
    ui::Mods mods;

    friend bool operator==(const Moused&, const Moused&) = default;
};

struct Wheeled {
    ui::Spot where;
    int steps = 0;
    ui::Axis axis = ui::Axis::vertical;
    ui::Mods mods;

    friend bool operator==(const Wheeled&, const Wheeled&) = default;
};

struct Pasted {
    std::string text;
    bool whole = true;

    friend bool operator==(const Pasted&, const Pasted&) = default;
};

struct Focused {
    bool in = false;

    friend bool operator==(const Focused&, const Focused&) = default;
};

using Heard = std::variant<Struck, Moused, Wheeled, Pasted, Focused>;

class Decoder {
public:
    Decoder();

    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    [[nodiscard]] std::vector<Heard> fed(std::string_view bytes, Clock::Moment now);

    [[nodiscard]] std::optional<Clock::Moment> due() const;

    [[nodiscard]] std::vector<Heard> lapsed(Clock::Moment now);

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

[[nodiscard]] bool dispatched(const Heard& one, ui::Root& onto);

}

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/surface.hpp"

namespace tal {

enum class Stage : std::uint8_t { prompt, input, output, done };

struct Staged {
    Stage stage = Stage::prompt;
    std::optional<int> code;

    friend bool operator==(const Staged&, const Staged&) = default;
};

enum class Mousing : std::uint8_t { none, clicks, drags, moves };

struct Modes {
    bool cursor_keys = false;
    bool keypad = false;
    bool paste = false;
    bool focus = false;
    Mousing mouse = Mousing::none;
    bool sgr_mouse = false;
    bool cursor = true;
    bool alternate = false;
    bool wrap = true;
    bool origin = false;
    bool insert = false;
    bool newline = false;
    bool synchronized = false;
    Caret::Form form = Caret::Form::usual;
    bool blink = true;

    friend bool operator==(const Modes&, const Modes&) = default;
};

struct Fed {
    Bytes replies;
    std::vector<Staged> stages;
    int bells = 0;
    bool titled = false;
    bool moved = false;
    bool switched = false;
};

class Emulator {
public:
    struct Row {
        std::vector<Cell> cells;
        bool wrapped = false;

        friend bool operator==(const Row&, const Row&) = default;
    };

    explicit Emulator(Size size, std::size_t kept = 5000);

    ~Emulator();

    Emulator(Emulator&&) noexcept;
    Emulator& operator=(Emulator&&) noexcept;
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    Fed feed(std::span<const std::byte> bytes);

    void resize(Size size);

    [[nodiscard]] Size size() const;

    [[nodiscard]] std::size_t history() const;

    [[nodiscard]] std::uint64_t scrolled() const;

    [[nodiscard]] Row row(int at) const;

    [[nodiscard]] Spot cursor() const;

    [[nodiscard]] const Modes& modes() const;

    [[nodiscard]] const std::string& title() const;

    [[nodiscard]] const std::string& folder() const;

    [[nodiscard]] std::string_view address(std::uint32_t link) const;

    [[nodiscard]] bool waiting() const;

    [[nodiscard]] std::optional<int> prompt() const;

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

}

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "talcite/area.hpp"
#include "talcite/bytes.hpp"
#include "talcite/shape.hpp"
#include "talcite/style.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

enum class Marked : std::uint8_t { prompt, input, output, done };

struct ShellMark {
    Marked kind = Marked::prompt;
    std::optional<int> code;

    friend bool operator==(const ShellMark&, const ShellMark&) = default;
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
    CaretShape caret;
};

struct Fed {
    Bytes replies;
    std::vector<ShellMark> marks;
    int bells = 0;
    bool titled = false;
    bool moved = false;
    bool switched = false;
};

class Emulator {
public:
    using Line = std::vector<Cell>;

    explicit Emulator(Size size, std::size_t kept = 5000);

    Fed feed(std::span<const std::byte> bytes);

    void resize(Size size);

    [[nodiscard]] Size size() const { return size_; }

    [[nodiscard]] std::size_t history() const;

    [[nodiscard]] const Line& line(int row) const;

    [[nodiscard]] Spot cursor() const { return {x_, y_}; }

    [[nodiscard]] const Modes& modes() const { return modes_; }

    [[nodiscard]] const std::string& title() const { return title_; }

    [[nodiscard]] const std::string& folder() const { return folder_; }

    [[nodiscard]] bool waiting() const;

    [[nodiscard]] std::optional<int> prompt() const;

private:
    enum class State : std::uint8_t { ground, escape, intermediate, csi, osc, ignored };

    enum class Stage : std::uint8_t { unknown, prompt, input, running };

    struct Saved {
        int x = 0;
        int y = 0;
        bool pending = false;
        Style style;
        bool concealed = false;
        bool origin = false;
        bool special = false;
        bool spare = false;
        bool shifted = false;
    };

    [[nodiscard]] std::vector<Line>& screen();

    [[nodiscard]] const std::vector<Line>& screen() const;

    [[nodiscard]] Line blank() const;

    [[nodiscard]] Cell erased() const;

    void step(char32_t one, Fed& fed);

    void control(char32_t one, Fed& fed);

    void print(char32_t one);

    void joined(char32_t one);

    void broke(int x);

    void index();

    void reverse();

    void newline();

    void scroll_up(int many);

    void scroll_down(int many);

    void remembered(Line one);

    void escaped(char32_t final);

    void dispatched(char32_t final, Fed& fed);

    void moded(bool on, Fed& fed);

    void graphic();

    void erase_display(int how);

    void erase_line(int how);

    void inserted_lines(int many);

    void deleted_lines(int many);

    void tab(int many);

    void placed(int x, int y);

    void saved();

    void restored();

    void reset();

    void switched(bool to, bool clearing, Fed& fed);

    void stringed(Fed& fed);

    void foldered(const std::string& said);

    void marked(const std::string& said, Fed& fed);

    [[nodiscard]] int number(std::size_t at, int fallback) const;

    [[nodiscard]] Color colored(std::size_t& at) const;

    Size size_;
    std::size_t kept_ = 0;
    std::vector<Line> primary_;
    std::vector<Line> alternate_;
    std::deque<Line> history_;
    std::uint64_t scrolled_ = 0;
    int x_ = 0;
    int y_ = 0;
    bool pending_ = false;
    Style style_;
    bool concealed_ = false;
    int top_ = 0;
    int bottom_ = 0;
    std::vector<bool> tabs_;
    Modes modes_;
    bool special_ = false;
    bool spare_ = false;
    bool shifted_ = false;
    Saved primary_saved_;
    Saved alternate_saved_;
    State state_ = State::ground;
    std::vector<std::vector<int>> params_;
    std::string intermediates_;
    char prefix_ = 0;
    bool fresh_ = true;
    std::string string_;
    bool string_escape_ = false;
    bool osc_ = false;
    std::string partial_;
    char32_t last_ = 0;
    std::string title_;
    std::string folder_;
    Stage stage_ = Stage::unknown;
    std::optional<std::uint64_t> prompt_;
};

}

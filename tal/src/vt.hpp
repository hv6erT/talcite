#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tal/clock.hpp"

namespace tal::vt {

inline constexpr std::size_t most_params = 16;
inline constexpr std::size_t most_parts = 4;
inline constexpr int most_value = 65535;

struct Param {
    std::array<int, most_parts> parts{-1, -1, -1, -1};

    [[nodiscard]] int part(std::size_t which, int otherwise = 0) const {
        return which < parts.size() && parts[which] >= 0 ? parts[which] : otherwise;
    }
};

struct Plain {
    char32_t letter = 0;
    bool alt = false;
};

struct Steered {
    unsigned char byte = 0;
    bool alt = false;
};

struct Lone {};

struct Csi {
    char lead = 0;
    std::vector<Param> params;
    std::string between;
    char last = 0;

    [[nodiscard]] int at(std::size_t which, int otherwise = 0) const {
        return which < params.size() ? params[which].part(0, otherwise) : otherwise;
    }

    [[nodiscard]] int part(std::size_t which, std::size_t sub, int otherwise = 0) const {
        return which < params.size() ? params[which].part(sub, otherwise) : otherwise;
    }
};

struct Ss3 {
    int number = 0;
    char last = 0;
};

struct Told {
    char lead = 0;
    std::string body;
};

struct Tracked {
    int mask = 0;
    int x = 0;
    int y = 0;
};

struct Glued {
    std::string text;
    bool whole = true;
};

struct Token {
    std::variant<Plain, Steered, Lone, Csi, Ss3, Told, Tracked, Glued> what;
    std::string raw;
};

class Reader {
public:
    explicit Reader(bool raw = false);

    void fed(std::string_view bytes, std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> due() const;

    void lapsed(std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    [[nodiscard]] std::string drained();

private:
    enum class State : std::uint8_t {
        ground,
        lettered,
        escaped,
        csi,
        csi_ignored,
        linux_keyed,
        ss3,
        told,
        told_escaped,
        told_ignored,
        tracked,
        glued
    };

    void ground();

    [[nodiscard]] std::string taken(unsigned char last);

    void stepped(unsigned char byte, std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    void in_letter(unsigned char byte, std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    void in_escape(unsigned char byte, std::vector<Token>& into);

    void in_csi(unsigned char byte, std::vector<Token>& into);

    void in_ignored_csi(unsigned char byte);

    void in_linux_key(unsigned char byte, std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    void in_ss3(unsigned char byte, std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    void in_string(unsigned char byte, std::vector<Token>& into);

    void in_string_escape(unsigned char byte, std::chrono::steady_clock::time_point now, std::vector<Token>& into);

    void in_ignored_string(unsigned char byte);

    void in_mouse(unsigned char byte, std::vector<Token>& into);

    void grounded(unsigned char byte, bool alt, std::vector<Token>& into);

    void sent(Token made, std::vector<Token>& into);

    void finished(unsigned char last, std::vector<Token>& into);

    void glued(unsigned char byte, std::vector<Token>& into);

    bool raw_ = false;
    State state_ = State::ground;
    std::string pending_;
    bool alt_ = false;
    std::string glued_;
    std::string tail_;
    bool overflowed_ = false;
    std::chrono::steady_clock::time_point since_{};
};

}

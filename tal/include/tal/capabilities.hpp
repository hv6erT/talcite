#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "tal/console.hpp"
#include "tal/geometry.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"

namespace tal {

enum class Mouse : std::uint8_t { none, basic, sgr, pixels };

enum class Screen : std::uint8_t { none, older, apart };

enum class Shade : std::uint8_t { neutral, light, dark };

enum class Graphics : std::uint8_t { none, sixel, kitty };

enum class Blocks : std::uint8_t { basic, sextants, octants };

struct Setup {
    std::optional<Colors> colors;
    std::optional<Mouse> mouse;
    std::optional<Screen> screen;
    std::optional<Shade> shade;
    std::optional<Graphics> graphics;
    std::optional<Blocks> blocks;
    std::optional<Size> cell;

    std::optional<bool> unicode;
    std::optional<bool> hover;
    std::optional<bool> bracketed_paste;
    std::optional<bool> synchronized;
    std::optional<bool> titles;
    std::optional<bool> links;
};

class Capabilities {
public:
    explicit Capabilities(Port<Console> with = {});

    ~Capabilities();

    Capabilities(const Capabilities&) = delete;
    Capabilities& operator=(const Capabilities&) = delete;
    Capabilities(Capabilities&&) = delete;
    Capabilities& operator=(Capabilities&&) = delete;

    [[nodiscard]] Colors colors() const;

    [[nodiscard]] Mouse mouse() const;

    [[nodiscard]] Screen screen() const;

    [[nodiscard]] Shade shade() const;

    [[nodiscard]] Graphics graphics() const;

    [[nodiscard]] Blocks blocks() const;

    [[nodiscard]] Size cell() const;

    [[nodiscard]] bool bracketed_paste() const;

    [[nodiscard]] bool focus_events() const;

    [[nodiscard]] bool synchronized() const;

    [[nodiscard]] bool unicode() const;

    [[nodiscard]] bool titles() const;

    [[nodiscard]] bool links() const;

    [[nodiscard]] int wide() const;

    [[nodiscard]] int varied() const;

    [[nodiscard]] int joined() const;

    [[nodiscard]] const std::string& named() const;

    [[nodiscard]] int version() const;

    [[nodiscard]] const std::string& rest() const;

private:
    struct Held;

    void ready() const;

    void ground() const;

    void probe() const;

    void fed(std::string_view bytes) const;

    std::unique_ptr<Held> held_;
};

}

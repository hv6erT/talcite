#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "talcite/area.hpp"
#include "talcite/blocks.hpp"
#include "talcite/console.hpp"
#include "talcite/seam.hpp"

namespace talcite {

enum class Colors : std::uint8_t { mono, ansi, palette, rgb };

enum class Mouse : std::uint8_t { none, basic, sgr };

enum class Screen : std::uint8_t { none, older, apart };

enum class Shade : std::uint8_t { neutral, light, dark };

enum class Graphics : std::uint8_t { none, sixel, kitty };

class Capabilities {
public:
    explicit Capabilities(Seam<Console> with = console());

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
    [[nodiscard]] ui::Blocks blocks() const;

    [[nodiscard]] ui::Size cell() const;

    [[nodiscard]] bool bracketed_paste() const;
    [[nodiscard]] bool focus_events() const;
    [[nodiscard]] bool synchronized() const;
    [[nodiscard]] bool unicode() const;
    [[nodiscard]] bool titles() const;

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

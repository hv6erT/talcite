#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "talcite/area.hpp"
#include "talcite/blocks.hpp"

namespace talcite {

enum class Colors : std::uint8_t { mono, ansi, palette, rgb };

enum class Mouse : std::uint8_t { none, basic, sgr };

enum class Screen : std::uint8_t { none, older, apart };

enum class Shade : std::uint8_t { neutral, light, dark };

enum class Graphics : std::uint8_t { none, sixel, kitty };

class Compatibilities {
public:
    Compatibilities();

    virtual ~Compatibilities();

    Compatibilities(const Compatibilities&) = delete;
    Compatibilities& operator=(const Compatibilities&) = delete;
    Compatibilities(Compatibilities&&) = delete;
    Compatibilities& operator=(Compatibilities&&) = delete;

    [[nodiscard]] Colors colors() const;
    [[nodiscard]] Mouse mouse() const;
    [[nodiscard]] Screen screen() const;
    [[nodiscard]] Shade shade() const;
    [[nodiscard]] Graphics graphics() const;
    [[nodiscard]] ui::Blocks blocks() const;

    [[nodiscard]] ui::Size cell() const;

    [[nodiscard]] bool pasting() const;
    [[nodiscard]] bool focusing() const;
    [[nodiscard]] bool syncing() const;
    [[nodiscard]] bool unicode() const;
    [[nodiscard]] bool titling() const;

    [[nodiscard]] int wide() const;
    [[nodiscard]] int varied() const;
    [[nodiscard]] int joined() const;

    [[nodiscard]] const std::string& named() const;
    [[nodiscard]] int version() const;

    [[nodiscard]] const std::string& rest() const;

protected:
    [[nodiscard]] virtual bool lively() const;

    virtual void tell(std::string_view bytes) const;

    [[nodiscard]] virtual std::string hear(std::chrono::milliseconds patience) const;

    [[nodiscard]] virtual bool painting() const;

private:
    struct Held;

    void ready() const;

    void ground() const;

    void probe() const;

    void fed(std::string_view bytes) const;

    std::unique_ptr<Held> held_;
};

}

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "talcite/area.hpp"
#include "talcite/bell.hpp"
#include "talcite/seam.hpp"

namespace talcite {

class Console : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "console"; }

    [[nodiscard]] virtual std::optional<std::string> variable(std::string_view name) const = 0;

    [[nodiscard]] virtual bool lively() const = 0;

    [[nodiscard]] virtual bool vt() const = 0;

    [[nodiscard]] virtual std::optional<ui::Size> size() const = 0;

    [[nodiscard]] virtual std::chrono::milliseconds double_click() const = 0;

    virtual void raw(bool on) = 0;

    virtual void write(std::string_view bytes) = 0;

    [[nodiscard]] virtual std::string hear(std::optional<std::chrono::milliseconds> patience,
                                           const Bell* also) = 0;

    [[nodiscard]] virtual bool resumed() { return false; }
};

[[nodiscard]] Seam<Console> console();

}

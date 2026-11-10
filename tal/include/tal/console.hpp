#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/geometry.hpp"
#include "tal/port.hpp"

namespace tal {

class Console : public Adapter {
public:
    static constexpr std::string_view kind = "console";

    [[nodiscard]] virtual std::optional<std::string> variable(std::string_view name) const = 0;

    [[nodiscard]] virtual bool lively() const = 0;

    [[nodiscard]] virtual bool vt() const = 0;

    [[nodiscard]] virtual std::optional<Size> size() const = 0;

    [[nodiscard]] virtual std::chrono::milliseconds double_click() const = 0;

    virtual void raw(bool on) = 0;

    virtual void write(std::string_view bytes) = 0;

    [[nodiscard]] virtual std::string hear(std::optional<std::chrono::milliseconds> patience) = 0;

    virtual void interrupt() = 0;

    [[nodiscard]] virtual bool resumed() { return false; }

    [[nodiscard]] static std::shared_ptr<Console> builtin();
};

class ScriptedConsole : public Console {
public:
    explicit ScriptedConsole(std::string said = {}, std::size_t piece = 0);

    std::map<std::string, std::string, std::less<>> environment{{"TERM", "xterm-256color"}, {"LANG", "C.UTF-8"}};
    bool alive = true;
    bool sequences = false;
    std::optional<Size> window;
    std::chrono::milliseconds twice{500};
    std::deque<std::string> replies;
    std::string written;
    int depth = 0;
    int entered = 0;
    int interrupted = 0;
    int rounds = 0;
    bool comeback = false;
    std::vector<std::optional<std::chrono::milliseconds>> patience;

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override;

    [[nodiscard]] bool lively() const override { return alive; }

    [[nodiscard]] bool vt() const override { return sequences; }

    [[nodiscard]] std::optional<Size> size() const override { return window; }

    [[nodiscard]] std::chrono::milliseconds double_click() const override { return twice; }

    void raw(bool on) override;

    void write(std::string_view bytes) override { written += bytes; }

    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds> patience) override;

    void interrupt() override { ++interrupted; }

    [[nodiscard]] bool resumed() override;
};

}

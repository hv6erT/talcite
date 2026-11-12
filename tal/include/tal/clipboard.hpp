#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/console.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"

namespace tal {

enum class Handing { copy, cut };

struct Handed {
    std::vector<Path> paths;
    Handing how = Handing::copy;

    friend bool operator==(const Handed&, const Handed&) = default;
};

class Clipboard : public Adapter {
public:
    static constexpr std::string_view kind = "clipboard";

    [[nodiscard]] static std::shared_ptr<Clipboard> builtin();

    [[nodiscard]] virtual Task<bool> copy(std::string what) = 0;

    [[nodiscard]] virtual Task<std::optional<std::string>> paste() = 0;

    [[nodiscard]] virtual Task<bool> give(Handed what);

    [[nodiscard]] virtual Task<std::optional<Handed>> taken();
};

[[nodiscard]] Port<Clipboard> clipboard(Port<Console> console, Port<Processes> processes);

class ScriptedClipboard : public Clipboard {
public:
    std::optional<std::string> held;
    std::optional<Handed> files;
    std::vector<std::string> copied;
    bool takes = true;

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] Task<bool> copy(std::string what) override;

    [[nodiscard]] Task<std::optional<std::string>> paste() override;

    [[nodiscard]] Task<bool> give(Handed what) override;

    [[nodiscard]] Task<std::optional<Handed>> taken() override;
};

}

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "tal/clipboard.hpp"
#include "tal/console.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"

namespace tal::clipboarding {

class Board {
public:
    virtual ~Board() = default;

    [[nodiscard]] virtual bool copy(std::string_view what) = 0;

    [[nodiscard]] virtual std::optional<std::optional<std::string>> paste() = 0;

    [[nodiscard]] virtual bool give(const Handed& what) = 0;

    [[nodiscard]] virtual std::optional<std::optional<Handed>> take() = 0;
};

[[nodiscard]] std::shared_ptr<Board> native();

[[nodiscard]] Port<Clipboard> clipboard(Port<Console> console, Port<Processes> processes, std::shared_ptr<Board> board);

}

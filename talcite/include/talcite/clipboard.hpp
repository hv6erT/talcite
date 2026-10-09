#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "talcite/console.hpp"
#include "talcite/seam.hpp"

namespace talcite {

class Clipboard : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "clipboard"; }

    [[nodiscard]] virtual bool copy(std::string_view what) = 0;

    [[nodiscard]] virtual std::optional<std::string> paste() = 0;
};

[[nodiscard]] Seam<Clipboard> clipboard();

[[nodiscard]] Seam<Clipboard> clipboard(Seam<Console> with);

}

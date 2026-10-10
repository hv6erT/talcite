#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/console.hpp"
#include "talcite/seam.hpp"

namespace talcite {

enum class Handing : std::uint8_t { copy, cut };

struct Files {
    std::vector<std::filesystem::path> paths;
    Handing how = Handing::copy;
};

class Clipboard : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "clipboard"; }

    [[nodiscard]] virtual bool copy(std::string_view what) = 0;

    [[nodiscard]] virtual std::optional<std::string> paste() = 0;

    [[nodiscard]] virtual bool give(const Files& what);

    [[nodiscard]] virtual std::optional<Files> taken();
};

[[nodiscard]] Seam<Clipboard> clipboard();

[[nodiscard]] Seam<Clipboard> clipboard(Seam<Console> with);

}

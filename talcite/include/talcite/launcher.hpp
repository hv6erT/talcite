#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "talcite/clipboard.hpp"
#include "talcite/console.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"

namespace talcite {

enum class Opened : std::uint8_t { opened, copied, refused };

class Launcher : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "launcher"; }

    [[nodiscard]] virtual Promise<Opened> open(std::string address) = 0;
};

[[nodiscard]] Seam<Launcher> launcher();

[[nodiscard]] Seam<Launcher> launcher(Seam<Console> with, Seam<Clipboard> copying);

}

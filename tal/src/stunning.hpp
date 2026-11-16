#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "tal/bytes.hpp"
#include "tal/net.hpp"

namespace tal::net::stunning {

using Number = std::array<std::byte, 12>;

struct Heard {
    Number number{};
    bool answer = false;
    bool error = false;
    int code = 0;
    std::string said;
    Address seen;
};

[[nodiscard]] Number numbered();

[[nodiscard]] Bytes asked(const Number& number);

[[nodiscard]] Bytes told(const Number& number, const Address& seen, bool xored = true);

[[nodiscard]] Bytes refused(const Number& number, int code, std::string_view said);

[[nodiscard]] std::optional<Heard> heard(const Bytes& what);

}

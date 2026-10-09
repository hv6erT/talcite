#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/udp.hpp"
#include "talcite/promise.hpp"

namespace talcite::net {

class StunClient {
public:
    enum class Mapping : std::uint8_t { unknown, alike, symmetric };

    struct Held;

    StunClient() = default;

    StunClient(Udp post, Patience patience);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    void add(Address server);

    [[nodiscard]] const std::vector<Address>& servers() const;

    [[nodiscard]] Promise<Address> seen() const;

    [[nodiscard]] Promise<Mapping> mapping() const;

    [[nodiscard]] Address last() const;

    [[nodiscard]] Mapping known() const;

private:
    std::shared_ptr<Held> held_;
};

}

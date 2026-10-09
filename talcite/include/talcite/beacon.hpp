#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"

namespace talcite::net {

struct Sighting {
    Address from;
    Bytes payload;

    friend bool operator==(const Sighting&, const Sighting&) = default;
};

class Beacon {
public:
    struct Held;

    static constexpr std::size_t widest_tag = 64;

    static constexpr std::size_t widest_payload = 1024;

    Beacon() = default;

    explicit Beacon(std::uint16_t port);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& mine() const;

    void offer(Bytes tag, Bytes payload) const;

    void withdraw(const Bytes& tag) const;

    void close();

    [[nodiscard]] static Promise<std::vector<Sighting>> sought(std::uint16_t port, Bytes tag, Patience patience,
                                                              std::vector<Address> to = {});

private:
    std::shared_ptr<Held> held_;
};

}

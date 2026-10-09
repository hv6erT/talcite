#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"

namespace talcite::net {

struct Datagram {
    Address from;
    Bytes bytes;
};

class Udp {
public:
    struct Held;

    Udp() = default;

    explicit Udp(Address where);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& mine() const;

    [[nodiscard]] Promise<void> send(Address to, Bytes what, Patience patience) const;

    [[nodiscard]] Promise<Datagram> receive(std::size_t most, Patience patience) const;

    void broadcast(bool given) const;

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

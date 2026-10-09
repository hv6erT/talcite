#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/udp.hpp"
#include "talcite/promise.hpp"

namespace talcite::net {

class Relay {
public:
    struct Held;

    Relay() = default;

    Relay(Udp post, Address server, std::string user = {}, std::string word = {});

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Promise<Address> open(Patience patience);

    [[nodiscard]] Address lent() const;

    [[nodiscard]] Address seen() const;

    [[nodiscard]] Promise<void> allow(Address peer, Patience patience);

    [[nodiscard]] Promise<void> send(Address to, Bytes what, Patience patience);

    [[nodiscard]] Promise<Datagram> receive(std::size_t most, Patience patience);

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

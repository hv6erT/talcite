#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/udp.hpp"
#include "talcite/promise.hpp"
#include "talcite/relay.hpp"
#include "talcite/stunclient.hpp"

namespace talcite::net {

struct Candidate {
    enum class Sort : std::uint8_t { here, seen, lent };

    Address where;
    Sort sort = Sort::here;

    friend bool operator==(const Candidate&, const Candidate&) = default;
};

class Peer {
public:
    struct Held;

    Peer() = default;

    explicit Peer(Udp post);

    Peer(Udp post, Relay relay);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Promise<std::vector<Candidate>> gather(StunClient scout, Patience patience);

    [[nodiscard]] std::vector<Candidate> candidates() const;

    void add(Candidate theirs);

    [[nodiscard]] std::vector<Candidate> theirs() const;

    [[nodiscard]] Promise<Address> reach(Patience patience);

    [[nodiscard]] Address met() const;

    [[nodiscard]] bool relayed() const;

    [[nodiscard]] Promise<void> send(Bytes what, Patience patience);

    [[nodiscard]] Promise<Bytes> receive(std::size_t most, Patience patience);

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

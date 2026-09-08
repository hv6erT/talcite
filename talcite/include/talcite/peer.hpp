#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/post.hpp"
#include "talcite/promise.hpp"
#include "talcite/relay.hpp"
#include "talcite/scout.hpp"

namespace talcite::net {

struct Way {
    enum class Sort : std::uint8_t { here, seen, lent };

    Address where;
    Sort sort = Sort::here;

    friend bool operator==(const Way&, const Way&) = default;
};

class Peer {
public:
    struct Held;

    Peer() = default;

    explicit Peer(Post post);

    Peer(Post post, Relay relay);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Promise<std::vector<Way>> gather(Scout scout, Patience patience);

    [[nodiscard]] std::vector<Way> ways() const;

    void add(Way theirs);

    [[nodiscard]] std::vector<Way> theirs() const;

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

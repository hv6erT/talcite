#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"

namespace talcite::net {

class Stream {
public:
    struct Held;

    Stream() = default;

    explicit Stream(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    [[nodiscard]] static Promise<Stream> connect(Address where, Patience patience);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& peer() const;

    [[nodiscard]] const Address& mine() const;

    [[nodiscard]] Promise<Bytes> read(std::size_t most, Patience patience) const;

    [[nodiscard]] Promise<void> write(Bytes what, Patience patience) const;

    void nagle(bool given) const;

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

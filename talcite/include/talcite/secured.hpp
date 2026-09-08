#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/secure.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

class Secured {
public:
    struct Held;

    Secured() = default;

    explicit Secured(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    [[nodiscard]] static Promise<Secured> client(Stream one, Guard guard, std::string host,
                                                 Patience patience);

    [[nodiscard]] static Promise<Secured> server(Stream one, Guard guard, Patience patience);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& peer() const;

    [[nodiscard]] const Address& mine() const;

    [[nodiscard]] Promise<Bytes> read(std::size_t most, Patience patience) const;

    [[nodiscard]] Promise<void> write(Bytes what, Patience patience) const;

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

#pragma once

#include <memory>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

class Gate {
public:
    struct Held;

    Gate() = default;

    explicit Gate(Address where, int waiting = 64);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& mine() const;

    [[nodiscard]] Promise<Stream> accept(Patience patience) const;

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

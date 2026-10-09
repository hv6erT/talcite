#pragma once

#include <memory>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/cipher.hpp"
#include "talcite/handshake.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

class Channel {
public:
    struct Held;

    Channel() = default;

    explicit Channel(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    [[nodiscard]] static Promise<Channel> opened(Stream one, std::shared_ptr<Handshake> how, Seam<CipherProvider> cipher,
                                                 Patience patience);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& peer() const;

    [[nodiscard]] Promise<void> send(Bytes what, Patience patience) const;

    [[nodiscard]] Promise<Bytes> receive(Patience patience) const;

    void close();

private:
    std::shared_ptr<Held> held_;
};

}

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "talcite/bytes.hpp"
#include "talcite/cipher.hpp"
#include "talcite/seam.hpp"

namespace talcite::net {

enum class Side : std::uint8_t { calling, answering };

struct Keying {
    Bytes sending;
    Bytes receiving;
};

class Handshake {
public:
    virtual ~Handshake() = default;

    Handshake(const Handshake&) = delete;
    Handshake& operator=(const Handshake&) = delete;
    Handshake(Handshake&&) = delete;
    Handshake& operator=(Handshake&&) = delete;

    [[nodiscard]] virtual std::optional<Bytes> said() = 0;

    virtual void heard(const Bytes& one) = 0;

    [[nodiscard]] virtual bool ready() const = 0;

    [[nodiscard]] virtual const Keying& keys() const = 0;

protected:
    Handshake() = default;
};

[[nodiscard]] std::shared_ptr<Handshake> keyed(Side side, Bytes key, std::string context, Seam<CipherProvider> cipher);

[[nodiscard]] std::shared_ptr<Handshake> cpace(Side side, Bytes password, std::string context,
                                               Seam<CipherProvider> cipher);

}

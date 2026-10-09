#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "talcite/bytes.hpp"
#include "talcite/seam.hpp"

namespace talcite {

class Agreement {
public:
    virtual ~Agreement() = default;

    Agreement(const Agreement&) = delete;
    Agreement& operator=(const Agreement&) = delete;
    Agreement(Agreement&&) = delete;
    Agreement& operator=(Agreement&&) = delete;

    [[nodiscard]] virtual Bytes mine() const = 0;

    [[nodiscard]] virtual std::optional<Bytes> shared(const Bytes& theirs) const = 0;

protected:
    Agreement() = default;
};

class CipherProvider : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "Cipher"; }

    [[nodiscard]] virtual std::unique_ptr<Agreement> agreement() = 0;

    [[nodiscard]] virtual std::unique_ptr<Agreement> agreement(const Bytes& secret) = 0;

    [[nodiscard]] virtual Bytes sealed(const Bytes& key, const Bytes& nonce, const Bytes& extra,
                                       const Bytes& plain) = 0;

    [[nodiscard]] virtual std::optional<Bytes> opened(const Bytes& key, const Bytes& nonce, const Bytes& extra,
                                                      const Bytes& sealed) = 0;
};

}

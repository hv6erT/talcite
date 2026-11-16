#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "tal/bytes.hpp"
#include "tal/port.hpp"

namespace tal::crypto {

enum class Sum { md5, sha1, sha256, sha512 };

class Summing {
public:
    explicit Summing(Sum which);

    Summing(Sum which, std::span<const std::byte> key);

    ~Summing();

    Summing(Summing&& other) noexcept;
    Summing& operator=(Summing&& other) noexcept;

    Summing(const Summing&) = delete;
    Summing& operator=(const Summing&) = delete;

    void feed(std::span<const std::byte> what);

    [[nodiscard]] Bytes done();

    struct Held;

private:
    std::unique_ptr<Held> held_;
};

[[nodiscard]] Bytes summed(Sum which, std::span<const std::byte> what);

[[nodiscard]] Bytes stamped(Sum which, std::span<const std::byte> key, std::span<const std::byte> what);

[[nodiscard]] Bytes derived(Sum which, std::span<const std::byte> salt, std::span<const std::byte> secret, std::span<const std::byte> info,
                            std::size_t much);

[[nodiscard]] Bytes random(std::size_t much);

[[nodiscard]] std::string base64(std::span<const std::byte> what);

[[nodiscard]] std::optional<Bytes> unbase64(std::string_view text);

void wiped(std::string& secret);

void wiped(Bytes& secret);

[[nodiscard]] bool same(std::span<const std::byte> one, std::span<const std::byte> other);

class Agreement {
public:
    Agreement() = default;
    virtual ~Agreement() = default;

    Agreement(const Agreement&) = delete;
    Agreement& operator=(const Agreement&) = delete;

    [[nodiscard]] virtual Bytes mine() const = 0;

    [[nodiscard]] virtual std::optional<Bytes> shared(const Bytes& theirs) const = 0;
};

class Cipher : public Adapter {
public:
    static constexpr std::string_view kind = "cipher";

    [[nodiscard]] static std::shared_ptr<Cipher> builtin();

    [[nodiscard]] virtual std::unique_ptr<Agreement> agreement() = 0;

    [[nodiscard]] virtual std::unique_ptr<Agreement> agreement(const Bytes& secret) = 0;

    [[nodiscard]] virtual Bytes sealed(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& plain) = 0;

    [[nodiscard]] virtual std::optional<Bytes> opened(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& sealed) = 0;
};

}

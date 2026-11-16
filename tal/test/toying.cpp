#include "toying.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

#include "tal/bytes.hpp"

// A cipher for the trials of what stands on the cipher's port, where the system gives none (Linux
// without an adapter): an agreement in the group of numbers modulo 2³² − 5, which is a real
// Diffie–Hellman with numbers far too small to keep anything, and a seal of a keystream from
// SHA-256 with an HMAC tag, which detects any change. Its point is the mechanics, never secrecy.
namespace tal::testing {

namespace {

constexpr std::uint64_t prime = 4294967291;

// `one` times `two` modulo the prime.
std::uint64_t times(std::uint64_t one, std::uint64_t two) { return one % prime * (two % prime) % prime; }

// `base` to the power `exponent` modulo the prime.
std::uint64_t power(std::uint64_t base, std::uint64_t exponent) {
    std::uint64_t made = 1;

    for (base %= prime; exponent != 0; exponent >>= 1) {
        if ((exponent & 1) != 0) made = times(made, base);

        base = times(base, base);
    }

    return made;
}

// The number of the first eight bytes of `given`, least significant first.
std::uint64_t number(const Bytes& given) {
    std::uint64_t made = 0;

    for (std::size_t at = 0; at < 8 && at < given.size(); ++at) made |= std::to_integer<std::uint64_t>(given[at]) << (8 * at);

    return made;
}

// The number `value` as 32 bytes, least significant first.
Bytes written(std::uint64_t value) {
    Bytes made(32);

    for (std::size_t at = 0; at < 8; ++at) made[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);

    return made;
}

// One side of the toy agreement: the exponent `secret`.
class Toy final : public crypto::Agreement {
public:
    // The exponent of `secret`, never zero.
    explicit Toy(const Bytes& secret) : secret_(number(secret) % (prime - 1) + 1) {}

    // Nine to the exponent.
    [[nodiscard]] Bytes mine() const override { return written(power(9, secret_)); }

    // `theirs` to the exponent; nothing for zero and for what has no 32 bytes.
    [[nodiscard]] std::optional<Bytes> shared(const Bytes& theirs) const override {
        if (theirs.size() != 32 || number(theirs) % prime == 0) return std::nullopt;

        return written(power(number(theirs), secret_));
    }

private:
    std::uint64_t secret_;
};

// The keystream of `key` and `nonce` as long as `much`.
Bytes streamed(const Bytes& key, const Bytes& nonce, std::size_t much) {
    Bytes made;

    for (std::uint32_t block = 0; made.size() < much; ++block) {
        Bytes input = key;

        input.insert(input.end(), nonce.begin(), nonce.end());

        for (int shift = 24; shift >= 0; shift -= 8) input.push_back(static_cast<std::byte>((block >> shift) & 0xff));

        const Bytes one = crypto::summed(crypto::Sum::sha256, input);

        made.insert(made.end(), one.begin(), one.end());
    }

    made.resize(much);

    return made;
}

// The tag of `body` under `key`, `nonce` and `extra`: 16 bytes of an HMAC.
Bytes tagged(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& body) {
    Bytes input = nonce;

    for (const Bytes* one : {&extra, &body}) {
        const auto much = static_cast<std::uint32_t>(one->size());

        for (int shift = 24; shift >= 0; shift -= 8) input.push_back(static_cast<std::byte>((much >> shift) & 0xff));

        input.insert(input.end(), one->begin(), one->end());
    }

    Bytes made = crypto::stamped(crypto::Sum::sha256, key, input);

    made.resize(16);

    return made;
}

// The toy cipher.
class Toying final : public crypto::Cipher {
public:
    // Its name.
    [[nodiscard]] std::string_view name() const override { return "toy"; }

    // A fresh exponent.
    [[nodiscard]] std::unique_ptr<crypto::Agreement> agreement() override { return std::make_unique<Toy>(crypto::random(32)); }

    // The exponent of `secret`.
    [[nodiscard]] std::unique_ptr<crypto::Agreement> agreement(const Bytes& secret) override { return std::make_unique<Toy>(secret); }

    // `plain` streamed and tagged.
    [[nodiscard]] Bytes sealed(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& plain) override {
        const Bytes stream = streamed(key, nonce, plain.size());
        Bytes made(plain.size());

        for (std::size_t at = 0; at < plain.size(); ++at) made[at] = plain[at] ^ stream[at];

        const Bytes tag = tagged(key, nonce, extra, made);

        made.insert(made.end(), tag.begin(), tag.end());

        return made;
    }

    // What `sealed` made, opened; nothing when anything was changed.
    [[nodiscard]] std::optional<Bytes> opened(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& sealed) override {
        if (sealed.size() < 16) return std::nullopt;

        const Bytes body(sealed.begin(), sealed.end() - 16);

        if (!crypto::same(tagged(key, nonce, extra, body), Bytes(sealed.end() - 16, sealed.end()))) return std::nullopt;

        const Bytes stream = streamed(key, nonce, body.size());
        Bytes made(body.size());

        for (std::size_t at = 0; at < body.size(); ++at) made[at] = body[at] ^ stream[at];

        return made;
    }
};

}

// The toy cipher.
std::shared_ptr<crypto::Cipher> toy() { return std::make_shared<Toying>(); }

}

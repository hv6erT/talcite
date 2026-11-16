#include "tal/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "tal/trouble.hpp"

#include "summing.hpp"

// The cryptography of tal that needs no port: digests, HMAC and HKDF, base64, wiping a secret and
// comparing secrets in constant time. Digests come from the system (`summing::system`); our own
// are the spare only where the programmer built them in (TAL_OWN_SUMS), and MD5 and SHA-1, broken
// for collisions, exist only on a second word (TAL_WEAK_SUMS). Without them each says so as
// unsupported.
namespace tal::crypto {

struct Summing::Held {
    std::unique_ptr<summing::Machine> machine;
    bool over = false;
};

namespace {

// The digest `which`, an HMAC with `key` when one is given: from the system, or from our own when
// the system gives none and they are built in. Throws `Trouble` (unsupported) for a weak digest
// not built in and when there are no digests at all.
std::unique_ptr<Summing::Held> started(Sum which, summing::Key key) {
#ifndef TAL_WEAK_SUMS
    if (which == Sum::md5 || which == Sum::sha1) throw Trouble{Kind::unsupported, "MD5 and SHA-1 are turned off (TAL_WEAK_SUMS)."};
#endif

    auto made = std::make_unique<Summing::Held>();

    made->machine = summing::system(which, key);

#ifdef TAL_OWN_SUMS
    if (made->machine == nullptr) made->machine = summing::own(which, key);
#endif

    if (made->machine == nullptr) throw Trouble{Kind::unsupported, "The system provides no digests (TAL_OWN_SUMS)."};

    return made;
}

}

// A digest of the kind `which`.
Summing::Summing(Sum which) : held_(started(which, std::nullopt)) {}

// An HMAC of the kind `which` with `key`.
Summing::Summing(Sum which, std::span<const std::byte> key) : held_(started(which, key)) {}

Summing::~Summing() = default;

Summing::Summing(Summing&& other) noexcept = default;

Summing& Summing::operator=(Summing&& other) noexcept = default;

// Takes in the next piece of the content; throws `Trouble` once the digest is done.
void Summing::feed(std::span<const std::byte> what) {
    if (held_ == nullptr || held_->over) throw Trouble{Kind::other, "The digest is done."};

    held_->machine->feed(what);
}

// The digest, once; nothing is taken in after it.
Bytes Summing::done() {
    if (held_ == nullptr || held_->over) throw Trouble{Kind::other, "The digest is done."};

    held_->over = true;

    return held_->machine->done();
}

// The digest `which` of `what`.
Bytes summed(Sum which, std::span<const std::byte> what) {
    Summing one{which};

    one.feed(what);

    return one.done();
}

// The HMAC `which` of `what` with `key`.
Bytes stamped(Sum which, std::span<const std::byte> key, std::span<const std::byte> what) {
    Summing one{which, key};

    one.feed(what);

    return one.done();
}

// `much` bytes of HKDF (RFC 5869) with the HMAC `which`: a key drawn from `secret` with `salt`,
// then stretched with `info`. An empty salt is the digest's width of zeros there, which HMAC takes
// as it takes no key at all. At most 255 digests' worth; the drawn key and the pieces are wiped.
Bytes derived(Sum which, std::span<const std::byte> salt, std::span<const std::byte> secret, std::span<const std::byte> info,
              std::size_t much) {
    const std::size_t width = summing::wide(which);

    if (much > 255 * width) throw Trouble{Kind::other, "HKDF gives at most 255 digests' worth."};

    Bytes key = stamped(which, salt, secret);
    Bytes made;
    Bytes last;

    for (std::uint8_t turn = 1; made.size() < much; ++turn) {
        Summing one{which, key};
        const std::byte counter{turn};

        one.feed(last);
        one.feed(info);
        one.feed(std::span<const std::byte>{&counter, 1});
        wiped(last);
        last = one.done();
        made.insert(made.end(), last.begin(), last.end());
    }

    wiped(last);
    wiped(key);

    std::fill(made.begin() + static_cast<std::ptrdiff_t>(much), made.end(), std::byte{0});
    made.resize(much);

    return made;
}

namespace {

constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::uint8_t outside = 0xff;

constexpr std::array<std::uint8_t, 256> values = [] {
    std::array<std::uint8_t, 256> made{};

    made.fill(outside);

    for (std::size_t at = 0; at < alphabet.size(); ++at) made[static_cast<unsigned char>(alphabet[at])] = static_cast<std::uint8_t>(at);

    return made;
}();

}

// `what` as base64 (RFC 4648): the standard alphabet, padded with `=` to whole groups of four.
std::string base64(std::span<const std::byte> what) {
    std::string made;
    std::size_t at = 0;

    made.reserve((what.size() + 2) / 3 * 4);

    for (; at + 3 <= what.size(); at += 3) {
        const std::uint32_t three = (std::to_integer<std::uint32_t>(what[at]) << 16) | (std::to_integer<std::uint32_t>(what[at + 1]) << 8) |
                                    std::to_integer<std::uint32_t>(what[at + 2]);

        for (int shift = 18; shift >= 0; shift -= 6) made += alphabet[(three >> shift) & 63];
    }

    const std::size_t rest = what.size() - at;

    if (rest == 0) return made;

    std::uint32_t tail = std::to_integer<std::uint32_t>(what[at]) << 16;

    if (rest == 2) tail |= std::to_integer<std::uint32_t>(what[at + 1]) << 8;

    made += alphabet[(tail >> 18) & 63];
    made += alphabet[(tail >> 12) & 63];
    made += rest == 2 ? alphabet[(tail >> 6) & 63] : '=';
    made += '=';

    return made;
}

// The bytes of strict base64 `text`: whole groups of four, padding only at the very end and at most
// two, no spaces or line ends, and no bits left over in the last group. Nothing for anything else;
// guessing what another meant is not ours.
std::optional<Bytes> unbase64(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;

    Bytes made;

    made.reserve(text.size() / 4 * 3);

    for (std::size_t at = 0; at < text.size(); at += 4) {
        std::size_t padded = 0;

        if (at + 4 == text.size() && text[at + 3] == '=') padded = text[at + 2] == '=' ? 2 : 1;

        std::uint32_t four = 0;

        for (std::size_t one = 0; one < 4; ++one) {
            const std::uint8_t value = one >= 4 - padded ? std::uint8_t{0} : values[static_cast<unsigned char>(text[at + one])];

            if (value == outside) return std::nullopt;

            four = (four << 6) | value;
        }

        if ((padded == 1 && (four & 0xff) != 0) || (padded == 2 && (four & 0xffff) != 0)) return std::nullopt;

        made.push_back(static_cast<std::byte>(four >> 16));

        if (padded < 2) made.push_back(static_cast<std::byte>(four >> 8));
        if (padded < 1) made.push_back(static_cast<std::byte>(four));
    }

    return made;
}

namespace {

// Zeroes the `much` units at `at` through a volatile pointer, so that the compiler cannot drop the
// stores as unread.
template <typename Unit>
void zeroed(Unit* at, std::size_t much) {
    volatile Unit* const each = at;

    for (std::size_t one = 0; one < much; ++one) each[one] = Unit{};
}

}

// Zeroes the secret `secret`; its length stays.
void wiped(std::string& secret) { zeroed(secret.data(), secret.size()); }

// Zeroes the secret `secret`; its length stays.
void wiped(Bytes& secret) { zeroed(secret.data(), secret.size()); }

// Whether `one` and `other` are equal, in a time that depends on their length only, never on where
// they differ; different lengths are unequal at once, since a length is no secret.
bool same(std::span<const std::byte> one, std::span<const std::byte> other) {
    if (one.size() != other.size()) return false;

    std::byte differ{0};

    for (std::size_t at = 0; at < one.size(); ++at) differ |= one[at] ^ other[at];

    return differ == std::byte{0};
}

#ifndef _WIN32

// No cipher of the system outside Windows; an adapter brings one (OpenSSL).
std::shared_ptr<Cipher> Cipher::builtin() { return nullptr; }

#endif

}

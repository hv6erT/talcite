#include "tal/testing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "talcite/bytes.hpp"
#include "talcite/x25519.hpp"

#include "curving.hpp"

using namespace talcite;

// X25519 in talcite: the Montgomery ladder used as a fallback when the system does not provide
// curve 25519 (CNG before Windows 10, Wine). The RFC 7748 vectors (§5.2, also iterated, and §6.1),
// small-order points with and without bit 255, non-canonical values and scalar clamping; and on top
// of that the edges of the field's canonical form. A trial in talcite-adapters compares the ladder
// with libcrypto on random inputs. The expected results for the edges were computed independently
// in Python.
namespace {

// Turns a hexadecimal string into bytes, two digits per byte.
Bytes lump(std::string_view hex) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(static_cast<std::byte>(std::stoi(std::string{hex.substr(at, 2)}, nullptr, 16)));

    return made;
}

// Spells the result as lowercase hexadecimal, or "none" when X25519 refused.
std::string spelled(const std::optional<Bytes>& one) {
    static constexpr char digits[] = "0123456789abcdef";

    if (!one) return "none";

    std::string made;

    for (const std::byte each : *one) {
        made += digits[static_cast<unsigned>(each) >> 4];
        made += digits[static_cast<unsigned>(each) & 15];
    }

    return made;
}

// The base point u = 9 as 32 little-endian bytes.
Bytes base() {
    Bytes made(32);

    made.front() = std::byte{9};

    return made;
}

const Bytes scalar = lump("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
const Bytes point = lump("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
const std::string product = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";

}

TRIAL("X25519: both vectors from RFC 7748 §5.2") {
    SAME(spelled(x25519(scalar, point)), product);
    SAME(spelled(x25519(lump("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
                        lump("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"))),
         std::string{"95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"});
}

// k = u = 9, then k ← X25519(k, u) and u ← the previous k.
TRIAL("X25519: the iterated vectors from RFC 7748 §5.2, once and a thousand times") {
    Bytes k = base();
    Bytes u = base();

    for (int round = 1; round <= 1000; ++round) {
        const std::optional<Bytes> next = x25519(k, u);

        OUGHT(next.has_value());

        if (!next) return;

        u = k;
        k = *next;

        if (round == 1)
            SAME(spelled(k), std::string{"422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"});
    }

    SAME(spelled(k), std::string{"684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"});
}

TRIAL("X25519: Alice and Bob from RFC 7748 §6.1") {
    const Bytes alice = lump("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes bob = lump("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    const std::optional<Bytes> alice_public = x25519(alice, base());
    const std::optional<Bytes> bob_public = x25519(bob, base());

    SAME(spelled(alice_public), std::string{"8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"});
    SAME(spelled(bob_public), std::string{"de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"});

    if (!alice_public || !bob_public) return;

    const std::string shared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

    SAME(spelled(x25519(alice, *bob_public)), shared);
    SAME(spelled(x25519(bob, *alice_public)), shared);
}

// Seven small-order points (0, 1, two of order 8, p − 1, p, p + 1), each also with bit 255 set; a
// zero result is a refusal.
TRIAL("X25519: a small-order point is refused, with and without bit 255") {
    const std::string_view low[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };

    for (const std::string_view one : low) {
        Bytes u = lump(one);

        SAME(spelled(x25519(scalar, u)), std::string{"none"});

        u.back() |= std::byte{0x80};

        SAME(spelled(x25519(scalar, u)), std::string{"none"});
    }
}

// Bit 255 of the coordinate is ignored, and u from p upward is treated as reduced: p + 9 is 9.
TRIAL("X25519: bit 255 ignored, a non-canonical u reduced") {
    Bytes high = point;

    high.back() |= std::byte{0x80};

    SAME(spelled(x25519(scalar, high)), product);

    Bytes past = lump("f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");

    SAME(spelled(x25519(scalar, past)), spelled(x25519(scalar, base())));
}

// Clamping from the RFC: bits 0–2 and 255 of the scalar mean nothing, bit 254 is always one.
TRIAL("X25519: the scalar is clamped as in RFC 7748") {
    Bytes flipped = scalar;

    flipped.front() ^= std::byte{0x07};
    flipped.back() ^= std::byte{0xc0};

    SAME(spelled(x25519(flipped, point)), product);

    Bytes other = scalar;

    other.front() ^= std::byte{0x08};

    OUGHT(spelled(x25519(other, point)) != product);
}

// The edges of `packed`: 2²⁵⁶ − 1 lies above 2p, so it needs two subtractions of p; an excess in
// the last limb that, after wrapping around, passes through fifteen full limbs needs a third carry;
// −1 is p − 1.
TRIAL("X25519: the field gives the canonical form also at the edges") {
    curving::Limbs full{};

    full.fill(0xffff);

    SAME(spelled(curving::packed(full)), std::string{"2500000000000000000000000000000000000000000000000000000000000000"});

    curving::Limbs around = full;

    around[15] += std::int64_t{1724} << 16;

    SAME(spelled(curving::packed(around)), std::string{"0d00010000000000000000000000000000000000000000000000000000000000"});

    curving::Limbs below{};

    below[0] = -1;

    SAME(spelled(curving::packed(below)), std::string{"ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"});
}

TRIAL("X25519: a wrong scalar size throws, a wrong u size is a refusal") {
    THREW(static_cast<void>(x25519(Bytes(31), point)));
    THREW(static_cast<void>(x25519(Bytes(33), point)));

    SAME(spelled(x25519(scalar, Bytes(31))), std::string{"none"});
    SAME(spelled(x25519(scalar, Bytes(33))), std::string{"none"});
    SAME(spelled(x25519(scalar, Bytes{})), std::string{"none"});
}

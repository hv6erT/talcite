#include "deflating.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Our own deflate (RFC 1951) in a zlib wrapper (RFC 1950), for what tal writes compressed: the
// thumbnails it keeps as PNG. One block of the fixed Huffman codes, which need no table in the
// stream, and matches of LZ77 found through chains of a hash of three bytes over a window of
// 32 KiB, each chain followed at most `longest_chain` links, so the time is linear in the input
// whatever it holds. It is checked by our own `Inflater`, which takes back what it gives.
namespace tal::deflating {

namespace {

constexpr std::size_t window = 32768;
constexpr std::size_t shortest = 3;
constexpr std::size_t longest = 258;
constexpr std::size_t longest_chain = 64;
constexpr std::size_t hash_bits = 15;

// The first length of each length code (257 to 285) and how many extra bits follow it.
constexpr std::array<std::uint16_t, 29> length_base{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

// The first distance of each distance code (0 to 29) and how many extra bits follow it.
constexpr std::array<std::uint16_t, 30> distance_base{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// A byte as an unsigned number.
[[nodiscard]] constexpr std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// Bits written as deflate packs them: from the lowest bit of each byte up.
class Bits {
public:
    // Appends the `many` lowest bits of `value`, the lowest first.
    void put(std::uint32_t value, int many) {
        for (int one = 0; one < many; ++one) {
            if (filled_ == 0) made_.push_back(std::byte{0});

            if (((value >> one) & 1u) != 0) made_.back() |= static_cast<std::byte>(1u << filled_);

            filled_ = (filled_ + 1) % 8;
        }
    }

    // Appends a Huffman code of `many` bits, whose highest bit goes first.
    void code(std::uint32_t value, int many) {
        for (int one = many - 1; one >= 0; --one) put((value >> one) & 1u, 1);
    }

    // The bytes, the last filled up with zeros.
    [[nodiscard]] Bytes done() { return std::move(made_); }

private:
    Bytes made_;
    int filled_ = 0;
};

// Writes a literal or length symbol (0 to 287) in the fixed code.
void symbol(Bits& out, std::uint32_t one) {
    if (one < 144) out.code(0x30 + one, 8);
    else if (one < 256) out.code(0x190 + one - 144, 9);
    else if (one < 280) out.code(one - 256, 7);
    else out.code(0xC0 + one - 280, 8);
}

// Writes a match of `length` bytes `distance` back: the length code and its extra bits, then the
// distance code (five bits in the fixed code) and its extra bits.
void matched(Bits& out, std::size_t length, std::size_t distance) {
    std::size_t code = length_base.size() - 1;

    while (length_base[code] > length) --code;

    symbol(out, static_cast<std::uint32_t>(257 + code));
    out.put(static_cast<std::uint32_t>(length - length_base[code]), length_extra[code]);

    std::size_t far = distance_base.size() - 1;

    while (distance_base[far] > distance) --far;

    out.code(static_cast<std::uint32_t>(far), 5);
    out.put(static_cast<std::uint32_t>(distance - distance_base[far]), distance_extra[far]);
}

// The hash of the three bytes at `at`; the caller made sure they are there.
[[nodiscard]] std::size_t hashed(std::span<const std::byte> given, std::size_t at) {
    const std::uint32_t three = (sample(given[at]) << 16) | (sample(given[at + 1]) << 8) | sample(given[at + 2]);

    return (three * 2654435761u) >> (32 - hash_bits);
}

// The Adler-32 of the bytes, the check of zlib.
[[nodiscard]] std::uint32_t adler32(std::span<const std::byte> given) {
    std::uint32_t low = 1;
    std::uint32_t high = 0;

    for (const std::byte one : given) {
        low = (low + sample(one)) % 65521;
        high = (high + low) % 65521;
    }

    return (high << 16) | low;
}

}

// The bytes compressed into a zlib stream: its header (deflate, window of 32 KiB), one final
// block of the fixed codes with the longest match each chain gives at every place, or a literal
// where none of three bytes is there, and the Adler-32 of the bytes.
Bytes deflated(std::span<const std::byte> given) {
    Bits out;

    out.put(0x78, 8);
    out.put(0x01, 8);
    out.put(1, 1);
    out.put(1, 2);

    std::vector<std::int64_t> head(std::size_t{1} << hash_bits, -1);
    std::vector<std::int64_t> previous(window, -1);

    const auto remembered = [&](std::size_t at) {
        if (at + shortest > given.size()) return;

        const std::size_t key = hashed(given, at);

        previous[at % window] = head[key];
        head[key] = static_cast<std::int64_t>(at);
    };

    std::size_t at = 0;

    while (at < given.size()) {
        std::size_t best = 0;
        std::size_t best_distance = 0;

        if (at + shortest <= given.size()) {
            std::int64_t candidate = head[hashed(given, at)];
            const std::size_t most = std::min(longest, given.size() - at);

            for (std::size_t links = 0; candidate >= 0 && links < longest_chain; ++links) {
                const auto from = static_cast<std::size_t>(candidate);

                if (at - from > window) break;

                std::size_t length = 0;

                while (length < most && given[from + length] == given[at + length]) ++length;

                if (length > best) {
                    best = length;
                    best_distance = at - from;

                    if (length == most) break;
                }

                const std::int64_t next = previous[from % window];

                if (next >= candidate) break;

                candidate = next;
            }
        }

        if (best >= shortest) {
            matched(out, best, best_distance);

            for (std::size_t one = 0; one < best; ++one) remembered(at + one);

            at += best;
        } else {
            symbol(out, sample(given[at]));
            remembered(at);
            ++at;
        }
    }

    symbol(out, 256);

    Bytes made = out.done();
    const std::uint32_t check = adler32(given);

    for (int shift = 24; shift >= 0; shift -= 8) made.push_back(static_cast<std::byte>((check >> shift) & 0xFF));

    return made;
}

}

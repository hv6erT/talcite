#include "summing.hpp"

#ifdef TAL_OWN_SUMS

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "tal/trouble.hpp"

// Our own digests, the spare for a system that gives none (a container whose filter blocks the
// kernel's AF_ALG), built only on the programmer's word (TAL_OWN_SUMS), and MD5 and SHA-1 only on
// a second one (TAL_WEAK_SUMS). They follow FIPS 180-4 and RFC 1321 to the bit; no branch and no
// table lookup depends on what is digested, so the time tells nothing of it. HMAC (RFC 2104)
// stands on any of them.
namespace tal::crypto::summing {

namespace {

// The 32-bit word `value` turned right by `much` bits.
constexpr std::uint32_t right(std::uint32_t value, int much) { return (value >> much) | (value << (32 - much)); }

// The 64-bit word `value` turned right by `much` bits.
constexpr std::uint64_t right64(std::uint64_t value, int much) { return (value >> much) | (value << (64 - much)); }

// The big-endian 32-bit word at `at`.
std::uint32_t big32(const std::byte* at) {
    std::uint32_t made = 0;

    for (int one = 0; one < 4; ++one) made = (made << 8) | std::to_integer<std::uint32_t>(at[one]);

    return made;
}

// The big-endian 64-bit word at `at`.
std::uint64_t big64(const std::byte* at) {
    std::uint64_t made = 0;

    for (int one = 0; one < 8; ++one) made = (made << 8) | std::to_integer<std::uint64_t>(at[one]);

    return made;
}

// Appends the 32-bit word `value` to `into`, big-endian or little-endian (`big`).
void put32(Bytes& into, std::uint32_t value, bool big) {
    for (int one = 0; one < 4; ++one) into.push_back(static_cast<std::byte>(value >> (big ? 24 - 8 * one : 8 * one)));
}

// Appends the 64-bit word `value` to `into`, big-endian.
void put64(Bytes& into, std::uint64_t value) {
    for (int one = 0; one < 8; ++one) into.push_back(static_cast<std::byte>(value >> (56 - 8 * one)));
}

// SHA-256 (FIPS 180-4, 6.2).
struct Sha256 {
    static constexpr std::size_t block = 64;
    static constexpr std::size_t lengthed = 8;
    static constexpr bool big = true;

    static constexpr std::array<std::uint32_t, 64> rounds{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // Takes in the block at `at`.
    void compressed(const std::byte* at) {
        std::array<std::uint32_t, 64> w{};

        for (std::size_t one = 0; one < 16; ++one) w[one] = big32(at + 4 * one);

        for (std::size_t one = 16; one < 64; ++one) {
            const std::uint32_t low = right(w[one - 15], 7) ^ right(w[one - 15], 18) ^ (w[one - 15] >> 3);
            const std::uint32_t high = right(w[one - 2], 17) ^ right(w[one - 2], 19) ^ (w[one - 2] >> 10);

            w[one] = w[one - 16] + low + w[one - 7] + high;
        }

        std::array<std::uint32_t, 8> v = state;

        for (std::size_t one = 0; one < 64; ++one) {
            const std::uint32_t upper = right(v[4], 6) ^ right(v[4], 11) ^ right(v[4], 25);
            const std::uint32_t chosen = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t first = v[7] + upper + chosen + rounds[one] + w[one];
            const std::uint32_t lower = right(v[0], 2) ^ right(v[0], 13) ^ right(v[0], 22);
            const std::uint32_t most = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);

            v = {first + lower + most, v[0], v[1], v[2], v[3] + first, v[4], v[5], v[6]};
        }

        for (std::size_t one = 0; one < 8; ++one) state[one] += v[one];
    }

    // The digest.
    [[nodiscard]] Bytes out() const {
        Bytes made;

        for (const std::uint32_t one : state) put32(made, one, true);

        return made;
    }
};

// SHA-512 (FIPS 180-4, 6.4).
struct Sha512 {
    static constexpr std::size_t block = 128;
    static constexpr std::size_t lengthed = 16;
    static constexpr bool big = true;

    static constexpr std::array<std::uint64_t, 80> rounds{
        0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b,
        0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1,
        0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275,
        0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
        0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
        0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
        0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8,
        0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
        0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec, 0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915,
        0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
        0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6,
        0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

    std::array<std::uint64_t, 8> state{0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                                       0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};

    // Takes in the block at `at`.
    void compressed(const std::byte* at) {
        std::array<std::uint64_t, 80> w{};

        for (std::size_t one = 0; one < 16; ++one) w[one] = big64(at + 8 * one);

        for (std::size_t one = 16; one < 80; ++one) {
            const std::uint64_t low = right64(w[one - 15], 1) ^ right64(w[one - 15], 8) ^ (w[one - 15] >> 7);
            const std::uint64_t high = right64(w[one - 2], 19) ^ right64(w[one - 2], 61) ^ (w[one - 2] >> 6);

            w[one] = w[one - 16] + low + w[one - 7] + high;
        }

        std::array<std::uint64_t, 8> v = state;

        for (std::size_t one = 0; one < 80; ++one) {
            const std::uint64_t upper = right64(v[4], 14) ^ right64(v[4], 18) ^ right64(v[4], 41);
            const std::uint64_t chosen = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint64_t first = v[7] + upper + chosen + rounds[one] + w[one];
            const std::uint64_t lower = right64(v[0], 28) ^ right64(v[0], 34) ^ right64(v[0], 39);
            const std::uint64_t most = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);

            v = {first + lower + most, v[0], v[1], v[2], v[3] + first, v[4], v[5], v[6]};
        }

        for (std::size_t one = 0; one < 8; ++one) state[one] += v[one];
    }

    // The digest.
    [[nodiscard]] Bytes out() const {
        Bytes made;

        for (const std::uint64_t one : state) put64(made, one);

        return made;
    }
};

#ifdef TAL_WEAK_SUMS

// The 32-bit word `value` turned left by `much` bits.
constexpr std::uint32_t turned(std::uint32_t value, int much) { return (value << much) | (value >> (32 - much)); }

// SHA-1 (FIPS 180-4, 6.1).
struct Sha1 {
    static constexpr std::size_t block = 64;
    static constexpr std::size_t lengthed = 8;
    static constexpr bool big = true;

    std::array<std::uint32_t, 5> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

    // Takes in the block at `at`.
    void compressed(const std::byte* at) {
        std::array<std::uint32_t, 80> w{};

        for (std::size_t one = 0; one < 16; ++one) w[one] = big32(at + 4 * one);

        for (std::size_t one = 16; one < 80; ++one) w[one] = turned(w[one - 3] ^ w[one - 8] ^ w[one - 14] ^ w[one - 16], 1);

        std::array<std::uint32_t, 5> v = state;

        for (std::size_t one = 0; one < 80; ++one) {
            std::uint32_t mixed = 0;
            std::uint32_t added = 0;

            if (one < 20) {
                mixed = (v[1] & v[2]) | (~v[1] & v[3]);
                added = 0x5a827999;
            } else if (one < 40) {
                mixed = v[1] ^ v[2] ^ v[3];
                added = 0x6ed9eba1;
            } else if (one < 60) {
                mixed = (v[1] & v[2]) | (v[1] & v[3]) | (v[2] & v[3]);
                added = 0x8f1bbcdc;
            } else {
                mixed = v[1] ^ v[2] ^ v[3];
                added = 0xca62c1d6;
            }

            v = {turned(v[0], 5) + mixed + v[4] + added + w[one], v[0], turned(v[1], 30), v[2], v[3]};
        }

        for (std::size_t one = 0; one < 5; ++one) state[one] += v[one];
    }

    // The digest.
    [[nodiscard]] Bytes out() const {
        Bytes made;

        for (const std::uint32_t one : state) put32(made, one, true);

        return made;
    }
};

// MD5 (RFC 1321): words little-endian, the length too.
struct Md5 {
    static constexpr std::size_t block = 64;
    static constexpr std::size_t lengthed = 8;
    static constexpr bool big = false;

    static constexpr std::array<std::uint32_t, 64> sines{
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    static constexpr std::array<int, 16> shifts{7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

    std::array<std::uint32_t, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};

    // Takes in the block at `at`.
    void compressed(const std::byte* at) {
        std::array<std::uint32_t, 16> w{};

        for (std::size_t one = 0; one < 16; ++one)
            for (int byte = 3; byte >= 0; --byte) w[one] = (w[one] << 8) | std::to_integer<std::uint32_t>(at[4 * one + static_cast<std::size_t>(byte)]);

        std::array<std::uint32_t, 4> v = state;

        for (std::size_t one = 0; one < 64; ++one) {
            const std::size_t round = one / 16;
            std::uint32_t mixed = 0;
            std::size_t word = 0;

            if (round == 0) {
                mixed = (v[1] & v[2]) | (~v[1] & v[3]);
                word = one;
            } else if (round == 1) {
                mixed = (v[3] & v[1]) | (~v[3] & v[2]);
                word = (5 * one + 1) % 16;
            } else if (round == 2) {
                mixed = v[1] ^ v[2] ^ v[3];
                word = (3 * one + 5) % 16;
            } else {
                mixed = v[2] ^ (v[1] | ~v[3]);
                word = (7 * one) % 16;
            }

            const std::uint32_t moved = v[1] + turned(v[0] + mixed + sines[one] + w[word], shifts[4 * round + one % 4]);

            v = {v[3], moved, v[1], v[2]};
        }

        for (std::size_t one = 0; one < 4; ++one) state[one] += v[one];
    }

    // The digest.
    [[nodiscard]] Bytes out() const {
        Bytes made;

        for (const std::uint32_t one : state) put32(made, one, false);

        return made;
    }
};

#endif

// A digest of `Core`, taking the content in pieces of any size and padding it at the end: a one
// bit, zeros, and the length in bits in the last bytes of the last block.
template <typename Core>
class Padded final : public Machine {
public:
    // Takes in `what`, block by block.
    void feed(std::span<const std::byte> what) override {
        length_ += what.size();

        while (!what.empty()) {
            const std::size_t much = std::min(what.size(), Core::block - filled_);

            std::memcpy(buffer_.data() + filled_, what.data(), much);
            filled_ += much;
            what = what.subspan(much);

            if (filled_ == Core::block) {
                core_.compressed(buffer_.data());
                filled_ = 0;
            }
        }
    }

    // Pads the content and gives the digest.
    Bytes done() override {
        const std::uint64_t bits = length_ * 8;
        const std::uint64_t over = Core::lengthed > 8 ? length_ >> 61 : 0;
        std::array<std::byte, Core::block * 2> tail{};
        const std::size_t room = filled_ + 1 + Core::lengthed <= Core::block ? Core::block : 2 * Core::block;

        std::memcpy(tail.data(), buffer_.data(), filled_);
        tail[filled_] = std::byte{0x80};

        for (std::size_t one = 0; one < 8; ++one) {
            const auto shift = static_cast<int>(8 * one);
            const std::size_t at = Core::big ? room - 1 - one : room - 8 + one;

            tail[at] = static_cast<std::byte>(bits >> shift);

            if (Core::lengthed > 8) tail[room - 9 - one] = static_cast<std::byte>(over >> shift);
        }

        for (std::size_t at = 0; at < room; at += Core::block) core_.compressed(tail.data() + at);

        return core_.out();
    }

private:
    Core core_;
    std::array<std::byte, Core::block> buffer_{};
    std::size_t filled_ = 0;
    std::uint64_t length_ = 0;
};

// A plain digest of the kind `which`.
std::unique_ptr<Machine> plain(Sum which) {
    switch (which) {
        case Sum::sha256: return std::make_unique<Padded<Sha256>>();
        case Sum::sha512: return std::make_unique<Padded<Sha512>>();
#ifdef TAL_WEAK_SUMS
        case Sum::sha1: return std::make_unique<Padded<Sha1>>();
        case Sum::md5: return std::make_unique<Padded<Md5>>();
#endif
        default: throw Trouble{Kind::unsupported, "This digest is turned off (TAL_WEAK_SUMS)."};
    }
}

// HMAC (RFC 2104) of the kind `which`: the key, digested first when longer than a block, padded
// with zeros, mixed into the inner and the outer digest; the pads are wiped once taken in.
class Keyed final : public Machine {
public:
    // An HMAC with `key`.
    Keyed(Sum which, std::span<const std::byte> key) : inner_(plain(which)), outer_(plain(which)) {
        const std::size_t block = which == Sum::sha512 ? 128 : 64;
        Bytes padded(block);

        if (key.size() > block) {
            Bytes short_key = summed(which, key);

            std::copy(short_key.begin(), short_key.end(), padded.begin());
            wiped(short_key);
        } else {
            std::copy(key.begin(), key.end(), padded.begin());
        }

        for (std::byte& one : padded) one ^= std::byte{0x36};

        inner_->feed(padded);

        for (std::byte& one : padded) one ^= std::byte{0x36 ^ 0x5c};

        outer_->feed(padded);
        wiped(padded);
    }

    // Takes in `what`.
    void feed(std::span<const std::byte> what) override { inner_->feed(what); }

    // The stamp.
    Bytes done() override {
        outer_->feed(inner_->done());

        return outer_->done();
    }

private:
    // The digest of `what` alone.
    static Bytes summed(Sum which, std::span<const std::byte> what) {
        const std::unique_ptr<Machine> one = plain(which);

        one->feed(what);

        return one->done();
    }

    std::unique_ptr<Machine> inner_;
    std::unique_ptr<Machine> outer_;
};

}

// Our own digest of the kind `which`, an HMAC with `key` when one is given.
std::unique_ptr<Machine> own(Sum which, Key key) {
    if (key.has_value()) return std::make_unique<Keyed>(which, *key);

    return plain(which);
}

}

#endif

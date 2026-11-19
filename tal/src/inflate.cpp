#include "inflating.hpp"

#include <algorithm>
#include <string>

#include "tal/trouble.hpp"

// Inflate (RFC 1951) and the gzip around it (RFC 1952), our own: Windows gives no public deflate,
// and archives come from strangers, so the reader is written to meet hostile bytes. It takes its
// input in pieces, as a file is read, and gives out what each piece decodes to. Every step is one
// header or one symbol, read whole before it changes anything: when the input ends inside a step,
// the step is taken back and waits for more. The codes are checked as zlib checks them: a code
// over-subscribed, a code of the code lengths not complete, a code of lengths or distances not
// complete beyond a single code, a repeat with nothing before it or past the end, a block without
// the code of its end, a distance back before the start: each is malformed. A gzip member ends
// with its CRC-32 and size checked, and members may follow one another; after the last, nothing
// may follow. Decoding walks the canonical code bit by bit, as Mark Adler's puff does.
namespace tal::inflating {

namespace {

// The window of deflate: how far back a distance may reach.
constexpr std::size_t window = 32768;

// The longest name or comment in a gzip header.
constexpr std::size_t longest_named = 65536;

constexpr std::array<std::uint16_t, 29> length_base{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> length_extra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> distance_base{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> distance_extra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The order in which the lengths of the code of code lengths come.
constexpr std::array<std::uint8_t, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Throws `Trouble` (malformed) saying `why`.
[[noreturn]] void broken(const std::string& why) { throw Trouble{Kind::malformed, "The compressed data is damaged: " + why + "."}; }

// The table of the CRC-32 of zip and gzip (the reflected polynomial 0xEDB88320), made once.
const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> made = [] {
        std::array<std::uint32_t, 256> table{};

        for (std::uint32_t at = 0; at < 256; ++at) {
            std::uint32_t one = at;

            for (int bit = 0; bit < 8; ++bit) one = (one & 1U) != 0 ? 0xEDB88320U ^ (one >> 1) : one >> 1;

            table[at] = one;
        }

        return table;
    }();

    return made;
}

}

// The CRC-32 of `bytes` continued from `crc`; zero starts a new one.
std::uint32_t crc32(std::uint32_t crc, std::span<const std::byte> bytes) {
    const std::array<std::uint32_t, 256>& table = crc_table();
    std::uint32_t made = ~crc;

    for (const std::byte one : bytes) made = table[(made ^ std::to_integer<std::uint32_t>(one)) & 0xFFU] ^ (made >> 8);

    return ~made;
}

namespace {

// The canonical code of the `lengths` of its symbols: how many codes of each length (`counts`),
// the symbols in the order of their codes, and the longest length. How many codes of the full
// tree are left unused: zero for a complete code, below zero for one over-subscribed.
int built(std::array<std::uint16_t, 16>& counts, std::vector<std::uint16_t>& symbols, std::size_t& longest, std::span<const std::uint16_t> lengths) {
    counts.fill(0);
    longest = 0;

    for (const std::uint16_t one : lengths) {
        ++counts[one];
        longest = std::max<std::size_t>(longest, one);
    }

    if (counts[0] == lengths.size()) {
        symbols.clear();

        return 0;
    }

    int left = 1;

    for (std::size_t length = 1; length < 16; ++length) {
        left <<= 1;
        left -= counts[length];

        if (left < 0) return left;
    }

    std::array<std::uint16_t, 16> offsets{};

    for (std::size_t length = 1; length < 15; ++length) offsets[length + 1] = static_cast<std::uint16_t>(offsets[length] + counts[length]);

    symbols.assign(lengths.size(), 0);

    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol] != 0) symbols[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);

    return left;
}

}

// An inflater of raw deflate or of gzip.
Inflater::Inflater(Wrapping how) : how_(how), stage_(how == Wrapping::gzip ? Stage::header : Stage::block) {}

// What `given` decodes to, after what came before. Throws `Trouble` (malformed) for damage and for
// anything after the end; the inflater is of no use then.
Bytes Inflater::fed(std::span<const std::byte> given) {
    Bytes out;

    in_.insert(in_.end(), given.begin(), given.end());

    for (;;) {
        if (stage_ == Stage::done) {
            if (bit_ != 0) {
                bit_ = 0;
                ++at_;
            }

            if (at_ >= in_.size()) break;

            if (how_ != Wrapping::gzip) broken("something follows the end");

            stage_ = Stage::header;
        }

        const std::size_t at = at_;
        const int bit = bit_;

        try {
            step(out);
        } catch (const Short&) {
            at_ = at;
            bit_ = bit;

            break;
        }
    }

    if (at_ > 65536 && at_ * 2 > in_.size()) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(at_));
        at_ = 0;
    }

    return out;
}

// Whether the stream has ended: the last block of raw deflate, or a gzip member's trailer.
bool Inflater::ended() const { return stage_ == Stage::done; }

// One step of the stage the inflater is in.
void Inflater::step(Bytes& out) {
    switch (stage_) {
        case Stage::header: headed(); break;
        case Stage::block: blocked(); break;
        case Stage::stored: stored(out); break;
        case Stage::coded: coded(out); break;
        case Stage::trailer: trailed(); break;
        case Stage::done: break;
    }
}

// The next `many` bits, the first the lowest; throws `Short` when the input ends first.
std::uint32_t Inflater::bits(int many) {
    std::uint32_t made = 0;

    for (int got = 0; got < many;) {
        if (at_ >= in_.size()) throw Short{};

        const int take = std::min(8 - bit_, many - got);

        made |= ((std::to_integer<std::uint32_t>(in_[at_]) >> bit_) & ((1U << take) - 1U)) << got;
        got += take;
        bit_ += take;

        if (bit_ == 8) {
            bit_ = 0;
            ++at_;
        }
    }

    return made;
}

// The next symbol of `table`, its code read bit by bit, the first bit the highest of the code; a
// code longer than the table's longest is broken at once, as zlib finds it, not after 15 bits.
int Inflater::decoded(const Table& table) {
    int code = 0;
    int first = 0;
    int index = 0;

    for (std::size_t length = 1; length <= table.longest; ++length) {
        code |= static_cast<int>(bits(1));

        const int count = table.counts[length];

        if (code - count < first) return table.symbols[static_cast<std::size_t>(index + (code - first))];

        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    broken("a code that the table does not hold");
}

// A gzip member's header: its magic, deflate as the method, no reserved flags, and the extra
// field, the name, the comment and the header's CRC skipped when there.
void Inflater::headed() {
    const auto byte = [this] { return bits(8); };

    if (byte() != 0x1F || byte() != 0x8B) broken("not gzip");

    if (byte() != 8) broken("a gzip method other than deflate");

    const std::uint32_t flags = byte();

    if ((flags & 0xE0U) != 0) broken("reserved gzip flags");

    for (int skip = 0; skip < 6; ++skip) (void)byte();

    if ((flags & 0x04U) != 0) {
        const std::uint32_t much = byte() | (byte() << 8);

        for (std::uint32_t skip = 0; skip < much; ++skip) (void)byte();
    }

    for (const std::uint32_t flag : {0x08U, 0x10U}) {
        if ((flags & flag) == 0) continue;

        for (std::size_t taken = 0; byte() != 0; ++taken)
            if (taken >= longest_named) broken("a gzip name or comment too long");
    }

    if ((flags & 0x02U) != 0) (void)bits(16);

    stage_ = Stage::block;
    history_.clear();
    emitted_ = 0;
    crc_ = 0xFFFFFFFFU;
    size_ = 0;
    last_ = false;
}

// A block's header: stored with its length checked, or the fixed code, or the dynamic code read
// and checked.
void Inflater::blocked() {
    const bool last = bits(1) == 1;
    const std::uint32_t type = bits(2);

    if (type == 0) {
        if (bit_ != 0) {
            bit_ = 0;
            ++at_;
        }

        const std::uint32_t length = bits(16);

        if ((length ^ 0xFFFFU) != bits(16)) broken("a stored block's length and its complement differ");

        left_ = length;
        last_ = last;
        stage_ = Stage::stored;

        return;
    }

    if (type == 3) broken("a block of the reserved type");

    std::array<std::uint16_t, 320> lengths{};
    std::size_t literals = 288;
    std::size_t distances = 32;

    if (type == 1) {
        std::fill(lengths.begin(), lengths.begin() + 144, 8);
        std::fill(lengths.begin() + 144, lengths.begin() + 256, 9);
        std::fill(lengths.begin() + 256, lengths.begin() + 280, 7);
        std::fill(lengths.begin() + 280, lengths.begin() + 288, 8);
        std::fill(lengths.begin() + 288, lengths.begin() + 320, 5);
    } else {
        literals = bits(5) + 257;
        distances = bits(5) + 1;

        const std::uint32_t coding = bits(4) + 4;

        if (literals > 286 || distances > 30) broken("too many codes");

        std::array<std::uint16_t, 19> coded{};

        for (std::uint32_t at = 0; at < coding; ++at) coded[order[at]] = static_cast<std::uint16_t>(bits(3));

        Table lengthy;

        if (built(lengthy.counts, lengthy.symbols, lengthy.longest, coded) != 0) broken("the code of code lengths is not complete");

        for (std::size_t at = 0; at < literals + distances;) {
            const int symbol = decoded(lengthy);

            if (symbol < 16) {
                lengths[at++] = static_cast<std::uint16_t>(symbol);

                continue;
            }

            std::uint16_t repeated = 0;
            std::uint32_t times = 0;

            if (symbol == 16) {
                if (at == 0) broken("a repeat with no length before it");

                repeated = lengths[at - 1];
                times = 3 + bits(2);
            } else {
                times = symbol == 17 ? 3 + bits(3) : 11 + bits(7);
            }

            if (at + times > literals + distances) broken("a repeat past the last length");

            for (std::uint32_t one = 0; one < times; ++one) lengths[at++] = repeated;
        }

        if (lengths[256] == 0) broken("a block without the code of its end");
    }

    const std::span<const std::uint16_t> all{lengths};
    Table literal;
    Table distance;

    if (const int left = built(literal.counts, literal.symbols, literal.longest, all.subspan(0, literals));
        left < 0 || (left > 0 && type == 2 && literals != std::size_t{literal.counts[0]} + literal.counts[1]))
        broken("the code of lengths is not complete");

    if (const int left = built(distance.counts, distance.symbols, distance.longest, all.subspan(type == 1 ? 288 : literals, distances));
        left < 0 || (left > 0 && type == 2 && distances != std::size_t{distance.counts[0]} + distance.counts[1]))
        broken("the code of distances is not complete");

    lengths_ = std::move(literal);
    distances_ = std::move(distance);
    last_ = last;
    stage_ = Stage::coded;
}

// The bytes of a stored block that the input holds, or its end.
void Inflater::stored(Bytes& out) {
    if (left_ == 0) {
        stage_ = last_ ? (how_ == Wrapping::gzip ? Stage::trailer : Stage::done) : Stage::block;

        return;
    }

    if (at_ >= in_.size()) throw Short{};

    const std::size_t much = std::min<std::size_t>(left_, in_.size() - at_);

    for (std::size_t one = 0; one < much; ++one) history_.push_back(in_[at_ + one]);

    at_ += much;
    left_ -= static_cast<std::uint32_t>(much);
    kept(out);
}

// One symbol of a coded block: a byte, the end of the block, or a length and a distance back.
void Inflater::coded(Bytes& out) {
    const int symbol = decoded(lengths_);

    if (symbol < 256) {
        history_.push_back(static_cast<std::byte>(symbol));
        kept(out);

        return;
    }

    if (symbol == 256) {
        stage_ = last_ ? (how_ == Wrapping::gzip ? Stage::trailer : Stage::done) : Stage::block;

        return;
    }

    const auto at = static_cast<std::size_t>(symbol - 257);

    if (at >= length_base.size()) broken("a length symbol out of range");

    const std::uint32_t length = length_base[at] + bits(length_extra[at]);
    const auto far = static_cast<std::size_t>(decoded(distances_));

    if (far >= distance_base.size()) broken("a distance symbol out of range");

    const std::uint32_t distance = distance_base[far] + bits(distance_extra[far]);

    if (distance > history_.size()) broken("a distance back before the start");

    for (std::uint32_t one = 0; one < length; ++one) history_.push_back(history_[history_.size() - distance]);

    kept(out);
}

// A gzip member's trailer, from the next whole byte: its CRC-32 and size must be what came.
void Inflater::trailed() {
    if (bit_ != 0) {
        bit_ = 0;
        ++at_;
    }

    const std::uint32_t crc = bits(16) | (bits(16) << 16);
    const std::uint32_t size = bits(16) | (bits(16) << 16);

    if (crc != ~crc_) broken("the CRC-32 of a gzip member differs");

    if (size != size_) broken("the size of a gzip member differs");

    stage_ = Stage::done;
}

// Moves what the last step added to the history into `out`, counts it into the CRC-32 and the
// size of gzip, and keeps of the history the window alone, cut when it is twice that.
void Inflater::kept(Bytes& out) {
    const std::span<const std::byte> fresh = std::span<const std::byte>{history_}.subspan(emitted_);

    out.insert(out.end(), fresh.begin(), fresh.end());

    if (how_ == Wrapping::gzip) {
        const std::array<std::uint32_t, 256>& table = crc_table();

        for (const std::byte one : fresh) crc_ = table[(crc_ ^ std::to_integer<std::uint32_t>(one)) & 0xFFU] ^ (crc_ >> 8);

        size_ += static_cast<std::uint32_t>(fresh.size());
    }

    emitted_ = history_.size();

    if (history_.size() >= 2 * window) {
        history_.erase(history_.begin(), history_.end() - static_cast<std::ptrdiff_t>(window));
        emitted_ = history_.size();
    }
}

}

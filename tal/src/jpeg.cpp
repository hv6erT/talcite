#include "jpeging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include "tal/trouble.hpp"

#include "reducing.hpp"

// Our own JPEG decoder, built into tal-media: the Huffman coding of the baseline, of the extended
// sequential process of eight bits and of the progressive one, with restart intervals, any sampling
// of one to four components, into one frame of RGBA. Arithmetic coding, twelve bits, the lossless
// and the hierarchical processes are refused as unsupported: cameras and editors do not write them.
//
// The pixels come as libjpeg gives them by default, so that what we show matches what others show:
// chroma sampled at half is brought up by libjpeg's "fancy" triangle filters (h2v1, h1v2, h2v2),
// other ratios by repeating; YCbCr becomes RGB in libjpeg's fixed point; the inverse DCT is
// computed in floating point, within a few levels of libjpeg's integer one. Three components are
// YCbCr, unless Adobe's marker or the components' names say RGB; four are CMYK, or YCCK when
// Adobe's marker says so, inverted as Adobe writes them when its marker is there.
//
// An image wanted smaller (`most`) is reduced a half, a quarter or an eighth each way while it
// still covers what is wanted (`reduction`): each block, once turned back, gives the averages of
// its squares of two, four or eight samples, so a photo of fifty megapixels never stands whole in
// memory as pixels; only its coefficients do, which `most_coefficients` holds to 128 MiB.
//
// A file that is no JPEG, broken tables or frame, an unsupported process and an image over the
// limits are refused before anything is allocated. Damage in the coded data, or a file cut short,
// ends the decoding where it is: the blocks decoded stand and the rest stay gray, since the top of
// a picture is a better answer than nothing. A progressive file is held to `most_scans` scans and
// to going through its coefficients `most_work` times in all, since each scan costs a walk over
// every block whatever its size.
namespace tal::media::jpeging {

namespace {

constexpr std::size_t bands = 4;
constexpr std::size_t most_scans = 256;
constexpr std::uint64_t most_work = 16;
constexpr std::uint64_t most_coefficients = std::uint64_t{64} << 20;

// The place in a block of each coefficient in the order of the zigzag.
constexpr std::array<std::uint8_t, 64> natural{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                               41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                               30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Thrown inside the decoding of a scan when its coded data is damaged; the decoding stops there
// and keeps what it has.
struct Damaged {};

// A byte as an unsigned number.
[[nodiscard]] constexpr std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// A table of Huffman codes, kept as the standard's decoding procedure (F.2.2.3) wants it: for
// each length the largest code, the first code and where its values start.
struct Huffman {
    std::array<std::int32_t, 17> largest{};
    std::array<std::int32_t, 17> first{};
    std::array<std::int32_t, 17> start{};
    std::vector<std::uint8_t> values;
    bool present = false;
};

// A component of the frame: its name, sampling, table of quantization and tables of coding, its
// blocks across and down as allocated (whole units of coding) and as the image covers them, its
// coefficients and the prediction of its DC.
struct Component {
    std::uint32_t id = 0;
    std::uint32_t h = 1;
    std::uint32_t v = 1;
    std::uint32_t quantization = 0;
    std::uint32_t dc = 0;
    std::uint32_t ac = 0;
    std::uint32_t blocks_across = 0;
    std::uint32_t blocks_down = 0;
    std::uint32_t covered_across = 0;
    std::uint32_t covered_down = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::int16_t> coefficients;
    std::int64_t predicted = 0;
};

// The bits of coded data: bytes stuffed after 0xFF taken back, and zeros once a marker or the end
// is reached, where the data of a scan stops.
class Bits {
public:
    // Over the file, from `at`.
    Bits(std::span<const std::byte> what, std::size_t at) : what_(what), at_(at) {}

    // The next `many` bits (at most sixteen), the first the highest.
    [[nodiscard]] std::uint32_t bits(int many) {
        if (many == 0) return 0;

        filled(many);
        count_ -= many;

        return static_cast<std::uint32_t>((held_ >> count_) & ((1u << many) - 1));
    }

    // The next bit.
    [[nodiscard]] std::uint32_t bit() { return bits(1); }

    // The next `many` bits as a value of that many bits, extended to its sign as the standard
    // does (F.2.2.1): a leading zero makes it negative.
    [[nodiscard]] std::int32_t received(int many) {
        if (many == 0) return 0;

        const auto value = static_cast<std::int32_t>(bits(many));

        return value < (1 << (many - 1)) ? value - (1 << many) + 1 : value;
    }

    // The next value of the table; damage for a code the table does not have.
    [[nodiscard]] std::uint32_t decoded(const Huffman& table) {
        std::int32_t code = static_cast<std::int32_t>(bit());

        for (int length = 1; length <= 16; ++length) {
            if (code <= table.largest[length]) return table.values[static_cast<std::size_t>(table.start[length] + code - table.first[length])];

            code = (code << 1) | static_cast<std::int32_t>(bit());
        }

        throw Damaged{};
    }

    // Goes on after a restart marker: the bits left of the byte are dropped, and the next marker,
    // which must be a restart, is passed; damage for any other.
    void restarted() {
        held_ = 0;
        count_ = 0;
        stopped_ = false;

        while (at_ + 1 < what_.size() && !(sample(what_[at_]) == 0xFF && sample(what_[at_ + 1]) != 0x00 && sample(what_[at_ + 1]) != 0xFF)) ++at_;

        if (at_ + 1 >= what_.size() || sample(what_[at_ + 1]) < 0xD0 || sample(what_[at_ + 1]) > 0xD7) throw Damaged{};

        at_ += 2;
    }

    // Where the coded data stopped: at the marker after it, or at the end.
    [[nodiscard]] std::size_t ended() {
        while (at_ + 1 < what_.size() && !(sample(what_[at_]) == 0xFF && sample(what_[at_ + 1]) != 0x00 && sample(what_[at_ + 1]) != 0xFF &&
                                           (sample(what_[at_ + 1]) < 0xD0 || sample(what_[at_ + 1]) > 0xD7)))
            ++at_;

        return at_;
    }

    // Whether the data ran past the end of the file.
    [[nodiscard]] bool short_of() const { return short_; }

    // Whether the data stopped, at a marker or at the end of the file.
    [[nodiscard]] bool stopped() const { return stopped_; }

private:
    // Fills the bits held to at least `many`: a byte, a stuffed 0xFF, or zeros at a marker.
    void filled(int many) {
        while (count_ < many) {
            std::uint32_t next = 0;

            if (!stopped_ && at_ >= what_.size()) {
                stopped_ = true;
                short_ = true;
            }

            if (!stopped_) {
                next = sample(what_[at_]);

                if (next != 0xFF) {
                    ++at_;
                } else if (at_ + 1 < what_.size() && sample(what_[at_ + 1]) == 0x00) {
                    at_ += 2;
                } else {
                    next = 0;
                    stopped_ = true;
                }
            }

            held_ = (held_ << 8) | next;
            count_ += 8;
        }
    }

    std::span<const std::byte> what_;
    std::size_t at_;
    std::uint64_t held_ = 0;
    int count_ = 0;
    bool stopped_ = false;
    bool short_ = false;
};

// What a scan says: its components (as places in the frame), the band of coefficients and the
// bits of successive approximation.
struct Scan {
    std::vector<std::size_t> components;
    std::uint32_t start = 0;
    std::uint32_t end = 63;
    std::uint32_t high = 0;
    std::uint32_t low = 0;
};

// The decoding of one file: the tables, the frame and what the markers said.
class Decoder {
public:
    // Over the file, for a frame that covers `most` when it can be smaller.
    Decoder(std::span<const std::byte> what, Size most) : what_(what), most_(most) {}

    // Reads the markers and the scans, then hands the frame to `each`.
    void decode(const Taking& each);

private:
    [[nodiscard]] std::uint32_t big16(std::size_t at) const;

    void tabled(std::span<const std::byte> data);

    void quantized(std::span<const std::byte> data);

    void framed(std::span<const std::byte> data, bool progressive);

    [[nodiscard]] Scan scanned(std::span<const std::byte> data) const;

    [[nodiscard]] std::size_t decoded(const Scan& scan, std::size_t at);

    void block(Bits& bits, const Scan& scan, Component& one, std::int16_t* coefficients);

    [[nodiscard]] Bytes pixels() const;

    [[nodiscard]] std::vector<std::uint8_t> plane(const Component& one) const;

    std::span<const std::byte> what_;
    Size most_;
    std::uint32_t scale_ = 1;
    std::array<Huffman, 4> dc_{};
    std::array<Huffman, 4> ac_{};
    std::array<std::array<std::uint32_t, 64>, 4> quantization_{};
    std::array<bool, 4> quantization_present_{};
    std::vector<Component> components_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t h_most_ = 1;
    std::uint32_t v_most_ = 1;
    std::uint32_t restart_ = 0;
    std::uint32_t eob_run_ = 0;
    std::uint64_t coefficients_ = 0;
    std::uint64_t work_ = 0;
    bool worn_ = false;
    std::optional<std::uint32_t> adobe_;
    bool progressive_ = false;
    bool framed_ = false;
};

// The big-endian 16-bit number at `at` of the file; the caller made sure the two bytes are there.
std::uint32_t Decoder::big16(std::size_t at) const { return (sample(what_[at]) << 8) | sample(what_[at + 1]); }

// Takes the tables of Huffman of DHT, each built for the standard's decoding procedure; a table
// whose counts ask for more codes than their lengths have is broken.
void Decoder::tabled(std::span<const std::byte> data) {
    std::size_t at = 0;

    while (at < data.size()) {
        if (data.size() - at < 17) throw Trouble{Kind::malformed, "A table of Huffman of the JPEG is broken."};

        const std::uint32_t kind = sample(data[at]) >> 4;
        const std::uint32_t place = sample(data[at]) & 15;

        if (kind > 1 || place > 3) throw Trouble{Kind::malformed, "A table of Huffman of the JPEG is broken."};

        Huffman made;
        std::size_t total = 0;
        std::int32_t code = 0;

        for (int length = 1; length <= 16; ++length) {
            const std::uint32_t many = sample(data[at + static_cast<std::size_t>(length)]);

            made.first[length] = code;
            made.start[length] = static_cast<std::int32_t>(total);
            code += static_cast<std::int32_t>(many);
            made.largest[length] = many == 0 ? -1 : code - 1;
            total += many;

            if (code > (1 << length)) throw Trouble{Kind::malformed, "A table of Huffman of the JPEG is broken."};

            code <<= 1;
        }

        if (total > 256 || data.size() - at - 17 < total) throw Trouble{Kind::malformed, "A table of Huffman of the JPEG is broken."};

        for (std::size_t one = 0; one < total; ++one) made.values.push_back(static_cast<std::uint8_t>(sample(data[at + 17 + one])));

        made.present = true;
        (kind == 0 ? dc_ : ac_)[place] = std::move(made);
        at += 17 + total;
    }
}

// Takes the tables of quantization of DQT, of eight or sixteen bits, in the order of the zigzag
// as they come, kept in the natural order of a block.
void Decoder::quantized(std::span<const std::byte> data) {
    std::size_t at = 0;

    while (at < data.size()) {
        const std::uint32_t precision = sample(data[at]) >> 4;
        const std::uint32_t place = sample(data[at]) & 15;
        const std::size_t size = precision == 0 ? 64 : 128;

        if (precision > 1 || place > 3 || data.size() - at - 1 < size) throw Trouble{Kind::malformed, "A table of quantization of the JPEG is broken."};

        for (std::size_t one = 0; one < 64; ++one)
            quantization_[place][natural[one]] = precision == 0 ? sample(data[at + 1 + one]) : (sample(data[at + 1 + 2 * one]) << 8) | sample(data[at + 2 + 2 * one]);

        quantization_present_[place] = true;
        at += 1 + size;
    }
}

// Takes the frame of SOF: eight bits, a size of at least a pixel each way, one to four components
// of sampling 1 to 4; settles the reduction (`reduction`) and allocates the coefficients of every
// block in whole units of coding. Throws `Trouble`: malformed for a broken frame, unsupported for
// twelve bits or a height left to DNL, full for an image over the limits.
void Decoder::framed(std::span<const std::byte> data, bool progressive) {
    if (framed_) throw Trouble{Kind::malformed, "The JPEG has two frames."};

    if (data.size() < 6) throw Trouble{Kind::malformed, "The frame of the JPEG is broken."};

    if (sample(data[0]) != 8) throw Trouble{Kind::unsupported, "A JPEG of other than eight bits."};

    height_ = (sample(data[1]) << 8) | sample(data[2]);
    width_ = (sample(data[3]) << 8) | sample(data[4]);

    const std::uint32_t many = sample(data[5]);

    if (height_ == 0) throw Trouble{Kind::unsupported, "A JPEG whose height comes after its data."};

    if (width_ == 0 || many == 0 || many > 4 || many == 2 || data.size() != 6 + 3 * std::size_t{many}) throw Trouble{Kind::malformed, "The frame of the JPEG is broken."};

    for (std::size_t one = 0; one < many; ++one) {
        Component made;

        made.id = sample(data[6 + 3 * one]);
        made.h = sample(data[7 + 3 * one]) >> 4;
        made.v = sample(data[7 + 3 * one]) & 15;
        made.quantization = sample(data[8 + 3 * one]);

        if (made.h < 1 || made.h > 4 || made.v < 1 || made.v > 4 || made.quantization > 3) throw Trouble{Kind::malformed, "The frame of the JPEG is broken."};

        h_most_ = std::max(h_most_, made.h);
        v_most_ = std::max(v_most_, made.v);
        components_.push_back(made);
    }

    scale_ = reduction(Size{width_, height_}, most_);

    const std::uint32_t units_across = (width_ + 8 * h_most_ - 1) / (8 * h_most_);
    const std::uint32_t units_down = (height_ + 8 * v_most_ - 1) / (8 * v_most_);
    std::uint64_t coefficients = 0;

    for (const Component& one : components_) coefficients += std::uint64_t{units_across} * one.h * units_down * one.v * 64;

    if (coefficients > most_coefficients) throw Trouble{Kind::full, "The JPEG is over the ceiling."};

    coefficients_ = coefficients;

    for (Component& one : components_) {
        one.width = (width_ * one.h + h_most_ - 1) / h_most_;
        one.height = (height_ * one.v + v_most_ - 1) / v_most_;
        one.covered_across = (one.width + 7) / 8;
        one.covered_down = (one.height + 7) / 8;
        one.blocks_across = units_across * one.h;
        one.blocks_down = units_down * one.v;
        one.coefficients.assign(std::size_t{one.blocks_across} * one.blocks_down * 64, 0);
    }

    progressive_ = progressive;
    framed_ = true;
}

// Reads the head of SOS: its components among the frame's with their tables, and the band and
// bits of the coefficients it codes, as the process allows them. Throws `Trouble` malformed for
// a head that breaks the rules.
Scan Decoder::scanned(std::span<const std::byte> data) const {
    if (!framed_ || data.empty()) throw Trouble{Kind::malformed, "A scan of the JPEG is broken."};

    const std::uint32_t many = sample(data[0]);

    if (many < 1 || many > 4 || data.size() != 4 + 2 * std::size_t{many}) throw Trouble{Kind::malformed, "A scan of the JPEG is broken."};

    Scan made;

    for (std::size_t one = 0; one < many; ++one) {
        const std::uint32_t id = sample(data[1 + 2 * one]);
        const auto found = std::ranges::find(components_, id, &Component::id);

        if (found == components_.end()) throw Trouble{Kind::malformed, "A scan of the JPEG names no component of its frame."};

        made.components.push_back(static_cast<std::size_t>(found - components_.begin()));
    }

    made.start = sample(data[1 + 2 * many]);
    made.end = sample(data[2 + 2 * many]);
    made.high = sample(data[3 + 2 * many]) >> 4;
    made.low = sample(data[3 + 2 * many]) & 15;

    const bool good = progressive_ ? made.start <= made.end && made.end <= 63 && (made.start == 0) == (made.end == 0) && (made.start == 0 || many == 1) && made.low <= 13 &&
                                         (made.high == 0 || made.high == made.low + 1)
                                   : made.start == 0 && made.end == 63 && made.high == 0 && made.low == 0;

    if (!good) throw Trouble{Kind::malformed, "A scan of the JPEG is broken."};

    return made;
}

// Decodes the coefficients of one block of `one` in the scan: the whole block in a sequential
// scan; in a progressive one its DC first or refined, or a band of its AC first or refined, with
// the run of blocks at their end of band (`eob_run_`) the scans carry. Throws `Damaged` for coded
// data that breaks the rules.
void Decoder::block(Bits& bits, const Scan& scan, Component& one, std::int16_t* coefficients) {
    const auto shifted = [&scan](std::int64_t value) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(value * (std::int64_t{1} << scan.low))); };

    if (scan.start == 0 && !(progressive_ && scan.high != 0)) {
        const std::uint32_t size = bits.decoded(dc_[one.dc]);

        if (size > 11) throw Damaged{};

        one.predicted += bits.received(static_cast<int>(size));
        coefficients[0] = shifted(one.predicted);

        if (progressive_) return;

        for (std::uint32_t at = 1; at < 64;) {
            const std::uint32_t pair = bits.decoded(ac_[one.ac]);
            const std::uint32_t run = pair >> 4;
            const std::uint32_t magnitude = pair & 15;

            if (magnitude == 0) {
                if (run != 15) break;

                at += 16;

                continue;
            }

            at += run;

            if (at > 63) throw Damaged{};

            coefficients[natural[at]] = static_cast<std::int16_t>(bits.received(static_cast<int>(magnitude)));
            ++at;
        }

        return;
    }

    if (scan.start == 0) {
        if (bits.bit() != 0) coefficients[0] = static_cast<std::int16_t>(coefficients[0] | (1 << scan.low));

        return;
    }

    if (scan.high == 0) {
        if (eob_run_ > 0) {
            --eob_run_;

            return;
        }

        for (std::uint32_t at = scan.start; at <= scan.end;) {
            const std::uint32_t pair = bits.decoded(ac_[one.ac]);
            const std::uint32_t run = pair >> 4;
            const std::uint32_t magnitude = pair & 15;

            if (magnitude == 0) {
                if (run < 15) {
                    eob_run_ = (1u << run) - 1 + bits.bits(static_cast<int>(run));

                    break;
                }

                at += 16;

                continue;
            }

            at += run;

            if (at > scan.end) throw Damaged{};

            coefficients[natural[at]] = shifted(bits.received(static_cast<int>(magnitude)));
            ++at;
        }

        return;
    }

    const auto bit = static_cast<std::int16_t>(1 << scan.low);
    const auto refined = [&bits, bit](std::int16_t& coefficient) {
        if (bits.bit() != 0 && (coefficient & bit) == 0) coefficient = static_cast<std::int16_t>(coefficient >= 0 ? coefficient + bit : coefficient - bit);
    };

    if (eob_run_ > 0) {
        --eob_run_;

        for (std::uint32_t at = scan.start; at <= scan.end; ++at)
            if (coefficients[natural[at]] != 0) refined(coefficients[natural[at]]);

        return;
    }

    for (std::uint32_t at = scan.start; at <= scan.end;) {
        const std::uint32_t pair = bits.decoded(ac_[one.ac]);
        std::uint32_t run = pair >> 4;
        const std::uint32_t magnitude = pair & 15;
        std::int16_t value = 0;

        if (magnitude == 0) {
            if (run < 15) {
                eob_run_ = (1u << run) - 1 + bits.bits(static_cast<int>(run));
                run = 64;
            }
        } else {
            if (magnitude != 1) throw Damaged{};

            value = bits.bit() != 0 ? bit : static_cast<std::int16_t>(-bit);
        }

        while (at <= scan.end) {
            std::int16_t& coefficient = coefficients[natural[at++]];

            if (coefficient != 0) {
                refined(coefficient);
            } else {
                if (run == 0) {
                    coefficient = value;

                    break;
                }

                --run;
            }
        }
    }
}

// Decodes the coded data of a scan from `at`, unit after unit (or block after block of the one
// component of a scan that is not interleaved, as far as the image covers), going past a restart
// marker after every interval; damage or a file cut short ends it quietly. Data that stops at a
// marker inside an interval leaves the rest of the interval undecoded, as libjpeg does, rather than
// decoding it from nothing, and ends the scan where there is no interval. A scan that would take
// the coefficients gone through past `most_work` times their number is not decoded, and no scan
// after it (`worn_`). Where the data stopped.
std::size_t Decoder::decoded(const Scan& scan, std::size_t at) {
    for (const std::size_t one : scan.components) {
        const bool dc_wanted = scan.start == 0 && scan.high == 0;
        const bool ac_wanted = scan.end > 0 || !progressive_;

        if ((dc_wanted && !dc_[components_[one].dc].present) || (ac_wanted && !ac_[components_[one].ac].present))
            throw Trouble{Kind::malformed, "A scan of the JPEG has no table of Huffman."};

        components_[one].predicted = 0;
    }

    eob_run_ = 0;

    Bits bits{what_, at};
    const bool single = scan.components.size() == 1;
    const Component& lone = components_[scan.components.front()];
    const std::uint32_t across = single ? lone.covered_across : (width_ + 8 * h_most_ - 1) / (8 * h_most_);
    const std::uint32_t down = single ? lone.covered_down : (height_ + 8 * v_most_ - 1) / (8 * v_most_);
    const std::uint64_t units = std::uint64_t{across} * down;
    std::uint64_t blocks = 0;

    for (const std::size_t one : scan.components) blocks += single ? 1 : std::uint64_t{components_[one].h} * components_[one].v;

    work_ += units * blocks * (scan.end - scan.start + 1);

    if (work_ > most_work * coefficients_) {
        worn_ = true;

        return at;
    }

    try {
        for (std::uint64_t unit = 0; unit < units; ++unit) {
            if (restart_ != 0 && unit != 0 && unit % restart_ == 0) {
                bits.restarted();
                eob_run_ = 0;

                for (const std::size_t one : scan.components) components_[one].predicted = 0;
            }

            const auto x = static_cast<std::uint32_t>(unit % across);
            const auto y = static_cast<std::uint32_t>(unit / across);

            for (const std::size_t place : scan.components) {
                Component& one = components_[place];
                const std::uint32_t wide = single ? 1 : one.h;
                const std::uint32_t high = single ? 1 : one.v;

                for (std::uint32_t row = 0; row < high; ++row)
                    for (std::uint32_t column = 0; column < wide; ++column) {
                        const std::size_t index = std::size_t{y * high + row} * one.blocks_across + x * wide + column;

                        block(bits, scan, one, one.coefficients.data() + index * 64);
                    }
            }

            if (!bits.stopped()) continue;

            if (bits.short_of() || restart_ == 0) break;

            unit = (unit / restart_ + 1) * restart_ - 1;
        }
    } catch (const Damaged&) {
    }

    return bits.ended();
}

// The samples of a component, its blocks dequantized and turned back by the inverse DCT, level
// shifted, averaged over squares of `scale_` samples and rounded into eight bits: a block with no
// AC is one value, the rest go through the transform in two passes of eight.
std::vector<std::uint8_t> Decoder::plane(const Component& one) const {
    static const std::array<std::array<float, 8>, 8> cosines = [] {
        std::array<std::array<float, 8>, 8> made{};

        for (std::size_t x = 0; x < 8; ++x)
            for (std::size_t u = 0; u < 8; ++u)
                made[x][u] = static_cast<float>((u == 0 ? std::numbers::sqrt2 / 4 : 0.5) * std::cos((2.0 * static_cast<double>(x) + 1.0) * static_cast<double>(u) * std::numbers::pi / 16.0));

        return made;
    }();

    const std::size_t side = 8 / scale_;
    const std::size_t stride = std::size_t{one.blocks_across} * side;
    std::vector<std::uint8_t> made(stride * one.blocks_down * side, 128);
    const std::array<std::uint32_t, 64>& table = quantization_[one.quantization];

    for (std::uint32_t by = 0; by < one.blocks_down; ++by)
        for (std::uint32_t bx = 0; bx < one.blocks_across; ++bx) {
            const std::int16_t* coefficients = one.coefficients.data() + (std::size_t{by} * one.blocks_across + bx) * 64;
            std::array<float, 64> block{};
            bool flat = true;

            for (std::size_t at = 0; at < 64; ++at) {
                block[at] = static_cast<float>(std::int64_t{coefficients[at]} * table[at]);

                if (at != 0 && coefficients[at] != 0) flat = false;
            }

            std::array<float, 64> rows{};

            if (flat) {
                rows.fill(block[0] / 8.0f);
            } else {
                std::array<float, 64> half{};

                for (std::size_t v = 0; v < 8; ++v)
                    for (std::size_t x = 0; x < 8; ++x) {
                        float sum = 0;

                        for (std::size_t u = 0; u < 8; ++u) sum += cosines[x][u] * block[v * 8 + u];

                        half[v * 8 + x] = sum;
                    }

                for (std::size_t y = 0; y < 8; ++y)
                    for (std::size_t x = 0; x < 8; ++x) {
                        float sum = 0;

                        for (std::size_t v = 0; v < 8; ++v) sum += cosines[y][v] * half[v * 8 + x];

                        rows[y * 8 + x] = sum;
                    }
            }

            for (std::size_t y = 0; y < side; ++y)
                for (std::size_t x = 0; x < side; ++x) {
                    float sum = 0;

                    for (std::size_t down = 0; down < scale_; ++down)
                        for (std::size_t across = 0; across < scale_; ++across) sum += rows[(y * scale_ + down) * 8 + x * scale_ + across];

                    const float level = std::round(sum / static_cast<float>(scale_ * scale_) + 128.0f);

                    made[(std::size_t{by} * side + y) * stride + std::size_t{bx} * side + x] = static_cast<std::uint8_t>(std::clamp(level, 0.0f, 255.0f));
                }
        }

    return made;
}

// The sample of a component at a pixel of the image, brought up to it: by libjpeg's triangle
// filters where the component is sampled at half across, down or both, by repeating otherwise.
// The neighbours past the edge of what the component covers (`room`) are its edge.
[[nodiscard]] std::uint32_t upsampled(const std::vector<std::uint8_t>& plane, std::size_t stride, Size room, const Component& one, std::uint32_t h_most,
                                      std::uint32_t v_most, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t across = h_most / one.h;
    const std::uint32_t down = v_most / one.v;
    const bool fancy = (across == 1 || across == 2) && (down == 1 || down == 2) && h_most % one.h == 0 && v_most % one.v == 0;
    const auto at = [&](std::int64_t column, std::int64_t row) -> std::uint32_t {
        const auto c = static_cast<std::size_t>(std::clamp<std::int64_t>(column, 0, std::int64_t{room.width} - 1));
        const auto r = static_cast<std::size_t>(std::clamp<std::int64_t>(row, 0, std::int64_t{room.height} - 1));

        return plane[r * stride + c];
    };

    if (!fancy) return at(std::int64_t{x} * one.h / h_most, std::int64_t{y} * one.v / v_most);

    const std::int64_t column = x / across;
    const std::int64_t row = y / down;
    const std::int64_t beside = across == 2 ? (x % 2 == 0 ? column - 1 : column + 1) : column;
    const std::int64_t over = down == 2 ? (y % 2 == 0 ? row - 1 : row + 1) : row;

    if (across == 1 && down == 1) return at(column, row);

    if (down == 1) return (3 * at(column, row) + at(beside, row) + (x % 2 == 0 ? 1 : 2)) >> 2;

    if (across == 1) return (3 * at(column, row) + at(column, over) + (y % 2 == 0 ? 1 : 2)) >> 2;

    const std::uint32_t here = 3 * at(column, row) + at(column, over);
    const std::uint32_t next = 3 * at(beside, row) + at(beside, over);

    return (3 * here + next + (x % 2 == 0 ? 8 : 7)) >> 4;
}

// A number of libjpeg's fixed point of sixteen bits.
[[nodiscard]] constexpr std::int32_t fixed(double value) { return static_cast<std::int32_t>(value * 65536.0 + 0.5); }

// YCbCr as RGB, in libjpeg's fixed point and rounding, clamped to eight bits.
[[nodiscard]] std::array<std::uint32_t, 3> rgb_of(std::int32_t y, std::int32_t cb, std::int32_t cr) {
    constexpr std::int32_t half = 1 << 15;
    const std::int32_t blue = cb - 128;
    const std::int32_t red = cr - 128;
    const auto clamped = [](std::int32_t one) { return static_cast<std::uint32_t>(std::clamp(one, 0, 255)); };

    return {clamped(y + ((fixed(1.40200) * red + half) >> 16)), clamped(y + ((-fixed(0.34414) * blue - fixed(0.71414) * red + half) >> 16)),
            clamped(y + ((fixed(1.77200) * blue + half) >> 16))};
}

// The frame as RGBA, reduced `scale_` times: every component brought to the size of the image and
// its colours made RGB as its components and Adobe's marker say.
Bytes Decoder::pixels() const {
    std::vector<std::vector<std::uint8_t>> planes;
    std::vector<Size> rooms;

    for (const Component& one : components_) {
        planes.push_back(plane(one));
        rooms.push_back(reduced(Size{one.width, one.height}, scale_));
    }

    const Size size = reduced(Size{width_, height_}, scale_);

    const bool adobe = adobe_.has_value();
    const bool named_rgb = components_.size() == 3 && components_[0].id == 'R' && components_[1].id == 'G' && components_[2].id == 'B';
    const bool rgb = components_.size() == 3 && ((adobe && *adobe_ == 0) || (!adobe && named_rgb));
    const bool ycck = components_.size() == 4 && adobe && *adobe_ == 2;
    Bytes made(std::size_t{size.width} * size.height * bands);

    for (std::uint32_t y = 0; y < size.height; ++y)
        for (std::uint32_t x = 0; x < size.width; ++x) {
            std::array<std::uint32_t, 4> samples{};

            for (std::size_t one = 0; one < components_.size(); ++one)
                samples[one] = upsampled(planes[one], std::size_t{components_[one].blocks_across} * 8 / scale_, rooms[one], components_[one], h_most_, v_most_, x, y);

            std::array<std::uint32_t, 3> colour{samples[0], samples[0], samples[0]};

            if (components_.size() == 3) {
                colour = rgb ? std::array<std::uint32_t, 3>{samples[0], samples[1], samples[2]}
                             : rgb_of(static_cast<std::int32_t>(samples[0]), static_cast<std::int32_t>(samples[1]), static_cast<std::int32_t>(samples[2]));
            } else if (components_.size() == 4) {
                std::array<std::uint32_t, 3> inks{samples[0], samples[1], samples[2]};

                if (ycck) {
                    inks = rgb_of(static_cast<std::int32_t>(samples[0]), static_cast<std::int32_t>(samples[1]), static_cast<std::int32_t>(samples[2]));

                    for (std::uint32_t& ink : inks) ink = 255 - ink;
                }

                const std::uint32_t black = samples[3];

                for (std::size_t band = 0; band < 3; ++band) colour[band] = adobe ? (inks[band] * black + 127) / 255 : ((255 - inks[band]) * (255 - black) + 127) / 255;
            }

            const std::size_t place = (std::size_t{y} * size.width + x) * bands;

            for (std::size_t band = 0; band < 3; ++band) made[place + band] = static_cast<std::byte>(colour[band]);

            made[place + 3] = std::byte{255};
        }

    return made;
}

// Reads the markers: tables, the frame, Adobe's marker, the restart interval, and every scan with
// its coded data, until the end of the image or of the file; then hands the frame to `each`.
// Throws `Trouble`: malformed for broken markers or tables, no frame or no scan; unsupported for
// a process we do not decode.
void Decoder::decode(const Taking& each) {
    std::size_t at = 2;
    std::size_t scans = 0;

    while (at + 4 <= what_.size()) {
        if (sample(what_[at]) != 0xFF) throw Trouble{Kind::malformed, "The markers of the JPEG are broken."};

        const std::uint32_t marker = sample(what_[at + 1]);

        if (marker == 0xFF) {
            ++at;

            continue;
        }

        if (marker == 0xD9) break;

        if ((marker >= 0xD0 && marker <= 0xD8) || marker == 0x01) {
            at += 2;

            continue;
        }

        const std::uint32_t length = big16(at + 2);

        if (length < 2 || what_.size() - at - 2 < length) break;

        const std::span<const std::byte> data = what_.subspan(at + 4, length - 2);

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            framed(data, marker == 0xC2);
        } else if (marker == 0xC3 || (marker >= 0xC5 && marker <= 0xCF && marker != 0xC8 && marker != 0xCC)) {
            throw Trouble{Kind::unsupported, "A JPEG of a process we do not decode."};
        } else if (marker == 0xC4) {
            tabled(data);
        } else if (marker == 0xDB) {
            quantized(data);
        } else if (marker == 0xDD) {
            if (data.size() != 2) throw Trouble{Kind::malformed, "The restart interval of the JPEG is broken."};

            restart_ = (sample(data[0]) << 8) | sample(data[1]);
        } else if (marker == 0xEE && data.size() >= 12 && std::string_view{reinterpret_cast<const char*>(data.data()), 5} == "Adobe") {
            adobe_ = sample(data[11]);
        } else if (marker == 0xDA) {
            const Scan scan = scanned(data);

            for (std::size_t index = 0; index < scan.components.size(); ++index) {
                Component& one = components_[scan.components[index]];

                if (!quantization_present_[one.quantization]) throw Trouble{Kind::malformed, "A component of the JPEG has no table of quantization."};

                one.dc = sample(data[2 + 2 * index]) >> 4;
                one.ac = sample(data[2 + 2 * index]) & 15;

                if (one.dc > 3 || one.ac > 3) throw Trouble{Kind::malformed, "A scan of the JPEG is broken."};
            }

            at = decoded(scan, at + 2 + length);
            ++scans;

            if (worn_ || scans >= most_scans) break;

            continue;
        }

        at += 2 + length;
    }

    if (!framed_ || scans == 0) throw Trouble{Kind::malformed, "The JPEG has no image."};

    const Bytes made = pixels();

    (void)each(made, reduced(Size{width_, height_}, scale_), std::chrono::milliseconds{0});
}

}

// Decodes a JPEG into one frame of RGBA handed to `each`, reduced as far as it still covers `most`
// (zero: whole). Throws `Trouble`: malformed for no JPEG or broken markers, tables, frame or scans;
// unsupported for a process we do not decode; full for an image over the limits. Damage in the
// coded data, or a file cut short, gives what came before.
void jpeged(std::span<const std::byte> what, const Taking& each, Size most) {
    if (what.size() < 4 || sample(what[0]) != 0xFF || sample(what[1]) != 0xD8) throw Trouble{Kind::malformed, "Not a JPEG."};

    Decoder{what, most}.decode(each);
}

}

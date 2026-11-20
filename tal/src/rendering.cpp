#include "rendering.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

// The protocol between a program and a process that renders documents or pages for it (version 1),
// described for the makers of engines in RENDERING.md at the root of tal: frames over the process's
// standard streams, each its length, sort, number and payload, little-endian.
//
// Everything a frame says is checked as the words of a stranger: a frame is at most `widest` long
// (a picture at the ceiling of pixels and some), a text at most a frame, and a frame read to its
// end leaves nothing behind.
namespace tal::rendering {

namespace {

// The trouble of a frame that breaks the protocol.
Trouble broken(std::string said) { return Trouble{Kind::malformed, "The renderer broke the protocol: " + std::move(said)}; }

// The number of `much` bytes at `at` of `from`, little-endian.
std::uint64_t little(std::span<const std::byte> from, std::size_t at, std::size_t much) {
    std::uint64_t made = 0;

    for (std::size_t one = much; one > 0; --one) made = (made << 8) | std::to_integer<std::uint64_t>(from[at + one - 1]);

    return made;
}

// `value` in `much` bytes, little-endian, after the bytes of `into`.
void put(Bytes& into, std::uint64_t value, std::size_t much) {
    for (std::size_t one = 0; one < much; ++one) into.push_back(static_cast<std::byte>((value >> (8 * one)) & 0xFF));
}

}

// The frame `frame` in bytes, ready to be written.
Bytes framed(const Frame& frame) {
    Bytes made;

    made.reserve(head + frame.payload.size());
    put(made, 5 + frame.payload.size(), 4);
    made.push_back(static_cast<std::byte>(frame.sort));
    put(made, frame.id, 4);
    made.insert(made.end(), frame.payload.begin(), frame.payload.end());

    return made;
}

// Keeps `more` after what came before it and is not taken yet.
void Unframing::add(std::span<const std::byte> more) {
    if (at_ > 0) {
        held_.erase(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(at_));
        at_ = 0;
    }

    held_.insert(held_.end(), more.begin(), more.end());
}

// The next whole frame, when it has come. Throws `Trouble` malformed for a length shorter than the
// sort and the number or longer than the most allowed.
std::optional<Frame> Unframing::next() {
    const std::span<const std::byte> left{held_.data() + at_, held_.size() - at_};

    if (left.size() < 4) return std::nullopt;

    const std::uint64_t length = little(left, 0, 4);

    if (length < 5 || length > most_ - 4) throw broken("a frame of " + std::to_string(length) + " bytes.");

    if (left.size() < 4 + length) return std::nullopt;

    Frame made;

    made.sort = static_cast<Sort>(std::to_integer<std::uint8_t>(left[4]));
    made.id = static_cast<std::uint32_t>(little(left, 5, 4));
    made.payload.assign(left.begin() + head, left.begin() + static_cast<std::ptrdiff_t>(4 + length));
    at_ += 4 + static_cast<std::size_t>(length);

    if (at_ == held_.size()) {
        held_.clear();
        at_ = 0;
    }

    return made;
}

// Writes `value` in one byte.
Writing& Writing::u8(std::uint8_t value) {
    put(made_, value, 1);

    return *this;
}

// Writes `value` in four bytes.
Writing& Writing::u32(std::uint32_t value) {
    put(made_, value, 4);

    return *this;
}

// Writes `value` in eight bytes.
Writing& Writing::u64(std::uint64_t value) {
    put(made_, value, 8);

    return *this;
}

// Writes `value` as IEEE 754 in eight bytes.
Writing& Writing::f64(double value) {
    put(made_, std::bit_cast<std::uint64_t>(value), 8);

    return *this;
}

// Writes `value` as its length and its bytes.
Writing& Writing::text(std::string_view value) {
    put(made_, value.size(), 4);

    for (const char one : value) made_.push_back(static_cast<std::byte>(one));

    return *this;
}

// Writes the bytes `value` as they are, with no length.
Writing& Writing::bytes(std::span<const std::byte> value) {
    made_.insert(made_.end(), value.begin(), value.end());

    return *this;
}

// The next `much` bytes. Throws `Trouble` malformed when fewer are left.
std::span<const std::byte> Reading::taken(std::size_t much) {
    if (much > from_.size() - at_) throw broken("a frame shorter than what it says.");

    const std::span<const std::byte> made = from_.subspan(at_, much);

    at_ += much;

    return made;
}

// The next byte.
std::uint8_t Reading::u8() { return static_cast<std::uint8_t>(little(taken(1), 0, 1)); }

// The next four bytes as a number.
std::uint32_t Reading::u32() { return static_cast<std::uint32_t>(little(taken(4), 0, 4)); }

// The next eight bytes as a number.
std::uint64_t Reading::u64() { return little(taken(8), 0, 8); }

// The next eight bytes as a fraction. Throws `Trouble` malformed for one that is not finite.
double Reading::f64() {
    const double made = std::bit_cast<double>(little(taken(8), 0, 8));

    if (!std::isfinite(made)) throw broken("a number that is not finite.");

    return made;
}

// The next text, its broken UTF-8 repaired (U+FFFD for what is not well-formed).
std::string Reading::text() {
    const std::uint32_t much = u32();
    const std::span<const std::byte> bytes = taken(much);

    return text::repaired(std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

// The next `much` bytes as they are.
std::span<const std::byte> Reading::bytes(std::size_t much) { return taken(much); }

// Everything left.
std::span<const std::byte> Reading::rest() { return taken(from_.size() - at_); }

// Throws `Trouble` malformed when anything is left.
void Reading::ended() const {
    if (at_ != from_.size()) throw broken("a frame longer than what it says.");
}

// The hello in `payload`. Throws `Trouble` malformed for one of another shape.
Hello greeting(std::span<const std::byte> payload) {
    Reading from{payload};
    Hello made;

    made.version = from.u32();
    made.engine = from.text();
    made.abilities = from.u8();
    from.ended();

    return made;
}

// The payload of the hello `hello`.
Bytes greeted(const Hello& hello) { return Writing{}.u32(hello.version).text(hello.engine).u8(hello.abilities).done(); }

// The trouble a process answered with, out of the payload of a failed frame: the kind and the
// sentence. Throws `Trouble` malformed for one of another shape or of a kind tal does not know.
Trouble failure(std::span<const std::byte> payload) {
    Reading from{payload};
    const std::uint8_t kind = from.u8();
    std::string said = from.text();

    from.ended();

    if (kind > static_cast<std::uint8_t>(Kind::other)) throw broken("a trouble of no kind.");

    return Trouble{static_cast<Kind>(kind), std::move(said)};
}

// The payload of a failed frame for `trouble`.
Bytes failed_with(const Trouble& trouble) { return Writing().u8(static_cast<std::uint8_t>(trouble.kind())).text(trouble.what()).done(); }

// The picture of `size` in `from`, as the process sent it: the width, the height, RGBA, straight
// alpha. Throws `Trouble` malformed for another size than asked or a length that is not the
// size's.
media::Pixels pictured(Reading& from, media::Size size) {
    const std::uint32_t wide = from.u32();
    const std::uint32_t high = from.u32();

    if (wide != size.width || high != size.height) throw broken("a picture of another size than asked.");

    const std::span<const std::byte> rgba = from.rest();

    if (rgba.size() != std::size_t{wide} * high * 4) throw broken("a picture of a wrong length.");

    std::vector<media::Still> stills(1);

    stills.front().rgba.assign(rgba.begin(), rgba.end());

    return media::Pixels{size, std::move(stills)};
}

// The pages of a document as its process told them: their number, then the width and height of
// each. Throws `Trouble` malformed for more pages than the frame holds or a page of no area.
std::vector<docs::Page> paged(std::span<const std::byte> payload) {
    Reading from{payload};
    const std::uint32_t count = from.u32();

    if (std::uint64_t{count} * 16 > payload.size()) throw broken("more pages than it sent.");

    std::vector<docs::Page> made(count);

    for (docs::Page& one : made) {
        one.width = from.f64();
        one.height = from.f64();

        if (one.width <= 0 || one.height <= 0) throw broken("a page of no area.");
    }

    from.ended();

    return made;
}

}

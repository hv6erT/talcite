#include "gifting.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "tal/trouble.hpp"

// Our own GIF decoder, ported from talcite-adapters, built into tal-media: it always decodes, with
// no port. A file that is no GIF, a broken header and a canvas over `ceiling` are refused before
// anything is allocated (GIF allows 65535 by 65535, seventeen terabytes of RGBA); a truncated file
// or an unknown block ends the stream quietly, since the top half of a picture is a better answer
// than nothing, and a file manager also looks at files that are still being copied.
namespace tal::media::gifting {

namespace {

// The width and height of the canvas or of a rectangle on it.
struct Extent {
    int wide = 0;
    int high = 0;
};

// A place on the canvas.
struct Corner {
    int x = 0;
    int y = 0;
};

// A rectangle on the canvas: its corner and its extent.
struct Area {
    Corner at;
    Extent size;
};

// A byte of the input as an unsigned 8-bit number.
[[nodiscard]] constexpr std::uint8_t sample(std::byte one) { return std::to_integer<std::uint8_t>(one); }

// An 8-bit number as a byte of the canvas.
[[nodiscard]] constexpr std::byte stored(std::uint8_t one) { return std::byte{one}; }

constexpr std::size_t bands = 4;

constexpr std::uint8_t image = 0x2C;
constexpr std::uint8_t extra = 0x21;
constexpr std::uint8_t ending = 0x3B;

constexpr std::uint8_t steering = 0xF9;

// The standard says "as fast as possible", but in practice that means a hundred frames per second
// and a strobe. For twenty years browsers have read zero and one hundredth as ten hundredths, and
// half of the GIFs on the web are made for that, so we follow the world rather than the letter.
constexpr int slowest = 1;
constexpr int instead = 10;

// Interlacing: four passes over the rows, each with its own start and step. The data in the file
// comes in order, but the rows of the picture do not, and that is the easiest place to stumble.
constexpr int starts[4] = {0, 4, 2, 1};
constexpr int steps[4] = {8, 8, 4, 2};

// A cursor over the input with a single spoil flag that, once set, never clears. Every read past
// the end sets it and returns zero, so the loop checks one bit once per block instead of twenty
// length checks scattered over the code. A forgotten check of that kind is exactly what kills
// decoders.
struct Reader {
    std::span<const std::byte> what;
    std::size_t at = 0;
    bool spoiled = false;

    // One byte; zero past the end, which sets the flag.
    std::uint8_t byte() {
        if (at >= what.size()) {
            spoiled = true;

            return 0;
        }

        return sample(what[at++]);
    }

    // A little-endian 16-bit number, the byte order of every GIF field; a byte past the end counts
    // as zero.
    unsigned word() {
        const unsigned low = byte();

        return low | (static_cast<unsigned>(byte()) << 8);
    }

    // A view of the next `much` bytes. Past the end it returns nothing, moves to the end and sets
    // the flag.
    std::span<const std::byte> take(std::size_t much) {
        if (much > what.size() - at) {
            spoiled = true;
            at = what.size();

            return {};
        }

        const std::span<const std::byte> made = what.subspan(at, much);

        at += much;

        return made;
    }
};

struct Table {
    std::array<std::uint8_t, 256 * 3> tones{};
    int much = 0;
};

// A color table (the global palette or a local one) of `much` RGB entries. A table cut off by the
// end of the input comes back empty, and the reader's flag is set.
[[nodiscard]] Table tabled(Reader& from, int much) {
    Table made;

    const std::span<const std::byte> got = from.take(static_cast<std::size_t>(much) * 3);

    if (from.spoiled) return made;

    made.much = much;

    for (std::size_t at = 0; at < got.size(); ++at) made.tones[at] = sample(got[at]);

    return made;
}

// What the Graphic Control Extension carries, which applies to the next image, not the one it
// follows. After each image it returns to the defaults, because the extension applies once.
struct Control {
    std::chrono::milliseconds after{instead * 10};
    int clear = -1;
    int disposal = 0;
};

// Image data lies in sub-blocks: a length byte, that many bytes, and so on up to a zero. They are
// first glued into one contiguous buffer, and only then are the bits read. A bit reader aware of
// sub-blocks would save one copy and be much harder to verify, and compressed GIF data is small.
[[nodiscard]] Bytes glued(Reader& from) {
    Bytes made;

    while (true) {
        const std::uint8_t much = from.byte();

        if (from.spoiled || much == 0) break;

        // The last sub-block may be cut off together with the file. We take as much of it as there
        // really is; unpacking counts how many pixels came out of it anyway.
        const std::size_t left = from.what.size() - from.at;
        const std::span<const std::byte> got = from.take(std::min<std::size_t>(much, left));

        made.insert(made.end(), got.begin(), got.end());

        if (much > left) {
            from.spoiled = true;

            break;
        }
    }

    return made;
}

// GIF's LZW. Fixed-size tables, not a single allocation per code. Expansion follows the prefix
// chain onto a stack and pops it in reverse; the chain cannot be infinite, because every entry
// points back to a number smaller than its own, so the loop ends by construction rather than by
// guarding. The output is bounded in advance by the frame size: when it fills up, the remaining
// codes are dropped instead of written past it.
//
// Returns the number of pixels written, because for a file cut off halfway that number is the whole
// answer: that much of the picture is real, and the rest has to stay empty rather than be filled
// with color zero.
[[nodiscard]] std::size_t unpacked(std::span<const std::byte> from, int least,
                                   std::span<std::uint8_t> into) {
    if (least < 2 || least > 8 || into.empty()) return 0;

    const unsigned clear = 1u << least;
    const unsigned stop = clear + 1;
    const unsigned none = 4096;

    std::array<std::uint16_t, 4096> prefix{};
    std::array<std::uint8_t, 4096> suffix{};
    std::array<std::uint8_t, 4096> stack{};

    unsigned free = clear + 2;
    unsigned width = static_cast<unsigned>(least) + 1;
    unsigned last = none;
    std::uint8_t first = 0;

    std::size_t made = 0;
    std::size_t at = 0;
    unsigned held = 0;
    unsigned bits = 0;

    while (true) {
        while (bits < width) {
            // A truncated stream is not the same as a broken one: we return as much of the frame
            // as came out, because a cut-off GIF should show its beginning, not nothing.
            if (at >= from.size()) return made;

            held |= static_cast<unsigned>(sample(from[at++])) << bits;
            bits += 8;
        }

        const unsigned code = held & ((1u << width) - 1);

        held >>= width;
        bits -= width;

        if (code == clear) {
            free = clear + 2;
            width = static_cast<unsigned>(least) + 1;
            last = none;

            continue;
        }

        if (code == stop) break;

        if (code > free) return made;

        std::size_t deep = 0;
        unsigned walk = code;

        if (code == free) {
            if (last == none) return made;

            stack[deep++] = first;
            walk = last;
        }

        while (walk >= clear) {
            if (deep >= stack.size()) return made;

            stack[deep++] = suffix[walk];
            walk = prefix[walk];
        }

        first = static_cast<std::uint8_t>(walk);
        stack[deep++] = first;

        while (deep > 0) {
            if (made >= into.size()) return made;

            into[made++] = stack[--deep];
        }

        if (last != none && free < 4096) {
            prefix[free] = static_cast<std::uint16_t>(last);
            suffix[free] = first;
            ++free;

            if (free == (1u << width) && width < 12) ++width;
        }

        last = code;
    }

    return made;
}

// The byte offset of pixel (x, y) in the RGBA canvas.
[[nodiscard]] std::size_t spot(Extent screen, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(screen.wide) +
            static_cast<std::size_t>(x)) *
           bands;
}

// Disposal 2: clears the rectangle to transparent black, clipped to the canvas.
void wiped(Bytes& canvas, Extent screen, Area where) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y) {
        if (y < 0 || y >= screen.high) continue;

        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (x < 0 || x >= screen.wide) continue;

            const std::size_t at = spot(screen, x, y);

            for (std::size_t band = 0; band < bands; ++band) canvas[at + band] = std::byte{0};
        }
    }
}

// A copy of the pixels of the rectangle, kept for disposal 3. Pixels outside the canvas come out as
// transparent black, so the copy always has the full size of the rectangle.
[[nodiscard]] Bytes copied(const Bytes& canvas, Extent screen, Area where) {
    Bytes made;

    made.reserve(static_cast<std::size_t>(where.size.wide) *
                 static_cast<std::size_t>(where.size.high) * bands);

    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (y < 0 || y >= screen.high || x < 0 || x >= screen.wide) {
                made.insert(made.end(), bands, std::byte{0});

                continue;
            }

            const std::size_t at = spot(screen, x, y);

            made.insert(made.end(), canvas.begin() + static_cast<std::ptrdiff_t>(at),
                        canvas.begin() + static_cast<std::ptrdiff_t>(at + bands));
        }

    return made;
}

// Disposal 3: puts the pixels saved by `copied` back into the rectangle, clipped to the canvas.
void restored(Bytes& canvas, Extent screen, Area where, const Bytes& kept) {
    std::size_t from = 0;

    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x, from += bands) {
            if (y < 0 || y >= screen.high || x < 0 || x >= screen.wide) continue;

            if (from + bands > kept.size()) return;

            const std::size_t at = spot(screen, x, y);

            for (std::size_t band = 0; band < bands; ++band)
                canvas[at + band] = kept[from + band];
        }
}

// Laying an unpacked frame onto the canvas. Three things at once: interlacing, the transparent
// index and clipping to the canvas, because the rectangle announced in the file need not fit.
void laid(Bytes& canvas, Extent screen, const std::vector<std::uint8_t>& marks,
          std::size_t filled, Area where, const Table& palette, int clear, bool woven) {
    int row = 0;

    for (int pass = 0; pass < (woven ? 4 : 1); ++pass) {
        const int from = woven ? starts[pass] : 0;
        const int step = woven ? steps[pass] : 1;

        for (int y = from; y < where.size.high; y += step, ++row) {
            const int onto = where.at.y + y;

            if (onto < 0 || onto >= screen.high) continue;

            for (int x = 0; x < where.size.wide; ++x) {
                const int across = where.at.x + x;

                if (across < 0 || across >= screen.wide) continue;

                const std::size_t which = static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(where.size.wide) +
                                          static_cast<std::size_t>(x);

                if (which >= filled) continue;

                const int mark = marks[which];

                if (mark == clear || mark >= palette.much) continue;

                const std::size_t at = spot(screen, across, onto);
                const std::size_t tone = static_cast<std::size_t>(mark) * 3;

                canvas[at + 0] = stored(palette.tones[tone + 0]);
                canvas[at + 1] = stored(palette.tones[tone + 1]);
                canvas[at + 2] = stored(palette.tones[tone + 2]);
                canvas[at + 3] = std::byte{255};
            }
        }
    }
}

// Whether the bytes start with a GIF signature: "GIF87a" or "GIF89a".
[[nodiscard]] bool marked(std::span<const std::byte> what) {
    if (what.size() < 6) return false;

    const char* named = "GIF8";

    for (std::size_t at = 0; at < 4; ++at)
        if (sample(what[at]) != static_cast<std::uint8_t>(named[at])) return false;

    const std::uint8_t age = sample(what[4]);

    return (age == '7' || age == '9') && sample(what[5]) == 'a';
}

}

// Decodes a GIF and hands each frame to `each` as the whole RGBA canvas with the frame laid on it,
// together with the frame's delay. Throws `Trouble` only before decoding: malformed for no GIF or a
// broken header, full for a canvas over `ceiling`; a truncated or broken stream ends quietly after
// the frames that came out. Stops early when `each` returns false.
void gifted(std::span<const std::byte> what, const Taking& each) {
    if (!marked(what)) throw Trouble{Kind::malformed, "Not a GIF."};

    Reader from{what};

    from.take(6);

    const unsigned wide = from.word();
    const unsigned high = from.word();
    const std::uint8_t packed = from.byte();

    from.byte();
    from.byte();

    if (from.spoiled || wide == 0 || high == 0) throw Trouble{Kind::malformed, "The header of the GIF is broken."};

    const Extent screen{static_cast<int>(wide), static_cast<int>(high)};

    if (static_cast<std::uint64_t>(wide) * static_cast<std::uint64_t>(high) > ceiling) throw Trouble{Kind::full, "The canvas of the GIF is over the ceiling."};

    Table palette;

    if ((packed & 0x80) != 0) palette = tabled(from, 2 << (packed & 7));

    // A palette cut off together with the file means a truncated file, not a broken one: the stream
    // ends without a frame and without a message, like any later truncation.
    if (from.spoiled) return;

    // The canvas is created once and lives to the end; frames are laid onto it one after another,
    // and each goes to the caller as the same buffer. A caller who wants to keep a frame copies it;
    // one who only wants to scale it down and drop it pays for nothing.
    Bytes canvas(static_cast<std::size_t>(wide) * static_cast<std::size_t>(high) * bands,
                 std::byte{0});

    Bytes kept;
    Area saved{};
    Area last{};
    Control control;
    int disposal = 0;

    while (!from.spoiled) {
        const std::uint8_t lead = from.byte();

        if (from.spoiled || lead == ending) break;

        if (lead == extra) {
            const std::uint8_t mark = from.byte();
            const Bytes said = glued(from);

            if (mark == steering && said.size() >= 4) {
                const std::uint8_t how = sample(said[0]);
                const int hundredths =
                    sample(said[1]) | (static_cast<int>(sample(said[2])) << 8);

                control.disposal = (how >> 2) & 7;
                control.clear = (how & 1) != 0 ? static_cast<int>(sample(said[3])) : -1;
                control.after = std::chrono::milliseconds{
                    (hundredths <= slowest ? instead : hundredths) * 10};
            }

            continue;
        }

        // An unrecognized byte means we are lost in the file. Further guessing would only produce
        // garbage, so we return what we already have.
        if (lead != image) break;

        const unsigned left = from.word();
        const unsigned top = from.word();
        const unsigned across = from.word();
        const unsigned down = from.word();
        const std::uint8_t flags = from.byte();

        if (from.spoiled) break;

        Table local = palette;

        if ((flags & 0x80) != 0) local = tabled(from, 2 << (flags & 7));

        const bool woven = (flags & 0x40) != 0;
        const std::uint8_t least = from.byte();
        const Bytes codes = glued(from);

        // A truncated file is not an empty file. A file manager also looks at files being copied
        // and at damaged ones, and there the top half of a picture is a better answer than nothing.
        // So we take what arrived, show it once and stop.
        const bool cut = from.spoiled;

        const std::size_t much =
            static_cast<std::size_t>(across) * static_cast<std::size_t>(down);

        if (much == 0 || much > ceiling) break;

        const Area where{{static_cast<int>(left), static_cast<int>(top)},
                             {static_cast<int>(across), static_cast<int>(down)}};

        // Disposal of the previous frame, not of this one: the standard disposes of a frame only
        // after it has been shown, so this happens just before the next one goes on the canvas.
        if (disposal == 2) wiped(canvas, screen, last);
        else if (disposal == 3 && !kept.empty()) restored(canvas, screen, saved, kept);

        if (control.disposal == 3) {
            // Only the rectangle is saved, not the whole canvas: nothing else will change.
            kept = copied(canvas, screen, where);
            saved = where;
        } else {
            kept.clear();
        }

        std::vector<std::uint8_t> marks(much, 0);

        const std::size_t filled = unpacked(codes, least, marks);

        if (filled == 0) break;

        laid(canvas, screen, marks, filled, where, local, control.clear, woven);

        if (!each(canvas, Size{wide, high}, control.after)) return;

        if (cut) break;

        disposal = control.disposal;
        last = where;
        control = Control{};
    }
}

}

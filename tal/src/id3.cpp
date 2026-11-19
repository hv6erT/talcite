#include "tagging.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/trouble.hpp"

#include "inflating.hpp"

// ID3v2 (2.2, 2.3, 2.4) and ID3v1, ported from talcite-adapters' tags. A field key is the frame
// identifier in the 2.3/2.4 spelling: 2.2 frames have three letters and are rewritten to four
// (TT2 → TIT2, PIC → APIC), because it is the same vocabulary in an older spelling and twa should
// parse only one. Nothing else is translated: TYER from 2.3 and TDRC from 2.4 stay as they are,
// and "(13)Pop" in TCON stays a string; the caller gives it meaning.
//
// Frames with a description (TXXX, WXXX, COMM, USLT, UFID) carry the description in the key after a
// colon, as in "TXXX:MusicBrainz Album Id", because only the pair says what the field is. A frame
// with several values (2.4 separates them with a zero) gives several fields with the same key, in
// file order. Pictures (APIC, PIC) are not fields: they go to the picture list, and their bytes are
// read only on request.
//
// Compressed frames are inflated by our own `Inflater` and then parsed like any other. Encrypted
// frames are not parsed, nor is the rest of the vocabulary (PRIV, GEOB, POPM and the like),
// because it does not carry the identity of a track.
namespace tal::meta::tagging {

namespace {

// How much of a picture frame is read to parse its header: the type, the role and a description of
// any reasonable length fit, and the first bytes of the picture, from which its type is recognized
// when the frame does not say it.
constexpr std::size_t picture_head = 64 * 1024;

// How many compressed bytes the inflater is fed at a time: deflate expands a byte at most about a
// thousand times, so one piece cannot go far past `most_block` before the limit is checked.
constexpr std::size_t inflated_piece = 1024;

// A byte as an unsigned number, ready to be shifted into a wider integer.
[[nodiscard]] constexpr std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// A "synchsafe" size: seven bits per byte, the eighth always zero. A byte with the eighth bit set
// means the size is not synchsafe.
[[nodiscard]] bool synchsafe(std::span<const std::byte> four) {
    return std::all_of(four.begin(), four.end(), [](std::byte one) { return sample(one) < 0x80; });
}

// A synchsafe number: seven bits from each of four bytes. Zero when fewer arrived (a tag truncated
// in its header); the cursor they were taken from reports the damage.
[[nodiscard]] std::uint32_t synchsafe_value(std::span<const std::byte> four) {
    if (four.size() < 4) return 0;

    return (sample(four[0]) << 21) | (sample(four[1]) << 14) | (sample(four[2]) << 7) | sample(four[3]);
}

// Four bytes as an ordinary big-endian number (2.3 sizes, and 2.4 sizes that are not synchsafe);
// zero when fewer than four arrived.
[[nodiscard]] std::uint32_t plain(std::span<const std::byte> four) {
    if (four.size() < 4) return 0;

    return (sample(four[0]) << 24) | (sample(four[1]) << 16) | (sample(four[2]) << 8) | sample(four[3]);
}

// Undoing unsynchronization: every FF 00 turns back into FF.
[[nodiscard]] Bytes resynced(const Bytes& given) {
    Bytes made;

    made.reserve(given.size());

    for (std::size_t at = 0; at < given.size(); ++at) {
        made.push_back(given[at]);

        if (sample(given[at]) == 0xFF && at + 1 < given.size() && sample(given[at + 1]) == 0) ++at;
    }

    return made;
}

// The content of a compressed frame (a zlib stream) inflated; none when the stream is damaged or
// cut short, or when it would come out over `most_block` or over what is left of the file's budget
// of unpacking (`unpackable`), against which it is counted. It is fed in pieces of
// `inflated_piece` bytes, so that a stream made to inflate without end stops at the limit, not
// after it.
[[nodiscard]] std::optional<Bytes> inflated(std::span<const std::byte> given, Gathered& out) {
    inflating::Inflater inflater{inflating::Wrapping::zlib};
    Bytes made;

    const std::uint64_t most = std::min<std::uint64_t>(most_block, out.unpackable());

    try {
        for (std::size_t at = 0; at < given.size(); at += inflated_piece) {
            const Bytes more = inflater.fed(given.subspan(at, std::min(inflated_piece, given.size() - at)));

            out.unpacked(more.size());

            if (more.size() > most - made.size()) return std::nullopt;

            made.insert(made.end(), more.begin(), more.end());
        }
    } catch (const Trouble&) {
        return std::nullopt;
    }

    if (!inflater.ended()) return std::nullopt;

    return made;
}

// Whether a character may stand in a frame identifier: an upper-case letter or a digit.
[[nodiscard]] bool frame_char(std::uint32_t one) { return (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9'); }

// The 2.2 vocabulary in the 2.3 spelling, only for the frames that are parsed; the rest is skipped
// anyway.
constexpr std::array<std::pair<std::string_view, std::string_view>, 38> renamed{{
    {"BUF", "RBUF"}, {"COM", "COMM"}, {"PIC", "APIC"}, {"TAL", "TALB"}, {"TBP", "TBPM"}, {"TCM", "TCOM"},
    {"TCO", "TCON"}, {"TCP", "TCMP"}, {"TCR", "TCOP"}, {"TDA", "TDAT"}, {"TEN", "TENC"}, {"TKE", "TKEY"},
    {"TLA", "TLAN"}, {"TLE", "TLEN"}, {"TOA", "TOPE"}, {"TOR", "TORY"}, {"TOT", "TOAL"}, {"TP1", "TPE1"},
    {"TP2", "TPE2"}, {"TP3", "TPE3"}, {"TP4", "TPE4"}, {"TPA", "TPOS"}, {"TPB", "TPUB"}, {"TRC", "TSRC"},
    {"TRK", "TRCK"}, {"TS2", "TSO2"}, {"TSA", "TSOA"}, {"TSC", "TSOC"}, {"TSP", "TSOP"}, {"TST", "TSOT"},
    {"TT1", "TIT1"}, {"TT2", "TIT2"}, {"TT3", "TIT3"}, {"TXX", "TXXX"}, {"TYE", "TYER"}, {"UFI", "UFID"},
    {"ULT", "USLT"}, {"WXX", "WXXX"},
}};

// The frame identifier in the 2.3/2.4 spelling. A four-letter identifier stays as it is; a
// three-letter 2.2 one is looked up in `renamed`.
[[nodiscard]] std::string spelled(std::string id) {
    if (id.size() != 3) return id;

    for (const auto& [old, now] : renamed)
        if (id == old) return std::string{now};

    // A 2.2 frame outside the vocabulary keeps its three letters; there is no 2.3 spelling for it.
    return id;
}

// The end of a string: its length (`text`) and its length with the terminator (`taken`).
struct Cut {
    std::size_t text;
    std::size_t taken;
};

// Where the first string in `given` ends in the given encoding: at two aligned zeros for UTF-16
// (encodings 1 and 2), at one zero for Latin-1 and UTF-8. Without a terminator both lengths are the
// whole span.
[[nodiscard]] Cut terminated(std::span<const std::byte> given, std::uint8_t encoding) {
    if (encoding == 1 || encoding == 2) {
        for (std::size_t at = 0; at + 1 < given.size(); at += 2)
            if (sample(given[at]) == 0 && sample(given[at + 1]) == 0) return {at, at + 2};

        return {given.size(), given.size()};
    }

    for (std::size_t at = 0; at < given.size(); ++at)
        if (sample(given[at]) == 0) return {at, at + 1};

    return {given.size(), given.size()};
}

// A string in the frame's encoding. With UTF-16 and a byte order mark (encoding 1) each value may
// have its own mark; a value without one takes the order of the previous one, which is how programs
// write it in practice.
[[nodiscard]] std::string decoded(std::span<const std::byte> given, std::uint8_t encoding, bool& big) {
    switch (encoding) {
        case 0: return latin1(given);
        case 1:
            if (given.size() >= 2 && sample(given[0]) == 0xFE && sample(given[1]) == 0xFF) {
                big = true;

                return utf16(given.subspan(2), true);
            }

            if (given.size() >= 2 && sample(given[0]) == 0xFF && sample(given[1]) == 0xFE) {
                big = false;

                return utf16(given.subspan(2), false);
            }

            return utf16(given, big);
        case 2: return utf16(given, true);
        default: return utf8(given);
    }
}

// All values of a text frame: separated by the terminator, without an empty tail.
[[nodiscard]] std::vector<std::string> values(std::span<const std::byte> given, std::uint8_t encoding) {
    std::vector<std::string> made;
    bool big = false;

    while (!given.empty()) {
        const Cut cut = terminated(given, encoding);

        made.push_back(decoded(given.first(cut.text), encoding, big));

        given = given.subspan(cut.taken);
    }

    while (!made.empty() && made.back().empty()) made.pop_back();

    return made;
}

// A frame to parse: the identifier in the 2.3 spelling, the tag version (2.2 lays out pictures
// differently) and whether unsynchronization has to be undone on the content.
struct Frame {
    std::string id;
    std::uint8_t version = 3;
    bool resync = false;
};

// A picture: only its header (type, role, description) is parsed from the piece; the bytes are read
// when someone asks for them.
Task<void> picture(Walk& body, const Frame& frame, Gathered& out) {
    const std::uint64_t size = body.left();
    const Bytes head = co_await body.take(static_cast<std::size_t>(std::min<std::uint64_t>(size, picture_head)));

    if (head.empty()) co_return;

    const std::uint8_t encoding = static_cast<std::uint8_t>(sample(head[0]));
    std::size_t at = 1;
    std::string type;

    if (frame.version == 2) {
        if (head.size() < 5) co_return out.damaged();

        type = latin1(std::span{head}.subspan(1, 3));
        at = 4;
    } else {
        const Cut cut = terminated(std::span{head}.subspan(1), 0);

        if (cut.taken == head.size() - 1 && cut.text == cut.taken) co_return out.damaged();

        type = latin1(std::span{head}.subspan(1, cut.text));
        at = 1 + cut.taken;
    }

    // A picture given as a link instead of bytes: there is nothing to hand out.
    if (type == "-->") co_return;

    if (at >= head.size()) co_return out.damaged();

    const std::uint32_t kind = sample(head[at++]);
    const Cut said = terminated(std::span{head}.subspan(at), encoding);

    if (said.text == said.taken && at + said.taken == head.size() && head.size() < size) co_return out.damaged();

    bool big = false;
    std::string described = decoded(std::span{head}.subspan(at, said.text), encoding, big);

    at += said.taken;

    const std::uint64_t data = size - at;

    Picture one{role_of(kind), image_named(type), std::move(described), static_cast<std::size_t>(data)};

    if (one.type.empty() || one.type == "image/") {
        const std::size_t seen = std::min<std::size_t>(head.size() - at, 16);

        one.type = image_type(std::span{head}.subspan(at, seen));
    }

    if (!out.pictured(std::move(one))) co_return;

    // The bytes start at the data within the frame: part of them is already in the head, the rest
    // comes straight from the file.
    body.seek(body.end() - data);

    Bytes taken = co_await body.take(static_cast<std::size_t>(data));

    out.took(std::move(taken));
}

// Parses the content of one frame: APIC goes to the picture list; URL frames, UFID, TXXX and WXXX,
// COMM and USLT, and text frames become fields; any other frame is ignored. Content over
// `most_text` is skipped, and while the bytes of a picture are wanted only pictures are looked at.
Task<void> frame_read(Walk& body, const Frame& frame, Gathered& out) {
    const std::string& id = frame.id;

    if (id == "APIC") {
        co_await picture(body, frame, out);

        co_return;
    }

    // When looking for the bytes of a picture, the fields are not needed and are not read at all.
    if (out.wanted) co_return;

    if (body.left() > most_text) co_return;

    const Bytes raw = co_await body.take(static_cast<std::size_t>(body.left()));
    const std::span<const std::byte> all{raw};

    if (all.empty()) co_return;

    // URL frames: just the address, in Latin-1, without an encoding byte.
    if (id[0] == 'W' && id != "WXXX") {
        if (std::string url = latin1(all); !url.empty()) out.field(Scheme::id3v2, id, std::move(url));

        co_return;
    }

    const std::uint8_t encoding = static_cast<std::uint8_t>(sample(all[0]));
    const std::span<const std::byte> rest = all.subspan(1);

    if (id == "UFID") {
        const Cut owner = terminated(all, 0);

        out.field(Scheme::id3v2, "UFID:" + latin1(all.first(owner.text)), latin1(all.subspan(owner.taken)));

        co_return;
    }

    if (encoding > 3) co_return out.damaged();

    if (id == "TXXX" || id == "WXXX") {
        const Cut cut = terminated(rest, encoding);
        bool big = false;
        const std::string key = id + ":" + decoded(rest.first(cut.text), encoding, big);

        if (id == "WXXX") {
            if (std::string url = latin1(rest.subspan(cut.taken)); !url.empty())
                out.field(Scheme::id3v2, key, std::move(url));

            co_return;
        }

        for (std::string& one : values(rest.subspan(cut.taken), encoding))
            out.field(Scheme::id3v2, key, std::move(one));

        co_return;
    }

    if (id == "COMM" || id == "USLT") {
        if (rest.size() < 3) co_return out.damaged();

        const std::span<const std::byte> text = rest.subspan(3);
        const Cut cut = terminated(text, encoding);
        bool big = false;
        const std::string said = decoded(text.first(cut.text), encoding, big);
        std::string body_text = decoded(text.subspan(cut.taken), encoding, big);

        if (!body_text.empty()) out.field(Scheme::id3v2, said.empty() ? id : id + ":" + said, std::move(body_text));

        co_return;
    }

    if (id[0] == 'T')
        for (std::string& one : values(rest, encoding)) out.field(Scheme::id3v2, id, std::move(one));
}

// Whether a frame header, padding or the end lies at `at` in the content. It recognizes 2.4 sizes
// written as plain numbers instead of synchsafe ones (old iTunes).
Task<bool> framed_at(Reader& from, std::uint64_t at, std::uint64_t end) {
    if (at == end) co_return true;

    if (at > end || end - at < 4) co_return at <= end;

    const Bytes four = co_await from.read(at, 4);

    if (four.size() < 4) co_return false;

    if (sample(four[0]) == 0) co_return true;

    co_return std::all_of(four.begin(), four.end(), [](std::byte one) { return frame_char(sample(one)); });
}

// The frames of a tag, from the current position to the end of the cursor. The content of a frame
// is unsynchronized first and inflated after, as 2.4 orders it.
Task<void> frames(Walk& body, std::uint8_t version, bool resync_all, Gathered& out) {
    const std::size_t named = version == 2 ? 3 : 4;
    const std::size_t headed = version == 2 ? 6 : 10;

    while (body.left() >= headed && !out.done()) {
        const std::uint64_t began = body.at();

        co_await body.need(headed);

        if (body.ahead(std::string_view{"\0", 1})) co_return;

        const std::string raw_id = body.chars(named);

        if (!std::all_of(raw_id.begin(), raw_id.end(), [](char one) { return frame_char(static_cast<unsigned char>(one)); }))
            co_return out.damaged();

        std::uint32_t size = 0;
        std::uint16_t flags = 0;

        if (version == 2) {
            size = body.be24();
        } else {
            const Bytes four = body.bytes(4);

            if (version == 3) {
                size = plain(four);
            } else if (!synchsafe(four)) {
                size = plain(four);
            } else {
                size = synchsafe_value(four);

                const std::uint32_t other = plain(four);

                if (other != size) {
                    const bool fits = co_await framed_at(body.reader(), began + headed + size, body.end());

                    if (!fits) {
                        const bool fits_other = co_await framed_at(body.reader(), began + headed + other, body.end());

                        if (fits_other) size = other;
                    }
                }
            }

            flags = body.be16();
        }

        if (size > body.left()) out.damaged();

        Walk part = body.part(size);
        Frame frame{spelled(raw_id), version, false};
        bool compressed = false;

        if (version == 3) {
            if ((flags & 0x0040) != 0) continue;

            compressed = (flags & 0x0080) != 0;

            // The size after inflating comes first; it is not trusted, since inflating stops at
            // `most_block` anyway.
            if (compressed) part.skip(4);

            if ((flags & 0x0020) != 0) part.skip(1);
        } else if (version == 4) {
            if ((flags & 0x0004) != 0) continue;

            compressed = (flags & 0x0008) != 0;

            // A compressed frame has to state its size after inflating (the data length indicator).
            if (compressed && (flags & 0x0001) == 0) {
                out.damaged();

                continue;
            }

            if ((flags & 0x0040) != 0) part.skip(1);

            if ((flags & 0x0001) != 0) part.skip(4);

            frame.resync = resync_all || (flags & 0x0002) != 0;
        }

        if (frame.resync || compressed) {
            if (part.left() > most_block) {
                if (compressed) out.damaged();

                continue;
            }

            Bytes content = co_await part.take(static_cast<std::size_t>(part.left()));

            if (frame.resync) content = resynced(content);

            if (compressed) {
                std::optional<Bytes> made = inflated(content, out);

                if (!made) {
                    out.damaged();

                    continue;
                }

                content = std::move(*made);
            }

            Held held{std::move(content)};
            Walk inner = co_await Walk::over(held);

            co_await frame_read(inner, frame, out);

            continue;
        }

        co_await frame_read(part, frame, out);
    }
}

// Whether ten bytes form a valid ID3v2 header: "ID3", version 2 to 4, a revision other than FF and
// a synchsafe size.
[[nodiscard]] bool headed(std::span<const std::byte> head) {
    if (head.size() < 10 || std::memcmp(head.data(), "ID3", 3) != 0) return false;

    const std::uint32_t version = sample(head[3]);

    return version >= 2 && version <= 4 && sample(head[4]) != 0xFF && synchsafe(head.subspan(6, 4));
}

}

// Whether an ID3v2 header starts at the current position.
Task<bool> id3v2_at(Walk& walk) {
    if (walk.left() < 10) co_return false;

    const Bytes head = co_await walk.reader().read(walk.at(), 10);

    co_return headed(head);
}

// ID3v2 from `walk.at()`, including several tags one after another; returns the position past the
// last one.
Task<std::uint64_t> id3v2(Walk& walk, Gathered& out) {
    while (!out.done()) {
        const bool tagged = co_await id3v2_at(walk);

        if (!tagged) break;

        co_await walk.need(10);

        const Bytes head = walk.bytes(10);
        const std::uint8_t version = static_cast<std::uint8_t>(sample(head[3]));
        const std::uint8_t flags = static_cast<std::uint8_t>(sample(head[5]));
        const std::uint32_t size = synchsafe_value(std::span{head}.subspan(6, 4));
        const std::uint64_t ending = walk.at() + size + (version == 4 && (flags & 0x10) != 0 ? 10 : 0);

        if (size > walk.left()) out.damaged();

        Walk body = walk.part(size);

        walk.seek(std::min(ending, walk.end()));

        // 2.2 with the whole tag compressed: the standard does not say how, so there is nothing to
        // read.
        if (version == 2 && (flags & 0x40) != 0) continue;

        // Unsynchronization of the whole tag in 2.2 and 2.3 is undone on the whole tag in memory.
        // In 2.4 it only announces that every frame is unsynchronized, and each frame says so in
        // its own flags as well.
        if (version < 4 && (flags & 0x80) != 0) {
            if (body.left() > most_block) {
                out.damaged();

                continue;
            }

            const Bytes raw = co_await body.take(static_cast<std::size_t>(body.left()));
            Held held{resynced(raw)};
            Walk inner = co_await Walk::over(held);

            if (version == 3 && (flags & 0x40) != 0) {
                co_await inner.need(4);

                inner.skip(inner.be32());
            }

            co_await frames(inner, version, false, out);

            continue;
        }

        if (version == 3 && (flags & 0x40) != 0) {
            co_await body.need(4);

            body.skip(body.be32());
        }

        if (version == 4 && (flags & 0x40) != 0) {
            co_await body.need(4);

            const Bytes four = body.bytes(4);

            body.skip(synchsafe_value(four) >= 4 ? synchsafe_value(four) - 4 : 0);
        }

        const bool resync_all = version == 4 && (flags & 0x80) != 0;

        co_await frames(body, version, resync_all, out);
    }

    co_return walk.at();
}

// --- ID3v1 ---------------------------------------------------------------------------------------

// ID3v1 (and 1.1) in the last 128 bytes before `end` of a file of `size` bytes; returns the new end
// (the start of the tag), or `end` when there is no tag. While the bytes of a picture are wanted,
// only the end is computed.
Task<std::uint64_t> id3v1(Reader& from, std::uint64_t size, std::uint64_t end, Gathered& out) {
    if (end < 128) co_return end;

    Walk walk{from, size, end - 128, end};

    co_await walk.need(128);

    const Bytes tag = walk.bytes(128);

    if (tag.size() < 128 || std::memcmp(tag.data(), "TAG", 3) != 0) co_return end;

    if (out.wanted) co_return end - 128;

    const std::span<const std::byte> all{tag};

    // A fixed-width string: up to a zero, without trailing padding spaces. The code page is unknown
    // by design; Latin-1 is what can be read without guessing.
    const auto text = [&](std::size_t at, std::size_t much) {
        std::string made = latin1(all.subspan(at, much));

        while (!made.empty() && made.back() == ' ') made.pop_back();

        return made;
    };

    const auto put = [&](std::string_view key, std::string value) {
        if (!value.empty()) out.field(Scheme::id3v1, key, std::move(value));
    };

    put("title", text(3, 30));
    put("artist", text(33, 30));
    put("album", text(63, 30));
    put("year", text(93, 4));

    // 1.1: a zero in the next-to-last byte of the comment and a nonzero last byte give the track
    // number.
    const bool tracked = sample(all[125]) == 0 && sample(all[126]) != 0;

    put("comment", text(97, tracked ? 28 : 30));

    if (tracked) out.field(Scheme::id3v1, "track", static_cast<std::int64_t>(sample(all[126])));

    if (sample(all[127]) != 0xFF) out.field(Scheme::id3v1, "genre", static_cast<std::int64_t>(sample(all[127])));

    co_return end - 128;
}

}

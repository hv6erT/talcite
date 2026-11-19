#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "tagging.hpp"

// ISO-BMFF (m4a, m4b, mp4, mov), ported from talcite-adapters' tags. The file is a tree of atoms
// (size, four-character type, content). The fields sit in moov/udta/meta/ilst: each atom there is
// one field with a four-character key ("©nam", "aART", "trkn"), and a program's own field is "----"
// with a namespace and a name, which are joined into a key like
// "----:com.apple.iTunes:MusicBrainz Album Id". The values are carried by one or more "data" atoms
// with a type: UTF-8 and UTF-16 text, signed and unsigned numbers, pictures (JPEG, PNG, BMP).
//
// The number/total pair in "trkn" and "disk" is two numbers in one atom; it is given as the string
// "5/12" in the spelling of ID3 TRCK, so that the caller parses one notation for both formats.
// "gnre" is the ID3v1 genre number plus one; it stays a number, as in the file.
//
// Properties: the duration from mvhd (or from the mdhd of the sound track), channels and sample
// rate from the sample entry, bit depth only for lossless codecs (ALAC, FLAC), since the 16 in an
// AAC entry is a nominal value, not a depth. A video track makes the file a video; without one it
// is audio.
//
// Streams: every picture, sound and subtitle track, with its codec from the sample entry, its
// language from mdhd, a picture's size from the sample entry and its bit depth and transfer from
// hvcC, av1C and colr, a sound's properties as above, and the average bitrate from btrt or esds.
// A QuickTime text track counts as subtitles only when it is enabled: disabled ones carry chapters.
//
// The moov atom often comes after mdat, that is at the end of a file of several hundred megabytes,
// so the content of atoms that do not matter here is skipped instead of read. An atom that fits in
// the window is loaded together with its header; a larger one (mdat, often moov) is read piece by
// piece, each structure loading the bytes it reads.
namespace tal::meta::tagging {

namespace {

struct Box {
    std::string type;
    Walk body;
};

// The next atom from the parent's cursor, or none when the parent has ended. Size 1 means a 64-bit
// size right after the type; size 0 means up to the end of the parent. An atom shorter than its own
// header means a broken file. A body that fits in the window is loaded right after the header, so
// that the part holds it in memory.
[[nodiscard]] Task<std::optional<Box>> next(Walk& parent, Gathered& out) {
    if (parent.left() < 8) co_return std::nullopt;

    co_await parent.need(4 + 4 + 8);

    std::uint64_t size = parent.be32();
    std::string type = parent.chars(4);
    std::uint64_t header = 8;

    if (size == 1) {
        size = parent.be64();
        header = 16;
    } else if (size == 0) {
        size = parent.left() + header;
    }

    if (size < header) {
        out.damaged();
        parent.seek(parent.end());

        co_return std::nullopt;
    }

    if (size - header > parent.left()) out.damaged();

    if (size - header <= windowed()) co_await parent.need(static_cast<std::size_t>(size - header));

    co_return Box{std::move(type), parent.part(size - header)};
}

// The duration from an mvhd or mdhd atom: a 32-bit time scale and a duration that is 32-bit in
// version 0 and 64-bit in version 1. None when the atom is truncated, the scale is zero or the
// duration is the all-ones "unknown" value.
[[nodiscard]] std::optional<std::chrono::milliseconds> lasting(Walk& body) {
    const std::uint8_t version = body.u8();

    body.skip(3);

    std::uint64_t scale = 0;
    std::uint64_t duration = 0;

    if (version == 1) {
        body.skip(16);
        scale = body.be32();
        duration = body.be64();
    } else {
        body.skip(8);
        scale = body.be32();
        duration = body.be32();
    }

    if (body.spoiled() || scale == 0 || duration == ~std::uint64_t{0} || duration == 0xFFFFFFFFull) return std::nullopt;

    return std::chrono::milliseconds{static_cast<std::int64_t>(duration * 1000 / scale)};
}

// The length of an MPEG-4 descriptor: up to four bytes of seven bits each.
[[nodiscard]] std::uint32_t described_size(Walk& body) {
    std::uint32_t made = 0;

    for (int which = 0; which < 4; ++which) {
        const std::uint8_t one = body.u8();

        made = (made << 7) | (one & 0x7Fu);

        if ((one & 0x80u) == 0) break;
    }

    return made;
}

struct Elementary {
    std::optional<std::uint8_t> object;
    std::optional<std::uint32_t> bitrate;
};

// The most bytes `elementary` reads: the full atom header, the ES_Descriptor's tag and size, its
// stream number, flags and optional fields (a URL of up to 255 bytes), the tag and size of the
// DecoderConfigDescriptor, and its fields up to the average bitrate.
constexpr std::size_t esds_most = 4 + 1 + 4 + 2 + 1 + 2 + 1 + 255 + 2 + 1 + 4 + 1 + 8 + 4;

// esds: an ES_Descriptor containing a DecoderConfigDescriptor with the object type (which audio
// codec, MPEG-4 AAC or MPEG-1/2 Layer III) and the average bitrate.
[[nodiscard]] Elementary elementary(Walk& body) {
    Elementary made;

    body.skip(4);

    if (body.u8() != 0x03) return made;

    (void)described_size(body);
    body.skip(2);

    const std::uint8_t flags = body.u8();

    if ((flags & 0x80u) != 0) body.skip(2);

    if ((flags & 0x40u) != 0) body.skip(body.u8());

    if ((flags & 0x20u) != 0) body.skip(2);

    if (body.u8() != 0x04) return made;

    (void)described_size(body);

    const std::uint8_t object = body.u8();

    body.skip(1 + 3 + 4);

    const std::uint32_t average = body.be32();

    if (body.spoiled()) return made;

    made.object = object;

    if (average > 0) made.bitrate = average;

    return made;
}

struct Track {
    std::string handler;
    bool enabled = true;
    std::optional<std::chrono::milliseconds> duration;
    Properties sound;
    Stream stream;
};

// The codec of a sample entry by its four characters, in the short names of the meta streams; an
// entry not listed stays as it is called.
[[nodiscard]] std::string coded(const std::string& entry) {
    static constexpr std::pair<std::string_view, std::string_view> known[] = {
        {"avc1", "h264"}, {"avc3", "h264"}, {"hvc1", "hevc"}, {"hev1", "hevc"}, {"dvh1", "hevc"}, {"dvhe", "hevc"},
        {"av01", "av1"}, {"vp09", "vp9"}, {"vp08", "vp8"}, {"mp4v", "mpeg4"}, {"apcn", "prores"}, {"apch", "prores"},
        {"apcs", "prores"}, {"apco", "prores"}, {"ap4h", "prores"}, {"jpeg", "mjpeg"}, {"mjpa", "mjpeg"},
        {"mp4a", "aac"}, {".mp3", "mp3"}, {"ac-3", "ac3"}, {"ec-3", "eac3"}, {"alac", "alac"}, {"fLaC", "flac"},
        {"Opus", "opus"}, {"mlpa", "truehd"}, {"dtsc", "dts"}, {"dtsh", "dts"}, {"dtsl", "dts"}, {"dtse", "dts"},
        {"lpcm", "pcm"}, {"sowt", "pcm"}, {"twos", "pcm"}, {"ipcm", "pcm"}, {"fpcm", "pcm"}, {"tx3g", "mov_text"},
        {"wvtt", "webvtt"}, {"stpp", "ttml"}, {"c608", "cea608"}};

    for (const auto& [given, made] : known)
        if (entry == given) return std::string{made};

    return entry;
}

// The language of mdhd: three letters of five bits each after the time fields (ISO 639-2/T, "und"
// for none). A QuickTime file may give a Macintosh language number instead, under 0x400, which
// says nothing here.
[[nodiscard]] std::string spoken(Walk& body) {
    const std::uint8_t version = body.u8();

    body.skip(3 + (version == 1 ? 8 + 8 + 4 + 8 : 4 + 4 + 4 + 4));

    const std::uint16_t code = body.be16();

    if (body.spoiled() || code < 0x400) return {};

    std::string made;

    for (const int shift : {10, 5, 0}) made += static_cast<char>(((code >> shift) & 0x1Fu) + 0x60);

    return made;
}

// The boxes inside a sample entry that tell more than the entry: btrt (the average bitrate), and
// for a picture hvcC and av1C (the bit depth) and colr (the transfer).
[[nodiscard]] Task<void> configured(Walk& body, Track& track, Gathered& out) {
    while (true) {
        std::optional<Box> inner = co_await next(body, out);

        if (!inner) break;

        Walk& one = inner->body;

        if (inner->type == "btrt") {
            co_await one.need(8 + 4);
            one.skip(8);

            const std::uint32_t average = one.be32();

            if (!one.spoiled() && average > 0) track.stream.bitrate = average;
        } else if (inner->type == "hvcC" || inner->type == "av1C") {
            co_await one.need(32);

            const Bytes record = one.bytes(static_cast<std::size_t>(std::min<std::uint64_t>(one.left(), 32)));

            if (const std::optional<std::uint16_t> depth = depth_in(inner->type == "hvcC" ? "hevc" : "av1", record))
                track.stream.depth = depth;
        } else if (inner->type == "colr") {
            co_await one.need(4 + 2 + 2);

            const std::string kind = one.chars(4);

            one.skip(2);

            const std::uint16_t transfer = one.be16();

            if (!one.spoiled() && (kind == "nclx" || kind == "nclc")) track.stream.transfer = transfer_of(transfer);
        }
    }
}

// A visual sample entry: the coded size in pixels, then the boxes that tell more.
[[nodiscard]] Task<void> pictured_entry(Box& entry, Track& track, Gathered& out) {
    Walk& body = entry.body;

    co_await body.need(24 + 2 + 2);
    body.skip(24);

    const std::uint16_t wide = body.be16();
    const std::uint16_t high = body.be16();

    body.skip(50);

    if (body.spoiled()) co_return;

    if (wide > 0) track.stream.width = wide;

    if (high > 0) track.stream.height = high;

    co_await configured(body, track, out);
}

// An audio sample entry (mp4a, alac, fLaC, Opus…): channels and sample rate in the common part, or
// elsewhere in QuickTime version 2; the codec atoms follow.
[[nodiscard]] Task<void> sampled(Box& entry, Track& track, Gathered& out) {
    Walk& body = entry.body;

    co_await body.need(8 + 2 + 6 + 2 + 6 + 4 + 4 + 8 + 4);
    body.skip(8);

    const std::uint16_t version = body.be16();

    body.skip(6);

    std::uint32_t channels = body.be16();

    body.skip(6);

    std::uint32_t rate = body.be32() >> 16;

    if (version == 1) {
        body.skip(16);
    } else if (version == 2) {
        body.skip(4);

        const double precise = std::bit_cast<double>(body.be64());

        channels = body.be32();
        body.skip(20);

        rate = precise > 0 && precise < 1e7 ? static_cast<std::uint32_t>(precise) : 0;
    }

    if (rate > 0) track.sound.rate = rate;

    if (channels > 0 && channels < 0x10000) track.sound.channels = static_cast<std::uint16_t>(channels);

    while (true) {
        std::optional<Box> inner = co_await next(body, out);

        if (!inner) break;

        if (inner->type == "esds") {
            co_await inner->body.need(esds_most);

            const Elementary made = elementary(inner->body);

            if (made.bitrate) track.sound.bitrate = made.bitrate;

            if (made.object == 0x69 || made.object == 0x6B) track.stream.codec = "mp3";
        } else if (inner->type == "btrt") {
            co_await inner->body.need(8 + 4);
            inner->body.skip(8);

            const std::uint32_t average = inner->body.be32();

            if (!inner->body.spoiled() && average > 0 && !track.sound.bitrate) track.sound.bitrate = average;
        } else if (inner->type == "alac") {
            Walk& alac = inner->body;

            co_await alac.need(4 + 4 + 1 + 1 + 3 + 1 + 2 + 4 + 4 + 4);
            alac.skip(4 + 4 + 1);

            const std::uint8_t depth = alac.u8();

            alac.skip(3);

            const std::uint8_t heard = alac.u8();

            alac.skip(2 + 4);

            const std::uint32_t bitrate = alac.be32();
            const std::uint32_t sampled_at = alac.be32();

            if (!alac.spoiled()) {
                if (depth > 0) track.sound.depth = depth;

                if (heard > 0) track.sound.channels = heard;

                if (bitrate > 0) track.sound.bitrate = bitrate;

                if (sampled_at > 0) track.sound.rate = sampled_at;
            }
        } else if (inner->type == "dfLa") {
            // FLAC in MP4: ordinary FLAC blocks after the full atom header, the first one being
            // STREAMINFO, which loads its own bytes.
            Walk& flac = inner->body;

            flac.skip(4 + 4);

            Description seen;
            Gathered aside{seen};

            co_await streaminfo(flac, aside);

            if (seen.properties.depth) track.sound.depth = seen.properties.depth;
        }
    }
}

// Reads a trak atom into `track`, descending through mdia, minf and stbl: whether it is enabled
// from tkhd, the duration and the language from mdhd, the handler ("soun", "vide", "sbtl"…) from
// hdlr and from the first sample entry in stsd the codec and, for a sound or a picture, what its
// entry tells.
[[nodiscard]] Task<void> track_read(Walk& body, Track& track, Gathered& out) {
    while (true) {
        std::optional<Box> box = co_await next(body, out);

        if (!box) break;

        if (box->type == "mdia" || box->type == "minf" || box->type == "stbl") {
            co_await track_read(box->body, track, out);
        } else if (box->type == "tkhd") {
            co_await box->body.need(1 + 3);
            box->body.skip(1);

            const std::uint32_t flags = box->body.be24();

            if (!box->body.spoiled()) track.enabled = (flags & 1u) != 0;
        } else if (box->type == "mdhd") {
            co_await box->body.need(4 + 8 + 8 + 4 + 8 + 2);

            Walk again = box->body;

            track.duration = lasting(box->body);
            track.stream.language = spoken(again);
        } else if (box->type == "hdlr") {
            co_await box->body.need(8 + 4);
            box->body.skip(8);
            track.handler = box->body.chars(4);
        } else if (box->type == "stsd") {
            box->body.skip(8);

            std::optional<Box> entry = co_await next(box->body, out);

            if (!entry) continue;

            track.stream.codec = coded(entry->type);

            if (track.handler == "soun") co_await sampled(*entry, track, out);
            else if (track.handler == "vide") co_await pictured_entry(*entry, track, out);
        }
    }
}

// The stream of a read track: a picture, a sound (with the properties of its entry) or subtitles;
// none for other tracks (hints, time codes, metadata) and for a disabled QuickTime text track,
// which carries chapters.
[[nodiscard]] std::optional<Stream> streamed(Track track) {
    Stream made = std::move(track.stream);

    if (track.handler == "vide") {
        made.kind = Stream::Kind::video;
    } else if (track.handler == "soun") {
        made.kind = Stream::Kind::audio;
        made.channels = track.sound.channels;
        made.rate = track.sound.rate;
        made.depth = track.sound.depth;
        made.bitrate = track.sound.bitrate ? track.sound.bitrate : made.bitrate;
    } else if (track.handler == "sbtl" || track.handler == "subt" || track.handler == "clcp" || (track.handler == "text" && track.enabled)) {
        made.kind = Stream::Kind::subtitle;
    } else {
        return std::nullopt;
    }

    return made;
}

// A big-endian integer of up to eight bytes from a "data" atom; with `signed_` a shorter value is
// sign-extended.
[[nodiscard]] std::int64_t number(const Bytes& given, bool signed_) {
    std::uint64_t made = 0;

    for (const std::byte one : given) made = (made << 8) | std::to_integer<std::uint64_t>(one);

    if (signed_ && !given.empty() && given.size() < 8 && (std::to_integer<int>(given[0]) & 0x80) != 0)
        made |= ~std::uint64_t{0} << (given.size() * 8);

    return static_cast<std::int64_t>(made);
}

// One ilst field: "data" atoms (and for "----", "mean" and "name" before them). The first 16 bytes
// of a picture, which tell its type when the atom does not, are read from the file as they are,
// even past the atom.
[[nodiscard]] Task<void> item(Box& box, Gathered& out) {
    std::string key = latin1(std::span{reinterpret_cast<const std::byte*>(box.type.data()), box.type.size()});
    std::string mean;
    std::string name;

    while (true) {
        std::optional<Box> inner = co_await next(box.body, out);

        if (!inner) break;

        if (out.done()) co_return;

        Walk& body = inner->body;

        if (inner->type == "mean" || inner->type == "name") {
            body.skip(4);
            co_await body.need(1024);

            std::string& into = inner->type == "mean" ? mean : name;

            into = utf8(body.bytes(static_cast<std::size_t>(std::min<std::uint64_t>(body.left(), 1024))));

            continue;
        }

        if (inner->type != "data") continue;

        co_await body.need(4);

        const std::uint32_t kind = body.be32() & 0x00FFFFFFu;

        body.skip(4);

        const std::uint64_t size = body.left();

        if (box.type == "covr") {
            const Bytes head = co_await body.reader().read(body.at(), 16);
            std::string type = kind == 13 ? "image/jpeg" : kind == 14 ? "image/png" : kind == 27 ? "image/bmp" : image_type(head);

            if (out.pictured({Role::front, std::move(type), {}, static_cast<std::size_t>(size)})) {
                Bytes data = co_await body.take(static_cast<std::size_t>(size));

                out.took(std::move(data));
            }

            continue;
        }

        if (out.wanted || size > most_text) continue;

        if (box.type == "----") key = "----:" + mean + ":" + name;

        const Bytes value = co_await body.take(static_cast<std::size_t>(size));

        if ((box.type == "trkn" || box.type == "disk") && value.size() >= 6) {
            const std::uint32_t one = (std::to_integer<std::uint32_t>(value[2]) << 8) | std::to_integer<std::uint32_t>(value[3]);
            const std::uint32_t all = (std::to_integer<std::uint32_t>(value[4]) << 8) | std::to_integer<std::uint32_t>(value[5]);

            out.field(Scheme::itunes, key, all > 0 ? std::to_string(one) + "/" + std::to_string(all) : std::to_string(one));

            continue;
        }

        switch (kind) {
            case 1: out.field(Scheme::itunes, key, utf8(value)); break;
            case 2: out.field(Scheme::itunes, key, utf16(value, true)); break;
            case 0:
            case 21:
            case 22:
                if (!value.empty() && value.size() <= 8 && (kind != 0 || box.type == "gnre"))
                    out.field(Scheme::itunes, key, number(value, kind == 21));

                break;
            default: break;
        }
    }
}

// The fields of an ilst atom, one child atom per field, until the search is done.
[[nodiscard]] Task<void> listed(Walk& body, Gathered& out) {
    while (true) {
        std::optional<Box> box = co_await next(body, out);

        if (!box) break;

        co_await item(*box, out);

        if (out.done()) co_return;
    }
}

// meta is a full atom in MP4 (version and flags before the children) and a plain one in QuickTime.
// A peek into the file tells them apart: when an hdlr atom follows right after the header, there
// are no four version bytes.
[[nodiscard]] Task<void> meta_read(Walk& body, Gathered& out) {
    const Bytes head = co_await body.reader().read(body.at(), 8);

    if (!(head.size() == 8 && std::memcmp(head.data() + 4, "hdlr", 4) == 0)) body.skip(4);

    while (true) {
        std::optional<Box> box = co_await next(body, out);

        if (!box) break;

        if (box->type == "ilst") co_await listed(box->body, out);

        if (out.done()) co_return;
    }
}

}

// Parses an ISO-BMFF file of `size` bytes: the brand from ftyp, then the first moov with the
// duration (mvhd), the tracks and the metadata (meta in udta or directly in moov). Sets the type
// (video with a video track, audio/mp4 with a sound track or an "M4" brand), the properties of the
// first sound track, and a bitrate averaged over the file size when the codec gives none. A file
// without moov is marked damaged.
Task<void> mp4(Reader& from, std::uint64_t size, Gathered& out) {
    Walk file{from, size, 0, size};

    std::string brand;
    bool moov = false;
    bool video = false;
    std::optional<std::chrono::milliseconds> whole;
    std::optional<Track> sound;

    while (true) {
        std::optional<Box> top = co_await next(file, out);

        if (!top) break;

        if (top->type == "ftyp") {
            co_await top->body.need(4);
            brand = top->body.chars(4);

            continue;
        }

        if (top->type != "moov") continue;

        moov = true;

        while (true) {
            std::optional<Box> box = co_await next(top->body, out);

            if (!box) break;

            if (box->type == "mvhd") {
                co_await box->body.need(4 + 16 + 4 + 8);
                whole = lasting(box->body);
            } else if (box->type == "trak") {
                Track track;

                co_await track_read(box->body, track, out);

                if (track.handler == "vide") video = true;

                if (std::optional<Stream> stream = streamed(track)) out.into.streams.push_back(std::move(*stream));

                if (track.handler == "soun" && !sound) sound = std::move(track);
            } else if (box->type == "udta") {
                while (true) {
                    std::optional<Box> inner = co_await next(box->body, out);

                    if (!inner) break;

                    if (inner->type == "meta") co_await meta_read(inner->body, out);
                }
            } else if (box->type == "meta") {
                co_await meta_read(box->body, out);
            }

            if (out.done()) co_return;
        }

        break;
    }

    const bool quick = brand == "qt  ";

    if (!video && (sound || brand.starts_with("M4"))) out.into.type = "audio/mp4";
    else out.into.type = quick ? "video/quicktime" : "video/mp4";

    if (!moov) {
        out.damaged();

        co_return;
    }

    Properties& made = out.into.properties;

    // The duration from mvhd is the presentation time (after the edit list, without the AAC priming
    // samples); the track's mdhd counts the media samples including the priming, and is used only
    // when mvhd says nothing.
    if (sound) made = sound->sound;

    made.duration = whole ? whole : sound ? sound->duration : std::nullopt;

    if (!made.bitrate && made.duration && made.duration->count() > 0)
        made.bitrate = static_cast<std::uint32_t>(size * 8 * 1000 / static_cast<std::uint64_t>(made.duration->count()));
}

}

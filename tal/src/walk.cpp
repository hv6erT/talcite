#include "tagging.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <utility>

#include "tal/text.hpp"

// Shared tools of the tag readers, ported from talcite-adapters' tags: a cursor over the file,
// the collector of results, text decoding and image recognition. Each format has its own file
// (`id3.cpp`, `ape.cpp`, `mpeg.cpp`, `flac.cpp`, `ogg.cpp`, `mp4.cpp`, `wavpack.cpp`,
// `matroska.cpp`, `avi.cpp`); `meta.cpp` recognizes the container and calls them in file order.
//
// Limits: a foreign file states its own sizes (ID3 up to 256 MB, a FLAC block up to 16 MB, an MP4
// atom up to exabytes), and none of them is trusted when allocating memory. A single string over a
// megabyte is not a title but garbage or an embedded image (`most_text`); a comment block over
// 16 MB is skipped (`most_block`); an Ogg packet is assembled up to 64 MB at most (`most_packet`,
// since Ogg comments carry images in base64); a picture over 64 MB is refused (`most_picture`).
//
// The cursor (`Walk`) has a single spoil flag, like the reader of the GIF decoder: a read past the
// bound sets `spoiled`, returns zeros and never clears, so a parser checks one bit once per
// structure instead of guarding the length at every byte. The bound (`end`) is the end of the
// structure, not of the file. The file is reached only by `need`, which loads the next bytes into a
// window of at least 64 KiB (`windowed`), and by `take` for a large piece (a picture); every other
// read comes from the window, synchronously. A read within the bound but past the window is a
// reader that forgot `need`: a mistake of ours, which stops a debug build.
//
// The collector of results (`Gathered`): the same parsers serve both describing a file and handing
// out a picture: when describing, pictures are only entered into the list; when handing out, the
// number `wanted` says whose bytes should come, and the parser for which `pictured` returns true
// reads them and hands them over through `took`. So a picture number means the same on both paths,
// because the list is built the same way over the same file.
//
// Text comes into UTF-8: Latin-1 is copied byte by byte; UTF-8 passes through, with broken
// sequences replaced by the replacement character; UTF-16 comes in the given byte order or with a
// byte order mark in front (without a mark it is little-endian, which is what Windows programs
// write). A zero ends all three: what follows is padding garbage, not text.
namespace tal::meta::tagging {

namespace {

// A byte as an unsigned number, ready to be shifted into a wider integer.
constexpr std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// The least a `need` loads: `window`, unless the trials shrank it.
std::atomic<std::size_t> loaded{window};

}

// --- Walk ----------------------------------------------------------------------------------------

// A cursor over [at, end) of a file of `size` bytes. An end past the file is clipped at once and
// the flag set: a structure that promises more than the file holds is truncated, and that is
// information, not an error.
Walk::Walk(Reader& from, std::uint64_t size, std::uint64_t at, std::uint64_t end) : from_(&from), size_(size), at_(at), end_(end) {
    if (end_ > size_) {
        end_ = size_;
        spoiled_ = true;
    }

    if (at_ > end_) {
        at_ = end_;
        spoiled_ = true;
    }
}

// A cursor over the whole of `from`.
Task<Walk> Walk::over(Reader& from) {
    const std::uint64_t size = co_await from.size();

    co_return Walk{from, size, 0, size};
}

// Loads the next `much` bytes, or what is left of them before the bound, into the window, unless
// it holds them already; at least 64 KiB when the bound allows, so that small reads after it cost
// nothing. A file shorter than its size said moves the bound to where it ends, with the flag.
Task<void> Walk::need(std::size_t much) {
    const std::uint64_t wanted = std::min<std::uint64_t>(much, left());

    if (at_ >= window_at_ && at_ + wanted <= window_at_ + window_.size()) co_return;

    const std::uint64_t room = std::min<std::uint64_t>(std::max<std::uint64_t>(wanted, windowed()), left());

    Bytes read = co_await from_->read(at_, static_cast<std::size_t>(room));

    held_ = std::make_shared<const Bytes>(std::move(read));
    window_ = *held_;
    window_at_ = at_;

    if (window_.size() < room) {
        end_ = window_at_ + window_.size();
        spoiled_ = true;
    }
}

// `much` bytes from the current position, read past the window when it does not hold them; past
// the bound what there is, with the flag: a truncated string is better than none.
Task<Bytes> Walk::take(std::size_t much) {
    if (much > left()) {
        much = static_cast<std::size_t>(left());
        spoiled_ = true;
    }

    if (at_ >= window_at_ && at_ + much <= window_at_ + window_.size()) co_return bytes(much);

    Bytes made = co_await from_->read(at_, much);

    if (made.size() < much) {
        end_ = at_ + made.size();
        spoiled_ = true;
    }

    at_ += made.size();

    co_return made;
}

// The least a `need` loads, 64 KiB (`window`) unless set otherwise.
std::size_t windowed() { return loaded.load(std::memory_order_relaxed); }

// Sets the least a `need` loads to `least` bytes (at least one). The trials set a byte, so that
// every read a `need` before it did not cover stops a debug build, whatever else the window
// would have held by chance.
void windowed(std::size_t least) { loaded.store(std::max<std::size_t>(least, 1), std::memory_order_relaxed); }

// A pointer to `much` bytes at the current position in the window, which moves past them. Past
// the bound it returns null, moves to the end and sets the flag.
const std::byte* Walk::near(std::size_t much) {
    if (much > left()) {
        at_ = end_;
        spoiled_ = true;

        return nullptr;
    }

    if (at_ < window_at_ || at_ + much > window_at_ + window_.size()) {
        assert(!"tal::meta::tagging::Walk: a read the window does not hold, without need");

        at_ = end_;
        spoiled_ = true;

        return nullptr;
    }

    const std::byte* made = window_.data() + (at_ - window_at_);

    at_ += much;

    return made;
}

// One byte; zero past the bound, where `near` sets the flag.
std::uint8_t Walk::u8() {
    const std::byte* got = near(1);

    return got == nullptr ? std::uint8_t{0} : static_cast<std::uint8_t>(sample(got[0]));
}

// A big-endian 16-bit number; zero past the bound.
std::uint16_t Walk::be16() {
    const std::byte* got = near(2);

    return got == nullptr ? std::uint16_t{0} : static_cast<std::uint16_t>((sample(got[0]) << 8) | sample(got[1]));
}

// A big-endian 24-bit number; zero past the bound.
std::uint32_t Walk::be24() {
    const std::byte* got = near(3);

    return got == nullptr ? 0u : (sample(got[0]) << 16) | (sample(got[1]) << 8) | sample(got[2]);
}

// A big-endian 32-bit number; zero past the bound.
std::uint32_t Walk::be32() {
    const std::byte* got = near(4);

    return got == nullptr ? 0u : (sample(got[0]) << 24) | (sample(got[1]) << 16) | (sample(got[2]) << 8) | sample(got[3]);
}

// A big-endian 64-bit number, read as two 32-bit halves, the high one first; a half past the bound
// is zero.
std::uint64_t Walk::be64() {
    const std::uint64_t high = be32();

    return (high << 32) | be32();
}

// A little-endian 16-bit number; zero past the bound.
std::uint16_t Walk::le16() {
    const std::byte* got = near(2);

    return got == nullptr ? std::uint16_t{0} : static_cast<std::uint16_t>(sample(got[0]) | (sample(got[1]) << 8));
}

// A little-endian 32-bit number; zero past the bound.
std::uint32_t Walk::le32() {
    const std::byte* got = near(4);

    return got == nullptr ? 0u : sample(got[0]) | (sample(got[1]) << 8) | (sample(got[2]) << 16) | (sample(got[3]) << 24);
}

// A little-endian 64-bit number, read as two 32-bit halves, the low one first; a half past the
// bound is zero.
std::uint64_t Walk::le64() {
    const std::uint64_t low = le32();

    return low | (static_cast<std::uint64_t>(le32()) << 32);
}

// `much` bytes from the window; past the bound what there is, with the flag.
Bytes Walk::bytes(std::size_t much) {
    if (much > left()) {
        much = static_cast<std::size_t>(left());
        spoiled_ = true;
    }

    const std::byte* got = near(much);

    return got == nullptr ? Bytes{} : Bytes(got, got + much);
}

// `much` bytes of the window as a string, as they are, without decoding (atom types, frame
// identifiers); truncated past the bound like `bytes`.
std::string Walk::chars(std::size_t much) {
    const Bytes got = bytes(much);

    return std::string(reinterpret_cast<const char*>(got.data()), got.size());
}

// A peek into the window that neither moves the cursor nor sets the flag: it asks whether these
// bytes are here, it does not read them.
bool Walk::ahead(std::string_view magic) {
    if (magic.size() > left() || at_ < window_at_ || at_ + magic.size() > window_at_ + window_.size()) return false;

    return std::memcmp(window_.data() + (at_ - window_at_), magic.data(), magic.size()) == 0;
}

// Moves the cursor `much` bytes forward. A skip past the bound stops at the end and sets the flag.
void Walk::skip(std::uint64_t much) {
    if (much > left()) {
        at_ = end_;
        spoiled_ = true;

        return;
    }

    at_ += much;
}

// Moves the cursor to the absolute file position `to`. A position past the bound stops at the end
// and sets the flag; a position before the current one is allowed.
void Walk::seek(std::uint64_t to) {
    if (to > end_) {
        at_ = end_;
        spoiled_ = true;

        return;
    }

    at_ = to;
}

// A cursor over the next `much` bytes; this cursor moves past them. The piece shares the window,
// which is not copied. A piece longer than what is left is clipped, and both cursors get the flag.
Walk Walk::part(std::uint64_t much) {
    const bool short_of = much > left();
    const std::uint64_t taken = short_of ? left() : much;

    Walk made{*from_, size_, at_, at_ + taken};

    made.held_ = held_;
    made.window_ = window_;
    made.window_at_ = window_at_;

    if (short_of) {
        made.spoiled_ = true;
        spoiled_ = true;
    }

    at_ += taken;

    return made;
}

// --- Gathered ------------------------------------------------------------------------------------

// Appends one field to the description.
void Gathered::field(Scheme scheme, std::string key, Value value) {
    into.fields.push_back({scheme, std::move(key), std::move(value)});
}

// Enters a picture into the list; true when its bytes are the ones asked for, and the parser is to
// hand them over. A picture over the limit is refused only when someone asks for its bytes; the
// description lists it as usual, with its real size.
bool Gathered::pictured(Picture one) {
    const std::size_t size = one.size;
    const bool mine = wanting();

    into.pictures.push_back(std::move(one));

    if (!mine) return false;

    if (size > most_picture) {
        refused = "The picture is too large.";

        return false;
    }

    return true;
}

// Hands over the bytes of the wanted picture, when the caller gave a place for them, and marks the
// search as finished.
void Gathered::took(Bytes data) {
    if (bytes != nullptr) *bytes = std::move(data);

    found = true;
}

// --- Text ----------------------------------------------------------------------------------------

// Latin-1 into UTF-8, byte by byte, up to the first zero.
std::string latin1(std::span<const std::byte> given) {
    std::string made;

    made.reserve(given.size());

    for (const std::byte one : given) {
        const std::uint32_t code = sample(one);

        if (code == 0) break;

        text::encoded(made, static_cast<char32_t>(code));
    }

    return made;
}

// Through the library's decoder, one character at a time: a broken sequence comes out as the
// replacement character instead of passing on as bytes that nothing parses later. A leading byte
// order mark is dropped.
std::string utf8(std::span<const std::byte> given) {
    const std::string_view raw{reinterpret_cast<const char*>(given.data()), given.size()};
    const std::string_view text = raw.substr(0, std::min(raw.find('\0'), raw.size()));

    std::string made;
    std::size_t at = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;

    made.reserve(text.size());

    while (at < text.size()) text::encoded(made, text::decoded(text, at).value_or(char32_t{0xFFFD}));

    return made;
}

// UTF-16 in the given byte order into UTF-8, up to the first zero unit. A surrogate pair becomes
// one character; a lone surrogate becomes U+FFFD.
std::string utf16(std::span<const std::byte> given, bool big) {
    std::string made;

    made.reserve(given.size());

    for (std::size_t at = 0; at + 1 < given.size(); at += 2) {
        const auto unit = [&](std::size_t where) {
            return big ? (sample(given[where]) << 8) | sample(given[where + 1])
                       : sample(given[where]) | (sample(given[where + 1]) << 8);
        };

        const std::uint32_t one = unit(at);

        if (one == 0) break;

        if (one >= 0xD800 && one < 0xDC00 && at + 3 < given.size()) {
            const std::uint32_t two = unit(at + 2);

            if (two >= 0xDC00 && two < 0xE000) {
                text::encoded(made, static_cast<char32_t>(0x10000 + ((one - 0xD800) << 10) + (two - 0xDC00)));
                at += 2;

                continue;
            }
        }

        text::encoded(made, one >= 0xD800 && one < 0xE000 ? char32_t{0xFFFD} : static_cast<char32_t>(one));
    }

    return made;
}

// UTF-16 with an optional byte order mark into UTF-8. The mark is dropped; without one the text is
// read as little-endian.
std::string utf16(std::span<const std::byte> given) {
    if (given.size() >= 2 && sample(given[0]) == 0xFE && sample(given[1]) == 0xFF) return utf16(given.subspan(2), true);

    if (given.size() >= 2 && sample(given[0]) == 0xFF && sample(given[1]) == 0xFE) return utf16(given.subspan(2), false);

    return utf16(given, false);
}

// The string with the ASCII letters a-z in upper case; every other byte, including UTF-8 sequences,
// stays as it is.
std::string upper(std::string ascii) {
    for (char& one : ascii)
        if (one >= 'a' && one <= 'z') one = static_cast<char>(one - 'a' + 'A');

    return ascii;
}

// A big-endian 32-bit number at offset `at` of a buffer; zero when fewer than four bytes are left
// there.
std::uint32_t be32_of(std::span<const std::byte> given, std::size_t at) {
    if (at + 4 > given.size()) return 0;

    return (sample(given[at]) << 24) | (sample(given[at + 1]) << 16) | (sample(given[at + 2]) << 8) | sample(given[at + 3]);
}

// --- Images --------------------------------------------------------------------------------------

// The picture role from the type number shared by ID3 (APIC) and FLAC (PICTURE).
Role role_of(std::uint32_t kind) {
    switch (kind) {
        case 3: return Role::front;
        case 4: return Role::back;
        case 7:
        case 8: return Role::artist;
        default: return Role::other;
    }
}

// The picture type from its magic bytes, when the file does not state it (APE, part of MP4).
std::string image_type(std::span<const std::byte> head) {
    const auto starts = [&](std::string_view magic, std::size_t at = 0) {
        return head.size() >= at + magic.size() && std::memcmp(head.data() + at, magic.data(), magic.size()) == 0;
    };

    if (starts("\xFF\xD8\xFF")) return "image/jpeg";

    if (starts("\x89PNG\r\n\x1A\n")) return "image/png";

    if (starts("GIF8")) return "image/gif";

    if (starts("BM")) return "image/bmp";

    if (starts("RIFF") && starts("WEBP", 8)) return "image/webp";

    return {};
}

// A uniform spelling of the type the file does state: "image/jpg" and "JPG" from ID3v2.2 become
// "image/jpeg".
std::string image_named(std::string_view said) {
    std::string made;

    for (const char one : said) made += one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one;

    if (made == "image/jpg" || made == "jpg" || made == "jpeg") return "image/jpeg";

    if (made == "png" || made == "gif" || made == "bmp") return "image/" + made;

    return made;
}

// The bit depth a codec's configuration record says (Matroska's CodecPrivate, MP4's hvcC and
// av1C): HEVC (hvcC, bitDepthLumaMinus8 in byte 17) and AV1
// (av1C, the high_bitdepth and twelve_bit flags in byte 2). None for other codecs and for data
// too short to say it.
std::optional<std::uint16_t> depth_in(std::string_view codec, std::span<const std::byte> data) {
    if (codec == "hevc" && data.size() > 17 && std::to_integer<unsigned>(data[0]) == 1)
        return static_cast<std::uint16_t>((std::to_integer<unsigned>(data[17]) & 0x07u) + 8);

    if (codec == "av1" && data.size() > 2 && (std::to_integer<unsigned>(data[0]) & 0x80u) != 0) {
        const unsigned flags = std::to_integer<unsigned>(data[2]);

        if ((flags & 0x40u) == 0) return std::uint16_t{8};

        return static_cast<std::uint16_t>((flags & 0x20u) != 0 ? 12 : 10);
    }

    return std::nullopt;
}

// The transfer of a picture by the code of ITU-T H.273: 16 is PQ (HDR10, Dolby Vision), 18 is HLG;
// the rest is a standard range.
Transfer transfer_of(std::uint64_t code) {
    if (code == 16) return Transfer::pq;

    if (code == 18) return Transfer::hlg;

    return Transfer::standard;
}

// The codec of a picture by the four characters of its compression (Video for Windows: AVI, and
// Matroska's V_MS/VFW/FOURCC), in the short names of the meta streams; one not listed stays as it
// is written, in lower case.
std::string vfw_codec(std::string fourcc) {
    for (char& one : fourcc)
        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

    static constexpr std::pair<std::string_view, std::string_view> known[] = {
        {"h264", "h264"}, {"x264", "h264"}, {"avc1", "h264"}, {"hevc", "hevc"}, {"h265", "hevc"}, {"hvc1", "hevc"},
        {"xvid", "mpeg4"}, {"divx", "mpeg4"}, {"dx50", "mpeg4"}, {"fmp4", "mpeg4"}, {"mp4v", "mpeg4"}, {"div3", "msmpeg4"},
        {"mjpg", "mjpeg"}, {"ffv1", "ffv1"}, {"mpg2", "mpeg2"}, {"vp80", "vp8"}, {"vp90", "vp9"}, {"av01", "av1"}};

    for (const auto& [given, made] : known)
        if (fourcc == given) return std::string{made};

    while (!fourcc.empty() && (fourcc.back() == ' ' || fourcc.back() == '\0')) fourcc.pop_back();

    return fourcc;
}

// The codec of a sound by its WAVE format tag (AVI, and Matroska's A_MS/ACM); one not listed is
// none.
std::string acm_codec(std::uint16_t tag) {
    switch (tag) {
        case 0x0001:
        case 0x0003:
        case 0xFFFE: return "pcm";
        case 0x0050: return "mp2";
        case 0x0055: return "mp3";
        case 0x00FF:
        case 0x1610:
        case 0x706D: return "aac";
        case 0x2000: return "ac3";
        case 0x2001: return "dts";
        case 0xF1AC: return "flac";
        case 0x674F:
        case 0x6750:
        case 0x6751: return "vorbis";
        default: return {};
    }
}

}

#include "archiving.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "tal/text.hpp"

// A tar archive, plain or through gzip, read from the first header to the end, ported from
// talcite's tarring.cpp: tar has no table of contents, so the members are found by walking the
// headers, each followed by its data padded to 512 bytes. Understood are the POSIX ustar header
// (a name with its prefix), GNU's long names (`L`) and pax extended headers (`x`), whose path,
// size and time win over the header's. A header whose checksum does not add up ends the reading as
// damage: past it nothing tells where the next one starts. Links, hard and symbolic, are listed
// but refused; devices and other kinds are passed over, as an extraction has nothing to make of
// them. Through gzip the stream is read in one pass, inflated as it goes; a plain tar skips data by
// moving on in the file.
namespace tal::archive::archiving {

namespace {

constexpr std::size_t block = 512;

// What is inflated or skipped at once.
constexpr std::size_t piece = 64 * 1024;

// What is read of the packed file at once.
constexpr std::size_t packed_piece = 16 * 1024;

// The longest extension header: a longer name is damage, not a name.
constexpr std::uint64_t extension_most = 1024 * 1024;

using Header = std::array<std::byte, block>;

// The text of the field of `much` bytes at `at` of `header`, up to its first zero.
std::string_view field(const Header& header, std::size_t at, std::size_t much) {
    const std::string_view all{reinterpret_cast<const char*>(header.data() + at), much};

    return all.substr(0, all.find('\0'));
}

// The number of the field of `much` bytes at `at`: octal digits between spaces and zeros, or, with
// the top bit of its first byte set, a big-endian binary number in the rest (GNU's base-256, for
// sizes past 8 GiB); nothing for anything else.
std::optional<std::uint64_t> number(const Header& header, std::size_t at, std::size_t much) {
    const unsigned first = std::to_integer<unsigned>(header[at]);

    if ((first & 0x80U) != 0) {
        if (first == 0xFFU) return std::nullopt;

        std::uint64_t made = first & 0x7FU;

        for (std::size_t one = at + 1; one < at + much; ++one) {
            if (made >> 56 != 0) return std::nullopt;

            made = (made << 8) | std::to_integer<std::uint64_t>(header[one]);
        }

        return made;
    }

    std::uint64_t made = 0;
    bool begun = false;

    for (std::size_t one = at; one < at + much; ++one) {
        const auto letter = std::to_integer<char>(header[one]);

        if (letter == ' ' || letter == '\0') {
            if (begun) break;

            continue;
        }

        if (letter < '0' || letter > '7') return std::nullopt;

        made = made * 8 + static_cast<std::uint64_t>(letter - '0');
        begun = true;
    }

    return made;
}

// Whether the checksum of `header` adds up: the sum of its bytes with the checksum field read as
// spaces, unsigned as POSIX says, or signed as some old packers counted.
bool checked(const Header& header) {
    const std::optional<std::uint64_t> stored = number(header, 148, 8);

    if (!stored) return false;

    std::uint64_t plain = 0;
    std::int64_t signed_sum = 0;

    for (std::size_t at = 0; at < block; ++at) {
        const unsigned one = at >= 148 && at < 156 ? 0x20U : std::to_integer<unsigned>(header[at]);

        plain += one;
        signed_sum += static_cast<std::int8_t>(static_cast<std::uint8_t>(one));
    }

    return *stored == plain || static_cast<std::int64_t>(*stored) == signed_sum;
}

// Whether `header` is all zeros: two of them end the archive.
bool blank(const Header& header) {
    return std::all_of(header.begin(), header.end(), [](std::byte one) { return one == std::byte{0}; });
}

// `much` rounded up to whole blocks.
std::uint64_t padded(std::uint64_t much) { return (much + block - 1) / block * block; }

// What extensions before a header said about the member it describes.
struct Extended {
    std::optional<std::string> path;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modified;
};

// The records of a pax extended header `text`, "length key=value\n" one after another, into
// `into`: the path, the size and the whole seconds of the time. A record that does not add up ends
// the reading of the rest.
void paxed(std::string_view text, Extended& into) {
    while (!text.empty()) {
        const std::size_t space = text.find(' ');

        if (space == std::string_view::npos || space > 19) return;

        std::uint64_t length = 0;

        for (const char one : text.substr(0, space)) {
            if (one < '0' || one > '9') return;

            length = length * 10 + static_cast<std::uint64_t>(one - '0');
        }

        if (length <= space + 1 || length > text.size()) return;

        const std::string_view record = text.substr(space + 1, static_cast<std::size_t>(length) - space - 1);
        const std::size_t equals = record.find('=');

        text = text.substr(static_cast<std::size_t>(length));

        if (equals == std::string_view::npos || record.back() != '\n') continue;

        const std::string_view key = record.substr(0, equals);
        const std::string_view value = record.substr(equals + 1, record.size() - equals - 2);

        if (key == "path") into.path = std::string{value};

        if (key != "size" && key != "mtime") continue;

        std::uint64_t made = 0;
        bool digits = false;

        for (const char one : value) {
            if (one == '.') break;

            if (one < '0' || one > '9' || made > (UINT64_MAX - 9) / 10) {
                digits = false;

                break;
            }

            made = made * 10 + static_cast<std::uint64_t>(one - '0');
            digits = true;
        }

        if (digits && key == "size") into.size = made;
        if (digits && key == "mtime" && made <= static_cast<std::uint64_t>(INT64_MAX)) into.modified = static_cast<std::int64_t>(made);
    }
}

// The data of an extension header of `much` bytes, at most a mebibyte, its padding skipped.
Task<std::string> extension_of(Source& source, std::uint64_t much, Path where) {
    if (much > extension_most) damaged("A tar extension header is too long.", where);

    Bytes made(static_cast<std::size_t>(much));

    co_await source.taken(made);
    co_await source.skipped(padded(much) - much);
    co_return text::of(made);
}

// The name `raw` as UTF-8: as it stands when it is valid UTF-8, from Latin-1 otherwise, which
// every byte decodes in.
std::string utf8_of(std::string_view raw) { return text::valid(raw) ? std::string{raw} : from_latin1(raw); }

}

// A source of the bytes of the archive `file` at `where`, `size` bytes long, through gzip when
// `gzip` says so.
Source::Source(fs::File file, Path where, std::uint64_t size, bool gzip) : file_(std::move(file)), where_(std::move(where)), size_(size) {
    if (gzip) gzip_.emplace(inflating::Wrapping::gzip);
}

// Up to `into.size()` next bytes; zero only at the end. Through gzip the inflated bytes wait
// between calls, and a gzip stream that stops before its end is damage.
Task<std::size_t> Source::read(std::span<std::byte> into) {
    if (into.empty()) co_return 0;

    if (!gzip_) {
        const auto much = static_cast<std::size_t>(std::min<std::uint64_t>(into.size(), size_ - raw_));

        if (much == 0) co_return 0;

        const Bytes got = co_await file_.read(raw_, much);

        std::copy(got.begin(), got.end(), into.begin());
        raw_ += got.size();
        at_ += got.size();

        co_return got.size();
    }

    while (used_ == waiting_.size()) {
        if (raw_ >= size_) {
            if (!gzip_->ended()) damaged("The gzip stream is cut short.", where_);

            co_return 0;
        }

        const Bytes packed = co_await file_.read(raw_, static_cast<std::size_t>(std::min<std::uint64_t>(packed_piece, size_ - raw_)));

        if (packed.empty()) damaged("The gzip stream is cut short.", where_);

        raw_ += packed.size();
        waiting_ = gzip_->fed(packed);
        used_ = 0;
    }

    const std::size_t much = std::min(into.size(), waiting_.size() - used_);

    std::copy_n(waiting_.begin() + static_cast<std::ptrdiff_t>(used_), much, into.begin());
    used_ += much;
    at_ += much;

    co_return much;
}

// Exactly `into.size()` next bytes; an archive that ends first is damage.
Task<void> Source::taken(std::span<std::byte> into) {
    for (std::size_t got = 0; got < into.size();) {
        const std::size_t now = co_await read(into.subspan(got));

        if (now == 0) damaged("The tar archive is cut short.", where_);

        got += now;
    }
}

// Passes over `much` bytes: in a plain tar by moving on in the file, through gzip by inflating
// them piece by piece.
Task<void> Source::skipped(std::uint64_t much) {
    if (!gzip_) {
        if (much > size_ - raw_) damaged("The tar archive is cut short.", where_);

        raw_ += much;
        at_ += much;

        co_return;
    }

    Bytes scratch(piece);

    while (much > 0) {
        const auto now = static_cast<std::size_t>(std::min<std::uint64_t>(much, scratch.size()));

        co_await taken(std::span<std::byte>{scratch}.first(now));
        much -= now;
    }
}

// The headers of the tar archive in `source` walked into `held`: a member and a spot (where its
// data starts in the unpacked stream, and its size) per file, folder or link, in the archive's
// order. The end is two blocks of zeros, or the end of the stream after a whole member. More than
// a million members are damage, since a small stream of tiny headers would fill the memory.
Task<void> tar_read(Archive::Held& held, Source& source) {
    Header header{};
    Extended extended;
    std::optional<std::string> longer;
    int zeros = 0;
    bool first = true;

    for (;;) {
        std::size_t got = 0;

        while (got < block) {
            const std::size_t now = co_await source.read(std::span<std::byte>{header}.subspan(got));

            if (now == 0) break;

            got += now;
        }

        if (got == 0) break;

        if (got < block) damaged("The tar archive is cut short.", held.where);

        if (blank(header)) {
            if (++zeros == 2) break;

            continue;
        }

        zeros = 0;

        if (!checked(header)) damaged(first ? "Not a tar archive." : "A tar header is damaged.", held.where);

        first = false;

        const auto type = std::to_integer<char>(header[156]);
        const std::optional<std::uint64_t> told_size = number(header, 124, 12);

        if (!told_size) damaged("A tar header is damaged.", held.where);

        if (type == 'L') {
            const std::string name = co_await extension_of(source, *told_size, held.where);

            longer = name.substr(0, name.find('\0'));

            continue;
        }

        if (type == 'x') {
            paxed(co_await extension_of(source, *told_size, held.where), extended);

            continue;
        }

        if (type == 'K' || type == 'g') {
            (void)co_await extension_of(source, *told_size, held.where);

            continue;
        }

        const std::uint64_t size = extended.size.value_or(*told_size);
        std::string raw;

        if (size > UINT64_MAX - block) damaged("A tar header is damaged.", held.where);

        if (extended.path) raw = *extended.path;
        else if (longer) raw = *longer;
        else if (field(header, 257, 5) == "ustar" && !field(header, 345, 155).empty()) raw = std::string{field(header, 345, 155)} + "/" + std::string{field(header, 0, 100)};
        else raw = std::string{field(header, 0, 100)};

        const std::uint64_t data_at = source.at();
        const bool file = type == '0' || type == '\0' || type == '7';
        const bool folder = type == '5';
        const bool link = type == '1' || type == '2';

        if (file || folder || link) {
            const std::string name = utf8_of(raw);
            const Laid place = laid(name);
            const std::optional<std::uint64_t> mtime = number(header, 136, 12);
            Member made;

            made.name = place.name;
            made.stored = name;
            made.type = folder ? fs::Type::folder : fs::Type::file;
            made.size = file ? size : 0;
            made.packed = made.size;
            made.modified = instant(extended.modified.value_or(static_cast<std::int64_t>(std::min<std::uint64_t>(mtime.value_or(0), INT64_MAX))));

            if (const std::optional<std::uint64_t> mode = number(header, 100, 8); mode && *mode != 0) made.rights = static_cast<std::uint16_t>(*mode & 0777);

            if (place.unsafe) made.refusal = Refusal::unsafe;
            else if (link) made.refusal = Refusal::link;

            held.members.push_back(std::move(made));
            held.spots.push_back(Spot{data_at, file ? size : 0, 0, 0});

            if (held.members.size() > members_most) damaged("The archive holds too many members.", held.where);
        }

        // Links and folders carry no data, whatever their size says (a hard link's is the size of
        // the file it names, in some packers); devices and other kinds may.
        co_await source.skipped(file || (!folder && !link) ? padded(size) : 0);

        extended = Extended{};
        longer.reset();
    }

    if (first && source.at() == 0) damaged("Not a tar archive.", held.where);
}

}

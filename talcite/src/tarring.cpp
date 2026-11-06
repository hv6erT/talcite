#include "archiving.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "fscore.hpp"

// Reading a tar archive, plain or through gzip, from the first header to the end: tar has no table
// of contents, so the members are found by walking the headers, each followed by its data padded to
// 512 bytes. Understood are the POSIX ustar header (name with prefix), the GNU long names (`L`),
// and the pax extended headers (`x`), whose path, size and time win over the header's. A header
// whose checksum does not add up ends the reading with a refusal: past it nothing tells where the
// next one starts. Links (hard and symbolic) are listed but refused, and so are devices and other
// kinds, which an extraction has nothing to make of. Through gzip the stream is read in one pass,
// inflated as it goes; a plain tar skips data by moving on in the file.
namespace talcite::archive {

using namespace fs::core;

namespace {

constexpr std::size_t block = 512;

constexpr std::size_t piece = 64 * 1024;

constexpr std::size_t packed_piece = 16 * 1024;

constexpr std::size_t extension_most = 1024 * 1024;

using Header = std::array<std::byte, block>;

// The bytes of a header field as text, up to the first NUL.
std::string_view field(const Header& header, std::size_t at, std::size_t much) {
    const std::string_view all{reinterpret_cast<const char*>(header.data() + at), much};

    return all.substr(0, all.find('\0'));
}

// A number of a header field: octal digits between spaces and NULs, or, with the top bit of the
// first byte set, a big-endian binary number in the rest (GNU's base-256, for sizes past 8 GiB).
// Nothing for anything else.
std::optional<std::uint64_t> number(const Header& header, std::size_t at, std::size_t much) {
    if ((std::to_integer<unsigned>(header[at]) & 0x80U) != 0) {
        if (std::to_integer<unsigned>(header[at]) == 0xFFU) return std::nullopt;

        std::uint64_t made = std::to_integer<unsigned>(header[at]) & 0x7FU;

        for (std::size_t one = at + 1; one < at + much; ++one) made = (made << 8) | std::to_integer<std::uint64_t>(header[one]);

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

// Whether the header's checksum adds up: the sum of its bytes with the checksum field read as
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

// Whether the whole block is zeros: the end of the archive, after two of them.
bool blank(const Header& header) {
    return std::all_of(header.begin(), header.end(), [](std::byte one) { return one == std::byte{0}; });
}

// The size rounded up to whole blocks.
std::uint64_t padded(std::uint64_t much) { return (much + block - 1) / block * block; }

// What the extensions before a header said about the member it describes.
struct Extended {
    std::optional<std::string> path;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modified;
};

// Reads the records of a pax extended header, "length key=value\n" one after another, keeping the
// path, the size and the whole seconds of the time. A record that does not add up ends the
// reading of the rest.
void paxed(std::string_view text, Extended& into) {
    while (!text.empty()) {
        const std::size_t space = text.find(' ');

        if (space == std::string_view::npos) return;

        std::uint64_t length = 0;

        for (const char one : text.substr(0, space)) {
            if (one < '0' || one > '9') return;

            length = length * 10 + static_cast<std::uint64_t>(one - '0');
        }

        if (length <= space + 1 || length > text.size()) return;

        const std::string_view record = text.substr(space + 1, static_cast<std::size_t>(length) - space - 1);
        const std::size_t equals = record.find('=');

        text = text.substr(static_cast<std::size_t>(length));

        if (equals == std::string_view::npos || record.empty() || record.back() != '\n') continue;

        const std::string_view key = record.substr(0, equals);
        const std::string_view value = record.substr(equals + 1, record.size() - equals - 2);

        if (key == "path") into.path = std::string{value};

        if (key == "size" || key == "mtime") {
            std::uint64_t made = 0;
            bool digits = false;

            for (const char one : value) {
                if (one == '.') break;

                if (one < '0' || one > '9') {
                    digits = false;

                    break;
                }

                made = made * 10 + static_cast<std::uint64_t>(one - '0');
                digits = true;
            }

            if (digits && key == "size") into.size = made;
            if (digits && key == "mtime") into.modified = static_cast<std::int64_t>(made);
        }
    }
}

// The data of an extension header, at most a mebibyte (a longer name is damage, not a name), with
// the padding after it skipped.
std::string extension_of(Source& source, std::uint64_t much, const std::filesystem::path& where) {
    if (much > extension_most) damaged("A tar extension header is too long.", where);

    std::string made(static_cast<std::size_t>(much), '\0');

    source.taken(std::span{reinterpret_cast<std::byte*>(made.data()), made.size()});
    source.skipped(padded(much) - much);

    return made;
}

// The member's name as UTF-8: as it stands when it is valid UTF-8, otherwise from Latin-1, which
// every byte decodes in.
std::string utf8_of(std::string_view raw) { return utf8_whole(raw) ? std::string{raw} : from_latin1(raw); }

}

// A source of the archive's bytes from the start of `file`, `size` bytes long, through `gzip` when
// one is given.
Source::Source(std::shared_ptr<fs::File::Held> file, std::filesystem::path where, std::uintmax_t size, std::unique_ptr<Unpacking> gzip)
    : file_(std::move(file)), where_(std::move(where)), size_(size), gzip_(std::move(gzip)) {}

// Up to `into.size()` next bytes; zero only at the end. Through gzip the inflated bytes wait in a
// buffer between calls; a gzip stream that stops before its end is damage.
std::size_t Source::read(std::span<std::byte> into) {
    if (into.empty()) return 0;

    if (!gzip_) {
        const auto much = static_cast<std::size_t>(std::min<std::uintmax_t>(into.size(), size_ - raw_));
        const std::size_t got = much == 0 ? 0 : read_into(*file_, raw_, into.first(much));

        raw_ += got;
        at_ += got;

        return got;
    }

    while (used_ == waiting_.size()) {
        if (raw_ >= size_) {
            if (!gzip_->ended()) damaged("The gzip stream is cut short.", where_);

            return 0;
        }

        Bytes packed(static_cast<std::size_t>(std::min<std::uintmax_t>(packed_piece, size_ - raw_)));

        const std::size_t got = read_into(*file_, raw_, packed);

        if (got == 0) damaged("The gzip stream is cut short.", where_);

        raw_ += got;
        packed.resize(got);

        waiting_ = gzip_->fed(packed);
        used_ = 0;
    }

    const std::size_t much = std::min(into.size(), waiting_.size() - used_);

    std::copy_n(waiting_.begin() + static_cast<std::ptrdiff_t>(used_), much, into.begin());

    used_ += much;
    at_ += much;

    return much;
}

// Exactly `into.size()` next bytes, or a refusal: the archive ends in the middle.
void Source::taken(std::span<std::byte> into) {
    for (std::size_t got = 0; got < into.size();) {
        const std::size_t now = read(into.subspan(got));

        if (now == 0) damaged("The tar archive is cut short.", where_);

        got += now;
    }
}

// Passes over `much` bytes: in a plain tar by moving on in the file, through gzip by inflating
// them.
void Source::skipped(std::uintmax_t much) {
    if (!gzip_) {
        if (much > size_ - raw_) damaged("The tar archive is cut short.", where_);

        raw_ += much;
        at_ += much;

        return;
    }

    Bytes scratch(piece);

    while (much > 0) {
        const auto now = static_cast<std::size_t>(std::min<std::uintmax_t>(much, scratch.size()));

        taken(std::span{scratch}.first(now));

        much -= now;
    }
}

// Walks the headers of the tar archive in `source` into `held`: one member and one spot (where its
// data starts in the unpacked stream, and its size) per file, folder or link, in the archive's
// order. The end is two blocks of zeros, or the end of the stream after a whole member.
void tar_read(Archive::Held& held, Source& source) {
    Header header{};
    Extended extended;
    std::optional<std::string> longer;
    int zeros = 0;
    bool first = true;

    for (;;) {
        std::size_t got = 0;

        while (got < block) {
            const std::size_t now = source.read(std::span{header}.subspan(got));

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
            std::string name = extension_of(source, *told_size, held.where);

            longer = name.substr(0, name.find('\0'));

            continue;
        }

        if (type == 'x') {
            paxed(extension_of(source, *told_size, held.where), extended);

            continue;
        }

        if (type == 'K' || type == 'g') {
            (void)extension_of(source, *told_size, held.where);

            continue;
        }

        const std::uint64_t size = extended.size.value_or(*told_size);

        std::string raw;

        if (extended.path) raw = *extended.path;
        else if (longer) raw = *longer;
        else if (field(header, 257, 5) == "ustar" && !field(header, 345, 155).empty())
            raw = std::string{field(header, 345, 155)} + "/" + std::string{field(header, 0, 100)};
        else raw = std::string{field(header, 0, 100)};

        const std::uintmax_t data_at = source.at();
        const bool file = type == '0' || type == '\0' || type == '7';
        const bool folder = type == '5';
        const bool link = type == '1' || type == '2';

        if (file || folder || link) {
            const std::string name = utf8_of(raw);
            const Laid place = laid(name);

            Member made;

            made.name = place.name;
            made.stored = name;
            made.kind = folder ? fs::Kind::folder : fs::Kind::file;
            made.size = file ? size : 0;
            made.packed = made.size;

            const std::optional<std::uint64_t> mtime = number(header, 136, 12);

            made.modified = std::chrono::sys_seconds{std::chrono::seconds{extended.modified.value_or(static_cast<std::int64_t>(mtime.value_or(0)))}};

            if (const std::optional<std::uint64_t> mode = number(header, 100, 8); mode && *mode != 0)
                made.rights = static_cast<std::filesystem::perms>(*mode & 0777);

            if (place.unsafe) made.refusal = Refusal::unsafe;
            else if (link) made.refusal = Refusal::link;

            held.members.push_back(std::move(made));
            held.spots.push_back(Spot{data_at, file ? size : 0, 0, 0});
        }

        // Links and folders carry no data, whatever their size field says (a hard link's is the
        // size of the file it names, in some packers); devices and other kinds may.
        source.skipped(file || (!folder && !link) ? padded(size) : 0);

        extended = Extended{};
        longer.reset();
    }

    if (first && held.members.empty() && source.at() == 0) damaged("Not a tar archive.", held.where);
}

}

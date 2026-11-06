#include "archiving.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fscore.hpp"

// Reading the table of contents of a ZIP archive (APPNOTE 6.3.10). The central directory at the end
// of the file is the archive's own account of what it holds, so it alone is read, never the local
// headers one after another: a ZIP may be appended to a program (a self-extracting one) or hold
// bytes nobody refers to, and only the central directory says which entries count. Its end record
// is looked for backwards, because a comment of up to 64 KiB may follow it. Sizes and offsets that
// do not fit in 32 bits come from ZIP64: its end record, found through the locator right before the
// ordinary one, and the extra field of each entry. Names are UTF-8 when their bytes are valid
// UTF-8, whether the entry says so (bit 11) or not, since many packers write UTF-8 without the
// flag; otherwise they are code page 437, as the format prescribes. The time comes from the
// extended timestamp (0x5455) when there is one, in UTC; the DOS time stored otherwise is local
// time, as the packer's clock showed it.
namespace talcite::archive {

using namespace fs::core;

namespace {

constexpr std::uint32_t end_mark = 0x06054b50;
constexpr std::uint32_t locator_mark = 0x07064b50;
constexpr std::uint32_t end64_mark = 0x06064b50;
constexpr std::uint32_t central_mark = 0x02014b50;

constexpr std::size_t end_size = 22;
constexpr std::size_t locator_size = 20;
constexpr std::size_t end64_size = 56;
constexpr std::size_t central_size = 46;
constexpr std::size_t tail_most = end_size + 0xFFFF;

constexpr std::uint32_t wide32 = 0xFFFFFFFF;
constexpr std::uint16_t wide16 = 0xFFFF;

// The unsigned little-endian number of `much` bytes at `at`; zero past the end of `from`.
std::uint64_t le(std::span<const std::byte> from, std::size_t at, std::size_t much) {
    std::uint64_t made = 0;

    if (at + much > from.size()) return 0;

    for (std::size_t one = much; one > 0; --one) made = (made << 8) | std::to_integer<std::uint64_t>(from[at + one - 1]);

    return made;
}

// Exactly `much` bytes of the file from `at`, or a refusal naming the archive as damaged.
Bytes exact(const fs::File::Held& file, const std::filesystem::path& where, std::uintmax_t at, std::size_t much) {
    Bytes made(much);

    std::size_t got = 0;

    while (got < much) {
        const std::size_t now = read_into(file, at + got, std::span{made}.subspan(got));

        if (now == 0) damaged("The ZIP archive is cut short.", where);

        got += now;
    }

    return made;
}

// A DOS date and time, read as local time; the start of 1980 for nonsense.
std::chrono::sys_seconds dos_time(std::uint16_t date, std::uint16_t time) {
    std::tm told{};

    told.tm_year = 80 + (date >> 9);
    told.tm_mon = ((date >> 5) & 0x0F) - 1;
    told.tm_mday = date & 0x1F;
    told.tm_hour = time >> 11;
    told.tm_min = (time >> 5) & 0x3F;
    told.tm_sec = (time & 0x1F) * 2;
    told.tm_isdst = -1;

    if (told.tm_mon < 0 || told.tm_mon > 11 || told.tm_mday == 0) return std::chrono::sys_seconds{std::chrono::seconds{315532800}};

    const std::time_t made = std::mktime(&told);

    if (made == static_cast<std::time_t>(-1)) return std::chrono::sys_seconds{std::chrono::seconds{315532800}};

    return std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(made)}};
}

// The name as UTF-8: as it stands when it is valid UTF-8, whether the entry says so (bit 11) or
// not, and from code page 437 otherwise. A flagged name that is not UTF-8 after all is read the
// same way, rather than refused.
std::string named(std::string_view raw) { return utf8_whole(raw) ? std::string{raw} : from_cp437(raw); }

// Where the end record starts in `tail`, the last bytes of the file: the last place with its mark
// whose comment reaches no further than the file.
std::optional<std::size_t> end_in(std::span<const std::byte> tail) {
    if (tail.size() < end_size) return std::nullopt;

    for (std::size_t at = tail.size() - end_size + 1; at-- > 0;) {
        if (le(tail, at, 4) != end_mark) continue;

        if (at + end_size + le(tail, at + 20, 2) <= tail.size()) return at;
    }

    return std::nullopt;
}

// What one entry of the central directory says, before its name is laid out.
struct Told {
    std::uint16_t made_by = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint16_t time = 0;
    std::uint16_t date = 0;
    std::uint32_t crc = 0;
    std::uint64_t packed = 0;
    std::uint64_t size = 0;
    std::uint32_t outer = 0;
    std::uint64_t offset = 0;
    std::string raw;
    std::optional<std::string> unicode;
    std::optional<std::int64_t> stamped;
};

// Reads the extra fields of an entry: ZIP64 sizes and offset in the order the format gives them,
// each only where the 32-bit field is full; the extended timestamp; and the Unicode path, which
// counts only when it was made from this very name (its CRC-32 of the stored name matches).
void extras(std::span<const std::byte> field, Told& told) {
    for (std::size_t at = 0; at + 4 <= field.size();) {
        const auto id = static_cast<std::uint16_t>(le(field, at, 2));
        const auto much = static_cast<std::size_t>(le(field, at + 2, 2));
        const std::span<const std::byte> body = field.subspan(at + 4, std::min(much, field.size() - at - 4));

        if (id == 0x0001) {
            std::size_t in = 0;

            if (told.size == wide32 && in + 8 <= body.size()) told.size = le(body, std::exchange(in, in + 8), 8);
            if (told.packed == wide32 && in + 8 <= body.size()) told.packed = le(body, std::exchange(in, in + 8), 8);
            if (told.offset == wide32 && in + 8 <= body.size()) told.offset = le(body, in, 8);
        } else if (id == 0x5455 && body.size() >= 5 && (std::to_integer<int>(body[0]) & 1) != 0) {
            told.stamped = static_cast<std::int32_t>(static_cast<std::uint32_t>(le(body, 1, 4)));
        } else if (id == 0x7075 && body.size() >= 5 && std::to_integer<int>(body[0]) == 1) {
            const auto wanted = static_cast<std::uint32_t>(le(body, 1, 4));
            const std::span<const std::byte> raw{reinterpret_cast<const std::byte*>(told.raw.data()), told.raw.size()};

            if (crc32(0, raw) == wanted) {
                const std::span<const std::byte> name = body.subspan(5);

                told.unicode = std::string{reinterpret_cast<const char*>(name.data()), name.size()};
            }
        }

        at += 4 + much;
    }
}

// The member an entry describes: its laid-out name, kind, sizes, time and rights, and why it
// cannot be extracted, if it cannot.
Member member_of(const Told& told) {
    Member made;

    const std::string name = told.unicode && utf8_whole(*told.unicode) ? *told.unicode : named(told.raw);
    const Laid place = laid(name);

    made.name = place.name;
    made.stored = name;
    made.size = told.size;
    made.packed = told.packed;
    made.modified = told.stamped ? std::chrono::sys_seconds{std::chrono::seconds{*told.stamped}} : dos_time(told.date, told.time);

    const unsigned host = told.made_by >> 8;
    const std::uint32_t mode = told.outer >> 16;

    if (host == 3 && mode != 0) made.rights = static_cast<std::filesystem::perms>(mode & 0777);

    const bool slashed = !name.empty() && (name.back() == '/' || name.back() == '\\');

    if (slashed || (host == 3 && (mode & 0170000) == 0040000) || (host != 3 && (told.outer & 0x10) != 0)) {
        made.kind = fs::Kind::folder;
        made.size = 0;
    }

    if (place.unsafe) made.refusal = Refusal::unsafe;
    else if (host == 3 && (mode & 0170000) == 0120000) made.refusal = Refusal::link;
    else if ((told.flags & 0x0001) != 0 || told.method == 99) made.refusal = Refusal::encrypted;
    else if (made.kind == fs::Kind::file && told.method != 0 && told.method != 8) made.refusal = Refusal::method;

    return made;
}

// Refuses an archive whose members share or overlap their data: each member's local header (at
// least its fixed part) and packed data must end before the next member's header begins, and the
// last before the central directory at `directory`. One packed kernel read as thousands of
// members is the trick of a zip bomb (petabytes out of megabytes), never the work of a packer.
void apart_in(const Archive::Held& held, std::uintmax_t directory) {
    constexpr std::uintmax_t header = 30;

    std::vector<std::pair<std::uintmax_t, std::uintmax_t>> laid;

    laid.reserve(held.spots.size());

    for (const Spot& one : held.spots) laid.emplace_back(one.at, one.packed);

    std::sort(laid.begin(), laid.end());

    for (std::size_t at = 0; at < laid.size(); ++at) {
        const std::uintmax_t next = at + 1 < laid.size() ? laid[at + 1].first : directory;
        const std::uintmax_t ends = laid[at].first + header + laid[at].second;

        if (ends < laid[at].first || ends > next) damaged("ZIP members overlap, as in a zip bomb.", held.where);
    }
}

}

// Reads the central directory of the ZIP archive in `file` into `held`: one member and one spot
// (local header, packed size, CRC-32, method) per entry, in the archive's order. Bytes before the
// archive proper (a self-extracting program) shift every offset by as much, which the end record
// shows: the directory ends where that record begins. More than a million members, and members
// whose data overlap (`apart_in`), are refused: both are ways to make a small file cost the memory
// or the disk of the whole computer.
void zip_read(Archive::Held& held, const fs::File::Held& file, std::uintmax_t size) {
    if (size < end_size) damaged("Not a ZIP archive.", held.where);

    const std::uintmax_t tail_at = size - std::min<std::uintmax_t>(size, tail_most);
    const Bytes tail = exact(file, held.where, tail_at, static_cast<std::size_t>(size - tail_at));
    const std::optional<std::size_t> end = end_in(tail);

    if (!end) damaged("Not a ZIP archive.", held.where);

    const std::uintmax_t end_at = tail_at + *end;

    std::uint64_t count = le(tail, *end + 10, 2);
    std::uint64_t length = le(tail, *end + 12, 4);
    std::uint64_t offset = le(tail, *end + 16, 4);
    std::uintmax_t shift = 0;

    if (count == wide16 || length == wide32 || offset == wide32) {
        if (end_at < locator_size) damaged("The ZIP64 locator is missing.", held.where);

        const Bytes locator = exact(file, held.where, end_at - locator_size, locator_size);

        if (le(locator, 0, 4) != locator_mark) damaged("The ZIP64 locator is missing.", held.where);

        const std::uint64_t at = le(locator, 8, 8);

        if (at + end64_size > size) damaged("The ZIP64 end record lies outside the file.", held.where);

        const Bytes end64 = exact(file, held.where, at, end64_size);

        if (le(end64, 0, 4) != end64_mark) damaged("The ZIP64 end record is damaged.", held.where);

        count = le(end64, 32, 8);
        length = le(end64, 40, 8);
        offset = le(end64, 48, 8);
    } else if (offset + length <= end_at) {
        shift = end_at - (offset + length);
    }

    if (offset + shift + length > size) damaged("The central directory lies outside the file.", held.where);

    if (count > members_most) damaged("The archive holds too many members.", held.where);

    const Bytes table = exact(file, held.where, offset + shift, static_cast<std::size_t>(length));

    std::size_t at = 0;

    for (std::uint64_t entry = 0; entry < count; ++entry) {
        if (at + central_size > table.size() || le(table, at, 4) != central_mark) damaged("The central directory is damaged.", held.where);

        Told told;

        told.made_by = static_cast<std::uint16_t>(le(table, at + 4, 2));
        told.flags = static_cast<std::uint16_t>(le(table, at + 8, 2));
        told.method = static_cast<std::uint16_t>(le(table, at + 10, 2));
        told.time = static_cast<std::uint16_t>(le(table, at + 12, 2));
        told.date = static_cast<std::uint16_t>(le(table, at + 14, 2));
        told.crc = static_cast<std::uint32_t>(le(table, at + 16, 4));
        told.packed = le(table, at + 20, 4);
        told.size = le(table, at + 24, 4);

        const auto name = static_cast<std::size_t>(le(table, at + 28, 2));
        const auto extra = static_cast<std::size_t>(le(table, at + 30, 2));
        const auto comment = static_cast<std::size_t>(le(table, at + 32, 2));

        told.outer = static_cast<std::uint32_t>(le(table, at + 38, 4));
        told.offset = le(table, at + 42, 4);

        if (at + central_size + name + extra + comment > table.size()) damaged("The central directory is damaged.", held.where);

        told.raw.assign(reinterpret_cast<const char*>(table.data() + at + central_size), name);

        extras(std::span{table}.subspan(at + central_size + name, extra), told);

        held.members.push_back(member_of(told));
        held.spots.push_back(Spot{told.offset + shift, told.packed, told.crc, told.method});

        at += central_size + name + extra + comment;
    }

    apart_in(held, offset + shift);
}

}

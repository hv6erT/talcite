#include "talcite/archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/executor.hpp"

#include "archiving.hpp"
#include "fscore.hpp"
#include "pouring.hpp"

// Archives as folders that can only be read: ZIP, and tar plain or through gzip. Opening reads the
// table of contents once (`zip_read`, `tar_read`) into members with names laid out safely
// (`laid`): a name that leads out of the archive (an absolute path, a drive, `..`) or carries
// control characters stays listed but refused, so that extracting can never write outside the
// place it was given, whatever the archive says. Folders that no entry names but the names imply
// are added, and of two entries with one name the later counts, as unpackers do.
//
// Extracting writes like copying (`pouring.hpp`): a file appears under its name only whole, an
// existing one is replaced by writing beside it and renaming, a conflict is settled by the same
// rule (`settled_on`), and a folder the extraction made is taken back when it does not finish. The
// bytes are checked as they come: a ZIP member against its CRC-32 and size, so a damaged member
// never stays on disk looking whole. A tar is read in its own order, in one pass for all the picks
// together, because through gzip there is no other way to reach a member than through the ones
// before it.
namespace talcite::archive {

using namespace fs::core;

namespace {

constexpr std::size_t piece = 64 * 1024;

constexpr std::size_t packed_piece = 16 * 1024;

constexpr std::uint32_t local_mark = 0x04034b50;

constexpr std::size_t local_size = 30;

// The upper half of code page 437, as Unicode.
constexpr std::array<char32_t, 128> cp437{
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

// The table of the CRC-32 of ZIP and gzip (the reflected polynomial 0xEDB88320), made once.
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

// Whether `inner` lies in `folder` or is it; everything lies in the empty folder, the archive's
// root.
bool inside(const std::filesystem::path& inner, const std::filesystem::path& folder) {
    if (folder.empty()) return true;

    auto mine = inner.begin();

    for (const std::filesystem::path& part : folder) {
        if (mine == inner.end() || *mine != part) return false;

        ++mine;
    }

    return true;
}

// The modification time to stamp a file or a folder with, nothing when the archive gave none.
std::optional<Instant> instant_of(const Member& one) {
    if (one.modified.time_since_epoch().count() == 0) return std::nullopt;

    return Instant{std::chrono::duration_cast<std::chrono::nanoseconds>(one.modified.time_since_epoch())};
}

// The rights an archive gave a member, applied on systems that have them; elsewhere, and when the
// archive gave none, the file keeps what it was created with.
void righted([[maybe_unused]] const std::filesystem::path& where, [[maybe_unused]] const Member& one) {
#ifndef _WIN32
    if (!one.rights) return;

    std::error_code ignored;

    stood::permissions(long_form(where), *one.rights & stood::perms::all, ignored);
#endif
}

// The archive's table made whole: entries without a name (the root itself, "./") left out, of two
// with one name the later kept in the place of the earlier, and every folder that the names
// imply but no entry names added after them, without a time of its own. An unsafe name implies
// nothing: what it was laid out to is for showing, not a place.
void finished(Archive::Held& held) {
    std::vector<Member> members;
    std::vector<Spot> spots;
    std::map<std::filesystem::path, std::size_t> named;

    for (std::size_t at = 0; at < held.members.size(); ++at) {
        Member& one = held.members[at];

        if (one.name.empty()) continue;

        if (const auto found = named.find(one.name); found != named.end()) {
            members[found->second] = std::move(one);
            spots[found->second] = held.spots[at];

            continue;
        }

        named.emplace(one.name, members.size());
        members.push_back(std::move(one));
        spots.push_back(held.spots[at]);
    }

    const std::size_t given = members.size();

    for (std::size_t at = 0; at < given; ++at) {
        if (members[at].refusal == Refusal::unsafe) continue;

        for (std::filesystem::path up = members[at].name.parent_path(); !up.empty(); up = up.parent_path()) {
            if (named.contains(up)) break;

            Member folder;

            folder.name = up;
            folder.stored = fs::to_string(up);
            folder.kind = fs::Kind::folder;
            folder.modified = std::chrono::sys_seconds{};

            named.emplace(up, members.size());
            members.push_back(std::move(folder));
            spots.push_back(Spot{});
        }
    }

    held.members = std::move(members);
    held.spots = std::move(spots);
}

// The bytes of one ZIP member, as they come: stored ones read as they are, deflated ones through
// the unpacker, and the whole checked against the size and the CRC-32 the directory gives. A
// member that gives more than its size, or less, or other bytes, is damage.
class Unzipping {
public:
    // Finds where the member's data starts, after its local header, which must be there.
    Unzipping(const Archive::Held& held, const fs::File::Held& file, std::uintmax_t size, std::size_t which)
        : held_(held), file_(file), member_(held.members[which]), spot_(held.spots[which]) {
        Bytes local(local_size);

        if (spot_.at + local_size > size || read_into(file_, spot_.at, local) != local_size) damaged("A ZIP member lies outside the file.", held_.where);

        const auto lead = [&local](std::size_t at, std::size_t much) {
            std::uint32_t made = 0;

            for (std::size_t one = much; one > 0; --one) made = (made << 8) | std::to_integer<std::uint32_t>(local[at + one - 1]);

            return made;
        };

        if (lead(0, 4) != local_mark) damaged("A ZIP local header is damaged.", held_.where);

        at_ = spot_.at + local_size + lead(26, 2) + lead(28, 2);

        if (at_ + spot_.packed > size) damaged("A ZIP member lies outside the file.", held_.where);

        if (spot_.method == 8) inflater_ = held_.unpacking.provider().started(Packing::deflate);
    }

    // The next bytes of the member into `into`; false when there are none any more.
    bool pumped(Bytes& into) {
        into.clear();

        while (into.empty()) {
            if (taken_ >= spot_.packed) return false;

            Bytes packed(static_cast<std::size_t>(std::min<std::uintmax_t>(spot_.method == 8 ? packed_piece : piece, spot_.packed - taken_)));

            const std::size_t got = read_into(file_, at_ + taken_, packed);

            if (got == 0) damaged("A ZIP member is cut short.", held_.where);

            taken_ += got;
            packed.resize(got);

            into = inflater_ ? inflater_->fed(packed) : std::move(packed);
        }

        given_ += into.size();
        crc_ = crc32(crc_, into);

        if (given_ > member_.size) damaged("A ZIP member is longer than the archive says: " + member_.stored + ".", held_.where);

        return true;
    }

    // Refuses a member whose bytes did not come out as the directory says.
    void finished() const {
        if (given_ != member_.size || (inflater_ && !inflater_->ended())) damaged("A ZIP member is cut short: " + member_.stored + ".", held_.where);

        if (crc_ != spot_.crc) damaged("A ZIP member is damaged (its CRC-32 differs): " + member_.stored + ".", held_.where);
    }

private:
    const Archive::Held& held_;
    const fs::File::Held& file_;
    const Member& member_;
    Spot spot_;
    std::uintmax_t at_ = 0;
    std::uintmax_t taken_ = 0;
    std::uintmax_t given_ = 0;
    std::uint32_t crc_ = 0;
    std::unique_ptr<Unpacking> inflater_;
};

// The bytes of one tar member: as many as its size, from where the source stands, which is the
// start of its data.
class Untarring {
public:
    // The member of `size` bytes from the source's current place.
    Untarring(Source& source, std::uintmax_t size) : source_(source), left_(size) {}

    // The next bytes of the member into `into`; false when there are none any more.
    bool pumped(Bytes& into) {
        if (left_ == 0) return false;

        into.resize(static_cast<std::size_t>(std::min<std::uintmax_t>(piece, left_)));

        source_.taken(into);

        left_ -= into.size();

        return true;
    }

    // Nothing to check: tar carries no checksum of the data.
    void finished() const {}

private:
    Source& source_;
    std::uintmax_t left_ = 0;
};

// One file to write: which member, and where.
struct Job {
    std::size_t member = 0;
    std::filesystem::path to;
};

// One folder to make: which member (none for the folder a pick names when it is the root), and
// where.
struct Making {
    std::optional<std::size_t> member;
    std::filesystem::path to;
};

// What the picks come to: the files to write in the archive's order, the folders to make parents
// first, the refused members, and the bytes in all.
struct Plan {
    std::vector<Job> files;
    std::vector<Making> folders;
    std::vector<Member> refused;
    std::uintmax_t bytes = 0;
};

// Lays the picks out over the members: a file goes where its pick says, a folder (the root
// included, an empty name) goes there with everything under it, each in its place below. A pick
// that names nothing in the archive is a refusal.
Plan planned(const Archive::Held& held, const std::vector<Pick>& picks) {
    Plan made;

    for (const Pick& pick : picks) {
        const auto named = std::find_if(held.members.begin(), held.members.end(), [&pick](const Member& one) { return one.name == pick.inner; });

        if (!pick.inner.empty() && named == held.members.end()) amiss("The archive holds no such member.", held.where / pick.inner);

        if (!pick.inner.empty() && named->kind == fs::Kind::file) {
            const auto at = static_cast<std::size_t>(named - held.members.begin());

            if (named->refusal != Refusal::none) made.refused.push_back(*named);
            else made.files.push_back(Job{at, pick.to});

            continue;
        }

        made.folders.push_back(Making{pick.inner.empty() ? std::nullopt : std::optional{static_cast<std::size_t>(named - held.members.begin())}, pick.to});

        for (std::size_t at = 0; at < held.members.size(); ++at) {
            const Member& one = held.members[at];

            if (one.name == pick.inner || !inside(one.name, pick.inner)) continue;

            if (one.refusal != Refusal::none) {
                made.refused.push_back(one);

                continue;
            }

            const std::filesystem::path to = pick.to / (pick.inner.empty() ? one.name : one.name.lexically_relative(pick.inner));

            if (one.kind == fs::Kind::folder) made.folders.push_back(Making{at, to});
            else made.files.push_back(Job{at, to});
        }
    }

    std::stable_sort(made.files.begin(), made.files.end(),
                     [&held](const Job& one, const Job& two) { return held.spots[one.member].at < held.spots[two.member].at; });

    std::stable_sort(made.folders.begin(), made.folders.end(), [](const Making& one, const Making& two) {
        return std::distance(one.to.begin(), one.to.end()) < std::distance(two.to.begin(), two.to.end());
    });

    for (const Job& one : made.files) made.bytes += held.members[one.member].size;

    return made;
}

// Writes one member from `pump` to `to`, the conflict settled by `how`: nothing when skipped
// (its bytes still count as done; the next member is reached from wherever the reading stands),
// false when the work was stopped halfway (nothing of the file stays then), a refusal when the
// bytes are damaged.
template <typename Pump>
bool written(const Member& member, Pump& pump, const std::filesystem::path& to, fs::Conflict how, Counted& counted, Undoing& undoing) {
    const std::filesystem::path where = settled_on(to, how);

    if (where.empty()) {
        counted.added(member.size);

        return true;
    }

    const bool fresh = kind_at(where) == fs::Kind::none;

    const auto fill = [&](const std::filesystem::path& into, bool& made) -> std::shared_ptr<fs::File::Held> {
        std::shared_ptr<fs::File::Held> yours = open_at(into, fs::Mode::exclusive);

        made = true;

        Bytes now;
        std::uintmax_t at = 0;

        while (pump.pumped(now)) {
            if (!rested()) return nullptr;

            for (std::size_t put = 0; put < now.size();) put += write_from(*yours, at + put, std::span{now}.subspan(put));

            at += now.size();
            counted.added(now.size());
        }

        pump.finished();

        return yours;
    };

    if (!poured_at(where, Placing{}, instant_of(member), fill)) return false;

    righted(where, member);

    if (fresh) undoing.made(where);

    return true;
}

}

// Whether the bytes are valid UTF-8.
bool utf8_whole(std::string_view raw) { return utf8::strict(raw); }

// The bytes read as code page 437, in UTF-8.
std::string from_cp437(std::string_view raw) {
    std::string made;

    for (const char one : raw) {
        const auto byte = static_cast<unsigned char>(one);

        made += byte < 0x80 ? std::string(1, one) : utf8::encode(cp437[byte - 0x80]);
    }

    return made;
}

// The bytes read as Latin-1, in UTF-8.
std::string from_latin1(std::string_view raw) {
    std::string made;

    for (const char one : raw) made += utf8::encode(static_cast<char32_t>(static_cast<unsigned char>(one)));

    return made;
}

// The CRC-32 of `bytes` continued from `crc`; zero starts a new one.
std::uint32_t crc32(std::uint32_t crc, std::span<const std::byte> bytes) {
    const std::array<std::uint32_t, 256>& table = crc_table();

    std::uint32_t made = ~crc;

    for (const std::byte one : bytes) made = table[(made ^ std::to_integer<std::uint32_t>(one)) & 0xFFU] ^ (made >> 8);

    return ~made;
}

// Throws an `fs` `Trouble` for the archive at `where`.
void damaged(const std::string& said, const std::filesystem::path& where) { amiss(said, where); }

// A stored name laid out as a path within the archive: backslashes count as slashes, empty parts
// and `.` drop out. Unsafe, and still laid out as well as it goes for showing, is a name that
// starts at the root, names a drive or an alternate stream (a colon anywhere), climbs out with
// `..`, or carries control characters.
Laid laid(std::string_view stored) {
    Laid made;

    std::string text{stored};

    std::replace(text.begin(), text.end(), '\\', '/');

    if (!text.empty() && text.front() == '/') made.unsafe = true;

    if (std::any_of(text.begin(), text.end(), [](char one) { return static_cast<unsigned char>(one) < 0x20 || one == 0x7F || one == ':'; }))
        made.unsafe = true;

    for (std::size_t from = 0; from <= text.size();) {
        const std::size_t to = std::min(text.find('/', from), text.size());
        const std::string part = text.substr(from, to - from);

        from = to + 1;

        if (part.empty() || part == ".") continue;

        if (part == "..") {
            made.unsafe = true;

            continue;
        }

        made.name /= fs::from_string(part);
    }

    return made;
}

// The archive's file.
const std::filesystem::path& Archive::where() const { return held_->where; }

// The archive's format.
Format Archive::format() const { return held_->format; }

// Every member, in the archive's order, followed by the folders only the names imply.
const std::vector<Member>& Archive::members() const { return held_->members; }

// The place of the member with this name among `members`, or nothing.
std::optional<std::size_t> Archive::found(const std::filesystem::path& inner) const {
    const auto at = std::find_if(held_->members.begin(), held_->members.end(), [&inner](const Member& one) { return one.name == inner; });

    if (at == held_->members.end()) return std::nullopt;

    return static_cast<std::size_t>(at - held_->members.begin());
}

// The places of the members directly in `folder` (the empty path is the root), in the archive's
// order.
std::vector<std::size_t> Archive::within(const std::filesystem::path& folder) const {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < held_->members.size(); ++at)
        if (held_->members[at].name.parent_path() == folder) made.push_back(at);

    return made;
}

// Extracts the picks, each member (or folder with all under it, or the root, an empty name) to
// where its pick says, in a pool task, with progress in bytes. A conflict is settled by `how`, as
// a copy settles it: for a folder pick once for the folder (skipped, refused, renamed, or merged
// with what stands there when overwriting), and then for each file in it. Gives the members it
// left out because they are refused; a pick that names nothing, a folder that cannot be made,
// damage and a file that cannot be written are a refusal, and what this extraction made then goes
// again, unless it merged into folders that stood before. Stopping stops between pieces.
Promise<std::vector<Member>> Archive::extracted(std::vector<Pick> picks, fs::Conflict how) const {
    return run(io(), [held = held_, given = std::move(picks), how](Work& work) -> std::vector<Member> {
        Counted counted{&work};

        std::vector<Pick> picks;

        for (const Pick& one : given) {
            const auto named = std::find_if(held->members.begin(), held->members.end(), [&one](const Member& each) { return each.name == one.inner; });
            const bool folder = one.inner.empty() || (named != held->members.end() && named->kind == fs::Kind::folder);

            if (!folder) {
                picks.push_back(one);

                continue;
            }

            if (std::filesystem::path to = settled_on(one.to, how); !to.empty()) picks.push_back(Pick{one.inner, std::move(to)});
        }

        Plan plan = planned(*held, picks);

        counted.total(plan.bytes);

        Undoing undoing{std::all_of(picks.begin(), picks.end(), [](const Pick& one) { return kind_at(one.to) == fs::Kind::none; })};

        for (const Making& one : plan.folders) {
            const fs::Kind there = kind_at(one.to);

            if (there == fs::Kind::folder) continue;

            if (there != fs::Kind::none) amiss("Something other than a folder stands where a folder goes.", one.to);

            make_folder_at(one.to);
            undoing.made(one.to);
        }

        const std::shared_ptr<fs::File::Held> file = open_at(held->where, fs::Mode::read);
        const std::uintmax_t size = sized(*file);

        if (held->format == Format::zip) {
            for (const Job& one : plan.files) {
                if (!rested()) throw Cancelled{};

                Unzipping pump{*held, *file, size, one.member};

                if (!written(held->members[one.member], pump, one.to, how, counted, undoing)) throw Cancelled{};
            }
        } else {
            Source source{file, held->where, size, held->format == Format::tar_gzip ? held->unpacking.provider().started(Packing::gzip) : nullptr};

            for (const Job& one : plan.files) {
                if (!rested()) throw Cancelled{};

                source.skipped(held->spots[one.member].at - source.at());

                Untarring pump{source, held->members[one.member].size};

                if (!written(held->members[one.member], pump, one.to, how, counted, undoing)) throw Cancelled{};
            }
        }

        for (auto at = plan.folders.rbegin(); at != plan.folders.rend(); ++at) {
            if (!at->member) continue;

            const Member& one = held->members[*at->member];

            if (const std::optional<Instant> when = instant_of(one)) stamp_at(at->to, *when);

            righted(at->to, one);
        }

        undoing.keep();

        return plan.refused;
    });
}

// The format of an archive by the end of its name: .zip; .tar; .tar.gz and .tgz. Nothing for any
// other name, whatever the file holds.
std::optional<Format> format_of(const std::filesystem::path& name) {
    std::string lower = fs::to_string(name.filename());

    std::transform(lower.begin(), lower.end(), lower.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

    const auto ends = [&lower](std::string_view tail) { return lower.size() > tail.size() && lower.ends_with(tail); };

    if (ends(".zip")) return Format::zip;

    if (ends(".tar.gz") || ends(".tgz")) return Format::tar_gzip;

    if (ends(".tar")) return Format::tar;

    return std::nullopt;
}

// Opens the archive at `where` in a pool task and reads its table of contents; the format comes
// from the name (`format_of`). `unpacking` inflates deflated ZIP members and the gzip of a
// .tar.gz; without it such an archive still opens, and only reading those bytes is refused.
Promise<Archive> opened(const std::filesystem::path& where, Seam<UnpackProvider> unpacking) {
    return run(io(), [where, unpacking = std::move(unpacking)]() -> Archive {
        const std::optional<Format> format = format_of(where);

        if (!format) amiss("Not an archive talcite reads.", where);

        auto held = std::make_shared<Archive::Held>();

        held->where = where;
        held->format = *format;
        held->unpacking = unpacking;

        const std::shared_ptr<fs::File::Held> file = open_at(where, fs::Mode::read);
        const std::uintmax_t size = sized(*file);

        if (*format == Format::zip) {
            zip_read(*held, *file, size);
        } else {
            Source source{file, where, size, *format == Format::tar_gzip ? unpacking.provider().started(Packing::gzip) : nullptr};

            tar_read(*held, source);
        }

        finished(*held);

        return Archive{held};
    });
}

}

#include "archiving.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <map>
#include <memory>
#include <utility>

#include "tal/crypto.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

// Archives as folders that can only be read, ported from talcite's archive.cpp: zip, and tar plain
// or through gzip. Opening reads the table of contents once (`zip_read`, `tar_read`) into members
// whose names are laid out safely (`laid`): a name that leads out of the archive (an absolute path,
// a drive, `..`) or carries control characters stays listed but refused, so that extracting never
// writes outside the place it was given, whatever the archive says. Folders that the names imply
// but no entry names are added, and of two entries with one name the later counts, as unpackers do.
//
// Extracting writes through `tal::fs`: a file appears under its name only whole, written beside it
// and moved there, a conflict settled as a copy settles it, and what the extraction made goes again
// when it does not finish, unless it went into places that stood before. The bytes are checked as
// they come: a zip member against its CRC-32 and size, so a damaged member never stays on disk
// looking whole. A tar is read in its own order, in one pass for all the picks, since through gzip
// no member is reached but through the ones before it. Before the first byte the whole must fit:
// an archive that unpacks to more than the target holds, a zip bomb among them, is refused instead
// of filling the disk, and no member comes out larger than it says.
namespace tal::archive {

namespace archiving {

namespace {

// The upper half of code page 437, as Unicode.
constexpr std::array<char32_t, 128> cp437{
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6,
    0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1,
    0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555,
    0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566,
    0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590,
    0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1,
    0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

// The largest file of FAT.
constexpr std::uint64_t fat_most = (std::uint64_t{1} << 32) - 1;

// The furthest time from 1970 an archive may say, about 272 years either way: a clock counts no
// further in nanoseconds without overflowing.
constexpr std::int64_t furthest = std::int64_t{1} << 33;

}

// The moment `seconds` after 1970, held within 272 years of it.
std::chrono::system_clock::time_point instant(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds{std::clamp(seconds, -furthest, furthest)})};
}

// The bytes `raw` read as code page 437, in UTF-8.
std::string from_cp437(std::string_view raw) {
    std::string made;

    for (const char one : raw) {
        const auto byte = static_cast<unsigned char>(one);

        if (byte < 0x80) made += one;
        else text::encoded(made, cp437[byte - 0x80]);
    }

    return made;
}

// The bytes `raw` read as Latin-1, in UTF-8.
std::string from_latin1(std::string_view raw) {
    std::string made;

    for (const char one : raw) text::encoded(made, static_cast<char32_t>(static_cast<unsigned char>(one)));

    return made;
}

// Throws `Trouble` unless files of `bytes` in all, the largest of `largest`, fit on `volume` for
// `to`: full when it has less room, unsupported when the largest is too large for FAT.
void fits(const fs::Volume& volume, std::uint64_t bytes, std::uint64_t largest, const Path& to) {
    std::string format = volume.format;

    std::transform(format.begin(), format.end(), format.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

    if (bytes > volume.free) throw Trouble{Kind::full, "The archive's files do not fit on the volume.", to.text()};

    if (largest > fat_most && (format == "vfat" || format == "fat" || format == "fat32" || format == "msdos"))
        throw Trouble{Kind::unsupported, "A file of the archive is too large for FAT.", to.text()};
}

// Throws `Trouble` (malformed) saying `said` about the archive at `where`.
void damaged(const std::string& said, const Path& where) { throw Trouble{Kind::malformed, said, where.text()}; }

// The stored name `stored` laid out as a path within the archive: backslashes count as slashes,
// empty parts and `.` drop out. Unsafe, and laid out as well as it goes for showing, is a name that
// starts at the root, names a drive or an alternate stream (a colon anywhere), climbs out with
// `..`, or carries control characters.
Laid laid(std::string_view stored) {
    Laid made;
    std::string text{stored};
    std::string joined;

    std::replace(text.begin(), text.end(), '\\', '/');

    if (!text.empty() && text.front() == '/') made.unsafe = true;

    if (std::any_of(text.begin(), text.end(), [](char one) { return static_cast<unsigned char>(one) < 0x20 || one == 0x7F || one == ':'; })) made.unsafe = true;

    for (std::size_t from = 0; from <= text.size();) {
        const std::size_t to = std::min(text.find('/', from), text.size());
        const std::string part = text.substr(from, to - from);

        from = to + 1;

        if (part.empty() || part == ".") continue;

        if (part == "..") {
            made.unsafe = true;

            continue;
        }

        joined += (joined.empty() ? "" : "/") + part;
    }

    made.name = Path{joined};

    return made;
}

}

namespace {

using archiving::damaged;
using archiving::Spot;

// What is written at once.
constexpr std::size_t piece = 64 * 1024;

// What is read of a deflated member at once.
constexpr std::size_t packed_piece = 16 * 1024;

constexpr std::uint32_t local_mark = 0x04034b50;
constexpr std::size_t local_size = 30;

// Whether the in-archive name `inner` is `folder` or lies in it; everything lies in the root, the
// empty name. Names compare exactly, as archives hold them.
bool inside(const Path& inner, const Path& folder) {
    if (folder.empty()) return true;

    const std::string_view mine = inner.stored();
    const std::string_view theirs = folder.stored();

    return mine == theirs || (mine.size() > theirs.size() && mine.starts_with(theirs) && mine[theirs.size()] == '/');
}

// `inner` relative to `folder`, in which it lies.
std::string_view below(const Path& inner, const Path& folder) { return folder.empty() ? inner.stored() : inner.stored().substr(folder.stored().size() + 1); }

// The archive's table made whole: entries without a name (the root itself, "./") left out, of two
// with one name the later in the place of the earlier, and every folder that the names imply but no
// entry names added after them, without a time of their own. An unsafe name implies nothing: what
// it was laid out to is for showing, not a place.
void finished(Archive::Held& held) {
    std::vector<Member> members;
    std::vector<Spot> spots;
    std::map<std::string, std::size_t> named;

    for (std::size_t at = 0; at < held.members.size(); ++at) {
        Member& one = held.members[at];

        if (one.name.empty()) continue;

        if (const auto found = named.find(std::string{one.name.stored()}); found != named.end()) {
            members[found->second] = std::move(one);
            spots[found->second] = held.spots[at];

            continue;
        }

        named.emplace(std::string{one.name.stored()}, members.size());
        members.push_back(std::move(one));
        spots.push_back(held.spots[at]);
    }

    const std::size_t given = members.size();

    for (std::size_t at = 0; at < given; ++at) {
        if (members[at].refusal == Refusal::unsafe) continue;

        for (Path up = members[at].name.parent(); !up.empty(); up = up.parent()) {
            if (named.contains(std::string{up.stored()})) break;

            Member folder;

            folder.name = up;
            folder.stored = std::string{up.stored()};
            folder.type = fs::Type::folder;

            named.emplace(std::string{up.stored()}, members.size());
            members.push_back(std::move(folder));
            spots.push_back(Spot{});
        }
    }

    held.members = std::move(members);
    held.spots = std::move(spots);
}

// The entry at `where`, or nothing when there is none.
Task<std::optional<fs::Entry>> standing(Path where) {
    std::optional<fs::Entry> made;

    try {
        made = co_await fs::look(where);
    } catch (const Trouble& why) {
        if (why.kind() != Kind::missing) throw;
    }

    co_return made;
}

// A free name beside `taken`: "name (2).ext", "name (3).ext" and so on.
Task<Path> renamed(Path taken) {
    const std::string stem{taken.stem()};
    const std::string extension = taken.extension().empty() ? std::string{} : "." + std::string{taken.extension()};

    for (int number = 2;; ++number) {
        const Path made = taken.parent() / (stem + " (" + std::to_string(number) + ")" + extension);
        const std::optional<fs::Entry> there = co_await standing(made);

        if (!there) co_return made;
    }
}

// Where an entry for `to` goes by the conflict rule `how`: `to` when it is free or replaced, a free
// name beside it, or nothing to skip; throws `Trouble` (exists) to fail.
Task<std::optional<Path>> settled_on(Path to, fs::Conflict how) {
    const std::optional<fs::Entry> there = co_await standing(to);

    if (!there) co_return to;

    switch (how) {
        case fs::Conflict::fail: throw Trouble{Kind::exists, "The target already exists.", to.text()};
        case fs::Conflict::skip: co_return std::nullopt;
        case fs::Conflict::rename: co_return co_await renamed(to);
        case fs::Conflict::replace: break;
    }

    co_return to;
}

// What an extraction made, taken away again when it does not finish, last first; unless what it
// went into stood before, where taking anything away could take what was there.
class Undoing {
public:
    // An undoing that acts only when `whole` says every target was new.
    explicit Undoing(bool whole) : whole_(whole) {}

    // Remembers that `one` was made.
    void made(Path one) { made_.push_back(std::move(one)); }

    // Takes back what was made, as well as it goes.
    Task<void> undone() {
        if (!whole_) co_return;

        for (auto at = made_.rbegin(); at != made_.rend(); ++at) {
            try {
                co_await fs::remove(*at, fs::Removing{.trash = false, .deep = true});
            } catch (const Trouble&) {
            }
        }
    }

private:
    bool whole_;
    std::vector<Path> made_;
};

// The bytes of one member, as they come.
class Pump {
public:
    Pump() = default;
    virtual ~Pump() = default;

    Pump(const Pump&) = delete;
    Pump& operator=(const Pump&) = delete;

    // The next bytes, or nothing at the end.
    [[nodiscard]] virtual Task<std::optional<Bytes>> next() = 0;

    // Refuses a member whose bytes did not come out as the archive says.
    virtual void finished() const = 0;
};

// The bytes of one zip member: stored as they are, deflated through the inflater, the whole
// checked against the size and the CRC-32 of the directory. A member that gives more than its
// size, or less, or other bytes, is damage.
class Unzipping final : public Pump {
public:
    // The member `which` of `held` in `file` of `size` bytes; its data starts after its local
    // header, which must be there.
    static Task<std::unique_ptr<Unzipping>> started(const Archive::Held& held, fs::File file, std::uint64_t size, std::size_t which) {
        auto made = std::unique_ptr<Unzipping>(new Unzipping(held, file, which));
        const Spot& spot = held.spots[which];

        if (spot.at > size || size - spot.at < local_size) damaged("A zip member lies outside the file.", held.where);

        const Bytes local = co_await file.read(spot.at, local_size);

        if (local.size() != local_size) damaged("A zip member lies outside the file.", held.where);

        const auto le = [&local](std::size_t at) { return std::to_integer<std::uint32_t>(local[at]) | (std::to_integer<std::uint32_t>(local[at + 1]) << 8); };

        if ((le(0) | (le(2) << 16)) != local_mark) damaged("A zip local header is damaged.", held.where);

        made->at_ = spot.at + local_size + le(26) + le(28);

        if (made->at_ > size || size - made->at_ < spot.packed) damaged("A zip member lies outside the file.", held.where);

        if (spot.method == 8) made->inflater_.emplace(inflating::Wrapping::raw);

        co_return made;
    }

    // The next bytes of the member, or nothing when there are no more.
    Task<std::optional<Bytes>> next() override {
        const Spot& spot = held_.spots[which_];
        const Member& member = held_.members[which_];
        Bytes made;

        while (made.empty()) {
            if (taken_ >= spot.packed) co_return std::nullopt;

            const Bytes packed = co_await file_.read(at_ + taken_, static_cast<std::size_t>(std::min<std::uint64_t>(inflater_ ? packed_piece : piece, spot.packed - taken_)));

            if (packed.empty()) damaged("A zip member is cut short.", held_.where);

            taken_ += packed.size();
            made = inflater_ ? inflater_->fed(packed) : packed;
        }

        given_ += made.size();
        crc_ = inflating::crc32(crc_, made);

        if (given_ > member.size) damaged("A zip member is longer than the archive says: " + member.stored + ".", held_.where);

        co_return made;
    }

    // Refuses a member that came out shorter than its size, or other than its CRC-32.
    void finished() const override {
        const Member& member = held_.members[which_];

        if (given_ != member.size || (inflater_ && !inflater_->ended())) damaged("A zip member is cut short: " + member.stored + ".", held_.where);

        if (crc_ != held_.spots[which_].crc) damaged("A zip member is damaged (its CRC-32 differs): " + member.stored + ".", held_.where);
    }

private:
    // The pump of the member `which` of `held` in `file`.
    Unzipping(const Archive::Held& held, fs::File file, std::size_t which) : held_(held), file_(std::move(file)), which_(which) {}

    const Archive::Held& held_;
    fs::File file_;
    std::size_t which_;
    std::uint64_t at_ = 0;
    std::uint64_t taken_ = 0;
    std::uint64_t given_ = 0;
    std::uint32_t crc_ = 0;
    std::optional<inflating::Inflater> inflater_;
};

// The bytes of one tar member: `size` of them from where the source stands, the start of its data.
class Untarring final : public Pump {
public:
    // The member of `size` bytes from the current place of `source`.
    Untarring(archiving::Source& source, std::uint64_t size) : source_(source), left_(size) {}

    // The next bytes of the member, or nothing when there are no more.
    Task<std::optional<Bytes>> next() override {
        if (left_ == 0) co_return std::nullopt;

        Bytes made(static_cast<std::size_t>(std::min<std::uint64_t>(piece, left_)));

        co_await source_.taken(made);
        left_ -= made.size();

        co_return made;
    }

    // Nothing to check: tar carries no checksum of the data.
    void finished() const override {}

private:
    archiving::Source& source_;
    std::uint64_t left_;
};

// One file to write: which member, and where.
struct Job {
    std::size_t member = 0;
    Path to;
};

// One folder to make: which member (none for the folder a pick of the root names), and where.
struct Making {
    std::optional<std::size_t> member;
    Path to;
};

// What the picks come to: the files to write in the archive's order, the folders to make (each made
// with the folders on the way to it), the members refused, and the bytes in all.
struct Plan {
    std::vector<Job> files;
    std::vector<Making> folders;
    std::vector<Member> refused;
    std::uint64_t bytes = 0;
};

// The picks `picks` laid over the members of `held`: a file goes where its pick says, a folder (the
// root, an empty name, too) goes there with everything under it, each in its place below. A pick
// that names nothing in the archive is missing.
Plan planned(const Archive::Held& held, const std::vector<Pick>& picks) {
    Plan made;

    for (const Pick& pick : picks) {
        const auto named = std::find_if(held.members.begin(), held.members.end(), [&pick](const Member& one) { return one.name == pick.inner; });

        if (!pick.inner.empty() && named == held.members.end()) throw Trouble{Kind::missing, "The archive holds no such member.", (held.where / pick.inner.stored()).text()};

        if (!pick.inner.empty() && named->type == fs::Type::file) {
            if (named->refusal != Refusal::none) made.refused.push_back(*named);
            else made.files.push_back(Job{static_cast<std::size_t>(named - held.members.begin()), pick.to});

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

            const Path to = pick.to / below(one.name, pick.inner);

            if (one.type == fs::Type::folder) made.folders.push_back(Making{at, to});
            else made.files.push_back(Job{at, to});
        }
    }

    std::stable_sort(made.files.begin(), made.files.end(), [&held](const Job& one, const Job& two) { return held.spots[one.member].at < held.spots[two.member].at; });

    for (const Job& one : made.files) made.bytes += held.members[one.member].size;

    return made;
}

// Throws `Trouble` unless `bytes` fit where `to` goes, on the volume of the nearest folder on the
// way to it that stands (`fits`).
Task<void> fitted(Path to, std::uint64_t bytes, std::uint64_t largest) {
    Path standing_at = to.parent();

    while (!standing_at.empty() && !standing_at.root()) {
        const std::optional<fs::Entry> there = co_await standing(standing_at);

        if (there) break;

        standing_at = standing_at.parent();
    }

    archiving::fits(co_await fs::volume(standing_at.empty() ? to : standing_at), bytes, largest, to);
}

// The member `member` written from `pump` to `to`, the conflict settled by `how`: written beside
// and moved there whole, then given its time and rights. A member skipped still counts as done. A
// failure takes the half-written file away.
Task<void> written(const Member& member, Pump& pump, Path to, fs::Conflict how, Progress* progress, Undoing& undoing) {
    const std::optional<Path> where = co_await settled_on(to, how);

    if (!where) {
        if (progress != nullptr) progress->add(member.size);

        co_return;
    }

    const std::optional<fs::Entry> before = co_await standing(*where);
    const bool fresh = !before;
    const Bytes drawn = crypto::random(6);
    std::string mark;

    for (const std::byte one : drawn) mark += "0123456789abcdef"[std::to_integer<unsigned>(one) >> 4], mark += "0123456789abcdef"[std::to_integer<unsigned>(one) & 15];

    const Path beside = where->parent() / ("." + std::string{where->name()} + "." + mark + ".part");
    std::exception_ptr failed;

    try {
        fs::File file = co_await fs::open(beside, fs::Mode::exclusive);
        std::uint64_t at = 0;

        for (;;) {
            std::optional<Bytes> some = co_await pump.next();

            if (!some) break;

            const std::size_t much = some->size();

            co_await file.write(at, std::move(*some));
            at += much;

            if (progress != nullptr) progress->add(much);
        }

        pump.finished();
        co_await file.flush();
        file.close();
        co_await fs::move(beside, *where, how == fs::Conflict::replace ? fs::Conflict::replace : fs::Conflict::fail);
    } catch (...) {
        failed = std::current_exception();
    }

    if (failed) {
        try {
            co_await fs::remove(beside);
        } catch (const Trouble&) {
        }

        std::rethrow_exception(failed);
    }

    if (fresh) undoing.made(*where);

    fs::Changing changing;

    if (member.modified.time_since_epoch().count() != 0) changing.modified = member.modified;

#ifndef _WIN32
    changing.rights = member.rights;
#endif

    if (changing.modified || changing.rights) co_await fs::change(*where, changing);
}

// The extraction of `given` from `held` by `how`, counted in `progress`; the members left out as
// refused.
Task<std::vector<Member>> extracting(std::shared_ptr<Archive::Held> held, std::vector<Pick> given, fs::Conflict how, Progress* progress) {
    std::vector<Pick> picks;
    bool whole = true;

    for (const Pick& one : given) {
        const auto named = std::find_if(held->members.begin(), held->members.end(), [&one](const Member& each) { return each.name == one.inner; });
        const bool folder = one.inner.empty() || (named != held->members.end() && named->type == fs::Type::folder);

        const std::optional<fs::Entry> there = co_await standing(one.to);

        whole = whole && !there;

        if (!folder) {
            picks.push_back(one);

            continue;
        }

        std::optional<Path> to = co_await settled_on(one.to, how);

        if (to) picks.push_back(Pick{one.inner, std::move(*to)});
    }

    const Plan plan = planned(*held, picks);

    if (!plan.files.empty()) {
        std::uint64_t largest = 0;

        for (const Job& one : plan.files) largest = std::max(largest, held->members[one.member].size);

        co_await fitted(plan.files.front().to, plan.bytes, largest);
    }

    if (progress != nullptr) progress->expect(plan.bytes);

    Undoing undoing{whole};
    std::exception_ptr failed;

    try {
        for (const Making& one : plan.folders) {
            const std::optional<fs::Entry> there = co_await standing(one.to);

            if (there && there->type == fs::Type::folder) continue;

            if (there) throw Trouble{Kind::exists, "Something other than a folder stands where a folder goes.", one.to.text()};

            co_await fs::make(one.to);
            undoing.made(one.to);
        }

        const fs::File file = co_await fs::open(held->where, fs::Mode::read);
        const std::uint64_t size = co_await file.size();

        if (held->format == Format::zip) {
            for (const Job& one : plan.files) {
                const std::unique_ptr<Unzipping> pump = co_await Unzipping::started(*held, file, size, one.member);

                co_await written(held->members[one.member], *pump, one.to, how, progress, undoing);
            }
        } else {
            archiving::Source source{file, held->where, size, held->format == Format::tar_gzip};

            for (const Job& one : plan.files) {
                co_await source.skipped(held->spots[one.member].at - source.at());

                Untarring pump{source, held->members[one.member].size};

                co_await written(held->members[one.member], pump, one.to, how, progress, undoing);
            }
        }

        for (auto at = plan.folders.rbegin(); at != plan.folders.rend(); ++at) {
            if (!at->member) continue;

            const Member& one = held->members[*at->member];
            fs::Changing changing;

            if (one.modified.time_since_epoch().count() != 0) changing.modified = one.modified;

#ifndef _WIN32
            changing.rights = one.rights;
#endif

            if (changing.modified || changing.rights) co_await fs::change(at->to, changing);
        }
    } catch (...) {
        failed = std::current_exception();
    }

    if (failed) {
        co_await undoing.undone();
        std::rethrow_exception(failed);
    }

    co_return plan.refused;
}

}

// The archive `held` read.
Archive::Archive(std::shared_ptr<Held> held) : held_(std::move(held)) {}

// The archive's file.
const Path& Archive::where() const { return held_->where; }

// The archive's format.
Format Archive::format() const { return held_->format; }

// Every member, in the archive's order, followed by the folders only the names imply.
const std::vector<Member>& Archive::members() const { return held_->members; }

// The place among `members` of the member named `inner`, or nothing.
std::optional<std::size_t> Archive::found(const Path& inner) const {
    const auto at = std::find_if(held_->members.begin(), held_->members.end(), [&inner](const Member& one) { return one.name == inner; });

    if (at == held_->members.end()) return std::nullopt;

    return static_cast<std::size_t>(at - held_->members.begin());
}

// The places of the members right in `folder` (the empty path is the root), in the archive's order.
std::vector<std::size_t> Archive::within(const Path& folder) const {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < held_->members.size(); ++at)
        if (held_->members[at].name.parent() == folder) made.push_back(at);

    return made;
}

// The picks extracted, each member, or folder with all under it, or the root (an empty name), to
// where its pick says, with progress in bytes. A conflict is settled by `how` as a copy settles it:
// for a folder pick once for the folder (skipped, refused, renamed, or merged with what stands
// there when replacing), and then for each file in it. Gives the members left out as refused; a
// pick that names nothing (missing), a folder that cannot be made, damage and a file that cannot be
// written are troubles, and what this extraction made goes again; an empty archive is the
// caller's trouble (other).
Task<std::vector<Member>> Archive::extracted(std::vector<Pick> picks, fs::Conflict how, Progress* progress) const {
    if (held_ == nullptr) throw Trouble{Kind::other, "The archive is empty."};

    co_return co_await extracting(held_, std::move(picks), how, progress);
}

// The format of an archive by the end of `name`: .zip; .tar; .tar.gz and .tgz. Nothing for any
// other name, whatever the file holds.
std::optional<Format> format_of(const Path& name) {
    std::string lower{name.name()};

    std::transform(lower.begin(), lower.end(), lower.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

    const auto ends = [&lower](std::string_view tail) { return lower.size() > tail.size() && lower.ends_with(tail); };

    if (ends(".zip")) return Format::zip;

    if (ends(".tar.gz") || ends(".tgz")) return Format::tar_gzip;

    if (ends(".tar")) return Format::tar;

    return std::nullopt;
}

// The archive at `where` opened and its table of contents read; the format comes from the name
// (`format_of`). Throws `Trouble`: unsupported for a name of no archive, malformed for damage.
Task<Archive> opened(Path where) {
    const std::optional<Format> format = format_of(where);

    if (!format) throw Trouble{Kind::unsupported, "Not an archive tal reads.", where.text()};

    auto held = std::make_shared<Archive::Held>();

    held->where = where;
    held->format = *format;

    const fs::File file = co_await fs::open(where, fs::Mode::read);
    const std::uint64_t size = co_await file.size();

    if (*format == Format::zip) {
        co_await archiving::zip_read(*held, file, size);
    } else {
        archiving::Source source{file, where, size, *format == Format::tar_gzip};

        co_await archiving::tar_read(*held, source);
    }

    finished(*held);

    co_return Archive{held};
}

}

#include "talcite/fs.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include "talcite/bell.hpp"
#include "talcite/encoding.hpp"
#include "talcite/pool.hpp"
#include "talcite/random.hpp"

#include "fscore.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#endif

namespace talcite::fs {

using namespace core;

namespace {

// The longest name a folder takes: 255 bytes of UTF-8 on Linux and macOS, 255 UTF-16 units on
// Windows.
constexpr std::size_t name_most = 255;

// A name in its native form cut to at most `room` units, never inside a character: a UTF-8
// sequence or a UTF-16 surrogate pair is dropped whole when it does not fit.
path::string_type shortened(path::string_type name, std::size_t room) {
    if (name.size() <= room) return name;

    name.resize(room);

#ifdef _WIN32
    if (!name.empty() && name.back() >= 0xD800 && name.back() <= 0xDBFF) name.pop_back();
#else
    // A character the cut left unfinished, by the one rule (`utf8::span`), goes whole; the bytes of
    // a name that is no UTF-8 at all stay as they are.
    std::size_t lead = name.size();

    while (lead > 0 && name.size() - lead < 3 && (static_cast<unsigned char>(name[lead - 1]) & 0xC0U) == 0x80U) --lead;

    if (lead > 0 && utf8::span(std::string_view{name}.substr(lead - 1)).kind == utf8::Span::Kind::partial) name.resize(lead - 1);
#endif

    return name;
}

}

// A file beside the target for the time of a replacement: a dot, the target's name, six random
// characters and `.part`. Media servers do not take `.part` for a film, and the random tail does
// not hit someone else's file. A target named near the longest name a folder takes lends the end
// of its name to the rest, so the file can still be made.
path core::aside_of(const path& to) {
    static constexpr std::string_view digits = "0123456789abcdef";

    std::string tail;

    for (const std::byte one : random(3)) {
        const auto value = static_cast<unsigned>(one);

        tail += digits[value >> 4U];
        tail += digits[value & 0x0fU];
    }

    path made = to;

    const path::string_type after = path{"." + tail + ".part"}.native();

    made.replace_filename(path{"."}.native() + shortened(to.filename().native(), name_most - 1 - after.size()) + after);

    return made;
}

namespace {

// The counter of an operation done in installments: how many bytes or entries out of how many are
// done. Every change goes to the promise of the operation (`Work::progress`), so the progress bar
// asks the promise and nobody has to pass anything upward. A counter without work (a part counted
// apart) only counts.
struct Counted {
    explicit Counted(const Work* given = nullptr) : work(given) {}

    // Sets how much there is in all.
    void total(std::uintmax_t much) {
        whole.store(much);

        told();
    }

    // Sets how much is done.
    void reached(std::uintmax_t much) {
        done.store(much);

        told();
    }

    // Adds to what is done.
    void added(std::uintmax_t much) {
        done.fetch_add(much);

        told();
    }

    // Reports the state to the promise.
    void told() const {
        if (work != nullptr) work->progress(Progress{done.load(), whole.load()});
    }

    std::atomic<std::uintmax_t> done{0};
    std::atomic<std::uintmax_t> whole{0};
    const Work* work = nullptr;
};

// Where the copy of one file reports its bytes. A file copied on its own owns the operation's
// counter and sets its whole; a file inside a folder or a move adds its bytes to the counter from
// where it started (`base`), so that the bar of a folder moves with every byte, not only after
// every entry.
struct Tally {
    Counted& held;
    std::uintmax_t base = 0;
    bool owning = true;
    std::size_t rata = 0;
};

// The installment size for a copy: the smaller of the two disks' preferred chunks. A tree asks once
// and hands the answer to every file (`Tally::rata`); each question is a `statfs`, and on FUSE and
// GVfs a reading of /proc/mounts.
std::size_t rated(const path& from, const path& to) {
    return std::min(chunk_at(from), chunk_at(to));
}

// Following links can lead back to where we have already been: a link pointing to an ancestor
// would loop the copy forever. So the real paths of the folders already followed are kept.
using Seen = std::vector<path>;

// True when `real` was already followed; otherwise records it and returns false.
bool sighted(Seen& seen, const path& real) {
    if (std::find(seen.begin(), seen.end(), real) != seen.end()) return true;

    seen.push_back(real);

    return false;
}

// Whether, for this conflict rule, the question "is it perhaps the same file?" makes sense. `skip`
// may simply do nothing, and `rename` makes a copy beside it, which is the only sensible way to
// copy a file onto itself. That leaves `overwrite`, which would delete the source, and `fail`,
// which deserves a sentence that says what the matter is.
bool asked(Conflict how) { return how == Conflict::overwrite || how == Conflict::fail; }


// A copy of a file interrupted halfway must not leave behind a file that looks complete. The target
// disappears unless the copy reached the end, and only when the copy created it: a flag the copy
// raises once its exclusive creation succeeded (`made`). A file that someone else put under the name
// in the meantime makes that creation fail, the flag stays down, and their file is not removed.
class Sweeping {
public:
    // Watches the target, which goes only when `made` says the copy created it.
    Sweeping(path where, const bool* made) : where_(std::move(where)), made_(made) {}

    // Removes the target, unless `keep` was called or it is not the copy's own.
    ~Sweeping() {
        if (kept_ || !*made_) return;

        std::error_code ignored;
        stood::remove(long_form(where_), ignored);
    }

    Sweeping(const Sweeping&) = delete;
    Sweeping& operator=(const Sweeping&) = delete;
    Sweeping(Sweeping&&) = delete;
    Sweeping& operator=(Sweeping&&) = delete;

    // Marks the target as complete, so that it stays.
    void keep() { kept_ = true; }

private:
    path where_;
    const bool* made_ = nullptr;
    bool kept_ = false;
};

// What a folder copy has made, taken back when the copy does not finish: the files it wrote and the
// folders it created, newest first, a folder only when it is empty by then. Only what the copy made
// goes: a file someone else put into one of its new folders meanwhile stays, and so does the folder
// that holds it. A copy into a folder that stood before (`owning` false) takes nothing back, since
// what arrived is merged with what was there.
class Undoing {
public:
    // Starts with nothing made.
    explicit Undoing(bool owning) : owning_(owning) {}

    // Takes back what was made, unless `keep` was called or the target folder stood before.
    ~Undoing() {
        if (kept_ || !owning_) return;

        for (auto at = made_.rbegin(); at != made_.rend(); ++at) {
            std::error_code ignored;
            stood::remove(long_form(*at), ignored);
        }
    }

    Undoing(const Undoing&) = delete;
    Undoing& operator=(const Undoing&) = delete;
    Undoing(Undoing&&) = delete;
    Undoing& operator=(Undoing&&) = delete;

    // Notes a file written or a folder created by the copy.
    void made(path where) { made_.push_back(std::move(where)); }

    // Marks the copy as complete, so that everything stays.
    void keep() { kept_ = true; }

private:
    std::vector<path> made_;
    bool owning_ = false;
    bool kept_ = false;
};

// A file that stood under a name in a folder the copy created itself, where nothing should stand:
// two source names that the target takes for one (A.txt and a.txt on a disk that ignores case,
// two spellings of one accented letter), or a file someone else put there meanwhile. Thrown from
// the exclusive creation, and caught by `placed_at`, which then settles the conflict as anywhere
// else.
struct Taken {};

// Three kinds of writing to a file: a new one is created exclusively (`Mode::exclusive`) and written
// directly, so a file that someone else put under the name in the meantime is refused, not
// overwritten; a replacement writes beside it and renames, so an interrupted or failed one does not
// touch the old target, and a second hard link to the old file keeps the old content; and `durable`
// additionally waits until the data is on the medium before it says "done". A replacement is
// always made durable: without that, a power loss right after the rename could leave an empty
// file in place of the target. In a folder the copy has just created (`fresh`) nothing can stand
// under the name, so the target is not asked about.
//
// `fill` gets the path where the content is to appear and a flag to raise once it has created the
// file there, and returns an open handle to it, or nothing when the task was cancelled halfway;
// then the promise is not settled at all. The modification time, when there is one, is written
// through the same handle before syncing.
template <typename Fill>
bool poured_at(const path& to, bool durable, std::optional<Instant> when, bool fresh, Fill fill) {
    const bool there = !fresh && kind_at(to) != Kind::none;
    const path into = there ? aside_of(to) : to;

    bool made = false;

    Sweeping sweeping{into, &made};

    {
        const std::shared_ptr<File::Held> yours = fill(into, made);

        if (!yours) return false;

        if (when) stamp_on(*yours, *when);

        if (there || durable) sync_at(*yours);
    }

    if (there && !replaced_at(into, to)) amiss("Cannot replace the file.", to);

    sweeping.keep();

    return true;
}

// Writing in installments through our own buffer, when the content is already at hand or when the
// system cannot copy on its own.
template <typename Pour>
bool poured_at(const path& to, bool durable, Pour pour) {
    const auto fill = [&](const path& into, bool& made) -> std::shared_ptr<File::Held> {
        std::shared_ptr<File::Held> yours = open_at(into, Mode::exclusive);

        made = true;

        if (!pour(*yours)) return nullptr;

        return yours;
    };

    return poured_at(to, durable, std::nullopt, false, fill);
}

// The piece for an in-kernel copy: larger than a read installment, because with a server-side
// copy every piece is one question over the network, and a small piece would eat up the whole
// benefit. A cancellation waits for at most one piece.
constexpr std::size_t spliced_piece = 16 * 1024 * 1024;

#ifdef _WIN32

// `CopyFileExW` copies on its own: server-side on an SMB share, with ODX offload on a storage
// array, with attributes and streams. Progress, cancellation and a pause go through the callback,
// on the same thread, so the worker's bell is heard as in our own loop: a paused copy rests in the
// callback, between the system's pieces. When cancelled, the system removes the target itself.
DWORD CALLBACK copied(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                      LPVOID given) {
    const Tally& tally = *static_cast<const Tally*>(given);

    tally.held.reached(tally.base + static_cast<std::uintmax_t>(done.QuadPart));

    return rested() ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
}

#endif

// A copy of a file that found its source changed while it went: thrown from inside the copy, so the
// half-made target goes with the unwinding, and caught by `file_at` to start again.
struct Shifting {};

// Creates the target of a copy exclusively. In a folder the copy made itself (`fresh`) a name that
// is taken all the same is `Taken`, a conflict still to settle; anywhere else the refusal stands.
std::shared_ptr<File::Held> created_at(const path& into, bool fresh) {
    try {
        return open_at(into, Mode::exclusive);
    } catch (const Trouble&) {
        if (fresh && kind_at(into) != Kind::none) throw Taken{};

        throw;
    }
}

#ifdef _WIN32

// Whether `CopyFileExW` refused only because it does not know this way (Wine, a medium without
// it), so that our own loop may take over. Any other refusal (a broken connection, a full disk, no
// rights) is the answer.
bool unsupported(DWORD why) {
    return why == ERROR_NOT_SUPPORTED || why == ERROR_INVALID_FUNCTION || why == ERROR_CALL_NOT_IMPLEMENTED ||
           why == ERROR_INVALID_PARAMETER;
}

#endif

// One attempt at copying a file to `to`: first the system's own copy (Windows), then a clone, then
// an in-kernel copy in pieces, and finally our own read and write loop. The bytes go to `tally`.
// Before the target is handed over, the copy checks itself: exactly as many bytes as the source had
// when it was opened, the open source still of that length and write time, and the same file still
// under the source's name. Anything else means someone changed the source meanwhile (a file still
// being written, one cut shorter, one replaced), and the attempt throws `Shifting`. In a folder the
// copy has just created (`fresh`) the target is not asked about. False when cancelled halfway.
bool copied_at(const path& from, const path& to, Tally tally, bool durable, bool fresh) {
    const std::shared_ptr<File::Held> mine = open_at(from, Mode::read);

    const Facet before = facet_of(*mine);
    const std::uintmax_t whole = before.size;

    if (tally.owning) tally.held.total(whole);

    // How much of this file is done, as the counter sees it.
    const auto at_byte = [&tally](std::uintmax_t at) { tally.held.reached(tally.base + at); };

    // Whether, after `much` bytes, the source is still what it was when the copy began.
    const auto unchanged = [&](std::uintmax_t much) {
        if (much != whole || facet_of(*mine) != before) return false;

        const std::optional<Facet> there = facet_at(from);

        return there && there->device == before.device && there->node == before.node;
    };

    const std::size_t rata = tally.rata > 0 ? tally.rata : rated(from, to);

    return poured_at(to, durable, before.changed, fresh, [&](const path& into, bool& made) -> std::shared_ptr<File::Held> {
#ifdef _WIN32
        BOOL cancel = FALSE;

        if (CopyFileExW(longly(from).c_str(), longly(into).c_str(), copied, &tally, &cancel,
                        COPY_FILE_FAIL_IF_EXISTS) != 0) {
            made = true;

            // The read-only attribute came from the source; left in place, it would block syncing
            // here and every later overwrite of the target. Our own loop does not carry it over
            // either.
            const DWORD told = GetFileAttributesW(longly(into).c_str());

            if (told != INVALID_FILE_ATTRIBUTES && (told & FILE_ATTRIBUTE_READONLY) != 0)
                SetFileAttributesW(longly(into).c_str(), told & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

            at_byte(whole);

            std::shared_ptr<File::Held> yours = open_at(into, Mode::write);

            if (!unchanged(sized(*yours))) throw Shifting{};

            return yours;
        }

        const DWORD why = GetLastError();
        const bool foreign = why == ERROR_FILE_EXISTS || why == ERROR_ALREADY_EXISTS;

        // Unless the name was taken before it began, whatever the system's copy left under it is its
        // own, and so the copy's.
        if (!foreign) made = true;

        if (why == ERROR_REQUEST_ABORTED || stopped()) return nullptr;

        if (foreign && fresh) throw Taken{};

        if (!unsupported(why)) amiss("Cannot copy the file: " + std::system_category().message(static_cast<int>(why)), from);

        // A system that does not know the way may still have left an empty target behind; it goes,
        // and our own loop creates the file anew.
        {
            std::error_code ignored;
            stood::remove(long_form(into), ignored);
        }

        made = false;
#endif
        std::shared_ptr<File::Held> yours = created_at(into, fresh);

        made = true;

        if (whole > 0 && cloned_on(*mine, *yours)) {
            at_byte(whole);

            if (!unchanged(whole)) throw Shifting{};

            return yours;
        }

        bool ranged = true;

        std::uintmax_t at = 0;

        Bytes buffer;

        while (at < whole) {
            if (!rested()) return nullptr;

            if (ranged) {
                const std::optional<std::size_t> went =
                    spliced_on(*mine, *yours, at, static_cast<std::size_t>(std::min<std::uintmax_t>(
                                                      whole - at, std::max(rata, spliced_piece))));

                // Zero before the end is not the end of the file but a system that did not keep
                // to this path (generated files, some FUSE file systems); the rest goes our way.
                if (went && *went > 0) {
                    at += *went;
                    at_byte(at);

                    continue;
                }

                ranged = false;
            }

            // One buffer for the whole file, allocated on the first installment that needs it.
            buffer.resize(rata);

            const std::size_t got =
                read_into(*mine, at, std::span{buffer}.first(static_cast<std::size_t>(std::min<std::uintmax_t>(rata, whole - at))));
            if (got == 0) break;

            at += write_from(*yours, at, std::span{buffer}.first(got));
            at_byte(at);
        }

        if (!unchanged(at)) throw Shifting{};

        return yours;
    });
}

// Copies one file to `to` (see `copied_at`). A source that changed while it went is copied once
// more from the start; a second change in a row is a refusal, and the source stays as it is. False
// when cancelled halfway.
bool file_at(const path& from, const path& to, Tally tally, bool durable = false, bool fresh = false) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            return copied_at(from, to, tally, durable, fresh);
        } catch (const Shifting&) {
        }
    }

    amiss("The file changed while it was being copied.", from);
}

// Copies one file into a folder entry, settling a conflict by `how`. In a folder the copy made
// itself (`fresh`) the target is not asked about first, since nothing should stand there; only when
// the exclusive creation finds the name taken all the same (`Taken`) is the conflict settled, as
// anywhere else. Returns where the file went, empty when it was skipped, and nothing when cancelled.
std::optional<path> placed_at(const path& from, const path& to, Tally tally, Conflict how, bool durable, bool fresh) {
    if (fresh) {
        try {
            if (!file_at(from, to, tally, durable, true)) return std::nullopt;

            return to;
        } catch (const Taken&) {
        }
    }

    const path where = settled_on(to, how);

    if (where.empty()) return path{};

    if (!file_at(from, where, tally, durable, false)) return std::nullopt;

    return where;
}

// The folder's time with full precision, or the one from the listing when the folder has vanished
// in the meantime.
Instant instant_or(const path& where, std::chrono::sys_seconds otherwise) {
    try {
        return instant_at(where);
    } catch (const Trouble&) {
        return Instant{otherwise};
    }
}

// A check before copying. Without it a 40 GB copy would fail at 4 GiB on FAT32, or at 95% on a
// full disk, after a quarter of an hour of work and with half a file to clean up. So the question
// is asked earlier: about the largest file when the target is FAT (exFAT has no such limit), and
// about the total when the target medium says how much room it has. A medium that does not answer
// does not block the copy: the check is there to save time, not to guess.
constexpr std::uintmax_t fat_limit = 0xFFFFFFFFull;

// Throws `Trouble` when the largest file does not fit on a FAT target or the total does not fit in
// the free space.
void fits_at(const path& to, std::uintmax_t whole, std::uintmax_t largest) {
    if (largest > fat_limit && fat_at(to)) amiss("The file is too large for this disk.", to);

    if (const std::optional<std::uintmax_t> room = room_at(to); room && *room < whole)
        amiss("Not enough free space.", to);
}

// One entry of a tree to copy, worked out before the first byte: where it comes from (for a followed
// link, what it points to), where it goes, what it is, and whether its bytes count in the total
// (what lies behind a followed link does not).
struct Laid {
    path from;
    path to;
    Kind kind = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds modified{};
    bool counted = true;
};

// The tree under `from` as it will be copied to `to`, parents before children, every folder read in
// full. A folder that cannot be read, an entry that is neither a file nor a folder (a pipe, a
// socket, a device) and a link that leads nowhere refuse the whole copy, with the place, before
// anything is written: a copy that skipped them would end in success without a part of the tree,
// and opening a pipe for reading would hang a pool thread beyond rescue. A link is followed: to a
// file, the file is copied; to a folder, its tree, at most 64 links deep; a link back to a folder
// already being copied is left out, since following it would never end. False when cancelled while
// reading.
bool charted(const path& from, const path& to, std::vector<Laid>& into, Seen& seen, int deep, bool counted) {
    const std::vector<Entry> among = list_whole_at(from);

    if (stopped()) return false;

    for (const Entry& one : among) {
        const path where = to / one.where.filename();

        if (one.kind == Kind::folder) {
            into.push_back(Laid{one.where, where, Kind::folder, 0, one.modified, counted});

            if (!charted(one.where, where, into, seen, deep, counted)) return false;

            continue;
        }

        if (one.kind == Kind::file) {
            into.push_back(Laid{one.where, where, Kind::file, one.size, one.modified, counted});

            continue;
        }

        if (one.kind != Kind::link) amiss("This entry cannot be copied.", one.where);

        std::error_code trouble;

        const path onto = real_at(one.where, trouble);

        if (trouble) amiss("This entry cannot be copied.", one.where);

        const Kind kind = kind_at(onto);

        if (kind == Kind::file) {
            into.push_back(Laid{onto, where, Kind::file, 0, one.modified, false});

            continue;
        }

        if (kind != Kind::folder) amiss("This entry cannot be copied.", one.where);

        if (sighted(seen, onto)) continue;

        if (deep >= 64) amiss("This entry cannot be copied.", one.where);

        into.push_back(Laid{onto, where, Kind::folder, 0, one.modified, false});

        if (!charted(onto, where, into, seen, deep + 1, false)) return false;
    }

    return true;
}

// A folder goes entry by entry, from one reading of the whole tree (`charted`) that also gives the
// total; with `fitting` it is checked against the target's room first. Progress counts the bytes of
// its files plus one for every entry, so a big file moves the bar while it is copied and a folder
// of empty entries still moves it. Parents come before their children, so folders are created in
// order. The content of followed links has its own counter and is not added to this one, because
// it is not in the total. A folder the copy creates itself is empty, so what goes into it asks no
// question about a conflict (`fresh`); only a folder that stood before (a merge) asks, and so does
// every folder under it that stood too. False when cancelled.
bool tree_at(const path& from, const path& to, Counted& held, Conflict how, Seen& seen, int deep, bool fitting) {
    std::vector<Laid> laid;

    if (!charted(from, to, laid, seen, deep, true)) return false;

    std::uintmax_t bytes = 0;
    std::uintmax_t largest = 0;

    for (const Laid& one : laid) {
        if (one.kind != Kind::file || !one.counted) continue;

        bytes += one.size;
        largest = std::max(largest, one.size);
    }

    if (fitting) fits_at(to, bytes, largest);

    held.total(bytes + laid.size() + 1);

    const bool standing = kind_at(to) != Kind::none;

    Undoing undoing{!standing};

    make_folder_at(to);
    held.added(1);

    if (!standing) undoing.made(to);

    const std::size_t rata = rated(from, to);

    std::vector<std::pair<path, Instant>> folders;

    // Which target folders the copy created, by path; a child's path is its parent's joined with a
    // name, so the parent is found by `parent_path`.
    std::map<path, bool> fresh{{to, !standing}};

    const auto anew = [&fresh](const path& where) {
        const auto found = fresh.find(where.parent_path());

        return found != fresh.end() && found->second;
    };

    for (const Laid& one : laid) {
        if (!rested()) return false;

        const std::uintmax_t base = held.done.load();
        const bool inside = anew(one.to);

        if (one.kind == Kind::folder) {
            const bool there = !inside && kind_at(one.to) != Kind::none;

            make_folder_at(one.to);
            fresh[one.to] = !there;
            folders.emplace_back(one.to, instant_or(one.from, one.modified));

            if (!there) undoing.made(one.to);
        } else {
            Counted apart;

            const Tally tally = one.counted ? Tally{held, base, false, rata} : Tally{apart, 0, true, rata};
            const std::optional<path> went = placed_at(one.from, one.to, tally, how, false, inside);

            if (!went) return false;

            if (!went->empty()) undoing.made(*went);
        }

        // The listed size, not the copied one: the total was made from the listing.
        if (one.kind == Kind::file && one.counted) held.reached(base + one.size);

        held.added(1);
    }

    undoing.keep();

    // The folders' times only at the end and from the deepest: every entry put inside changes the
    // time, so stamping earlier would leave the copy's subfolders with the time of copying instead
    // of the source's time.
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) stamp_at(at->first, at->second);

    stamp_at(to, instant_at(from));

    return true;
}

// How much a move takes: the files only.
void measured(const std::vector<Entry>& among, std::uintmax_t& whole, std::uintmax_t& largest) {
    for (const Entry& one : among) {
        if (one.kind != Kind::file) continue;

        whole += one.size;
        largest = std::max(largest, one.size);
    }
}

// Copies a file or a folder (following a link given as `from`) to `to`, after checking that it
// fits. Throws `Trouble` for nothing to copy, a dangling link, an entry that is neither file nor
// folder, or a copy onto itself. False when cancelled.
bool copy_at(const path& from, const path& to, Counted& held, Conflict how) {
    Kind what = kind_at(from);

    if (what == Kind::none) amiss("Nothing to copy.", from);

    path onto = from;

    if (what == Kind::link) {
        std::error_code trouble;

        onto = real_at(from, trouble);
        if (trouble) amiss("The link leads nowhere.", from);

        what = kind_at(onto);
    }

    Seen seen;

    if (what == Kind::folder) {
        // Nothing onto itself. A folder copied onto its own path would walk into what it is just
        // creating; MC refuses, and so does this.
        if (asked(how) && kind_at(to) != Kind::none && equivalent_at(onto, to))
            amiss("Cannot copy a directory onto itself.", from);

        sighted(seen, canonical_at(onto));

        return tree_at(onto, to, held, how, seen, 0, true);
    }

    if (what != Kind::file) amiss("This entry cannot be copied.", from);

    // A file is not copied onto itself. With `Conflict::overwrite` the target would be opened for
    // writing and truncated to zero, deleting what was to be copied. The question is asked before
    // `settled_on`, so that the sentence says what the matter really is and not "the target
    // already exists"; and only when the target exists, because an equivalence check has nothing
    // to compare without the other side.
    if (asked(how) && kind_at(to) != Kind::none && equivalent_at(onto, to))
        amiss("Cannot copy a file onto itself.", from);

    const path where = settled_on(to, how);
    if (where.empty()) return true;

    {
        std::error_code ignored;

        const std::uintmax_t much = stood::file_size(long_form(onto), ignored);

        if (!ignored) fits_at(where, much, much);
    }

    return file_at(onto, where, Tally{held});
}

// Moving entry by entry, between media or when merging a folder with an existing one. Every file
// first tries a plain rename (instant within one medium); when the system answers "another
// medium", the file is copied, made durable on the target medium and only then disappears from
// the source. The source folders go last, and only when empty.
//
// Copying everything first and removing the source afterward would be wrong: a failure of that
// removal would get lost, and everything the copy did not see (a folder without access, a listing
// cut off by a broken connection, a skipped entry) would go to waste together with the rest.
//
// The obstacles are checked before the first move: an entry that cannot be moved (a pipe, a
// device, a dangling link) and a folder without access refuse the whole move.
std::vector<Entry> surveyed(const path& from) {
    std::vector<Entry> made;
    std::vector<path> waiting{from};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        for (Entry& one : list_whole_at(here)) {
            if (one.kind == Kind::folder) waiting.push_back(one.where);

            const bool stale = one.kind == Kind::link && one.reach == Kind::none;

            if (stale || one.kind == Kind::device || one.kind == Kind::special || one.kind == Kind::none)
                amiss("This entry cannot be moved.", one.where);

            made.push_back(std::move(one));
        }
    }

    std::sort(made.begin(), made.end(), [](const Entry& one, const Entry& two) { return one.where < two.where; });

    return made;
}

// One file or link; the bytes of a plain file go to `tally`. In a folder the move has just created
// (`fresh`) nothing stands under the name, so the copy to another medium does not ask about it.
// False when cancelled during the copy; the source then stays whole.
bool carried(const path& from, const path& to, Conflict how, Tally tally, bool fresh) {
    const Shift made = shifted(from, to, how);

    if (made.where.empty() || made.done) return true;

    // Another medium: the system refused before it looked at the target, so the conflict is
    // settled once more (`placed_at`), unless the folder is the move's own.
    Counted apart;
    Seen seen;

    path where;

    if (kind_at(from) == Kind::link) {
        std::error_code trouble;

        const path onto = real_at(from, trouble);
        if (trouble) amiss("The link leads nowhere.", from);

        if (kind_at(onto) == Kind::folder) {
            where = fresh ? to : settled_on(to, how);
            if (where.empty()) return true;

            sighted(seen, onto);

            if (!tree_at(onto, where, apart, how, seen, 1, false)) return false;
        } else {
            const std::optional<path> went = placed_at(onto, to, Tally{apart}, how, true, fresh);

            if (!went) return false;
            if (went->empty()) return true;

            where = *went;
        }
    } else {
        const std::optional<path> went = placed_at(from, to, tally, how, true, fresh);

        if (!went) return false;
        if (went->empty()) return true;

        where = *went;
    }

    // The copy is on the medium; its entry in the folder must be too, before the source goes, or a
    // power loss right after the removal could leave neither (POSIX; NTFS keeps the entry with the
    // file).
    settled_folder(where.parent_path());

    std::error_code trouble;

    stood::remove(long_form(from), trouble);
    if (trouble) amiss("Cannot remove the source after copying.", from);

    return true;
}

// Moves `from` to `to` entry by entry, for a move across media or a merge of folders. Throws
// `Trouble` when something cannot be moved or entries stayed behind; false when cancelled.
bool relocated(const path& from, const path& to, Counted& held, Conflict how) {
    // A single file comes here only from another medium; within one, a rename was enough. Its
    // progress is its bytes; a link counts as one step.
    if (kind_at(from) != Kind::folder) {
        std::uintmax_t much = 0;

        if (kind_at(from) == Kind::file) {
            std::error_code ignored;

            much = stood::file_size(long_form(from), ignored);

            if (ignored) much = 0;
            else fits_at(to, much, much);
        }

        held.total(much > 0 ? much : 1);

        if (!carried(from, to, how, Tally{held, 0, false}, false)) return false;

        held.reached(held.whole.load());

        return true;
    }

    const std::vector<Entry> among = surveyed(from);

    std::uintmax_t bytes = 0;
    std::uintmax_t largest = 0;

    measured(among, bytes, largest);

    if (!together_at(from, to)) fits_at(to, bytes, largest);

    // As in a folder copy: the bytes of the files plus one for every entry.
    held.total(bytes + among.size() + 1);

    const Instant stamp = instant_at(from);
    const bool standing = kind_at(to) != Kind::none;

    make_folder_at(to);
    held.added(1);

    // The installment of the two media, asked once for the tree, as a folder copy does: per file it
    // would be two `statfs` and, on FUSE and GVfs, two readings of the mount table.
    const std::size_t rata = rated(from, to);

    std::vector<std::pair<path, Instant>> folders;

    // Which target folders the move created, as in a folder copy (`tree_at`): the entries come
    // sorted by path, so a folder comes before what lies in it.
    std::map<path, bool> fresh{{to, !standing}};

    for (const Entry& one : among) {
        if (!rested()) return false;

        const path where = to / one.where.lexically_relative(from);

        const std::uintmax_t base = held.done.load();

        const auto parent = fresh.find(where.parent_path());
        const bool inside = parent != fresh.end() && parent->second;

        if (one.kind == Kind::folder) {
            const bool there = !inside && kind_at(where) != Kind::none;

            make_folder_at(where);
            fresh[where] = !there;
            folders.emplace_back(one.where, instant_or(one.where, one.modified));
        } else if (!carried(one.where, where, how, Tally{held, base, false, rata}, inside)) {
            return false;
        }

        // A rename within one medium reports nothing, and the copied size may differ from the
        // listed one; the listed size keeps the count in step with the total.
        if (one.kind == Kind::file) held.reached(base + one.size);

        held.added(1);
    }

    // From the deepest: a folder goes only when empty, and its modification time passes to the
    // target only now, because every entry put inside changes it.
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) {
        std::error_code ignored;

        stamp_at(to / at->first.lexically_relative(from), at->second);
        stood::remove(long_form(at->first), ignored);
    }

    std::error_code ignored;

    stood::remove(long_form(from), ignored);
    stamp_at(to, stamp);

    if (how != Conflict::skip && exists_at(from)) amiss("Some entries stayed behind.", from);

    return true;
}

}

// Reads the whole file in installments, reporting progress; a cancelled read stops between them.
Promise<Bytes> read(const path& where) {
    return run(io(), [where](Work& work) -> Bytes {
        Counted held{&work};

        const std::shared_ptr<File::Held> one = open_at(where, Mode::read);

        const std::uintmax_t whole = sized(*one);
        held.total(whole);

        const std::size_t rata = chunk_at(where);

        // The bytes land in the result directly, in installments, so the file is not copied once
        // more in memory.
        Bytes made(static_cast<std::size_t>(whole));

        std::size_t at = 0;

        while (at < made.size()) {
            if (stopped()) throw Cancelled{};

            const std::size_t got = read_into(*one, at, std::span{made}.subspan(at, std::min(rata, made.size() - at)));
            if (got == 0) break;

            at += got;
            held.reached(at);
        }

        made.resize(at);

        return made;
    });
}

// Writing over an existing file goes beside it and renames onto the target (`poured_at`), so an
// interrupted write leaves no stub under the real name.
Promise<void> write(const path& where, Bytes what, Conflict how) {
    return run(io(), [where, what = std::move(what), how](Work& work) {
        Counted held{&work};

        const path at_all = settled_on(where, how);

        if (at_all.empty()) return;

        held.total(what.size());

        const std::size_t rata = chunk_at(at_all);

        const bool whole = poured_at(at_all, false, [&](const File::Held& one) {
            for (std::size_t at = 0; at < what.size();) {
                if (stopped()) return false;

                const std::size_t much = std::min<std::size_t>(rata, what.size() - at);

                at += write_from(one, at, std::span{what}.subspan(at, much));
                held.reached(at);
            }

            return true;
        });

        if (!whole) throw Cancelled{};
    });
}

// Copies a file or a folder in a pool task, with progress in bytes (for a folder, its files' bytes
// plus one per entry); `how` settles a conflict with an existing target.
Promise<void> copy(const path& from, const path& to, Conflict how) {
    return run(io(), [from, to, how](Work& work) {
        Counted held{&work};

        if (!copy_at(from, to, held, how)) throw Cancelled{};
    });
}

// First a plain rename, because within one medium it is instant. Only when the system answers
// "another medium", and only then, the entries are moved one by one (`relocated`). Every other
// refusal is an error: a file open in a player must not end up as a 40 GB copy, present twice.
// A folder overwritten with a folder merges with it, also entry by entry.
Promise<void> move(const path& from, const path& to, Conflict how) {
    return run(io(), [from, to, how](Work& work) {
        Counted held{&work};

        if (kind_at(from) == Kind::none) amiss("Nothing to move.", from);

        if (how == Conflict::overwrite && kind_at(from) == Kind::folder && kind_at(to) == Kind::folder &&
            !equivalent_at(from, to)) {
            if (!relocated(from, to, held, how)) throw Cancelled{};

            return;
        }

        const Shift made = shifted(from, to, how);

        // Another medium: the system did not even have to look at the target. So the conflict is
        // settled once more before anything is moved.
        const path where = made.where.empty() || made.done ? path{} : settled_on(to, how);

        if (where.empty()) {
            held.total(1);
            held.reached(1);

            return;
        }

        if (!relocated(from, where, held, how)) throw Cancelled{};
    });
}

// Removes a file or a whole folder; a missing entry counts as already removed.
Promise<void> erase(const path& where) {
    return run(io(), [where](Work& work) {
        Counted held{&work};

        if (kind_at(where) == Kind::none) return;

        std::vector<Entry> among;

        if (kind_at(where) == Kind::folder) among = walk_at(where);

        held.total(among.size() + 1);

        // From the deepest, because a folder can be removed only when empty.
        for (auto at = among.rbegin(); at != among.rend(); ++at) {
            if (!rested()) throw Cancelled{};

            std::error_code ignored;
            stood::remove(long_form(at->where), ignored);

            held.added(1);
        }

        std::error_code trouble;
        stood::remove(long_form(where), trouble);

        if (trouble) amiss("Cannot remove.", where);

        held.added(1);
    });
}

}

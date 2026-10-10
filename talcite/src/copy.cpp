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
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include "talcite/bell.hpp"
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

// The ticket of an operation done in installments: it knows how many bytes or entries out of how
// many are done. The virtual progress of `Ticket` is here so that the progress bar can ask, and
// nobody has to pass anything upward.
template <typename T>
struct Counted : detail::Result<T> {
    std::atomic<std::uintmax_t> done{0};
    std::atomic<std::uintmax_t> whole{0};

    // The finished share between 0 and 1; a settled operation counts as complete.
    [[nodiscard]] double progress() const override {
        if (over(*this)) return 1.0;

        const std::uintmax_t all = whole.load();
        if (all == 0) return 0.0;

        return std::min(1.0, static_cast<double>(done.load()) / static_cast<double>(all));
    }
};

// Where the copy of one file reports its bytes. A file copied on its own owns the operation's
// counter and sets its whole; a file inside a folder or a move adds its bytes to the counter from
// where it started (`base`), so that the bar of a folder moves with every byte, not only after
// every entry.
struct Tally {
    Counted<void>& held;
    std::uintmax_t base = 0;
    bool owning = true;
};

template <typename T>
class HaulPromise : public Promise<T> {
public:
    HaulPromise(typename Promise<T>::Callback given, std::shared_ptr<Counted<T>> made)
        : Promise<T>(std::move(given), std::move(made)) {}
};

// The installment size for a copy: the smaller of the two disks' preferred chunks.
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


// A copy interrupted halfway must not leave behind a file that looks complete. The target
// disappears unless the copy reached the end.
class Sweeping {
public:
    // Remembers whether the target existed before; an existing one is never removed.
    explicit Sweeping(path where) : where_(std::move(where)), was_(exists_at(where_)) {}

    // Removes the target, unless `keep` was called or it existed before.
    ~Sweeping() {
        if (kept_ || was_) return;

        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Sweeping(const Sweeping&) = delete;
    Sweeping& operator=(const Sweeping&) = delete;
    Sweeping(Sweeping&&) = delete;
    Sweeping& operator=(Sweeping&&) = delete;

    // Marks the target as complete, so that it stays.
    void keep() { kept_ = true; }

private:
    path where_;
    bool was_ = false;
    bool kept_ = false;
};

// A file beside the target for the time of a replacement: a dot, the target's name, six random
// characters and `.part`. Media servers do not take `.part` for a film, and the random tail does
// not hit someone else's file.
path aside_of(const path& to) {
    static constexpr std::string_view digits = "0123456789abcdef";

    std::string tail;

    for (const std::byte one : random(3)) {
        const auto value = static_cast<unsigned>(one);

        tail += digits[value >> 4U];
        tail += digits[value & 0x0fU];
    }

    path made = to;

    made.replace_filename(path{"."}.native() + to.filename().native() + path{"." + tail + ".part"}.native());

    return made;
}

// Three kinds of writing to a file: `Mode::truncate` writes directly (the target does not exist);
// a replacement writes beside it and renames, so an interrupted or failed one does not touch the
// old target, and a second hard link to the old file keeps the old content; and `durable`
// additionally waits until the data is on the medium before it says "done". A replacement is
// always made durable: without that, a power loss right after the rename could leave an empty
// file in place of the target.
//
// `fill` gets the path where the content is to appear and returns an open handle to it, or
// nothing when the task was cancelled halfway; then the promise is not settled at all. The
// modification time, when there is one, is written through the same handle before syncing.
template <typename Fill>
bool poured_at(const path& to, bool durable, std::optional<Instant> when, Fill fill) {
    const bool there = kind_at(to) != Kind::none;
    const path into = there ? aside_of(to) : to;

    Sweeping sweeping{into};

    {
        const std::shared_ptr<File::Held> yours = fill(into);

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
    return poured_at(to, durable, std::nullopt, [&](const path& into) -> std::shared_ptr<File::Held> {
        std::shared_ptr<File::Held> yours = open_at(into, Mode::truncate);

        if (!pour(*yours)) return nullptr;

        return yours;
    });
}

// The piece for an in-kernel copy: larger than a read installment, because with a server-side
// copy every piece is one question over the network, and a small piece would eat up the whole
// benefit. A cancellation waits for at most one piece.
constexpr std::size_t spliced_piece = 16 * 1024 * 1024;

#ifdef _WIN32

// `CopyFileExW` copies on its own: server-side on an SMB share, with ODX offload on a storage
// array, with attributes and streams. Progress and cancellation go through the callback, on the
// same thread, so the worker's bell is heard as in our own loop. When cancelled, the system
// removes the target itself.
DWORD CALLBACK copied(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                      LPVOID given) {
    const Tally& tally = *static_cast<const Tally*>(given);

    tally.held.done.store(tally.base + static_cast<std::uintmax_t>(done.QuadPart));

    return stopped() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

#endif

// Copies one file to `to`: first the system's own copy (Windows), then a clone, then an in-kernel
// copy in pieces, and finally our own read and write loop. The bytes go to `tally`. False when
// cancelled halfway.
bool file_at(const path& from, const path& to, Tally tally, bool durable = false) {
    const std::shared_ptr<File::Held> mine = open_at(from, Mode::read);

    const std::uintmax_t whole = sized(*mine);

    if (tally.owning) tally.held.whole.store(whole);

    // How much of this file is done, as the counter sees it.
    const auto at_byte = [&tally](std::uintmax_t at) { tally.held.done.store(tally.base + at); };

    const Instant when = instant_at(from);

    const std::size_t rata = rated(from, to);

    return poured_at(to, durable, when, [&](const path& into) -> std::shared_ptr<File::Held> {
#ifdef _WIN32
        BOOL cancel = FALSE;

        if (CopyFileExW(longly(from).c_str(), longly(into).c_str(), copied, &tally, &cancel,
                        COPY_FILE_FAIL_IF_EXISTS) != 0) {
            // The read-only attribute came from the source; left in place, it would block syncing
            // here and every later overwrite of the target. Our own loop does not carry it over
            // either.
            const DWORD told = GetFileAttributesW(longly(into).c_str());

            if (told != INVALID_FILE_ATTRIBUTES && (told & FILE_ATTRIBUTE_READONLY) != 0)
                SetFileAttributesW(longly(into).c_str(), told & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

            at_byte(whole);

            return open_at(into, Mode::write);
        }

        if (GetLastError() == ERROR_REQUEST_ABORTED || stopped()) return nullptr;

        // Another refusal: a system without this path (Wine), a medium that does not like it. The
        // plain loop will say what is really wrong, or simply copy.
#endif
        std::shared_ptr<File::Held> yours = open_at(into, Mode::truncate);

        if (whole > 0 && cloned_on(*mine, *yours)) {
            at_byte(whole);

            return yours;
        }

        bool ranged = true;

        for (std::uintmax_t at = 0; at < whole;) {
            if (stopped()) return nullptr;

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

            const Bytes part = read_at(*mine, at, rata);
            if (part.empty()) break;

            at += write_at(*yours, at, part);
            at_byte(at);
        }

        return yours;
    });
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

// How much the copy takes: the files only. What lies behind links is not counted, so the total is
// a lower bound, enough to refuse what certainly does not fit.
void measured(const std::vector<Entry>& among, std::uintmax_t& whole, std::uintmax_t& largest) {
    for (const Entry& one : among) {
        if (one.kind != Kind::file) continue;

        whole += one.size;
        largest = std::max(largest, one.size);
    }
}

bool tree_at(const path& from, const path& to, Counted<void>& held, Conflict how, Seen& seen,
             int deep);

// One entry from the listing. A link is followed: what it points to is copied, not the link
// itself. A pipe, a socket and a device are skipped: opening a pipe for reading would hang a pool
// thread beyond rescue, and that is worse than a missing copy.
bool spread_at(const path& from, Kind kind, const path& to, Conflict how, Seen& seen, int deep, Tally tally) {
    if (kind == Kind::folder) {
        make_folder_at(to);

        return true;
    }

    path onto = from;

    if (kind == Kind::link) {
        std::error_code trouble;

        onto = stood::canonical(from, trouble);
        if (trouble) return true;

        kind = kind_at(onto);

        if (kind == Kind::folder) {
            if (sighted(seen, onto)) return true;

            Counted<void> apart;

            return tree_at(onto, to, apart, how, seen, deep + 1);
        }

        if (kind != Kind::file) return true;

        const path where = settled_on(to, how);
        if (where.empty()) return true;

        // What a link points to is not in the folder's total, so its bytes are not counted.
        Counted<void> apart;

        return file_at(onto, where, Tally{apart});
    }

    if (kind != Kind::file) return true;

    const path where = settled_on(to, how);
    if (where.empty()) return true;

    return file_at(onto, where, tally);
}

// A folder goes entry by entry. Progress counts the bytes of its files plus one for every entry,
// so a big file moves the bar while it is copied and a folder of empty entries still moves it.
// The listing is sorted by path and a parent always precedes its children, so folders can be
// created in order. The content of followed links has its own counter and is not added to this
// one, because it is not in the total.
bool tree_at(const path& from, const path& to, Counted<void>& held, Conflict how, Seen& seen,
             int deep) {
    if (deep > 64) return true;

    const std::vector<Entry> among = walk_at(from);

    std::uintmax_t bytes = 0;
    std::uintmax_t largest = 0;

    measured(among, bytes, largest);

    held.whole.store(bytes + among.size() + 1);

    Sweeping sweeping{to};

    make_folder_at(to);
    held.done.fetch_add(1);

    std::vector<std::pair<path, Instant>> folders;

    for (const Entry& one : among) {
        if (stopped()) return false;

        const path where = to / one.where.lexically_relative(from);

        const std::uintmax_t base = held.done.load();

        if (!spread_at(one.where, one.kind, where, how, seen, deep, Tally{held, base, false})) return false;

        // The listed size, not the copied one: the total was made from the listing.
        if (one.kind == Kind::file) held.done.store(base + one.size);

        if (one.kind == Kind::folder) folders.emplace_back(where, instant_or(one.where, one.modified));

        held.done.fetch_add(1);
    }

    sweeping.keep();

    // The folders' times only at the end and from the deepest: every entry put inside changes the
    // time, so stamping earlier would leave the copy's subfolders with the time of copying instead
    // of the source's time.
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) stamp_at(at->first, at->second);

    stamp_at(to, instant_at(from));

    return true;
}


// Copies a file or a folder (following a link given as `from`) to `to`, after checking that it
// fits. Throws `Trouble` for nothing to copy, a dangling link, an entry that is neither file nor
// folder, or a copy onto itself. False when cancelled.
bool copy_at(const path& from, const path& to, Counted<void>& held, Conflict how) {
    Kind what = kind_at(from);

    if (what == Kind::none) amiss("Nothing to copy.", from);

    path onto = from;

    if (what == Kind::link) {
        std::error_code trouble;

        onto = stood::canonical(from, trouble);
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

        std::uintmax_t whole = 0;
        std::uintmax_t largest = 0;

        measured(walk_at(onto), whole, largest);
        fits_at(to, whole, largest);

        return tree_at(onto, to, held, how, seen, 0);
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

        const std::uintmax_t much = stood::file_size(onto, ignored);

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

        for (Entry& one : list_at(here)) {
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

// One file or link; the bytes of a plain file go to `tally`. False when cancelled during the copy;
// the source then stays whole.
bool carried(const path& from, const path& to, Conflict how, Tally tally) {
    const Shift made = shifted(from, to, how);

    if (made.where.empty() || made.done) return true;

    // Another medium: the system refused before it looked at the target, so the conflict is
    // settled once more.
    const path where = settled_on(to, how);
    if (where.empty()) return true;

    Counted<void> apart;
    Seen seen;

    if (kind_at(from) == Kind::link) {
        std::error_code trouble;

        const path onto = stood::canonical(from, trouble);
        if (trouble) amiss("The link leads nowhere.", from);

        if (kind_at(onto) == Kind::folder) {
            sighted(seen, onto);

            if (!tree_at(onto, where, apart, how, seen, 1)) return false;
        } else if (!file_at(onto, where, Tally{apart}, true)) {
            return false;
        }
    } else if (!file_at(from, where, tally, true)) {
        return false;
    }

    std::error_code trouble;

    stood::remove(from, trouble);
    if (trouble) amiss("Cannot remove the source after copying.", from);

    return true;
}

// Moves `from` to `to` entry by entry, for a move across media or a merge of folders. Throws
// `Trouble` when something cannot be moved or entries stayed behind; false when cancelled.
bool relocated(const path& from, const path& to, Counted<void>& held, Conflict how) {
    // A single file comes here only from another medium; within one, a rename was enough. Its
    // progress is its bytes; a link counts as one step.
    if (kind_at(from) != Kind::folder) {
        std::uintmax_t much = 0;

        if (kind_at(from) == Kind::file) {
            std::error_code ignored;

            much = stood::file_size(from, ignored);

            if (ignored) much = 0;
            else fits_at(to, much, much);
        }

        held.whole.store(much > 0 ? much : 1);

        if (!carried(from, to, how, Tally{held, 0, false})) return false;

        held.done.store(held.whole.load());

        return true;
    }

    const std::vector<Entry> among = surveyed(from);

    std::uintmax_t bytes = 0;
    std::uintmax_t largest = 0;

    measured(among, bytes, largest);

    if (!together_at(from, to)) fits_at(to, bytes, largest);

    // As in a folder copy: the bytes of the files plus one for every entry.
    held.whole.store(bytes + among.size() + 1);

    const Instant stamp = instant_at(from);

    make_folder_at(to);
    held.done.fetch_add(1);

    std::vector<std::pair<path, Instant>> folders;

    for (const Entry& one : among) {
        if (stopped()) return false;

        const path where = to / one.where.lexically_relative(from);

        const std::uintmax_t base = held.done.load();

        if (one.kind == Kind::folder) {
            make_folder_at(where);
            folders.emplace_back(one.where, instant_or(one.where, one.modified));
        } else if (!carried(one.where, where, how, Tally{held, base, false})) {
            return false;
        }

        // A rename within one medium reports nothing, and the copied size may differ from the
        // listed one; the listed size keeps the count in step with the total.
        if (one.kind == Kind::file) held.done.store(base + one.size);

        held.done.fetch_add(1);
    }

    // From the deepest: a folder goes only when empty, and its modification time passes to the
    // target only now, because every entry put inside changes it.
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) {
        std::error_code ignored;

        stamp_at(to / at->first.lexically_relative(from), at->second);
        stood::remove(at->first, ignored);
    }

    std::error_code ignored;

    stood::remove(from, ignored);
    stamp_at(to, stamp);

    if (how != Conflict::skip && exists_at(from)) amiss("Some entries stayed behind.", from);

    return true;
}

}

// Reads the whole file in installments, reporting progress; a cancelled read never settles.
Promise<Bytes> read(const path& where) {
    auto held = std::make_shared<Counted<Bytes>>();

    return HaulPromise<Bytes>{[where, held](auto resolve, auto) {
                              const std::shared_ptr<File::Held> one =
                                  open_at(where, Mode::read);

                              const std::uintmax_t whole = sized(*one);
                              held->whole.store(whole);

                              const std::size_t rata = chunk_at(where);

                              Bytes made;
                              made.reserve(static_cast<std::size_t>(whole));

                              for (std::uintmax_t at = 0; at < whole;) {
                                  if (stopped()) return;

                                  Bytes part = read_at(*one, at, rata);
                                  if (part.empty()) break;

                                  at += part.size();
                                  held->done.store(at);

                                  made.insert(made.end(), part.begin(), part.end());
                              }

                              resolve(std::move(made));
                          },
                          held};
}

// Writing over an existing file goes beside it and renames onto the target (`poured_at`), so an
// interrupted write leaves no stub under the real name.
Promise<void> write(const path& where, Bytes what, Conflict how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[where, what = std::move(what), how, held](auto resolve, auto) {
                             const path at_all = settled_on(where, how);

                             if (at_all.empty()) {
                                 resolve();
                                 return;
                             }

                             held->whole.store(what.size());

                             const std::size_t rata = chunk_at(at_all);

                             const bool whole = poured_at(at_all, false, [&](const File::Held& one) {
                                 for (std::size_t at = 0; at < what.size();) {
                                     if (stopped()) return false;

                                     const std::size_t much = std::min<std::size_t>(rata, what.size() - at);

                                     Bytes part{what.begin() + static_cast<std::ptrdiff_t>(at),
                                                what.begin() + static_cast<std::ptrdiff_t>(at + much)};

                                     at += write_at(one, at, part);
                                     held->done.store(at);
                                 }

                                 return true;
                             });

                             if (!whole) return;

                             resolve();
                         },
                         held};
}

// Copies a file or a folder in a pool task, with progress in bytes (for a folder, its files' bytes
// plus one per entry); `how` settles a conflict with an existing target.
Promise<void> copy(const path& from, const path& to, Conflict how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[from, to, how, held](auto resolve, auto) {
                             if (!copy_at(from, to, *held, how)) return;

                             resolve();
                         },
                         held};
}

// First a plain rename, because within one medium it is instant. Only when the system answers
// "another medium", and only then, the entries are moved one by one (`relocated`). Every other
// refusal is an error: a file open in a player must not end up as a 40 GB copy, present twice.
// A folder overwritten with a folder merges with it, also entry by entry.
Promise<void> move(const path& from, const path& to, Conflict how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[from, to, how, held](auto resolve, auto) {
                             if (kind_at(from) == Kind::none)
                                 amiss("Nothing to move.", from);

                             if (how == Conflict::overwrite && kind_at(from) == Kind::folder &&
                                 kind_at(to) == Kind::folder && !equivalent_at(from, to)) {
                                 if (!relocated(from, to, *held, how)) return;

                                 resolve();
                                 return;
                             }

                             const Shift made = shifted(from, to, how);

                             // Another medium: the system did not even have to look at the target.
                             // So the conflict is settled once more before anything is moved.
                             const path where = made.where.empty() || made.done ? path{} : settled_on(to, how);

                             if (where.empty()) {
                                 held->whole.store(1);
                                 held->done.store(1);

                                 resolve();
                                 return;
                             }

                             if (!relocated(from, where, *held, how)) return;

                             resolve();
                         },
                         held};
}

// Removes a file or a whole folder; a missing entry counts as already removed.
Promise<void> erase(const path& where) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[where, held](auto resolve, auto) {
                             if (kind_at(where) == Kind::none) {
                                 resolve();
                                 return;
                             }

                             std::vector<Entry> among;

                             if (kind_at(where) == Kind::folder) among = walk_at(where);

                             held->whole.store(among.size() + 1);

                             // From the deepest, because a folder can be removed only when empty.
                             for (auto at = among.rbegin(); at != among.rend(); ++at) {
                                 if (stopped()) return;

                                 std::error_code ignored;
                                 stood::remove(at->where, ignored);

                                 held->done.fetch_add(1);
                             }

                             std::error_code trouble;
                             stood::remove(where, trouble);

                             if (trouble) amiss("Cannot remove.", where);

                             held->done.fetch_add(1);

                             resolve();
                         },
                         held};
}

}

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
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include "talcite/place.hpp"
#include "talcite/promise.hpp"

#include "fscore.hpp"
#include "looking.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TALCITE_LINUX_FS)

#include <poll.h>
#include <sys/inotify.h>

#endif

#endif

namespace talcite::fs {

using namespace core;

// ---------------------------------------------------------------------------------------
// Watching directories. One thread for the whole library, just like the timer in Time: a hundred
// watched directories must not cost a hundred threads.
// ---------------------------------------------------------------------------------------

struct Watch::Held {
    path where;
    bool deep = false;
    std::uint64_t which = 0;

    struct Wanting {
        std::uint64_t id = 0;
        Settle<std::vector<Change>> settle;
    };

    std::mutex guard;
    std::deque<Change> pending;
    std::deque<Wanting> waiting;
    std::uint64_t asked = 0;

    ~Held();

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;
};

namespace {

constexpr std::size_t brim = 4096;

class Watching;

Watching& watching();

// The common outlet of both paths. A backend does not know whom it serves: it hands over changes
// under a number, and the dispatcher knows whether that is a plain watch or a file guarded through
// its parent directory.
void reached(std::uint64_t which, std::vector<Change> among);

// The event backend has lost hearing for this watch (no descriptors left, a broken share, the
// root gone) and hands it over to polling.
void forsaken(std::uint64_t which);

// A request whose promise was given up leaves the queue of waiting ones, otherwise the first
// batch would go to nobody. It runs inside the cancelling call; the request is destroyed outside
// the lock.
void forgotten(const std::weak_ptr<Watch::Held>& whose, std::uint64_t id) {
    const std::shared_ptr<Watch::Held> held = whose.lock();

    if (!held) return;

    std::optional<Watch::Held::Wanting> gone;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        const auto at = std::find_if(held->waiting.begin(), held->waiting.end(),
                                     [id](const Watch::Held::Wanting& one) { return one.id == id; });

        if (at == held->waiting.end()) return;

        gone.emplace(std::move(*at));
        held->waiting.erase(at);
    }
}

// Repeated "modified" for the same path are merged: the file system produces bursts, and the
// panel redraws once anyway. On overflow a single entry remains: count lost.
void noting(Watch::Held& held, Change made) {
    if (made.kind == Change::Kind::modified) {
        std::size_t looked = 0;

        for (auto at = held.pending.rbegin(); at != held.pending.rend() && looked < 32;
             ++at, ++looked) {
            if (at->where != made.where) continue;
            if (at->kind == Change::Kind::modified) return;

            break;
        }
    }

    held.pending.push_back(std::move(made));

    if (held.pending.size() <= brim) return;

    held.pending.clear();
    held.pending.push_back(Change{held.where, path{}, Change::Kind::overflow});
}

// Adds the changes to the watch's pending ones and, if someone is waiting, hands all pending
// changes to the first waiting request. A request may have been given up a moment ago and not
// have left the queue yet; it refuses the batch, and the next one gets it. The batch is handed
// over as a copy, so that a refusal loses nothing: whatever nobody took stays pending. Settling
// runs no code of the waiting side here, so it may happen under the lock.
void handed(const std::shared_ptr<Watch::Held>& held, std::vector<Change> among) {
    std::deque<Watch::Held::Wanting> gone;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        for (Change& one : among) noting(*held, std::move(one));

        while (!held->waiting.empty() && !held->pending.empty()) {
            Watch::Held::Wanting first = std::move(held->waiting.front());

            held->waiting.pop_front();

            const bool taken =
                first.settle.value(std::vector<Change>(held->pending.begin(), held->pending.end()));

            gone.push_back(std::move(first));

            if (taken) held->pending.clear();
        }
    }
}

}

namespace {

#if defined(TALCITE_LINUX_FS)

constexpr std::uint32_t noticed = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                                  IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

struct Post {
    path where;
    bool deep = false;
    std::vector<int> spots;
};

// The directories under the root are collected by our own walk, not by walk_at: only directories
// are needed here, while walk_at gathers every entry and answers to the stop of the work running on
// its thread, and the watcher thread runs none.
std::vector<path> under(const path& where) {
    std::vector<path> made;
    std::vector<path> waiting{where};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        std::error_code trouble;
        stood::directory_iterator at{here, stood::directory_options::skip_permission_denied,
                                     trouble};

        if (trouble) continue;

        for (const stood::directory_entry& one : at) {
            std::error_code ignored;

            if (!one.is_directory(ignored) || one.is_symlink(ignored)) continue;

            made.push_back(one.path());
            waiting.push_back(one.path());
        }
    }

    return made;
}

// The event backend on Linux: one inotify descriptor, read by one thread.
class Prompt {
public:
    // Opens the inotify descriptor and starts the reading thread.
    void begin() {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

        keeper_ = std::thread{[this] { serve(); }};
    }

    // Whether inotify could be opened.
    [[nodiscard]] bool ready() const { return fd_ >= 0; }

    // Adds the watch `which` on `where` (and, when `deep`, on every directory below). False means
    // the system ran out of watch descriptors (ENOSPC, the max_user_watches limit, or ENOMEM) and
    // the watch goes to polling. A watch with holes, missing some subdirectories, would be worse
    // than a slower one: it would stay silent about changes in places the panel shows.
    [[nodiscard]] bool add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;

        std::vector<std::pair<int, path>> made;

        const int first = inotify_add_watch(fd_, where.c_str(), noticed);

        if (first < 0) {
            if (spent()) return false;

            amiss("Cannot watch.", where);
        }

        made.emplace_back(first, where);

        if (deep)
            for (const path& one : under(where)) {
                const int more = inotify_add_watch(fd_, one.c_str(), noticed);

                if (more >= 0) {
                    made.emplace_back(more, one);
                } else if (spent()) {
                    unwatched(made);

                    return false;
                }
            }

        const std::lock_guard<std::mutex> kept{guard_};

        for (const auto& [spot, where] : made) {
            post->spots.push_back(spot);
            spots_[spot].push_back(Spot{which, where});
        }

        posts_[which] = std::move(post);

        return true;
    }

    // Removes the watch `which`. inotify returns the same descriptor for the same place and only
    // merges the masks, so two watches of one directory share one entry. It is removed only when
    // nobody uses it any more.
    void drop(std::uint64_t which) {
        std::vector<int> spare;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) return;

            const std::shared_ptr<Post> taken = std::move(at->second);
            posts_.erase(at);

            for (const int one : taken->spots)
                if (released(one, which)) spare.push_back(one);
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

private:
    struct Spot {
        std::uint64_t which = 0;
        path where;
    };

    // Whether the last inotify error means that the system ran out of watches or memory.
    static bool spent() { return errno == ENOSPC || errno == ENOMEM; }

    // Removes the entries of one watch from a descriptor; true when nobody uses it any more.
    // Called under the lock.
    bool released(int spot, std::uint64_t which) {
        const auto found = spots_.find(spot);
        if (found == spots_.end()) return false;

        std::vector<Spot>& among = found->second;

        among.erase(std::remove_if(among.begin(), among.end(), [which](const Spot& mine) { return mine.which == which; }),
                    among.end());

        if (!among.empty()) return false;

        spots_.erase(found);

        return true;
    }

    // Rolls back a failed addition: removes the descriptors that no other watch shares.
    void unwatched(const std::vector<std::pair<int, path>>& made) {
        std::vector<int> spare;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            for (const auto& [spot, where] : made)
                if (spots_.find(spot) == spots_.end()) spare.push_back(spot);
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

    // The loop of the reading thread: waits up to a second for events and passes each batch to
    // `take`.
    void serve() {
        std::vector<char> room(64 * 1024);

        while (true) {
            pollfd told{};
            told.fd = fd_;
            told.events = POLLIN;

            if (::poll(&told, 1, 1000) <= 0) continue;

            const ssize_t given = ::read(fd_, room.data(), room.size());
            if (given <= 0) continue;

            take(room.data(), static_cast<std::size_t>(given));
        }
    }

    // Whether `where` is the root of the watch `which`. The root has its own rules: only for it
    // is "removed" reported from its own event. A subdirectory of a deep watch is reported by its
    // parent (`IN_DELETE`, `IN_MOVED_FROM`), so it must not also be reported from its own event,
    // nor as "modified" from `IN_IGNORED`, which every descriptor gets after removal.
    bool rooted(std::uint64_t which, const path& where) {
        const std::lock_guard<std::mutex> kept{guard_};

        const auto at = posts_.find(which);

        return at != posts_.end() && at->second->where == where;
    }

    // Turns a batch of `much` bytes of inotify events into changes for each watch, pairs moves
    // into renames, follows directories that appear and leave, and hands watches that went deaf
    // over to polling.
    void take(const char* room, std::size_t much) {
        std::unordered_map<std::uint64_t, std::vector<Change>> among;
        std::map<std::pair<std::uint64_t, std::uint32_t>, path> moving;
        std::vector<std::pair<std::uint64_t, path>> fresh;
        std::vector<std::pair<std::uint64_t, path>> gone;
        std::vector<std::uint64_t> blind;

        for (std::size_t at = 0; at + sizeof(inotify_event) <= much;) {
            const auto* told = reinterpret_cast<const inotify_event*>(room + at);
            at += sizeof(inotify_event) + told->len;

            if ((told->mask & IN_Q_OVERFLOW) != 0) {
                spilled();
                return;
            }

            std::vector<Spot> spots;

            {
                const std::lock_guard<std::mutex> kept{guard_};

                const auto found = spots_.find(told->wd);
                if (found == spots_.end()) continue;

                spots = found->second;

                // After `IN_IGNORED` the descriptor no longer exists, and the kernel may give its
                // number to someone new; an entry left behind would assign their events to us.
                if ((told->mask & IN_IGNORED) != 0) {
                    for (const Spot& one : spots) {
                        const auto post = posts_.find(one.which);

                        if (post == posts_.end()) continue;

                        std::vector<int>& mine = post->second->spots;

                        mine.erase(std::remove(mine.begin(), mine.end(), told->wd), mine.end());
                    }

                    spots_.erase(found);
                }
            }

            // The same descriptor can belong to several watches at once, so one event goes to
            // each of them separately.
            for (const Spot& spot : spots) {
                const std::uint64_t which = spot.which;
                const path where = told->len > 0 ? spot.where / path{told->name} : spot.where;

                std::vector<Change>& mine = among[which];

                if ((told->mask & IN_IGNORED) != 0) {
                    // A root without a descriptor (removed, unmounted) will not hear anything
                    // any more. Polling remains, and it will also see the root come back.
                    if (rooted(which, spot.where)) blind.push_back(which);

                    continue;
                }

                if ((told->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    if (rooted(which, spot.where)) mine.push_back(Change{where, path{}, Change::Kind::removed});

                    continue;
                }

                if ((told->mask & IN_MOVED_FROM) != 0) {
                    moving[{which, told->cookie}] = where;
                    mine.push_back(Change{where, path{}, Change::Kind::removed});

                    if ((told->mask & IN_ISDIR) != 0) gone.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_MOVED_TO) != 0) {
                    const auto paired = moving.find({which, told->cookie});

                    if (paired == moving.end()) {
                        mine.push_back(Change{where, path{}, Change::Kind::created});
                    } else {
                        // A pair of "moved out of here" and "moved in there" is one rename, so the
                        // removal reported a moment ago is taken back.
                        const auto stale =
                            std::find_if(mine.begin(), mine.end(), [&paired](const Change& one) {
                                return one.kind == Change::Kind::removed && one.where == paired->second;
                            });

                        if (stale != mine.end()) mine.erase(stale);

                        mine.push_back(Change{where, paired->second, Change::Kind::renamed});

                        const auto left = std::find(gone.begin(), gone.end(), std::make_pair(which, paired->second));

                        if (left != gone.end()) gone.erase(left);

                        moving.erase(paired);
                    }

                    if ((told->mask & IN_ISDIR) != 0) fresh.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_CREATE) != 0) {
                    mine.push_back(Change{where, path{}, Change::Kind::created});

                    if ((told->mask & IN_ISDIR) != 0) fresh.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_DELETE) != 0) {
                    mine.push_back(Change{where, path{}, Change::Kind::removed});
                    continue;
                }

                mine.push_back(Change{where, path{}, Change::Kind::modified});
            }
        }

        for (const auto& [which, where] : gone) pruned(which, where);

        for (const auto& [which, where] : fresh)
            if (!sprout(which, where)) blind.push_back(which);

        for (auto& [which, mine] : among)
            if (!mine.empty()) reached(which, std::move(mine));

        for (const std::uint64_t which : blind) {
            drop(which);
            forsaken(which);
        }
    }

    // A new subdirectory under a deep watch has to be added separately: inotify does not know how
    // to watch a tree, only single directories. That is the price of inotify: a large tree costs
    // thousands of descriptors.
    //
    // A directory renamed within the tree already has descriptors, and the kernel returns the
    // same numbers. Then we only correct the paths: adding them a second time would deliver every
    // event twice, and the old entries would keep the old name, so a file in the renamed directory
    // would be reported under a path that no longer exists.
    //
    // False means the descriptors ran out and the watch has to switch to polling.
    bool sprout(std::uint64_t which, const path& where) {
        std::vector<path> among{where};

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end() || !at->second->deep) return true;
        }

        for (const path& one : under(where)) among.push_back(one);

        for (const path& one : among) {
            const int spot = inotify_add_watch(fd_, one.c_str(), noticed);

            if (spot < 0) {
                if (spent()) return false;

                continue;
            }

            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) {
                if (spots_.find(spot) == spots_.end()) inotify_rm_watch(fd_, spot);
                continue;
            }

            std::vector<Spot>& sharing = spots_[spot];

            const auto mine =
                std::find_if(sharing.begin(), sharing.end(), [which](const Spot& spot) { return spot.which == which; });

            if (mine != sharing.end()) {
                mine->where = one;
                continue;
            }

            at->second->spots.push_back(spot);
            sharing.push_back(Spot{which, one});
        }

        return true;
    }

    // A directory moved out of the watched tree still has our descriptors, since its inode lives
    // on elsewhere. Left in place, they would report changes there under a path in our tree.
    void pruned(std::uint64_t which, const path& where) {
        std::vector<int> spare;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) return;

            std::vector<int>& mine = at->second->spots;

            for (auto one = mine.begin(); one != mine.end();) {
                const auto found = spots_.find(*one);

                const bool inside =
                    found != spots_.end() &&
                    std::any_of(found->second.begin(), found->second.end(), [&](const Spot& spot) {
                        if (spot.which != which) return false;

                        const auto [left, right] =
                            std::mismatch(where.begin(), where.end(), spot.where.begin(), spot.where.end());

                        return left == where.end();
                    });

                if (!inside) {
                    ++one;
                    continue;
                }

                if (released(*one, which)) spare.push_back(*one);

                one = mine.erase(one);
            }
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

    // The kernel's event queue overflowed: every watch gets an overflow change, since anything
    // may have been lost.
    void spilled() {
        std::vector<std::pair<std::uint64_t, path>> among;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            for (const auto& [which, post] : posts_) among.emplace_back(which, post->where);
        }

        for (const auto& [which, where] : among)
            reached(which, std::vector<Change>{Change{where, path{}, Change::Kind::overflow}});
    }

    std::mutex guard_;
    std::unordered_map<int, std::vector<Spot>> spots_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Post>> posts_;
    int fd_ = -1;
    std::thread keeper_;
};

#endif

}

namespace {

#if defined(_WIN32)

constexpr DWORD noticed = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                          FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_LAST_WRITE;

struct Post {
    path where;
    bool deep = false;
    std::uint64_t which = 0;

    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED told{};
    std::vector<char> room = std::vector<char>(64 * 1024);
    bool going = false;
};

// The event backend on Windows: ReadDirectoryChangesW on each directory, completed through one
// I/O completion port served by one thread.
class Prompt {
public:
    // Creates the completion port and starts the thread that serves it.
    void begin() {
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);

        keeper_ = std::thread{[this] { serve(); }};
    }

    // Whether the completion port could be created.
    [[nodiscard]] bool ready() const { return port_ != nullptr; }

    // Adds the watch `which` on `where`. False means the directory exists but the system cannot
    // report on it: a share on a server without notifications (some NAS devices, WebDAV, FUSE file
    // systems under WinFsp) answers ERROR_INVALID_FUNCTION or ERROR_NOT_SUPPORTED. Then we poll
    // ourselves instead of refusing to watch.
    [[nodiscard]] bool add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;

        post->handle = CreateFileW(longly(where).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

        if (post->handle == INVALID_HANDLE_VALUE) amiss("Cannot watch.", where);

        post->which = which;

        if (CreateIoCompletionPort(post->handle, port_, static_cast<ULONG_PTR>(which), 0) ==
            nullptr) {
            CloseHandle(post->handle);
            amiss("Cannot watch.", where);
        }

        {
            const std::lock_guard<std::mutex> kept{guard_};

            posts_[which] = post;
        }

        if (armed(*post)) return true;

        // Arming failed, so nothing is in flight and the handle may be closed at once.
        {
            const std::lock_guard<std::mutex> kept{guard_};

            posts_.erase(which);
        }

        CloseHandle(post->handle);

        return false;
    }

    // Removes the watch `which`, but does not close the handle here. A read is in flight, and the
    // system holds the buffer and the OVERLAPPED until it completes; closing it underneath would
    // leave a write into freed memory. So we mark the entry and wait until the cancelled
    // completion comes back to the watcher thread.
    void drop(std::uint64_t which) {
        const std::lock_guard<std::mutex> kept{guard_};

        const auto at = posts_.find(which);
        if (at == posts_.end()) return;

        at->second->going = true;

        CancelIoEx(at->second->handle, &at->second->told);
    }

private:
    // Starts the next asynchronous read of changes on the directory; false when it is refused.
    static bool armed(Post& post) {
        DWORD given = 0;

        return ReadDirectoryChangesW(post.handle, post.room.data(),
                                     static_cast<DWORD>(post.room.size()),
                                     post.deep ? TRUE : FALSE, noticed, &given, &post.told,
                                     nullptr) != 0;
    }

    // The loop of the completion port thread: takes each completed read, hands its changes on and
    // arms the next one, and closes the entries that ended.
    void serve() {
        while (true) {
            DWORD given = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* told = nullptr;

            const BOOL fine = GetQueuedCompletionStatus(port_, &given, &key, &told, INFINITE);

            if (told == nullptr) continue;

            std::shared_ptr<Post> post;
            bool ended = false;

            {
                const std::lock_guard<std::mutex> kept{guard_};

                const auto at = posts_.find(static_cast<std::uint64_t>(key));
                if (at == posts_.end()) continue;

                post = at->second;

                if (post->going || fine == 0) {
                    CloseHandle(post->handle);
                    posts_.erase(at);

                    ended = true;
                }
            }

            // The read was broken off by the system, not by us: the share disconnected
            // (ERROR_NETNAME_DELETED), the directory disappeared. The watch must not simply fall
            // silent; it switches to polling and says that it lost count.
            if (ended) {
                if (!post->going) forsaken(post->which);

                continue;
            }

            // A read of zero bytes means the system ran out of room for events. There is nothing
            // to recover; what remains is to admit it and ask for the directory to be read again.
            if (given == 0)
                reached(post->which,
                        std::vector<Change>{Change{post->where, path{}, Change::Kind::overflow}});
            else reached(post->which, take(*post, given));

            // Re-arming was refused: nothing is in flight, so the handle is closed here, without
            // waiting for a completion that will not come.
            if (!armed(*post)) {
                {
                    const std::lock_guard<std::mutex> kept{guard_};

                    posts_.erase(post->which);
                }

                CloseHandle(post->handle);

                forsaken(post->which);
            }
        }
    }

    // Turns `much` bytes of FILE_NOTIFY_INFORMATION records into changes, pairing the old and
    // the new name of a rename.
    static std::vector<Change> take(const Post& post, DWORD much) {
        std::vector<Change> among;

        path stale;
        std::size_t at = 0;

        while (at + sizeof(FILE_NOTIFY_INFORMATION) <= much) {
            const auto* told =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(post.room.data() + at);

            const std::wstring named{told->FileName,
                                     told->FileNameLength / sizeof(wchar_t)};

            const path where = post.where / path{named};

            switch (told->Action) {
                case FILE_ACTION_ADDED:
                    among.push_back(Change{where, path{}, Change::Kind::created});
                    break;

                case FILE_ACTION_REMOVED:
                    among.push_back(Change{where, path{}, Change::Kind::removed});
                    break;

                case FILE_ACTION_RENAMED_OLD_NAME:
                    stale = where;
                    break;

                case FILE_ACTION_RENAMED_NEW_NAME:
                    among.push_back(Change{where, stale, Change::Kind::renamed});
                    stale.clear();
                    break;

                default:
                    among.push_back(Change{where, path{}, Change::Kind::modified});
                    break;
            }

            if (told->NextEntryOffset == 0) break;

            at += told->NextEntryOffset;
        }

        return among;
    }

    std::mutex guard_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Post>> posts_;
    HANDLE port_ = nullptr;
    std::thread keeper_;
};

#endif

}

namespace core {

// Polling. Where the system cannot tell by itself (and also where it can, but lies), we look into
// the directory every so often and compare with what we saw the previous time. There are no
// renames here: the old name disappears, a new one appears, and that is all that can honestly be
// said without the system's help.
//
// A listing carries times in seconds, and a write of the same length within the same second as
// the previous look changes neither the second nor the size, so the change would be lost. That is
// why a file modified within the last minute (`fresh`) is asked separately for its exact time.
// Older ones are not: an extra question for every file on a network share would cost as much
// again, and for them a second is enough. A minute rather than two seconds, because the server's
// clock can drift from ours.
//
// The root itself stands in a look without size and time: its disappearance and return are a
// change, and a new file inside is not, just as in the system's notifications.
namespace {

constexpr std::chrono::seconds fresh{60};

// The exact modification time of a file changed within `fresh` of `now`, or an empty instant for
// anything else or when it cannot be read.
Instant finely(const path& where, Kind kind, std::chrono::sys_seconds modified,
               std::chrono::system_clock::time_point now) {
    if (kind != Kind::file) return Instant{};

    const auto gap = now - modified;

    if (gap > fresh || gap < -fresh) return Instant{};

    try {
        return instant_at(where);
    } catch (...) {
        return Instant{};
    }
}

// What stands at the path, or nothing when that cannot be told: unlike `kind_at`, which takes
// every failure for absence, a failure other than "no such entry" stays unknown.
std::optional<Kind> known_kind(const path& where) {
    std::error_code why;

    (void)stood::symlink_status(long_form(where), why);

    if (why && why != std::errc::no_such_file_or_directory && why != std::errc::not_a_directory) return std::nullopt;

    return kind_at(where);
}

}

// One look at `where`: what was seen (the root and, for a directory, its entries, the whole tree
// when `deep`), each as a `Sight` of kind, size, time in seconds and, for a fresh file, the exact
// time (`fine`); the folders whose entries could not be read; and whether the root itself could not
// be told (a share that does not answer, no right to look), which is not the same as its absence.
// A folder that cannot be read is noted, not taken for empty: a NAS that does not answer for a
// moment, or a folder whose rights were taken away, would otherwise report everything inside as
// removed, and everything as created when it answers again.
Look looked(const path& where, bool deep) {
    Look made;

    const std::optional<Kind> what = known_kind(where);

    if (!what) {
        made.blind = true;

        return made;
    }

    if (*what == Kind::none) return made;

    const auto now = std::chrono::system_clock::now();

    if (*what != Kind::folder) {
        Sight mine;
        mine.kind = *what;

        std::error_code ignored;
        mine.size = stood::file_size(long_form(where), ignored);

        try {
            mine.modified = modified_at(where);
        } catch (...) {
        }

        mine.fine = finely(where, *what, mine.modified, now);

        made.seen[where] = mine;

        return made;
    }

    made.seen[where] = Sight{Kind::folder, 0, {}, {}};

    std::vector<path> waiting{where};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        std::vector<Entry> among;

        try {
            among = list_whole_at(here);
        } catch (...) {
            made.unread.push_back(here);

            continue;
        }

        for (const Entry& one : among) {
            made.seen[one.where] = Sight{one.kind, one.size, one.modified, finely(one.where, one.kind, one.modified, now)};

            if (deep && one.kind == Kind::folder) waiting.push_back(one.where);
        }
    }

    return made;
}

namespace {

// Whether the path lies under one of the folders, the folders themselves excluded.
bool beneath(const path& where, const std::vector<path>& folders) {
    for (const path& folder : folders) {
        auto one = where.begin();
        bool inside = true;

        for (auto two = folder.begin(); two != folder.end(); ++two, ++one)
            if (one == where.end() || *one != *two) {
                inside = false;

                break;
            }

        if (inside && one != where.end()) return true;
    }

    return false;
}

}

// The new look with the gaps of its reading filled from the previous one: what the previous look saw
// under a folder that could not be read now is taken as unchanged, so it is neither removed now nor
// created again later.
std::map<path, Sight> filled(Look now, const std::map<path, Sight>& was) {
    if (!now.unread.empty())
        for (const auto& [where, mine] : was)
            if (!now.seen.contains(where) && beneath(where, now.unread)) now.seen.emplace(where, mine);

    return std::move(now.seen);
}

// The changes between two looks: created, modified and removed entries.
std::vector<Change> apart(const std::map<path, Sight>& was, const std::map<path, Sight>& now) {
    std::vector<Change> made;

    for (const auto& [where, mine] : now) {
        const auto at = was.find(where);

        if (at == was.end()) {
            made.push_back(Change{where, path{}, Change::Kind::created});
            continue;
        }

        // The exact time is compared only when both looks have it: a file that has just dropped
        // out of the last minute must not pose as a change.
        const bool finer = at->second.fine != Instant{} && mine.fine != Instant{} && at->second.fine != mine.fine;

        if (at->second.kind != mine.kind || at->second.size != mine.size ||
            at->second.modified != mine.modified || finer)
            made.push_back(Change{where, path{}, Change::Kind::modified});
    }

    for (const auto& [where, mine] : was)
        if (now.find(where) == now.end()) made.push_back(Change{where, path{}, Change::Kind::removed});

    return made;
}

}

namespace {

// The polling backend: one thread that looks into every polled watch in turn.
class Polling {
public:
    // Starts the polling thread.
    void begin() { keeper_ = std::thread{[this] { serve(); }}; }

    // Adds the watch `which`. The first look is taken here, in the caller's thread, so that
    // changes right after watch() returns are not lost. From then on `seen` belongs only to the
    // polling thread, and the handover goes through this lock.
    void add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;
        post->seen = looked(where, deep).seen;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            posts_[which] = std::move(post);
        }

        stirred_.notify_all();
    }

    // Stops polling the watch `which`.
    void drop(std::uint64_t which) {
        const std::lock_guard<std::mutex> kept{guard_};

        posts_.erase(which);
    }

private:
    struct Post {
        path where;
        bool deep = false;
        std::map<path, Sight> seen;
    };

    // The loop of the polling thread. It never uses more than a tenth of a core: the pause is ten
    // times longer than the looking itself, no shorter than half a second and no longer than ten.
    // A small directory is thus polled twice a second, and a huge tree rarely, because walking it
    // costs. A fixed rate would be either too slow or too expensive.
    void serve() {
        while (true) {
            std::vector<std::pair<std::uint64_t, std::shared_ptr<Post>>> among;

            {
                std::unique_lock<std::mutex> kept{guard_};

                if (posts_.empty()) {
                    stirred_.wait(kept);
                    continue;
                }

                among.reserve(posts_.size());
                for (const auto& [which, post] : posts_) among.emplace_back(which, post);
            }

            const auto began = std::chrono::steady_clock::now();

            // A look that could not tell the root keeps the previous one: nothing is known, so
            // nothing is reported.
            for (const auto& [which, post] : among) {
                Look look = looked(post->where, post->deep);

                if (look.blind) continue;

                std::map<path, Sight> now = filled(std::move(look), post->seen);
                std::vector<Change> mine = apart(post->seen, now);

                post->seen = std::move(now);

                if (!mine.empty()) reached(which, std::move(mine));
            }

            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began);

            const auto rest = std::clamp(spent * 10, std::chrono::milliseconds{500},
                                         std::chrono::milliseconds{10000});

            std::unique_lock<std::mutex> kept{guard_};
            stirred_.wait_for(kept, rest);
        }
    }

    std::mutex guard_;
    std::condition_variable stirred_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Post>> posts_;
    std::thread keeper_;
};

// The dispatcher. Events from the system are cheaper and faster, so we use them wherever they are
// reliable, and on a network share they are not. inotify does not see changes made by another
// machine, because the kernel knows nothing of them; only our own polling can be relied on. So
// the choice is made by the same medium detection that gives the rate in `slice`.
class Watching {
public:
    // Starts the polling thread and, where the build has one, the event backend.
    void begin() {
        polling_.begin();

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        prompt_.begin();
#endif
    }

    // Registers a new watch and returns its number. It goes to events when they are reliable for
    // the place and available, otherwise to polling.
    std::uint64_t add(const std::shared_ptr<Watch::Held>& held) {
        Kept kept;

        kept.whose = held;

        bool polled = asking(held->where);

        // A single file is not watched directly. ReadDirectoryChangesW cannot do it at all, and
        // inotify binds to the inode, not to the name, so a write by replacement would leave the
        // watch on the removed content. So we guard the parent directory and look at the file
        // itself whenever something stirs at our name.
        if (!polled && kind_at(held->where) != Kind::folder) {
            kept.peering = true;
            kept.only = held->where;
            kept.seen = looked(held->where, false).seen;
        }

        const path where = kept.peering ? held->where.parent_path() : held->where;
        const bool deep = kept.peering ? false : held->deep;

        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> held_{guard_};

            which = ++next_;
        }

        kept.where = where;
        kept.deep = deep;

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        if (!polled && !prompt_.add(which, where, deep)) polled = true;
#endif

        if (polled) polling_.add(which, where, deep);

        // Only after a successful addition, so a failed one leaves no entry to remove.
        const std::lock_guard<std::mutex> held_{guard_};

        among_[which] = std::move(kept);

        return which;
    }

    // Removes the watch `which` from both paths: it may have started on events and switched to
    // polling (`forsaken`), and removing it from a path it is not on does nothing.
    void drop(std::uint64_t which) {
        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (among_.erase(which) == 0) return;
        }

        polling_.drop(which);

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        prompt_.drop(which);
#endif
    }

    // The event backend gave up the watch: from now on it is polled. An overflow change tells
    // the waiting side that something may have been lost while switching paths and it is worth
    // reading again.
    void forsaken(std::uint64_t which) {
        path where;
        bool deep = false;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            where = at->second.where;
            deep = at->second.deep;
        }

        polling_.add(which, where, deep);

        // The watch was closed meanwhile, while `drop` was still looking for it on the event path.
        bool gone = false;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            gone = among_.find(which) == among_.end();
        }

        if (gone) {
            polling_.drop(which);
            return;
        }

        reached(which, std::vector<Change>{Change{where, path{}, Change::Kind::overflow}});
    }

    // Delivers changes for the watch `which`: directly for a plain watch, and for a file
    // guarded through its parent only after looking at the file itself.
    void reached(std::uint64_t which, std::vector<Change> among) {
        std::shared_ptr<Watch::Held> held;
        bool peering = false;
        path only;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            held = at->second.whose.lock();
            peering = at->second.peering;
            only = at->second.only;
        }

        if (!held) return;

        if (!peering) {
            handed(held, std::move(among));
            return;
        }

        if (!touched(among, only)) return;

        // The directory only said that something stirred at our name. What exactly is told only
        // by a look at the file itself, compared with the previous one.
        Look look = looked(only, false);

        if (look.blind) return;

        std::map<path, Sight> now = std::move(look.seen);
        std::vector<Change> mine;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            mine = apart(at->second.seen, now);
            at->second.seen = std::move(now);
        }

        if (!mine.empty()) handed(held, std::move(mine));
    }

private:
    struct Kept {
        std::weak_ptr<Watch::Held> whose;
        path where;
        bool deep = false;
        bool peering = false;
        path only;
        std::map<path, Sight> seen;
    };

    // Whether the changes concern the file `only`, or overflowed and so may concern it.
    static bool touched(const std::vector<Change>& among, const path& only) {
        for (const Change& one : among) {
            if (one.kind == Change::Kind::overflow) return true;
            if (one.where == only || one.was == only) return true;
        }

        return false;
    }

    // Whether `where` must be polled. Events from the system are cheaper and faster, so we use
    // them wherever they are reliable. On a network share under Linux they are not: inotify does
    // not see changes made by another machine, because the kernel knows nothing of them, and that
    // is exactly the case of a NAS. The same medium detection as for the rate decides. Windows
    // copes on a share by itself, because the notifications come from the server.
    static bool asking(const path& where) {
#if defined(TALCITE_WATCH_POLLING)
        (void)where;

        return true;
#elif defined(_WIN32)
        (void)where;

        return false;
#elif defined(TALCITE_LINUX_FS)
        return afar_at(kind_at(where) == Kind::folder ? where : where.parent_path());
#else
        (void)where;

        return true;
#endif
    }

    std::mutex guard_;
    std::unordered_map<std::uint64_t, Kept> among_;
    std::uint64_t next_ = 0;

    Polling polling_;

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
    Prompt prompt_;
#endif
};

// The watcher is deliberately never destroyed, for the same reason as the timer: it is created
// later than the pool, so it would go first, while the workers are still running.
Watching& watching() {
    static Watching* only = [] {
        auto* made = new Watching;
        made->begin();

        return made;
    }();

    return *only;
}

// Passes changes from a backend to the dispatcher.
void reached(std::uint64_t which, std::vector<Change> among) {
    watching().reached(which, std::move(among));
}

// Passes a watch that a backend gave up to the dispatcher, which moves it to polling.
void forsaken(std::uint64_t which) { watching().forsaken(which); }

}

// Unregisters the watch when the last handle to it goes away.
Watch::Held::~Held() {
    if (which != 0) watching().drop(which);

    for (Wanting& one : waiting) (void)one.settle.fail(std::make_exception_ptr(Trouble{"The watch is closed.", where}));
}

// The watched path, or an empty path for a closed watch.
const path& Watch::where() const {
    static const path nowhere;

    return held_ ? held_->where : nowhere;
}

// Starts watching a directory (the whole tree below it when `deep`) or a single file. Fails
// when there is nothing at `where`.
Promise<Watch> watch(const path& where, bool deep) {
    return run(io(), [where, deep]() -> Watch {
        if (kind_at(where) != Kind::folder && kind_at(where) != Kind::file)
            amiss("Nothing to watch.", where);

        auto held = std::make_shared<Watch::Held>();

        held->where = where;
        held->deep = deep;
        held->which = watching().add(held);

        return Watch{held};
    });
}

// The next batch of changes: everything pending at once, or the first batch that arrives. Giving
// up the promise takes the request out of the queue.
Promise<std::vector<Change>> Watch::next() {
    const std::shared_ptr<Held> held = held_;

    if (!held) return failed<std::vector<Change>>(std::make_exception_ptr(Trouble{"The watch is closed.", path{}}));

    std::vector<Change> giving;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->pending.empty()) {
            auto [made, settle] = promised<std::vector<Change>>();

            const std::uint64_t id = ++held->asked;

            (void)detail::stopping(settle.stop(), [weak = std::weak_ptr<Held>{held}, id] { forgotten(weak, id); });

            held->waiting.push_back(Held::Wanting{id, std::move(settle)});

            return std::move(made);
        }

        giving.assign(std::make_move_iterator(held->pending.begin()), std::make_move_iterator(held->pending.end()));
        held->pending.clear();
    }

    return ready(std::move(giving));
}

}

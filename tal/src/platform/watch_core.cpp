#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include "tal/fs.hpp"

#include "core.hpp"

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

// Watching folders on the disk, ported from talcite's watch.cpp: one inotify descriptor on Linux,
// ReadDirectoryChangesW on Windows, and polling for network shares on Linux (whose changes made by
// another machine inotify never hears of) and for a watch the system gave up on. One thread serves
// each path for the whole program. A watch gathers what it hears as the old one did: repeated
// changes of content at one path are one, and changes of content alone wait until the oldest is
// `settling` old, so that a file written in small pieces wakes its reader a few times a second;
// a change of what stands goes at once.

namespace tal::platform {

using namespace core;

namespace {

// How long changes of content alone wait before they go to the reader.
constexpr std::chrono::milliseconds settling{200};

// The common outlet of every path: changes heard for the watch `which`.
void reached(std::uint64_t which, std::vector<Change> among);

// The event backend has lost hearing for this watch (no descriptors left, a broken share, the
// root gone) and hands it over to polling.
void forsaken(std::uint64_t which);

}

namespace core {

struct Sight {
    Kind kind = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds modified{};
    Instant fine{};
};

struct Look {
    std::map<path, Sight> seen;
    std::vector<path> unread;
    bool blind = false;
};

[[nodiscard]] Look looked(const path& where, bool deep);

[[nodiscard]] std::map<path, Sight> filled(Look now, const std::map<path, Sight>& was);

[[nodiscard]] std::vector<Change> apart(const std::map<path, Sight>& was, const std::map<path, Sight>& now);


[[nodiscard]] Look looked(const path& where, bool deep);

[[nodiscard]] std::map<path, Sight> filled(Look now, const std::map<path, Sight>& was);

[[nodiscard]] std::vector<Change> apart(const std::map<path, Sight>& was, const std::map<path, Sight>& now);

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
// its thread, and the watcher thread runs none. A directory that fails halfway (a drive pulled out)
// gives what was read of it: the walk runs on the watcher's thread, where a throw would end the
// program.
std::vector<path> under(const path& where) {
    std::vector<path> made;
    std::vector<path> waiting{where};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        std::error_code trouble;
        stood::directory_iterator at{here, stood::directory_options::skip_permission_denied,
                                     trouble};

        for (; !trouble && at != stood::directory_iterator{}; at.increment(trouble)) {
            const stood::directory_entry& one = *at;
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
    // `take`. The kernel queues the two halves of a move one after the other, and a read can come
    // between them; read apart they would be a removal and a creation, so a batch with a half
    // alone waits a little for the other.
    void serve() {
        constexpr std::size_t piece = 64 * 1024;
        std::vector<char> room(piece);

        while (true) {
            pollfd told{};
            told.fd = fd_;
            told.events = POLLIN;

            if (::poll(&told, 1, 1000) <= 0) continue;

            const ssize_t given = ::read(fd_, room.data(), room.size());
            if (given <= 0) continue;

            auto held = static_cast<std::size_t>(given);

            for (int round = 0; round < 3 && halved(room.data(), held) && ::poll(&told, 1, 20) > 0; ++round) {
                if (room.size() - held < piece) room.resize(held + piece);

                const ssize_t more = ::read(fd_, room.data() + held, room.size() - held);
                if (more <= 0) break;

                held += static_cast<std::size_t>(more);
            }

            take(room.data(), held);
        }
    }

    // Whether a batch of `much` bytes of inotify events holds a move out of a folder without the
    // move in that shares its cookie.
    static bool halved(const char* room, std::size_t much) {
        std::vector<std::uint32_t> open;

        for (std::size_t at = 0; at + sizeof(inotify_event) <= much;) {
            const auto* told = reinterpret_cast<const inotify_event*>(room + at);
            at += sizeof(inotify_event) + told->len;

            if ((told->mask & IN_MOVED_FROM) != 0) open.push_back(told->cookie);

            if ((told->mask & IN_MOVED_TO) != 0) open.erase(std::remove(open.begin(), open.end(), told->cookie), open.end());
        }

        return !open.empty();
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
    // over to polling. After an overflow of the kernel's queue every watch hears it (`spilled`),
    // and a deep watch adds again every directory of its tree: one that came while events were
    // lost would otherwise stay unwatched for good.
    void take(const char* room, std::size_t much) {
        bool spill = false;
        std::unordered_map<std::uint64_t, std::vector<Change>> among;
        std::map<std::pair<std::uint64_t, std::uint32_t>, path> moving;
        std::vector<std::pair<std::uint64_t, path>> fresh;
        std::vector<std::pair<std::uint64_t, path>> gone;
        std::vector<std::uint64_t> blind;

        for (std::size_t at = 0; at + sizeof(inotify_event) <= much;) {
            const auto* told = reinterpret_cast<const inotify_event*>(room + at);
            at += sizeof(inotify_event) + told->len;

            if ((told->mask & IN_Q_OVERFLOW) != 0) {
                spill = true;
                continue;
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

        if (spill) {
            for (const auto& [which, root] : trees())
                if (std::find(blind.begin(), blind.end(), which) == blind.end() && !sprout(which, root)) blind.push_back(which);

            spilled();
        } else {
            for (auto& [which, mine] : among)
                if (!mine.empty()) reached(which, std::move(mine));
        }

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

    // The deep watches with their roots.
    std::vector<std::pair<std::uint64_t, path>> trees() {
        std::vector<std::pair<std::uint64_t, path>> made;

        const std::lock_guard<std::mutex> kept{guard_};

        for (const auto& [which, post] : posts_)
            if (post->deep) made.emplace_back(which, post->where);

        return made;
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

// The new look with the gaps of its reading filled from the previous one: what the previous look
// saw under a folder that could not be read now is taken as unchanged, so it is neither removed now
// nor created again later.
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
    // times longer than the looking itself, and no shorter than half a second. A small directory is
    // thus polled twice a second, and a huge tree rarely, because walking it costs; a pause with a
    // ceiling would break the tenth for a tree that takes more than a second to walk (a share with
    // a hundred thousand entries), and it would weigh on the share for the whole of a copy. A fixed
    // rate would be either too slow or too expensive.
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

            const auto rest = std::max(spent * 10, std::chrono::milliseconds{500});

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
// One watch of the disk: the folder, how deep, its number with the dispatcher, and what it heard
// that has not gone to its reader yet. Whatever is ripe goes on to the queue of the port
// (`fs::Watching::add`) at once; the rest waits for the ripener.
struct Heard : fs::Watching {
    path where;
    bool deep = false;
    std::uint64_t which = 0;

    std::mutex held;
    std::deque<Change> pending;
    std::chrono::steady_clock::time_point since{};

    ~Heard() override;
};

class Watching;

Watching& watching();

// The one thread that hands changes of content to the reader once they have settled: it sleeps
// until the earliest watch with something pending is due.
class Ripening {
public:
    // Starts the thread; it lives as long as the program.
    Ripening() : keeper_{[this] { served(); }} { keeper_.detach(); }

    Ripening(const Ripening&) = delete;
    Ripening& operator=(const Ripening&) = delete;

    // Looks at the watch again once `when` has come.
    void due(const std::shared_ptr<Heard>& heard, std::chrono::steady_clock::time_point when) {
        {
            const std::lock_guard<std::mutex> kept{guard_};

            waiting_.emplace(when, heard);
        }

        stirred_.notify_one();
    }

private:
    // Waits for the next watch that is due and hands over what has ripened.
    void served();

    std::mutex guard_;
    std::condition_variable stirred_;
    std::multimap<std::chrono::steady_clock::time_point, std::weak_ptr<Heard>> waiting_;
    std::thread keeper_;
};

// The ripener of the program, never destroyed: its thread must outlive every watch.
Ripening& ripening() {
    static auto* const made = new Ripening;

    return *made;
}

// A change of the core as the port tells it.
fs::Change told_change(const Change& one) {
    const auto kind = [&one] {
        switch (one.kind) {
            case Change::Kind::created: return fs::Change::Kind::created;
            case Change::Kind::removed: return fs::Change::Kind::removed;
            case Change::Kind::modified: return fs::Change::Kind::modified;
            case Change::Kind::renamed: return fs::Change::Kind::renamed;
            case Change::Kind::overflow: return fs::Change::Kind::overflow;
        }

        return fs::Change::Kind::overflow;
    }();

    return fs::Change{kind, one.where.empty() ? Path{} : told(one.where), one.was.empty() ? Path{} : told(one.was)};
}

// Repeated "modified" for the same path are merged: the file system produces bursts, and the
// reader looks once anyway. The first change to wait marks the moment the waiting began.
void noting(Heard& heard, Change made) {
    if (heard.pending.empty()) heard.since = std::chrono::steady_clock::now();

    if (made.kind == Change::Kind::modified) {
        std::size_t looked_back = 0;

        for (auto at = heard.pending.rbegin(); at != heard.pending.rend() && looked_back < 32; ++at, ++looked_back) {
            if (at->where != made.where) continue;

            if (at->kind == Change::Kind::modified) return;

            break;
        }
    }

    heard.pending.push_back(std::move(made));
}

// Whether the pending changes may go now: a change of what stands (created, removed, renamed, an
// overflow) goes at once, changes of content alone once the oldest is `settling` old.
bool ripe(const Heard& heard) {
    if (heard.pending.empty()) return false;

    const bool content = std::all_of(heard.pending.begin(), heard.pending.end(),
                                     [](const Change& one) { return one.kind == Change::Kind::modified; });

    return !content || std::chrono::steady_clock::now() - heard.since >= settling;
}

// Hands what is ripe to the port's queue, or asks the ripener to look again when it will be.
void delivered(const std::shared_ptr<Heard>& heard) {
    std::vector<Change> going;
    std::optional<std::chrono::steady_clock::time_point> later;

    {
        const std::lock_guard<std::mutex> kept{heard->held};

        if (ripe(*heard)) {
            going.assign(std::make_move_iterator(heard->pending.begin()), std::make_move_iterator(heard->pending.end()));
            heard->pending.clear();
        } else if (!heard->pending.empty()) {
            later = heard->since + settling;
        }
    }

    std::vector<fs::Change> told;

    for (const Change& one : going) told.push_back(told_change(one));

    heard->add(std::move(told));

    if (later) ripening().due(heard, *later);
}

// Waits for the next watch that is due and hands over what has ripened.
void Ripening::served() {
    std::unique_lock<std::mutex> kept{guard_};

    for (;;) {
        if (waiting_.empty()) {
            stirred_.wait(kept);

            continue;
        }

        const auto when = waiting_.begin()->first;

        if (std::chrono::steady_clock::now() < when) {
            stirred_.wait_until(kept, when);

            continue;
        }

        const std::shared_ptr<Heard> heard = waiting_.begin()->second.lock();

        waiting_.erase(waiting_.begin());
        kept.unlock();

        if (heard) delivered(heard);

        kept.lock();
    }
}

// The dispatcher: registers watches with the event backend where it is reliable and available,
// with polling otherwise, and passes what a path heard to its watch.
class Watching {
public:
    // Starts the polling thread and, where the build has one, the event backend.
    void begin() {
        polling_.begin();

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        prompt_.begin();
#endif
    }

    // Registers a new watch and gives its number. It goes to events when they are reliable for
    // the place and available, otherwise to polling.
    std::uint64_t add(const std::shared_ptr<Heard>& heard) {
        bool polled = asking(heard->where);
        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            which = ++next_;
        }

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        if (!polled && !prompt_.add(which, heard->where, heard->deep)) polled = true;
#endif

        if (polled) polling_.add(which, heard->where, heard->deep);

        // Only after a successful addition, so a failed one leaves no entry to remove.
        const std::lock_guard<std::mutex> kept{guard_};

        among_[which] = Kept{heard, heard->where, heard->deep};

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

    // The event backend gave up the watch: from now on it is polled. An overflow tells the reader
    // that something may have been lost while switching paths and it is worth reading again.
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

    // Gives what was heard to the watch `which`, which hands on what is ripe.
    void reached(std::uint64_t which, std::vector<Change> among) {
        std::shared_ptr<Heard> heard;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);

            if (at == among_.end()) return;

            heard = at->second.whose.lock();
        }

        if (!heard) return;

        {
            const std::lock_guard<std::mutex> kept{heard->held};

            for (Change& one : among) noting(*heard, std::move(one));
        }

        delivered(heard);
    }

private:
    // A watch as the dispatcher keeps it.
    struct Kept {
        std::weak_ptr<Heard> whose;
        path where;
        bool deep = false;
    };

    // Whether `where` must be polled: on a network share under Linux, whose changes made by another
    // machine inotify does not see. Windows hears a share from its server.
    static bool asking(const path& where) {
#if defined(TALCITE_WATCH_POLLING)
        (void)where;

        return true;
#elif defined(_WIN32)
        (void)where;

        return false;
#elif defined(TALCITE_LINUX_FS)
        return afar_at(where);
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

// The dispatcher of the program, never destroyed: it is made later than the pools, so it would
// go first while their workers still run.
Watching& watching() {
    static Watching* only = [] {
        auto* made = new Watching;

        made->begin();

        return made;
    }();

    return *only;
}

// Passes changes from a backend to the dispatcher.
void reached(std::uint64_t which, std::vector<Change> among) { watching().reached(which, std::move(among)); }

// Passes a watch that a backend gave up to the dispatcher, which moves it to polling.
void forsaken(std::uint64_t which) { watching().forsaken(which); }

// Unregisters the watch when the last holder lets it go.
Heard::~Heard() {
    if (which != 0) watching().drop(which);
}

}

namespace core {

// Watches a folder that is there, the whole tree below it when `deep`.
std::shared_ptr<fs::Watching> watched(const path& where, bool deep) {
    auto heard = std::make_shared<Heard>();

    heard->where = where;
    heard->deep = deep;
    heard->which = watching().add(heard);

    return heard;
}

}

}

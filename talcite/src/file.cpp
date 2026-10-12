#include "talcite/fs.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
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

#include "fscore.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TALCITE_LINUX_FS)

#include <linux/fs.h>
#include <sys/ioctl.h>

#endif

#endif

namespace talcite::fs {

using namespace core;

// The handle is shared, not copied: File is a value type just like Promise, so copies share the
// same open file. This lets a File pass through a promise without pointer tricks.
//
// `close()` closes the file, not just its own copy. A copy stays in every promise the File has
// passed through (the result of `open` keeps one as long as the chain lives), so closing only the
// copy would leave the handle open. On Windows, replacing a file that someone holds open is
// refused, so a later rename of that file would fail. Closing waits for calls in progress (`busy`):
// a descriptor number released under a running write would be handed by the system to someone else.
struct File::Held {
    path where;

    std::mutex guard;
    int busy = 0;
    bool shut = false;

#ifdef _WIN32
    void* handle = nullptr;

    // Closes the Windows handle, if any, and forgets it.
    void released() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);

        handle = nullptr;
    }
#else
    int handle = -1;

    // Closes the descriptor, if any, and forgets it.
    void released() {
        if (handle >= 0) ::close(handle);

        handle = -1;
    }
#endif

    // The last copy of a File closes the file.
    ~Held() { released(); }

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;
};

namespace core {

// Opens or creates the file in the given mode and returns its shared handle; throws "The file
// already exists." for an exclusive create of an existing file and "Cannot open the file." for any
// other failure.
std::shared_ptr<File::Held> open_at(const path& where, Mode how) {
    auto made = std::make_shared<File::Held>();
    made->where = where;

#ifdef _WIN32
    // Truncation goes through OPEN_ALWAYS and `SetEndOfFile`, not through CREATE_ALWAYS. The
    // latter, with the normal attribute, denies access to an existing hidden or system file, so
    // overwriting a hidden `.nfo` would fail. FILE_SHARE_DELETE allows renaming or deleting a file
    // that we are copying or hashing at the moment, instead of locking it in Explorer.
    const DWORD reach = how == Mode::read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD when = how == Mode::read ? OPEN_EXISTING : how == Mode::exclusive ? CREATE_NEW : OPEN_ALWAYS;

    made->handle = CreateFileW(longly(where).c_str(), reach, shared, nullptr, when,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    if (made->handle == INVALID_HANDLE_VALUE) {
        const DWORD why = GetLastError();

        if (how == Mode::exclusive && (why == ERROR_FILE_EXISTS || why == ERROR_ALREADY_EXISTS))
            amiss("The file already exists.", where);

        amiss("Cannot open the file.", where);
    }

    if (how == Mode::truncate && SetEndOfFile(made->handle) == 0) amiss("Cannot open the file.", where);
#else
    int flags = how == Mode::read ? O_RDONLY : O_RDWR;
    if (how == Mode::write) flags |= O_CREAT;
    if (how == Mode::truncate) flags |= O_CREAT | O_TRUNC;
    if (how == Mode::exclusive) flags |= O_CREAT | O_EXCL;

    // Closed when a child program is started; otherwise the child inherits every file we have open
    // at that moment and keeps it after our `close()`.
    made->handle = ::open(where.c_str(), flags | O_CLOEXEC, 0644);

    if (made->handle < 0) {
        if (how == Mode::exclusive && errno == EEXIST) amiss("The file already exists.", where);

        amiss("Cannot open the file.", where);
    }
#endif

    return made;
}

// Reads up to `much` bytes from offset `at`, looping over short reads; the result is shorter only
// at the end of the file. Windows reads at most 1 GiB per call.
Bytes read_at(const File::Held& held, std::uintmax_t at, std::size_t much) {
    Bytes made(much);

    std::size_t got = 0;

    while (got < much) {
#ifdef _WIN32
        OVERLAPPED told{};
        told.Offset = static_cast<DWORD>((at + got) & 0xFFFFFFFFull);
        told.OffsetHigh = static_cast<DWORD>((at + got) >> 32);

        DWORD taken = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(much - got, 1u << 30));

        if (ReadFile(held.handle, made.data() + got, wanted, &taken, &told) == 0) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;

            amiss("Cannot read the file.", held.where);
        }
#else
        const ssize_t taken = ::pread(held.handle, made.data() + got, much - got,
                                      static_cast<off_t>(at + got));

        if (taken < 0) amiss("Cannot read the file.", held.where);
#endif

        if (taken == 0) break;

        got += static_cast<std::size_t>(taken);
    }

    made.resize(got);

    return made;
}

// Writes all of `what` at offset `at`, looping over short writes, and returns the byte count.
std::size_t write_at(const File::Held& held, std::uintmax_t at, const Bytes& what) {
    std::size_t put = 0;

    while (put < what.size()) {
#ifdef _WIN32
        OVERLAPPED told{};
        told.Offset = static_cast<DWORD>((at + put) & 0xFFFFFFFFull);
        told.OffsetHigh = static_cast<DWORD>((at + put) >> 32);

        DWORD given = 0;
        const DWORD wanted =
            static_cast<DWORD>(std::min<std::size_t>(what.size() - put, 1u << 30));

        if (WriteFile(held.handle, what.data() + put, wanted, &given, &told) == 0)
            amiss("Cannot write the file.", held.where);
#else
        const ssize_t given = ::pwrite(held.handle, what.data() + put, what.size() - put,
                                       static_cast<off_t>(at + put));

        if (given < 0) amiss("Cannot write the file.", held.where);
#endif

        // A zero-byte write means no progress, not the end of the work: the caller loops on the
        // offset, so returning zero for a non-empty chunk would make copying spin on a full core.
        // We say so openly instead of staying silent.
        if (given == 0) amiss("Cannot write the file.", held.where);

        put += static_cast<std::size_t>(given);
    }

    return put;
}

// Making the data durable. A write ends in the system's memory; only this call waits until the data
// is on the medium, also on a network share, where `FlushFileBuffers` becomes a FLUSH command to
// the server. Without it a power loss right after a write can leave the file empty or old, and a
// copy moved between media could delete the source before the target really existed. On macOS
// `fsync` does not flush the disk's own cache; only F_FULLFSYNC does.
void sync_at(const File::Held& held) {
#ifdef _WIN32
    if (FlushFileBuffers(held.handle) == 0) amiss("Cannot flush the file.", held.where);
#else
#ifdef F_FULLFSYNC
    if (::fcntl(held.handle, F_FULLFSYNC) == 0) return;
#endif

    if (::fsync(held.handle) != 0) amiss("Cannot flush the file.", held.where);
#endif
}

// Sets the file's last write time, to the 100 ns tick on Windows and to the nanosecond on POSIX;
// the access time is left alone.
void stamp_on(const File::Held& held, Instant when) {
#ifdef _WIN32
    const auto ticks = std::chrono::floor<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
        when.time_since_epoch());

    const auto much = static_cast<std::uint64_t>(ticks.count() + 11644473600LL * 10000000LL);

    FILETIME told{};

    told.dwLowDateTime = static_cast<DWORD>(much & 0xFFFFFFFFull);
    told.dwHighDateTime = static_cast<DWORD>(much >> 32);

    if (SetFileTime(held.handle, nullptr, nullptr, &told) == 0) amiss("Cannot set the write time.", held.where);
#else
    const auto whole = std::chrono::floor<std::chrono::seconds>(when);

    const timespec told[2] = {{0, UTIME_OMIT},
                              {static_cast<time_t>(whole.time_since_epoch().count()),
                               static_cast<long>((when - whole).count())}};

    if (::futimens(held.handle, told) != 0) amiss("Cannot set the write time.", held.where);
#endif
}

// Reflink: the target gets the same blocks as the source, and its own only on the first write. On
// Btrfs and XFS a 40 GB copy takes a moment and uses no space. Elsewhere the system refuses
// (EOPNOTSUPP, EXDEV, EINVAL) and copying continues the ordinary way. Returns true when the clone
// succeeded.
bool cloned_on([[maybe_unused]] const File::Held& from, [[maybe_unused]] const File::Held& to) {
#if defined(TALCITE_LINUX_FS) && defined(FICLONE)
    return ::ioctl(to.handle, FICLONE, from.handle) == 0;
#else
    return false;
#endif
}

// `copy_file_range` moves the data inside the kernel, without copying through our memory; on NFS
// 4.2 and on an SMB share (cifs) the server copies on its own side and nothing goes over the
// network. An older kernel, a pair of file systems it does not support, or a file system without
// this operation says "cannot do" (ENOSYS, EXDEV, EOPNOTSUPP, EINVAL); then the result is empty and
// we read ourselves. Any other error is a real write error.
std::optional<std::size_t> spliced_on([[maybe_unused]] const File::Held& from,
                                      [[maybe_unused]] const File::Held& to, [[maybe_unused]] std::uintmax_t at,
                                      [[maybe_unused]] std::size_t much) {
#if defined(TALCITE_LINUX_FS)
    auto there = static_cast<off64_t>(at);
    auto here = static_cast<off64_t>(at);

    for (;;) {
        const ssize_t went = ::copy_file_range(from.handle, &there, to.handle, &here, much, 0);

        if (went >= 0) return static_cast<std::size_t>(went);

        if (errno == EINTR) continue;

        if (errno == ENOSYS || errno == EXDEV || errno == EOPNOTSUPP || errno == EINVAL) return std::nullopt;

        amiss("Cannot write the file.", to.where);
    }
#else
    return std::nullopt;
#endif
}

// The current size of the open file in bytes.
std::uintmax_t sized(const File::Held& held) {
#ifdef _WIN32
    LARGE_INTEGER told{};

    if (GetFileSizeEx(held.handle, &told) == 0) amiss("Cannot read the file size.",
                                                      held.where);

    return static_cast<std::uintmax_t>(told.QuadPart);
#else
    struct stat told {};

    if (::fstat(held.handle, &told) != 0) amiss("Cannot read the file size.", held.where);

    return static_cast<std::uintmax_t>(told.st_size);
#endif
}

}

namespace {

// Keeps the handle in use for the duration of one call. A closed file refuses; a close during the
// call releases the handle only when the last call leaves.
class Lease {
public:
    // Enters the call, or throws "The file is not open." when the file was closed.
    explicit Lease(File::Held& held) : held_(held) {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (held_.shut) amiss("The file is not open.", held_.where);

        ++held_.busy;
    }

    // Leaves the call; the last one out releases a handle that was closed in the meantime.
    ~Lease() {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (--held_.busy == 0 && held_.shut) held_.released();
    }

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&&) = delete;
    Lease& operator=(Lease&&) = delete;

private:
    File::Held& held_;
};

}

// Closes the file for every copy of this File. A call in progress keeps the handle until it
// finishes; this copy forgets the file at once.
void File::close() {
    if (!held_) return;

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->shut = true;

        if (held_->busy == 0) held_->released();
    }

    held_.reset();
}

// The path the file was opened with; empty for a File that holds nothing.
const path& File::where() const {
    static const path nowhere;

    return held_ ? held_->where : nowhere;
}

// Reads up to `much` bytes from offset `at` on the pool; a short result means the end of the file.
Promise<Bytes> File::read(std::uintmax_t at, std::size_t much) const {
    auto held = held_;

    return run(io(), [held, at, much]() -> Bytes {
        if (!held) amiss("The file is not open.", path{});

        Bytes made;

        {
            const Lease lease{*held};

            made = read_at(*held, at, much);
        }

        return made;
    });
}

// Writes all of `what` at offset `at` on the pool and resolves with the byte count.
Promise<std::size_t> File::write(std::uintmax_t at, Bytes what) const {
    auto held = held_;

    return run(io(), [held, at, what = std::move(what)]() -> std::size_t {
        if (!held) amiss("The file is not open.", path{});

        std::size_t made = 0;

        {
            const Lease lease{*held};

            made = write_at(*held, at, what);
        }

        return made;
    });
}

struct Cursor::Held {
    std::shared_ptr<File::Held> file;
    std::atomic<std::uintmax_t> at{0};
    std::atomic<bool> busy{false};
};

namespace {

// The busy flag is taken when the promise is created, not when the body runs. The body runs later,
// on the pool, so two reads created before either of them starts would begin at the same position,
// and which of them fails would depend on the order in the pool. The guard releases the stream when
// the operation ends (also with an exception), and when a promise let go of before its body began
// takes the body with it.
class Claim {
public:
    explicit Claim(std::shared_ptr<Cursor::Held> held) : held_(std::move(held)) {}

    // A body that never ran still releases the stream.
    ~Claim() { free(); }

    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
    Claim(Claim&&) = delete;
    Claim& operator=(Claim&&) = delete;

    // Releases the stream's busy flag; calling it again does nothing.
    void free() {
        if (!held_) return;

        held_->busy.store(false);
        held_.reset();
    }

private:
    std::shared_ptr<Cursor::Held> held_;
};

// Releases a `Claim` when the scope of an operation ends, whether normally or by an exception.
class Freeing {
public:
    explicit Freeing(std::shared_ptr<Claim> claim) : claim_(std::move(claim)) {}

    // Releases the claim when the operation leaves its scope.
    ~Freeing() { claim_->free(); }

    Freeing(const Freeing&) = delete;
    Freeing& operator=(const Freeing&) = delete;
    Freeing(Freeing&&) = delete;
    Freeing& operator=(Freeing&&) = delete;

private:
    std::shared_ptr<Claim> claim_;
};

// A function that returns a promise should not throw in the caller's face; everything that failed
// has to arrive the same way as the module's other errors. So instead of an exception we return an
// empty guard with the reason, and the caller turns it into a promise that rejects.
struct Claiming {
    std::shared_ptr<Claim> claim;
    std::string said;
    path where;
};

// Marks the stream busy; when it is closed or already busy, returns an empty claim with the reason
// and the path.
Claiming claimed(const std::shared_ptr<Cursor::Held>& held) {
    if (!held) return {nullptr, "The stream is closed.", path{}};

    bool spare = false;

    if (!held->busy.compare_exchange_strong(spare, true))
        return {nullptr, "The stream is busy.", held->file->where};

    return {std::make_shared<Claim>(held), std::string{}, path{}};
}

}

// A new sequential stream over this file, starting at offset 0; throws when the file is not open.
Cursor File::cursor() const {
    if (!held_) amiss("The file is not open.", path{});

    auto made = std::make_shared<Cursor::Held>();
    made->file = held_;

    return Cursor{made};
}

// The path of the file under the stream; empty for an empty stream.
const path& Cursor::where() const {
    static const path nowhere;

    return held_ ? held_->file->where : nowhere;
}

// The current position of the stream; 0 for an empty stream.
std::uintmax_t Cursor::at() const { return held_ ? held_->at.load() : 0; }

// Moves the stream to offset `at`; throws when the stream is closed or an operation is running.
void Cursor::seek(std::uintmax_t at) {
    if (!held_) amiss("The stream is closed.", path{});

    if (held_->busy.load()) amiss("The stream is busy.", held_->file->where);

    held_->at.store(at);
}

// Reads up to `much` bytes from the current position and advances it by what was read. One
// operation at a time: a second one rejects with "The stream is busy.". A read let go of before it
// began reads nothing; one already reading finishes, so the position moves, and then frees the
// stream.
Promise<Bytes> Cursor::read(std::size_t much) {
    auto held = held_;
    Claiming got = claimed(held);

    if (!got.claim) return failed<Bytes>(std::make_exception_ptr(Trouble{got.said, got.where}));

    return run(io(), [held, claim = std::move(got.claim), much]() -> Bytes {
        Bytes made;

        // The release must happen before the value is given, not after it: the value wakes the
        // thread waiting for it, and that thread immediately goes for the next read. Releasing
        // after it would leave a gap in which the next read of a loop is told "busy".
        {
            const Freeing freeing{claim};

            const std::uintmax_t at = held->at.load();

            const Lease lease{*held->file};

            made = read_at(*held->file, at, much);

            held->at.store(at + made.size());
        }

        return made;
    });
}

// Writes all of `what` at the current position and advances it. One operation at a time, as in
// `read`.
Promise<std::size_t> Cursor::write(Bytes what) {
    auto held = held_;
    Claiming got = claimed(held);

    if (!got.claim) return failed<std::size_t>(std::make_exception_ptr(Trouble{got.said, got.where}));

    return run(io(), [held, claim = std::move(got.claim), what = std::move(what)]() -> std::size_t {
            std::size_t much = 0;

            {
                const Freeing freeing{claim};

                const std::uintmax_t at = held->at.load();

                const Lease lease{*held->file};

                much = write_at(*held->file, at, what);

                held->at.store(at + much);
            }

            return much;
        });
}

// Waits on the pool until the written data is on the medium (see `sync_at`).
Promise<void> File::sync() const {
    auto held = held_;

    return run(io(), [held]() {
        if (!held) amiss("The file is not open.", path{});

        {
            const Lease lease{*held};

            sync_at(*held);
        }
    });
}

// The current size of the file, read on the pool.
Promise<std::uintmax_t> File::size() const {
    auto held = held_;

    return run(io(), [held]() -> std::uintmax_t {
        if (!held) amiss("The file is not open.", path{});

        std::uintmax_t made = 0;

        {
            const Lease lease{*held};

            made = sized(*held);
        }

        return made;
    });
}

// Opens the file on the pool in the given mode.
Promise<File> open(const path& where, Mode how) {
    return run(io(), [where, how]() -> File { return File{open_at(where, how)}; });
}

}

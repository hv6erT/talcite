#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "core.hpp"

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


namespace tal::platform {

// Closes the descriptor or the handle, if any, and forgets it.
void Handle::released() {
#ifdef _WIN32
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);

    handle = nullptr;
#else
    if (handle >= 0) ::close(handle);

    handle = -1;
#endif
}

// The last holder of an open file closes it.
Handle::~Handle() { released(); }

namespace core {

// Opens or creates the file in the given mode and returns its shared handle; throws "The file
// already exists." for an exclusive create of an existing file and "Cannot open the file." for any
// other failure. A file it creates gets the POSIX `rights` before its first byte, exactly, whatever
// the umask; Windows has no such rights, and a file there takes its folder's access list.
std::shared_ptr<Handle> open_at(const path& where, Mode how, std::optional<std::uint16_t> rights) {
    auto made = std::make_shared<Handle>();
    made->where = where;
    made->mode = how;

#ifdef _WIN32
    (void)rights;

    // Truncation goes through OPEN_ALWAYS and `SetEndOfFile`, not through CREATE_ALWAYS. The
    // latter, with the normal attribute, denies access to an existing hidden or system file, so
    // overwriting a hidden `.nfo` would fail. FILE_SHARE_DELETE allows renaming or deleting a file
    // that we are copying or hashing at the moment, instead of locking it in Explorer.
    const DWORD reach = how == Mode::read ? GENERIC_READ : how == Mode::append ? GENERIC_READ | FILE_APPEND_DATA : GENERIC_READ | GENERIC_WRITE;
    const DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD when = how == Mode::read ? OPEN_EXISTING : how == Mode::exclusive ? CREATE_NEW : OPEN_ALWAYS;

    made->handle = CreateFileW(longly(where).c_str(), reach, shared, nullptr, when,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    if (made->handle == INVALID_HANDLE_VALUE) {
        const DWORD why = GetLastError();

        if (how == Mode::exclusive && (why == ERROR_FILE_EXISTS || why == ERROR_ALREADY_EXISTS))
            amiss("The file already exists.", where, ::tal::Kind::exists);

        amiss("Cannot open the file.", where, std::error_code{static_cast<int>(why), std::system_category()});
    }

    if (how == Mode::truncate && SetEndOfFile(made->handle) == 0) amiss("Cannot open the file.", where);
#else
    int flags = how == Mode::read ? O_RDONLY : O_RDWR;
    if (how == Mode::write) flags |= O_CREAT;
    if (how == Mode::truncate) flags |= O_CREAT | O_TRUNC;
    if (how == Mode::exclusive) flags |= O_CREAT | O_EXCL;
    if (how == Mode::append) flags |= O_CREAT | O_APPEND;

    // Closed when a child program is started; otherwise the child inherits every file we have open
    // at that moment and keeps it after our `close()`.
    made->handle = ::open(where.c_str(), flags | O_CLOEXEC, rights ? static_cast<mode_t>(*rights) : 0644);

    if (made->handle < 0) {
        if (how == Mode::exclusive && errno == EEXIST) amiss("The file already exists.", where, ::tal::Kind::exists);

        amiss("Cannot open the file.", where);
    }

    if (rights && how == Mode::exclusive && ::fchmod(made->handle, static_cast<mode_t>(*rights)) != 0) amiss("Cannot set the rights of the file.", where);
#endif

    return made;
}

// Reads into `into` from offset `at`, looping over short reads, and returns how many bytes came;
// fewer than asked only at the end of the file. The bytes land where the caller wants them, so a
// whole file read in installments is not copied a second time.
std::size_t read_into(const Handle& held, std::uintmax_t at, std::span<std::byte> into) {
    std::size_t got = 0;

    while (got < into.size()) {
#ifdef _WIN32
        OVERLAPPED told{};
        told.Offset = static_cast<DWORD>((at + got) & 0xFFFFFFFFull);
        told.OffsetHigh = static_cast<DWORD>((at + got) >> 32);

        DWORD taken = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(into.size() - got, 1u << 30));

        if (ReadFile(held.handle, into.data() + got, wanted, &taken, &told) == 0) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;

            amiss("Cannot read the file.", held.where);
        }
#else
        const ssize_t taken = ::pread(held.handle, into.data() + got, into.size() - got, static_cast<off_t>(at + got));

        if (taken < 0) amiss("Cannot read the file.", held.where);
#endif

        if (taken == 0) break;

        got += static_cast<std::size_t>(taken);
    }

    return got;
}

// Reads up to `much` bytes from offset `at` into a new buffer; the result is shorter only at the
// end of the file.
Bytes read_at(const Handle& held, std::uintmax_t at, std::size_t much) {
    Bytes made(much);

    made.resize(read_into(held, at, made));

    return made;
}

// Writes all of `what` at offset `at`, looping over short writes, and returns the byte count.
std::size_t write_from(const Handle& held, std::uintmax_t at, std::span<const std::byte> what) {
    std::size_t put = 0;

    while (put < what.size()) {
#ifdef _WIN32
        OVERLAPPED told{};
        told.Offset = static_cast<DWORD>((at + put) & 0xFFFFFFFFull);
        told.OffsetHigh = static_cast<DWORD>((at + put) >> 32);

        DWORD given = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(what.size() - put, 1u << 30));

        if (WriteFile(held.handle, what.data() + put, wanted, &given, &told) == 0)
            amiss("Cannot write the file.", held.where);
#else
        const ssize_t given = ::pwrite(held.handle, what.data() + put, what.size() - put, static_cast<off_t>(at + put));

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

// Writes all of `what` at offset `at` and returns the byte count.
std::size_t write_at(const Handle& held, std::uintmax_t at, const Bytes& what) { return write_from(held, at, what); }

// Making the data durable. A write ends in the system's memory; only this call waits until the data
// is on the medium, also on a network share, where `FlushFileBuffers` becomes a FLUSH command to
// the server. Without it a power loss right after a write can leave the file empty or old, and a
// copy moved between media could delete the source before the target really existed. On macOS
// `fsync` does not flush the disk's own cache; only F_FULLFSYNC does.
void sync_at(const Handle& held) {
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
void stamp_on(const Handle& held, Instant when) {
#ifdef _WIN32
    const std::uint64_t much = ticks_of(when);

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
bool cloned_on([[maybe_unused]] const Handle& from, [[maybe_unused]] const Handle& to) {
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
std::optional<std::size_t> spliced_on([[maybe_unused]] const Handle& from,
                                      [[maybe_unused]] const Handle& to, [[maybe_unused]] std::uintmax_t at,
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
std::uintmax_t sized(const Handle& held) {
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

namespace {

#ifdef _WIN32

// The facet from what Windows tells about an open handle.
Facet facet_from(const BY_HANDLE_FILE_INFORMATION& told) {
    const std::uint64_t ticks = (static_cast<std::uint64_t>(told.ftLastWriteTime.dwHighDateTime) << 32) | told.ftLastWriteTime.dwLowDateTime;

    Facet made;
    made.size = (static_cast<std::uintmax_t>(told.nFileSizeHigh) << 32) | told.nFileSizeLow;
    made.changed = ticked(ticks);
    made.device = told.dwVolumeSerialNumber;
    made.node = (static_cast<std::uint64_t>(told.nFileIndexHigh) << 32) | told.nFileIndexLow;

    return made;
}

#else

// The facet from what `stat` tells.
Facet facet_from(const struct stat& told) {
#ifdef __APPLE__
    const timespec& when = told.st_mtimespec;
#else
    const timespec& when = told.st_mtim;
#endif

    Facet made;
    made.size = static_cast<std::uintmax_t>(told.st_size);
    made.changed = Instant{std::chrono::seconds{when.tv_sec} + std::chrono::nanoseconds{when.tv_nsec}};
    made.device = static_cast<std::uint64_t>(told.st_dev);
    made.node = static_cast<std::uint64_t>(told.st_ino);

    return made;
}

#endif

}

// What the open file is now: its length, its write time and which file it is on which medium.
Facet facet_of(const Handle& held) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION told{};

    if (GetFileInformationByHandle(held.handle, &told) == 0) amiss("Cannot read the file.", held.where);

    return facet_from(told);
#else
    struct stat told {};

    if (::fstat(held.handle, &told) != 0) amiss("Cannot read the file.", held.where);

    return facet_from(told);
#endif
}

// What stands at the path now, as `facet_of` tells it; nothing when nothing can be read there.
std::optional<Facet> facet_at(const path& where) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(longly(where).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

    BY_HANDLE_FILE_INFORMATION told{};

    const bool read = GetFileInformationByHandle(handle, &told) != 0;

    CloseHandle(handle);

    if (!read) return std::nullopt;

    return facet_from(told);
#else
    struct stat told {};

    if (::stat(where.c_str(), &told) != 0) return std::nullopt;

    return facet_from(told);
#endif
}

}

}

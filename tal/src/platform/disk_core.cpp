#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "tal/fs.hpp"
#include "tal/text.hpp"

#include "core.hpp"

#ifdef _WIN32

#include <windows.h>

#include <aclapi.h>
#include <shlobj.h>
#include <winnetwk.h>

#else

#include <cerrno>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TALCITE_LINUX_FS)

#include <sys/statfs.h>
#include <sys/syscall.h>

#elif defined(TALCITE_BSD_FS)

#include <sys/param.h>
#include <sys/mount.h>

#if defined(__APPLE__)
#include <cstdio>
#endif

#endif

#endif


namespace tal::platform {

using namespace core;

namespace core {

// An entry the system cannot follow exists all the same. `std::filesystem::exists` follows a
// reparse point, and when it meets a tag it does not know, it reports "not there". This is what a
// Unix socket file looks like on Windows: it is in place, GetFileAttributesW sees it, DeleteFile
// deletes it, and `exists` answers false with error 1920 (measured with a separate test program on
// Windows: attributes visible, reparse tag set, `exists` false, error 1920).
//
// So we ask about the entry itself, without following anything, but only when the first question
// reported an error. A plain missing file is not an error: `status` then answers "not there" and
// clears the error code, so this branch does not even run and costs nothing.
//
// A dangling symlink and a dangling junction still answer "not there", because GetFileAttributesW
// follows them and fails too, which is the answer we expect from `exists`.
#ifdef _WIN32

// A path too long for `std::filesystem`. In a program without longPathAware in its manifest (the
// tests, any program on the library that lacks it) or on a system without LongPathsEnabled, MSVC
// calls the system without the \\?\ prefix, and beyond 260 characters the system answers "no such
// path", without an error. `exists` and `kind` would then say "not there" about a file that is
// there, and a write with a collision policy would take it for a free spot. For such paths we ask
// ourselves, with the prefix; shorter ones are left alone. The limit of 248 is the directory limit,
// lower than the file one.
DWORD attributes_beyond(const path& where) {
    std::error_code ignored;

    const path whole = where.is_absolute() ? where : stood::absolute(where, ignored);

    if (whole.native().size() < MAX_PATH - 12) return INVALID_FILE_ATTRIBUTES;

    return GetFileAttributesW(longly(where).c_str());
}

#endif

// Whether anything exists at the path, including entries the system cannot follow and paths beyond
// the Windows length limit.
bool exists_at(const path& where) {
    std::error_code why;

    if (stood::exists(long_form(where), why)) return true;

#ifdef _WIN32
    if (why) return GetFileAttributesW(longly(where).c_str()) != INVALID_FILE_ATTRIBUTES;

    return attributes_beyond(where) != INVALID_FILE_ATTRIBUTES;
#else
    return false;
#endif
}

// The kind of the entry itself, without following a symlink; `Kind::none` when there is nothing or
// it cannot be asked about.
Kind kind_at(const path& where) {
    std::error_code trouble;

    const stood::file_status told = stood::symlink_status(long_form(where), trouble);

#ifdef _WIN32
    if (trouble || !stood::exists(told)) {
        const DWORD seen = attributes_beyond(where);

        if (seen != INVALID_FILE_ATTRIBUTES) {
            if ((seen & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return Kind::link;

            return (seen & FILE_ATTRIBUTE_DIRECTORY) != 0 ? Kind::folder : Kind::file;
        }
    }
#endif

    if (trouble) return Kind::none;

    if (stood::is_symlink(told)) return Kind::link;
    if (stood::is_regular_file(told)) return Kind::file;
    if (stood::is_directory(told)) return Kind::folder;
    if (!stood::exists(told)) return Kind::none;

    // A character or block device is `device`; a pipe and a socket are `special`. Midnight
    // Commander tells them apart the same way and paints them the same way.
    if (stood::is_character_file(told) || stood::is_block_file(told)) return Kind::device;

    return Kind::special;
}

// The size of the file in bytes; throws when it cannot be read.
std::uintmax_t size_at(const path& where) {
    std::error_code trouble;

    const std::uintmax_t told = stood::file_size(long_form(where), trouble);
    if (trouble) amiss("Cannot read the file size.", where, trouble);

    return told;
}

// On Windows the attributes are one bit word. On POSIX hiding is a naming convention (a leading
// dot), and read-only is taken from the owner's write permission. POSIX has no system flag, and we
// do not pretend it has.
Attributes attributes_at(const path& where) {
    Attributes made;

#ifdef _WIN32

    const DWORD told = GetFileAttributesW(longly(where).c_str());
    if (told == INVALID_FILE_ATTRIBUTES) amiss("Cannot read attributes.", where);

    made.hidden = (told & FILE_ATTRIBUTE_HIDDEN) != 0;
    made.readonly = (told & FILE_ATTRIBUTE_READONLY) != 0;
    made.system = (told & FILE_ATTRIBUTE_SYSTEM) != 0;

#else

    std::error_code trouble;

    const stood::file_status told = stood::status(where, trouble);
    if (trouble) amiss("Cannot read attributes.", where, trouble);

    const std::string named = where.filename().string();

    made.hidden = named.size() > 1 && named.front() == '.';
    made.readonly = (told.permissions() & stood::perms::owner_write) == stood::perms::none;

#endif

    return made;
}

// Sets the hidden, read-only and system attributes on Windows. On POSIX only read-only can be set:
// it removes every write permission, and clearing it gives the owner write permission back.
void attributed_at(const path& where, Attributes what) {
#ifdef _WIN32

    const std::wstring named = longly(where);

    DWORD told = GetFileAttributesW(named.c_str());
    if (told == INVALID_FILE_ATTRIBUTES) amiss("Cannot read attributes.", where);

    const auto sway = [&told](DWORD flag, bool on) {
        if (on) told |= flag;
        else told &= ~flag;
    };

    sway(FILE_ATTRIBUTE_HIDDEN, what.hidden);
    sway(FILE_ATTRIBUTE_READONLY, what.readonly);
    sway(FILE_ATTRIBUTE_SYSTEM, what.system);

    if (told == 0) told = FILE_ATTRIBUTE_NORMAL;

    if (SetFileAttributesW(named.c_str(), told) == 0)
        amiss("Cannot set attributes.", where);

#else

    std::error_code trouble;

    const stood::file_status told = stood::status(where, trouble);
    if (trouble) amiss("Cannot read attributes.", where, trouble);

    constexpr stood::perms writing = stood::perms::owner_write | stood::perms::group_write |
                                     stood::perms::others_write;

    stood::perms rights = told.permissions();

    if (what.readonly) rights &= ~writing;
    else rights |= stood::perms::owner_write;

    stood::permissions(where, rights, trouble);
    if (trouble) amiss("Cannot set attributes.", where, trouble);

#endif
}

// Whether both paths lead to the same file; throws when they cannot be compared.
bool equivalent_at(const path& one, const path& two) {
    std::error_code trouble;

    const bool told = stood::equivalent(long_form(one), long_form(two), trouble);
    if (trouble) amiss("Cannot compare the paths.", one, trouble);

    return told;
}

// Where a symlink points; throws "Not a link." when the entry is not one.
path target_at(const path& where) {
    std::error_code trouble;

    const path told = stood::read_symlink(long_form(where), trouble);
    if (trouble) amiss("Not a link.", where, trouble);

    return told;
}

// Creates a hard link or a symlink at `where` pointing to `onto`; a symlink to a directory is
// created as a directory symlink, which Windows distinguishes.
void linking_at(const path& where, const path& onto, bool hard) {
    std::error_code trouble;

    if (hard) stood::create_hard_link(long_form(onto), long_form(where), trouble);
    else if (kind_at(onto) == Kind::folder) stood::create_directory_symlink(onto, long_form(where), trouble);
    else stood::create_symlink(onto, long_form(where), trouble);

    if (trouble) amiss("Cannot create the link.", where, trouble);
}

#ifdef _WIN32

// Windows file time counts hundreds of nanoseconds from the year 1601. The Unix epoch is 11 644 473
// 600 seconds later. We convert directly, without a detour through the difference of clocks: here
// the exact value is known, not just the distance between two "now"s.
constexpr std::int64_t epochs_apart = 11644473600LL * 10000000LL;

// A Windows file time in its 100 ns ticks as a nanosecond instant since the Unix epoch; also for
// the times an open handle tells (`file.cpp`). Nanoseconds reach only from 1678 to 2262, so a time
// beyond (a file time of zero is the year 1601) is held at 9 200 000 000 seconds from the epoch, in
// 2261, or as far before it.
Instant ticked(std::uint64_t much) {
    constexpr std::int64_t furthest = 9'200'000'000LL * 10000000LL;
    const std::int64_t since = std::clamp(static_cast<std::int64_t>(much) - epochs_apart, -furthest, furthest);

    return Instant{std::chrono::nanoseconds{since * 100}};
}

// An instant as the ticks of a Windows file time, rounded down to the 100 ns tick.
std::uint64_t ticks_of(Instant when) {
    const auto ticks = std::chrono::floor<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(when.time_since_epoch());

    return static_cast<std::uint64_t>(ticks.count() + epochs_apart);
}

// A Windows file time as a nanosecond instant since the Unix epoch.
Instant instant_of(FILETIME told) { return ticked((static_cast<std::uint64_t>(told.dwHighDateTime) << 32) | told.dwLowDateTime); }

// An instant as a Windows file time, rounded down to the 100 ns tick.
FILETIME filetime_of(Instant when) {
    const std::uint64_t much = ticks_of(when);

    FILETIME made{};

    made.dwLowDateTime = static_cast<DWORD>(much & 0xFFFFFFFFull);
    made.dwHighDateTime = static_cast<DWORD>(much >> 32);

    return made;
}

#endif

#ifndef _WIN32

// A time of POSIX as a nanosecond instant since the epoch. Nanoseconds reach only from 1678 to
// 2262, so a time beyond, which some file systems keep, is held at 9 200 000 000 seconds from the
// epoch, in 2261, or as far before it.
Instant instant_of(const timespec& when) {
    constexpr std::int64_t furthest = 9'200'000'000LL;
    const std::int64_t seconds = std::clamp(static_cast<std::int64_t>(when.tv_sec), -furthest, furthest);

    return Instant{std::chrono::seconds{seconds} + std::chrono::nanoseconds{when.tv_nsec}};
}

#endif

// The write time straight from the system, without `std::filesystem`, which goes through the file
// clock and loses everything below a second. The precision is the system's: nanoseconds on POSIX,
// hundreds of nanoseconds on Windows, and a copy carries it over whole.
Instant instant_at(const path& where) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA told{};

    if (GetFileAttributesExW(longly(where).c_str(), GetFileExInfoStandard, &told) == 0)
        amiss("Cannot read the write time.", where);

    return instant_of(told.ftLastWriteTime);
#else
    struct stat told {};

    if (::stat(where.c_str(), &told) != 0) amiss("Cannot read the write time.", where);

#ifdef __APPLE__
    const timespec& when = told.st_mtimespec;
#else
    const timespec& when = told.st_mtim;
#endif

    return instant_of(when);
#endif
}

// The write time rounded down to whole seconds.
std::chrono::sys_seconds modified_at(const path& where) {
    return std::chrono::floor<std::chrono::seconds>(instant_at(where));
}

namespace {

// The space of the volume under `where`, a path in the long form. On Windows it comes straight from
// GetDiskFreeSpaceExW: the space of MinGW's library refuses the \\?\ form with ERROR_INVALID_NAME
// (under Wine and on Windows, 17.11), and every path here goes in that form. The function wants a
// folder with a backslash at its end, so a file is asked about through its folder.
stood::space_info spaced(const path& where, std::error_code& trouble) {
#ifdef _WIN32
    std::error_code ignored;
    const path folder = !stood::is_directory(where, ignored) && where.has_parent_path() ? where.parent_path() : where;
    std::wstring asked = folder.wstring();

    if (!asked.empty() && asked.back() != L'\\') asked += L'\\';

    ULARGE_INTEGER available{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER left{};

    if (GetDiskFreeSpaceExW(asked.c_str(), &available, &total, &left) == 0) {
        trouble = std::error_code{static_cast<int>(GetLastError()), std::system_category()};

        constexpr auto unknown = static_cast<std::uintmax_t>(-1);

        return stood::space_info{unknown, unknown, unknown};
    }

    trouble.clear();

    return stood::space_info{total.QuadPart, left.QuadPart, available.QuadPart};
#else
    return stood::space(where, trouble);
#endif
}

}

// The total and available space of the volume under the path; throws when it cannot be read.
Space space_at(const path& where) {
    std::error_code trouble;

    const stood::space_info told = spaced(long_form(where), trouble);
    if (trouble) amiss("Cannot check the free space.", where, trouble);

    return Space{told.capacity, told.available};
}

// A path starting with two slashes is a network path by definition. Asking the system about the
// kind of a UNC root is unreliable, and two things depend on this answer at once: the chunk size
// and whether notifications can be trusted. The check by definition is more certain.
[[maybe_unused]] [[nodiscard]] bool unc_at(const path& where) {
    const auto named = where.native();

    if (named.size() < 2) return false;

    using Letter = path::value_type;

    const auto slashed = [](Letter one) {
        return one == static_cast<Letter>('\\') || one == static_cast<Letter>('/');
    };

    return slashed(named[0]) && slashed(named[1]);
}

#ifdef _WIN32

// Windows breaks on paths longer than 260 characters unless they are given with the \\?\ prefix.
// The prefix in turn switches off all normalization, so the path must already be absolute and
// without dots, and a network share has its own form: \\?\UNC\server\share. `std::filesystem` under
// MSVC does this only in a program with longPathAware on a system with LongPathsEnabled; our own
// system calls have to do it explicitly, and there are many of them: opening a file, attributes,
// listing a directory and watching (`exists` and `kind`, see `attributes_beyond`).
std::wstring longly(const path& where) {
    std::error_code ignored;

    const path whole =
        (where.is_absolute() ? where : stood::absolute(where, ignored)).lexically_normal();

    std::wstring made = whole.wstring();

    if (made.rfind(L"\\\\?\\", 0) == 0 || made.rfind(L"\\\\.\\", 0) == 0) return made;

    for (wchar_t& one : made)
        if (one == L'/') one = L'\\';

    // Windows reads a drive root without a trailing backslash as "the current directory on this
    // drive".
    if (made.size() == 2 && made[1] == L':') made += L'\\';

    if (made.size() > 1 && made[0] == L'\\' && made[1] == L'\\')
        return L"\\\\?\\UNC\\" + made.substr(2);

    return L"\\\\?\\" + made;
}

#endif

// The path as a call into `std::filesystem` should get it. On Windows that is the \\?\ form
// (`longly`): the standard library hands the path to the system as it is, and without the prefix
// the system refuses anything longer than 260 characters unless the program's manifest and the
// registry both allow long paths, which neither the tests nor every program on the library can
// promise. Elsewhere it is the path itself. Every such call in the shared code of `fs` goes through
// here, which a trial checks; an empty path stays empty, since the prefix would make it the current
// folder.
path long_form(const path& where) {
#ifdef _WIN32
    if (where.empty()) return where;

    return path{longly(where)};
#else
    return where;
#endif
}

// A path the system handed back for one given through `long_form`, as the caller wrote it:
// \\?\C:\... becomes C:\..., and \\?\UNC\server\share becomes \\server\share, so that paths in
// entries and answers look the same whichever way they were asked. Any other prefixed path (a
// volume by its GUID) has no plain form and stays as it is.
path plain_form(const path& where) {
#ifdef _WIN32
    const std::wstring& named = where.native();

    if (named.rfind(L"\\\\?\\UNC\\", 0) == 0) return path{L"\\\\" + named.substr(8)};

    const bool lettered = named.size() >= 6 && named.rfind(L"\\\\?\\", 0) == 0 && std::iswalpha(named[4]) != 0 &&
                          named[5] == L':';

    if (lettered) return path{named.substr(4)};
#endif

    return where;
}

// Where a path really leads, every link on the way followed; the entry must exist. On Windows the
// system says it from an open handle (`GetFinalPathNameByHandleW`), since `std::filesystem` given a
// prefixed path does not resolve it the same way in every library; elsewhere `canonical`.
path real_at(const path& where, std::error_code& trouble) {
    trouble.clear();

#ifdef _WIN32
    const HANDLE opened = CreateFileW(longly(where).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (opened == INVALID_HANDLE_VALUE) {
        trouble = std::error_code{static_cast<int>(GetLastError()), std::system_category()};

        return {};
    }

    constexpr DWORD asked = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;

    std::wstring made(MAX_PATH, L'\0');

    DWORD much = GetFinalPathNameByHandleW(opened, made.data(), static_cast<DWORD>(made.size()), asked);

    if (much >= made.size()) {
        made.resize(much + 1);
        much = GetFinalPathNameByHandleW(opened, made.data(), static_cast<DWORD>(made.size()), asked);
    }

    const DWORD why = GetLastError();

    CloseHandle(opened);

    if (much == 0 || much >= made.size()) {
        trouble = std::error_code{static_cast<int>(why), std::system_category()};

        return {};
    }

    made.resize(much);

    return plain_form(path{made});
#else
    return stood::canonical(long_form(where), trouble);
#endif
}

#ifndef _WIN32

Mount minded(const std::string& kind);

std::string format_at(const path& where);

#endif

path present_at(const path& where);

// What kind of medium the path lies on: a fixed disk, a network share, removable, optical or other.
Mount mount_at(const path& where) {
#ifdef _WIN32

    if (unc_at(where)) return Mount::remote;

    std::error_code ignored;

    path root = stood::absolute(where, ignored).root_path();
    if (root.empty()) return Mount::other;

    std::wstring named = root.wstring();
    if (named.back() != L'\\') named += L'\\';

    switch (GetDriveTypeW(named.c_str())) {
        case DRIVE_REMOTE: return Mount::remote;
        case DRIVE_REMOVABLE: return Mount::removable;
        case DRIVE_CDROM: return Mount::optical;
        case DRIVE_FIXED: return Mount::disk;
        default: return Mount::other;
    }

#elif defined(TALCITE_LINUX_FS)

    struct statfs told {};

    const path asked = exists_at(where) ? where : where.parent_path();

    if (statfs(asked.c_str(), &told) != 0) return Mount::other;

    // NFS, SMB (old and new), CIFS, Ceph, Lustre, AFS: network; ISO 9660 and UDF: optical. FUSE is
    // only a way of mounting, so the file system name from the mount table decides.
    switch (static_cast<unsigned long>(told.f_type)) {
        case 0x6969ul:
        case 0x517Bul:
        case 0xFF534D42ul:
        case 0xFE534D42ul:
        case 0x00C36400ul:
        case 0x0BD00BD0ul:
        case 0x5346414Ful: return Mount::remote;
        case 0x65735546ul: return minded(format_at(asked));
        case 0x9660ul:
        case 0x15013346ul: return Mount::optical;
        default: return Mount::disk;
    }

#elif defined(TALCITE_BSD_FS)

    // macOS and BSD give the kind by name in `statfs` itself, without a mount table.
    return minded(format_at(exists_at(where) ? where : where.parent_path()));

#else

    // Portable POSIX has no way to ask about the kind of medium. An existing path is a disk:
    // watching goes by polling here anyway, and the disk chunk is the fastest.
    return exists_at(present_at(where)) ? Mount::disk : Mount::other;

#endif
}

// The nearest existing path: the target of a copy usually does not exist yet, and we ask about its
// medium.
path present_at(const path& where) {
    std::error_code ignored;

    path made = where.is_absolute() ? where : stood::absolute(where, ignored);

    while (!made.empty() && !exists_at(made)) {
        const path up = made.parent_path();

        if (up == made) break;

        made = up;
    }

    return made;
}

// Makes a rename inside the directory durable on POSIX by syncing the directory; on Windows
// MOVEFILE_WRITE_THROUGH does this.
void settled_folder([[maybe_unused]] const path& where) {
#ifndef _WIN32
    const int folder = ::open(where.empty() ? "." : where.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (folder < 0) return;

    // A network share and some file systems refuse to sync a directory; the rename has happened
    // anyway, so the refusal is not an error here.
    (void)::fsync(folder);
    ::close(folder);
#endif
}

// Whether the medium under the path is FAT (FAT16, FAT32); the check before a copy asks about the 4
// GiB limit.
bool fat_at(const path& where) {
    const path here = present_at(where);

#ifdef _WIN32
    std::vector<wchar_t> root(32768, L'\0');

    if (GetVolumePathNameW(longly(here).c_str(), root.data(), static_cast<DWORD>(root.size())) == 0) return false;

    std::wstring format(MAX_PATH + 1, L'\0');

    if (GetVolumeInformationW(root.data(), nullptr, 0, nullptr, nullptr, nullptr, format.data(), MAX_PATH) == 0)
        return false;

    // "FAT" and "FAT32"; exFAT has no such limit.
    return format.rfind(L"FAT", 0) == 0;
#elif defined(TALCITE_LINUX_FS)
    struct statfs told {};

    return statfs(here.c_str(), &told) == 0 && static_cast<unsigned long>(told.f_type) == 0x4d44ul;
#elif defined(TALCITE_BSD_FS)
    // "msdos" on macOS, "msdosfs" on FreeBSD.
    return format_at(here).rfind("msdos", 0) == 0;
#else
    (void)here;

    return false;
#endif
}

// Whether both paths lie on one volume; then a move is a rename and needs no space.
bool together_at(const path& one, const path& two) {
    const path left = present_at(one);
    const path right = present_at(two);

#ifdef _WIN32
    std::vector<wchar_t> first(32768, L'\0');
    std::vector<wchar_t> second(32768, L'\0');

    if (GetVolumePathNameW(longly(left).c_str(), first.data(), static_cast<DWORD>(first.size())) == 0 ||
        GetVolumePathNameW(longly(right).c_str(), second.data(), static_cast<DWORD>(second.size())) == 0)
        return false;

    return _wcsicmp(first.data(), second.data()) == 0;
#else
    struct stat here {};
    struct stat there {};

    if (::stat(left.c_str(), &here) != 0 || ::stat(right.c_str(), &there) != 0) return false;

    return here.st_dev == there.st_dev;
#endif
}

// How much free space the medium under the path (or under its nearest existing ancestor) has;
// nothing when the system does not say.
std::optional<std::uintmax_t> room_at(const path& where) {
    std::error_code trouble;

    const stood::space_info told = spaced(long_form(present_at(where)), trouble);

    if (trouble || told.available == static_cast<std::uintmax_t>(-1)) return std::nullopt;

    return told.available;
}

// The read chunk for the medium under the path: the same on a fixed disk and on a network share,
// smaller on removable and optical media. Cancellation works between chunks, not inside them.
std::size_t chunk_at(const path& where) {
    switch (mount_at(where)) {
        case Mount::remote: return afar;
        case Mount::removable:
        case Mount::optical: return astray;
        default: return on_disk;
    }
}

// The flag of the task whose work runs on this thread, set by the adapter around the work.
thread_local const std::atomic<bool>* halting = nullptr;

// Lets the work on this thread see whether its task was cancelled: the flag while the work runs,
// none afterwards.
void halted(const std::atomic<bool>* flag) { halting = flag; }

// The only place where work asks whether it is still needed: whether the task of the work running
// on this thread was cancelled. Walking a tree is an operation that takes long enough for this to
// matter.
bool stopped() { return halting != nullptr && halting->load(); }

// A place where long work on files may rest between pieces; false when the work is stopped. Tasks
// are not paused, so resting only asks.
bool rested() { return !stopped(); }

// The kind of trouble a system error is: a missing entry, refused access, an entry there already,
// one in use, a full medium, and the rest. Windows errors are recognized by their own numbers too.
::tal::Kind kind_of(const std::error_code& why) {
#ifdef _WIN32
    if (why.category() == std::system_category()) {
        switch (why.value()) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_BAD_NETPATH:
            case ERROR_BAD_NET_NAME:
            case ERROR_INVALID_DRIVE: return ::tal::Kind::missing;
            case ERROR_ACCESS_DENIED:
            case ERROR_WRITE_PROTECT:
            case ERROR_PRIVILEGE_NOT_HELD: return ::tal::Kind::denied;
            case ERROR_FILE_EXISTS:
            case ERROR_ALREADY_EXISTS: return ::tal::Kind::exists;
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
            case ERROR_BUSY: return ::tal::Kind::busy;
            case ERROR_DISK_FULL:
            case ERROR_HANDLE_DISK_FULL: return ::tal::Kind::full;
            case ERROR_INVALID_NAME:
            case ERROR_FILENAME_EXCED_RANGE: return ::tal::Kind::malformed;
            case ERROR_NOT_SUPPORTED: return ::tal::Kind::unsupported;
            case ERROR_SEM_TIMEOUT: return ::tal::Kind::overdue;
            case ERROR_BROKEN_PIPE:
            case ERROR_NO_DATA: return ::tal::Kind::broken;
            default: break;
        }
    }
#endif

    const std::error_condition plain = why.default_error_condition();

    if (plain == std::errc::no_such_file_or_directory || plain == std::errc::not_a_directory || plain == std::errc::no_such_device)
        return ::tal::Kind::missing;

    if (plain == std::errc::permission_denied || plain == std::errc::operation_not_permitted || plain == std::errc::read_only_file_system)
        return ::tal::Kind::denied;

    if (plain == std::errc::file_exists) return ::tal::Kind::exists;

    if (plain == std::errc::device_or_resource_busy || plain == std::errc::text_file_busy) return ::tal::Kind::busy;

    if (plain == std::errc::no_space_on_device || plain == std::errc::file_too_large) return ::tal::Kind::full;

    if (plain == std::errc::filename_too_long || plain == std::errc::invalid_argument || plain == std::errc::illegal_byte_sequence)
        return ::tal::Kind::malformed;

    if (plain == std::errc::not_supported || plain == std::errc::operation_not_supported || plain == std::errc::function_not_supported)
        return ::tal::Kind::unsupported;

    if (plain == std::errc::io_error || plain == std::errc::broken_pipe) return ::tal::Kind::broken;

    if (plain == std::errc::timed_out) return ::tal::Kind::overdue;

    return ::tal::Kind::other;
}

// The error the system reported last on this thread: errno, or GetLastError on Windows.
std::error_code last_error() {
#ifdef _WIN32
    return {static_cast<int>(GetLastError()), std::system_category()};
#else
    return {errno, std::generic_category()};
#endif
}

// Throws a trouble of the given kind about the path, with the sentence a person may read.
void amiss(const std::string& said, const path& where, ::tal::Kind kind) { throw Trouble{kind, said, told(where).text()}; }

// Throws a trouble about the path of the kind the system error is, with its number.
void amiss(const std::string& said, const path& where, const std::error_code& why) {
    throw Trouble{kind_of(why), said, told(where).text(), why.value()};
}

// Throws a trouble about the path of the kind of the error the system reported last; the call
// that failed must be the last one before.
void amiss(const std::string& said, const path& where) { amiss(said, where, last_error()); }

#ifndef _WIN32

// The kind of entry straight from the mode, as in stat(2): a symlink is recognized before anything
// else, because `lstat` describes the entry, not what it points to.
Kind kinded(::mode_t mode) {
    if (S_ISLNK(mode)) return Kind::link;
    if (S_ISREG(mode)) return Kind::file;
    if (S_ISDIR(mode)) return Kind::folder;
    if (S_ISCHR(mode) || S_ISBLK(mode)) return Kind::device;

    return Kind::special;
}

// Everything from one question. `std::filesystem` asks about the same things piece by piece: the
// kind, the size, the permissions, the time and whether the target exists, and each piece goes to
// the system separately (measured with strace: three stat calls per entry, 150 entries, 454 calls,
// and four with our own call for permissions and owner). One `lstat` gives it all at once.
//
// A symlink costs a second question, `stat`, that is through the link. It pays for three things:
// whether the target exists at all (`stale`), what it is (`reach`, by which the panel knows that a
// symlink to a directory can be entered), and its permissions. The flags describe what the entry
// points to (`readonly` of a link to a read-only file must be true), while `rights` describe the
// entry itself, so a symlink has its own lrwxrwxrwx.
//
// An entry that cannot be asked about comes back with `Kind::none`.
Entry noted(const stood::directory_entry& one) {
    Entry made;
    made.where = one.path();

    struct stat told {};

    if (::lstat(one.path().c_str(), &told) != 0) return made;

    made.kind = kinded(told.st_mode);
    made.reach = made.kind;

    if (made.kind == Kind::file) made.size = static_cast<std::uintmax_t>(told.st_size);

#ifdef __APPLE__
    made.modified = instant_of(told.st_mtimespec);
    made.accessed = instant_of(told.st_atimespec);
#else
    made.modified = instant_of(told.st_mtim);
    made.accessed = instant_of(told.st_atim);
#endif
    made.rights = static_cast<stood::perms>(told.st_mode & 07777);
    made.links = told.st_nlink;
    made.owner = static_cast<std::uint32_t>(told.st_uid);
    made.group = static_cast<std::uint32_t>(told.st_gid);

    const std::string named = one.path().filename().string();

    made.attributes.hidden = named.size() > 1 && named.front() == '.';

    ::mode_t mode = told.st_mode;

    if (made.kind == Kind::link) {
        std::error_code ignored;

        const path where = stood::read_symlink(one.path(), ignored);

        if (!ignored) made.target = where;

        struct stat through {};

        made.reach = Kind::none;

        if (::stat(one.path().c_str(), &through) == 0) {
            mode = through.st_mode;
            made.reach = kinded(through.st_mode);
        } else {
            made.attributes.stale = true;
        }
    }

    // Executable when any execute bit is set, as MC counts it.
    made.attributes.readonly = (mode & S_IWUSR) == 0;
    made.attributes.executable = (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;

    return made;
}

// One entry, the same way as in a listing, without reading the whole directory. An entry that does
// not exist does not exist here either: it comes back with `Kind::none`, like an entry that cannot
// be asked about.
Entry entry_at(const path& where) {
    std::error_code ignored;

    return noted(stood::directory_entry{where, ignored});
}

// The entries of one directory, without "." and "..", as the Windows `found_in` gives them: a
// directory that cannot be opened (no right to it, among others) is an error, or with `quiet` an
// empty list, for walking a tree; a listing broken off in the middle is always an error, because a
// truncated listing must not pass for a complete one. A cancelled reading gives what it got.
std::vector<Entry> found_in(const path& where, bool quiet) {
    std::vector<Entry> made;

    std::error_code trouble;
    stood::directory_iterator at{where, trouble};

    if (trouble) {
        if (quiet) return made;

        amiss("Cannot open the directory.", where, trouble);
    }

    for (const stood::directory_iterator end; !trouble && at != end; at.increment(trouble)) {
        if (stopped()) return made;

        made.push_back(noted(*at));
    }

    if (trouble) amiss("Cannot read the directory.", where, trouble);

    return made;
}

#endif

// Puts the entries in the order of `before`, which compares two of them. The sort moves numbers of
// places, and every entry moves once at the end: an entry is two paths and more, and moving it at
// every step of the sort cost more than the comparisons.
template <typename Before>
void arranged(std::vector<Entry>& among, Before before) {
    if (std::is_sorted(among.begin(), among.end(), before)) return;

    std::vector<std::size_t> order(among.size());

    for (std::size_t at = 0; at < order.size(); ++at) order[at] = at;

    std::sort(order.begin(), order.end(),
              [&among, &before](std::size_t one, std::size_t two) { return before(among[one], among[two]); });

    std::vector<Entry> made;

    made.reserve(among.size());

    for (const std::size_t at : order) made.push_back(std::move(among[at]));

    among = std::move(made);
}

// Sorts the entries by path, name by name, as a tree needs it (`walk_at`).
void sorted(std::vector<Entry>& among) {
    arranged(among, [](const Entry& one, const Entry& two) { return one.where < two.where; });
}

// Sorts the entries of one directory. They all start with the same parent, spelled the same, so
// comparing the whole native text gives the order of the names, the order the paths would give,
// without walking the parent element by element at every comparison.
void sorted_in(std::vector<Entry>& among) {
    arranged(among, [](const Entry& one, const Entry& two) { return one.where.native() < two.where.native(); });
}

#ifdef _WIN32

// The extensions Windows runs directly. PATHEXT has more, but these four are what a person calls a
// "program"; the rest (.vbs, .js, .msc) are scripts for some other engine.
bool runnable(const path& where) {
    std::wstring ending = where.extension().wstring();

    for (wchar_t& one : ending) one = static_cast<wchar_t>(std::towlower(one));

    return ending == L".exe" || ending == L".com" || ending == L".bat" || ending == L".cmd";
}

// A reparse point is not always a link. The kind is told by the tag that FIND_DATA carries in
// dwReserved0. A symlink, a mount point (junction), a WSL link and an app alias from the store are
// links. An on-demand OneDrive file, a file after server deduplication and a file from a WIM image
// are ordinary files and directories that the system reads transparently, so they keep their size.
// An AF_UNIX socket is what it is on POSIX: something special.
constexpr DWORD wsl_link = 0xA000001DUL;
constexpr DWORD app_alias = 0x8000001BUL;
constexpr DWORD unix_socket = 0x80000023UL;

// The kind of a found entry, from its attributes and reparse tag.
Kind reparsed(const WIN32_FIND_DATAW& told) {
    const bool folder = (told.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if ((told.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return folder ? Kind::folder : Kind::file;

    switch (told.dwReserved0) {
        case IO_REPARSE_TAG_SYMLINK:
        case IO_REPARSE_TAG_MOUNT_POINT:
        case wsl_link:
        case app_alias: return Kind::link;
        case unix_socket: return Kind::special;
        default: return folder ? Kind::folder : Kind::file;
    }
}

// One question to the system returns the name, kind, size, time and attributes together.
// directory_iterator does not expose attributes, so every entry would have to be asked about
// separately, which on a network share doubles the number of round trips.
Entry noted(const WIN32_FIND_DATAW& told, const path& here) {
    Entry made;

    made.where = here / path{told.cFileName};

    made.kind = reparsed(told);

    if (made.kind == Kind::file)
        made.size = (static_cast<std::uintmax_t>(told.nFileSizeHigh) << 32) | told.nFileSizeLow;

    made.modified = instant_of(told.ftLastWriteTime);
    made.accessed = instant_of(told.ftLastAccessTime);

    made.attributes.hidden = (told.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    made.attributes.readonly = (told.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    made.attributes.system = (told.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;

    // Executable by extension, since Windows has no execute bit. `stale` stays false: checking the
    // target of a reparse point is a separate trip to the system for every entry, and on a network
    // share it would double the number of trips, which is why `noted` reads from FIND_DATA.
    made.attributes.executable = made.kind == Kind::file && runnable(made.where);

    // Permissions are faked the same way `std::filesystem` does on this system: everything is
    // allowed unless the file is read-only. FIND_DATA carries no link count, owner or group, and
    // getting them would require opening every file separately; MC on Windows does not show them
    // either, so they stay empty.
    made.rights = made.attributes.readonly ? stood::perms::owner_read | stood::perms::group_read |
                                            stood::perms::others_read | stood::perms::owner_exec |
                                            stood::perms::group_exec | stood::perms::others_exec
                                      : stood::perms::all;

    // The link target as on POSIX, only for a link. `read_symlink` reads symlinks, not mount points
    // (junctions); for those the target stays empty, just as `stale` stays false, and for the same
    // reason.
    made.reach = made.kind;

    if (made.kind == Kind::link) {
        std::error_code ignored;

        const path aim = stood::read_symlink(long_form(made.where), ignored);

        if (!ignored) made.target = aim;

        const stood::file_status through = stood::status(long_form(made.where), ignored);

        made.reach = ignored ? Kind::none
                     : stood::is_directory(through)    ? Kind::folder
                     : stood::is_regular_file(through) ? Kind::file
                     : stood::exists(through)          ? Kind::special
                                                       : Kind::none;
    }

    return made;
}

// The entries of one directory, without "." and "..". `quiet` turns a directory that cannot be
// opened into an empty list instead of an error, for walking a tree.
std::vector<Entry> found_in(const path& where, bool quiet) {
    std::vector<Entry> made;

    WIN32_FIND_DATAW told{};

    const HANDLE looking =
        FindFirstFileExW((longly(where) + L"\\*").c_str(), FindExInfoBasic, &told,
                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

    if (looking == INVALID_HANDLE_VALUE) {
        if (quiet) return made;

        amiss("Cannot open the directory.", where);
    }

    do {
        const std::wstring named{told.cFileName};

        if (named == L"." || named == L"..") continue;

        if (stopped()) break;

        made.push_back(noted(told, where));
    } while (FindNextFileW(looking, &told) != 0);

    // Only ERROR_NO_MORE_FILES ends the listing. Any other error (a connection to the share broken
    // in the middle of the listing) is an error: a truncated listing must not pass for a complete
    // one, or moving a directory would delete what it did not get to see.
    const DWORD why = GetLastError();

    FindClose(looking);

    if (why != ERROR_NO_MORE_FILES && !stopped())
        amiss("Cannot read the directory.", where, std::error_code{static_cast<int>(why), std::system_category()});

    return made;
}

// One entry: the FIND_DATA of the entry itself, as in a listing, plus the link count from an open
// handle. A listing does not have it, because it would cost opening every file; for one entry it is
// one open, and without it a "write beside and rename" does not know it would break a hard link. An
// entry that does not exist does not exist here either: it comes back with `Kind::none`.
Entry entry_at(const path& where) {
    WIN32_FIND_DATAW told{};

    const HANDLE looking = FindFirstFileExW(longly(where).c_str(), FindExInfoBasic, &told,
                                            FindExSearchNameMatch, nullptr, 0);

    if (looking == INVALID_HANDLE_VALUE) {
        Entry none;

        none.where = where;

        return none;
    }

    FindClose(looking);

    Entry made = noted(told, where.parent_path());

    const HANDLE opened = CreateFileW(longly(where).c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);

    if (opened != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION about{};

        if (GetFileInformationByHandle(opened, &about) != 0) made.links = about.nNumberOfLinks;

        CloseHandle(opened);
    }

    return made;
}

#endif

// A directory at the path, or a symlink leading to a directory, but only at the root. The panel
// enters through a symlink as MC does, while `kind_at` does not expand it, because the entry of a
// link has to show the link.
namespace {

// Whether the path is a directory or a symlink to one.
bool folder_at(const path& where) {
    const Kind kind = kind_at(where);

    if (kind == Kind::folder) return true;

    if (kind != Kind::link) return false;

    std::error_code trouble;

    return stood::is_directory(stood::status(long_form(where), trouble));
}

}

// The entries of the directory (or of the directory a symlink leads to), sorted by path; throws
// "Not a directory." for anything else. A directory that cannot be opened or read to the end is a
// refusal with the place, on every system (`found_in`). Stops early when the work is cancelled.
std::vector<Entry> list_at(const path& where) {
    if (!folder_at(where)) amiss("Not a directory.", where, ::tal::Kind::other);

    std::vector<Entry> made = found_in(where, false);

    sorted_in(made);

    return made;
}

// The same reading as `list_at`, but only of a real directory: a link to one is "Not a directory."
// even at the root, because copying and moving a tree must not follow a link out of it. A
// cancelled reading gives what it got; the caller asks `stopped`.
std::vector<Entry> list_whole_at(const path& where) {
    if (kind_at(where) != Kind::folder) amiss("Not a directory.", where, ::tal::Kind::other);

    std::vector<Entry> made = found_in(where, false);

    sorted_in(made);

    return made;
}

// A directory we have no access to is skipped instead of failing the whole walk: losing the right
// to one subdirectory must not invalidate the scan of a library. A symlink is followed only at the
// root (`folder_at`); deeper down it stays an entry, because a link to an ancestor would close a
// loop.
std::vector<Entry> walk_at(const path& where) {
    if (!folder_at(where)) amiss("Not a directory.", where, ::tal::Kind::other);

    std::vector<Entry> made;
    std::vector<path> waiting{where};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        for (Entry& one : found_in(here, true)) {
            if (one.kind == Kind::folder) waiting.push_back(one.where);

            made.push_back(std::move(one));
        }

        if (stopped()) return made;
    }

    sorted(made);

    return made;
}

// Creates the directory together with any missing parents; an existing directory is fine, an
// empty path is not. The missing ones are found on the path as given and created one by one from
// the outermost, each through `long_form`: `create_directories` would walk the parents of the
// prefixed path itself, and what the parent of a \\?\ root is differs between standard libraries.
// Whatever the system enters as a folder ends the search: a folder, a link to one, and on Windows
// a junction or a mount point, which `kind_at` may read as neither. A parent that someone else
// creates in the meantime is fine too.
void make_folder_at(const path& where) {
    if (where.empty()) amiss("Cannot create the directory.", where, ::tal::Kind::malformed);

    std::vector<path> missing;

    for (path at = where; !at.empty(); at = at.parent_path()) {
        const Kind kind = kind_at(at);

        if (kind == Kind::none) {
            missing.push_back(at);

            if (at.parent_path() == at) break;

            continue;
        }

        std::error_code trouble;

        if (kind == Kind::folder || stood::is_directory(stood::status(long_form(at), trouble))) break;

        amiss("Cannot create the directory.", where, ::tal::Kind::exists);
    }

    for (auto at = missing.rbegin(); at != missing.rend(); ++at) {
        std::error_code trouble;

        if (!stood::create_directory(long_form(*at), trouble) && kind_at(*at) != Kind::folder)
            amiss("Cannot create the directory.", where, trouble);
    }
}

// Removes an empty folder and nothing else: a missing entry counts as removed, a file, a link (also
// one to a folder) and a folder with anything inside are refused, and nothing inside is touched.
// The emptiness is decided by the system in the removal itself, not by an earlier look, so an entry
// that appears in between stays and the removal is refused.
void unmade_at(const path& where) {
    const Kind what = kind_at(where);

    if (what == Kind::none) return;

    if (what != Kind::folder) amiss("Not a folder.", where, ::tal::Kind::other);

#ifdef _WIN32
    if (RemoveDirectoryW(longly(where).c_str()) != 0) return;

    const DWORD why = GetLastError();

    if (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND) return;

    if (why == ERROR_DIR_NOT_EMPTY) amiss("The folder is not empty.", where, ::tal::Kind::other);

    amiss("Cannot remove the folder.", where, std::error_code{static_cast<int>(why), std::system_category()});
#else
    if (::rmdir(where.c_str()) == 0) return;

    const int why = errno;

    if (why == ENOENT) return;

    if (why == ENOTEMPTY || why == EEXIST) amiss("The folder is not empty.", where, ::tal::Kind::other);

    if (why == ENOTDIR) amiss("Not a folder.", where, ::tal::Kind::other);

    amiss("Cannot remove the folder.", where, std::error_code{why, std::generic_category()});
#endif
}

// The name beside: target.bin → target (2).bin. Built on the native form of the path, not through
// `string()`, which under MSVC returns the ANSI code page and breaks or rejects Polish letters.
path beside(const path& to, int at) {
    const path::string_type tail = path{" (" + std::to_string(at) + ")"}.native();

    path made = to;
    made.replace_filename(to.stem().native() + tail + to.extension().native());

    return made;
}

// Where to write given the collision policy: the target itself when free or overwritten, nothing
// for skip, an error for fail, and the first free name beside it (up to 100) for rename.
path settled_on(const path& to, Conflict how) {
    if (kind_at(to) == Kind::none) return to;

    switch (how) {
        case Conflict::overwrite: return to;
        case Conflict::skip: return {};
        case Conflict::fail: amiss("The target already exists.", to, ::tal::Kind::exists);
        case Conflict::rename: break;
    }

    for (int at = 2; at <= 100; ++at) {
        const path other = beside(to, at);

        if (kind_at(other) == Kind::none) return other;
    }

    amiss("No free name beside the target.", to, ::tal::Kind::exists);
}

namespace {

#if defined(TALCITE_LINUX_FS)

// The RENAME_NOREPLACE flag of renameat2, called through syscall.
constexpr unsigned noreplace = 1U;

#endif

#ifdef _WIN32

// The same entry under a different spelling, on Windows: the same directory and a name equal
// ignoring case. Hard links in one directory differ by name also ignoring case, and in different
// directories by directory. We compare names, not the directory listing: `from` given in a spelling
// different from the one on disk leaves an entry with the spelling of `to` in the listing, although
// it is the same entry.
bool respelled(const path& from, const path& to) {
    std::error_code trouble;

    const path one = stood::absolute(from, trouble).lexically_normal();
    const path two = stood::absolute(to, trouble).lexically_normal();

    const std::wstring left = one.filename().wstring();
    const std::wstring right = two.filename().wstring();

    if (CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                             static_cast<int>(right.size()), TRUE) != CSTR_EQUAL)
        return false;

    return one.parent_path() == two.parent_path() ||
           stood::equivalent(long_form(one.parent_path()), long_form(two.parent_path()), trouble);
}

#endif

// The result of one attempt to move through the system.
enum class Push : std::uint8_t { done, taken, away, refused };

// A move that does not overwrite, atomically. Checking the target and moving separately would leave
// a window in which the target could appear, and `std::filesystem::rename` on POSIX silently
// overwrites a file, and an empty directory too. So the system itself is asked whether the target
// exists, in the same call: on Windows MoveFileExW without MOVEFILE_REPLACE_EXISTING, on Linux
// renameat2 with RENAME_NOREPLACE. A file system that does not know this flag (EINVAL) gets the
// plain way: a check and a move.
//
// A refusal has a reason, and what happens next depends on it. A taken target is decided by the
// collision policy; a different medium (EXDEV, ERROR_NOT_SAME_DEVICE) is the only reason for which
// `move` may copy and clean up the source. Any other refusal (a file open in a player, missing
// rights, a name too long) is an error.
Push pushed(const path& from, const path& to) {
#ifdef _WIN32
    // MoveFileExW without overwriting silently accepts a rename onto a second hard link of the same
    // file, although it is a separate entry. So we first ask whether the target exists (one
    // question about attributes) and report an existing one as taken. Only the same entry still
    // goes to the system: a move onto itself and a different spelling (heat → Heat), which
    // MoveFileExW does by itself.
    if (GetFileAttributesW(longly(to).c_str()) != INVALID_FILE_ATTRIBUTES && !respelled(from, to))
        return Push::taken;

    if (MoveFileExW(longly(from).c_str(), longly(to).c_str(), 0) != 0) return Push::done;

    switch (GetLastError()) {
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return Push::taken;
        case ERROR_NOT_SAME_DEVICE: return Push::away;
        default: return Push::refused;
    }
#else
#if defined(TALCITE_LINUX_FS)
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), noreplace) == 0) return Push::done;

    if (errno == EEXIST) return Push::taken;
    if (errno == EXDEV) return Push::away;
    if (errno != EINVAL && errno != ENOSYS) return Push::refused;
#else
#if defined(TALCITE_BSD_FS) && defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return Push::done;

    if (errno == EEXIST) return Push::taken;
    if (errno == EXDEV) return Push::away;
    if (errno != ENOTSUP && errno != EINVAL) return Push::refused;
#endif

    // Without a "rename without overwrite" in the system (BSD, portable POSIX), a file goes through
    // a hard link: `linkat` never overwrites (EEXIST), so the new name appears atomically and the
    // old one is removed afterwards. A directory cannot be moved this way, and a file system
    // without hard links (FAT, some FUSE) refuses; then a check and a rename remain.
    struct stat told {};

    if (::lstat(from.c_str(), &told) == 0 && !S_ISDIR(told.st_mode)) {
        if (::linkat(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0) == 0) {
            if (::unlink(from.c_str()) == 0) return Push::done;

            (void)::unlink(to.c_str());

            return Push::refused;
        }

        if (errno == EEXIST) return Push::taken;
        if (errno == EXDEV) return Push::away;
        if (errno != EPERM && errno != ENOTSUP && errno != EOPNOTSUPP && errno != EMLINK && errno != ENOSYS)
            return Push::refused;
    }
#endif

    if (kind_at(to) != Kind::none) return Push::taken;

    std::error_code trouble;

    stood::rename(from, to, trouble);

    if (!trouble) return Push::done;

    return trouble == std::errc::cross_device_link ? Push::away : Push::refused;
#endif
}

// A move that replaces the target. Writing beside and renaming onto the target is the way of every
// safe write, so the rename has to be durable: Windows writes it through the disk
// (MOVEFILE_WRITE_THROUGH), and on POSIX the directory in which the name changed is synced;
// otherwise after a power loss the directory may still point to the old file or to none.
#ifdef _WIN32

// Replacing with POSIX semantics (Windows 10 1709+): a target opened by someone else with
// FILE_SHARE_DELETE (a player, indexing, a second twa window) is replaced as on Linux, and open
// handles keep seeing the old content. MoveFileExW refuses access in that situation. A system or
// medium without this way (older Windows, FAT, some shares) answers "not supported", and then
// MoveFileExW is used. Nothing means "not supported here".
std::optional<Push> posix_replaced(const path& from, const path& to) {
    const HANDLE one = CreateFileW(longly(from).c_str(), DELETE | SYNCHRONIZE | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (one == INVALID_HANDLE_VALUE) return std::nullopt;

    const std::wstring named = longly(to);
    const std::size_t room = sizeof(FILE_RENAME_INFO) + named.size() * sizeof(wchar_t);

    std::vector<std::byte> held(room);

    auto* told = reinterpret_cast<FILE_RENAME_INFO*>(held.data());

    told->Flags = 0x00000001 | 0x00000002;
    told->RootDirectory = nullptr;
    told->FileNameLength = static_cast<DWORD>(named.size() * sizeof(wchar_t));

    std::memcpy(told->FileName, named.c_str(), named.size() * sizeof(wchar_t));

    // FileRenameInfoEx is 22; the flags are FILE_RENAME_FLAG_REPLACE_IF_EXISTS and
    // FILE_RENAME_FLAG_POSIX_SEMANTICS, written as numbers because older MinGW headers lack them.
    const BOOL done = SetFileInformationByHandle(one, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22), told,
                                                 static_cast<DWORD>(room));

    const DWORD why = done != 0 ? ERROR_SUCCESS : GetLastError();

    // Makes durable what MOVEFILE_WRITE_THROUGH would: the entry with the new name on the medium.
    if (done != 0) (void)FlushFileBuffers(one);

    CloseHandle(one);

    if (done != 0) return Push::done;

    if (why == ERROR_NOT_SAME_DEVICE) return Push::away;

    if (why == ERROR_INVALID_PARAMETER || why == ERROR_NOT_SUPPORTED || why == ERROR_INVALID_FUNCTION)
        return std::nullopt;

    return Push::refused;
}

#endif

// Moves `from` onto `to`, replacing the target durably (see above).
Push replaced(const path& from, const path& to) {
#ifdef _WIN32
    if (const std::optional<Push> made = posix_replaced(from, to)) return *made;

    if (MoveFileExW(longly(from).c_str(), longly(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return Push::done;

    return GetLastError() == ERROR_NOT_SAME_DEVICE ? Push::away : Push::refused;
#else
    if (::rename(from.c_str(), to.c_str()) != 0) return errno == EXDEV ? Push::away : Push::refused;

    settled_folder(to.parent_path());

    return Push::done;
#endif
}

// An entry does not fit inside itself. Comparing absolute paths is enough: an ancestor hidden
// behind a symlink is rejected by the system itself (EINVAL), and a refusal by the system is an
// error, not an invitation to copy. This is about a sentence that says what happened. The kind of
// the entry is not asked: a file has no inside either, and asking would cost a look at every file
// moved.
bool within(const path& from, const path& to) {
    std::error_code ignored;

    const path whole = stood::absolute(from, ignored).lexically_normal();
    const path aim = stood::absolute(to, ignored).lexically_normal();

    const auto [left, right] = std::mismatch(whole.begin(), whole.end(), aim.begin(), aim.end());

    return left == whole.end() && right != aim.end();
}

#ifndef _WIN32

// The same entry under a different spelling. On a case-insensitive system heat and Heat are one
// entry, so the target "exists", but a rename between them is an ordinary rename. A hard link is
// `equivalent` too, but it is a separate entry that must not be overwritten. The directory tells
// them apart: a separate entry lies in it under exactly that name, a different spelling of the same
// one does not.
bool same_entry(const path& from, const path& to) {
    std::error_code trouble;

    if (!stood::equivalent(from, to, trouble) || trouble) return false;

    // The same path is the same entry. The name alone is not enough: a hard link in another
    // directory under the same name is a separate entry, and `rename` between links does nothing.
    if (stood::absolute(from, trouble).lexically_normal() == stood::absolute(to, trouble).lexically_normal())
        return true;

    for (const stood::directory_entry& one : stood::directory_iterator{to.parent_path(), trouble})
        if (one.path().filename() == to.filename()) return false;

    return !trouble;
}

#endif

}

// Whether the move only changes the letter case of the name (heat → Heat) within one directory.
bool recased(const path& from, const path& to) {
    const std::string one = told(from.filename()).text();
    const std::string two = told(to.filename()).text();

    if (one == two) return false;

    std::size_t left = 0;
    std::size_t right = 0;

    while (left < one.size() && right < two.size())
        if (text::folded(text::decoded(one, left).value_or(0xFFFD)) != text::folded(text::decoded(two, right).value_or(0xFFFD))) return false;

    if (left != one.size() || right != two.size()) return false;

    std::error_code trouble;

    return stood::absolute(from, trouble).lexically_normal().parent_path() ==
           stood::absolute(to, trouble).lexically_normal().parent_path();
}

// A case change that the server accepted silently and did not perform (Samba without case
// sensitivity, some NAS devices): we check the directory listing (`exact`), and when the name is
// not there, go in two steps through a name beside it. `move` is `pushed` in a real run and a fake
// server in tests. False when the second step failed; the first one is then undone.
bool respelled_twice(const path& from, const path& to, const std::function<bool(const path&, const path&)>& move,
                     const std::function<bool(const path&)>& exact) {
    if (exact(to)) return true;

    path aside = to;

    aside += ".~case";

    if (!move(from, aside)) return false;

    if (move(aside, to)) return true;

    (void)move(aside, from);

    return false;
}

namespace {

// The directory listing has an entry under exactly this name, not under a different spelling of the
// same one.
bool spelled_exactly(const path& to) {
    std::error_code trouble;

    const path folder = to.has_parent_path() ? to.parent_path() : path{"."};

    for (const stood::directory_entry& one : stood::directory_iterator{long_form(folder), trouble})
        if (one.path().filename() == to.filename()) return true;

    return false;
}

// After a successful case-only rename: whether it really happened, and if not, a second attempt in
// two steps.
Shift confirmed(const path& from, const path& to, Shift made) {
    if (!made.done || !recased(from, to)) return made;

    const auto move = [](const path& one, const path& two) { return pushed(one, two) == Push::done; };

    if (!respelled_twice(from, to, move, spelled_exactly)) amiss("Cannot rename.", from);

    return made;
}

}

// A move with the collision policy applied. An empty `where` means skipped; `done` false with a
// non-empty `where` means only a different medium, and the caller copies. Every other refusal is
// reported here, because only a different medium allows moving by a copy.
Shift shifted(const path& from, const path& to, Conflict how) {
    if (within(from, to)) amiss("Cannot move an entry into itself.", from, ::tal::Kind::other);

    if (how == Conflict::overwrite) {
        switch (replaced(from, to)) {
            case Push::done: return {to, true};
            case Push::away: return {to, false};
            default: amiss("Cannot rename.", from);
        }
    }

    const auto settled = [&from](const path& aim) -> std::optional<Shift> {
        switch (pushed(from, aim)) {
            case Push::done: return Shift{aim, true};
            case Push::away: return Shift{aim, false};
            case Push::taken: return std::nullopt;
            case Push::refused: break;
        }

        amiss("Cannot rename.", from);
    };

    if (const std::optional<Shift> made = settled(to)) return confirmed(from, to, *made);

#ifndef _WIN32
    // On Windows the same entry is already handled by `pushed`; only a really taken target gets
    // here.
    if (same_entry(from, to)) {
        std::error_code trouble;

        stood::rename(from, to, trouble);
        if (trouble) amiss("Cannot rename.", from, trouble);

        return confirmed(from, to, {to, true});
    }
#endif

    switch (how) {
        case Conflict::skip: return {};
        case Conflict::fail: amiss("The target already exists.", to, ::tal::Kind::exists);
        case Conflict::overwrite:
        case Conflict::rename: break;
    }

    for (int at = 2; at <= 100; ++at)
        if (const std::optional<Shift> made = settled(beside(to, at))) return *made;

    amiss("No free name beside the target.", to, ::tal::Kind::exists);
}

// Replaces the target with a rename, durably; false when the system refused.
bool replaced_at(const path& from, const path& to) { return replaced(from, to) == Push::done; }

// Puts `from` under the name `to` in one step that never goes over what stands there (`pushed`);
// false when the name is taken. Throws `Trouble` for any other refusal.
bool placed_at(const path& from, const path& to) {
    switch (pushed(from, to)) {
        case Push::done: return true;
        case Push::taken: return false;
        case Push::away:
        case Push::refused: break;
    }

    amiss("Cannot put the file in its place.", to);
}

}

namespace core {

// Whether the path lies on a network share, by the same recognition of the medium that picks the
// chunk size (`mount_at`); a share has no reliable notifications.
bool afar_at(const path& where) { return mount_at(where) == Mount::remote; }

#ifdef _WIN32

// Drive letters from GetLogicalDrives, without asking the media themselves: asking for a label and
// space can wait for seconds on a disconnected network drive or an empty card reader, and the list
// has to appear at once. The details of each letter come from `volume_at`, separately, so a slow
// medium slows down only itself.
std::vector<path> roots_at() {
    std::vector<path> made;

    const DWORD mask = GetLogicalDrives();

    for (int at = 0; at < 26; ++at)
        if ((mask & (1ul << at)) != 0) made.emplace_back(std::wstring{static_cast<wchar_t>(L'A' + at)} + L":\\");

    return made;
}

// One volume: a letter or a share \\server\share. The root gets a trailing backslash, because
// without it Windows reads C: as the current directory on that drive and does not recognize a share
// at all.
Volume volume_at(const path& where) {
    Volume one;

    std::wstring root = where.wstring();

    std::replace(root.begin(), root.end(), L'/', L'\\');

    if (!root.empty() && root.back() != L'\\') root += L'\\';

    one.where = path{root};
    one.mount = mount_at(one.where);

    std::wstring label(MAX_PATH + 1, L'\0');
    std::wstring format(MAX_PATH + 1, L'\0');

    if (GetVolumeInformationW(root.c_str(), label.data(), MAX_PATH, nullptr, nullptr, nullptr, format.data(),
                              MAX_PATH) != 0) {
        label.resize(std::wcslen(label.c_str()));
        format.resize(std::wcslen(format.c_str()));

        // Through `told`, not `string()`: a label outside the ANSI code page would throw and
        // take down the whole list of volumes.
        one.label = told(path{label}).text();
        one.format = told(path{format}).text();
    }

    std::error_code ignored;

    const stood::space_info told = spaced(long_form(one.where), ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    // A mapped drive shows up as a letter, but the panel should know what stands behind it. A share
    // given directly is its own source.
    if (one.mount == Mount::remote) {
        if (root.size() == 3 && root[1] == L':') {
            std::wstring behind(1024, L'\0');
            DWORD much = static_cast<DWORD>(behind.size());

            const std::wstring named = root.substr(0, 2);

            if (WNetGetConnectionW(named.c_str(), behind.data(), &much) == NO_ERROR) {
                behind.resize(std::wcslen(behind.c_str()));
                one.source = path{behind};
            }
        } else {
            one.source = path{root.substr(0, root.size() - 1)};
        }
    }

    return one;
}

#else

// Names in /proc/mounts have spaces written in octal, because spaces separate the fields.
std::string plainly(const std::string& said) {
    std::string made;

    for (std::size_t at = 0; at < said.size(); ++at) {
        if (said[at] != '\\' || at + 3 >= said.size()) {
            made += said[at];
            continue;
        }

        const std::string trio = said.substr(at + 1, 3);

        if (trio.find_first_not_of("01234567") != std::string::npos) {
            made += said[at];
            continue;
        }

        made += static_cast<char>(std::stoi(trio, nullptr, 8));
        at += 3;
    }

    return made;
}

// Mount points under /proc, /sys, /dev and /run are the system's innards, not places to look at.
// The file system kind alone is not enough, because /sys/fs/cgroup is an ordinary tmpfs.
bool tucked(const std::string& where) {
    static const std::vector<std::string> among{"/proc", "/sys", "/dev", "/run"};

    for (const std::string& one : among)
        if (where == one || where.rfind(one + "/", 0) == 0) return true;

    return false;
}

// Whether the file system kind is a virtual one that holds no user files (proc, sysfs, cgroup and
// the like).
bool phony(const std::string& kind) {
    static const std::vector<std::string> among{
        "proc",     "sysfs",     "cgroup",   "cgroup2",     "devpts",  "securityfs",
        "debugfs",  "tracefs",   "pstore",   "bpf",         "configfs", "fusectl",
        "hugetlbfs", "mqueue",   "binfmt_misc", "autofs",   "devtmpfs", "efivarfs",
        "selinuxfs", "nsfs",     "ramfs",    "rpc_pipefs",  "squashfs"};

    return std::find(among.begin(), among.end(), kind) != among.end();
}

// FUSE may be a disk: fuseblk (ntfs-3g, exfat-fuse on a USB drive), mergerfs and bindfs combining
// local directories, encrypting overlays on a local directory. These get the disk chunk and
// notifications. An unknown fuse.* stays network: polling sees everything, only more slowly.
bool fused_locally(const std::string& kind) {
    static const std::vector<std::string> among{
        "fuseblk",       "fuse.mergerfs", "fuse.bindfs", "fuse.ntfs-3g",          "fuse.exfat",
        "fuse.encfs",    "fuse.gocryptfs", "fuse.cryfs", "fuse.fuse-overlayfs",   "fuse.unionfs",
        "fuse.squashfuse", "fuse.securefs"};

    return std::find(among.begin(), among.end(), kind) != among.end();
}

// The kind of medium from the file system name.
Mount minded(const std::string& kind) {
    if (fused_locally(kind)) return Mount::disk;

    // Names from Linux and from the `statfs` of macOS and BSD: afpfs, webdav and macfuse on the
    // network side, cd9660 and cddafs on the optical side.
    if (kind.rfind("nfs", 0) == 0 || kind == "cifs" || kind == "smbfs" || kind == "smb3" || kind == "ceph" ||
        kind == "lustre" || kind.rfind("fuse", 0) == 0 || kind == "afs" || kind == "9p" || kind == "afpfs" ||
        kind == "webdav" || kind == "macfuse" || kind == "osxfuse")
        return Mount::remote;

    if (kind == "iso9660" || kind == "udf" || kind == "cd9660" || kind == "cddafs") return Mount::optical;

    return Mount::disk;
}

#if defined(TALCITE_LINUX_FS)

// Volume labels live in /dev/disk/by-label as links to devices. There is nothing better without
// extra libraries, and it is enough for the panel to show a name instead of /dev/sdb1.
std::map<path, std::string> labelled() {
    std::map<path, std::string> made;

    std::error_code trouble;
    stood::directory_iterator at{"/dev/disk/by-label", trouble};

    if (trouble) return made;

    for (const stood::directory_entry& one : at) {
        std::error_code ignored;

        const path onto = stood::canonical(one.path(), ignored);
        if (ignored) continue;

        made[onto] = plainly(one.path().filename().string());
    }

    return made;
}

// The file system name under the path: the longest mount point from the table that is its ancestor.
std::string format_at(const path& where) {
    std::error_code ignored;

    const path here = stood::weakly_canonical(where, ignored);

    std::ifstream reading{"/proc/mounts"};
    if (!reading) return {};

    std::string made;
    std::size_t deepest = 0;
    std::string line;

    while (std::getline(reading, line)) {
        std::istringstream taking{line};

        std::string device;
        std::string mounted;
        std::string kind;

        if (!(taking >> device >> mounted >> kind)) continue;

        const path at{plainly(mounted)};

        const auto [left, right] = std::mismatch(at.begin(), at.end(), here.begin(), here.end());

        if (left != at.end()) continue;

        const auto depth = static_cast<std::size_t>(std::distance(at.begin(), at.end()));

        // A later entry at the same place covers an earlier one, so a tie goes to the last.
        if (depth >= deepest) {
            deepest = depth;
            made = kind;
        }
    }

    return made;
}

// One line of the mount table.
struct Mounted {
    std::string device;
    path where;
    std::string kind;
};

// The mount table without the system's innards. The same mount point may appear several times; the
// last one counts, because it covers the previous ones.
std::vector<Mounted> mounted_at() {
    std::vector<Mounted> made;

    std::ifstream reading{"/proc/mounts"};
    if (!reading) reading.open("/etc/mtab");
    if (!reading) return made;

    std::string line;

    while (std::getline(reading, line)) {
        std::istringstream taking{line};

        std::string device;
        std::string where;
        std::string kind;

        if (!(taking >> device >> where >> kind)) continue;

        const std::string here = plainly(where);

        if (phony(kind) || tucked(here)) continue;

        Mounted one{plainly(device), path{here}, kind};

        const auto stale = std::find_if(made.begin(), made.end(),
                                        [&one](const Mounted& mine) { return mine.where == one.where; });

        if (stale != made.end()) *stale = std::move(one);
        else made.push_back(std::move(one));
    }

    std::sort(made.begin(), made.end(), [](const Mounted& one, const Mounted& two) { return one.where < two.where; });

    return made;
}

// Only the mount points, from the table, without asking the media. A dead NFS share can hang a
// question about space forever; `volume_at` asks each one separately, so only that one hangs.
std::vector<path> roots_at() {
    std::vector<path> made;

    for (const Mounted& one : mounted_at()) made.push_back(one.where);

    return made;
}

// The volume under a mount point. A path that is not a mount point gets what can be said about it
// without the table: the kind and file system of the medium it lies on, and its own name.
Volume volume_at(const path& where) {
    Volume one;

    one.where = where;

    const std::vector<Mounted> table = mounted_at();

    const auto found =
        std::find_if(table.begin(), table.end(), [&where](const Mounted& mine) { return mine.where == where; });

    if (found != table.end()) {
        one.mount = minded(found->kind);
        one.label = found->device;
        one.format = found->kind;

        std::error_code ignored;

        const path onto = stood::canonical(one.label, ignored);

        if (!ignored) {
            const std::map<path, std::string> labels = labelled();

            const auto named = labels.find(onto);
            if (named != labels.end()) one.label = named->second;
        }

        if (one.mount == Mount::remote) one.source = path{found->device};
    } else {
        one.mount = mount_at(where);
        one.format = format_at(where);
        one.label = where.has_filename() ? where.filename().string() : where.string();
    }

    std::error_code ignored;

    const stood::space_info told = spaced(long_form(one.where), ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    return one;
}

#elif defined(TALCITE_BSD_FS)

// The file system kind straight from `statfs`: apfs, msdos, smbfs, nfs, cd9660.
std::string format_at(const path& where) {
    struct statfs told {};

    if (::statfs(present_at(where).c_str(), &told) != 0) return {};

    return std::string{told.f_fstypename};
}

// The list of volumes from `getmntinfo`: mount point, source and kind. There are no labels: macOS
// calls the mount point the label, so the directory name stays.
std::vector<Volume> mounted_at() {
    std::vector<Volume> made;

    struct statfs* among = nullptr;

    const int many = ::getmntinfo(&among, MNT_NOWAIT);

    for (int at = 0; at < many; ++at) {
        const std::string kind{among[at].f_fstypename};
        const std::string here{among[at].f_mntonname};

        if (phony(kind) || kind == "devfs" || kind == "fdescfs" || kind == "procfs" || tucked(here)) continue;

        Volume one;

        one.where = path{here};
        one.mount = minded(kind);
        one.format = kind;
        one.label = one.where == path{"/"} ? std::string{"/"} : one.where.filename().string();

        if (one.mount == Mount::remote) one.source = path{std::string{among[at].f_mntfromname}};

        one.space.total = static_cast<std::uintmax_t>(among[at].f_blocks) * among[at].f_bsize;
        one.space.free = static_cast<std::uintmax_t>(among[at].f_bavail) * among[at].f_bsize;

        made.push_back(std::move(one));
    }

    std::sort(made.begin(), made.end(),
              [](const Volume& one, const Volume& two) { return one.where < two.where; });

    return made;
}

// MNT_NOWAIT returns what the kernel already knows, without asking the media, so the list comes at
// once.
std::vector<path> roots_at() {
    std::vector<path> made;

    for (const Volume& one : mounted_at()) made.push_back(one.where);

    return made;
}

// The volume under a mount point from `getmntinfo`; any other path gets the kind, file system, name
// and space of the medium it lies on.
Volume volume_at(const path& where) {
    for (Volume& one : mounted_at())
        if (one.where == where) return std::move(one);

    Volume one;

    one.where = where;
    one.mount = mount_at(where);
    one.format = format_at(where);
    one.label = where.has_filename() ? where.filename().string() : where.string();

    std::error_code ignored;

    const stood::space_info told = spaced(long_form(one.where), ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    return one;
}

#else

// Portable POSIX: nobody knows the kind, and the list of volumes is the root alone.
std::string format_at(const path&) { return {}; }

// The only root of portable POSIX.
std::vector<path> roots_at() { return {path{"/"}}; }

// The volume at the path: its name, kind of medium and space.
Volume volume_at(const path& where) {
    Volume one;

    one.where = where;
    one.label = where.has_filename() ? where.filename().string() : where.string();
    one.mount = mount_at(where);

    std::error_code ignored;

    const stood::space_info told = spaced(long_form(one.where), ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    return one;
}

#endif

#endif

// The volume a path lies on: its root, not the path itself. `volume_at` speaks about a mount point
// or a drive letter, while a file manager asks about a file. Windows knows this by itself
// (GetVolumePathNameW, also for a share and for a directory mounted in NTFS); elsewhere it is the
// longest root from the mount table that begins the path. A path outside all roots (it should not
// happen, but the table may be incomplete) gets its own root.
path mounting_at(const path& where) {
#ifdef _WIN32
    std::wstring room(32768, L'\0');

    if (GetVolumePathNameW(longly(where).c_str(), room.data(), static_cast<DWORD>(room.size())) != 0) {
        room.resize(std::wcslen(room.c_str()));

        // The long \\?\ prefix from `longly` comes back in the answer; the root has to look as in
        // the list.
        if (room.rfind(L"\\\\?\\UNC\\", 0) == 0) room = L"\\\\" + room.substr(8);
        else if (room.rfind(L"\\\\?\\", 0) == 0) room = room.substr(4);

        return path{room};
    }

    return where.root_path();
#else
    std::error_code ignored;

    path whole = stood::weakly_canonical(where, ignored);

    if (ignored) whole = where;

    path best = whole.root_path();
    std::size_t longest = 0;

    for (const path& root : roots_at()) {
        std::size_t parts = 0;
        auto mine = whole.begin();
        bool under = true;

        for (const path& piece : root) {
            if (piece.empty()) continue;

            if (mine == whole.end() || *mine != piece) {
                under = false;

                break;
            }

            ++mine;
            ++parts;
        }

        if (under && parts > longest) {
            longest = parts;
            best = root;
        }
    }

    return best;
#endif
}

// Sets the write time through a handle, also for a directory: on Windows a directory is opened with
// FILE_FLAG_BACKUP_SEMANTICS, which `std::filesystem` from MinGW does not do, so it would not set a
// directory's time at all.
void stamp_at(const path& where, Instant when) {
#ifdef _WIN32
    const HANDLE one = CreateFileW(longly(where).c_str(), FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (one == INVALID_HANDLE_VALUE) amiss("Cannot set the write time.", where);

    const FILETIME told = filetime_of(when);
    const BOOL done = SetFileTime(one, nullptr, nullptr, &told);

    CloseHandle(one);

    if (done == 0) amiss("Cannot set the write time.", where);
#else
    const auto whole = std::chrono::floor<std::chrono::seconds>(when);

    const timespec told[2] = {{0, UTIME_OMIT},
                              {static_cast<time_t>(whole.time_since_epoch().count()),
                               static_cast<long>((when - whole).count())}};

    if (::utimensat(AT_FDCWD, where.c_str(), told, 0) != 0) amiss("Cannot set the write time.", where);
#endif
}

// The path with symlinks resolved as far as it exists; the path unchanged when that fails. On
// Windows the nearest part that exists is resolved by the system (`real_at`) and the rest joined to
// it as written.
path canonical_at(const path& where) {
#ifdef _WIN32
    std::error_code ignored;

    const path whole = (where.is_absolute() ? where : stood::absolute(where, ignored)).lexically_normal();

    path rest;

    for (path at = whole; !at.empty(); at = at.parent_path()) {
        std::error_code trouble;

        const path real = real_at(at, trouble);

        if (!trouble) return rest.empty() ? real : (real / rest).lexically_normal();

        if (at.parent_path() == at) break;

        rest = rest.empty() ? at.filename() : at.filename() / rest;
    }

    return where;
#else
    std::error_code trouble;

    const path told = stood::weakly_canonical(long_form(where), trouble);

    return trouble ? where : told;
#endif
}

}

namespace core {

// The home directory from USERPROFILE on Windows and HOME elsewhere; empty when it is not set.
path home() {
#ifdef _WIN32

    if (const wchar_t* told = _wgetenv(L"USERPROFILE")) return path{told};

#else

    if (const char* told = std::getenv("HOME")) return path{told};

#endif

    return {};
}

// The system's directory for temporary files; empty when it cannot be found.
path temp() {
    std::error_code ignored;

    return stood::temp_directory_path(ignored);
}

namespace {

#ifdef _WIN32

// The known folder IDs are written here, not taken from knownfolders.h: there they are declarations
// whose definitions MinGW keeps in a separate uuid library. The values are Windows constants; they
// do not change between releases, because programs from years ago depend on them. The cache is
// the local application data, which roaming profiles leave on the machine; the settings are the
// roaming application data, which goes with the user.
GUID known_of(fs::Known which) {
    switch (which) {
        case fs::Known::home: return {0x5e6c858f, 0x0e22, 0x4760, {0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}};
        case fs::Known::desktop: return {0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};
        case fs::Known::documents: return {0xfdd39ad0, 0x238f, 0x46af, {0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7}};
        case fs::Known::downloads: return {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};
        case fs::Known::music: return {0x4bd8d571, 0x6d19, 0x48d3, {0xbe, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0e, 0x43}};
        case fs::Known::pictures: return {0x33e28130, 0x4e1e, 0x4676, {0x83, 0x5a, 0x98, 0x39, 0x5c, 0x3b, 0xc3, 0xbb}};
        case fs::Known::temp: break;
        case fs::Known::videos: return {0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}};
        case fs::Known::cache: return {0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};
        case fs::Known::settings: return {0x3eb685db, 0x65f9, 0x4cf6, {0xa0, 0x3a, 0xe3, 0xef, 0x65, 0x72, 0x9f, 0x3d}};
    }

    return {};
}

// The path of the known folder, as the shell reports it.
std::optional<path> asked_folder(fs::Known which) {
    PWSTR told = nullptr;

    const GUID id = known_of(which);

    std::optional<path> made;

    if (SHGetKnownFolderPath(id, 0, nullptr, &told) == S_OK && told != nullptr) made = path{told};

    CoTaskMemFree(told);

    return made;
}

#else

// The variable name in user-dirs.dirs and the directory in the home directory used when the file or
// the entry is missing. macOS names videos its own way; the temporary folder and the cache have
// neither, since the caller takes the one from `temp()` and the other from `XDG_CACHE_HOME`, nor
// have the settings, from `XDG_CONFIG_HOME`.
std::pair<std::string_view, std::string_view> xdg_of(fs::Known which) {
    switch (which) {
        case fs::Known::home: return {"", ""};
        case fs::Known::desktop: return {"XDG_DESKTOP_DIR", "Desktop"};
        case fs::Known::documents: return {"XDG_DOCUMENTS_DIR", "Documents"};
        case fs::Known::downloads: return {"XDG_DOWNLOAD_DIR", "Downloads"};
        case fs::Known::music: return {"XDG_MUSIC_DIR", "Music"};
        case fs::Known::pictures: return {"XDG_PICTURES_DIR", "Pictures"};
#ifdef __APPLE__
        case fs::Known::videos: return {"XDG_VIDEOS_DIR", "Movies"};
#else
        case fs::Known::videos: return {"XDG_VIDEOS_DIR", "Videos"};
#endif
        case fs::Known::temp:
        case fs::Known::cache:
        case fs::Known::settings: break;
    }

    return {"", ""};
}

// The path without trailing slashes: /home/ala/ and /home/ala are the same directory.
std::string bared(const path& where) {
    std::string made = where.lexically_normal().string();

    while (made.size() > 1 && made.back() == '/') made.pop_back();

    return made;
}

// An entry from user-dirs.dirs as xdg-user-dirs writes it: XDG_DOWNLOAD_DIR="$HOME/Pobrane", in
// quotes, an absolute path or one starting at $HOME. An entry equal to the home directory itself
// means "disabled": then the folder does not exist and no substitute is looked for. A missing entry
// and a malformed one (a relative path) are something else: nobody said anything about the folder,
// and the customary name stays. Hence two levels: the outer one says whether there was an entry.
std::optional<std::optional<path>> configured(std::string_view key, const path& house) {
    const char* given = std::getenv("XDG_CONFIG_HOME");

    const path where = (given != nullptr && *given == '/' ? path{given} : house / ".config") / "user-dirs.dirs";

    std::ifstream reading{where};

    std::string line;

    while (std::getline(reading, line)) {
        if (line.rfind(std::string{key} + "=", 0) != 0) continue;

        std::string told = line.substr(key.size() + 1);

        if (told.size() >= 2 && told.front() == '"' && told.back() == '"') told = told.substr(1, told.size() - 2);

        std::string made;

        if (told.rfind("$HOME", 0) == 0) made = bared(house) + told.substr(5);
        else if (!told.empty() && told.front() == '/') made = told;
        else continue;

        if (bared(path{made}) == bared(house)) return std::optional<path>{};

        return std::optional<path>{path{bared(path{made})}};
    }

    return std::nullopt;
}

// The known folder from user-dirs.dirs, or the customary name in the home directory when the file
// says nothing; nothing when the folder is disabled or there is no home directory. The cache is
// `XDG_CACHE_HOME` when it is absolute, otherwise ".cache" in the home directory (on macOS
// "Library/Caches"); the settings `XDG_CONFIG_HOME` when it is absolute, otherwise ".config" (on
// macOS "Library/Application Support").
std::optional<path> asked_folder(fs::Known which) {
    const path house = home();

    if (house.empty()) return std::nullopt;

    if (which == fs::Known::home) return house;

    if (which == fs::Known::cache) {
#ifdef __APPLE__
        return house / "Library" / "Caches";
#else
        const char* given = std::getenv("XDG_CACHE_HOME");

        return given != nullptr && *given == '/' ? path{given} : house / ".cache";
#endif
    }

    if (which == fs::Known::settings) {
#ifdef __APPLE__
        return house / "Library" / "Application Support";
#else
        const char* given = std::getenv("XDG_CONFIG_HOME");

        return given != nullptr && *given == '/' ? path{given} : house / ".config";
#endif
    }

    const auto [key, fallback] = xdg_of(which);

    if (std::optional<std::optional<path>> made = configured(key, house)) return *made;

    return house / std::string{fallback};
}

#endif

}

// A known folder only when it really is on disk: an entry in the system settings whose directory
// nobody created is not a place one can enter. The folder for temporary files is the system's.
std::optional<path> known_at(fs::Known which) {
    std::optional<path> made = which == fs::Known::temp ? std::optional<path>{temp()} : asked_folder(which);
    std::error_code ignored;

    if (made && (made->empty() || !stood::is_directory(long_form(*made), ignored))) made.reset();

    return made;
}

}

}

namespace tal::platform::core {

#ifdef _WIN32

// A path of Windows as the bytes of WTF-8: UTF-8 that also writes a lone surrogate, as NTFS allows
// one in a name, so that the stored form keeps every name whole. Shown, such a surrogate is the
// replacement character (`Path::text`).
Path told(const path& native) {
    const std::wstring& units = native.native();
    std::string made;

    made.reserve(units.size());

    for (std::size_t at = 0; at < units.size(); ++at) {
        char32_t one = static_cast<char32_t>(static_cast<std::uint16_t>(units[at]));

        if (one >= 0xD800 && one <= 0xDBFF && at + 1 < units.size()) {
            const auto low = static_cast<char32_t>(static_cast<std::uint16_t>(units[at + 1]));

            if (low >= 0xDC00 && low <= 0xDFFF) {
                one = 0x10000 + ((one - 0xD800) << 10U) + (low - 0xDC00);
                ++at;
            }
        }

        text::encoded(made, one);
    }

    return Path::restored(made);
}

// A path of Windows from the stored form of `Path`, the surrogates of WTF-8 coming back as the
// lone units they were.
path native(const Path& given) {
    const std::string_view stored = given.stored();
    std::wstring made;

    made.reserve(stored.size());

    for (std::size_t at = 0; at < stored.size();) {
        const auto lead = static_cast<unsigned char>(stored[at]);

        if (lead == 0xED && at + 2 < stored.size() && (static_cast<unsigned char>(stored[at + 1]) & 0xE0U) == 0xA0U) {
            const char32_t unit = 0xD000U | ((static_cast<unsigned char>(stored[at + 1]) & 0x3FU) << 6U) |
                                  (static_cast<unsigned char>(stored[at + 2]) & 0x3FU);

            made.push_back(static_cast<wchar_t>(unit));
            at += 3;

            continue;
        }

        const char32_t one = text::decoded(stored, at).value_or(0xFFFD);

        if (one >= 0x10000) {
            made.push_back(static_cast<wchar_t>(0xD800 + ((one - 0x10000) >> 10U)));
            made.push_back(static_cast<wchar_t>(0xDC00 + ((one - 0x10000) & 0x3FFU)));
        } else {
            made.push_back(static_cast<wchar_t>(one == U'/' ? U'\\' : one));
        }
    }

    return path{made};
}

#else

// A path of POSIX as the bytes it is.
Path told(const path& native) { return Path::restored(native.native()); }

// A path of POSIX from the bytes `Path` keeps.
path native(const Path& given) { return path{std::string{given.stored()}}; }

#endif

}

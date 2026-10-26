#pragma once

#include <cstddef>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "talcite/fs.hpp"

// Which file-system path to take. Linux has its own: `statfs` with a type magic, /proc/mounts,
// renameat2, inotify, `copy_file_range`. macOS and the BSDs have `statfs` with a type name and
// `getmntinfo`. The rest of POSIX gets the portable path, poor but real. `TALCITE_GENERIC_POSIX`
// forces it on Linux too, like `TALCITE_POLL` does for networking, so that it can be tested where
// something better happens to be available.
#if !defined(_WIN32) && !defined(TALCITE_GENERIC_POSIX)
#if defined(__linux__)
#define TALCITE_LINUX_FS 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define TALCITE_BSD_FS 1
#endif
#endif

namespace talcite::fs::core {

// The shared core of `fs`: what all four files need, namely path queries (`fs.cpp`), files and
// cursors (`file.cpp`), copying (`copy.cpp`) and watching (`watch.cpp`). The synchronous *_at
// functions live in `core`, and each file calls them unqualified through a using-directive for
// `core`.

namespace stood = std::filesystem;

// Read chunk sizes by medium: local disk, network share, everything else.
inline constexpr std::size_t on_disk = 1024 * 1024;
inline constexpr std::size_t afar = 64 * 1024;
inline constexpr std::size_t astray = 256 * 1024;

// Throws an `fs` `Trouble` with the given message and the path it concerns.
[[noreturn]] inline void amiss(const std::string& said, const path& where) {
    throw Trouble{said, where};
}

// Synchronous functions called from more than one file.
[[nodiscard]] bool afar_at(const path& where);

[[nodiscard]] path canonical_at(const path& where);

[[nodiscard]] std::size_t chunk_at(const path& where);

[[nodiscard]] bool equivalent_at(const path& one, const path& two);

[[nodiscard]] bool exists_at(const path& where);

[[nodiscard]] Kind kind_at(const path& where);

[[nodiscard]] std::vector<Entry> list_at(const path& where);

// The entries of a directory read in full, or a refusal; never a part passed off as the whole.
[[nodiscard]] std::vector<Entry> list_whole_at(const path& where);

void make_folder_at(const path& where);

void unmade_at(const path& where);

[[nodiscard]] std::chrono::sys_seconds modified_at(const path& where);

// Modification time at the system's full precision: nanoseconds on POSIX, hundreds of
// nanoseconds on Windows. A copy carries it over whole; going through seconds from
// `std::filesystem` and converting the file clock would lose the fraction and can be off by one
// second at a boundary.
using Instant = std::chrono::sys_time<std::chrono::nanoseconds>;

[[nodiscard]] Instant instant_at(const path& where);

// Sets the modification time through a handle, file or folder alike; on Windows a folder is
// opened with `FILE_FLAG_BACKUP_SEMANTICS`, which MinGW's `std::filesystem` does not do.
void stamp_on(const File::Held& held, Instant when);

[[nodiscard]] std::shared_ptr<File::Held> open_at(const path& where, Mode how);

[[nodiscard]] Bytes read_at(const File::Held& held, std::uintmax_t at, std::size_t much);

// Reading into, and writing from, memory the caller already has, without a buffer per installment.
[[nodiscard]] std::size_t read_into(const File::Held& held, std::uintmax_t at, std::span<std::byte> into);

[[nodiscard]] std::size_t write_from(const File::Held& held, std::uintmax_t at, std::span<const std::byte> what);

[[nodiscard]] path beside(const path& to, int at);

[[nodiscard]] path settled_on(const path& to, Conflict how);

// The result of a move with collision resolution: an empty `where` means skipped (skip); a false
// `done` with a non-empty `where` means only one thing, a different medium. `shifted` reports every
// other refusal itself, because only a different medium allows moving by copying.
struct Shift {
    path where;
    bool done = false;
};

[[nodiscard]] Shift shifted(const path& from, const path& to, Conflict how);

// A change of letter case only (heat -> Heat) within one folder.
[[nodiscard]] bool recased(const path& from, const path& to);

// A case change that the server accepted silently but did not perform (case-insensitive Samba,
// some NAS devices): we check the folder listing (`exact`), and when the name is not there, take
// two steps through a name beside it. The move (`move`) is `pushed` in a real run and a fake
// server in trials. False when the second step failed (the first is then undone).
[[nodiscard]] bool respelled_twice(const path& from, const path& to,
                                   const std::function<bool(const path&, const path&)>& move,
                                   const std::function<bool(const path&)>& exact);

// Replaces the target by renaming, durably; false when the system refused.
[[nodiscard]] bool replaced_at(const path& from, const path& to);

// A free name beside the target for writing a replacement before it takes the target's place.
[[nodiscard]] path aside_of(const path& to);

[[nodiscard]] std::uintmax_t sized(const File::Held& held);

// What tells one state of a file from another: its length, its write time at full precision, and
// which file it is on which medium (device and inode on POSIX, volume and file index on Windows).
struct Facet {
    std::uintmax_t size = 0;
    Instant changed{};
    std::uint64_t device = 0;
    std::uint64_t node = 0;

    friend bool operator==(const Facet&, const Facet&) = default;
};

[[nodiscard]] Facet facet_of(const File::Held& held);

[[nodiscard]] std::optional<Facet> facet_at(const path& where);

void sync_at(const File::Held& held);

// Makes a folder durable after a rename inside it (POSIX); on Windows `MOVEFILE_WRITE_THROUGH`
// does this.
void settled_folder(const path& where);

// Whether the medium under the path is FAT (FAT16, FAT32); the check before a copy asks about the
// 4 GiB limit.
[[nodiscard]] bool fat_at(const path& where);

// Whether both paths lie on one medium; then a move is a rename and needs no free space.
[[nodiscard]] bool together_at(const path& one, const path& two);

// How much free space the medium under the path (or under its nearest existing ancestor) has;
// nothing when the system does not tell.
[[nodiscard]] std::optional<std::uintmax_t> room_at(const path& where);

void stamp_at(const path& where, Instant when);

// File content copied by the system, without passing through our memory. `cloned_on` shares
// blocks (reflink on Btrfs and XFS) and returns true when the whole content is already there;
// `spliced_on` moves a piece starting at `at` in the kernel (on NFS 4.2 and SMB as a server-side
// copy) and returns how much went through, or nothing when the system cannot do it for this pair
// and we have to read and write ourselves.
[[nodiscard]] bool cloned_on(const File::Held& from, const File::Held& to);

[[nodiscard]] std::optional<std::size_t> spliced_on(const File::Held& from, const File::Held& to,
                                                    std::uintmax_t at, std::size_t much);

[[nodiscard]] bool stopped();

[[nodiscard]] bool rested();

[[nodiscard]] std::vector<Entry> walk_at(const path& where);

[[nodiscard]] std::size_t write_at(const File::Held& held, std::uintmax_t at, const Bytes& what);

// The path for a call into `std::filesystem` (the \\?\ form on Windows), and back.
[[nodiscard]] path long_form(const path& where);

[[nodiscard]] path plain_form(const path& where);

// Where a path really leads, every link followed; the entry must exist.
[[nodiscard]] path real_at(const path& where, std::error_code& trouble);

#ifdef _WIN32

[[nodiscard]] std::wstring longly(const path& where);

#endif

}

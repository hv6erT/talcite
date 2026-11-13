#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/fs.hpp"
#include "tal/progress.hpp"
#include "tal/path.hpp"
#include "tal/trouble.hpp"

#if !defined(_WIN32) && !defined(TALCITE_GENERIC_POSIX)
#if defined(__linux__)
#define TALCITE_LINUX_FS 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define TALCITE_BSD_FS 1
#endif
#endif

namespace tal::platform {

namespace stood = std::filesystem;

using path = std::filesystem::path;

enum class Kind : std::uint8_t { none, file, folder, link, device, special };

struct Space {
    std::uintmax_t total = 0;
    std::uintmax_t free = 0;
};

enum class Mount : std::uint8_t { disk, remote, removable, optical, other };

struct Volume {
    path where;
    path source;
    std::string label;
    std::string format;
    Mount mount = Mount::other;
    Space space;
};

struct Attributes {
    bool hidden = false;
    bool readonly = false;
    bool system = false;
    bool executable = false;
    bool stale = false;
};

struct Entry {
    path where;
    std::optional<path> target;
    Kind kind = Kind::none;
    Kind reach = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds modified{};
    std::chrono::sys_seconds accessed{};
    Attributes attributes;
    std::filesystem::perms rights = std::filesystem::perms::unknown;
    std::uintmax_t links = 1;
    std::optional<std::uint32_t> owner;
    std::optional<std::uint32_t> group;
};

enum class Conflict : std::uint8_t { overwrite, skip, rename, fail };

enum class Mode : std::uint8_t { read, write, truncate, exclusive, append };

struct Change {
    enum class Kind : std::uint8_t { created, removed, modified, renamed, overflow };

    path where;
    path was;
    Kind kind = Kind::modified;
};

struct Handle {
    path where;
    Mode mode = Mode::read;
    std::mutex guard;
    int busy = 0;
    bool shut = false;
#ifdef _WIN32
    void* handle = nullptr;
#else
    int handle = -1;
#endif

    Handle() = default;
    ~Handle();

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    void released();
};

class Bell;

namespace core {

[[nodiscard]] ::tal::Kind kind_of(const std::error_code& why);

[[nodiscard]] std::error_code last_error();

[[noreturn]] void amiss(const std::string& said, const path& where, ::tal::Kind kind);

[[noreturn]] void amiss(const std::string& said, const path& where, const std::error_code& why);

[[noreturn]] void amiss(const std::string& said, const path& where);

[[nodiscard]] Path told(const path& native);

[[nodiscard]] path native(const Path& given);

void halted(const std::atomic<bool>* flag);

[[nodiscard]] Bell* ringing();

[[nodiscard]] Volume volume_at(const path& where);

[[nodiscard]] std::vector<path> roots_at();

[[nodiscard]] Attributes attributes_at(const path& where);

void attributed_at(const path& where, Attributes what);

void linking_at(const path& where, const path& onto, bool hard);

[[nodiscard]] Entry entry_at(const path& where);

[[nodiscard]] Kind kind_at(const path& where);

void unmade_at(const path& where);

[[nodiscard]] path target_at(const path& where);

[[nodiscard]] std::optional<path> known_at(fs::Known which);

[[nodiscard]] std::shared_ptr<fs::Watching> watched(const path& where, bool deep);

void trashed_at(const path& where);

[[nodiscard]] Bytes read_whole(const path& where);

[[nodiscard]] bool write_whole(const path& where, const Bytes& what, Conflict how, std::optional<std::uint16_t> rights = std::nullopt);

void copy_whole(const path& from, const path& to, Progress* progress, Conflict how);

void move_whole(const path& from, const path& to, Conflict how);

void erase_whole(const path& where);

void make_folder_at(const path& where);

[[nodiscard]] std::vector<Entry> list_whole_at(const path& where);

[[nodiscard]] std::vector<Entry> walk_at(const path& where);

[[nodiscard]] path canonical_at(const path& where);

[[nodiscard]] std::chrono::sys_seconds modified_at(const path& where);

[[nodiscard]] Space space_at(const path& where);

[[nodiscard]] Mount mount_at(const path& where);

[[nodiscard]] path mounting_at(const path& where);

[[nodiscard]] std::uintmax_t size_at(const path& where);

inline constexpr std::size_t on_disk = 1024 * 1024;
inline constexpr std::size_t afar = 1024 * 1024;
inline constexpr std::size_t astray = 256 * 1024;

[[nodiscard]] bool afar_at(const path& where);

[[nodiscard]] path canonical_at(const path& where);

[[nodiscard]] std::size_t chunk_at(const path& where);

[[nodiscard]] bool equivalent_at(const path& one, const path& two);

[[nodiscard]] bool exists_at(const path& where);

[[nodiscard]] Kind kind_at(const path& where);

[[nodiscard]] std::vector<Entry> list_at(const path& where);

[[nodiscard]] std::vector<Entry> list_whole_at(const path& where);

void make_folder_at(const path& where);

void unmade_at(const path& where);

[[nodiscard]] std::chrono::sys_seconds modified_at(const path& where);

using Instant = std::chrono::sys_time<std::chrono::nanoseconds>;

[[nodiscard]] Instant instant_at(const path& where);

void stamp_on(const Handle& held, Instant when);

[[nodiscard]] std::shared_ptr<Handle> open_at(const path& where, Mode how, std::optional<std::uint16_t> rights = std::nullopt);

[[nodiscard]] Bytes read_at(const Handle& held, std::uintmax_t at, std::size_t much);

[[nodiscard]] std::size_t read_into(const Handle& held, std::uintmax_t at, std::span<std::byte> into);

[[nodiscard]] std::size_t write_from(const Handle& held, std::uintmax_t at, std::span<const std::byte> what);

[[nodiscard]] path beside(const path& to, int at);

[[nodiscard]] path settled_on(const path& to, Conflict how);

struct Shift {
    path where;
    bool done = false;
};

[[nodiscard]] Shift shifted(const path& from, const path& to, Conflict how);

[[nodiscard]] bool recased(const path& from, const path& to);

[[nodiscard]] bool respelled_twice(const path& from, const path& to,
                                   const std::function<bool(const path&, const path&)>& move,
                                   const std::function<bool(const path&)>& exact);

[[nodiscard]] bool replaced_at(const path& from, const path& to);

[[nodiscard]] path aside_of(const path& to);

[[nodiscard]] std::uintmax_t sized(const Handle& held);

struct Facet {
    std::uintmax_t size = 0;
    Instant changed{};
    std::uint64_t device = 0;
    std::uint64_t node = 0;

    friend bool operator==(const Facet&, const Facet&) = default;
};

[[nodiscard]] Facet facet_of(const Handle& held);

[[nodiscard]] std::optional<Facet> facet_at(const path& where);

void sync_at(const Handle& held);

void settled_folder(const path& where);

[[nodiscard]] bool fat_at(const path& where);

[[nodiscard]] bool together_at(const path& one, const path& two);

[[nodiscard]] std::optional<std::uintmax_t> room_at(const path& where);

void stamp_at(const path& where, Instant when);

[[nodiscard]] bool cloned_on(const Handle& from, const Handle& to);

[[nodiscard]] std::optional<std::size_t> spliced_on(const Handle& from, const Handle& to,
                                                    std::uintmax_t at, std::size_t much);

[[nodiscard]] bool stopped();

[[nodiscard]] bool rested();

[[nodiscard]] std::vector<Entry> walk_at(const path& where);

[[nodiscard]] std::size_t write_at(const Handle& held, std::uintmax_t at, const Bytes& what);

[[nodiscard]] path long_form(const path& where);

[[nodiscard]] path plain_form(const path& where);

[[nodiscard]] path real_at(const path& where, std::error_code& trouble);

#ifdef _WIN32

[[nodiscard]] std::wstring longly(const path& where);

[[nodiscard]] Instant ticked(std::uint64_t much);

[[nodiscard]] std::uint64_t ticks_of(Instant when);

#endif

}

}

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/promise.hpp"
#include "talcite/trouble.hpp"

namespace talcite::fs {

using path = std::filesystem::path;

using talcite::Bytes;

enum class Kind : std::uint8_t { none, file, folder, link, device, special };

struct Space {
    std::uintmax_t total = 0;
    std::uintmax_t free = 0;
};

enum class Mount : std::uint8_t { disk, remote, removable, optical, other };

enum class Known : std::uint8_t { home, desktop, documents, downloads, music, pictures, videos };

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

struct Change {
    enum class Kind : std::uint8_t { created, removed, modified, renamed, overflow };

    path where;
    path was;
    Kind kind = Kind::modified;
};

class Trouble : public talcite::Trouble {
public:
    Trouble(const std::string& said, path where);

    [[nodiscard]] const path& where() const { return where_; }

private:
    path where_;
};

[[nodiscard]] std::string to_string(const path& where);

[[nodiscard]] path from_string(std::string_view said);

[[nodiscard]] std::string stored(const path& where);

[[nodiscard]] path restored(std::string_view said);

[[nodiscard]] Promise<bool> exists(const path& where);

[[nodiscard]] Promise<Kind> kind(const path& where);

[[nodiscard]] Promise<std::uintmax_t> size(const path& where);

[[nodiscard]] Promise<std::chrono::sys_seconds> modified(const path& where);

[[nodiscard]] Promise<void> modified(const path& where, std::chrono::sys_seconds when);

[[nodiscard]] Promise<Space> space(const path& where);

[[nodiscard]] Promise<std::size_t> chunk(const path& where);

[[nodiscard]] Promise<Mount> mount(const path& where);

[[nodiscard]] Promise<std::vector<path>> roots();

[[nodiscard]] Promise<Volume> volume(const path& where);

[[nodiscard]] Promise<path> mounted(const path& where);

[[nodiscard]] Promise<Attributes> attributes(const path& where);

[[nodiscard]] Promise<std::string> owner(std::uint32_t which);

[[nodiscard]] Promise<std::string> group(std::uint32_t which);

[[nodiscard]] Promise<void> attributes(const path& where, Attributes what);

[[nodiscard]] Promise<Entry> entry(const path& where);

[[nodiscard]] Promise<void> rights(const path& where, std::filesystem::perms what);

[[nodiscard]] Promise<bool> equivalent(const path& one, const path& two);

[[nodiscard]] Promise<path> target(const path& where);

[[nodiscard]] Promise<path> canonical(const path& where);

[[nodiscard]] Promise<void> link(const path& where, const path& onto, bool hard = false);

[[nodiscard]] path current();

[[nodiscard]] Promise<void> current(const path& where);

[[nodiscard]] path home();

[[nodiscard]] path temp();

[[nodiscard]] Promise<path> runtime();

[[nodiscard]] Promise<std::optional<path>> folder(Known which);

[[nodiscard]] Promise<void> make_folder(const path& where);

[[nodiscard]] Promise<void> unmade(const path& where);

[[nodiscard]] Promise<void> rename(const path& from, const path& to,
                                   Conflict how = Conflict::fail);

[[nodiscard]] Promise<std::vector<Entry>> list(const path& where);

[[nodiscard]] Promise<std::vector<Entry>> walk(const path& where);

[[nodiscard]] Promise<Bytes> read(const path& where);

[[nodiscard]] Promise<void> write(const path& where, Bytes what,
                                  Conflict how = Conflict::overwrite);

[[nodiscard]] Promise<void> copy(const path& from, const path& to,
                                 Conflict how = Conflict::fail);

[[nodiscard]] Promise<void> move(const path& from, const path& to,
                                 Conflict how = Conflict::fail);

[[nodiscard]] Promise<void> erase(const path& where);

enum class Mode : std::uint8_t { read, write, truncate, exclusive };

class File;
class Cursor;

[[nodiscard]] Promise<File> open(const path& where, Mode how = Mode::read);

class File {
public:
    struct Held;

    File() = default;

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const path& where() const;

    [[nodiscard]] Promise<Bytes> read(std::uintmax_t at, std::size_t much) const;

    [[nodiscard]] Promise<std::size_t> write(std::uintmax_t at, Bytes what) const;

    [[nodiscard]] Promise<std::uintmax_t> size() const;

    [[nodiscard]] Promise<void> sync() const;

    [[nodiscard]] Cursor cursor() const;

    void close();

private:
    friend Promise<File> open(const path& where, Mode how);

    explicit File(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

class Watch;

[[nodiscard]] Promise<Watch> watch(const path& where, bool deep = false);

class Watch {
public:
    struct Held;

    Watch() = default;

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const path& where() const;

    [[nodiscard]] Promise<std::vector<Change>> next();

    void close() { held_.reset(); }

private:
    friend Promise<Watch> watch(const path& where, bool deep);

    explicit Watch(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

class Cursor {
public:
    struct Held;

    Cursor() = default;

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const path& where() const;

    [[nodiscard]] std::uintmax_t at() const;

    void seek(std::uintmax_t at);

    [[nodiscard]] Promise<Bytes> read(std::size_t much);

    [[nodiscard]] Promise<std::size_t> write(Bytes what);

    void close() { held_.reset(); }

private:
    friend Cursor File::cursor() const;

    explicit Cursor(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

}

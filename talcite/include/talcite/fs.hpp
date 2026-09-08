#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/promise.hpp"

namespace talcite::fs {

using path = std::filesystem::path;

using talcite::Bytes;

enum class Kind : std::uint8_t { none, file, folder, link, device, special };

struct Room {
    std::uintmax_t whole = 0;
    std::uintmax_t spare = 0;
};

enum class Mount : std::uint8_t { disk, remote, removable, optical, other };

struct Drive {
    path where;
    path behind;
    std::string named;
    Mount mount = Mount::other;
    Room room;
};

struct Marks {
    bool hidden = false;
    bool readonly = false;
    bool system = false;
    bool executable = false;
    bool stale = false;
};

struct Entry {
    path where;
    Kind kind = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds written{};
    Marks marks;
};

enum class Clash : std::uint8_t { overwrite, skip, beside, refuse };

enum class Stir : std::uint8_t { made, gone, changed, renamed, lost };

struct Change {
    path where;
    path was;
    Stir stir = Stir::changed;
};

class Trouble : public std::runtime_error {
public:
    Trouble(const std::string& said, path where);

    [[nodiscard]] const path& where() const { return where_; }

private:
    path where_;
};

[[nodiscard]] Promise<bool> exists(const path& where);

[[nodiscard]] Promise<Kind> kind(const path& where);

[[nodiscard]] Promise<std::uintmax_t> size(const path& where);

[[nodiscard]] Promise<std::chrono::sys_seconds> written(const path& where);

[[nodiscard]] Promise<void> written(const path& where, std::chrono::sys_seconds when);

[[nodiscard]] Promise<Room> roomy(const path& where);

[[nodiscard]] Promise<std::size_t> slice(const path& where);

[[nodiscard]] Promise<Mount> mount(const path& where);

[[nodiscard]] Promise<std::vector<Drive>> drives();

[[nodiscard]] Promise<Marks> marks(const path& where);

[[nodiscard]] Promise<void> marks(const path& where, Marks what);

[[nodiscard]] Promise<bool> alike(const path& one, const path& two);

[[nodiscard]] Promise<path> target(const path& where);

[[nodiscard]] Promise<path> real(const path& where);

[[nodiscard]] Promise<void> link(const path& where, const path& onto, bool hard = false);

[[nodiscard]] path here();

[[nodiscard]] Promise<void> here(const path& where);

[[nodiscard]] path home();

[[nodiscard]] path temp();

[[nodiscard]] Promise<void> make(const path& where);

[[nodiscard]] Promise<void> rename(const path& from, const path& to,
                                   Clash how = Clash::refuse);

[[nodiscard]] Promise<std::vector<Entry>> list(const path& where);

[[nodiscard]] Promise<std::vector<Entry>> walk(const path& where);

[[nodiscard]] Promise<Bytes> read(const path& where);

[[nodiscard]] Promise<void> write(const path& where, Bytes what,
                                  Clash how = Clash::overwrite);

[[nodiscard]] Promise<void> copy(const path& from, const path& to,
                                 Clash how = Clash::refuse);

[[nodiscard]] Promise<void> move(const path& from, const path& to,
                                 Clash how = Clash::refuse);

[[nodiscard]] Promise<void> erase(const path& where);

enum class Mode : std::uint8_t { read, write, fresh };

class File;
class Flow;

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

    [[nodiscard]] Flow flow() const;

    void close() { held_.reset(); }

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

class Flow {
public:
    struct Held;

    Flow() = default;

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const path& where() const;

    [[nodiscard]] std::uintmax_t at() const;

    void seek(std::uintmax_t at);

    [[nodiscard]] Promise<Bytes> read(std::size_t much);

    [[nodiscard]] Promise<std::size_t> write(Bytes what);

    void close() { held_.reset(); }

private:
    friend Flow File::flow() const;

    explicit Flow(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

}

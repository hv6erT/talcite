#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/clock.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/progress.hpp"
#include "tal/task.hpp"

namespace tal::fs {

enum class Type { file, folder, link, device, other };

enum class Conflict { fail, replace, skip, rename };

enum class Known { home, desktop, documents, downloads, music, pictures, videos, temp };

enum class Mode { read, write, truncate, exclusive, append };

struct Attributes {
    bool hidden = false;
    bool readonly = false;
    bool system = false;
    bool executable = false;

    friend bool operator==(const Attributes&, const Attributes&) = default;
};

struct Entry {
    Path where;
    Type type = Type::other;
    Type reach = Type::other;
    std::optional<Path> target;
    std::uint64_t size = 0;
    std::chrono::system_clock::time_point modified;
    Attributes attributes;
    std::optional<std::uint16_t> rights;
};

struct Range {
    std::uint64_t from = 0;
    std::optional<std::uint64_t> much;
};

struct Writing {
    Conflict conflict = Conflict::replace;
    bool atomic = true;
};

struct Copying {
    Conflict conflict = Conflict::fail;
    Progress* progress = nullptr;
};

struct Removing {
    bool trash = false;
    bool deep = false;
};

struct Changing {
    std::optional<Attributes> attributes;
    std::optional<std::chrono::system_clock::time_point> modified;
    std::optional<std::uint16_t> rights;
};

struct Volume {
    Path where;
    std::string source;
    std::string label;
    std::string format;
    bool remote = false;
    bool removable = false;
    std::uint64_t total = 0;
    std::uint64_t free = 0;
};

struct Change {
    enum class Kind { created, removed, modified, renamed, overflow };

    Kind kind = Kind::modified;
    Path where;
    Path was;

    friend bool operator==(const Change&, const Change&) = default;
};

class Watching {
public:
    Watching() = default;
    virtual ~Watching() = default;

    Watching(const Watching&) = delete;
    Watching& operator=(const Watching&) = delete;

    void add(Change change);

    void close();

    [[nodiscard]] bool closed() const;

private:
    friend class Watch;

    [[nodiscard]] std::optional<std::vector<Change>> taken(const Pending<std::vector<Change>>& waiter);

    mutable std::mutex guard_;
    std::vector<Change> queued_;
    std::optional<Pending<std::vector<Change>>> waiter_;
    bool closed_ = false;
};

class Watch {
public:
    Watch() = default;

    explicit Watch(std::shared_ptr<Watching> held);

    Watch(Watch&& other) noexcept = default;

    Watch& operator=(Watch&& other) noexcept;

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    ~Watch();

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Task<std::vector<Change>> next() const;

private:
    std::shared_ptr<Watching> held_;
};

class Opened {
public:
    Opened() = default;
    virtual ~Opened() = default;

    Opened(const Opened&) = delete;
    Opened& operator=(const Opened&) = delete;

    [[nodiscard]] virtual Task<Bytes> read(std::uint64_t at, std::size_t most) = 0;

    [[nodiscard]] virtual Task<void> write(std::uint64_t at, Bytes what) = 0;

    [[nodiscard]] virtual Task<std::uint64_t> size() = 0;

    [[nodiscard]] virtual Task<void> flush() = 0;
};

class File {
public:
    File() = default;

    explicit File(std::shared_ptr<Opened> held);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Task<Bytes> read(std::uint64_t at, std::size_t most) const;

    [[nodiscard]] Task<void> write(std::uint64_t at, Bytes what) const;

    [[nodiscard]] Task<std::uint64_t> size() const;

    [[nodiscard]] Task<void> flush() const;

    void close() { held_.reset(); }

private:
    std::shared_ptr<Opened> held_;
};

class Files : public Adapter {
public:
    static constexpr std::string_view kind = "files";

    [[nodiscard]] static std::shared_ptr<Files> builtin();

    [[nodiscard]] virtual Task<std::vector<Entry>> list(const Path& folder) = 0;

    [[nodiscard]] virtual Task<std::vector<Entry>> walk(const Path& folder) = 0;

    [[nodiscard]] virtual Task<Entry> look(const Path& one) = 0;

    [[nodiscard]] virtual Task<Bytes> read(const Path& file, Range part = {}) = 0;

    [[nodiscard]] virtual Task<void> write(const Path& file, Bytes what, Writing how = {}) = 0;

    [[nodiscard]] virtual Task<File> open(const Path& file, Mode how = Mode::read) = 0;

    [[nodiscard]] virtual Task<void> make(const Path& folder) = 0;

    [[nodiscard]] virtual Task<void> move(const Path& from, const Path& to, Conflict how = Conflict::fail) = 0;

    [[nodiscard]] virtual Task<void> copy(const Path& from, const Path& to, Copying how = {}) = 0;

    [[nodiscard]] virtual Task<void> remove(const Path& what, Removing how = {}) = 0;

    [[nodiscard]] virtual Task<void> change(const Path& what, Changing how) = 0;

    [[nodiscard]] virtual Task<void> link(const Path& where, const Path& onto) = 0;

    [[nodiscard]] virtual Task<Path> canonical(const Path& one) = 0;

    [[nodiscard]] virtual Task<Volume> volume(const Path& one) = 0;

    [[nodiscard]] virtual Task<std::vector<Volume>> volumes() = 0;

    [[nodiscard]] virtual Task<std::optional<Path>> known(Known which) = 0;

    [[nodiscard]] virtual Watch watch(const Path& folder, bool deep = false) = 0;
};

[[nodiscard]] Task<std::vector<Entry>> list(const Path& folder);

[[nodiscard]] Task<std::vector<Entry>> walk(const Path& folder);

[[nodiscard]] Task<Entry> look(const Path& one);

[[nodiscard]] Task<Bytes> read(const Path& file, Range part = {});

[[nodiscard]] Task<void> write(const Path& file, Bytes what, Writing how = {});

[[nodiscard]] Task<File> open(const Path& file, Mode how = Mode::read);

[[nodiscard]] Task<void> make(const Path& folder);

[[nodiscard]] Task<void> move(const Path& from, const Path& to, Conflict how = Conflict::fail);

[[nodiscard]] Task<void> copy(const Path& from, const Path& to, Copying how = {});

[[nodiscard]] Task<void> remove(const Path& what, Removing how = {});

[[nodiscard]] Task<void> change(const Path& what, Changing how);

[[nodiscard]] Task<void> link(const Path& where, const Path& onto);

[[nodiscard]] Task<Path> canonical(const Path& one);

[[nodiscard]] Task<Volume> volume(const Path& one);

[[nodiscard]] Task<std::vector<Volume>> volumes();

[[nodiscard]] Task<std::optional<Path>> known(Known which);

[[nodiscard]] Watch watch(const Path& folder, bool deep = false);

class Memory : public Files {
public:
    explicit Memory(Port<Clock> time = {});

    [[nodiscard]] std::string_view name() const override { return "memory"; }

    [[nodiscard]] Task<std::vector<Entry>> list(const Path& folder) override;

    [[nodiscard]] Task<std::vector<Entry>> walk(const Path& folder) override;

    [[nodiscard]] Task<Entry> look(const Path& one) override;

    [[nodiscard]] Task<Bytes> read(const Path& file, Range part = {}) override;

    [[nodiscard]] Task<void> write(const Path& file, Bytes what, Writing how = {}) override;

    [[nodiscard]] Task<File> open(const Path& file, Mode how = Mode::read) override;

    [[nodiscard]] Task<void> make(const Path& folder) override;

    [[nodiscard]] Task<void> move(const Path& from, const Path& to, Conflict how = Conflict::fail) override;

    [[nodiscard]] Task<void> copy(const Path& from, const Path& to, Copying how = {}) override;

    [[nodiscard]] Task<void> remove(const Path& what, Removing how = {}) override;

    [[nodiscard]] Task<void> change(const Path& what, Changing how) override;

    [[nodiscard]] Task<void> link(const Path& where, const Path& onto) override;

    [[nodiscard]] Task<Path> canonical(const Path& one) override;

    [[nodiscard]] Task<Volume> volume(const Path& one) override;

    [[nodiscard]] Task<std::vector<Volume>> volumes() override;

    [[nodiscard]] Task<std::optional<Path>> known(Known which) override;

    [[nodiscard]] Watch watch(const Path& folder, bool deep = false) override;

    [[nodiscard]] std::vector<Path> trashed() const;

private:
    struct State;

    std::shared_ptr<State> state_;
};

}

#include "tal/fs.hpp"

#include <type_traits>
#include <utility>

namespace tal::fs {

namespace {

// The most changes a watch holds for its reader; past it they give way to one overflow, after
// which the reader looks at the folder again.
constexpr std::size_t most_queued = 4096;

// The value a task gives.
template <typename>
struct Valued;

// The value a task gives: its type argument.
template <typename T>
struct Valued<Task<T>> {
    using Type = T;
};

// Awaits `call` on the program's default `Files` adapter, held for as long as the call runs.
template <typename F>
auto through(F call) -> decltype(call(std::declval<Files&>())) {
    const Port<Files> files;

    if constexpr (std::is_void_v<typename Valued<decltype(call(std::declval<Files&>()))>::Type>) {
        co_await call(*files);
    } else {
        co_return co_await call(*files);
    }
}

}

// Queues a change for the reader, from any thread, and wakes a reader that waits; a closed watch
// takes nothing.
void Watching::add(Change change) {
    std::vector<Change> one;

    one.push_back(std::move(change));
    add(std::move(one));
}

// Queues changes heard together, so that a reader that waits gets them in one batch. The changes
// stay in the queue and the reader is only woken: it takes them itself, so a reader that went away
// meanwhile (cancelled) takes nothing with it, and the next reader finds them.
void Watching::add(std::vector<Change> changes) {
    std::optional<Pending<void>> woken;

    {
        const std::lock_guard<std::mutex> kept{guard_};

        if (closed_ || changes.empty()) return;

        for (Change& change : changes) {
            if (!queued_.empty() && queued_.back().kind == Change::Kind::overflow) break;

            if (queued_.size() + 1 >= most_queued) {
                queued_.clear();
                queued_.push_back({Change::Kind::overflow, {}, {}});
            } else {
                queued_.push_back(std::move(change));
            }
        }

        woken = std::exchange(waiter_, std::nullopt);
        waiting_ = 0;
    }

    if (woken) woken->settle();
}

// Ends the watch: a reader that waits is cancelled, and changes no longer come.
void Watching::close() {
    std::optional<Pending<void>> woken;

    {
        const std::lock_guard<std::mutex> kept{guard_};

        closed_ = true;
        queued_.clear();
        woken = std::exchange(waiter_, std::nullopt);
        waiting_ = 0;
    }

    if (woken) woken->fail(Cancelled{});
}

// Whether the watch was closed.
bool Watching::closed() const {
    const std::lock_guard<std::mutex> kept{guard_};

    return closed_;
}

// A number for a new reader, so that its waiting can be told from another reader's.
std::uint64_t Watching::begun() {
    const std::lock_guard<std::mutex> kept{guard_};

    return ++readers_;
}

// The changes queued so far, or none when there are none yet and `waiter` is kept to wake
// `reader` at the next one. One reader at a time; a closed watch cancels its reader.
std::optional<std::vector<Change>> Watching::taken(const Pending<void>& waiter, std::uint64_t reader) {
    const std::lock_guard<std::mutex> kept{guard_};

    if (closed_) throw Cancelled{};

    if (waiter_ && waiting_ != reader) throw Trouble{Kind::busy, "The watch has a reader already."};

    if (!queued_.empty()) {
        waiter_.reset();
        waiting_ = 0;

        return std::exchange(queued_, {});
    }

    waiter_ = waiter;
    waiting_ = reader;

    return std::nullopt;
}

// A reader that went away without its changes (cancelled while it waited) no longer waits, so the
// next reader is not refused and the next changes are not given to nobody.
void Watching::forgotten(std::uint64_t reader) {
    const std::lock_guard<std::mutex> kept{guard_};

    if (waiting_ != reader) return;

    waiter_.reset();
    waiting_ = 0;
}

// A watch over the changes that `held` gathers.
Watch::Watch(std::shared_ptr<Watching> held) : held_(std::move(held)) {}

// Ends the watch held so far and takes over the other one.
Watch& Watch::operator=(Watch&& other) noexcept {
    if (this != &other) {
        if (held_) held_->close();

        held_ = std::move(other.held_);
    }

    return *this;
}

// Ends the watch; a reader that waits is cancelled.
Watch::~Watch() {
    if (held_) held_->close();
}

// Waits for the next changes; they come in batches, oldest first. A watch of nothing refuses. The
// reader is woken when changes come and takes them itself; when it goes away while it waits (its
// task cancelled and destroyed), it takes itself off the watch on the way (`Forgetting`).
Task<std::vector<Change>> Watch::next() const {
    return [](std::shared_ptr<Watching> held) -> Task<std::vector<Change>> {
        if (held == nullptr) throw Trouble{Kind::missing, "Nothing is watched."};

        struct Forgetting {
            std::shared_ptr<Watching> held;
            std::uint64_t reader;

            // Keeps the watch and the reader's number.
            Forgetting(std::shared_ptr<Watching> watched, std::uint64_t number) : held(std::move(watched)), reader(number) {}

            Forgetting(const Forgetting&) = delete;
            Forgetting& operator=(const Forgetting&) = delete;

            // Takes the reader off the watch, if it still waits there.
            ~Forgetting() { held->forgotten(reader); }
        };

        const Forgetting forgetting{held, held->begun()};

        while (true) {
            const Pending<void> waiter;

            if (auto ready = held->taken(waiter, forgetting.reader)) co_return std::move(*ready);

            co_await waiter.task();
        }
    }(held_);
}

// A file kept open by what the adapter gave; it closes once neither the file nor a task of it
// holds it any longer.
File::File(std::shared_ptr<Opened> held) : held_(std::move(held)) {}

namespace {

// The opened file a task works on, or a refusal when the file was closed.
const std::shared_ptr<Opened>& opened(const std::shared_ptr<Opened>& held) {
    if (held == nullptr) throw Trouble{Kind::missing, "The file is closed."};

    return held;
}

}

// Reads at most `most` bytes from `at`; fewer at the end of the file, none past it.
Task<Bytes> File::read(std::uint64_t at, std::size_t most) const {
    return [](std::shared_ptr<Opened> held, std::uint64_t from, std::size_t much) -> Task<Bytes> {
        co_return co_await opened(held)->read(from, much);
    }(held_, at, most);
}

// Writes the bytes at `at`, or at the end for a file opened to append.
Task<void> File::write(std::uint64_t at, Bytes what) const {
    return [](std::shared_ptr<Opened> held, std::uint64_t from, Bytes given) -> Task<void> {
        co_await opened(held)->write(from, std::move(given));
    }(held_, at, std::move(what));
}

// The size of the file now.
Task<std::uint64_t> File::size() const {
    return [](std::shared_ptr<Opened> held) -> Task<std::uint64_t> { co_return co_await opened(held)->size(); }(held_);
}

// Waits until what was written is kept by the system.
Task<void> File::flush() const {
    return [](std::shared_ptr<Opened> held) -> Task<void> { co_await opened(held)->flush(); }(held_);
}

// Throws `what` into the trash as `remove` does, not knowing where it went (`kept` empty): an
// adapter whose trash gives entries back says so itself.
Task<Trashed> Files::trash(const Path& what) {
    return [](Files& files, Path given) -> Task<Trashed> {
        Task<void> removing = files.remove(given, Removing{true, true});

        co_await std::move(removing);

        co_return Trashed{std::move(given), {}};
    }(*this, what);
}

// Gives an entry back from the trash: refused as unsupported, unless the adapter keeps a trash
// that gives entries back.
Task<void> Files::restore(const Trashed& one, Conflict how) {
    (void)how;

    return [](Path from) -> Task<void> {
        throw Trouble{Kind::unsupported, "There is no trash here to restore from.", from.text()};

        co_return;
    }(one.from);
}

// Lists a folder through the default adapter.
Task<std::vector<Entry>> list(const Path& folder) {
    return through([folder](Files& files) { return files.list(folder); });
}

// Lists the whole tree below a folder through the default adapter.
Task<std::vector<Entry>> walk(const Path& folder) {
    return through([folder](Files& files) { return files.walk(folder); });
}

// Looks at one entry through the default adapter.
Task<Entry> look(const Path& one) {
    return through([one](Files& files) { return files.look(one); });
}

// Reads a file through the default adapter.
Task<Bytes> read(const Path& file, Range part) {
    return through([file, part](Files& files) { return files.read(file, part); });
}

// Writes a file through the default adapter.
Task<void> write(const Path& file, Bytes what, Writing how) {
    return through([file, what = std::move(what), how](Files& files) mutable { return files.write(file, std::move(what), how); });
}

// Opens a file through the default adapter.
Task<File> open(const Path& file, Mode how) {
    return through([file, how](Files& files) { return files.open(file, how); });
}

// Makes a folder with its parents through the default adapter.
Task<void> make(const Path& folder) {
    return through([folder](Files& files) { return files.make(folder); });
}

// Moves an entry through the default adapter.
Task<void> move(const Path& from, const Path& to, Conflict how) {
    return through([from, to, how](Files& files) { return files.move(from, to, how); });
}

// Copies an entry through the default adapter.
Task<void> copy(const Path& from, const Path& to, Copying how) {
    return through([from, to, how](Files& files) { return files.copy(from, to, how); });
}

// Removes an entry through the default adapter.
Task<void> remove(const Path& what, Removing how) {
    return through([what, how](Files& files) { return files.remove(what, how); });
}

// Throws an entry into the trash through the default adapter, telling where it went.
Task<Trashed> trash(const Path& what) {
    return through([what](Files& files) { return files.trash(what); });
}

// Gives an entry back from the trash through the default adapter.
Task<void> restore(const Trashed& one, Conflict how) {
    return through([one, how](Files& files) { return files.restore(one, how); });
}

// Changes the attributes or the time of an entry through the default adapter.
Task<void> change(const Path& what, Changing how) {
    return through([what, how](Files& files) { return files.change(what, how); });
}

// Makes a link through the default adapter.
Task<void> link(const Path& where, const Path& onto) {
    return through([where, onto](Files& files) { return files.link(where, onto); });
}

// The path with every link resolved, through the default adapter.
Task<Path> canonical(const Path& one) {
    return through([one](Files& files) { return files.canonical(one); });
}

// The volume holding a path, through the default adapter.
Task<Volume> volume(const Path& one) {
    return through([one](Files& files) { return files.volume(one); });
}

// Every volume, through the default adapter.
Task<std::vector<Volume>> volumes() {
    return through([](Files& files) { return files.volumes(); });
}

// A known folder, through the default adapter.
Task<std::optional<Path>> known(Known which) {
    return through([which](Files& files) { return files.known(which); });
}

// Watches a folder through the default adapter.
Watch watch(const Path& folder, bool deep) { return Port<Files>{}->watch(folder, deep); }

}

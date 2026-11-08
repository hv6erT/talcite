#include <algorithm>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/task.hpp"

#include "core.hpp"

// The disk of the system as the `Files` port: every operation goes to the pool for calls that block
// (`io`) and comes back to the executor it was called on, around the synchronous core ported from
// talcite. The adapter checks first what the trials of the port expect of every adapter alike (a
// relative path is malformed, a root stays, a read-only file is not replaced even for root), and
// leaves the rest to the system, whose errors become troubles of their kind (`core::kind_of`).

namespace tal::platform {

namespace {

using core::amiss;

// Runs synchronous work on the io pool and comes back to the executor of the caller with its
// result or its failure. While the work runs, the core sees whether the task was cancelled
// (`core::stopped`).
template <typename F>
auto offloaded(F work) -> Task<std::invoke_result_t<F&>> {
    using Result = std::invoke_result_t<F&>;

    Executor& back = co_await detail::Here{};
    const std::shared_ptr<detail::Chain> chain = co_await detail::Current{};

    co_await to(io());

    std::exception_ptr failed;

    if constexpr (std::is_void_v<Result>) {
        core::halted(chain->cancelling());

        try {
            work();
        } catch (...) {
            failed = std::current_exception();
        }

        core::halted(nullptr);

        co_await to(back);

        if (failed) std::rethrow_exception(failed);
    } else {
        std::optional<Result> made;

        core::halted(chain->cancelling());

        try {
            made.emplace(work());
        } catch (...) {
            failed = std::current_exception();
        }

        core::halted(nullptr);

        co_await to(back);

        if (failed) std::rethrow_exception(failed);

        co_return std::move(*made);
    }
}

// A path the disk takes: absolute, in the system's form.
path absolute(const Path& given) {
    if (!given.absolute()) throw Trouble{::tal::Kind::malformed, given.text() + " is not an absolute path.", given.text()};

    return core::native(given);
}

// The type of an entry in the port's terms.
fs::Type typed(platform::Kind kind) {
    switch (kind) {
        case platform::Kind::file: return fs::Type::file;
        case platform::Kind::folder: return fs::Type::folder;
        case platform::Kind::link: return fs::Type::link;
        case platform::Kind::device: return fs::Type::device;
        default: return fs::Type::other;
    }
}

// An entry of the core in the port's terms, shown under `shown`.
fs::Entry entered(const Entry& one, const Path& shown) {
    fs::Entry made;

    made.where = shown;
    made.type = typed(one.kind);
    made.reach = one.attributes.stale ? fs::Type::other : typed(one.reach == platform::Kind::none ? one.kind : one.reach);
    made.size = one.kind == platform::Kind::file ? one.size : 0;
    made.modified = std::chrono::system_clock::time_point{one.modified.time_since_epoch()};
    made.attributes = fs::Attributes{one.attributes.hidden, one.attributes.readonly, one.attributes.system, one.attributes.executable};

    if (one.target) made.target = core::told(*one.target);

#ifndef _WIN32
    if (one.rights != std::filesystem::perms::unknown) made.rights = static_cast<std::uint16_t>(static_cast<unsigned>(one.rights) & 07777U);
#endif

    return made;
}

// The conflict rule of the port in the core's terms.
Conflict conflicted(fs::Conflict how) {
    switch (how) {
        case fs::Conflict::fail: return Conflict::fail;
        case fs::Conflict::skip: return Conflict::skip;
        case fs::Conflict::rename: return Conflict::rename;
        case fs::Conflict::replace: return Conflict::overwrite;
    }

    return Conflict::fail;
}

// The entries of a folder, in order of the bytes of their names as the memory gives them.
std::vector<Entry> listed(const path& folder) {
    std::vector<Entry> found = core::list_whole_at(folder);

    std::sort(found.begin(), found.end(), [](const Entry& one, const Entry& two) {
        return core::told(one.where.filename()).stored() < core::told(two.where.filename()).stored();
    });

    return found;
}

// A folder that must be there, as the system reaches it: itself, or where a link to a folder
// leads. Missing when nothing is there, of the wrong type when a file is.
path folder_there(const path& where) {
    const platform::Kind kind = core::kind_at(where);

    if (kind == platform::Kind::none) amiss("There is no such folder.", where, ::tal::Kind::missing);

    if (kind == platform::Kind::folder) return where;

    if (kind == platform::Kind::link && core::entry_at(where).reach == platform::Kind::folder) return core::canonical_at(where);

    amiss("Not a folder.", where, ::tal::Kind::other);
}

#ifndef _WIN32

// Gives the right to run to whoever may read the entry, or takes it from all.
void runnable(const path& where, bool given) {
    using std::filesystem::perms;

    std::error_code trouble;
    const perms rights = std::filesystem::status(where, trouble).permissions();

    if (trouble) amiss("Cannot read the rights.", where, trouble);

    perms made = rights & ~(perms::owner_exec | perms::group_exec | perms::others_exec);

    if (given) {
        if ((rights & perms::owner_read) != perms::none) made |= perms::owner_exec;
        if ((rights & perms::group_read) != perms::none) made |= perms::group_exec;
        if ((rights & perms::others_read) != perms::none) made |= perms::others_exec;
    }

    std::filesystem::permissions(where, made, trouble);

    if (trouble) amiss("Cannot set the rights.", where, trouble);
}

#endif

// Where an entry goes by the conflict rule, as the memory settles it, and the rule left for the
// core: the target when it is free, none to skip, a free name beside it, or the target replaced
// whole. A file replaced by a file is left to the core, which writes beside it and renames over
// it, so the old file stays until the new one is whole; anything else that is replaced is taken
// away first, since the core would merge a folder into a folder until the first file that clashes.
std::pair<std::optional<path>, Conflict> settled(const path& source, const path& target, fs::Conflict how) {
    const platform::Kind there = core::kind_at(target);

    if (there == platform::Kind::none) return {target, Conflict::fail};

    switch (how) {
        case fs::Conflict::fail: amiss("The target already exists.", target, ::tal::Kind::exists);
        case fs::Conflict::skip: return {std::nullopt, Conflict::fail};
        case fs::Conflict::rename: return {core::settled_on(target, Conflict::rename), Conflict::fail};
        case fs::Conflict::replace: break;
    }

    if (there == platform::Kind::file && core::kind_at(source) == platform::Kind::file) return {target, Conflict::overwrite};

    core::erase_whole(target);

    return {target, Conflict::fail};
}

// A root, which no operation takes away, moves or changes.
void not_root(const Path& given, std::string_view what) {
    if (given.root()) throw Trouble{::tal::Kind::denied, given.text() + " cannot be " + std::string{what} + ".", given.text()};
}

// Appends the entries below the folder, each folder before what it holds, links not followed.
void walked(const Path& shown, const path& folder, std::vector<fs::Entry>& found) {
    for (const Entry& one : listed(folder)) {
        if (core::stopped()) throw Cancelled{};

        found.push_back(entered(one, shown / core::told(one.where.filename()).stored()));

        if (one.kind == platform::Kind::folder) walked(found.back().where, one.where, found);
    }
}

// A file kept open on the disk.
class Opening : public fs::Opened {
public:
    // The file the core opened.
    explicit Opening(std::shared_ptr<Handle> held) : held_(std::move(held)) {}

    // At most `most` bytes from `at`.
    Task<Bytes> read(std::uint64_t at, std::size_t most) override {
        return offloaded([held = held_, at, most] { return core::read_at(*held, at, most); });
    }

    // Writes at `at`, or at the end when appending; a file open to read refuses.
    Task<void> write(std::uint64_t at, Bytes what) override {
        return offloaded([held = held_, at, what = std::move(what)] {
            if (held->mode == Mode::read) amiss("The file is open for reading only.", held->where, ::tal::Kind::denied);

            (void)core::write_at(*held, at, what);
        });
    }

    // The size of the file now.
    Task<std::uint64_t> size() override {
        return offloaded([held = held_] { return std::uint64_t{core::sized(*held)}; });
    }

    // Waits until what was written is on the medium.
    Task<void> flush() override {
        return offloaded([held = held_] { core::sync_at(*held); });
    }

private:
    std::shared_ptr<Handle> held_;
};

// The system's disk.
class Disk : public fs::Files {
public:
    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "disk"; }

    // The entries directly inside a folder, in order of name.
    Task<std::vector<fs::Entry>> list(const Path& folder) override {
        return offloaded([folder] {
            const path where = folder_there(absolute(folder));

            std::vector<fs::Entry> found;

            for (const Entry& one : listed(where)) found.push_back(entered(one, folder / core::told(one.where.filename()).stored()));

            return found;
        });
    }

    // Every entry below a folder, each folder before what it holds; links are not followed.
    Task<std::vector<fs::Entry>> walk(const Path& folder) override {
        return offloaded([folder] {
            const path where = folder_there(absolute(folder));

            std::vector<fs::Entry> found;

            walked(folder, where, found);

            return found;
        });
    }

    // The entry itself; a link is looked at, not followed.
    Task<fs::Entry> look(const Path& one) override {
        return offloaded([one] {
            const path where = absolute(one);
            const Entry found = core::entry_at(where);

            if (found.kind == platform::Kind::none) amiss("There is no such entry.", where, ::tal::Kind::missing);

            return entered(found, one);
        });
    }

    // The bytes of a file, or of the part of it asked for.
    Task<Bytes> read(const Path& file, fs::Range part) override {
        return offloaded([file, part] {
            const path where = absolute(file);

            if (core::kind_at(where) == platform::Kind::folder) amiss("Not a file.", where, ::tal::Kind::other);

            if (part.from == 0 && !part.much) return core::read_whole(where);

            const std::shared_ptr<Handle> held = core::open_at(where, Mode::read);
            const std::uint64_t size = core::sized(*held);
            const std::uint64_t from = std::min(part.from, size);
            const std::uint64_t much = std::min(part.much.value_or(size), size - from);

            return core::read_at(*held, from, static_cast<std::size_t>(much));
        });
    }

    // Writes a file whole, beside it and renamed over it when it is there; a read-only file refuses
    // to be replaced, whoever runs the program.
    Task<void> write(const Path& file, Bytes what, fs::Writing how) override {
        return offloaded([file, what = std::move(what), how] {
            const path where = absolute(file);

            if (file.root() || core::kind_at(where) == platform::Kind::folder) amiss("Not a file.", where, ::tal::Kind::other);

            (void)folder_there(where.parent_path());

            if (how.conflict == fs::Conflict::replace && core::kind_at(where) == platform::Kind::file && core::attributes_at(where).readonly)
                amiss("The file is read-only.", where, ::tal::Kind::denied);

            (void)core::write_whole(where, what, conflicted(how.conflict));
        });
    }

    // Opens a file as asked; a read-only file opens only to read.
    Task<fs::File> open(const Path& file, fs::Mode how) override {
        return offloaded([file, how] {
            const path where = absolute(file);
            const platform::Kind found = core::kind_at(where);

            if (file.root() || found == platform::Kind::folder) amiss("Not a file.", where, ::tal::Kind::other);

            if (how != fs::Mode::read && found == platform::Kind::file && core::attributes_at(where).readonly)
                amiss("The file is read-only.", where, ::tal::Kind::denied);

            if (how != fs::Mode::read && found == platform::Kind::none) (void)folder_there(where.parent_path());

            const Mode mode = how == fs::Mode::read        ? Mode::read
                              : how == fs::Mode::write     ? Mode::write
                              : how == fs::Mode::truncate  ? Mode::truncate
                              : how == fs::Mode::exclusive ? Mode::exclusive
                                                           : Mode::append;

            return fs::File{std::make_shared<Opening>(core::open_at(where, mode))};
        });
    }

    // Makes a folder and every missing folder on the way to it.
    Task<void> make(const Path& folder) override {
        return offloaded([folder] { core::make_folder_at(absolute(folder)); });
    }

    // Moves an entry with all it holds; across media entry by entry.
    Task<void> move(const Path& from, const Path& to, fs::Conflict how) override {
        return offloaded([from, to, how] {
            const path source = absolute(from);
            const path target = absolute(to);

            not_root(from, "moved");

            if (core::kind_at(source) == platform::Kind::none) amiss("Nothing to move.", source, ::tal::Kind::missing);

            (void)folder_there(target.parent_path());

            if (to.within(from) && !to.same(from)) amiss("A folder cannot go inside itself.", target, ::tal::Kind::other);

            if (to.same(from)) return;

            if (how == fs::Conflict::replace && from.within(to))
                amiss("The target holds what would replace it.", target, ::tal::Kind::other);

            const auto [into, left] = settled(source, target, how);

            if (into) core::move_whole(source, *into, left);
        });
    }

    // Copies an entry with all it holds, counting bytes in the progress.
    Task<void> copy(const Path& from, const Path& to, fs::Copying how) override {
        return offloaded([from, to, how] {
            const path source = absolute(from);
            const path target = absolute(to);

            not_root(from, "copied whole");

            if (core::kind_at(source) == platform::Kind::none) amiss("Nothing to copy.", source, ::tal::Kind::missing);

            (void)folder_there(target.parent_path());

            if (to.same(from)) amiss("The copy would be the entry itself.", target, ::tal::Kind::exists);

            if (to.within(from)) amiss("A folder cannot go inside itself.", target, ::tal::Kind::other);

            if (how.conflict == fs::Conflict::replace && from.within(to))
                amiss("The target holds what would replace it.", target, ::tal::Kind::other);

            const auto [into, left] = settled(source, target, how.conflict);

            if (into) core::copy_whole(source, *into, how.progress, left);
        });
    }

    // Removes an entry: a folder that holds anything only when deep, into the trash when asked.
    Task<void> remove(const Path& what, fs::Removing how) override {
        return offloaded([what, how] {
            const path where = absolute(what);
            const platform::Kind found = core::kind_at(where);

            not_root(what, "removed");

            if (found == platform::Kind::none) amiss("There is no such entry.", where, ::tal::Kind::missing);

            if (!how.deep && found == platform::Kind::folder && !core::list_whole_at(where).empty())
                amiss("The folder is not empty.", where, ::tal::Kind::other);

            if (how.trash) {
                core::trashed_at(where);
            } else {
                core::erase_whole(where);
            }
        });
    }

    // Changes the attributes, the time or the rights of an entry; Windows has no rights to set.
    Task<void> change(const Path& what, fs::Changing how) override {
        return offloaded([what, how] {
            const path where = absolute(what);

            not_root(what, "changed");

            if (core::kind_at(where) == platform::Kind::none) amiss("There is no such entry.", where, ::tal::Kind::missing);

            if (how.attributes) {
                Attributes given = core::attributes_at(where);

                given.hidden = how.attributes->hidden;
                given.readonly = how.attributes->readonly;
                given.system = how.attributes->system;
                given.executable = how.attributes->executable;
                core::attributed_at(where, given);

#ifndef _WIN32
                // POSIX keeps the right to run in the rights: given to whoever may read, or taken
                // from all. Windows tells a program by its extension and has nothing to set.
                runnable(where, how.attributes->executable);
#endif
            }

            if (how.modified) core::stamp_at(where, std::chrono::time_point_cast<std::chrono::nanoseconds>(*how.modified));

            if (how.rights) {
#ifdef _WIN32
                amiss("Windows has no POSIX rights.", where, ::tal::Kind::unsupported);
#else
                std::error_code trouble;

                std::filesystem::permissions(where, static_cast<std::filesystem::perms>(*how.rights), trouble);

                if (trouble) amiss("Cannot set the rights.", where, trouble);
#endif
            }
        });
    }

    // Makes a link at `where` leading to `onto`, which need not be there.
    Task<void> link(const Path& where, const Path& onto) override {
        return offloaded([where, onto] {
            const path at = absolute(where);

            (void)folder_there(at.parent_path());

            if (core::kind_at(at) != platform::Kind::none) amiss("The entry exists already.", at, ::tal::Kind::exists);

            core::linking_at(at, core::native(onto), false);
        });
    }

    // The path with every link resolved; it must lead somewhere.
    Task<Path> canonical(const Path& one) override {
        return offloaded([one] {
            const path where = absolute(one);
            std::error_code trouble;

            if (!std::filesystem::exists(where, trouble)) amiss("The path leads nowhere.", where, ::tal::Kind::missing);

            return core::told(core::canonical_at(where));
        });
    }

    // The volume holding a path.
    Task<fs::Volume> volume(const Path& one) override {
        return offloaded([one] { return described(core::volume_at(core::mounting_at(absolute(one)))); });
    }

    // Every volume of the system.
    Task<std::vector<fs::Volume>> volumes() override {
        return offloaded([] {
            std::vector<fs::Volume> found;

            for (const path& root : core::roots_at()) found.push_back(described(core::volume_at(root)));

            return found;
        });
    }

    // A known folder, when it is there.
    Task<std::optional<Path>> known(fs::Known which) override {
        return offloaded([which]() -> std::optional<Path> {
            const std::optional<path> found = core::known_at(which);

            if (!found) return std::nullopt;

            return core::told(*found);
        });
    }

    // Watches a folder that is there; its changes come from the system as they happen.
    fs::Watch watch(const Path& folder, bool deep) override { return fs::Watch{core::watched(folder_there(absolute(folder)), deep)}; }

private:
    // A volume of the core in the port's terms.
    static fs::Volume described(const Volume& one) {
        return fs::Volume{core::told(one.where),
                          one.source.empty() ? std::string{} : core::told(one.source).text(),
                          one.label,
                          one.format,
                          one.mount == Mount::remote,
                          one.mount == Mount::removable || one.mount == Mount::optical,
                          one.space.total,
                          one.space.free};
    }
};

}

}

namespace tal::fs {

// The built-in adapter of the port: the system's disk, one for the whole program.
std::shared_ptr<Files> Files::builtin() {
    static const std::shared_ptr<Files> made = std::make_shared<platform::Disk>();

    return made;
}

}

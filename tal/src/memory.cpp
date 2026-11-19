#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
#include <utility>

#include "tal/fs.hpp"

// A file system in memory, for trials and for programs without a disk. It keeps one map from the
// stored form of each path to its node; roots exist without nodes. Every operation runs under one
// lock when its task runs, and the changes it makes go to the watches after the lock is let go.
// A change is reported once, at the top of what the operation touched: moving a folder reports
// the folder, not each entry inside it.

namespace tal::fs {

namespace {

// The most links followed while resolving one path, as the systems limit it.
constexpr int most_links = 40;

// One entry of the memory: a file with its bytes, a folder, or a link with its target.
struct Node {
    Type type = Type::file;
    Bytes data;
    std::chrono::system_clock::time_point modified;
    Attributes attributes;
    Path target;
    std::uint16_t rights = 0644;
};

// A watch kept by the memory: the folder it watches, resolved, and whether below it too.
struct Watcher : Watching {
    Path folder;
    bool deep = false;
};

// The root of a path and its names below the root, in order.
std::pair<Path, std::deque<std::string>> split(const Path& given) {
    std::deque<std::string> names;
    Path at = given;

    while (!at.root() && !at.empty()) {
        names.emplace_front(at.name());
        at = at.parent();
    }

    return {at, std::move(names)};
}

// The trouble of a path that is not there.
Trouble missing(const Path& where) { return Trouble{Kind::missing, "There is no " + where.text() + ".", where.text()}; }

// The trouble of a path that does not start at a root.
Trouble relative(const Path& where) {
    return Trouble{Kind::malformed, where.text() + " is not an absolute path.", where.text()};
}

// The trouble of an entry that is there already.
Trouble existing(const Path& where) { return Trouble{Kind::exists, where.text() + " exists already.", where.text()}; }

// The trouble of an entry of the wrong type for the operation.
Trouble mistyped(const Path& where, std::string_view wanted) {
    return Trouble{Kind::other, where.text() + " is not " + std::string{wanted} + ".", where.text()};
}

// Runs `work` when the task runs, so that the operation is as lazy as one on a disk.
template <typename F>
Task<std::invoke_result_t<F&>> later(F work) {
    if constexpr (std::is_void_v<std::invoke_result_t<F&>>) {
        work();
        co_return;
    } else {
        co_return work();
    }
}

}

// What the memory holds, shared with the tasks of its operations, which may outlive it.
struct Memory::State {
    struct Handle;

    mutable std::mutex guard;
    std::shared_ptr<Clock> time;
    std::map<std::string, Node, std::less<>> nodes;
    std::vector<std::weak_ptr<Watcher>> watchers;
    std::vector<Path> trash;
    std::vector<Change> made;

    // The node at a path, or none; roots have none.
    Node* at(const Path& where) {
        const auto found = nodes.find(where.stored());

        return found == nodes.end() ? nullptr : &found->second;
    }

    // Whether there is a folder at the resolved path: a root or a folder node.
    bool folder(const Path& where) {
        if (where.root()) return true;

        const Node* const node = at(where);

        return node != nullptr && node->type == Type::folder;
    }

    // Whether there is anything at the resolved path.
    bool there(const Path& where) { return where.root() || at(where) != nullptr; }

    // The path with its links resolved: every link on the way, and the last one too when `last`
    // is set. Every folder on the way must be there; the last name need not be.
    Path resolved(const Path& given, bool last) {
        if (!given.absolute()) throw relative(given);

        auto [current, names] = split(given);
        int links = 0;

        while (!names.empty()) {
            const std::string name = std::move(names.front());

            names.pop_front();

            if (name == "..") {
                current = current.parent();

                continue;
            }

            const Path next = current / name;
            const Node* const node = at(next);
            const bool final = names.empty();

            if (node != nullptr && node->type == Type::link && (!final || last)) {
                if (++links > most_links) throw Trouble{Kind::other, "Too many links lead from " + given.text() + ".", given.text()};

                const Path onto = node->target.absolute() ? node->target : current / node->target.stored();
                auto [root, more] = split(onto);

                names.insert(names.begin(), more.begin(), more.end());
                current = root;

                continue;
            }

            if (!final && !folder(next)) throw missing(given);

            current = next;
        }

        return current;
    }

    // The resolved path for a new entry `given`: its folder resolved and there, its own name kept.
    Path placed(const Path& given) {
        if (given.root()) throw existing(given);

        const Path folder_path = resolved(given.parent(), true);

        if (!folder(folder_path)) throw missing(given.parent());

        return folder_path / given.name();
    }

    // The entry for the resolved path `actual`, shown as `shown`.
    Entry entry(const Path& shown, const Path& actual) {
        Entry made_entry;

        made_entry.where = shown;

        if (actual.root()) {
            made_entry.type = Type::folder;
            made_entry.reach = Type::folder;

            return made_entry;
        }

        const Node& node = *at(actual);

        made_entry.type = node.type;
        made_entry.reach = node.type;
        made_entry.size = node.type == Type::file ? node.data.size() : 0;
        made_entry.modified = node.modified;
        made_entry.attributes = node.attributes;
        made_entry.rights = node.rights;

        if (node.type == Type::link) {
            made_entry.target = node.target;
            made_entry.reach = Type::other;

            try {
                const Path reached = resolved(actual, true);

                if (reached.root()) {
                    made_entry.reach = Type::folder;
                } else if (const Node* const end = at(reached)) {
                    made_entry.reach = end->type;
                }
            } catch (const Trouble&) {
            }
        }

        return made_entry;
    }

    // The resolved paths of the entries directly inside the resolved folder, in order of name.
    std::vector<Path> children(const Path& folder_path) {
        std::string prefix{folder_path.stored()};

        if (prefix.back() != '/') prefix += '/';

        std::vector<Path> found;

        for (auto at_node = nodes.lower_bound(prefix); at_node != nodes.end() && at_node->first.starts_with(prefix); ++at_node)
            if (at_node->first.find('/', prefix.size()) == std::string::npos) found.push_back(Path::restored(at_node->first));

        return found;
    }

    // The resolved path and every resolved path below it.
    std::vector<std::string> subtree(const Path& top) {
        std::vector<std::string> found{std::string{top.stored()}};
        const std::string prefix = std::string{top.stored()} + "/";

        for (auto at_node = nodes.lower_bound(prefix); at_node != nodes.end() && at_node->first.starts_with(prefix); ++at_node)
            found.push_back(at_node->first);

        return found;
    }

    // Appends the entries below the resolved folder `actual`, shown below `shown`, each folder
    // before what it holds. The place of a folder goes down by copy: a reference into `found`
    // would dangle once the list grows.
    void walked(const Path& shown, const Path& actual, std::vector<Entry>& found) {
        for (const Path& child : children(actual)) {
            found.push_back(entry(shown / child.name(), child));

            if (found.back().type == Type::folder) {
                const Path inner = found.back().where;

                walked(inner, child, found);
            }
        }
    }

    // A folder that must be there for an operation: the resolved path of `given`.
    Path listed(const Path& given) {
        const Path actual = resolved(given, true);

        if (!there(actual)) throw missing(given);

        if (!folder(actual)) throw mistyped(given, "a folder");

        return actual;
    }

    // Takes away the resolved path and everything below it.
    void erased(const Path& top) {
        for (const std::string& one : subtree(top)) nodes.erase(one);
    }

    // A free name for `taken` beside it: "name (2).ext", "name (3).ext" and so on.
    Path renamed(const Path& taken) {
        const std::string stem{taken.stem()};
        const std::string extension = taken.extension().empty() ? "" : "." + std::string{taken.extension()};

        for (int number = 2;; ++number) {
            const Path free = taken.parent() / (stem + " (" + std::to_string(number) + ")" + extension);

            if (!there(free)) return free;
        }
    }

    // Where an entry goes by the conflict rule when `target` may be taken already; none to skip.
    std::optional<Path> settled(const Path& target, Conflict how) {
        if (!there(target)) return target;

        switch (how) {
            case Conflict::fail: throw existing(target);
            case Conflict::skip: return std::nullopt;
            case Conflict::rename: return renamed(target);
            case Conflict::replace: break;
        }

        if (target.root()) throw Trouble{Kind::denied, target.text() + " cannot be replaced.", target.text()};

        erased(target);

        return target;
    }

    // Notes a change for the watches; they get it once the operation lets go of the lock.
    void noted(Change::Kind kind, const Path& where, const Path& was = {}) { made.push_back({kind, where, was}); }

    // Gives the changes noted by the operation to the watches that see them, each watch its own in
    // one batch, then forgets them and the watches that are gone. Called with the lock held; the
    // watches take their own locks.
    void told() {
        std::vector<Change> changes = std::exchange(made, {});

        std::erase_if(watchers, [](const std::weak_ptr<Watcher>& one) { return one.expired(); });

        for (const std::weak_ptr<Watcher>& weak : watchers) {
            const auto one = weak.lock();

            if (one == nullptr) continue;

            std::vector<Change> seen;

            for (const Change& change : changes)
                if (sees(*one, change.where) || sees(*one, change.was)) seen.push_back(change);

            one->add(std::move(seen));
        }
    }

    // Whether a watch sees a change at `where`: the folder itself, an entry in it, or one below it
    // for a deep watch.
    static bool sees(const Watcher& one, const Path& where) {
        if (where.empty()) return false;

        if (where == one.folder || where.parent() == one.folder) return true;

        return one.deep && where.within(one.folder);
    }

    // The time the clock gives for a change now.
    [[nodiscard]] std::chrono::system_clock::time_point now() const { return time->calendar(); }
};

// An empty memory with its root "/"; times of changes come from the given clock.
Memory::Memory(Port<Clock> time) : state_(std::make_shared<State>()) { state_->time = time.shared(); }

namespace {

// Runs an operation on the memory under its lock and then tells the watches what it changed.
template <typename S, typename F>
auto locked(const std::shared_ptr<S>& state, F work) {
    return later([state, work = std::move(work)]() mutable {
        const std::lock_guard<std::mutex> kept{state->guard};

        state->made.clear();

        if constexpr (std::is_void_v<decltype(work(*state))>) {
            work(*state);
            state->told();
        } else {
            auto result = work(*state);

            state->told();

            return result;
        }
    });
}

}

// A file of the memory kept open: it finds its node by the resolved path at every operation, so a
// file moved or removed meanwhile is missing to it.
struct Memory::State::Handle : Opened {
    std::shared_ptr<State> state;
    Path key;
    Mode mode = Mode::read;

    // A handle on the resolved path, opened as asked.
    Handle(std::shared_ptr<State> given, Path where, Mode how) : state(std::move(given)), key(std::move(where)), mode(how) {}

    // The node of the file, which must still be there.
    static Node& file(State& from, const Path& where) {
        Node* const node = from.at(where);

        if (node == nullptr || node->type != Type::file) throw missing(where);

        return *node;
    }

    // At most `most` bytes from `at`.
    Task<Bytes> read(std::uint64_t at, std::size_t most) override {
        return locked(state, [where = key, at, most](State& from) {
            const Bytes& data = file(from, where).data;
            const std::uint64_t begin = std::min<std::uint64_t>(at, data.size());
            const std::uint64_t end = std::min<std::uint64_t>(begin + most, data.size());

            return Bytes{data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(end)};
        });
    }

    // Writes at `at`, or at the end when appending; a gap before `at` fills with zeros.
    Task<void> write(std::uint64_t at, Bytes what) override {
        return locked(state, [where = key, how = mode, at, what = std::move(what)](State& from) mutable {
            if (how == Mode::read) throw Trouble{Kind::denied, where.text() + " is open for reading only.", where.text()};

            Node& node = file(from, where);
            const std::uint64_t place = how == Mode::append ? node.data.size() : at;

            if (node.data.size() < place + what.size()) node.data.resize(place + what.size());

            std::copy(what.begin(), what.end(), node.data.begin() + static_cast<std::ptrdiff_t>(place));
            node.modified = from.now();
            from.noted(Change::Kind::modified, where);
        });
    }

    // The size of the file now.
    Task<std::uint64_t> size() override {
        return locked(state, [where = key](State& from) { return std::uint64_t{file(from, where).data.size()}; });
    }

    // Nothing to keep: the memory holds what was written at once.
    Task<void> flush() override {
        return later([] {});
    }
};

// Opens a file: to read one that is there, to write or append to one made if missing, to write
// one emptied first, or to make a new one only. A read-only file opens only to read.
Task<File> Memory::open(const Path& file, Mode how) {
    return locked(state_, [file, how, self = state_](State& state) {
        const Path actual = state.resolved(file, true);
        Node* const node = state.at(actual);

        if (actual.root() || (node != nullptr && node->type != Type::file)) throw mistyped(file, "a file");

        if (node == nullptr && how == Mode::read) throw missing(file);

        if (node != nullptr && how == Mode::exclusive) throw existing(file);

        if (node != nullptr && how != Mode::read && node->attributes.readonly)
            throw Trouble{Kind::denied, file.text() + " is read-only.", file.text()};

        Path where = actual;

        if (node == nullptr) {
            where = state.placed(actual);
            state.nodes[std::string{where.stored()}] = Node{Type::file, {}, state.now(), {}, {}};
            state.noted(Change::Kind::created, where);
        } else if (how == Mode::truncate) {
            node->data.clear();
            node->modified = state.now();
            state.noted(Change::Kind::modified, where);
        }

        return File{std::make_shared<State::Handle>(self, where, how)};
    });
}

// The entries directly inside a folder, in order of name.
Task<std::vector<Entry>> Memory::list(const Path& folder) {
    return locked(state_, [folder](State& state) {
        const Path actual = state.listed(folder);
        std::vector<Entry> found;

        for (const Path& child : state.children(actual)) found.push_back(state.entry(folder / child.name(), child));

        return found;
    });
}

// Every entry below a folder, each folder before what it holds; links are not followed.
Task<std::vector<Entry>> Memory::walk(const Path& folder) {
    return locked(state_, [folder](State& state) {
        std::vector<Entry> found;

        state.walked(folder, state.listed(folder), found);

        return found;
    });
}

// The entry itself; a link is looked at, not followed.
Task<Entry> Memory::look(const Path& one) {
    return locked(state_, [one](State& state) {
        const Path actual = state.resolved(one, false);

        if (!state.there(actual)) throw missing(one);

        return state.entry(one, actual);
    });
}

// The bytes of a file, or of the part of it asked for.
Task<Bytes> Memory::read(const Path& file, Range part) {
    return locked(state_, [file, part](State& state) {
        const Path actual = state.resolved(file, true);
        const Node* const node = state.at(actual);

        if (!state.there(actual)) throw missing(file);

        if (node == nullptr || node->type != Type::file) throw mistyped(file, "a file");

        const std::uint64_t size = node->data.size();
        const std::uint64_t from = std::min(part.from, size);
        const std::uint64_t much = std::min(part.much.value_or(size), size - from);
        const auto begin = node->data.begin() + static_cast<std::ptrdiff_t>(from);

        return Bytes{begin, begin + static_cast<std::ptrdiff_t>(much)};
    });
}

// Writes a file whole; a link is written through, a read-only file refuses to be replaced, and
// the rights asked for become the file's.
Task<void> Memory::write(const Path& file, Bytes what, Writing how) {
    return locked(state_, [file, what = std::move(what), how](State& state) mutable {
        const Path actual = state.resolved(file, true);

        if (actual.root()) throw mistyped(file, "a file");

        const Path target = state.placed(actual);
        Node* const old = state.at(target);

        if (old != nullptr && old->type != Type::file) throw mistyped(file, "a file");

        if (old != nullptr && old->attributes.readonly && how.conflict == Conflict::replace)
            throw Trouble{Kind::denied, file.text() + " is read-only.", file.text()};

        if (old != nullptr && how.conflict == Conflict::replace) {
            old->data = std::move(what);
            old->modified = state.now();

            if (how.rights) old->rights = *how.rights;
            state.noted(Change::Kind::modified, target);

            return;
        }

        const std::optional<Path> into = state.settled(target, how.conflict);

        if (!into) return;

        Node made{Type::file, std::move(what), state.now(), {}, {}};

        if (how.rights) made.rights = *how.rights;

        state.nodes[std::string{into->stored()}] = std::move(made);
        state.noted(Change::Kind::created, *into);
    });
}

// Makes a folder and every missing folder on the way to it; one that is there already is fine.
Task<void> Memory::make(const Path& folder) {
    return locked(state_, [folder](State& state) {
        if (!folder.absolute()) throw relative(folder);

        auto [current, names] = split(folder);

        for (const std::string& name : names) {
            const Path next = state.resolved(current / name, true);

            if (!state.there(next)) {
                state.nodes[std::string{next.stored()}] = Node{Type::folder, {}, state.now(), {}, {}, 0755};
                state.noted(Change::Kind::created, next);
            } else if (!state.folder(next)) {
                throw existing(next);
            }

            current = next;
        }
    });
}

// Moves an entry, with all it holds; a link moves itself, not what it leads to.
Task<void> Memory::move(const Path& from, const Path& to, Conflict how) {
    return locked(state_, [from, to, how](State& state) {
        const Path source = state.resolved(from, false);

        if (!state.there(source)) throw missing(from);

        if (source.root()) throw Trouble{Kind::denied, from.text() + " cannot be moved.", from.text()};

        const Path wanted = state.placed(to);

        if (wanted == source) return;

        if (wanted.within(source)) throw Trouble{Kind::other, "A folder cannot go inside itself.", to.text()};

        if (how == Conflict::replace && source.within(wanted))
            throw Trouble{Kind::other, to.text() + " holds " + from.text() + " and cannot be replaced by it.", to.text()};

        const std::optional<Path> into = state.settled(wanted, how);

        if (!into) return;

        const std::string old{source.stored()};

        for (const std::string& one : state.subtree(source)) {
            auto node = state.nodes.extract(one);

            node.key() = std::string{into->stored()} + one.substr(old.size());
            state.nodes.insert(std::move(node));
        }

        state.noted(Change::Kind::renamed, *into, source);
    });
}

// Copies an entry with all it holds, keeping times and attributes; a link at the top is followed,
// links inside are copied as links. The progress counts bytes of files.
Task<void> Memory::copy(const Path& from, const Path& to, Copying how) {
    return locked(state_, [from, to, how](State& state) {
        const Path source = state.resolved(from, true);

        if (!state.there(source)) throw missing(from);

        if (source.root()) throw Trouble{Kind::denied, from.text() + " cannot be copied whole.", from.text()};

        const Path wanted = state.placed(to);

        if (wanted == source) throw existing(to);

        if (wanted.within(source)) throw Trouble{Kind::other, "A folder cannot go inside itself.", to.text()};

        if (how.conflict == Conflict::replace && source.within(wanted))
            throw Trouble{Kind::other, to.text() + " holds " + from.text() + " and cannot be replaced by it.", to.text()};

        const std::vector<std::string> all = state.subtree(source);
        std::uint64_t total = 0;

        for (const std::string& one : all) total += state.nodes.at(one).data.size();

        if (how.progress != nullptr) how.progress->expect(total);

        const std::optional<Path> into = state.settled(wanted, how.conflict);

        if (!into) return;

        const std::string old{source.stored()};

        for (const std::string& one : all) {
            const Node copied = state.nodes.at(one);

            state.nodes[std::string{into->stored()} + one.substr(old.size())] = copied;

            if (how.progress != nullptr) how.progress->add(copied.data.size());
        }

        state.noted(Change::Kind::created, *into);
    });
}

// Removes an entry: a folder that holds anything only when deep, and into the trash when asked.
Task<void> Memory::remove(const Path& what, Removing how) {
    return locked(state_, [what, how](State& state) {
        const Path actual = state.resolved(what, false);

        if (!state.there(actual)) throw missing(what);

        if (actual.root()) throw Trouble{Kind::denied, what.text() + " cannot be removed.", what.text()};

        if (!how.deep && state.folder(actual) && !state.children(actual).empty())
            throw Trouble{Kind::other, what.text() + " is not empty.", what.text()};

        state.erased(actual);

        if (how.trash) state.trash.push_back(actual);

        state.noted(Change::Kind::removed, actual);
    });
}

// Changes the attributes or the time of an entry; a link is followed.
Task<void> Memory::change(const Path& what, Changing how) {
    return locked(state_, [what, how](State& state) {
        const Path actual = state.resolved(what, true);
        Node* const node = state.at(actual);

        if (node == nullptr) {
            if (actual.root()) throw Trouble{Kind::denied, what.text() + " cannot be changed.", what.text()};

            throw missing(what);
        }

        if (how.attributes) node->attributes = *how.attributes;

        if (how.modified) node->modified = *how.modified;

        if (how.rights) node->rights = *how.rights;

        state.noted(Change::Kind::modified, actual);
    });
}

// Makes a link at `where` leading to `onto`, which need not be there; a relative target is read
// from the folder of the link.
Task<void> Memory::link(const Path& where, const Path& onto) {
    return locked(state_, [where, onto](State& state) {
        const Path actual = state.placed(where);

        if (state.there(actual)) throw existing(where);

        state.nodes[std::string{actual.stored()}] = Node{Type::link, {}, state.now(), {}, onto, 0777};
        state.noted(Change::Kind::created, actual);
    });
}

// The path with every link on it resolved; it must lead somewhere.
Task<Path> Memory::canonical(const Path& one) {
    return locked(state_, [one](State& state) {
        const Path actual = state.resolved(one, true);

        if (!state.there(actual)) throw missing(one);

        return actual;
    });
}

// The volume of a path: its root, holding as much as there is memory.
Task<Volume> Memory::volume(const Path& one) {
    return locked(state_, [one](State&) {
        if (!one.absolute()) throw relative(one);

        return Volume{split(one).first, "", "Memory", "memory", false, false, 0, 0};
    });
}

// The volumes: "/" and every other root that holds something, in order.
Task<std::vector<Volume>> Memory::volumes() {
    return locked(state_, [](State& state) {
        std::vector<Path> roots{Path{"/"}};

        for (const auto& [key, node] : state.nodes) {
            const Path root = split(Path::restored(key)).first;

            if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        }

        std::sort(roots.begin(), roots.end());

        std::vector<Volume> found;

        for (const Path& root : roots) found.push_back(Volume{root, "", "Memory", "memory", false, false, 0, 0});

        return found;
    });
}

// A known folder at its usual place below "/home" (the cache in "/home/.cache"), or "/tmp", when
// that folder is there.
Task<std::optional<Path>> Memory::known(Known which) {
    return locked(state_, [which](State& state) -> std::optional<Path> {
        const char* place = "/tmp";

        switch (which) {
            case Known::home: place = "/home"; break;
            case Known::desktop: place = "/home/Desktop"; break;
            case Known::documents: place = "/home/Documents"; break;
            case Known::downloads: place = "/home/Downloads"; break;
            case Known::music: place = "/home/Music"; break;
            case Known::pictures: place = "/home/Pictures"; break;
            case Known::videos: place = "/home/Videos"; break;
            case Known::temp: place = "/tmp"; break;
            case Known::cache: place = "/home/.cache"; break;
        }

        const Path found{place};

        if (!state.folder(found)) return std::nullopt;

        return found;
    });
}

// Watches a folder that is there now; changes come from the operations that follow.
Watch Memory::watch(const Path& folder, bool deep) {
    const std::lock_guard<std::mutex> kept{state_->guard};
    auto made = std::make_shared<Watcher>();

    made->folder = state_->listed(folder);
    made->deep = deep;
    state_->watchers.push_back(made);

    return Watch{std::move(made)};
}

// The paths removed into the trash, oldest first.
std::vector<Path> Memory::trashed() const {
    const std::lock_guard<std::mutex> kept{state_->guard};

    return state_->trash;
}

}

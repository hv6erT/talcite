#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/clock.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"

#include "proving.hpp"

// Files of tal::core, through the port and its memory adapter: reading and writing, folders,
// conflicts, moving, copying and removing, links, attributes, watches and the default adapter.
// The same trials will run on the disk adapter of platform/ once it is there.

using namespace std::chrono_literals;

namespace {

using tal::Path;
using tal::fs::Change;
using tal::fs::Conflict;
using tal::fs::Type;

// A memory and a loop on one manual clock, so that times of changes are known.
struct Desk {
    std::shared_ptr<tal::ManualClock> time = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{time}};
    tal::fs::Memory memory{tal::Port<tal::Clock>{time}};

    // Runs a task on the loop to its end and gives its value.
    template <typename T>
    T run(tal::Task<T> work) {
        return tal::wait(loop, std::move(work));
    }

    // The kind of trouble a task fails with, or none when it does not fail.
    template <typename T>
    std::optional<tal::Kind> refused(tal::Task<T> work) {
        try {
            (void)tal::wait(loop, std::move(work));
        } catch (const tal::Trouble& why) {
            return why.kind();
        }

        return std::nullopt;
    }

    // Writes a file with the given text.
    void put(std::string_view where, std::string_view what) { run(memory.write(Path{where}, tal::bytes(what))); }

    // The text of a file.
    std::string got(std::string_view where) { return tal::text(run(memory.read(Path{where}))); }

    // The names in a folder, in the order listed.
    std::vector<std::string> names(std::string_view folder) {
        std::vector<std::string> found;

        for (const tal::fs::Entry& one : run(memory.list(Path{folder}))) found.emplace_back(one.where.name());

        return found;
    }

    // Whether there is an entry at the path.
    bool there(std::string_view where) { return !refused(memory.look(Path{where})).has_value(); }
};

}

TRIAL("Memory: a file written is read back whole and in parts; look() tells its size and time") {
    Desk desk;

    desk.put("/a.txt", "Poirot");

    same(desk.got("/a.txt"), std::string{"Poirot"});
    same(tal::text(desk.run(desk.memory.read(Path{"/a.txt"}, {2, 3}))), std::string{"iro"});
    same(tal::text(desk.run(desk.memory.read(Path{"/a.txt"}, {4, {}}))), std::string{"ot"});
    same(tal::text(desk.run(desk.memory.read(Path{"/a.txt"}, {10, 3}))), std::string{});

    const tal::fs::Entry entry = desk.run(desk.memory.look(Path{"/a.txt"}));

    ought(entry.type == Type::file);
    same(entry.size, std::uint64_t{6});
    ought(entry.modified == desk.time->calendar());
    same(std::string{entry.where.stored()}, std::string{"/a.txt"});
}

TRIAL("Memory: list() gives the entries in order of name; a missing folder, a file, a relative path refuse") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/f/b"}));
    desk.put("/f/c.txt", "c");
    desk.put("/f/a.txt", "a");
    desk.put("/f/b/deep.txt", "d");

    same(desk.names("/f"), (std::vector<std::string>{"a.txt", "b", "c.txt"}));
    ought(desk.run(desk.memory.list(Path{"/f"}))[1].type == Type::folder);
    same(desk.names("/"), (std::vector<std::string>{"f"}));
    ought(desk.refused(desk.memory.list(Path{"/none"})) == tal::Kind::missing);
    ought(desk.refused(desk.memory.list(Path{"/f/a.txt"})) == tal::Kind::other);
    ought(desk.refused(desk.memory.list(Path{"f"})) == tal::Kind::malformed);
}

TRIAL("Memory: walk() gives the whole tree, each folder before what it holds") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/f/a/x"}));
    desk.put("/f/a.txt", "1");
    desk.put("/f/a/x/y.txt", "2");

    std::vector<std::string> found;

    for (const tal::fs::Entry& one : desk.run(desk.memory.walk(Path{"/f"}))) found.emplace_back(one.where.stored());

    same(found, (std::vector<std::string>{"/f/a", "/f/a/x", "/f/a/x/y.txt", "/f/a.txt"}));
}

TRIAL("Memory: make() makes every missing folder; a file in the way or a missing folder refuses") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/a/b/c"}));
    desk.run(desk.memory.make(Path{"/a/b"}));

    ought(desk.run(desk.memory.look(Path{"/a/b/c"})).type == Type::folder);

    desk.put("/a/file", "x");

    ought(desk.refused(desk.memory.make(Path{"/a/file/d"})) == tal::Kind::exists);
    ought(desk.refused(desk.memory.write(Path{"/none/x.txt"}, tal::bytes("x"))) == tal::Kind::missing);
    ought(desk.refused(desk.memory.write(Path{"/a/b"}, tal::bytes("x"))) == tal::Kind::other);
    ought(desk.refused(desk.memory.write(Path{"/"}, tal::bytes("x"))) == tal::Kind::other);
}

TRIAL("Memory: a write over a file fails, skips, renames or replaces as asked; read-only refuses") {
    Desk desk;

    desk.put("/a.txt", "first");

    ought(desk.refused(desk.memory.write(Path{"/a.txt"}, tal::bytes("x"), {Conflict::fail})) == tal::Kind::exists);

    desk.run(desk.memory.write(Path{"/a.txt"}, tal::bytes("skipped"), {Conflict::skip}));
    same(desk.got("/a.txt"), std::string{"first"});

    desk.run(desk.memory.write(Path{"/a.txt"}, tal::bytes("second"), {Conflict::rename}));
    desk.run(desk.memory.write(Path{"/a.txt"}, tal::bytes("third"), {Conflict::rename}));
    same(desk.got("/a (2).txt"), std::string{"second"});
    same(desk.got("/a (3).txt"), std::string{"third"});

    desk.time->advance(1h);
    desk.put("/a.txt", "replaced");
    same(desk.got("/a.txt"), std::string{"replaced"});
    ought(desk.run(desk.memory.look(Path{"/a.txt"})).modified == desk.time->calendar());

    desk.run(desk.memory.change(Path{"/a.txt"}, {tal::fs::Attributes{false, true, false, false}, {}}));
    ought(desk.refused(desk.memory.write(Path{"/a.txt"}, tal::bytes("x"))) == tal::Kind::denied);
}

TRIAL("Memory: move() takes a folder with all it holds; into itself or onto what holds it refuses") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/a/inner"}));
    desk.put("/a/inner/x.txt", "x");
    desk.run(desk.memory.make(Path{"/b"}));

    desk.run(desk.memory.move(Path{"/a"}, Path{"/b/a"}));

    ought(!desk.there("/a"));
    same(desk.got("/b/a/inner/x.txt"), std::string{"x"});
    ought(desk.refused(desk.memory.move(Path{"/b"}, Path{"/b/a/b"})) == tal::Kind::other);
    ought(desk.refused(desk.memory.move(Path{"/b/a/inner"}, Path{"/b/a"}, Conflict::replace)) == tal::Kind::other);
    ought(desk.refused(desk.memory.move(Path{"/none"}, Path{"/b/x"})) == tal::Kind::missing);
    ought(desk.refused(desk.memory.move(Path{"/"}, Path{"/b/x"})) == tal::Kind::denied);

    desk.put("/c.txt", "c");
    desk.put("/b/c.txt", "old");

    ought(desk.refused(desk.memory.move(Path{"/c.txt"}, Path{"/b/c.txt"})) == tal::Kind::exists);

    desk.run(desk.memory.move(Path{"/c.txt"}, Path{"/b/c.txt"}, Conflict::rename));
    same(desk.got("/b/c (2).txt"), std::string{"c"});

    desk.put("/d.txt", "new");
    desk.run(desk.memory.move(Path{"/d.txt"}, Path{"/b/c.txt"}, Conflict::replace));
    same(desk.got("/b/c.txt"), std::string{"new"});
    ought(!desk.there("/d.txt"));
}

TRIAL("Memory: copy() copies a tree, keeps times and counts the bytes in the progress") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/a/inner"}));
    desk.put("/a/one.txt", "12345");
    desk.put("/a/inner/two.txt", "678");

    const auto written = desk.run(desk.memory.look(Path{"/a/one.txt"})).modified;

    desk.time->advance(1h);

    tal::Progress progress;

    desk.run(desk.memory.copy(Path{"/a"}, Path{"/b"}, {Conflict::fail, &progress}));

    same(desk.got("/b/inner/two.txt"), std::string{"678"});
    same(desk.got("/a/one.txt"), std::string{"12345"});
    ought(desk.run(desk.memory.look(Path{"/b/one.txt"})).modified == written);
    same(progress.total(), std::uint64_t{8});
    same(progress.done(), std::uint64_t{8});
    ought(desk.refused(desk.memory.copy(Path{"/a"}, Path{"/a"})) == tal::Kind::exists);
    ought(desk.refused(desk.memory.copy(Path{"/a"}, Path{"/a/inner/a"})) == tal::Kind::other);
    ought(desk.refused(desk.memory.copy(Path{"/a"}, Path{"/b"})) == tal::Kind::exists);

    desk.run(desk.memory.copy(Path{"/a"}, Path{"/b"}, {Conflict::rename, nullptr}));
    same(desk.got("/b (2)/one.txt"), std::string{"12345"});
}

TRIAL("Memory: remove() takes a folder that holds anything only when deep; the trash keeps it") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/a/inner"}));
    desk.put("/a/inner/x.txt", "x");
    desk.run(desk.memory.make(Path{"/empty"}));

    ought(desk.refused(desk.memory.remove(Path{"/a"})) == tal::Kind::other);

    desk.run(desk.memory.remove(Path{"/empty"}));
    desk.run(desk.memory.remove(Path{"/a"}, {true, true}));

    ought(!desk.there("/a"));
    ought(!desk.there("/a/inner/x.txt"));
    ought(!desk.there("/empty"));
    same(desk.memory.trashed().size(), std::size_t{1});
    same(std::string{desk.memory.trashed()[0].stored()}, std::string{"/a"});
    ought(desk.refused(desk.memory.remove(Path{"/a"})) == tal::Kind::missing);
    ought(desk.refused(desk.memory.remove(Path{"/"})) == tal::Kind::denied);
}

TRIAL("Memory: links are followed on the way and by read and list, looked at by look and remove") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/films/Poirot"}));
    desk.put("/films/Poirot/e01.mkv", "odcinek");
    desk.run(desk.memory.link(Path{"/skrót"}, Path{"/films/Poirot"}));
    desk.run(desk.memory.link(Path{"/films/względny"}, Path{"Poirot/e01.mkv"}));
    desk.run(desk.memory.link(Path{"/zerwany"}, Path{"/nic"}));

    same(desk.names("/skrót"), (std::vector<std::string>{"e01.mkv"}));
    same(std::string{desk.run(desk.memory.list(Path{"/skrót"}))[0].where.stored()}, std::string{"/skrót/e01.mkv"});
    same(desk.got("/skrót/e01.mkv"), std::string{"odcinek"});
    same(desk.got("/films/względny"), std::string{"odcinek"});

    const tal::fs::Entry shortcut = desk.run(desk.memory.look(Path{"/skrót"}));

    ought(shortcut.type == Type::link);
    ought(shortcut.reach == Type::folder);
    ought(shortcut.target == Path{"/films/Poirot"});
    ought(desk.run(desk.memory.look(Path{"/films/względny"})).reach == Type::file);
    ought(desk.run(desk.memory.look(Path{"/zerwany"})).reach == Type::other);
    ought(desk.run(desk.memory.canonical(Path{"/skrót/e01.mkv"})) == Path{"/films/Poirot/e01.mkv"});
    ought(desk.refused(desk.memory.canonical(Path{"/zerwany"})) == tal::Kind::missing);
    ought(desk.refused(desk.memory.link(Path{"/skrót"}, Path{"/x"})) == tal::Kind::exists);

    desk.run(desk.memory.remove(Path{"/skrót"}));

    ought(!desk.there("/skrót"));
    same(desk.got("/films/Poirot/e01.mkv"), std::string{"odcinek"});
}

TRIAL("Memory: links that lead in a circle refuse instead of resolving forever") {
    Desk desk;

    desk.run(desk.memory.link(Path{"/a"}, Path{"/b"}));
    desk.run(desk.memory.link(Path{"/b"}, Path{"/a"}));

    ought(desk.refused(desk.memory.read(Path{"/a"})) == tal::Kind::other);
    ought(desk.run(desk.memory.look(Path{"/a"})).reach == Type::other);
}

TRIAL("Memory: change() sets attributes and the time; a root refuses") {
    Desk desk;
    const auto then = desk.time->calendar() - 24h;

    desk.put("/a.sh", "echo");
    desk.run(desk.memory.change(Path{"/a.sh"}, {tal::fs::Attributes{true, false, false, true}, then}));

    const tal::fs::Entry entry = desk.run(desk.memory.look(Path{"/a.sh"}));

    ought(entry.attributes.hidden);
    ought(entry.attributes.executable);
    ought(entry.modified == then);
    ought(desk.refused(desk.memory.change(Path{"/"}, {})) == tal::Kind::denied);
    ought(desk.refused(desk.memory.change(Path{"/none"}, {})) == tal::Kind::missing);
}

TRIAL("Memory: a watch sees the folder and what is in it, a deep one also what lies below") {
    Desk desk;

    desk.run(desk.memory.make(Path{"/f/inner"}));

    const tal::fs::Watch shallow = desk.memory.watch(Path{"/f"});
    const tal::fs::Watch deep = desk.memory.watch(Path{"/f"}, true);

    desk.put("/f/a.txt", "1");
    desk.put("/f/a.txt", "2");
    desk.put("/f/inner/b.txt", "3");
    desk.run(desk.memory.move(Path{"/f/a.txt"}, Path{"/f/c.txt"}));
    desk.run(desk.memory.remove(Path{"/f/c.txt"}));
    desk.put("/elsewhere.txt", "4");

    const std::vector<Change> seen = desk.run(shallow.next());

    same(seen.size(), std::size_t{4});
    ought(seen[0] == (Change{Change::Kind::created, Path{"/f/a.txt"}, {}}));
    ought(seen[1] == (Change{Change::Kind::modified, Path{"/f/a.txt"}, {}}));
    ought(seen[2] == (Change{Change::Kind::renamed, Path{"/f/c.txt"}, Path{"/f/a.txt"}}));
    ought(seen[3] == (Change{Change::Kind::removed, Path{"/f/c.txt"}, {}}));

    const std::vector<Change> below = desk.run(deep.next());

    same(below.size(), std::size_t{5});
    ought(below[2] == (Change{Change::Kind::created, Path{"/f/inner/b.txt"}, {}}));
}

TRIAL("Memory: a reader waiting on a watch wakes when a change comes from another thread") {
    Desk desk;
    const tal::fs::Watch watch = desk.memory.watch(Path{"/"});

    const auto [seen, written] = desk.run(tal::all(watch.next(), [](tal::fs::Memory& memory) -> tal::Task<int> {
        co_await tal::to(tal::pool());
        co_await memory.write(Path{"/x.txt"}, tal::bytes("x"));

        co_return 1;
    }(desk.memory)));

    same(written, 1);
    same(seen.size(), std::size_t{1});
    ought(seen[0].where == Path{"/x.txt"});
}

namespace {

// Awaits the next changes of a watch and counts a cancellation.
tal::Task<void> counted(tal::Task<std::vector<Change>> next, std::atomic<int>& cancelled) {
    try {
        (void)co_await std::move(next);
    } catch (const tal::Cancelled&) {
        ++cancelled;
    }
}

}

TRIAL("Memory: a watch ended while its reader waits cancels the reader; a second reader refuses") {
    Desk desk;
    tal::Scope scope{desk.loop};
    std::atomic<int> cancelled{0};
    auto watch = std::make_unique<tal::fs::Watch>(desk.memory.watch(Path{"/"}));

    scope.run(counted(watch->next(), cancelled));

    for (int at = 0; at < 5; ++at) (void)desk.loop.turn();

    ought(desk.refused(watch->next()) == tal::Kind::busy);

    *watch = desk.memory.watch(Path{"/"});

    for (int at = 0; at < 5; ++at) (void)desk.loop.turn();

    same(cancelled.load(), 1);

    scope.run(counted(watch->next(), cancelled));

    for (int at = 0; at < 5; ++at) (void)desk.loop.turn();

    watch.reset();

    for (int at = 0; at < 5; ++at) (void)desk.loop.turn();

    same(cancelled.load(), 2);
    ought(!scope.busy());
    ought(desk.refused(tal::fs::Watch{}.next()) == tal::Kind::missing);
}

TRIAL("Memory: a watch that falls far behind gives one overflow instead of every change") {
    Desk desk;
    const tal::fs::Watch watch = desk.memory.watch(Path{"/"});

    for (int at = 0; at < 5000; ++at) desk.put("/" + std::to_string(at), "x");

    const std::vector<Change> seen = desk.run(watch.next());

    same(seen.size(), std::size_t{1});
    ought(seen[0].kind == Change::Kind::overflow);

    desk.put("/after", "x");

    same(desk.run(watch.next()).size(), std::size_t{1});
}

TRIAL("Memory: an operation runs only when its task is awaited, even after the memory is gone") {
    tal::Loop loop;
    auto memory = std::make_unique<tal::fs::Memory>();
    tal::Task<void> writing = memory->write(Path{"/late.txt"}, tal::bytes("x"));

    ought(tal::wait(loop, memory->list(Path{"/"})).empty());

    tal::Task<tal::Bytes> reading = memory->read(Path{"/late.txt"});

    memory.reset();
    tal::wait(loop, std::move(writing));

    same(tal::text(tal::wait(loop, std::move(reading))), std::string{"x"});
}

TRIAL("Memory: known folders are given when they are there; the volumes are its roots") {
    Desk desk;

    ought(!desk.run(desk.memory.known(tal::fs::Known::music)).has_value());

    desk.run(desk.memory.make(Path{"/home/Music"}));

    ought(desk.run(desk.memory.known(tal::fs::Known::music)) == Path{"/home/Music"});
    ought(desk.run(desk.memory.known(tal::fs::Known::home)) == Path{"/home"});

    const std::vector<tal::fs::Volume> volumes = desk.run(desk.memory.volumes());

    same(volumes.size(), std::size_t{1});
    ought(volumes[0].where == Path{"/"});
    ought(desk.run(desk.memory.volume(Path{"/home/Music"})).where == Path{"/"});
}

TRIAL("fs: the free functions use the program's default adapter, and refuse without one") {
    tal::Loop loop;

    tal::defaults().set<tal::fs::Files>(std::make_shared<tal::fs::Memory>());

    tal::wait(loop, tal::fs::write(Path{"/a.txt"}, tal::bytes("domyślny")));
    same(tal::text(tal::wait(loop, tal::fs::read(Path{"/a.txt"}))), std::string{"domyślny"});
    same(tal::wait(loop, tal::fs::list(Path{"/"})).size(), std::size_t{1});

    const tal::fs::Watch watch = tal::fs::watch(Path{"/"});

    tal::wait(loop, tal::fs::remove(Path{"/a.txt"}));
    same(tal::wait(loop, watch.next()).size(), std::size_t{1});

    tal::defaults().clear<tal::fs::Files>();

    try {
        (void)tal::wait(loop, tal::fs::list(Path{"/"}));
        ought(false);
    } catch (const tal::Trouble& why) {
        ought(why.kind() == tal::Kind::missing);
    }
}

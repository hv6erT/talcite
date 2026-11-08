#include <atomic>
#include <chrono>
#include <filesystem>
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

// Files of tal::core, through the port: reading and writing, folders, conflicts, moving, copying
// and removing, links, attributes, watches and the default adapter. The trials of the port run on
// the memory and on the system's disk alike (`FILES_TRIAL`); what only the memory can show (its
// clock, its trash, its roots, its watches for now) is tried on the memory alone, and a difference
// of a disk is named where it is.

using namespace std::chrono_literals;

namespace {

using tal::Path;
using tal::fs::Change;
using tal::fs::Conflict;
using tal::fs::Type;

// What both benches share: running tasks on their loop, and shortcuts for files given by their
// path in the bench's own tree, "/" being its top.
template <typename Self>
struct Bench {
    // Runs a task on the loop to its end and gives its value.
    template <typename T>
    T run(tal::Task<T> work) {
        return tal::wait(self().loop, std::move(work));
    }

    // The kind of trouble a task fails with, or none when it does not fail.
    template <typename T>
    std::optional<tal::Kind> refused(tal::Task<T> work) {
        try {
            (void)tal::wait(self().loop, std::move(work));
        } catch (const tal::Trouble& why) {
            return why.kind();
        }

        return std::nullopt;
    }

    // Writes a file with the given text.
    void put(std::string_view where, std::string_view what) { run(self().files.write(self().at(where), tal::bytes(what))); }

    // The text of a file.
    std::string got(std::string_view where) { return tal::text::of(run(self().files.read(self().at(where)))); }

    // The names in a folder, in the order listed.
    std::vector<std::string> names(std::string_view folder) {
        std::vector<std::string> found;

        for (const tal::fs::Entry& one : run(self().files.list(self().at(folder)))) found.emplace_back(one.where.name());

        return found;
    }

    // Whether there is an entry at the path.
    bool there(std::string_view where) { return !refused(self().files.look(self().at(where))).has_value(); }

    // The stored forms of the given paths of the tree.
    std::vector<std::string> stored(const std::vector<std::string>& given) {
        std::vector<std::string> made;

        for (const std::string& one : given) made.emplace_back(self().at(one).stored());

        return made;
    }

    // Whether a time is the time of a change made just now, within the seconds a disk keeps.
    bool near(std::chrono::system_clock::time_point when) {
        const auto apart = when > self().now() ? when - self().now() : self().now() - when;

        return apart <= 3s;
    }

    // The bench as what it is.
    Self& self() { return static_cast<Self&>(*this); }
};

// A memory and a loop on one manual clock, so that times of changes are known.
struct MemoryDesk : Bench<MemoryDesk> {
    static constexpr bool manual = true;

    std::shared_ptr<tal::ManualClock> time = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{time}};
    tal::fs::Memory memory{tal::Port<tal::Clock>{time}};
    tal::fs::Files& files = memory;

    // A path of the tree: the memory's own.
    [[nodiscard]] Path at(std::string_view where) const { return Path{where}; }

    // The time of the memory's clock.
    [[nodiscard]] std::chrono::system_clock::time_point now() const { return time->calendar(); }

    // The memory always makes links.
    [[nodiscard]] static bool links() { return true; }
};

// The system's disk with a fresh folder for the trial, taken away with everything in it at the end.
struct DiskDesk : Bench<DiskDesk> {
    static constexpr bool manual = false;

    tal::Loop loop;
    std::shared_ptr<tal::fs::Files> held = tal::fs::Files::builtin();
    tal::fs::Files& files = *held;
    std::filesystem::path made = fresh();
    Path root = rooted(made);

    // Takes the folder away.
    ~DiskDesk() {
        std::error_code ignored;

        std::filesystem::remove_all(made, ignored);
    }

    DiskDesk() = default;
    DiskDesk(const DiskDesk&) = delete;
    DiskDesk& operator=(const DiskDesk&) = delete;

    // A path of the tree: below the trial's folder.
    [[nodiscard]] Path at(std::string_view where) const { return root / where; }

    // The time of the system.
    [[nodiscard]] static std::chrono::system_clock::time_point now() { return std::chrono::system_clock::now(); }

    // Whether the system lets this program make links: Windows does only with the right to.
    [[nodiscard]] bool links() {
        const std::optional<tal::Kind> made_link = refused(files.link(at("/probe"), at("/")));

        if (!made_link) (void)refused(files.remove(at("/probe")));

        return !made_link.has_value();
    }

    // The trial's folder as a path of the port, every link on the way to it resolved.
    static Path rooted(const std::filesystem::path& folder) {
        const std::u8string spelled = std::filesystem::canonical(folder).generic_u8string();

        return Path::restored(std::string{spelled.begin(), spelled.end()});
    }

    // A new folder in the system's temporary folder.
    static std::filesystem::path fresh() {
        static std::atomic<int> numbered{0};
        const std::filesystem::path made = std::filesystem::temp_directory_path() /
                                           ("tal-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                            std::to_string(++numbered));

        std::filesystem::create_directories(made);

        return made;
    }
};

}

// A trial of the files port, run once on the memory and once on the disk.
#define FILES_TRIAL(named, trial)                                                                    \
    template <typename Desk>                                                                         \
    static void trial();                                                                             \
    static const ::proving::Adding trial##_memory{"Memory: " named, __FILE__, __LINE__, &trial<MemoryDesk>}; \
    static const ::proving::Adding trial##_disk{"Disk: " named, __FILE__, __LINE__, &trial<DiskDesk>};       \
    template <typename Desk>                                                                         \
    static void trial()

FILES_TRIAL("a file written is read back whole and in parts; look() tells its size and time", files_trial_1) {
    Desk desk;

    desk.put("/a.txt", "Poirot");

    same(desk.got("/a.txt"), std::string{"Poirot"});
    same(tal::text::of(desk.run(desk.files.read(desk.at("/a.txt"), {2, 3}))), std::string{"iro"});
    same(tal::text::of(desk.run(desk.files.read(desk.at("/a.txt"), {4, {}}))), std::string{"ot"});
    same(tal::text::of(desk.run(desk.files.read(desk.at("/a.txt"), {10, 3}))), std::string{});

    const tal::fs::Entry entry = desk.run(desk.files.look(desk.at("/a.txt")));

    ought(entry.type == Type::file);
    same(entry.size, std::uint64_t{6});
    ought(desk.near(entry.modified));
    ought(entry.where == desk.at("/a.txt"));
}

FILES_TRIAL("list() gives the entries in order of name; a missing folder, a file, a relative path refuse", files_trial_2) {
    Desk desk;

    desk.run(desk.files.make(desk.at("/f/b")));
    desk.put("/f/c.txt", "c");
    desk.put("/f/a.txt", "a");
    desk.put("/f/b/deep.txt", "d");

    same(desk.names("/f"), (std::vector<std::string>{"a.txt", "b", "c.txt"}));
    ought(desk.run(desk.files.list(desk.at("/f")))[1].type == Type::folder);
    same(desk.names("/"), (std::vector<std::string>{"f"}));
    ought(desk.refused(desk.files.list(desk.at("/none"))) == tal::Kind::missing);
    ought(desk.refused(desk.files.list(desk.at("/f/a.txt"))) == tal::Kind::other);
    ought(desk.refused(desk.files.list(Path{"f"})) == tal::Kind::malformed);
}

FILES_TRIAL("walk() gives the whole tree, each folder before what it holds", files_trial_3) {
    Desk desk;

    desk.run(desk.files.make(desk.at("/f/a/x")));
    desk.put("/f/a.txt", "1");
    desk.put("/f/a/x/y.txt", "2");

    std::vector<std::string> found;

    for (const tal::fs::Entry& one : desk.run(desk.files.walk(desk.at("/f")))) found.emplace_back(one.where.stored());

    same(found, desk.stored({"/f/a", "/f/a/x", "/f/a/x/y.txt", "/f/a.txt"}));
}

FILES_TRIAL("make() makes every missing folder; a file in the way or a missing folder refuses", files_trial_4) {
    Desk desk;

    desk.run(desk.files.make(desk.at("/a/b/c")));
    desk.run(desk.files.make(desk.at("/a/b")));

    ought(desk.run(desk.files.look(desk.at("/a/b/c"))).type == Type::folder);

    desk.put("/a/file", "x");

    ought(desk.refused(desk.files.make(desk.at("/a/file/d"))) == tal::Kind::exists);
    ought(desk.refused(desk.files.write(desk.at("/none/x.txt"), tal::bytes("x"))) == tal::Kind::missing);
    ought(desk.refused(desk.files.write(desk.at("/a/b"), tal::bytes("x"))) == tal::Kind::other);
    ought(desk.refused(desk.files.write(desk.at("/"), tal::bytes("x"))) == tal::Kind::other);
}

FILES_TRIAL("a write over a file fails, skips, renames or replaces as asked; read-only refuses", files_trial_5) {
    Desk desk;

    desk.put("/a.txt", "first");

    ought(desk.refused(desk.files.write(desk.at("/a.txt"), tal::bytes("x"), {Conflict::fail})) == tal::Kind::exists);

    desk.run(desk.files.write(desk.at("/a.txt"), tal::bytes("skipped"), {Conflict::skip}));
    same(desk.got("/a.txt"), std::string{"first"});

    desk.run(desk.files.write(desk.at("/a.txt"), tal::bytes("second"), {Conflict::rename}));
    desk.run(desk.files.write(desk.at("/a.txt"), tal::bytes("third"), {Conflict::rename}));
    same(desk.got("/a (2).txt"), std::string{"second"});
    same(desk.got("/a (3).txt"), std::string{"third"});

    if constexpr (Desk::manual) desk.time->advance(1h);

    desk.put("/a.txt", "replaced");
    same(desk.got("/a.txt"), std::string{"replaced"});
    ought(desk.near(desk.run(desk.files.look(desk.at("/a.txt"))).modified));

    desk.run(desk.files.change(desk.at("/a.txt"), {tal::fs::Attributes{false, true, false, false}, {}, {}}));
    ought(desk.refused(desk.files.write(desk.at("/a.txt"), tal::bytes("x"))) == tal::Kind::denied);
}

FILES_TRIAL("move() takes a folder with all it holds; into itself or onto what holds it refuses", files_trial_6) {
    Desk desk;

    desk.run(desk.files.make(desk.at("/a/inner")));
    desk.put("/a/inner/x.txt", "x");
    desk.run(desk.files.make(desk.at("/b")));

    desk.run(desk.files.move(desk.at("/a"), desk.at("/b/a")));

    ought(!desk.there("/a"));
    same(desk.got("/b/a/inner/x.txt"), std::string{"x"});
    ought(desk.refused(desk.files.move(desk.at("/b"), desk.at("/b/a/b"))) == tal::Kind::other);
    ought(desk.refused(desk.files.move(desk.at("/b/a/inner"), desk.at("/b/a"), Conflict::replace)) == tal::Kind::other);
    ought(desk.refused(desk.files.move(desk.at("/none"), desk.at("/b/x"))) == tal::Kind::missing);
    // Only the memory's top is a root; the disk's trial folder is not one.
    if constexpr (Desk::manual) ought(desk.refused(desk.files.move(desk.at("/"), desk.at("/b/x"))) == tal::Kind::denied);

    desk.put("/c.txt", "c");
    desk.put("/b/c.txt", "old");

    ought(desk.refused(desk.files.move(desk.at("/c.txt"), desk.at("/b/c.txt"))) == tal::Kind::exists);

    desk.run(desk.files.move(desk.at("/c.txt"), desk.at("/b/c.txt"), Conflict::rename));
    same(desk.got("/b/c (2).txt"), std::string{"c"});

    desk.put("/d.txt", "new");
    desk.run(desk.files.move(desk.at("/d.txt"), desk.at("/b/c.txt"), Conflict::replace));
    same(desk.got("/b/c.txt"), std::string{"new"});
    ought(!desk.there("/d.txt"));
}

FILES_TRIAL("copy() copies a tree, keeps times and counts the bytes in the progress", files_trial_7) {
    Desk desk;

    desk.run(desk.files.make(desk.at("/a/inner")));
    desk.put("/a/one.txt", "12345");
    desk.put("/a/inner/two.txt", "678");

    const auto written = std::chrono::time_point_cast<std::chrono::seconds>(desk.now() - 24h);

    desk.run(desk.files.change(desk.at("/a/one.txt"), {{}, written, {}}));

    tal::Progress progress;

    desk.run(desk.files.copy(desk.at("/a"), desk.at("/b"), {Conflict::fail, &progress}));

    same(desk.got("/b/inner/two.txt"), std::string{"678"});
    same(desk.got("/a/one.txt"), std::string{"12345"});
    ought(desk.run(desk.files.look(desk.at("/b/one.txt"))).modified == written);
    ought(progress.total() >= 8);
    same(progress.done(), progress.total());
    ought(desk.refused(desk.files.copy(desk.at("/a"), desk.at("/a"))) == tal::Kind::exists);
    ought(desk.refused(desk.files.copy(desk.at("/a"), desk.at("/a/inner/a"))) == tal::Kind::other);
    ought(desk.refused(desk.files.copy(desk.at("/a"), desk.at("/b"))) == tal::Kind::exists);
    ought(desk.refused(desk.files.copy(desk.at("/a/inner"), desk.at("/a"), {Conflict::replace, nullptr})) == tal::Kind::other);

    desk.run(desk.files.copy(desk.at("/a"), desk.at("/b"), {Conflict::rename, nullptr}));
    same(desk.got("/b (2)/one.txt"), std::string{"12345"});
}

FILES_TRIAL("remove() takes a folder that holds anything only when deep; the trash keeps it", files_trial_8) {
    Desk desk;

    desk.run(desk.files.make(desk.at("/a/inner")));
    desk.put("/a/inner/x.txt", "x");
    desk.run(desk.files.make(desk.at("/empty")));

    ought(desk.refused(desk.files.remove(desk.at("/a"))) == tal::Kind::other);

    desk.run(desk.files.remove(desk.at("/empty")));
    desk.run(desk.files.remove(desk.at("/a"), {Desk::manual, true}));

    ought(!desk.there("/a"));
    ought(!desk.there("/a/inner/x.txt"));
    ought(!desk.there("/empty"));
    ought(desk.refused(desk.files.remove(desk.at("/a"))) == tal::Kind::missing);

    // The trash of the disk comes with the next step of E1e; only the memory's top is a root.
    if constexpr (Desk::manual) {
        same(desk.memory.trashed().size(), std::size_t{1});
        ought(desk.memory.trashed()[0] == desk.at("/a"));
        ought(desk.refused(desk.files.remove(desk.at("/"))) == tal::Kind::denied);
    }
}

FILES_TRIAL("links are followed on the way and by read and list, looked at by look and remove", files_trial_9) {
    Desk desk;

    // Windows makes links only for a program with the right to; without it there is nothing to try.
    if (!desk.links()) return;

    desk.run(desk.files.make(desk.at("/films/Poirot")));
    desk.put("/films/Poirot/e01.mkv", "odcinek");
    desk.run(desk.files.link(desk.at("/skrót"), desk.at("/films/Poirot")));
    desk.run(desk.files.link(desk.at("/films/względny"), Path{"Poirot/e01.mkv"}));
    desk.run(desk.files.link(desk.at("/zerwany"), desk.at("/nic")));

    same(desk.names("/skrót"), (std::vector<std::string>{"e01.mkv"}));
    ought(desk.run(desk.files.list(desk.at("/skrót")))[0].where == desk.at("/skrót/e01.mkv"));
    same(desk.got("/skrót/e01.mkv"), std::string{"odcinek"});
    same(desk.got("/films/względny"), std::string{"odcinek"});

    const tal::fs::Entry shortcut = desk.run(desk.files.look(desk.at("/skrót")));

    ought(shortcut.type == Type::link);
    ought(shortcut.reach == Type::folder);
    ought(shortcut.target == desk.at("/films/Poirot"));
    ought(desk.run(desk.files.look(desk.at("/films/względny"))).reach == Type::file);
    ought(desk.run(desk.files.look(desk.at("/zerwany"))).reach == Type::other);
    ought(desk.run(desk.files.canonical(desk.at("/skrót/e01.mkv"))) == desk.at("/films/Poirot/e01.mkv"));
    ought(desk.refused(desk.files.canonical(desk.at("/zerwany"))) == tal::Kind::missing);
    ought(desk.refused(desk.files.link(desk.at("/skrót"), desk.at("/x"))) == tal::Kind::exists);

    desk.run(desk.files.remove(desk.at("/skrót")));

    ought(!desk.there("/skrót"));
    same(desk.got("/films/Poirot/e01.mkv"), std::string{"odcinek"});
}

FILES_TRIAL("links that lead in a circle refuse instead of resolving forever", files_trial_10) {
    Desk desk;

    if (!desk.links()) return;

    desk.run(desk.files.link(desk.at("/a"), desk.at("/b")));
    desk.run(desk.files.link(desk.at("/b"), desk.at("/a")));

    ought(desk.refused(desk.files.read(desk.at("/a"))) == tal::Kind::other);
    ought(desk.run(desk.files.look(desk.at("/a"))).reach == Type::other);
}

FILES_TRIAL("change() sets attributes and the time; a root refuses", files_trial_11) {
    Desk desk;
    const auto then = std::chrono::time_point_cast<std::chrono::seconds>(desk.now() - 24h);

    desk.put("/a.sh", "echo");
    desk.run(desk.files.change(desk.at("/a.sh"), {tal::fs::Attributes{true, false, false, true}, then, {}}));

    const tal::fs::Entry entry = desk.run(desk.files.look(desk.at("/a.sh")));

    ought(entry.modified == then);

    // A disk keeps what its system has: POSIX tells a hidden entry by its name and has the right
    // to run, Windows has the hidden flag and tells a program by its extension.
#ifdef _WIN32
    ought(entry.attributes.hidden);
    ought(entry.attributes.executable || !Desk::manual);
#else
    ought(entry.attributes.hidden || !Desk::manual);
    ought(entry.attributes.executable);
#endif

    if constexpr (Desk::manual) ought(desk.refused(desk.files.change(desk.at("/"), {})) == tal::Kind::denied);
    ought(desk.refused(desk.files.change(desk.at("/none"), {})) == tal::Kind::missing);
}

TRIAL("Memory: a watch sees the folder and what is in it, a deep one also what lies below") {
    MemoryDesk desk;

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
    MemoryDesk desk;
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
    MemoryDesk desk;
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
    MemoryDesk desk;
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

    same(tal::text::of(tal::wait(loop, std::move(reading))), std::string{"x"});
}

TRIAL("Memory: known folders are given when they are there; the volumes are its roots") {
    MemoryDesk desk;

    ought(!desk.run(desk.memory.known(tal::fs::Known::music)).has_value());

    desk.run(desk.memory.make(Path{"/home/Music"}));

    ought(desk.run(desk.memory.known(tal::fs::Known::music)) == Path{"/home/Music"});
    ought(desk.run(desk.memory.known(tal::fs::Known::home)) == Path{"/home"});

    const std::vector<tal::fs::Volume> volumes = desk.run(desk.memory.volumes());

    same(volumes.size(), std::size_t{1});
    ought(volumes[0].where == Path{"/"});
    ought(desk.run(desk.memory.volume(Path{"/home/Music"})).where == Path{"/"});
}

TRIAL("fs: the free functions use the program's default adapter, and the disk without one") {
    tal::Loop loop;

    tal::defaults().set<tal::fs::Files>(std::make_shared<tal::fs::Memory>());

    tal::wait(loop, tal::fs::write(Path{"/a.txt"}, tal::bytes("domyślny")));
    same(tal::text::of(tal::wait(loop, tal::fs::read(Path{"/a.txt"}))), std::string{"domyślny"});
    same(tal::wait(loop, tal::fs::list(Path{"/"})).size(), std::size_t{1});

    const tal::fs::Watch watch = tal::fs::watch(Path{"/"});

    tal::wait(loop, tal::fs::remove(Path{"/a.txt"}));
    same(tal::wait(loop, watch.next()).size(), std::size_t{1});

    tal::defaults().clear<tal::fs::Files>();

    const DiskDesk disk;

    ought(tal::Port<tal::fs::Files>{}->name() == "disk");
    ought(tal::wait(loop, tal::fs::list(disk.root)).empty());
}

FILES_TRIAL("an open file reads and writes at a place, pads a gap with zeros and appends at the end", files_trial_12) {
    Desk desk;

    desk.put("/a.bin", "abcdef");

    const tal::fs::File file = desk.run(desk.files.open(desk.at("/a.bin"), tal::fs::Mode::write));

    same(tal::text::of(desk.run(file.read(2, 3))), std::string{"cde"});
    same(tal::text::of(desk.run(file.read(5, 10))), std::string{"f"});
    same(tal::text::of(desk.run(file.read(9, 10))), std::string{});

    desk.run(file.write(1, tal::bytes("XY")));
    desk.run(file.write(8, tal::bytes("Z")));

    same(desk.got("/a.bin"), (std::string{"aXYdef\0\0Z", 9}));
    same(desk.run(file.size()), std::uint64_t{9});

    desk.run(file.flush());

    const tal::fs::File log = desk.run(desk.files.open(desk.at("/log.txt"), tal::fs::Mode::append));

    desk.run(log.write(0, tal::bytes("one ")));
    desk.run(log.write(0, tal::bytes("two")));
    same(desk.got("/log.txt"), std::string{"one two"});
}

FILES_TRIAL("open() reads only what is there, truncates, makes only new files exclusively", files_trial_13) {
    Desk desk;

    desk.put("/a.txt", "content");

    ought(desk.refused(desk.files.open(desk.at("/none.txt"))) == tal::Kind::missing);
    ought(desk.refused(desk.files.open(desk.at("/a.txt"), tal::fs::Mode::exclusive)) == tal::Kind::exists);
    ought(desk.refused(desk.files.open(desk.at("/"))) == tal::Kind::other);
    ought(desk.refused(desk.files.open(desk.at("/none/x"), tal::fs::Mode::write)) == tal::Kind::missing);

    const tal::fs::File reading = desk.run(desk.files.open(desk.at("/a.txt")));

    ought(desk.refused(reading.write(0, tal::bytes("x"))) == tal::Kind::denied);

    (void)desk.run(desk.files.open(desk.at("/a.txt"), tal::fs::Mode::truncate));
    same(desk.got("/a.txt"), std::string{});

    const tal::fs::File fresh = desk.run(desk.files.open(desk.at("/new.txt"), tal::fs::Mode::exclusive));

    desk.run(fresh.write(0, tal::bytes("1")));
    same(desk.got("/new.txt"), std::string{"1"});

    desk.run(desk.files.change(desk.at("/new.txt"), {tal::fs::Attributes{false, true, false, false}, {}, {}}));
    ought(desk.refused(desk.files.open(desk.at("/new.txt"), tal::fs::Mode::append)) == tal::Kind::denied);
}

TRIAL("Memory: a closed file refuses; an open file outlives the memory; a removed one is missing") {
    tal::Loop loop;
    auto memory = std::make_unique<tal::fs::Memory>();

    tal::wait(loop, memory->write(Path{"/a.txt"}, tal::bytes("kept")));

    tal::fs::File file = tal::wait(loop, memory->open(Path{"/a.txt"}));
    tal::fs::File other = tal::wait(loop, memory->open(Path{"/a.txt"}));
    tal::Task<tal::Bytes> later = other.read(0, 4);

    other.close();
    same(tal::text::of(tal::wait(loop, std::move(later))), std::string{"kept"});
    ought(!other.held());

    try {
        (void)tal::wait(loop, other.read(0, 1));
        ought(false);
    } catch (const tal::Trouble& why) {
        ought(why.kind() == tal::Kind::missing);
    }

    memory.reset();
    same(tal::text::of(tal::wait(loop, file.read(0, 4))), std::string{"kept"});

    auto again = std::make_unique<tal::fs::Memory>();

    tal::wait(loop, again->write(Path{"/b.txt"}, tal::bytes("x")));

    const tal::fs::File gone = tal::wait(loop, again->open(Path{"/b.txt"}));

    tal::wait(loop, again->remove(Path{"/b.txt"}));

    try {
        (void)tal::wait(loop, gone.size());
        ought(false);
    } catch (const tal::Trouble& why) {
        ought(why.kind() == tal::Kind::missing);
    }
}

FILES_TRIAL("rights are 644 for files, 755 for folders and change() sets them", files_trial_14) {
    Desk desk;

    desk.put("/script.sh", "echo");
    desk.run(desk.files.make(desk.at("/bin")));

    // The rights of new entries on a disk follow the program's mask, so only the memory's are
    // known; Windows has none to give or to set.
    if constexpr (Desk::manual) {
        ought(desk.run(desk.files.look(desk.at("/script.sh"))).rights == std::uint16_t{0644});
        ought(desk.run(desk.files.look(desk.at("/bin"))).rights == std::uint16_t{0755});
    }

#ifdef _WIN32
    if constexpr (!Desk::manual) {
        ought(!desk.run(desk.files.look(desk.at("/script.sh"))).rights.has_value());
        ought(desk.refused(desk.files.change(desk.at("/script.sh"), {{}, {}, std::uint16_t{0700}})) == tal::Kind::unsupported);

        return;
    }
#endif

    desk.run(desk.files.change(desk.at("/script.sh"), {{}, {}, std::uint16_t{0700}}));

    ought(desk.run(desk.files.look(desk.at("/script.sh"))).rights == std::uint16_t{0700});
    ought(!desk.run(desk.files.look(desk.at("/script.sh"))).attributes.hidden);
}

TRIAL("fs: open() goes through the default adapter too") {
    tal::Loop loop;

    tal::defaults().set<tal::fs::Files>(std::make_shared<tal::fs::Memory>());

    const tal::fs::File file = tal::wait(loop, tal::fs::open(Path{"/a.txt"}, tal::fs::Mode::exclusive));

    tal::wait(loop, file.write(0, tal::bytes("x")));
    same(tal::text::of(tal::wait(loop, tal::fs::read(Path{"/a.txt"}))), std::string{"x"});

    tal::defaults().clear<tal::fs::Files>();
}

// A name that is not UTF-8 is kept by the system as it is, bytes on POSIX and a lone surrogate on
// Windows, and comes back from a listing whole, to be opened again by the same path; shown, each
// broken byte is a replacement character.
TRIAL("Disk: names that are not UTF-8 come back whole") {
    DiskDesk desk;

#ifdef _WIN32
    const std::string odd = "\xED\xA0\x80" "a.txt";
    const std::string shown = "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "a.txt";
#else
    const std::string odd = "\xFF" "a.txt";
    const std::string shown = "\xEF\xBF\xBD" "a.txt";
#endif

    const Path file = desk.root / odd;

    desk.run(desk.files.write(file, tal::bytes("x")));

    const std::vector<tal::fs::Entry> found = desk.run(desk.files.list(desk.root));

    same(found.size(), std::size_t{1});
    ought(found[0].where == file);
    same(found[0].where.text(), desk.root.text() + "/" + shown);
    same(tal::text::of(desk.run(desk.files.read(found[0].where))), std::string{"x"});
}

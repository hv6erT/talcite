#include "tal/testing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <version>

#include "talcite/fs.hpp"
#include "talcite/time.hpp"

#include "looking.hpp"

#ifndef _WIN32
#include <csignal>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#endif

#ifdef _WIN32
#include <vector>

#include <windows.h>

#include <aclapi.h>
#endif

using namespace talcite;
using namespace std::chrono_literals;

namespace {

// The volumes as a file manager gathers them: the roots, then the details of each one asked
// separately.
std::vector<fs::Volume> volumes() {
    std::vector<fs::Volume> made;

    for (const fs::path& one : fs::roots().get()) made.push_back(fs::volume(one).get());

    return made;
}

// The kind of change seen by `Watch`: a short name, because the watching trials ask about it on
// every line.
using Changed = fs::Change::Kind;

namespace stood = std::filesystem;

// A file of the given length without writing content: `resize_file`. libc++ on Windows cuts the
// length in it to 32 bits (its `off_t`), turning 5 GiB into 1 GiB, so there we do through the
// system what MSVC's `resize_file` does.
void stretched(const stood::path& where, std::uintmax_t size, std::error_code& why) {
    why.clear();

#if defined(_WIN32) && defined(_LIBCPP_VERSION)
    const HANDLE file = CreateFileW(where.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        why.assign(static_cast<int>(GetLastError()), std::system_category());

        return;
    }

    FILE_END_OF_FILE_INFO end{};

    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);

    if (SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof end) == 0)
        why.assign(static_cast<int>(GetLastError()), std::system_category());

    CloseHandle(file);
#else
    stood::resize_file(where, size, why);
#endif
}

// A directory for the duration of one case. It cleans up after itself, also when the case fails.
class Nook {
public:
    // Creates a fresh, empty directory with a name unique to this process and object.
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-fs-" + std::to_string(++counted) + "-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this))};

        std::error_code ignored;
        stood::remove_all(where_, ignored);
        stood::create_directories(where_, ignored);
    }

    // Removes the directory with everything in it, ignoring errors.
    ~Nook() {
        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Nook(const Nook&) = delete;
    Nook& operator=(const Nook&) = delete;
    Nook(Nook&&) = delete;
    Nook& operator=(Nook&&) = delete;

    // The path of the directory.
    [[nodiscard]] const fs::path& where() const { return where_; }

    // Creates a file of the given name in the directory with the given content, replacing an
    // existing one, and returns its path.
    [[nodiscard]] fs::path made(const std::string& named, const std::string& held = {}) const {
        const fs::path one = where_ / named;

        std::ofstream making{one, std::ios::binary | std::ios::trunc};
        making.write(held.data(), static_cast<std::streamsize>(held.size()));

        return one;
    }

private:
    fs::path where_;
};

}

TRIAL("fs: exists answers rather than throws") {
    const Nook nook;

    const fs::path one = nook.made("jest.txt", "cokolwiek");

    OUGHT(fs::exists(one).get());
    OUGHT(!fs::exists(nook.where() / "nie ma.txt").get());
}

TRIAL("fs: kind tells a file, a directory and nothing apart") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "x");
    const fs::path two = nook.where() / "katalog";

    fs::make_folder(two).get();

    SAME(static_cast<int>(fs::kind(one).get()), static_cast<int>(fs::Kind::file));
    SAME(static_cast<int>(fs::kind(two).get()), static_cast<int>(fs::Kind::folder));
    SAME(static_cast<int>(fs::kind(nook.where() / "puste").get()),
         static_cast<int>(fs::Kind::none));
}

TRIAL("fs: size measures the content") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", std::string(1234, 'a'));

    SAME(fs::size(one).get(), std::uintmax_t{1234});
}

TRIAL("fs: size on nothing rejects, and the reason carries the path") {
    const Nook nook;

    const fs::path missing = nook.where() / "nie ma.bin";

    bool caught = false;
    fs::path told;

    try {
        (void)fs::size(missing).get();
    } catch (const fs::Trouble& why) {
        caught = true;
        told = why.where();
    }

    OUGHT(caught);
    SAME(told, missing);
}

TRIAL("fs: written points to a moment close to now") {
    const Nook nook;

    const fs::path one = nook.made("świeży.txt", "x");

    const std::chrono::sys_seconds told = fs::modified(one).get();
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now());

    const auto apart = now - told;

    OUGHT(apart > -60s);
    OUGHT(apart < 600s);
}

TRIAL("fs: roomy gives the capacity and the free space") {
    const Nook nook;

    const fs::Space told = fs::space(nook.where()).get();

    OUGHT(told.total > 0);
    OUGHT(told.free > 0);
    OUGHT(told.free <= told.total);
}

TRIAL("fs: slice gives a sensible chunk") {
    const Nook nook;

    const std::size_t told = fs::chunk(nook.where()).get();

    OUGHT(told >= 4096);
    OUGHT(told <= 8 * 1024 * 1024);
    SAME(told & (told - 1), std::size_t{0});
}

TRIAL("fs: make creates the whole path and does not mind that it exists") {
    const Nook nook;

    const fs::path deep = nook.where() / "jeden" / "dwa" / "trzy";

    fs::make_folder(deep).get();

    OUGHT(fs::exists(deep).get());

    fs::make_folder(deep).get();

    OUGHT(fs::exists(deep).get());
}

// The parents are found on the path as given: a link to a folder on the way is entered, a file on
// the way refuses, and so does an empty path, which names no place.
TRIAL("fs: make goes through a link on the way and refuses a file on the way and an empty path") {
    const Nook nook;

    fs::make_folder(nook.where() / "prawdziwy").get();
    fs::link(nook.where() / "skrót", nook.where() / "prawdziwy").get();

    fs::make_folder(nook.where() / "skrót" / "nowy" / "głębiej").get();

    OUGHT(fs::exists(nook.where() / "prawdziwy" / "nowy" / "głębiej").get());

    const fs::path file = nook.made("plik.txt", "x");

    THREW(fs::make_folder(file / "pod").get());
    THREW(fs::make_folder(fs::path{}).get());
}

TRIAL("fs: rename moves") {
    const Nook nook;

    const fs::path one = nook.made("stary.txt", std::string(5, 'x'));
    const fs::path two = nook.where() / "nowy.txt";

    fs::rename(one, two).get();

    OUGHT(!fs::exists(one).get());
    OUGHT(fs::exists(two).get());
    SAME(fs::size(two).get(), std::uintmax_t{5});
}

TRIAL("fs: rename on nothing rejects") {
    const Nook nook;

    THREW(fs::rename(nook.where() / "nie ma", nook.where() / "gdziekolwiek").get());
}

TRIAL("fs: work starts at once, without waiting for the result to be asked for") {
    const Nook nook;

    const fs::path deep = nook.where() / "od razu";

    Promise<void> making = fs::make_folder(deep);

    SOON(fs::exists(deep).get());

    std::move(making).get();
}

TRIAL("fs: queries compose like any other promises") {
    const Nook nook;

    const fs::path one = nook.made("a.txt", "aaa");
    const fs::path two = nook.made("b.txt", "bbbbb");

    Promise<std::uintmax_t> first = fs::size(one);
    Promise<std::uintmax_t> second = fs::size(two);

    const auto [a, b] = all(std::move(first), std::move(second)).get();

    SAME(a + b, std::uintmax_t{8});
}

TRIAL("fs: list gives entries with a description, not just paths") {
    const Nook nook;

    (void)nook.made("b.txt", std::string(11, 'x'));
    (void)nook.made("a.txt", std::string(3, 'x'));

    fs::make_folder(nook.where() / "katalog").get();

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{3});

    SAME(among[0].where.filename(), fs::path{"a.txt"});
    SAME(static_cast<int>(among[0].kind), static_cast<int>(fs::Kind::file));
    SAME(among[0].size, std::uintmax_t{3});

    SAME(among[1].where.filename(), fs::path{"b.txt"});
    SAME(among[1].size, std::uintmax_t{11});

    SAME(among[2].where.filename(), fs::path{"katalog"});
    SAME(static_cast<int>(among[2].kind), static_cast<int>(fs::Kind::folder));

    OUGHT(among[0].modified.time_since_epoch().count() > 1700000000);
    OUGHT(among[0].accessed.time_since_epoch().count() > 1700000000);
}

TRIAL("fs: list on a file rejects") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "x");

    THREW((void)fs::list(one).get());
}

TRIAL("fs: list of an empty directory gives nothing") {
    const Nook nook;

    OUGHT(fs::list(nook.where()).get().empty());
}

// A directory we have no right to read is a refusal when listed, as on Windows, not an empty list
// that would pass for an empty directory; a walk of a tree skips it and gives the rest. Root reads
// everything, so the trial checks this only for an ordinary user.
TRIAL("fs: list of a directory without the right to read it refuses, walk skips it") {
#ifndef _WIN32
    const Nook nook;

    (void)nook.made("widoczny.txt", "x");

    fs::make_folder(nook.where() / "zamknięty").get();

    (void)nook.made("zamknięty/plik.txt", "x");

    if (::geteuid() != 0 && ::chmod((nook.where() / "zamknięty").c_str(), 0) == 0) {
        bool refused = false;

        try {
            (void)fs::list(nook.where() / "zamknięty").get();
        } catch (const std::exception&) {
            refused = true;
        }

        std::vector<fs::Entry> walked;

        try {
            walked = fs::walk(nook.where()).get();
        } catch (const std::exception&) {
        }

        (void)::chmod((nook.where() / "zamknięty").c_str(), 0700);

        OUGHT(refused);
        SAME(walked.size(), std::size_t{2});
    }
#endif
}

TRIAL("fs: walk goes down into the depth") {
    const Nook nook;

    fs::make_folder(nook.where() / "jeden" / "dwa").get();

    (void)nook.made("na wierzchu.txt", "x");

    {
        std::ofstream making{nook.where() / "jeden" / "środkowy.txt", std::ios::binary};
        making << "xx";
    }
    {
        std::ofstream making{nook.where() / "jeden" / "dwa" / "głęboki.txt", std::ios::binary};
        making << "xxx";
    }

    const std::vector<fs::Entry> among = fs::walk(nook.where()).get();

    SAME(among.size(), std::size_t{5});

    std::size_t files = 0;
    std::size_t folders = 0;

    for (const fs::Entry& one : among) {
        if (one.kind == fs::Kind::file) ++files;
        if (one.kind == fs::Kind::folder) ++folders;
    }

    SAME(files, std::size_t{3});
    SAME(folders, std::size_t{2});
}

TRIAL("fs: walk gives an ordered result") {
    const Nook nook;

    fs::make_folder(nook.where() / "z").get();
    fs::make_folder(nook.where() / "a").get();

    const std::vector<fs::Entry> among = fs::walk(nook.where()).get();

    for (std::size_t at = 1; at < among.size(); ++at) OUGHT(among[at - 1].where < among[at].where);
}

TRIAL("fs: one pass instead of a thousand queries") {
    const Nook nook;

    for (int at = 0; at < 200; ++at)
        (void)nook.made("plik-" + std::to_string(at) + ".bin", std::string(at, 'x'));

    Stopwatch held;

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{200});

    std::uintmax_t whole = 0;
    for (const fs::Entry& one : among) whole += one.size;

    SAME(whole, std::uintmax_t{200 * 199 / 2});

    // Two hundred sizes came together with the list, so there are no two hundred trips through the
    // pool here.
    OUGHT(held.spent() < 2000ms);
}

TRIAL("fs: a cancelled walk stops midway") {
    const Nook nook;

    for (int at = 0; at < 60; ++at) {
        const fs::path deep = nook.where() / ("gałąź-" + std::to_string(at));

        fs::make_folder(deep).get();

        for (int inner = 0; inner < 20; ++inner)
            (void)std::ofstream{deep / ("plik-" + std::to_string(inner) + ".bin"),
                                std::ios::binary};
    }

    Promise<std::vector<fs::Entry>> going = fs::walk(nook.where());

    // No waiting: the task is either still in the queue or has only just started. A pause would
    // give the walk a chance to finish before the cancellation, and the case would then measure a
    // race instead of the mechanism.
    going.cancel();
}

namespace {


// The bytes as a string, one character per byte.
std::string said_of(const fs::Bytes& given) {
    std::string made(given.size(), '\0');

    for (std::size_t at = 0; at < given.size(); ++at)
        made[at] = static_cast<char>(std::to_integer<unsigned char>(given[at]));

    return made;
}

}

TRIAL("File: opens for reading and reads a range") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    const fs::File held = fs::open(one).get();

    OUGHT(held.held());
    SAME(held.size().get(), std::uintmax_t{10});

    SAME(said_of(held.read(3, 4).get()), std::string{"3456"});
    SAME(said_of(held.read(0, 10).get()), std::string{"0123456789"});
}

TRIAL("File: a read past the end returns as much as there is") {
    const Nook nook;

    const fs::path one = nook.made("krótki.bin", "abc");

    const fs::File held = fs::open(one).get();

    SAME(said_of(held.read(1, 100).get()), std::string{"bc"});
    OUGHT(held.read(50, 10).get().empty());
}

TRIAL("File: writes at the given place, not from the start") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "aaaaaaaaaa");

    const fs::File held = fs::open(one, fs::Mode::write).get();

    SAME(held.write(4, bytes("XY")).get(), std::size_t{2});

    SAME(said_of(held.read(0, 10).get()), std::string{"aaaaXYaaaa"});
}

TRIAL("File: write creates the file and keeps its content, fresh truncates it") {
    const Nook nook;

    const fs::path one = nook.where() / "nowy.bin";

    {
        const fs::File held = fs::open(one, fs::Mode::write).get();
        (void)held.write(0, bytes("pierwsze")).get();
    }

    SAME(fs::size(one).get(), std::uintmax_t{8});

    {
        const fs::File held = fs::open(one, fs::Mode::write).get();
        (void)held.write(0, bytes("XX")).get();
    }

    SAME(fs::size(one).get(), std::uintmax_t{8});

    {
        const fs::File held = fs::open(one, fs::Mode::truncate).get();
        (void)held.write(0, bytes("XX")).get();
    }

    SAME(fs::size(one).get(), std::uintmax_t{2});
}

TRIAL("File: opening a missing file for reading rejects") {
    const Nook nook;

    THREW((void)fs::open(nook.where() / "nie ma.bin").get());
}

// Copies share one open file, so closing closes it for all of them: a copy stays in every promise
// the file has passed through, and closing only one copy would leave the handle open, on Windows
// long enough for replacing the file to be refused. Without closing, the copies read the same.
TRIAL("File: copies share the same open file, and closing closes it for all") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abcdef");

    fs::File first = fs::open(one).get();
    const fs::File second = first;

    SAME(said_of(second.read(0, 6).get()), std::string{"abcdef"});
    SAME(said_of(first.read(0, 3).get()), std::string{"abc"});

    const fs::Cursor flow = second.cursor();

    first.close();

    OUGHT(!first.held());

    // The refusal says what happened, instead of a read error from a handle that no longer exists.
    const auto refused = [](auto doing) {
        try {
            doing();
        } catch (const fs::Trouble& trouble) {
            return std::string{trouble.what()} == "The file is not open.";
        }

        return false;
    };

    fs::Cursor moving = flow;

    OUGHT(refused([&] { (void)second.read(0, 6).get(); }));
    OUGHT(refused([&] { (void)second.write(0, bytes("x")).get(); }));
    OUGHT(refused([&] { (void)second.size().get(); }));
    OUGHT(refused([&] { second.sync().get(); }));
    OUGHT(refused([&] { (void)moving.read(3).get(); }));

#ifndef _WIN32
    // The descriptor really went back to the system: no descriptor open in the process points to
    // this file any more.
    std::error_code ignored;

    for (const stood::directory_entry& fd : stood::directory_iterator{"/proc/self/fd", ignored})
        OUGHT(stood::read_symlink(fd.path(), ignored) != stood::canonical(one, ignored));
#endif
}

TRIAL("File: a closed file rejects instead of crashing") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abc");

    fs::File held = fs::open(one).get();
    held.close();

    THREW((void)held.read(0, 3).get());
    THREW((void)held.size().get());
}

TRIAL("File: reading in chunks assembles the whole file") {
    const Nook nook;

    std::string whole;
    for (int at = 0; at < 5000; ++at) whole += static_cast<char>('a' + (at % 26));

    const fs::path one = nook.made("duży.bin", whole);

    const fs::File held = fs::open(one).get();

    const std::uintmax_t total = held.size().get();
    const std::size_t rata = 512;

    std::string made;

    for (std::uintmax_t at = 0; at < total; at += rata)
        made += said_of(held.read(at, rata).get());

    SAME(made, whole);
}

namespace {

// A string of the given length cycling through the letters a to z.
std::string ladder(std::size_t much) {
    std::string made;
    made.reserve(much);

    for (std::size_t at = 0; at < much; ++at) made += static_cast<char>('a' + (at % 26));

    return made;
}

}

TRIAL("fs: read takes in the whole file") {
    const Nook nook;

    const std::string whole = ladder(70000);
    const fs::path one = nook.made("duży.bin", whole);

    SAME(said_of(fs::read(one).get()), whole);
}

TRIAL("fs: read of an empty file gives nothing") {
    const Nook nook;

    const fs::path one = nook.made("puste.bin", "");

    OUGHT(fs::read(one).get().empty());
}

TRIAL("fs: read of a missing file rejects") {
    const Nook nook;

    THREW((void)fs::read(nook.where() / "nie ma.bin").get());
}

TRIAL("fs: write and read round-trip") {
    const Nook nook;

    const std::string whole = ladder(70000);
    const fs::path one = nook.where() / "zapis.bin";

    fs::write(one, bytes(whole)).get();

    SAME(fs::size(one).get(), static_cast<std::uintmax_t>(whole.size()));
    SAME(said_of(fs::read(one).get()), whole);
}

TRIAL("fs: write truncates what was there before") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", std::string(5000, 'x'));

    fs::write(one, bytes("krótko")).get();

    SAME(said_of(fs::read(one).get()), std::string{"krótko"});
}

TRIAL("fs: read progress grows and ends at one") {
    const Nook nook;

    const fs::path one = nook.made("duży.bin", ladder(200000));

    Promise<fs::Bytes> going = fs::read(one);

    const double first = going.progress().fraction();

    OUGHT(first >= 0.0 && first <= 1.0);

    SOON(going.ready());

    SAME(going.progress().fraction(), 1.0);

    (void)std::move(going).get();
}

// The bar of a folder copy and of a move counts bytes and entries in one measure: what it shows
// while the pool works never goes back and never passes one, and the end is one. A folder with one
// large file and a few small ones would otherwise jump, once per entry.
TRIAL("fs: folder copy and move progress never goes back and ends at one") {
    const Nook nook;

    std::error_code ignored;

    stood::create_directories(nook.where() / "src" / "inner", ignored);

    (void)nook.made("src/large.bin", ladder(6000000));
    (void)nook.made("src/a.txt", "a");
    (void)nook.made("src/inner/b.txt", "bb");

    const auto watched = [](Promise<void> going) {

        double last = 0.0;
        bool steady = true;

        for (int tries = 0; tries < 100000; ++tries) {
            const double now = going.progress().fraction();

            if (now < last || now > 1.0) steady = false;

            last = now;

            if (now >= 1.0) break;

            std::this_thread::yield();
        }

        SOON(going.ready());

        SAME(going.progress().fraction(), 1.0);

        std::move(going).get();

        OUGHT(steady);
    };

    watched(fs::copy(nook.where() / "src", nook.where() / "copy"));

    SAME(stood::file_size(nook.where() / "copy" / "large.bin"), std::uintmax_t{6000000});

    watched(fs::move(nook.where() / "copy", nook.where() / "moved"));

    OUGHT(!stood::exists(nook.where() / "copy"));
    SAME(stood::file_size(nook.where() / "moved" / "inner" / "b.txt"), std::uintmax_t{2});
}

TRIAL("fs: copy reproduces a file to the byte") {
    const Nook nook;

    const std::string whole = ladder(70000);
    const fs::path from = nook.made("źródło.bin", whole);
    const fs::path to = nook.where() / "cel.bin";

    fs::copy(from, to).get();

    SAME(fs::size(to).get(), static_cast<std::uintmax_t>(whole.size()));
    SAME(said_of(fs::read(to).get()), whole);
    OUGHT(fs::exists(from).get());
}

// A copy that would end in success without a part of the tree is not a copy. A pipe in the tree
// (opening it would hang the copy) and a folder that cannot be read refuse the whole copy before
// the first byte, and no target is left.
TRIAL("fs: a copy of a tree with a pipe or an unreadable folder refuses before writing") {
#ifndef _WIN32
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    const fs::path to = nook.where() / "odbitka";

    fs::make_folder(from / "w głąb").get();

    {
        std::ofstream making{from / "w głąb" / "plik.txt", std::ios::binary};
        making << "treść";
    }

    if (::mkfifo((from / "w głąb" / "potok").c_str(), 0600) == 0) {
        THREW(fs::copy(from, to).get());
        OUGHT(!stood::exists(to));

        std::error_code ignored;

        stood::remove(from / "w głąb" / "potok", ignored);
    }

    fs::make_folder(from / "zamknięty").get();

    if (::geteuid() != 0 && ::chmod((from / "zamknięty").c_str(), 0) == 0) {
        THREW(fs::copy(from, to).get());
        OUGHT(!stood::exists(to));

        (void)::chmod((from / "zamknięty").c_str(), 0700);
    }

    fs::copy(from, to).get();

    SAME(said_of(fs::read(to / "w głąb" / "plik.txt").get()), std::string{"treść"});
#endif
}

// A file that someone keeps writing while it is copied is caught by the copy's own check: the bytes
// copied, the length and the write time no longer agree with the source as it was opened. The copy
// starts once more, and when the source changes again it refuses, leaving no target and the source
// as it is.
TRIAL("fs: a copy of a file that keeps changing refuses and leaves no target") {
    const Nook nook;

    const fs::path from = nook.made("rosnący.bin", std::string(std::size_t{16} << 20, 'x'));
    const fs::path to = nook.where() / "kopia.bin";

    std::atomic<bool> enough{false};
    std::atomic<bool> growing{false};

    std::thread writer{[&from, &enough, &growing] {
        std::ofstream more{from, std::ios::binary | std::ios::app};

        while (!enough.load()) {
            more << 'y';
            more.flush();
            growing.store(true);
        }
    }};

    // The copy starts only once the file is growing: on a busy machine the writer's thread may
    // start later than a copy of sixteen megabytes ends. It may also stand still for as long as a
    // whole copy takes, which then rightly ends whole; the trial tries again. Five tries were too
    // few on two busy cores, where all five copies once ended whole; it tries for twenty seconds.
    while (!growing.load()) std::this_thread::yield();

    std::string said;

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         said.empty() && std::chrono::steady_clock::now() < latest;) {
        try {
            fs::copy(from, to).get();
            fs::erase(to).get();
        } catch (const fs::Trouble& why) {
            said = why.what();
        }
    }

    enough.store(true);
    writer.join();

    SAME(said, std::string{"The file changed while it was being copied."});
    OUGHT(!stood::exists(to));
    OUGHT(stood::file_size(from) > (std::uintmax_t{16} << 20));
}

TRIAL("fs: copy carries the whole tree") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from / "w głąb" / "głębiej").get();

    {
        std::ofstream making{from / "pierwszy.txt", std::ios::binary};
        making << "jeden";
    }
    {
        std::ofstream making{from / "w głąb" / "drugi.txt", std::ios::binary};
        making << "dwa";
    }
    {
        std::ofstream making{from / "w głąb" / "głębiej" / "trzeci.txt", std::ios::binary};
        making << "trzy";
    }

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).get();

    SAME(said_of(fs::read(to / "pierwszy.txt").get()), std::string{"jeden"});
    SAME(said_of(fs::read(to / "w głąb" / "drugi.txt").get()), std::string{"dwa"});
    SAME(said_of(fs::read(to / "w głąb" / "głębiej" / "trzeci.txt").get()),
         std::string{"trzy"});

    SAME(fs::walk(to).get().size(), fs::walk(from).get().size());
}

// Waits until the copy has really started; otherwise the cancellation would hit the task before the
// pool and the case would pass without checking anything. We look at the target, not at the
// progress: the progress of a settled promise is one also after a rejection, so it does not tell
// work in progress from work that never started. Returns whether the target grew past the given
// size within ten seconds.
bool grew(const fs::path& to, std::uintmax_t past) {
    return tal::testing::came([&] {
        std::error_code ignored;

        return stood::exists(to, ignored) && stood::file_size(to, ignored) > past;
    });
}

// The cancellation may hit the copy before the first chunk or already after the last; that cannot
// be forced. The contract is phrased differently and always holds: the target is either complete or
// absent.
TRIAL("fs: a cancelled copy leaves the target complete or absent") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.where() / "cel.bin";

    Promise<void> going = fs::copy(from, to);

    OUGHT(grew(to, 0));

    going.cancel();

    later(300ms).get();

    if (fs::exists(to).get()) SAME(fs::size(to).get(), fs::size(from).get());
}

// A folder copy that does not finish takes back everything it created, its subfolders too, so the
// target is gone rather than left as an empty skeleton. The copy is caught inside a large file in a
// subfolder it has just created and cancelled there; a copy that finished first leaves the whole.
TRIAL("fs: a cancelled folder copy takes back the folders it created") {
    const Nook nook;

    const fs::path from = nook.where() / "serial";
    const fs::path to = nook.where() / "kopia";
    constexpr std::size_t whole = 128 * 1024 * 1024;

    stood::create_directories(from / "sezon 1");

    (void)nook.made("serial/sezon 1/odcinek.mkv", std::string(whole, 'x'));

    Promise<void> going = fs::copy(from, to);

    OUGHT(grew(to / "sezon 1" / "odcinek.mkv", 0));

    going.cancel();

    later(300ms).get();

    if (fs::exists(to).get()) SAME(fs::size(to / "sezon 1" / "odcinek.mkv").get(), std::uintmax_t{whole});
}

TRIAL("fs: a copy abandoned before starting creates no target") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.where() / "cel.bin";

    Promise<void> going = fs::copy(from, to);

    going.cancel();

    later(300ms).get();

    if (fs::exists(to).get()) SAME(fs::size(to).get(), fs::size(from).get());
}

// Replacing writes alongside (`.cel.bin.XXXXXX.part`) and only renames the whole onto the target,
// so a cancellation halfway leaves the target with its old content and leaves no file alongside at
// all.
TRIAL("fs: a cancelled copy leaves an existing target alone") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.made("cel.bin", "swoje");

    Promise<void> going = fs::copy(from, to, fs::Conflict::overwrite);

    // The copy goes to a file alongside first; a copy so quick that it is already done needs no
    // waiting either.
    (void)tal::testing::came([&] {
        for (const fs::Entry& one : fs::list(nook.where()).get())
            if (fs::to_string(one.where.filename()).find(".part") != std::string::npos) return true;

        return going.ready();
    });

    going.cancel();

    later(300ms).get();

    const std::string now = said_of(fs::read(to).get());

    OUGHT(now == "swoje" || now.size() == 8000000);

    for (const fs::Entry& one : fs::list(nook.where()).get())
        OUGHT(fs::to_string(one.where.filename()).find(".part") == std::string::npos);
}

// A file onto a directory with replacing: writing alongside succeeds, but renaming onto the
// directory does not. A refusal, and no trace of the file alongside.
TRIAL("fs: copying a file onto a directory refuses and leaves no file alongside") {
    const Nook nook;

    const fs::path from = nook.made("film.mkv", "film");
    const fs::path to = nook.where() / "katalog";

    fs::make_folder(to).get();

    THREW(fs::copy(from, to, fs::Conflict::overwrite).get());

    OUGHT(fs::kind(to).get() == fs::Kind::folder);

    for (const fs::Entry& one : fs::list(nook.where()).get())
        OUGHT(fs::to_string(one.where.filename()).find(".part") == std::string::npos);
}

// A second hard link to the replaced file keeps the old content: replacing gives the target a new
// file rather than rewriting the old one in place.
TRIAL("fs: replacing does not rewrite a second link") {
    const Nook nook;

    const fs::path from = nook.made("nowy.txt", "nowa treść");
    const fs::path to = nook.made("cel.txt", "stara");
    const fs::path twin = nook.where() / "bliźniak.txt";

    std::error_code trouble;

    stood::create_hard_link(to, twin, trouble);

    if (trouble) return;

    fs::copy(from, to, fs::Conflict::overwrite).get();

    SAME(said_of(fs::read(to).get()), std::string{"nowa treść"});
    SAME(said_of(fs::read(twin).get()), std::string{"stara"});
}

// A directory moved into its own interior: the system refuses the rename, and `move` must not take
// that for a different volume, copy the tree into itself and delete the source, which would lose
// everything while the promise said "done". It refuses, and nothing moves.
TRIAL("fs: moving a directory into its own interior refuses and touches nothing") {
    const Nook nook;

    const fs::path from = nook.where() / "biblioteka";

    fs::make_folder(from / "wnętrze").get();

    const fs::path film = nook.made("biblioteka/film.mkv", "film");

    std::string said;

    try {
        fs::move(from, from / "wnętrze" / "nowe").get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("into itself") != std::string::npos);
    SAME(said_of(fs::read(film).get()), std::string{"film"});
    OUGHT(!fs::exists(from / "wnętrze" / "nowe").get());

    THREW(fs::rename(from, from / "nowe").get());
    OUGHT(fs::exists(film).get());
}

// A directory overwritten with a directory merges with it entry by entry: the files of both stay, a
// shared name gets the content of the moved one, and the source disappears.
TRIAL("fs: moving a directory onto a directory with overwrite merges both") {
    const Nook nook;

    const fs::path from = nook.where() / "nowy";
    const fs::path to = nook.where() / "stary";

    fs::make_folder(from / "w głąb").get();
    fs::make_folder(to).get();

    (void)nook.made("nowy/wspólny.txt", "nowa");
    (void)nook.made("nowy/w głąb/tylko nowy.txt", "n");
    (void)nook.made("stary/wspólny.txt", "stara");
    (void)nook.made("stary/tylko stary.txt", "s");

    fs::move(from, to, fs::Conflict::overwrite).get();

    OUGHT(!fs::exists(from).get());
    SAME(said_of(fs::read(to / "wspólny.txt").get()), std::string{"nowa"});
    SAME(said_of(fs::read(to / "tylko stary.txt").get()), std::string{"s"});
    SAME(said_of(fs::read(to / "w głąb" / "tylko nowy.txt").get()), std::string{"n"});

    // Merging within a volume is renames: it does not ask about free space, so a file larger than
    // all the free space (sparse, taking almost nothing on disk) passes.
    const fs::path big = nook.where() / "duży";

    fs::make_folder(big).get();
    fs::make_folder(nook.where() / "pełny").get();

    std::error_code trouble;

    const stood::space_info room = stood::space(nook.where(), trouble);

    if (trouble) return;

    stretched(nook.made("duży/film.mkv", ""), room.available + (std::uintmax_t{1} << 30), trouble);

    if (trouble) return;

    fs::move(big, nook.where() / "pełny", fs::Conflict::overwrite).get();

    OUGHT(fs::exists(nook.where() / "pełny" / "film.mkv").get());
    OUGHT(!fs::exists(big).get());
}

// Exclusive creation: the same file a second time refuses and says why, and the first one stays.
TRIAL("fs: Mode::exclusive creates only a new file") {
    const Nook nook;

    const fs::path one = nook.where() / "numer.json";

    {
        const fs::File file = fs::open(one, fs::Mode::exclusive).get();

        SAME(file.write(0, bytes("pierwszy")).get(), std::size_t{8});

        file.sync().get();
    }

    std::string said;

    try {
        (void)fs::open(one, fs::Mode::exclusive).get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("already exists") != std::string::npos);
    SAME(said_of(fs::read(one).get()), std::string{"pierwszy"});
}

// A volume tells which file system it has: the check before a copy asks about the FAT32 limit. The
// portable path does not know the file system's name and says so with an empty string.
#if !defined(TALCITE_GENERIC_POSIX)
TRIAL("fs: the list of volumes knows the file system") {
    bool told = false;

    for (const fs::Volume& one : volumes())
        if (!one.format.empty()) told = true;

    OUGHT(told);
}
#endif

// The list of volumes is never empty: at least the root, and on Windows the drive letter of the
// system.
TRIAL("fs: the list of volumes is not empty") {
    const std::vector<fs::Volume> found = volumes();

    OUGHT(!found.empty());

    for (const fs::Volume& one : found) OUGHT(!one.where.empty());
}

TRIAL("fs: copy of a missing file rejects") {
    const Nook nook;

    THREW(fs::copy(nook.where() / "nie ma.bin", nook.where() / "cel.bin").get());
}

TRIAL("fs: move within a volume takes the source away") {
    const Nook nook;

    const std::string whole = ladder(3000);
    const fs::path from = nook.made("źródło.bin", whole);
    const fs::path to = nook.where() / "cel.bin";

    fs::move(from, to).get();

    OUGHT(!fs::exists(from).get());
    SAME(said_of(fs::read(to).get()), whole);
}

TRIAL("fs: move carries a directory too") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from / "w głąb").get();

    {
        std::ofstream making{from / "w głąb" / "plik.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path to = nook.where() / "gdzie indziej";

    fs::move(from, to).get();

    OUGHT(!fs::exists(from).get());
    SAME(said_of(fs::read(to / "w głąb" / "plik.txt").get()), std::string{"treść"});
}

TRIAL("fs: move of a missing file rejects") {
    const Nook nook;

    THREW(fs::move(nook.where() / "nie ma.bin", nook.where() / "cel.bin").get());
}

TRIAL("fs: erase removes a single file") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "cokolwiek");

    fs::erase(one).get();

    OUGHT(!fs::exists(one).get());
}

TRIAL("fs: erase removes a whole tree") {
    const Nook nook;

    const fs::path where = nook.where() / "drzewo";
    fs::make_folder(where / "w głąb" / "głębiej").get();

    {
        std::ofstream making{where / "w głąb" / "plik.txt", std::ios::binary};
        making << "treść";
    }

    fs::erase(where).get();

    OUGHT(!fs::exists(where).get());
}

TRIAL("fs: erasing nothing is not an error") {
    const Nook nook;

    fs::erase(nook.where() / "nie ma").get();
}

TRIAL("fs: erase progress ends at one") {
    const Nook nook;

    const fs::path where = nook.where() / "drzewo";
    fs::make_folder(where / "w głąb").get();

    for (int at = 0; at < 20; ++at) {
        std::ofstream making{where / "w głąb" / ("plik" + std::to_string(at) + ".txt"),
                             std::ios::binary};
        making << at;
    }

    Promise<void> going = fs::erase(where);

    const double first = going.progress().fraction();

    OUGHT(first >= 0.0 && first <= 1.0);

    SOON(going.ready());

    SAME(going.progress().fraction(), 1.0);

    std::move(going).get();
}

namespace {

// Links on Windows need a privilege that the test run does not have to have. When creating one
// fails, the case ends without a verdict instead of falsely accusing the module. Wine accepts a
// link and creates nothing, hence the question whether it really stands. Returns whether the link
// exists.
bool linked(const fs::path& where, const fs::path& onto, bool folder = false) {
    std::error_code trouble;

    if (folder) stood::create_directory_symlink(onto, where, trouble);
    else stood::create_symlink(onto, where, trouble);

    return !trouble && stood::is_symlink(stood::symlink_status(where, trouble)) && !trouble;
}

}

TRIAL("fs: unmade removes an empty folder, and a missing one counts as removed") {
    const Nook nook;

    const fs::path where = nook.where() / "pusty";
    fs::make_folder(where).get();

    fs::unmade(where).get();

    OUGHT(!stood::exists(where));

    fs::unmade(where).get();
    fs::unmade(nook.where() / "nie ma" / "wcale").get();
}

// A folder with anything inside is refused, and what is inside stays: unlike `erase`, `unmade`
// never descends. An empty subfolder is enough to refuse.
TRIAL("fs: unmade refuses a folder with anything inside and leaves it whole") {
    const Nook nook;

    const fs::path where = nook.where() / "pełny";
    fs::make_folder(where / "w środku").get();

    std::string said;

    try {
        fs::unmade(where).get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("not empty") != std::string::npos);
    OUGHT(stood::is_directory(where / "w środku"));

    const fs::path file = nook.made("pełny/w środku/plik.txt", "treść");

    THREW(fs::unmade(where / "w środku").get());
    SAME(said_of(fs::read(file).get()), std::string{"treść"});
}

// A file is not a folder, and neither is a link, even one to a folder: removing the link would
// look like success while the folder it points to stays.
TRIAL("fs: unmade refuses a file and a link, also a link to a folder") {
    const Nook nook;

    const fs::path file = nook.made("plik.txt", "x");

    THREW(fs::unmade(file).get());
    OUGHT(stood::exists(file));

    const fs::path folder = nook.where() / "cel";
    fs::make_folder(folder).get();

    const fs::path link = nook.where() / "skrót";

    if (!linked(link, folder, true)) return;

    THREW(fs::unmade(link).get());

    std::error_code ignored;

    OUGHT(stood::is_symlink(stood::symlink_status(link, ignored)));
    OUGHT(stood::is_directory(folder));
}

// A link to a directory is a directory to enter, as in MC: `list` and `walk` follow it when it
// stands at the very root. Deeper in the walk, links stay links: `walk` does not expand them,
// because a link to an ancestor would close a loop. A link to a file and a dangling one still
// reject.
TRIAL("fs: list and walk enter through a link to a directory") {
    const Nook nook;

    fs::make_folder(nook.where() / "katalog" / "głąb").get();

    {
        std::ofstream making{nook.where() / "katalog" / "plik.txt", std::ios::binary};
        making << "x";
    }

    if (!linked(nook.where() / "skrót", nook.where() / "katalog", true)) return;

    const std::vector<fs::Entry> listed = fs::list(nook.where() / "skrót").get();

    SAME(listed.size(), std::size_t{2});
    SAME(listed[1].where, nook.where() / "skrót" / "plik.txt");

    // `reach` says where an entry leads: an ordinary entry to where it stands, a link to its
    // target.
    const std::vector<fs::Entry> top = fs::list(nook.where()).get();

    SAME(top.size(), std::size_t{2});
    SAME(top[0].kind, fs::Kind::folder);
    SAME(top[0].reach, fs::Kind::folder);
    SAME(top[1].kind, fs::Kind::link);
    SAME(top[1].reach, fs::Kind::folder);

    OUGHT(linked(nook.where() / "katalog" / "głąb" / "w górę", nook.where() / "katalog", true));

    const std::vector<fs::Entry> walked = fs::walk(nook.where() / "skrót").get();

    SAME(walked.size(), std::size_t{3});

    std::size_t links = 0;

    for (const fs::Entry& one : walked)
        if (one.kind == fs::Kind::link) ++links;

    SAME(links, std::size_t{1});

    OUGHT(linked(nook.where() / "do pliku", nook.where() / "katalog" / "plik.txt"));
    OUGHT(linked(nook.where() / "wiszące", nook.where() / "nie ma"));

    const std::vector<fs::Entry> all = fs::list(nook.where()).get();

    SAME(all[0].reach, fs::Kind::file);
    SAME(all[3].kind, fs::Kind::link);
    SAME(all[3].reach, fs::Kind::none);

    THREW((void)fs::list(nook.where() / "do pliku").get());
    THREW((void)fs::walk(nook.where() / "wiszące").get());
}

TRIAL("fs: copy refuses when the target exists") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    THREW(fs::copy(from, to).get());

    SAME(said_of(fs::read(to).get()), std::string{"stare"});
}

TRIAL("fs: copy with overwrite overwrites") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Conflict::overwrite).get();

    SAME(said_of(fs::read(to).get()), std::string{"nowe"});
}

TRIAL("fs: copy with skip leaves the target alone") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Conflict::skip).get();

    SAME(said_of(fs::read(to).get()), std::string{"stare"});
}

TRIAL("fs: copy with beside creates a name alongside") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Conflict::rename).get();

    SAME(said_of(fs::read(to).get()), std::string{"stare"});
    SAME(said_of(fs::read(nook.where() / "cel (2).bin").get()), std::string{"nowe"});

    fs::copy(from, to, fs::Conflict::rename).get();

    SAME(said_of(fs::read(nook.where() / "cel (3).bin").get()), std::string{"nowe"});
}

// A name near the longest a folder takes: the file beside it for an overwrite gives up the end of
// the name, at a whole letter, so the overwrite does not fail as too long. A rename beside such a
// name still refuses ("fs: rename with an unacceptable name alongside says it did not move").
TRIAL("fs: an overwrite of a name near the longest still succeeds") {
    const Nook nook;

    // 250 bytes in UTF-8 and 248 units in UTF-16: the end falls inside the last "ł" in bytes, after
    // the first one in units.
    const std::string stem = std::string(246, 'a') + "łł";

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made(stem + ".bin", "stare");

    fs::copy(from, to, fs::Conflict::overwrite).get();

    SAME(said_of(fs::read(to).get()), std::string{"nowe"});

    std::size_t many = 0;

    for ([[maybe_unused]] const std::filesystem::directory_entry& one : std::filesystem::directory_iterator{nook.where()}) ++many;

    SAME(many, std::size_t{2});
}

TRIAL("fs: the policy applies to every entry of the tree separately") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).get();

    {
        std::ofstream making{from / "jeden.txt", std::ios::binary};
        making << "nowe";
    }
    {
        std::ofstream making{from / "dwa.txt", std::ios::binary};
        making << "nowe";
    }

    const fs::path to = nook.where() / "cel";
    fs::make_folder(to).get();

    {
        std::ofstream making{to / "jeden.txt", std::ios::binary};
        making << "stare";
    }

    fs::copy(from, to, fs::Conflict::skip).get();

    SAME(said_of(fs::read(to / "jeden.txt").get()), std::string{"stare"});
    SAME(said_of(fs::read(to / "dwa.txt").get()), std::string{"nowe"});
}

// A copy onto a folder that stands merges with it: a subfolder that stood too asks about every
// file in it, however deep, while a subfolder the copy creates takes its files as they are, since
// nothing stands in it.
TRIAL("fs: a merging copy asks in the folders that stood and not in the ones it made") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    const fs::path to = nook.where() / "cel";

    for (const fs::path& one : {from / "stary" / "głębiej", from / "nowy" / "głębiej", to / "stary" / "głębiej"})
        fs::make_folder(one).get();

    for (const fs::path& one : {from / "stary" / "a.txt", from / "stary" / "głębiej" / "b.txt", from / "nowy" / "c.txt",
                                from / "nowy" / "głębiej" / "d.txt"}) {
        std::ofstream making{one, std::ios::binary};
        making << "nowe";
    }

    for (const fs::path& one : {to / "stary" / "a.txt", to / "stary" / "głębiej" / "b.txt"}) {
        std::ofstream making{one, std::ios::binary};
        making << "stare";
    }

    fs::copy(from, to, fs::Conflict::rename).get();

    SAME(said_of(fs::read(to / "stary" / "a.txt").get()), std::string{"stare"});
    SAME(said_of(fs::read(to / "stary" / "a (2).txt").get()), std::string{"nowe"});
    SAME(said_of(fs::read(to / "stary" / "głębiej" / "b.txt").get()), std::string{"stare"});
    SAME(said_of(fs::read(to / "stary" / "głębiej" / "b (2).txt").get()), std::string{"nowe"});
    SAME(said_of(fs::read(to / "nowy" / "c.txt").get()), std::string{"nowe"});
    SAME(said_of(fs::read(to / "nowy" / "głębiej" / "d.txt").get()), std::string{"nowe"});
    OUGHT(!fs::exists(to / "nowy" / "c (2).txt").get());

    // Into a fresh folder: the whole tree arrives as it was.
    const fs::path anew = nook.where() / "świeży";

    fs::copy(from, anew, fs::Conflict::fail).get();

    for (const fs::path& one : {anew / "stary" / "a.txt", anew / "stary" / "głębiej" / "b.txt", anew / "nowy" / "c.txt",
                                anew / "nowy" / "głębiej" / "d.txt"})
        SAME(said_of(fs::read(one).get()), std::string{"nowe"});
}

TRIAL("fs: move refuses when the target exists") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    THREW(fs::move(from, to).get());

    OUGHT(fs::exists(from).get());
    SAME(said_of(fs::read(to).get()), std::string{"stare"});
}

TRIAL("fs: rename refuses when the target exists") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    THREW(fs::rename(from, to).get());

    SAME(said_of(fs::read(to).get()), std::string{"stare"});
}

// A collision on rename is settled the same way as on copy: `skip` leaves both files, `rename` puts
// the file alongside (also with Polish letters, because the name alongside is composed in the
// native form of the path), and `overwrite` replaces.
TRIAL("fs: rename and move settle a collision by the policy") {
    const Nook nook;

    for (const bool moving : {false, true}) {
        const auto shifted = [moving](const fs::path& from, const fs::path& to, fs::Conflict how) {
            return moving ? fs::move(from, to, how) : fs::rename(from, to, how);
        };

        const fs::path from = nook.made("źródło.bin", "nowe");
        const fs::path to = nook.made("Gorączka.bin", "stare");

        shifted(from, to, fs::Conflict::skip).get();

        OUGHT(fs::exists(from).get());
        SAME(said_of(fs::read(to).get()), std::string{"stare"});

        shifted(from, to, fs::Conflict::rename).get();

        OUGHT(!fs::exists(from).get());
        SAME(said_of(fs::read(nook.where() / "Gorączka (2).bin").get()), std::string{"nowe"});

        const fs::path again = nook.made("źródło.bin", "trzecie");

        shifted(again, to, fs::Conflict::rename).get();

        SAME(said_of(fs::read(nook.where() / "Gorączka (3).bin").get()), std::string{"trzecie"});

        const fs::path last = nook.made("źródło.bin", "ostatnie");

        shifted(last, to, fs::Conflict::overwrite).get();

        OUGHT(!fs::exists(last).get());
        SAME(said_of(fs::read(to).get()), std::string{"ostatnie"});

        fs::erase(nook.where() / "Gorączka (2).bin").get();
        fs::erase(nook.where() / "Gorączka (3).bin").get();
        fs::erase(to).get();
    }
}

// An empty directory is also an occupied target. `std::filesystem::rename` on POSIX replaces it
// silently, so the system itself is asked about an occupied target, in the same call as the move.
TRIAL("fs: rename and move do not replace an empty directory") {
    const Nook nook;

    const fs::path from = nook.where() / "źródło";
    const fs::path to = nook.where() / "cel";

    fs::make_folder(from).get();
    fs::make_folder(to).get();
    const fs::path inside = nook.made("źródło/plik.txt", "treść");

    THREW(fs::rename(from, to).get());
    THREW(fs::move(from, to).get());

    OUGHT(fs::exists(inside).get());
    SAME(fs::list(to).get().size(), std::size_t{0});
}

// A name alongside that the system does not accept (too long) ends the search at once and says what
// happened: further numbers would be just as long, and saying "there is no free name" would be
// untrue.
//
// The library itself writes the target: the path in the Windows temporary directory exceeds 260
// characters, and `std::ofstream` under MSVC without long paths does not create the file, so the
// rename would find a free place and succeed. At the end the target goes back under a short name,
// so that cleaning up the directory removes it.
TRIAL("fs: rename with an unacceptable name alongside says it did not move") {
    const Nook nook;

    const std::string stem(250, 'a');
    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.where() / (stem + ".bin");

    fs::write(to, bytes("stare")).get();

    OUGHT(fs::exists(to).get());

    std::string said;

    try {
        fs::rename(from, to, fs::Conflict::rename).get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("Cannot rename") != std::string::npos);
    OUGHT(fs::exists(from).get());

    fs::rename(to, nook.where() / "stare.bin").get();
}

#ifdef _WIN32

// A file beyond 260 characters of path: `std::filesystem` under MSVC without long paths says it
// "does not exist" without an error. The library sees it anyway: it exists, it is a file, and a
// write with `fail` leaves it alone.
TRIAL("fs: on Windows a file beyond 260 characters of path exists and is not a free place") {
    const Nook nook;

    const fs::path deep = nook.where() / (std::string(250, 'b') + ".bin");

    fs::write(deep, bytes("stare")).get();

    OUGHT(fs::exists(deep).get());
    SAME(static_cast<int>(fs::kind(deep).get()), static_cast<int>(fs::Kind::file));

    THREW(fs::write(deep, bytes("nowe"), fs::Conflict::fail).get());

    SAME(said_of(fs::read(deep).get()), std::string{"stare"});

    fs::rename(deep, nook.where() / "krótka.bin").get();
}

#endif

#ifndef _WIN32

// Between volumes the system refuses a rename before it even looks at the target, so `move` settles
// the collision once more before the copy overwrites anything. The second volume is `/dev/shm` when
// it exists and lies elsewhere than the trial's directory; otherwise there is nothing to check.
TRIAL("fs: move between volumes settles a collision before copying") {
    const Nook nook;

    const fs::path away = fs::path{"/dev/shm"} / ("talcite-nosnik-" + std::to_string(::getpid()));

    struct stat here {};
    struct stat there {};

    if (::stat(nook.where().c_str(), &here) != 0 || ::stat("/dev/shm", &there) != 0 || here.st_dev == there.st_dev)
        return;

    std::error_code trouble;

    stood::remove_all(away, trouble);
    stood::create_directories(away, trouble);

    if (trouble) return;

    const fs::path from = nook.made("film.bin", "nowe");
    const fs::path to = away / "film.bin";

    std::ofstream{to, std::ios::binary} << "stare";

    THREW(fs::move(from, to).get());

    OUGHT(fs::exists(from).get());
    SAME(said_of(fs::read(to).get()), std::string{"stare"});

    fs::move(from, to, fs::Conflict::rename).get();

    OUGHT(!fs::exists(from).get());
    SAME(said_of(fs::read(away / "film (2).bin").get()), std::string{"nowe"});

    stood::remove_all(away, trouble);
}

namespace {

// A second volume for the trials: a fresh directory on `/dev/shm`, when it lies on a different
// device than the trial's directory; otherwise an empty path.
fs::path elsewhere(const Nook& nook, const std::string& name) {
    struct stat here {};
    struct stat there {};

    if (::stat(nook.where().c_str(), &here) != 0 || ::stat("/dev/shm", &there) != 0 || here.st_dev == there.st_dev)
        return {};

    const fs::path away = fs::path{"/dev/shm"} / (name + "-" + std::to_string(::getpid()));

    std::error_code trouble;

    stood::remove_all(away, trouble);
    stood::create_directories(away, trouble);

    return trouble ? fs::path{} : away;
}

}

// Between volumes a directory moves entry by entry: everything is at the target with its
// modification times, and the source is gone.
TRIAL("fs: moving a directory between volumes carries the whole") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-drzewo");

    if (away.empty()) return;

    const fs::path from = nook.where() / "seriale";

    fs::make_folder(from / "Sezon 01").get();

    const fs::path episode = nook.made("seriale/Sezon 01/S01E01.mkv", ladder(5000));
    (void)nook.made("seriale/tvshow.nfo", "<tvshow/>");

    const auto when = std::chrono::sys_seconds{std::chrono::seconds{1500000000}};
    const auto season = std::chrono::sys_seconds{std::chrono::seconds{1400000000}};
    const auto show = std::chrono::sys_seconds{std::chrono::seconds{1300000000}};

    fs::modified(episode, when).get();
    fs::modified(from / "Sezon 01", season).get();
    fs::modified(from, show).get();

    fs::move(from, away / "seriale").get();

    OUGHT(!fs::exists(from).get());
    SAME(said_of(fs::read(away / "seriale" / "Sezon 01" / "S01E01.mkv").get()), ladder(5000));
    SAME(said_of(fs::read(away / "seriale" / "tvshow.nfo").get()), std::string{"<tvshow/>"});
    OUGHT(fs::modified(away / "seriale" / "Sezon 01" / "S01E01.mkv").get() == when);
    OUGHT(fs::modified(away / "seriale" / "Sezon 01").get() == season);
    OUGHT(fs::modified(away / "seriale").get() == show);

    // A file onto a file from another volume, with replacing: the target gets the new content, the
    // source is gone.
    const fs::path single = nook.made("nowy.nfo", "nowy");

    (void)fs::write(away / "stary.nfo", bytes("stary")).get();

    fs::move(single, away / "stary.nfo", fs::Conflict::overwrite).get();

    OUGHT(!fs::exists(single).get());
    SAME(said_of(fs::read(away / "stary.nfo").get()), std::string{"nowy"});

    std::error_code trouble;

    stood::remove_all(away, trouble);
}

// An entry that cannot be moved (a pipe) refuses the whole before the first step; a copy that
// skipped it would let `remove_all` delete it together with the source.
TRIAL("fs: move between volumes with an entry that cannot be moved touches nothing") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-potok");

    if (away.empty()) return;

    const fs::path from = nook.where() / "z potokiem";

    fs::make_folder(from).get();

    const fs::path film = nook.made("z potokiem/film.mkv", "film");

    if (::mkfifo((from / "potok").c_str(), 0600) != 0) return;

    THREW(fs::move(from, away / "cel").get());

    SAME(said_of(fs::read(film).get()), std::string{"film"});
    OUGHT(!fs::exists(away / "cel").get());

    std::error_code trouble;

    stood::remove_all(away, trouble);
}

namespace {

// A limit on the size of written files (`RLIMIT_FSIZE`) for the duration of a trial, with the
// `SIGXFSZ` signal ignored, so that a write beyond it fails with an error instead of killing the
// program. The free-space trial writes to `/dev/shm`, which lives in memory: if the check before a
// copy failed, a copy of a sparse file would fill the machine's whole memory.
class Capped {
public:
    // Lowers the soft limit to the given size and ignores `SIGXFSZ`; `held()` says whether it
    // worked.
    explicit Capped(rlim_t most) {
        held_ = ::getrlimit(RLIMIT_FSIZE, &was_) == 0;

        if (!held_) return;

        signal_ = std::signal(SIGXFSZ, SIG_IGN);

        rlimit now = was_;
        now.rlim_cur = most;

        held_ = ::setrlimit(RLIMIT_FSIZE, &now) == 0;
    }

    // Restores the previous limit and signal handler.
    ~Capped() {
        if (held_) ::setrlimit(RLIMIT_FSIZE, &was_);

        std::signal(SIGXFSZ, signal_ == SIG_ERR ? SIG_DFL : signal_);
    }

    Capped(const Capped&) = delete;
    Capped& operator=(const Capped&) = delete;
    Capped(Capped&&) = delete;
    Capped& operator=(Capped&&) = delete;

    // Whether the limit is in place.
    [[nodiscard]] bool held() const { return held_; }

private:
    rlimit was_{};
    void (*signal_)(int) = SIG_DFL;
    bool held_ = false;
};

}

// A copy that will not fit refuses at once and leaves nothing behind. The file is sparse: it takes
// almost nothing on disk but says of itself that it is larger than all the free space of the
// target.
TRIAL("fs: a copy larger than the free space refuses before writing") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-miejsce");

    if (away.empty()) return;

    std::error_code trouble;

    const stood::space_info room = stood::space(away, trouble);

    if (trouble) return;

    const fs::path from = nook.made("ogromny.bin", "");

    stretched(from, room.available + (std::uintmax_t{1} << 30), trouble);

    if (trouble) return;

    const Capped capped{64 * 1024 * 1024};

    if (!capped.held()) return;

    std::string said;

    try {
        fs::copy(from, away / "ogromny.bin").get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("free space") != std::string::npos);
    OUGHT(!fs::exists(away / "ogromny.bin").get());

    said.clear();

    try {
        fs::move(from, away / "ogromny.bin").get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("free space") != std::string::npos);
    OUGHT(fs::exists(from).get());

    // A directory counts as the sum of its files.
    const fs::path folder = nook.where() / "katalog";

    fs::make_folder(folder).get();
    fs::rename(from, folder / "ogromny.bin").get();

    said.clear();

    try {
        fs::copy(folder, away / "katalog").get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    OUGHT(said.find("free space") != std::string::npos);
    OUGHT(!fs::exists(away / "katalog").get());

    stood::remove_all(away, trouble);
}

#endif

#if defined(__linux__)

// A rename refused for a reason other than a different volume is an error, not an invitation to
// copy. An immutable file (`FS_IMMUTABLE_FL`) refuses renaming even to root; a `move` that then
// copied it alongside would fail silently to delete the source and leave two copies with the answer
// "done".
TRIAL("fs: a move refused by the system does not copy") {
    const Nook nook;

    const fs::path from = nook.made("zablokowany.mkv", "film");
    const fs::path to = nook.where() / "cel.mkv";

    const int held = ::open(from.c_str(), O_RDONLY);

    if (held < 0) return;

    int flags = 0;

    if (::ioctl(held, FS_IOC_GETFLAGS, &flags) != 0) {
        ::close(held);
        return;
    }

    const int locked = flags | FS_IMMUTABLE_FL;

    if (::ioctl(held, FS_IOC_SETFLAGS, &locked) != 0) {
        ::close(held);
        return;
    }

    std::string said;

    try {
        fs::move(from, to).get();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    // The same with replacing: the target keeps its own content, the source its own.
    const fs::path taken = nook.made("zajęty.mkv", "inny");

    std::string again;

    try {
        fs::move(from, taken, fs::Conflict::overwrite).get();
    } catch (const fs::Trouble& why) {
        again = why.what();
    }

    ::ioctl(held, FS_IOC_SETFLAGS, &flags);
    ::close(held);

    OUGHT(said.find("Cannot rename") != std::string::npos);
    OUGHT(again.find("Cannot rename") != std::string::npos);
    OUGHT(!fs::exists(to).get());
    SAME(said_of(fs::read(from).get()), std::string{"film"});
    SAME(said_of(fs::read(taken).get()), std::string{"inny"});
}

namespace {

// The `FS_IMMUTABLE_FL` flag for the duration of a trial; it removes the flag itself, also when the
// trial fails.
class Immutable {
public:
    // Opens the file and sets the immutable flag on it; `set()` says whether that worked.
    explicit Immutable(const fs::path& where) : held_(::open(where.c_str(), O_RDONLY)) {
        if (held_ < 0 || ::ioctl(held_, FS_IOC_GETFLAGS, &was_) != 0) return;

        const int locked = was_ | FS_IMMUTABLE_FL;

        set_ = ::ioctl(held_, FS_IOC_SETFLAGS, &locked) == 0;
    }

    // Restores the previous flags and closes the file.
    ~Immutable() {
        if (set_) ::ioctl(held_, FS_IOC_SETFLAGS, &was_);
        if (held_ >= 0) ::close(held_);
    }

    Immutable(const Immutable&) = delete;
    Immutable& operator=(const Immutable&) = delete;
    Immutable(Immutable&&) = delete;
    Immutable& operator=(Immutable&&) = delete;

    // Whether the flag was set.
    [[nodiscard]] bool set() const { return set_; }

private:
    int held_ = -1;
    int was_ = 0;
    bool set_ = false;
};

}

// A source that cannot be cleaned up after a copy between volumes: an immutable file stays and says
// so plainly, and an immutable empty directory leaves the source and a sentence that something
// stayed behind.
TRIAL("fs: move between volumes says what stayed in the source") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-zostalo");

    if (away.empty()) return;

    {
        const fs::path from = nook.where() / "z plikiem";

        fs::make_folder(from).get();

        const fs::path stuck = nook.made("z plikiem/film.mkv", "film");

        const Immutable lock{stuck};

        if (!lock.set()) return;

        std::string said;

        try {
            fs::move(from, away / "z plikiem").get();
        } catch (const fs::Trouble& why) {
            said = why.what();
        }

        OUGHT(said.find("Cannot remove the source") != std::string::npos);
        SAME(said_of(fs::read(away / "z plikiem" / "film.mkv").get()), std::string{"film"});
        OUGHT(fs::exists(stuck).get());
    }

    {
        const fs::path from = nook.where() / "z katalogiem";

        fs::make_folder(from / "pusty").get();
        (void)nook.made("z katalogiem/film.mkv", "film");

        const Immutable lock{from / "pusty"};

        if (!lock.set()) return;

        std::string said;

        try {
            fs::move(from, away / "z katalogiem").get();
        } catch (const fs::Trouble& why) {
            said = why.what();
        }

        OUGHT(said.find("stayed behind") != std::string::npos);
        SAME(said_of(fs::read(away / "z katalogiem" / "film.mkv").get()), std::string{"film"});
        OUGHT(!fs::exists(from / "film.mkv").get());
        OUGHT(fs::exists(from / "pusty").get());
    }

    std::error_code trouble;

    stood::remove_all(away, trouble);
}

#endif

// Renaming onto itself does nothing and succeeds. On a case-insensitive system `heat` → `Heat` goes
// the same way: the target "exists", but it is the same entry. A hard link is a separate entry,
// although `equivalent`, and it must not be overwritten.
TRIAL("fs: rename onto itself succeeds, onto a hard link it refuses") {
    const Nook nook;

    const fs::path one = nook.made("jeden.txt", "treść");

    fs::rename(one, one).get();
    fs::move(one, one).get();

    SAME(said_of(fs::read(one).get()), std::string{"treść"});

    const fs::path two = nook.where() / "dwa.txt";

    std::error_code trouble;

    stood::create_hard_link(one, two, trouble);

    if (trouble) return;

    THREW(fs::rename(one, two).get());
    THREW(fs::move(one, two).get());

    OUGHT(fs::exists(one).get());
    OUGHT(fs::exists(two).get());

    // The same name in another directory is also a separate entry.
    const fs::path other = nook.where() / "inny" / "jeden.txt";

    fs::make_folder(other.parent_path()).get();
    stood::create_hard_link(one, other, trouble);

    if (trouble) return;

    THREW(fs::rename(one, other).get());

    OUGHT(fs::exists(one).get());
    OUGHT(fs::exists(other).get());
}

#ifdef _WIN32

// On Windows `heat` and `Heat` are one entry, so the target "exists", and a change of letter case
// alone is an ordinary rename, for a file and for a directory, under every collision policy.
TRIAL("fs: on Windows a change of letter case alone is a rename") {
    const Nook nook;

    const fs::path file = nook.made("heat.txt", "treść");
    const fs::path folder = nook.where() / "alien";

    fs::make_folder(folder).get();

    fs::rename(file, nook.where() / "Heat.txt").get();
    fs::move(folder, nook.where() / "Alien").get();

    std::vector<std::string> named;

    for (const fs::Entry& one : fs::list(nook.where()).get()) named.push_back(fs::to_string(one.where.filename()));

    std::sort(named.begin(), named.end());

    SAME(named, (std::vector<std::string>{"Alien", "Heat.txt"}));
}

#endif

TRIAL("fs: write with refuse leaves an existing file alone") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "stare");

    THREW(fs::write(one, bytes("nowe"), fs::Conflict::fail).get());

    SAME(said_of(fs::read(one).get()), std::string{"stare"});
}

TRIAL("fs: a copy keeps the modification time") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", ladder(5000));
    const fs::path to = nook.where() / "cel.bin";

    const auto dawno = std::chrono::sys_seconds{std::chrono::seconds{1000000000}};

    fs::modified(from, dawno).get();

    SAME(fs::modified(from).get(), dawno);

    fs::copy(from, to).get();

    SAME(fs::modified(to).get(), dawno);
}

TRIAL("fs: a copy of a tree keeps the entries' modification times") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).get();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    const auto dawno = std::chrono::sys_seconds{std::chrono::seconds{1000000000}};
    fs::modified(from / "plik.txt", dawno).get();

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).get();

    SAME(fs::modified(to / "plik.txt").get(), dawno);
}

// Copying a file onto itself would delete it: the target would be opened for writing and truncated
// to zero while it is also the source. So it refuses, while `beside` is still allowed, because
// there the target is different.
TRIAL("fs: copying a file onto itself refuses instead of deleting it") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    THREW(fs::copy(one, one, fs::Conflict::overwrite).get());

    SAME(fs::read(one).get().size(), std::string{"treść"}.size());

    // The same path written differently is still the same file.
    const fs::path around = nook.where() / "." / "plik.txt";

    THREW(fs::copy(one, around, fs::Conflict::overwrite).get());

    SAME(fs::read(one).get().size(), std::string{"treść"}.size());

    // `beside` makes a copy alongside, and that is the only sensible way to copy a file onto
    // itself.
    fs::copy(one, one, fs::Conflict::rename).get();

    OUGHT(stood::exists(nook.where() / "plik (2).txt"));
    SAME(fs::read(one).get().size(), std::string{"treść"}.size());
}

// Without a guard of its own, a directory copied onto itself would fail only at the first file
// inside and talk about that file, while the question was about the directory. Hence a separate
// message.
TRIAL("fs: copying a directory onto itself refuses and talks about the directory") {
    const Nook nook;

    const fs::path one = nook.where() / "katalog";

    fs::make_folder(one).get();
    (void)nook.made("katalog/w środku.txt", "x");

    std::string said;

    try {
        fs::copy(one, one, fs::Conflict::overwrite).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    OUGHT(said.find("directory onto itself") != std::string::npos);
    OUGHT(stood::exists(one / "w środku.txt"));
}

TRIAL("fs: copy follows a link to a file") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path link = nook.where() / "skrót.txt";

    if (!linked(link, one)) return;

    SAME(static_cast<int>(fs::kind(link).get()), static_cast<int>(fs::Kind::link));

    const fs::path to = nook.where() / "cel.txt";

    fs::copy(link, to).get();

    SAME(static_cast<int>(fs::kind(to).get()), static_cast<int>(fs::Kind::file));
    SAME(said_of(fs::read(to).get()), std::string{"treść"});
}

TRIAL("fs: copy follows a link to a directory in a tree") {
    const Nook nook;

    const fs::path osobno = nook.where() / "osobno";
    fs::make_folder(osobno).get();

    {
        std::ofstream making{osobno / "w środku.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).get();

    if (!linked(from / "skrót", osobno, true)) return;

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).get();

    SAME(static_cast<int>(fs::kind(to / "skrót").get()), static_cast<int>(fs::Kind::folder));
    SAME(said_of(fs::read(to / "skrót" / "w środku.txt").get()), std::string{"treść"});
}

TRIAL("fs: a link to an ancestor does not make copying loop") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from / "w głąb").get();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    if (!linked(from / "w głąb" / "wstecz", from, true)) return;

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).get();

    SAME(said_of(fs::read(to / "plik.txt").get()), std::string{"treść"});
}

TRIAL("fs: a broken link in a tree refuses the copy before anything is written") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).get();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    if (!linked(from / "donikąd", nook.where() / "nie ma tego")) return;

    const fs::path to = nook.where() / "odbitka";

    THREW(fs::copy(from, to).get());
    OUGHT(!stood::exists(to));
}

TRIAL("fs: copying a broken link directly rejects") {
    const Nook nook;

    const fs::path link = nook.where() / "donikąd";

    if (!linked(link, nook.where() / "nie ma tego")) return;

    THREW(fs::copy(link, nook.where() / "cel").get());
}

TRIAL("fs: alike recognizes the same file under two paths") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");
    const fs::path two = nook.where() / "." / "plik.txt";
    const fs::path three = nook.made("inny.txt", "treść");

    OUGHT(fs::equivalent(one, two).get());
    OUGHT(!fs::equivalent(one, three).get());
}

TRIAL("fs: alike on a missing file rejects") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    THREW((void)fs::equivalent(one, nook.where() / "nie ma").get());
}

TRIAL("fs: real straightens a path") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");
    const fs::path krzywa = nook.where() / "." / "puste" / ".." / "plik.txt";

    SAME(fs::canonical(krzywa).get(), fs::canonical(one).get());
}

TRIAL("fs: target returns what a link points to") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path skrót = nook.where() / "skrót.txt";

    if (!linked(skrót, one)) return;

    SAME(fs::target(skrót).get(), one);
    OUGHT(fs::equivalent(skrót, one).get());
}

TRIAL("fs: target on an ordinary file rejects") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    THREW((void)fs::target(one).get());
}

TRIAL("fs: link creates a symbolic link") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path skrót = nook.where() / "skrót.txt";

    if (!linked(nook.where() / "próba", one)) return;

    fs::link(skrót, one).get();

    SAME(static_cast<int>(fs::kind(skrót).get()), static_cast<int>(fs::Kind::link));
    SAME(said_of(fs::read(skrót).get()), std::string{"treść"});
}

TRIAL("fs: link creates a hard link") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path drugi = nook.where() / "drugi.txt";

    fs::link(drugi, one, true).get();

    SAME(static_cast<int>(fs::kind(drugi).get()), static_cast<int>(fs::Kind::file));
    OUGHT(fs::equivalent(drugi, one).get());
}

TRIAL("fs: marks come back as they were set") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    OUGHT(!fs::attributes(one).get().readonly);

    fs::Attributes what;
    what.readonly = true;

    fs::attributes(one, what).get();

    OUGHT(fs::attributes(one).get().readonly);

    what.readonly = false;
    fs::attributes(one, what).get();

    OUGHT(!fs::attributes(one).get().readonly);
}

TRIAL("fs: a listing brings the attributes together with the entries") {
    const Nook nook;

    const fs::path one = nook.made("zwykły.txt", "treść");
    (void)nook.made(".skryty.txt", "treść");

    fs::Attributes what;
    what.readonly = true;
    fs::attributes(one, what).get();

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{2});

    bool widziany = false;

    for (const fs::Entry& wpis : among) {
        if (wpis.where.filename() == "zwykły.txt") {
            OUGHT(wpis.attributes.readonly);
            widziany = true;
        }
    }

    OUGHT(widziany);

    what.readonly = false;
    fs::attributes(one, what).get();
}

TRIAL("fs: temp and home point to existing directories") {
    SAME(static_cast<int>(fs::kind(fs::temp()).get()), static_cast<int>(fs::Kind::folder));

    const fs::path dom = fs::home();

    OUGHT(!dom.empty());
    SAME(static_cast<int>(fs::kind(dom).get()), static_cast<int>(fs::Kind::folder));
}

TRIAL("fs: here reads and changes the current directory") {
    const Nook nook;

    const fs::path skąd = fs::current();

    fs::current(nook.where()).get();

    OUGHT(fs::equivalent(fs::current(), nook.where()).get());

    fs::current(skąd).get();

    SAME(fs::current(), skąd);
}

TRIAL("fs: here on a missing directory rejects") {
    const Nook nook;

    THREW(fs::current(nook.where() / "nie ma").get());
}

namespace {

// A read long enough that the stream is nearly always still busy with it when the next call comes:
// the operation starts at once, and sixteen mebibytes take far longer than the step to the next
// line.
constexpr std::size_t long_read = std::size_t{16} << 20U;

// Reads from the stream once an operation let go of earlier has ended and freed it, waiting at most
// five seconds.
fs::Bytes read_when_free(fs::Cursor& flow, std::size_t much) {
    for (int again = 0;; ++again) {
        try {
            return flow.read(much).get();
        } catch (const fs::Trouble& why) {
            if (again >= 500 || std::string_view{why.what()}.find("busy") == std::string_view::npos) throw;
        }

        std::this_thread::sleep_for(10ms);
    }
}

}

TRIAL("Flow: reads in order without keeping count") {
    const Nook nook;

    const std::string whole = ladder(20000);
    const fs::path one = nook.made("duży.bin", whole);

    fs::Cursor flow = fs::open(one).get().cursor();

    std::string made;

    while (true) {
        const fs::Bytes part = flow.read(4096).get();
        if (part.empty()) break;

        made += said_of(part);
    }

    SAME(made, whole);
    SAME(flow.at(), static_cast<std::uintmax_t>(whole.size()));
}

TRIAL("Flow: a short read at the end moves only by as much as arrived") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abcdefghij");

    fs::Cursor flow = fs::open(one).get().cursor();

    SAME(said_of(flow.read(6).get()), std::string{"abcdef"});
    SAME(flow.at(), std::uintmax_t{6});

    SAME(said_of(flow.read(100).get()), std::string{"ghij"});
    SAME(flow.at(), std::uintmax_t{10});

    OUGHT(flow.read(100).get().empty());
    SAME(flow.at(), std::uintmax_t{10});
}

TRIAL("Flow: seek moves the reading position") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one).get().cursor();

    flow.seek(4);

    SAME(said_of(flow.read(3).get()), std::string{"456"});
    SAME(flow.at(), std::uintmax_t{7});
}

TRIAL("Flow: two streams from one file do not interfere") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    const fs::File held = fs::open(one).get();

    fs::Cursor first = held.cursor();
    fs::Cursor second = held.cursor();

    SAME(said_of(first.read(3).get()), std::string{"012"});
    SAME(said_of(second.read(3).get()), std::string{"012"});
    SAME(said_of(first.read(3).get()), std::string{"345"});

    SAME(first.at(), std::uintmax_t{6});
    SAME(second.at(), std::uintmax_t{3});
}

TRIAL("Flow: copies share the position") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor first = fs::open(one).get().cursor();
    fs::Cursor second = first;

    SAME(said_of(first.read(3).get()), std::string{"012"});
    SAME(second.at(), std::uintmax_t{3});
    SAME(said_of(second.read(3).get()), std::string{"345"});
    SAME(first.at(), std::uintmax_t{6});
}

// The first read starts at once and is long, so the second one nearly always finds the stream
// busy. When the first has already ended, the second may go, but then it begins where the first
// stopped, at the end of the file, and reads nothing: two reads never share a position.
TRIAL("Flow: a second request before the first one rejects") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", ladder(long_read));

    fs::Cursor flow = fs::open(one).get().cursor();

    Promise<fs::Bytes> first = flow.read(long_read);
    Promise<fs::Bytes> second = flow.read(1000);

    Outcome<fs::Bytes> later = std::move(second).outcome().get();

    SAME(said_of(std::move(first).get()).size(), long_read);

    if (later.ok()) {
        OUGHT(later.value().empty());
    } else {
        THREW((void)later.value());
    }

    SAME(flow.at(), std::uintmax_t{long_read});
}

TRIAL("Flow: an abandoned promise releases the stream") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one).get().cursor();

    { const Promise<fs::Bytes> nietknięta = flow.read(3); }

    const std::string got = said_of(read_when_free(flow, 3));

    // The abandoned read either never began or had already read its three bytes when it was let go.
    OUGHT((got == "012" && flow.at() == 3) || (got == "345" && flow.at() == 6));
}

TRIAL("Flow: a failed operation also releases the stream") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one, fs::Mode::read).get().cursor();

    THREW((void)flow.write(bytes("x")).get());

    SAME(flow.at(), std::uintmax_t{0});
    SAME(said_of(flow.read(3).get()), std::string{"012"});
}

TRIAL("Flow: seek during a read rejects") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", ladder(long_read));

    fs::Cursor flow = fs::open(one).get().cursor();

    Promise<fs::Bytes> going = flow.read(long_read);

    bool refused = false;

    try {
        flow.seek(0);
    } catch (const fs::Trouble&) {
        refused = true;
    }

    (void)std::move(going).get();

    // A refused seek leaves the position after the read. A seek let through came after the read
    // had ended, so it moved the position back; one let through during the read would have been
    // overwritten by the read's end.
    SAME(flow.at(), refused ? std::uintmax_t{long_read} : std::uintmax_t{0});

    flow.seek(0);
    SAME(flow.at(), std::uintmax_t{0});
}

TRIAL("Flow: writing advances by itself") {
    const Nook nook;

    const fs::path one = nook.where() / "zapis.bin";

    fs::Cursor flow = fs::open(one, fs::Mode::truncate).get().cursor();

    SAME(flow.write(bytes("pierwsza ")).get(), std::size_t{9});
    SAME(flow.write(bytes("druga")).get(), std::size_t{5});

    SAME(flow.at(), std::uintmax_t{14});
    SAME(said_of(fs::read(one).get()), std::string{"pierwsza druga"});
}

TRIAL("Flow: appending at the end after seeking") {
    const Nook nook;

    const fs::path one = nook.made("dane.txt", "stare");

    fs::Cursor flow = fs::open(one, fs::Mode::write).get().cursor();

    flow.seek(fs::size(one).get());

    (void)flow.write(bytes(" i nowe")).get();

    SAME(said_of(fs::read(one).get()), std::string{"stare i nowe"});
}

TRIAL("Flow: a closed stream rejects instead of crashing") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one).get().cursor();
    flow.close();

    OUGHT(!flow.held());
    THREW((void)flow.read(3).get());
    THREW((void)flow.write(bytes("x")).get());
    THREW(flow.seek(0));
    SAME(flow.at(), std::uintmax_t{0});
}

TRIAL("Flow: a closed file gives no stream") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::File held = fs::open(one).get();
    held.close();

    THREW((void)held.cursor());
}

TRIAL("Flow: a stream outlives the handle it came from") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow;

    {
        fs::File held = fs::open(one).get();
        flow = held.cursor();
    }

    SAME(said_of(flow.read(4).get()), std::string{"0123"});
}

namespace {

// Events from the file system arrive at their own pace, so we wait for them with a promise and a
// deadline, not with sleeping. Returns nothing when nothing arrived in time; the request given up
// at the deadline leaves the watch's queue, so the next batch is not lost to it. The patience is
// generous for a reason that follows directly from the polling rule: the pause is ten times the
// time a look takes, so on a loaded machine it can grow to seconds. Cases that expect silence give
// a short time explicitly.
std::vector<fs::Change> awaited(fs::Watch& watch, std::chrono::milliseconds patience = 6000ms) {
    try {
        return watch.next().timeout(patience).get();
    } catch (const std::exception&) {
        return {};
    }
}

// Whether the batch has a change of the given kind at the given path.
bool among(const std::vector<fs::Change>& given, const fs::path& where, fs::Change::Kind stir) {
    for (const fs::Change& one : given)
        if (one.where == where && one.kind == stir) return true;

    return false;
}

// The changes as text, one per line, for a failure to tell what the watch did report.
std::string told(const std::vector<fs::Change>& given) {
    static constexpr std::string_view kinds[] = {"created", "removed", "modified", "renamed", "overflow"};

    std::string made;

    for (const fs::Change& one : given) {
        made += "\n          ";
        made += kinds[static_cast<std::size_t>(one.kind)];
        made += " " + fs::to_string(one.where);

        if (!one.was.empty()) made += " from " + fs::to_string(one.was);
    }

    return made.empty() ? std::string{" nothing"} : made;
}

// Whether the batches the watch reports satisfy `done` in time (ten seconds, or the patience given:
// a polling watch pauses ten times as long as a look takes, so on a loaded machine it can take many
// seconds). Batches before the awaited one, of earlier work, are passed over. Each request waits for
// the rest of the time and none is given up before the end: a request given up may still take the
// next batch with it.
template <typename Done>
bool reported(fs::Watch& watch, Done done, std::chrono::milliseconds patience = 10000ms) {
    const auto latest = std::chrono::steady_clock::now() + patience;
    std::vector<fs::Change> heard;

    while (!done(heard)) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(latest - std::chrono::steady_clock::now());

        if (left <= 0ms) return false;

        const std::vector<fs::Change> more = awaited(watch, left);

        if (more.empty()) return false;

        heard.insert(heard.end(), more.begin(), more.end());
    }

    return true;
}

// Whether the watch reports the change of the given kind at the given path in time, as above.
bool reported(fs::Watch& watch, const fs::path& where, fs::Change::Kind stir,
              std::chrono::milliseconds patience = 10000ms) {
    return reported(watch, [&](const std::vector<fs::Change>& heard) { return among(heard, where, stir); }, patience);
}

}

// A polling look that cannot read a folder (a NAS silent for a moment, rights taken away) does not
// take its content for gone: what the previous look saw under it stays as it was, so nothing is
// reported as removed, and nothing as created when the folder can be read again. A root that cannot
// be told at all is blind, which is not the same as absent.
TRIAL("Watch: a polling look does not report as removed what it could not read") {
    using fs::core::Look;
    using fs::core::Sight;

    std::map<fs::path, Sight> was;

    was["/r"] = Sight{fs::Kind::folder, 0, {}, {}};
    was["/r/a"] = Sight{fs::Kind::folder, 0, {}, {}};
    was["/r/a/x.mkv"] = Sight{fs::Kind::file, 5, {}, {}};
    was["/r/a/b/y.mkv"] = Sight{fs::Kind::file, 6, {}, {}};
    was["/r/z.mkv"] = Sight{fs::Kind::file, 7, {}, {}};

    Look now;
    now.seen["/r"] = Sight{fs::Kind::folder, 0, {}, {}};
    now.seen["/r/a"] = Sight{fs::Kind::folder, 0, {}, {}};
    now.unread = {"/r/a"};

    const std::map<fs::path, Sight> kept = fs::core::filled(now, was);
    const std::vector<fs::Change> changes = fs::core::apart(was, kept);

    SAME(changes.size(), std::size_t{1});
    OUGHT(!changes.empty() && changes.front().where == fs::path{"/r/z.mkv"} && changes.front().kind == Changed::removed);
    OUGHT(kept.contains("/r/a/b/y.mkv"));

    const Nook nook;

    fs::make_folder(nook.where() / "zamknięty").get();
    (void)nook.made("zamknięty/plik.txt", "treść");

    OUGHT(!fs::core::looked(nook.where(), true).blind);
    SAME(fs::core::looked(nook.where() / "nie ma", true).seen.size(), std::size_t{0});

#ifndef _WIN32
    if (::geteuid() != 0 && ::chmod((nook.where() / "zamknięty").c_str(), 0) == 0) {
        const Look shut = fs::core::looked(nook.where(), true);

        (void)::chmod((nook.where() / "zamknięty").c_str(), 0700);

        OUGHT(shut.unread == std::vector<fs::path>{nook.where() / "zamknięty"});
        OUGHT(!shut.seen.contains(nook.where() / "zamknięty" / "plik.txt"));
    }
#endif
}

TRIAL("Watch: a new file is reported as created") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).get();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    OUGHT(reported(watch, one, Changed::created));
}

TRIAL("Watch: a deleted file is reported as removed") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::Watch watch = fs::watch(nook.where()).get();

    fs::erase(one).get();

    OUGHT(reported(watch, one, Changed::removed));
}

// Writing an existing file goes alongside and is renamed onto the target, so the watch sees a file
// arrive under that name: as a rename or a creation, depending on whether the system joins the two
// halves of the rename. A file written directly reports a modification.
TRIAL("Watch: a change of content is reported as a modification") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(nook.where()).get();

    fs::write(one, bytes("nowe")).get();

    OUGHT(reported(watch, [&one](const std::vector<fs::Change>& heard) {
        return among(heard, one, Changed::modified) || among(heard, one, Changed::created) ||
               among(heard, one, Changed::renamed);
    }));

    fs::Watch again = fs::watch(nook.where()).get();

    fs::File file = fs::open(one, fs::Mode::write).get();

    (void)file.write(0, bytes("inne")).get();

    // NTFS defers the modification time until the handle is closed, so Windows reports a
    // same-length write through an open file only at closing; inotify reports it at once.
    file.close();

    OUGHT(reported(again, one, Changed::modified));
}

// Two writes of the same length within one second, with a look of the watch between them: when
// polling, the second and the size do not change, so the precise time decides. The trial waits for
// the start of a new second, so that both writes and the look certainly fall within it.
TRIAL("Watch: a write of the same length in the same second is reported") {
    const Nook nook;

    for (;;) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000 < 50) break;

        std::this_thread::sleep_for(5ms);
    }

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(nook.where()).get();

    {
        std::fstream file{one, std::ios::in | std::ios::out | std::ios::binary};

        file << "inne";
    }

    OUGHT(reported(watch, one, Changed::modified));
}

// A file written in a hundred small pieces, as a copy into the watched folder writes it, wakes the
// watcher a few times, not a hundred: changes of content alone wait a moment for the next ones. A
// creation that follows still comes, and with it whatever waited.
TRIAL("Watch: a file written in many pieces comes in few batches") {
    const Nook nook;

    const fs::path one = nook.made("rosnący.bin", "");

    fs::Watch watch = fs::watch(nook.where()).get();

    const fs::path other = nook.where() / "drugi.bin";

    std::thread writer{[&one, &other] {
        fs::File file = fs::open(one, fs::Mode::write).get();

        for (std::uint64_t at = 0; at < 100; ++at) {
            (void)file.write(at, bytes("x")).get();

            std::this_thread::sleep_for(3ms);
        }

        file.close();

        std::ofstream making{other, std::ios::binary};
        making << "y";
    }};

    // The batches are read until the one with the creation, every request waiting for the rest of
    // the time: a request given up at a moment of silence could take the next batch with it.
    const auto latest = std::chrono::steady_clock::now() + 20000ms;
    std::vector<fs::Change> heard;
    int batches = 0;

    while (!among(heard, other, Changed::created)) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(latest - std::chrono::steady_clock::now());

        if (left <= 0ms) break;

        const std::vector<fs::Change> more = awaited(watch, left);

        if (more.empty()) break;

        ++batches;
        heard.insert(heard.end(), more.begin(), more.end());
    }

    writer.join();

    if (!among(heard, other, Changed::created)) tal::testing::fell(__FILE__, __LINE__, "the new file was not heard; heard:" + told(heard));

    // A hundred writes and the creation after them: at most ten batches of writes and the one with
    // the creation.
    OUGHT(batches <= 11);
}

TRIAL("Watch: a rename carries the previous name") {
    const Nook nook;

    const fs::path from = nook.made("stary.txt", "treść");
    const fs::path to = nook.where() / "nowy.txt";

    fs::Watch watch = fs::watch(nook.where()).get();

    fs::rename(from, to).get();

    // Polling has no way to recognize a rename: it only sees that one name is gone and another has
    // appeared. That is an honest limit of this path, and the case allows it.
    OUGHT(reported(watch, [&from, &to](const std::vector<fs::Change>& heard) {
        const bool jedno = std::any_of(heard.begin(), heard.end(), [&](const fs::Change& one) {
            return one.kind == Changed::renamed && one.where == to && one.was == from;
        });

        return jedno || (among(heard, from, Changed::removed) && among(heard, to, Changed::created));
    }));
}

TRIAL("Watch: events from before the request are not lost") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).get();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    later(300ms).get();

    OUGHT(reported(watch, one, Changed::created));
}

TRIAL("Watch: a shallow watch does not see into the depth, a deep one does") {
    const Nook nook;

    const fs::path głąb = nook.where() / "w głąb";
    fs::make_folder(głąb).get();

    fs::Watch płytka = fs::watch(nook.where()).get();
    fs::Watch głęboka = fs::watch(nook.where(), true).get();

    const fs::path one = głąb / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    OUGHT(reported(głęboka, one, Changed::created));
    OUGHT(!among(awaited(płytka, 300ms), one, Changed::created));
}

TRIAL("Watch: a deep watch covers directories created later") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where(), true).get();

    const fs::path głąb = nook.where() / "później";
    fs::make_folder(głąb).get();

    OUGHT(reported(watch, głąb, Changed::created));

    const fs::path one = głąb / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    OUGHT(reported(watch, one, Changed::created));
}

TRIAL("Watch: two watches of the same directory both receive") {
    const Nook nook;

    fs::Watch first = fs::watch(nook.where()).get();
    fs::Watch second = fs::watch(nook.where()).get();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    OUGHT(reported(first, one, Changed::created));
    OUGHT(reported(second, one, Changed::created));
}

TRIAL("Watch: a closed watch rejects") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).get();
    watch.close();

    OUGHT(!watch.held());
    THREW((void)watch.next().get());
}

TRIAL("Watch: watching nothing rejects") {
    const Nook nook;

    THREW((void)fs::watch(nook.where() / "nie ma").get());
}

TRIAL("Watch: a cancelled request does not take the batch from the next one") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).get();

    Promise<std::vector<fs::Change>> porzucone = watch.next();
    porzucone.cancel();

    later(100ms).get();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    OUGHT(reported(watch, one, Changed::created));
}

TRIAL("Watch: a hundred watches are still one thread") {
    const Nook nook;

    std::vector<fs::Watch> among;

    for (int at = 0; at < 100; ++at) {
        const fs::path one = nook.where() / ("katalog" + std::to_string(at));
        fs::make_folder(one).get();

        among.push_back(fs::watch(one).get());
    }

    const fs::path one = nook.where() / "katalog42" / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    OUGHT(reported(among[42], one, Changed::created));
}

TRIAL("Flow: a tight loop of reads never hears busy") {
    const Nook nook;

    const std::string whole = ladder(20000);
    const fs::path one = nook.made("duży.bin", whole);

    fs::Cursor flow = fs::open(one).get().cursor();

    std::string made;

    for (int at = 0; at < 200; ++at) {
        const fs::Bytes part = flow.read(100).get();
        if (part.empty()) break;

        made += said_of(part);
    }

    SAME(made.size(), std::size_t{20000});
    SAME(made, whole);
}

TRIAL("Flow: a tight loop of writes neither") {
    const Nook nook;

    const fs::path one = nook.where() / "zapis.bin";

    fs::Cursor flow = fs::open(one, fs::Mode::truncate).get().cursor();

    for (int at = 0; at < 200; ++at) (void)flow.write(bytes("0123456789")).get();

    SAME(fs::size(one).get(), std::uintmax_t{2000});
    SAME(flow.at(), std::uintmax_t{2000});
}

TRIAL("Watch: watching a single file, not a directory") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(one).get();

    fs::write(one, bytes("zupełnie inna treść")).get();

    OUGHT(reported(watch, [&one](const std::vector<fs::Change>& heard) {
        return among(heard, one, Changed::modified) || among(heard, one, Changed::created);
    }));
}

TRIAL("Watch: the disappearance of a watched file is reported") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::Watch watch = fs::watch(one).get();

    fs::erase(one).get();

    OUGHT(reported(watch, one, Changed::removed));
}

TRIAL("Watch: a watch survives many batches in a row") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).get();

    for (int at = 0; at < 5; ++at) {
        const fs::path one = nook.where() / ("plik" + std::to_string(at) + ".txt");

        {
            std::ofstream making{one, std::ios::binary};
            making << at;
        }

        const bool widziany = reported(watch, one, Changed::created, 24000ms);

        OUGHT(widziany);
    }
}

// This is how most editors save: the new content alongside, then a rename over the old one. A watch
// tied to the inode would stay with the deleted content and fall silent forever.
TRIAL("Watch: a file watch survives saving by replacement") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(one).get();

    const fs::path obok = nook.where() / "plik.txt.nowy";

    {
        std::ofstream making{obok, std::ios::binary};
        making << "zupełnie inna treść";
    }

    fs::rename(obok, one, fs::Conflict::overwrite).get();

    const bool widziany = reported(
        watch,
        [&one](const std::vector<fs::Change>& heard) {
            return among(heard, one, Changed::modified) || among(heard, one, Changed::created);
        },
        24000ms);

    OUGHT(widziany);
    SAME(said_of(fs::read(one).get()), std::string{"zupełnie inna treść"});

    // The real trial is here: after the replacement the watch must see the next change. Tied to the
    // old inode, it would see only its death and fall silent.
    fs::write(one, bytes("jeszcze inna"), fs::Conflict::overwrite).get();

    const bool dalej = reported(
        watch,
        [&one](const std::vector<fs::Change>& heard) {
            return among(heard, one, Changed::modified) || among(heard, one, Changed::created);
        },
        24000ms);

    OUGHT(dalej);
}

TRIAL("Watch: a file watch is silent about its neighbors") {
    const Nook nook;

    const fs::path one = nook.made("mój.txt", "treść");
    const fs::path sąsiad = nook.made("cudzy.txt", "treść");

    fs::Watch watch = fs::watch(one).get();

    fs::write(sąsiad, bytes("zupełnie inna treść"), fs::Conflict::overwrite).get();
    fs::write(one, bytes("moja nowa treść"), fs::Conflict::overwrite).get();

    // The batches are read until the watched file's own change, and nothing about the neighbor,
    // written first, may come before it or with it. No request is given up on the way: one given
    // up at a moment of silence could take the next batch with it.
    std::vector<fs::Change> given;

    const bool widziany = reported(
        watch,
        [&](const std::vector<fs::Change>& heard) {
            given = heard;

            return among(heard, one, Changed::modified) || among(heard, one, Changed::created);
        },
        24000ms);

    for (const fs::Change& jeden : given) OUGHT(jeden.where == one);

    OUGHT(widziany);
}

namespace {

// The modification time in nanoseconds since 1970, straight from the system: the fractional-second
// trials must not ask the same path they check. On Windows the grain is a hundred nanoseconds.
std::int64_t instant(const fs::path& where) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA told{};

    if (GetFileAttributesExW(where.c_str(), GetFileExInfoStandard, &told) == 0) return 0;

    const std::uint64_t much =
        (static_cast<std::uint64_t>(told.ftLastWriteTime.dwHighDateTime) << 32) | told.ftLastWriteTime.dwLowDateTime;

    return (static_cast<std::int64_t>(much) - 116444736000000000LL) * 100;
#else
    struct stat told {};

    if (::stat(where.c_str(), &told) != 0) return 0;

    return static_cast<std::int64_t>(told.st_mtim.tv_sec) * 1000000000LL + told.st_mtim.tv_nsec;
#endif
}

// Sets the modification time with a fraction: whole seconds plus 123 456 700 nanoseconds (a
// multiple of the Windows grain). A directory too, through a handle with
// FILE_FLAG_BACKUP_SEMANTICS.
void aged(const fs::path& where, std::int64_t seconds) {
    const std::int64_t whole = seconds * 1000000000LL + 123456700LL;

#ifdef _WIN32
    const HANDLE one = CreateFileW(where.c_str(), FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    const auto much = static_cast<std::uint64_t>(whole / 100 + 116444736000000000LL);

    FILETIME told{};

    told.dwLowDateTime = static_cast<DWORD>(much & 0xFFFFFFFFull);
    told.dwHighDateTime = static_cast<DWORD>(much >> 32);

    SetFileTime(one, nullptr, nullptr, &told);
    CloseHandle(one);
#else
    const timespec told[2] = {{0, UTIME_OMIT}, {static_cast<time_t>(whole / 1000000000LL), static_cast<long>(whole % 1000000000LL)}};

    ::utimensat(AT_FDCWD, where.c_str(), told, 0);
#endif
}

}

// A copy carries the modification time to a fraction of a second: a file, a directory and a
// directory inside. A media server comparing times with fractions would otherwise see every copy as
// a changed file.
TRIAL("fs: a copy carries the modification time with a fraction of a second") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";

    fs::make_folder(from / "Sezon 01").get();

    const fs::path one = nook.made("drzewo/Sezon 01/S01E01.mkv", ladder(5000));
    const fs::path two = nook.made("pojedynczy.mkv", ladder(3000));

    aged(one, 1500000000);
    aged(two, 1450000000);
    aged(from / "Sezon 01", 1400000000);
    aged(from, 1300000000);

    fs::copy(from, nook.where() / "odbitka").get();
    fs::copy(two, nook.where() / "druga.mkv").get();

    SAME(instant(nook.where() / "odbitka" / "Sezon 01" / "S01E01.mkv"), instant(one));
    SAME(instant(nook.where() / "odbitka" / "Sezon 01"), instant(from / "Sezon 01"));
    SAME(instant(nook.where() / "odbitka"), instant(from));
    SAME(instant(nook.where() / "druga.mkv"), instant(two));

    SAME(instant(two) % 1000000000LL, std::int64_t{123456700});

    // The public reading still returns whole seconds, rounded down.
    SAME(fs::modified(two).get(), std::chrono::sys_seconds{std::chrono::seconds{1450000000}});
}

// A file larger than one kernel copy chunk (16 MiB) and with a tail that does not fit in a chunk: a
// copy on the same volume and, where possible, on another, where the kernel refuses and we copy
// ourselves.
TRIAL("fs: a large file is copied faithfully in chunks") {
    const Nook nook;

    const std::string whole = ladder(17 * 1024 * 1024 + 3);

    const fs::path from = nook.made("duży.mkv", whole);

    fs::copy(from, nook.where() / "obok.mkv").get();

    SAME(said_of(fs::read(nook.where() / "obok.mkv").get()) == whole, true);

#ifndef _WIN32
    const fs::path away = elsewhere(nook, "talcite-duzy");

    if (away.empty()) return;

    fs::copy(from, away / "daleko.mkv").get();

    SAME(said_of(fs::read(away / "daleko.mkv").get()) == whole, true);

    // A move between volumes goes through a copy: the time with its fraction travels with the
    // content.
    aged(from, 1200000000);

    const std::int64_t was = instant(from);

    fs::move(from, away / "przeniesiony.mkv").get();

    SAME(instant(away / "przeniesiony.mkv"), was);

    std::error_code trouble;

    stood::remove_all(away, trouble);
#endif
}

namespace {

// Batches until those heard so far satisfy `done`, at most ten seconds (earlier work may still come
// in batches of its own on a loaded machine), then until silence, so that a change reported twice
// is among them too. Each request waits for the rest of the time, as in `reported`.
template <typename Done>
std::vector<fs::Change> gathered(fs::Watch& watch, Done done) {
    const auto latest = std::chrono::steady_clock::now() + 10000ms;
    std::vector<fs::Change> made;

    while (!done(made)) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(latest - std::chrono::steady_clock::now());

        if (left <= 0ms) break;

        const std::vector<fs::Change> more = awaited(watch, left);

        if (more.empty()) break;

        made.insert(made.end(), more.begin(), more.end());
    }

    for (std::vector<fs::Change> more = awaited(watch, 700ms); !more.empty(); more = awaited(watch, 700ms))
        made.insert(made.end(), more.begin(), more.end());

    return made;
}

// All batches until silence: the first with generous patience, then briefly.
std::vector<fs::Change> gathered(fs::Watch& watch) {
    std::vector<fs::Change> made = awaited(watch);

    for (std::vector<fs::Change> more = awaited(watch, 700ms); !more.empty(); more = awaited(watch, 700ms))
        made.insert(made.end(), more.begin(), more.end());

    return made;
}

// Whether the path lies under the root (or is the root itself).
bool under(const fs::path& where, const fs::path& root) {
    const auto [left, right] = std::mismatch(root.begin(), root.end(), where.begin(), where.end());

    return left == root.end();
}

}

// A renamed subdirectory of a deep watch: a file in it is reported under the new path, and only
// once. inotify must not add the same directory a second time, or every event would arrive twice,
// once under a name that no longer exists.
TRIAL("Watch: a renamed subdirectory is heard under its new name, once") {
    const Nook nook;

    const fs::path stary = nook.where() / "stary";

    fs::make_folder(stary / "w środku").get();

    fs::Watch watch = fs::watch(nook.where(), true).get();

    const fs::path nowy = nook.where() / "nowy";

    fs::rename(stary, nowy).get();

    // The rename itself is waited for, not silence after it: a request given up at the end of a
    // silence could take the next batch with it.
    OUGHT(reported(watch, [&nowy](const std::vector<fs::Change>& heard) {
        return among(heard, nowy, Changed::renamed) || among(heard, nowy, Changed::created);
    }));

    {
        std::ofstream making{nowy / "w środku" / "świeży.txt", std::ios::binary};
        making << "treść";
    }

    const std::vector<fs::Change> given = gathered(watch, [&nowy](const std::vector<fs::Change>& heard) {
        return among(heard, nowy / "w środku" / "świeży.txt", Changed::created);
    });

    std::size_t seen = 0;

    for (const fs::Change& one : given) {
        OUGHT(!under(one.where, stary));

        if (one.where == nowy / "w środku" / "świeży.txt" && one.kind == Changed::created) ++seen;
    }

    if (seen != 1) tal::testing::fell(__FILE__, __LINE__, "heard instead of one creation:" + told(given));
}

// A deleted subdirectory is one removal, not three entries: from the parent, from the directory
// itself and a "modified" from the descriptor's farewell.
TRIAL("Watch: a deleted subdirectory is reported once") {
    const Nook nook;

    const fs::path głąb = nook.where() / "głąb";

    fs::make_folder(głąb).get();

    fs::Watch watch = fs::watch(nook.where(), true).get();

    fs::erase(głąb).get();

    const std::vector<fs::Change> given =
        gathered(watch, [&głąb](const std::vector<fs::Change>& heard) { return among(heard, głąb, Changed::removed); });

    std::size_t about = 0;

    for (const fs::Change& one : given)
        if (one.where == głąb) {
            ++about;
            OUGHT(one.kind == Changed::removed);
        }

    SAME(about, std::size_t{1});
}

// A directory moved out of the watched tree no longer reports its changes under the old path.
TRIAL("Watch: a subdirectory moved out of the tree falls silent") {
    const Nook nook;

    fs::make_folder(nook.where() / "drzewo" / "gałąź").get();
    fs::make_folder(nook.where() / "obok").get();

    fs::Watch watch = fs::watch(nook.where() / "drzewo", true).get();

    fs::rename(nook.where() / "drzewo" / "gałąź", nook.where() / "obok" / "gałąź").get();

    (void)gathered(watch);

    {
        std::ofstream making{nook.where() / "obok" / "gałąź" / "świeży.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path stale = nook.where() / "drzewo" / "gałąź";

    for (const fs::Change& one : awaited(watch, 1500ms)) OUGHT(!under(one.where, stale));
}

#ifndef _WIN32

// The disappearance of the root itself is a change whatever the path: notifications report it
// themselves, and polling keeps the root in its view, so an empty directory does not disappear
// without a word.
TRIAL("Watch: a deleted empty root is reported as removed") {
    const Nook nook;

    const fs::path root = nook.where() / "pusty";

    fs::make_folder(root).get();

    fs::Watch watch = fs::watch(root).get();

    fs::erase(root).get();

    const bool heard = reported(watch, root, Changed::removed, 24000ms);

    OUGHT(heard);
}

#endif

#if defined(__linux__) && !defined(TALCITE_GENERIC_POSIX)

namespace {

// Whether any inotify instance of this process holds a descriptor on this directory: `fdinfo` lists
// each one as "inotify wd:… ino:…" with the inode number in hexadecimal.
bool inotified(const fs::path& where) {
    struct stat told {};

    if (::stat(where.c_str(), &told) != 0) return false;

    char wanted[32];

    std::snprintf(wanted, sizeof wanted, " ino:%lx ", static_cast<unsigned long>(told.st_ino));

    std::error_code trouble;

    for (const stood::directory_entry& one : stood::directory_iterator{"/proc/self/fdinfo", trouble}) {
        std::ifstream reading{one.path()};

        for (std::string line; std::getline(reading, line);)
            if (line.rfind("inotify wd:", 0) == 0 && line.find(wanted) != std::string::npos) return true;
    }

    return false;
}

}

namespace {

// The kernel returns the descriptors of a closed inotify instance with a delay. We wait until they
// are back; otherwise the next step would hit a limit still exhausted by the previous one and would
// go by polling.
void recovered(const fs::path& where) {
    const int probe = inotify_init1(IN_CLOEXEC);

    if (probe < 0) return;

    (void)tal::testing::came([&] { return inotify_add_watch(probe, where.c_str(), IN_MODIFY) >= 0; });

    ::close(probe);
}

}

// The max_user_watches limit exhausted: the watch neither refuses nor goes silently deaf, but
// polls. We exhaust the limit ourselves, with descriptors on as many files as the system allows;
// with a limit above 200 thousand the trial would be too expensive and is skipped. Three
// situations: no room for the root; room for the root but none for a subdirectory; and no room for
// a subdirectory created later.
TRIAL("Watch: without inotify descriptors a watch polls") {
    std::ifstream reading{"/proc/sys/fs/inotify/max_user_watches"};

    long most = 0;

    if (!(reading >> most) || most > 200000) return;

    const Nook spent;

    // A new inotify instance that takes everything that is left, one descriptor per file. `last` is
    // the last one taken, for giving back one place.
    int last = -1;

    const auto hogged = [&] {
        const int made = inotify_init1(IN_CLOEXEC);

        for (long at = 0; made >= 0 && at <= 2 * most; ++at) {
            const fs::path one = spent.where() / std::to_string(at);

            const int file = ::open(one.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);

            if (file >= 0) ::close(file);

            const int took = inotify_add_watch(made, one.c_str(), IN_MODIFY);

            if (took < 0) break;

            last = took;
        }

        return made;
    };

    const auto heard = [](fs::Watch& watch, const fs::path& one) {
        {
            std::ofstream making{one, std::ios::binary};
            making << "treść";
        }

        const bool made = reported(watch, one, Changed::created, 24000ms);

        return made;
    };

    const int hog = hogged();

    if (hog < 0) return;

    const Nook first;

    fs::Watch none = fs::watch(first.where(), true).get();

    OUGHT(heard(none, first.where() / "świeży.txt"));

    // One place free: the root fits, a subdirectory no longer does.
    inotify_rm_watch(hog, last);

    const Nook second;

    fs::make_folder(second.where() / "głąb").get();

    fs::Watch half = fs::watch(second.where(), true).get();

    OUGHT(heard(half, second.where() / "głąb" / "świeży.txt"));

    // The watch is not full of holes: either the whole tree stands in inotify or none of it, so a
    // root that fitted does not stay when the whole goes by polling. Which of the two happens
    // depends on whether someone else of the same user has just released a descriptor.
    OUGHT(inotified(second.where() / "głąb") == inotified(second.where()));

    ::close(hog);

    recovered(second.where());

    // The watch stands on events; a subdirectory appears when there are no descriptors left.
    const Nook third;

    fs::Watch later = fs::watch(third.where(), true).get();

    const int again = hogged();

    fs::make_folder(third.where() / "później").get();

    // Switching to polling reports an overflow only after the first look; a file written earlier
    // could already be part of that look and not be reported as new, which is what the overflow is
    // for.
    const bool switched = reported(later, third.where(), Changed::overflow, 24000ms);

    OUGHT(switched);

    // After switching, inotify holds nothing from this tree any more; otherwise a change in the
    // root would arrive twice, once from events and once from polling.
    OUGHT(!inotified(third.where()));
    OUGHT(heard(later, third.where() / "później" / "świeży.txt"));

    ::close(again);

    recovered(third.where());
}

// A closed watch gives back its inotify descriptors, also those of subdirectories. The watch may
// have switched to polling along the way, so it is removed from both paths at once.
TRIAL("Watch: a closed watch gives back its inotify descriptors") {
    const Nook nook;

    fs::make_folder(nook.where() / "głąb").get();

    fs::Watch watch = fs::watch(nook.where(), true).get();

    OUGHT(inotified(nook.where()));
    OUGHT(inotified(nook.where() / "głąb"));

    watch.close();

    // Closing releases the handle, and the descriptors are removed by whoever releases the watch
    // last, which may be a pool job just finishing a look. Under load that is a moment after
    // `close`, so we wait until it happens, at most two seconds: a descriptor that really leaks
    // stays forever.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};

    while ((inotified(nook.where()) || inotified(nook.where() / "głąb")) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds{20});

    OUGHT(!inotified(nook.where()));
    OUGHT(!inotified(nook.where() / "głąb"));
}

// A root deleted and created again: the watch does not go deaf forever, but switches to polling and
// sees what was created in the new one.
TRIAL("Watch: a root created again is still heard") {
    const Nook nook;

    const fs::path root = nook.where() / "korzeń";

    fs::make_folder(root).get();

    fs::Watch watch = fs::watch(root).get();

    fs::erase(root).get();

    const std::vector<fs::Change> given = gathered(watch, [&root](const std::vector<fs::Change>& heard) {
        return among(heard, root, Changed::removed) && among(heard, root, Changed::overflow);
    });

    // The disappearance of the root, and the admission that from now on the watch goes another way.
    OUGHT(among(given, root, Changed::removed));
    OUGHT(among(given, root, Changed::overflow));

    fs::make_folder(root).get();

    const fs::path one = root / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    const bool heard = reported(watch, one, Changed::created, 24000ms);

    OUGHT(heard);
}

#endif

TRIAL("fs: mount recognizes the volume") {
    const Nook nook;

    const int told = static_cast<int>(fs::mount(nook.where()).get());

    OUGHT(told == static_cast<int>(fs::Mount::disk) ||
          told == static_cast<int>(fs::Mount::remote) ||
          told == static_cast<int>(fs::Mount::other));
}

TRIAL("fs: the chunk comes from the recognized volume") {
    const Nook nook;

    const fs::Mount told = fs::mount(nook.where()).get();
    const std::size_t rata = fs::chunk(nook.where()).get();

    if (told == fs::Mount::removable || told == fs::Mount::optical)
        SAME(rata, std::size_t{256 * 1024});
    else SAME(rata, std::size_t{1024 * 1024});
}

TRIAL("fs: the list of drives is not empty and carries the space") {
    const std::vector<fs::Volume> among = volumes();

    OUGHT(!among.empty());

    bool któryś = false;

    for (const fs::Volume& one : among) {
        OUGHT(!one.where.empty());

        if (one.space.total > 0) {
            OUGHT(one.space.free <= one.space.total);
            któryś = true;
        }
    }

    OUGHT(któryś);
}

TRIAL("fs: every drive from the list can be looked at") {
    const std::vector<fs::Volume> among = volumes();

    for (const fs::Volume& one : among) {
        const fs::Kind told = fs::kind(one.where).get();

        OUGHT(told == fs::Kind::folder || told == fs::Kind::none);
    }
}

// The list of volumes is the roots and the volume of each of them: the same places, in the same
// order.
TRIAL("fs: the roots are the places from the list of volumes") {
    const std::vector<fs::path> roots = fs::roots().get();
    const std::vector<fs::Volume> among = volumes();

    OUGHT(!roots.empty());
    SAME(roots.size(), among.size());

    for (std::size_t at = 0; at < roots.size() && at < among.size(); ++at) SAME(roots[at], among[at].where);
}

// A file's volume is one of the roots, and the one the file lies under: the deepest of those that
// match.
TRIAL("fs: mounted gives the root of the volume the file lies on") {
    const Nook nook;

    fs::write(nook.where() / "a.txt", bytes("abc")).get();

    const fs::path root = fs::mounted(nook.where() / "a.txt").get();
    const std::vector<fs::path> roots = fs::roots().get();

    OUGHT(std::find(roots.begin(), roots.end(), root) != roots.end());

    const fs::path whole = fs::canonical(nook.where() / "a.txt").get();

    // The root is the beginning of the path, compared piece by piece, because letter case and
    // slashes may be written differently.
    auto mine = whole.begin();

    for (const fs::path& piece : root) {
        if (piece.empty()) continue;

        OUGHT(mine != whole.end());

        if (mine == whole.end()) break;

        OUGHT(*mine == piece);

        ++mine;
    }

    // No longer root is the beginning of the path.
    for (const fs::path& other : roots) {
        if (std::distance(other.begin(), other.end()) <= std::distance(root.begin(), root.end())) continue;

        const std::string text = fs::to_string(other);
        const std::string path = fs::to_string(whole);

        OUGHT(!(path.rfind(text, 0) == 0 && (path.size() == text.size() || path[text.size()] == '/')));
    }

    OUGHT(fs::volume(root).get().space.total > 0);
}

#if !defined(_WIN32)

// The root of the file system is always in the list.
TRIAL("fs: the root of the file system is among the roots") {
    const std::vector<fs::path> roots = fs::roots().get();

    OUGHT(std::find(roots.begin(), roots.end(), fs::path{"/"}) != roots.end());
}

#endif

#if defined(__linux__) && !defined(TALCITE_GENERIC_POSIX)

// A mount point is described by what the mount table knows about it: the file system and the device
// or label, not just its path, as for a directory that is not a mount point.
TRIAL("fs: the volume of a mount point speaks from the mount table") {
    const fs::Volume root = fs::volume(fs::path{"/"}).get();

    OUGHT(!root.format.empty());
    OUGHT(root.label != "/");
}

#endif

// The volume under a root says the same as its row in the list.
TRIAL("fs: the volume of a root is as in the list") {
    for (const fs::Volume& one : volumes()) {
        const fs::Volume alone = fs::volume(one.where).get();

        SAME(alone.where, one.where);
        SAME(alone.format, one.format);
        SAME(alone.label, one.label);
        OUGHT(alone.mount == one.mount);
        SAME(alone.source, one.source);
    }
}

// A path that is not a root gets the kind of the volume it lies on, and its own name.
TRIAL("fs: the volume of an ordinary directory") {
    const Nook nook;

    const fs::Volume made = fs::volume(nook.where()).get();

    OUGHT(made.mount == fs::mount(nook.where()).get());
    OUGHT(made.space.total > 0);
    OUGHT(made.space.free <= made.space.total);

#ifndef _WIN32
    SAME(made.where, nook.where());
    SAME(made.label, nook.where().filename().string());
#endif
}

#ifdef _WIN32

// A drive letter without a slash means "the current directory on that drive" on Windows, so the
// volume gets the root.
TRIAL("fs: the volume of a drive letter without a slash") {
    const fs::path system = fs::roots().get().front();

    const std::wstring letter = system.wstring().substr(0, 2);

    SAME(fs::volume(fs::path{letter}).get().where, system);
}

#endif

TRIAL("fs: a network share carries what stands behind it") {
    const std::vector<fs::Volume> among = volumes();

    for (const fs::Volume& one : among)
        if (one.mount != fs::Mount::remote) OUGHT(one.source.empty());
}

#ifndef _WIN32

// Permissions, the number of links, the owner and the group: the four fields on which MC's "long"
// listing will stand. The permissions are those of the entry, not of what it points to: a link has
// its own lrwxrwxrwx.
TRIAL("fs: list carries the permissions, the number of links, the owner and the group") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    std::error_code ignored;

    stood::permissions(one, stood::perms::owner_read | stood::perms::owner_write |
                                stood::perms::group_read,
                       stood::perm_options::replace, ignored);

    stood::create_symlink(one, nook.where() / "skrót", ignored);

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{2});

    // Ordered by path: plik.txt, then the link.
    SAME(among[0].where.filename().string(), std::string{"plik.txt"});
    SAME(among[0].rights, stood::perms::owner_read | stood::perms::owner_write |
                              stood::perms::group_read);
    SAME(among[0].links, std::uintmax_t{1});
    OUGHT(among[0].owner.has_value());
    OUGHT(among[0].group.has_value());
    SAME(*among[0].owner, static_cast<std::uint32_t>(::getuid()));
    SAME(*among[0].group, static_cast<std::uint32_t>(::getgid()));

    // A link has its own permissions, not those of its target.
    SAME(among[1].where.filename().string(), std::string{"skrót"});
    SAME(among[1].rights, stood::perms::all);

    // A hard link shows in the number of names; only that makes it possible to show MC's HARDLINK
    // role.
    fs::link(nook.where() / "druga nazwa", one, true).get();

    const std::vector<fs::Entry> potem = fs::list(nook.where()).get();

    for (const fs::Entry& each : potem)
        if (each.where.filename() == "plik.txt" || each.where.filename() == "druga nazwa")
            SAME(each.links, std::uintmax_t{2});
}

// One entry without reading the directory: the same as the listing brings, including the number of
// names. Writing "alongside and rename" stands on this, since it must know whether it would break a
// hard link.
TRIAL("fs: entry returns one entry with the permissions, the number of names and the link target") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    std::error_code ignored;

    stood::permissions(one, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read,
                       stood::perm_options::replace, ignored);

    fs::Entry got = fs::entry(one).get();

    SAME(got.where, one);
    SAME(static_cast<int>(got.kind), static_cast<int>(fs::Kind::file));
    SAME(got.size, std::uintmax_t{7});
    SAME(got.rights, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read);
    SAME(got.links, std::uintmax_t{1});

    fs::link(nook.where() / "druga nazwa", one, true).get();

    SAME(fs::entry(one).get().links, std::uintmax_t{2});

    stood::create_symlink(one, nook.where() / "skrót", ignored);

    got = fs::entry(nook.where() / "skrót").get();

    SAME(static_cast<int>(got.kind), static_cast<int>(fs::Kind::link));
    OUGHT(got.target.has_value());
    SAME(*got.target, one);

    // A missing entry is not an error, just `none`.
    SAME(static_cast<int>(fs::entry(nook.where() / "nie ma").get().kind), static_cast<int>(fs::Kind::none));
}

// A folder closed to everyone but its owner stays open to the owner all the way down: what it held
// before and what comes into it later.
TRIAL("fs: owner-only rights on a folder keep its contents open to the owner") {
    const Nook nook;

    const fs::path folder = nook.where() / "schowek";

    fs::make_folder(folder / "stare").get();
    fs::write(folder / "stare" / "a.txt", bytes("aaa")).get();

    fs::rights(folder, stood::perms::owner_all).get();

    SAME(said_of(fs::read(folder / "stare" / "a.txt").get()), std::string{"aaa"});

    fs::make_folder(folder / "nowe").get();
    fs::write(folder / "nowe" / "b.txt", bytes("bbb")).get();

    SAME(said_of(fs::read(folder / "nowe" / "b.txt").get()), std::string{"bbb"});
    SAME(fs::list(folder).get().size(), std::size_t{2});
}

TRIAL("fs: rights changes the permissions and follows a link") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write).get();

    SAME(fs::entry(one).get().rights, stood::perms::owner_read | stood::perms::owner_write);

    std::error_code ignored;

    stood::create_symlink(one, nook.where() / "skrót", ignored);

    fs::rights(nook.where() / "skrót", stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read).get();

    SAME(fs::entry(one).get().rights, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read);

    THREW(fs::rights(nook.where() / "nie ma", stood::perms::owner_read).get());
}

// The attribute tells about what the entry points to, and the permissions about the entry itself: a
// link to a read-only file is "read-only", although it has lrwxrwxrwx itself. This distinction
// costs a second question to the system per link, and that is why it is stated here explicitly.
TRIAL("fs: a link has its own permissions and its target's attributes") {
    const Nook nook;

    const fs::path one = nook.made("tylko-do-odczytu.txt", "treść");

    std::error_code ignored;

    stood::permissions(one, stood::perms::owner_read, stood::perm_options::replace, ignored);
    stood::create_symlink(one, nook.where() / "skrót", ignored);

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{2});
    SAME(among[0].where.filename().string(), std::string{"skrót"});

    SAME(among[0].rights, stood::perms::all);
    OUGHT(among[0].attributes.readonly);
    OUGHT(!among[0].attributes.stale);

    // The same file without a link: the permissions and the attribute say the same.
    OUGHT(among[1].attributes.readonly);
    SAME(among[1].rights, stood::perms::owner_read);

    stood::permissions(one, stood::perms::owner_read | stood::perms::owner_write,
                       stood::perm_options::replace, ignored);
}

// The entry's time comes straight from lstat, not through the standard library's file clock, so
// what we set has to come back to the second.
TRIAL("fs: list returns the modification time as it was set") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    const std::chrono::sys_seconds when{std::chrono::seconds{1'700'000'000}};

    fs::modified(one, when).get();

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{1});
    SAME(among[0].modified, when);
}

TRIAL("fs: the owner and group name by number, an unknown number without a name") {
    const std::string mine = fs::owner(static_cast<std::uint32_t>(::getuid())).get();

    OUGHT(!mine.empty());

    // A second question about the same number comes from memory, with the same answer.
    SAME(fs::owner(static_cast<std::uint32_t>(::getuid())).get(), mine);

    OUGHT(!fs::group(static_cast<std::uint32_t>(::getgid())).get().empty());

    // A number the system does not know returns nothing instead of throwing: whoever shows it then
    // prints the number.
    SAME(fs::owner(4294967294U).get(), std::string{});
    SAME(fs::group(4294967294U).get(), std::string{});
}

TRIAL("fs: list sees the execute bit and a link without a target") {
    const Nook nook;

    const fs::path program = nook.made("uruchom.sh", "#!/bin/sh\n");
    const fs::path plain = nook.made("dane.txt", "x");

    std::error_code ignored;

    stood::permissions(program, stood::perms::owner_exec, stood::perm_options::add, ignored);
    stood::create_symlink(nook.where() / "nie-ma", nook.where() / "wiszace", ignored);
    stood::create_symlink(plain, nook.where() / "zywe", ignored);

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{4});

    // Ordered by path: dane.txt, uruchom.sh, wiszace, zywe.
    OUGHT(!among[0].attributes.executable);
    OUGHT(among[1].attributes.executable);
    OUGHT(!among[1].attributes.stale);

    SAME(static_cast<int>(among[2].kind), static_cast<int>(fs::Kind::link));
    OUGHT(among[2].attributes.stale);

    SAME(static_cast<int>(among[3].kind), static_cast<int>(fs::Kind::link));
    OUGHT(!among[3].attributes.stale);

    // Only a link carries a target, a dangling one too, because the target is a string in the link,
    // not a file.
    OUGHT(!among[0].target.has_value());
    OUGHT(!among[1].target.has_value());
    OUGHT(among[2].target.has_value());
    SAME(*among[2].target, nook.where() / "nie-ma");
    OUGHT(among[3].target.has_value());
    SAME(*among[3].target, plain);

    // A link's time is the time of the link itself: a dangling one has it too, not the year 1970.
    const std::chrono::sys_seconds now =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    OUGHT(among[2].modified > now - std::chrono::hours{1});
    OUGHT(among[2].modified <= now + std::chrono::minutes{1});
    OUGHT(among[3].modified > now - std::chrono::hours{1});
}

TRIAL("fs: a pipe is a special entry, not a file") {
    const Nook nook;

    const fs::path pipe = nook.where() / "rura";

    if (::mkfifo(pipe.c_str(), 0600) != 0) return;

    const std::vector<fs::Entry> among = fs::list(nook.where()).get();

    SAME(among.size(), std::size_t{1});
    SAME(static_cast<int>(among[0].kind), static_cast<int>(fs::Kind::special));
    SAME(static_cast<int>(fs::kind(pipe).get()), static_cast<int>(fs::Kind::special));
}

#endif

// A path as text is always UTF-8, on every system. path::string() under MSVC returns the current
// ANSI code page, which garbles file names with Polish letters. This trial always passes on POSIX;
// on Windows it would fail with `string()` and passes with `to_string`, and that is its whole
// value: it guards a difference that one of the two systems does not see at all.
TRIAL("Files: a path becomes UTF-8 text and comes back as the same path") {
    const std::string said = "Zażółć gęślą jaźń.txt";

    const fs::path where = fs::from_string(said);

    SAME(fs::to_string(where), said);

    // Byte for byte, not "more or less": the text must be the same UTF-8 we gave.
    SAME(fs::to_string(where).size(), said.size());

    // And in a composition, because that is how the program uses it: a directory plus a name typed
    // by a person.
    const fs::path whole = fs::from_string("/home/user") / fs::from_string(said);

    SAME(fs::to_string(whole.filename()), said);
}

#ifdef _WIN32

namespace {

// A file's access list as Windows sees it: whether it is protected from inheritance, how many
// entries it has, and whether the first belongs to the user who started the trial.
struct Guard {
    bool shielded = false;
    DWORD entries = 0;
    bool mine = false;
};

// Reads the file's access list and compares its first entry with the user of the current process;
// all false when it cannot be read.
Guard guard(const fs::path& where) {
    PACL list = nullptr;
    PSECURITY_DESCRIPTOR held = nullptr;

    Guard made;

    if (GetNamedSecurityInfoW(where.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &list, nullptr, &held) != ERROR_SUCCESS)
        return made;

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;

    (void)GetSecurityDescriptorControl(held, &control, &revision);

    made.shielded = (control & SE_DACL_PROTECTED) != 0;
    made.entries = list != nullptr ? list->AceCount : 0;

    HANDLE token = nullptr;
    DWORD size = 0;

    (void)OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);

    std::vector<std::byte> user(size);

    (void)GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);

    void* first = nullptr;

    if (made.entries > 0 && GetAce(list, 0, &first))
        made.mine = EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(first)->SidStart,
                             reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) != 0;

    LocalFree(held);

    return made;
}

}

TRIAL("fs: on Windows owner-only permissions are a protected list with one entry") {
    const Nook nook;

    const fs::path one = nook.made("klucze.json", "tajne");

    OUGHT(!guard(one).shielded);

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write).get();

    const Guard narrowed = guard(one);

    OUGHT(narrowed.shielded);
    SAME(narrowed.entries, DWORD{1});
    OUGHT(narrowed.mine);

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read).get();

    OUGHT(!guard(one).shielded);
}

TRIAL("fs: on Windows a permission for others alone also removes the protection") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write).get();

    OUGHT(guard(one).shielded);

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write | stood::perms::others_read).get();

    OUGHT(!guard(one).shielded);
}

#endif

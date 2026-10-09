#include "proving.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "talcite/fs.hpp"
#include "talcite/time.hpp"

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

// Rodzaj zmiany widzianej przez `Watch` — skrót, bo próby obserwowania pytają o niego co wiersz.
using Changed = fs::Change::Kind;

namespace stood = std::filesystem;

// Katalog na czas jednego przypadku. Sprząta po sobie, także gdy przypadek padnie.
class Nook {
public:
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-fs-" + std::to_string(++counted) + "-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this))};

        std::error_code ignored;
        stood::remove_all(where_, ignored);
        stood::create_directories(where_, ignored);
    }

    ~Nook() {
        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Nook(const Nook&) = delete;
    Nook& operator=(const Nook&) = delete;
    Nook(Nook&&) = delete;
    Nook& operator=(Nook&&) = delete;

    [[nodiscard]] const fs::path& where() const { return where_; }

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

TRIAL("fs: exists odpowiada, a nie rzuca") {
    const Nook nook;

    const fs::path one = nook.made("jest.txt", "cokolwiek");

    ought(fs::exists(one).await());
    ought(!fs::exists(nook.where() / "nie ma.txt").await());
}

TRIAL("fs: kind rozróżnia plik, katalog i pustkę") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "x");
    const fs::path two = nook.where() / "katalog";

    fs::make_folder(two).await();

    same(static_cast<int>(fs::kind(one).await()), static_cast<int>(fs::Kind::file));
    same(static_cast<int>(fs::kind(two).await()), static_cast<int>(fs::Kind::folder));
    same(static_cast<int>(fs::kind(nook.where() / "puste").await()),
         static_cast<int>(fs::Kind::none));
}

TRIAL("fs: size mierzy zawartość") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", std::string(1234, 'a'));

    same(fs::size(one).await(), std::uintmax_t{1234});
}

TRIAL("fs: size na pustce odrzuca, a powód niesie ścieżkę") {
    const Nook nook;

    const fs::path missing = nook.where() / "nie ma.bin";

    bool caught = false;
    fs::path told;

    try {
        (void)fs::size(missing).await();
    } catch (const fs::Trouble& why) {
        caught = true;
        told = why.where();
    }

    ought(caught);
    same(told, missing);
}

TRIAL("fs: written wskazuje chwilę bliską teraz") {
    const Nook nook;

    const fs::path one = nook.made("świeży.txt", "x");

    const std::chrono::sys_seconds told = fs::modified(one).await();
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now());

    const auto apart = now - told;

    ought(apart > -60s);
    ought(apart < 600s);
}

TRIAL("fs: roomy podaje pojemność i wolne miejsce") {
    const Nook nook;

    const fs::Space told = fs::space(nook.where()).await();

    ought(told.total > 0);
    ought(told.free > 0);
    ought(told.free <= told.total);
}

TRIAL("fs: slice daje sensowną ratę") {
    const Nook nook;

    const std::size_t told = fs::chunk(nook.where()).await();

    ought(told >= 4096);
    ought(told <= 8 * 1024 * 1024);
    same(told & (told - 1), std::size_t{0});
}

TRIAL("fs: make tworzy całą drogę i nie przeszkadza mu, że już jest") {
    const Nook nook;

    const fs::path deep = nook.where() / "jeden" / "dwa" / "trzy";

    fs::make_folder(deep).await();

    ought(fs::exists(deep).await());

    fs::make_folder(deep).await();

    ought(fs::exists(deep).await());
}

TRIAL("fs: rename przenosi") {
    const Nook nook;

    const fs::path one = nook.made("stary.txt", std::string(5, 'x'));
    const fs::path two = nook.where() / "nowy.txt";

    fs::rename(one, two).await();

    ought(!fs::exists(one).await());
    ought(fs::exists(two).await());
    same(fs::size(two).await(), std::uintmax_t{5});
}

TRIAL("fs: rename na pustce odrzuca") {
    const Nook nook;

    threw(fs::rename(nook.where() / "nie ma", nook.where() / "gdziekolwiek").await());
}

TRIAL("fs: złota zasada — nic nie dzieje się bez pytania o wynik") {
    const Nook nook;

    const fs::path deep = nook.where() / "leniwy";

    Promise<void> making = fs::make_folder(deep);

    std::this_thread::sleep_for(100ms);

    ought(!fs::exists(deep).await());

    making.await();

    ought(fs::exists(deep).await());
}

TRIAL("fs: pytania składają się jak każde inne obietnice") {
    const Nook nook;

    const fs::path one = nook.made("a.txt", "aaa");
    const fs::path two = nook.made("b.txt", "bbbbb");

    Promise<std::uintmax_t> first = fs::size(one);
    Promise<std::uintmax_t> second = fs::size(two);

    Promise<std::uintmax_t>::all(first, second).await();

    same(first.await() + second.await(), std::uintmax_t{8});
}

TRIAL("fs: list podaje wpisy z opisem, nie same ścieżki") {
    const Nook nook;

    (void)nook.made("b.txt", std::string(11, 'x'));
    (void)nook.made("a.txt", std::string(3, 'x'));

    fs::make_folder(nook.where() / "katalog").await();

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{3});

    same(among[0].where.filename(), fs::path{"a.txt"});
    same(static_cast<int>(among[0].kind), static_cast<int>(fs::Kind::file));
    same(among[0].size, std::uintmax_t{3});

    same(among[1].where.filename(), fs::path{"b.txt"});
    same(among[1].size, std::uintmax_t{11});

    same(among[2].where.filename(), fs::path{"katalog"});
    same(static_cast<int>(among[2].kind), static_cast<int>(fs::Kind::folder));

    ought(among[0].modified.time_since_epoch().count() > 1700000000);
    ought(among[0].accessed.time_since_epoch().count() > 1700000000);
}

TRIAL("fs: list na pliku odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "x");

    threw((void)fs::list(one).await());
}

TRIAL("fs: list pustego katalogu daje pustkę") {
    const Nook nook;

    ought(fs::list(nook.where()).await().empty());
}

TRIAL("fs: walk schodzi w głąb") {
    const Nook nook;

    fs::make_folder(nook.where() / "jeden" / "dwa").await();

    (void)nook.made("na wierzchu.txt", "x");

    {
        std::ofstream making{nook.where() / "jeden" / "środkowy.txt", std::ios::binary};
        making << "xx";
    }
    {
        std::ofstream making{nook.where() / "jeden" / "dwa" / "głęboki.txt", std::ios::binary};
        making << "xxx";
    }

    const std::vector<fs::Entry> among = fs::walk(nook.where()).await();

    same(among.size(), std::size_t{5});

    std::size_t files = 0;
    std::size_t folders = 0;

    for (const fs::Entry& one : among) {
        if (one.kind == fs::Kind::file) ++files;
        if (one.kind == fs::Kind::folder) ++folders;
    }

    same(files, std::size_t{3});
    same(folders, std::size_t{2});
}

TRIAL("fs: walk daje wynik uporządkowany") {
    const Nook nook;

    fs::make_folder(nook.where() / "z").await();
    fs::make_folder(nook.where() / "a").await();

    const std::vector<fs::Entry> among = fs::walk(nook.where()).await();

    for (std::size_t at = 1; at < among.size(); ++at) ought(among[at - 1].where < among[at].where);
}

TRIAL("fs: jedno przejście zamiast tysiąca pytań") {
    const Nook nook;

    for (int at = 0; at < 200; ++at)
        (void)nook.made("plik-" + std::to_string(at) + ".bin", std::string(at, 'x'));

    Stopwatch held;

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{200});

    std::uintmax_t whole = 0;
    for (const fs::Entry& one : among) whole += one.size;

    same(whole, std::uintmax_t{200 * 199 / 2});

    // Dwieście rozmiarów przyszło razem z listą, więc nie ma tu dwustu przejść przez pulę.
    ought(held.spent() < 2000ms);
}

TRIAL("fs: anulowane przejście przerywa się w trakcie") {
    const Nook nook;

    for (int at = 0; at < 60; ++at) {
        const fs::path deep = nook.where() / ("gałąź-" + std::to_string(at));

        fs::make_folder(deep).await();

        for (int inner = 0; inner < 20; ++inner)
            (void)std::ofstream{deep / ("plik-" + std::to_string(inner) + ".bin"),
                                std::ios::binary};
    }

    Promise<std::vector<fs::Entry>> going = fs::walk(nook.where());

    // Bez odczekiwania: zadanie jest albo jeszcze w kolejce, albo dopiero co ruszyło. Pauza
    // dawałaby przejściu szansę skończyć się przed anulowaniem, a wtedy przypadek mierzyłby
    // wyścig zamiast mechanizmu.
    going.start();
    going.cancel();

    threw((void)going.await());
}

namespace {


std::string said_of(const fs::Bytes& given) {
    std::string made(given.size(), '\0');

    for (std::size_t at = 0; at < given.size(); ++at)
        made[at] = static_cast<char>(std::to_integer<unsigned char>(given[at]));

    return made;
}

}

TRIAL("File: otwiera do odczytu i czyta zakres") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    const fs::File held = fs::open(one).await();

    ought(held.held());
    same(held.size().await(), std::uintmax_t{10});

    same(said_of(held.read(3, 4).await()), std::string{"3456"});
    same(said_of(held.read(0, 10).await()), std::string{"0123456789"});
}

TRIAL("File: odczyt poza końcem oddaje tyle, ile jest") {
    const Nook nook;

    const fs::path one = nook.made("krótki.bin", "abc");

    const fs::File held = fs::open(one).await();

    same(said_of(held.read(1, 100).await()), std::string{"bc"});
    ought(held.read(50, 10).await().empty());
}

TRIAL("File: zapisuje w podanym miejscu, nie od początku") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "aaaaaaaaaa");

    const fs::File held = fs::open(one, fs::Mode::write).await();

    same(held.write(4, bytes("XY")).await(), std::size_t{2});

    same(said_of(held.read(0, 10).await()), std::string{"aaaaXYaaaa"});
}

TRIAL("File: write tworzy plik i zachowuje treść, fresh ją ucina") {
    const Nook nook;

    const fs::path one = nook.where() / "nowy.bin";

    {
        const fs::File held = fs::open(one, fs::Mode::write).await();
        (void)held.write(0, bytes("pierwsze")).await();
    }

    same(fs::size(one).await(), std::uintmax_t{8});

    {
        const fs::File held = fs::open(one, fs::Mode::write).await();
        (void)held.write(0, bytes("XX")).await();
    }

    same(fs::size(one).await(), std::uintmax_t{8});

    {
        const fs::File held = fs::open(one, fs::Mode::truncate).await();
        (void)held.write(0, bytes("XX")).await();
    }

    same(fs::size(one).await(), std::uintmax_t{2});
}

TRIAL("File: otwarcie nieistniejącego do odczytu odrzuca") {
    const Nook nook;

    threw((void)fs::open(nook.where() / "nie ma.bin").await());
}

// Kopie dzielą jeden otwarty plik, więc **zamknięcie zamyka go wszystkim**: kopia zostaje w każdej
// obietnicy, przez którą plik przeszedł, i dawne zamknięcie samej kopii zostawiało uchwyt otwarty
// — na Windows tak długo, że zastąpienie pliku odmawiało. Bez zamknięcia kopie czytają to samo.
TRIAL("File: kopie dzielą ten sam otwarty plik, a zamknięcie zamyka go wszystkim") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abcdef");

    fs::File first = fs::open(one).await();
    const fs::File second = first;

    same(said_of(second.read(0, 6).await()), std::string{"abcdef"});
    same(said_of(first.read(0, 3).await()), std::string{"abc"});

    const fs::Cursor flow = second.cursor();

    first.close();

    ought(!first.held());

    // Odmowa mówi, co się stało, zamiast błędu odczytu z uchwytu, którego już nie ma.
    const auto refused = [](auto doing) {
        try {
            doing();
        } catch (const fs::Trouble& trouble) {
            return std::string{trouble.what()} == "The file is not open.";
        }

        return false;
    };

    fs::Cursor moving = flow;

    ought(refused([&] { (void)second.read(0, 6).await(); }));
    ought(refused([&] { (void)second.write(0, bytes("x")).await(); }));
    ought(refused([&] { (void)second.size().await(); }));
    ought(refused([&] { second.sync().await(); }));
    ought(refused([&] { (void)moving.read(3).await(); }));

#ifndef _WIN32
    // Deskryptor naprawdę wrócił do systemu: żaden otwarty w procesie nie wskazuje już tego pliku.
    std::error_code ignored;

    for (const stood::directory_entry& fd : stood::directory_iterator{"/proc/self/fd", ignored})
        ought(stood::read_symlink(fd.path(), ignored) != stood::canonical(one, ignored));
#endif
}

TRIAL("File: zamknięty odrzuca zamiast się wywracać") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abc");

    fs::File held = fs::open(one).await();
    held.close();

    threw((void)held.read(0, 3).await());
    threw((void)held.size().await());
}

TRIAL("File: czytanie ratami składa cały plik") {
    const Nook nook;

    std::string whole;
    for (int at = 0; at < 5000; ++at) whole += static_cast<char>('a' + (at % 26));

    const fs::path one = nook.made("duży.bin", whole);

    const fs::File held = fs::open(one).await();

    const std::uintmax_t total = held.size().await();
    const std::size_t rata = 512;

    std::string made;

    for (std::uintmax_t at = 0; at < total; at += rata)
        made += said_of(held.read(at, rata).await());

    same(made, whole);
}

namespace {

std::string ladder(std::size_t much) {
    std::string made;
    made.reserve(much);

    for (std::size_t at = 0; at < much; ++at) made += static_cast<char>('a' + (at % 26));

    return made;
}

}

TRIAL("fs: read wciąga cały plik") {
    const Nook nook;

    const std::string whole = ladder(70000);
    const fs::path one = nook.made("duży.bin", whole);

    same(said_of(fs::read(one).await()), whole);
}

TRIAL("fs: read pustego pliku daje pustkę") {
    const Nook nook;

    const fs::path one = nook.made("puste.bin", "");

    ought(fs::read(one).await().empty());
}

TRIAL("fs: read nieistniejącego odrzuca") {
    const Nook nook;

    threw((void)fs::read(nook.where() / "nie ma.bin").await());
}

TRIAL("fs: write i read wracają tym samym") {
    const Nook nook;

    const std::string whole = ladder(70000);
    const fs::path one = nook.where() / "zapis.bin";

    fs::write(one, bytes(whole)).await();

    same(fs::size(one).await(), static_cast<std::uintmax_t>(whole.size()));
    same(said_of(fs::read(one).await()), whole);
}

TRIAL("fs: write ścina to, co było wcześniej") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", std::string(5000, 'x'));

    fs::write(one, bytes("krótko")).await();

    same(said_of(fs::read(one).await()), std::string{"krótko"});
}

TRIAL("fs: postęp odczytu rośnie i kończy się na jedynce") {
    const Nook nook;

    const fs::path one = nook.made("duży.bin", ladder(200000));

    Promise<fs::Bytes> going = fs::read(one);

    same(going.progress(), 0.0);

    (void)going.await();

    same(going.progress(), 1.0);
}

TRIAL("fs: copy odwzorowuje plik co do bajtu") {
    const Nook nook;

    const std::string whole = ladder(70000);
    const fs::path from = nook.made("źródło.bin", whole);
    const fs::path to = nook.where() / "cel.bin";

    fs::copy(from, to).await();

    same(fs::size(to).await(), static_cast<std::uintmax_t>(whole.size()));
    same(said_of(fs::read(to).await()), whole);
    ought(fs::exists(from).await());
}

TRIAL("fs: copy przenosi całe drzewo") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from / "w głąb" / "głębiej").await();

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

    fs::copy(from, to).await();

    same(said_of(fs::read(to / "pierwszy.txt").await()), std::string{"jeden"});
    same(said_of(fs::read(to / "w głąb" / "drugi.txt").await()), std::string{"dwa"});
    same(said_of(fs::read(to / "w głąb" / "głębiej" / "trzeci.txt").await()),
         std::string{"trzy"});

    same(fs::walk(to).await().size(), fs::walk(from).await().size());
}

// Czekamy, aż kopia naprawdę ruszy — inaczej anulowanie trafiłoby zadanie jeszcze przed
// pulą i przypadek przechodziłby, nic nie sprawdzając. Patrzymy na cel, a nie na postęp:
// postęp rozstrzygniętej obietnicy wynosi jeden także po odrzuceniu, więc nie odróżnia
// pracy trwającej od pracy, która nigdy nie ruszyła.
bool grew(const fs::path& to, std::uintmax_t past) {
    for (int at = 0; at < 400; ++at) {
        std::error_code ignored;

        if (stood::exists(to, ignored) && stood::file_size(to, ignored) > past) return true;

        std::this_thread::sleep_for(5ms);
    }

    return false;
}

// Anulowanie może trafić kopię przed pierwszą ratą albo już po ostatniej — nie da się tego
// wymusić. Umowa brzmi inaczej i sprawdza się zawsze: cel jest albo pełny, albo go nie ma.
TRIAL("fs: anulowana kopia zostawia cel pełny albo żaden") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.where() / "cel.bin";

    Promise<void> going = fs::copy(from, to);

    going.start();

    ought(grew(to, 0));

    going.cancel();

    Time::later(300ms).await();

    if (fs::exists(to).await()) same(fs::size(to).await(), fs::size(from).await());
}

TRIAL("fs: kopia porzucona przed ruszeniem nie tworzy celu") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.where() / "cel.bin";

    Promise<void> going = fs::copy(from, to);

    going.start();
    going.cancel();

    Time::later(300ms).await();

    if (fs::exists(to).await()) same(fs::size(to).await(), fs::size(from).await());
}

// Zastąpienie pisze obok (`.cel.bin.XXXXXX.part`) i dopiero całość przemianowuje na cel, więc
// anulowanie w połowie zostawia cel **ze starą treścią**, a pliku obok nie zostawia wcale.
TRIAL("fs: anulowana kopia nie rusza celu, który już był") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.made("cel.bin", "swoje");

    Promise<void> going = fs::copy(from, to, fs::Conflict::overwrite);

    going.start();

    bool aside = false;

    for (int at = 0; at < 400 && !aside; ++at) {
        for (const fs::Entry& one : fs::list(nook.where()).await())
            if (fs::to_string(one.where.filename()).find(".part") != std::string::npos) aside = true;

        if (!aside) std::this_thread::sleep_for(5ms);
    }

    going.cancel();

    Time::later(300ms).await();

    const std::string now = said_of(fs::read(to).await());

    ought(now == "swoje" || now.size() == 8000000);

    for (const fs::Entry& one : fs::list(nook.where()).await())
        ought(fs::to_string(one.where.filename()).find(".part") == std::string::npos);
}

// Plik na katalog z zastąpieniem: zapis obok się udaje, ale przemianowanie na katalog — nie. Odmowa
// i ani śladu pliku obok.
TRIAL("fs: kopia pliku na katalog odmawia i nie zostawia pliku obok") {
    const Nook nook;

    const fs::path from = nook.made("film.mkv", "film");
    const fs::path to = nook.where() / "katalog";

    fs::make_folder(to).await();

    threw(fs::copy(from, to, fs::Conflict::overwrite).await());

    ought(fs::kind(to).await() == fs::Kind::folder);

    for (const fs::Entry& one : fs::list(nook.where()).await())
        ought(fs::to_string(one.where.filename()).find(".part") == std::string::npos);
}

// Drugie dowiązanie twarde do zastępowanego pliku zostaje przy starej treści: zastąpienie daje
// celowi nowy plik, a nie przepisuje starego w miejscu.
TRIAL("fs: zastąpienie nie przepisuje drugiego dowiązania") {
    const Nook nook;

    const fs::path from = nook.made("nowy.txt", "nowa treść");
    const fs::path to = nook.made("cel.txt", "stara");
    const fs::path twin = nook.where() / "bliźniak.txt";

    std::error_code trouble;

    stood::create_hard_link(to, twin, trouble);

    if (trouble) return;

    fs::copy(from, to, fs::Conflict::overwrite).await();

    same(said_of(fs::read(to).await()), std::string{"nowa treść"});
    same(said_of(fs::read(twin).await()), std::string{"stara"});
}

// Katalog przenoszony do własnego wnętrza: dawniej system odmawiał zmiany nazwy, `move` brał to za
// inny nośnik, kopiował drzewo do środka samego siebie i kasował źródło — znikało wszystko, a obietnica
// mówiła „gotowe". Dziś odmowa i nic nie rusza się z miejsca.
TRIAL("fs: move katalogu do własnego wnętrza odmawia i niczego nie rusza") {
    const Nook nook;

    const fs::path from = nook.where() / "biblioteka";

    fs::make_folder(from / "wnętrze").await();

    const fs::path film = nook.made("biblioteka/film.mkv", "film");

    std::string said;

    try {
        fs::move(from, from / "wnętrze" / "nowe").await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    ought(said.find("into itself") != std::string::npos);
    same(said_of(fs::read(film).await()), std::string{"film"});
    ought(!fs::exists(from / "wnętrze" / "nowe").await());

    threw(fs::rename(from, from / "nowe").await());
    ought(fs::exists(film).await());
}

// Katalog nadpisywany katalogiem łączy się z nim wpis po wpisie: pliki obu zostają, wspólna nazwa
// dostaje treść przenoszonego, a źródło znika.
TRIAL("fs: move katalogu na katalog z overwrite łączy oba") {
    const Nook nook;

    const fs::path from = nook.where() / "nowy";
    const fs::path to = nook.where() / "stary";

    fs::make_folder(from / "w głąb").await();
    fs::make_folder(to).await();

    (void)nook.made("nowy/wspólny.txt", "nowa");
    (void)nook.made("nowy/w głąb/tylko nowy.txt", "n");
    (void)nook.made("stary/wspólny.txt", "stara");
    (void)nook.made("stary/tylko stary.txt", "s");

    fs::move(from, to, fs::Conflict::overwrite).await();

    ought(!fs::exists(from).await());
    same(said_of(fs::read(to / "wspólny.txt").await()), std::string{"nowa"});
    same(said_of(fs::read(to / "tylko stary.txt").await()), std::string{"s"});
    same(said_of(fs::read(to / "w głąb" / "tylko nowy.txt").await()), std::string{"n"});

    // Łączenie w obrębie nośnika to zmiany nazw — nie pyta o wolne miejsce, więc plik większy niż
    // całe wolne miejsce (rzadki, na dysku prawie nic) przechodzi.
    const fs::path big = nook.where() / "duży";

    fs::make_folder(big).await();
    fs::make_folder(nook.where() / "pełny").await();

    std::error_code trouble;

    const stood::space_info room = stood::space(nook.where(), trouble);

    if (trouble) return;

    stood::resize_file(nook.made("duży/film.mkv", ""), room.available + (std::uintmax_t{1} << 30), trouble);

    if (trouble) return;

    fs::move(big, nook.where() / "pełny", fs::Conflict::overwrite).await();

    ought(fs::exists(nook.where() / "pełny" / "film.mkv").await());
    ought(!fs::exists(big).await());
}

// Wyłączne utworzenie: drugi raz ten sam plik odmawia i mówi dlaczego, a pierwszy zostaje.
TRIAL("fs: Mode::exclusive tworzy tylko nowy plik") {
    const Nook nook;

    const fs::path one = nook.where() / "numer.json";

    {
        const fs::File file = fs::open(one, fs::Mode::exclusive).await();

        same(file.write(0, bytes("pierwszy")).await(), std::size_t{8});

        file.sync().await();
    }

    std::string said;

    try {
        (void)fs::open(one, fs::Mode::exclusive).await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    ought(said.find("already exists") != std::string::npos);
    same(said_of(fs::read(one).await()), std::string{"pierwszy"});
}

// Nośnik mówi, jaki ma system plików — sprawdzian przed kopią pyta o granicę FAT32. Droga
// przenośna nazwy systemu plików nie zna i mówi to pustym napisem.
#if !defined(TALCITE_GENERIC_POSIX)
TRIAL("fs: wykaz nośników zna system plików") {
    bool told = false;

    for (const fs::Volume& one : fs::volumes().await())
        if (!one.format.empty()) told = true;

    ought(told);
}
#endif

// Wykaz nośników nigdy nie jest pusty — choćby sam korzeń, a na Windows litera dysku systemu.
TRIAL("fs: wykaz nośników nie jest pusty") {
    const std::vector<fs::Volume> found = fs::volumes().await();

    ought(!found.empty());

    for (const fs::Volume& one : found) ought(!one.where.empty());
}

TRIAL("fs: copy nieistniejącego odrzuca") {
    const Nook nook;

    threw(fs::copy(nook.where() / "nie ma.bin", nook.where() / "cel.bin").await());
}

TRIAL("fs: move w obrębie nośnika zabiera źródło") {
    const Nook nook;

    const std::string whole = ladder(3000);
    const fs::path from = nook.made("źródło.bin", whole);
    const fs::path to = nook.where() / "cel.bin";

    fs::move(from, to).await();

    ought(!fs::exists(from).await());
    same(said_of(fs::read(to).await()), whole);
}

TRIAL("fs: move przenosi też katalog") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from / "w głąb").await();

    {
        std::ofstream making{from / "w głąb" / "plik.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path to = nook.where() / "gdzie indziej";

    fs::move(from, to).await();

    ought(!fs::exists(from).await());
    same(said_of(fs::read(to / "w głąb" / "plik.txt").await()), std::string{"treść"});
}

TRIAL("fs: move na nieistniejącym odrzuca") {
    const Nook nook;

    threw(fs::move(nook.where() / "nie ma.bin", nook.where() / "cel.bin").await());
}

TRIAL("fs: erase zdejmuje pojedynczy plik") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "cokolwiek");

    fs::erase(one).await();

    ought(!fs::exists(one).await());
}

TRIAL("fs: erase zdejmuje całe drzewo") {
    const Nook nook;

    const fs::path where = nook.where() / "drzewo";
    fs::make_folder(where / "w głąb" / "głębiej").await();

    {
        std::ofstream making{where / "w głąb" / "plik.txt", std::ios::binary};
        making << "treść";
    }

    fs::erase(where).await();

    ought(!fs::exists(where).await());
}

TRIAL("fs: erase pustki nie jest błędem") {
    const Nook nook;

    fs::erase(nook.where() / "nie ma").await();
}

TRIAL("fs: postęp usuwania kończy się na jedynce") {
    const Nook nook;

    const fs::path where = nook.where() / "drzewo";
    fs::make_folder(where / "w głąb").await();

    for (int at = 0; at < 20; ++at) {
        std::ofstream making{where / "w głąb" / ("plik" + std::to_string(at) + ".txt"),
                             std::ios::binary};
        making << at;
    }

    Promise<void> going = fs::erase(where);

    same(going.progress(), 0.0);

    going.await();

    same(going.progress(), 1.0);
}

namespace {

// Dowiązania na Windows wymagają uprawnienia, którego bieg testów mieć nie musi. Gdy się
// nie da, przypadek kończy się bez rozstrzygnięcia zamiast fałszywie oskarżać moduł.
bool linked(const fs::path& where, const fs::path& onto, bool folder = false) {
    std::error_code trouble;

    if (folder) stood::create_directory_symlink(onto, where, trouble);
    else stood::create_symlink(onto, where, trouble);

    return !trouble;
}

}

// **Dowiązanie do katalogu to katalog do wejścia**, jak w MC: `list` i `walk` idą za nim, gdy stoi
// u samego korzenia. W głębi przejścia dowiązania zostają dowiązaniami — `walk` ich nie rozwija, bo
// dowiązanie do przodka zamknęłoby koło. Dowiązanie do pliku i wiszące dalej odrzucają.
TRIAL("fs: list i walk wchodzą przez dowiązanie do katalogu") {
    const Nook nook;

    fs::make_folder(nook.where() / "katalog" / "głąb").await();

    {
        std::ofstream making{nook.where() / "katalog" / "plik.txt", std::ios::binary};
        making << "x";
    }

    if (!linked(nook.where() / "skrót", nook.where() / "katalog", true)) return;

    const std::vector<fs::Entry> listed = fs::list(nook.where() / "skrót").await();

    same(listed.size(), std::size_t{2});
    same(listed[1].where, nook.where() / "skrót" / "plik.txt");

    // `reach` mówi, dokąd wpis prowadzi: zwykły wpis — tam, gdzie stoi; dowiązanie — do celu.
    const std::vector<fs::Entry> top = fs::list(nook.where()).await();

    same(top.size(), std::size_t{2});
    same(top[0].kind, fs::Kind::folder);
    same(top[0].reach, fs::Kind::folder);
    same(top[1].kind, fs::Kind::link);
    same(top[1].reach, fs::Kind::folder);

    ought(linked(nook.where() / "katalog" / "głąb" / "w górę", nook.where() / "katalog", true));

    const std::vector<fs::Entry> walked = fs::walk(nook.where() / "skrót").await();

    same(walked.size(), std::size_t{3});

    std::size_t links = 0;

    for (const fs::Entry& one : walked)
        if (one.kind == fs::Kind::link) ++links;

    same(links, std::size_t{1});

    ought(linked(nook.where() / "do pliku", nook.where() / "katalog" / "plik.txt"));
    ought(linked(nook.where() / "wiszące", nook.where() / "nie ma"));

    const std::vector<fs::Entry> all = fs::list(nook.where()).await();

    same(all[0].reach, fs::Kind::file);
    same(all[3].kind, fs::Kind::link);
    same(all[3].reach, fs::Kind::none);

    threw((void)fs::list(nook.where() / "do pliku").await());
    threw((void)fs::walk(nook.where() / "wiszące").await());
}

TRIAL("fs: copy odmawia, gdy cel istnieje") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    threw(fs::copy(from, to).await());

    same(said_of(fs::read(to).await()), std::string{"stare"});
}

TRIAL("fs: copy z overwrite nadpisuje") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Conflict::overwrite).await();

    same(said_of(fs::read(to).await()), std::string{"nowe"});
}

TRIAL("fs: copy ze skip zostawia cel w spokoju") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Conflict::skip).await();

    same(said_of(fs::read(to).await()), std::string{"stare"});
}

TRIAL("fs: copy z beside zakłada nazwę obok") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Conflict::rename).await();

    same(said_of(fs::read(to).await()), std::string{"stare"});
    same(said_of(fs::read(nook.where() / "cel (2).bin").await()), std::string{"nowe"});

    fs::copy(from, to, fs::Conflict::rename).await();

    same(said_of(fs::read(nook.where() / "cel (3).bin").await()), std::string{"nowe"});
}

TRIAL("fs: polityka działa na każdym wpisie drzewa z osobna") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).await();

    {
        std::ofstream making{from / "jeden.txt", std::ios::binary};
        making << "nowe";
    }
    {
        std::ofstream making{from / "dwa.txt", std::ios::binary};
        making << "nowe";
    }

    const fs::path to = nook.where() / "cel";
    fs::make_folder(to).await();

    {
        std::ofstream making{to / "jeden.txt", std::ios::binary};
        making << "stare";
    }

    fs::copy(from, to, fs::Conflict::skip).await();

    same(said_of(fs::read(to / "jeden.txt").await()), std::string{"stare"});
    same(said_of(fs::read(to / "dwa.txt").await()), std::string{"nowe"});
}

TRIAL("fs: move odmawia, gdy cel istnieje") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    threw(fs::move(from, to).await());

    ought(fs::exists(from).await());
    same(said_of(fs::read(to).await()), std::string{"stare"});
}

TRIAL("fs: rename odmawia, gdy cel istnieje") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    threw(fs::rename(from, to).await());

    same(said_of(fs::read(to).await()), std::string{"stare"});
}

// Kolizja przy zmianie nazwy rozstrzyga się tak samo jak przy kopii: `skip` zostawia oba pliki,
// `rename` kładzie obok — także przy polskich literach, bo nazwa obok składa się na postaci
// rodzimej ścieżki — a `overwrite` zastępuje.
TRIAL("fs: rename i move rozstrzygają kolizję według polityki") {
    const Nook nook;

    for (const bool moving : {false, true}) {
        const auto shifted = [moving](const fs::path& from, const fs::path& to, fs::Conflict how) {
            return moving ? fs::move(from, to, how) : fs::rename(from, to, how);
        };

        const fs::path from = nook.made("źródło.bin", "nowe");
        const fs::path to = nook.made("Gorączka.bin", "stare");

        shifted(from, to, fs::Conflict::skip).await();

        ought(fs::exists(from).await());
        same(said_of(fs::read(to).await()), std::string{"stare"});

        shifted(from, to, fs::Conflict::rename).await();

        ought(!fs::exists(from).await());
        same(said_of(fs::read(nook.where() / "Gorączka (2).bin").await()), std::string{"nowe"});

        const fs::path again = nook.made("źródło.bin", "trzecie");

        shifted(again, to, fs::Conflict::rename).await();

        same(said_of(fs::read(nook.where() / "Gorączka (3).bin").await()), std::string{"trzecie"});

        const fs::path last = nook.made("źródło.bin", "ostatnie");

        shifted(last, to, fs::Conflict::overwrite).await();

        ought(!fs::exists(last).await());
        same(said_of(fs::read(to).await()), std::string{"ostatnie"});

        fs::erase(nook.where() / "Gorączka (2).bin").await();
        fs::erase(nook.where() / "Gorączka (3).bin").await();
        fs::erase(to).await();
    }
}

// Pusty katalog to też zajęty cel. `std::filesystem::rename` na POSIX-ie zastępuje go po cichu,
// więc o zajęty cel pyta sam system, w tym samym wywołaniu co przeniesienie.
TRIAL("fs: rename i move nie zastępują pustego katalogu") {
    const Nook nook;

    const fs::path from = nook.where() / "źródło";
    const fs::path to = nook.where() / "cel";

    fs::make_folder(from).await();
    fs::make_folder(to).await();
    const fs::path inside = nook.made("źródło/plik.txt", "treść");

    threw(fs::rename(from, to).await());
    threw(fs::move(from, to).await());

    ought(fs::exists(inside).await());
    same(fs::list(to).await().size(), std::size_t{0});
}

// Nazwa obok, której system nie przyjmie (za długa), kończy szukanie od razu i mówi, co się stało —
// dalsze numery byłyby tak samo za długie, a zdanie „nie ma wolnej nazwy" byłoby nieprawdą.
//
// Cel pisze sama biblioteka: ścieżka z katalogiem tymczasowym Windows przekracza 260 znaków,
// a `std::ofstream` pod MSVC bez długich ścieżek pliku nie zakłada — rename szedł wtedy na wolne
// miejsce i się udawał. Na koniec cel wraca pod krótką nazwę, żeby sprzątanie katalogu go zdjęło.
TRIAL("fs: rename z nazwą obok nie do przyjęcia mówi, że nie przeniósł") {
    const Nook nook;

    const std::string stem(250, 'a');
    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.where() / (stem + ".bin");

    fs::write(to, bytes("stare")).await();

    ought(fs::exists(to).await());

    std::string said;

    try {
        fs::rename(from, to, fs::Conflict::rename).await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    ought(said.find("Cannot rename") != std::string::npos);
    ought(fs::exists(from).await());

    fs::rename(to, nook.where() / "stare.bin").await();
}

#ifdef _WIN32

// Plik za 260 znakami ścieżki: `std::filesystem` pod MSVC bez długich ścieżek mówi o nim „nie ma"
// bez błędu. Biblioteka widzi go mimo to — istnieje, jest plikiem i zapis z `fail` go nie rusza.
TRIAL("fs: na Windows plik za 260 znakami ścieżki stoi i nie jest wolnym miejscem") {
    const Nook nook;

    const fs::path deep = nook.where() / (std::string(250, 'b') + ".bin");

    fs::write(deep, bytes("stare")).await();

    ought(fs::exists(deep).await());
    same(static_cast<int>(fs::kind(deep).await()), static_cast<int>(fs::Kind::file));

    threw(fs::write(deep, bytes("nowe"), fs::Conflict::fail).await());

    same(said_of(fs::read(deep).await()), std::string{"stare"});

    fs::rename(deep, nook.where() / "krótka.bin").await();
}

#endif

#ifndef _WIN32

// Między nośnikami system odmawia zmiany nazwy, zanim w ogóle spojrzy na cel — `move` rozstrzyga
// więc kolizję jeszcze raz, zanim kopia cokolwiek nadpisze. Drugi nośnik to `/dev/shm`, gdy jest
// i leży gdzie indziej niż katalog próby; inaczej nie ma czego sprawdzić.
TRIAL("fs: move między nośnikami rozstrzyga kolizję przed kopią") {
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

    threw(fs::move(from, to).await());

    ought(fs::exists(from).await());
    same(said_of(fs::read(to).await()), std::string{"stare"});

    fs::move(from, to, fs::Conflict::rename).await();

    ought(!fs::exists(from).await());
    same(said_of(fs::read(away / "film (2).bin").await()), std::string{"nowe"});

    stood::remove_all(away, trouble);
}

namespace {

// Drugi nośnik do prób: `/dev/shm`, gdy leży na innym urządzeniu niż katalog próby.
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

// Między nośnikami katalog przenosi się wpis po wpisie: wszystko jest u celu z czasami zapisu,
// a źródła nie ma.
TRIAL("fs: move katalogu między nośnikami przenosi całość") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-drzewo");

    if (away.empty()) return;

    const fs::path from = nook.where() / "seriale";

    fs::make_folder(from / "Sezon 01").await();

    const fs::path episode = nook.made("seriale/Sezon 01/S01E01.mkv", ladder(5000));
    (void)nook.made("seriale/tvshow.nfo", "<tvshow/>");

    const auto when = std::chrono::sys_seconds{std::chrono::seconds{1500000000}};
    const auto season = std::chrono::sys_seconds{std::chrono::seconds{1400000000}};
    const auto show = std::chrono::sys_seconds{std::chrono::seconds{1300000000}};

    fs::modified(episode, when).await();
    fs::modified(from / "Sezon 01", season).await();
    fs::modified(from, show).await();

    fs::move(from, away / "seriale").await();

    ought(!fs::exists(from).await());
    same(said_of(fs::read(away / "seriale" / "Sezon 01" / "S01E01.mkv").await()), ladder(5000));
    same(said_of(fs::read(away / "seriale" / "tvshow.nfo").await()), std::string{"<tvshow/>"});
    ought(fs::modified(away / "seriale" / "Sezon 01" / "S01E01.mkv").await() == when);
    ought(fs::modified(away / "seriale" / "Sezon 01").await() == season);
    ought(fs::modified(away / "seriale").await() == show);

    // Plik na plik z innego nośnika, z zastąpieniem: cel dostaje nową treść, źródła nie ma.
    const fs::path single = nook.made("nowy.nfo", "nowy");

    (void)fs::write(away / "stary.nfo", bytes("stary")).await();

    fs::move(single, away / "stary.nfo", fs::Conflict::overwrite).await();

    ought(!fs::exists(single).await());
    same(said_of(fs::read(away / "stary.nfo").await()), std::string{"nowy"});

    std::error_code trouble;

    stood::remove_all(away, trouble);
}

// Wpis, którego przenieść się nie da (potok), odmawia całości **przed pierwszym ruchem** — dawniej
// kopia go pomijała, a `remove_all` kasował go razem ze źródłem.
TRIAL("fs: move między nośnikami z wpisem nie do przeniesienia nie rusza niczego") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-potok");

    if (away.empty()) return;

    const fs::path from = nook.where() / "z potokiem";

    fs::make_folder(from).await();

    const fs::path film = nook.made("z potokiem/film.mkv", "film");

    if (::mkfifo((from / "potok").c_str(), 0600) != 0) return;

    threw(fs::move(from, away / "cel").await());

    same(said_of(fs::read(film).await()), std::string{"film"});
    ought(!fs::exists(away / "cel").await());

    std::error_code trouble;

    stood::remove_all(away, trouble);
}

namespace {

// Granica rozmiaru pisanych plików (`RLIMIT_FSIZE`) na czas próby, z sygnałem `SIGXFSZ` zignorowanym
// — zapis ponad nią odmawia wtedy błędem zamiast zabijać program. Próba wolnego miejsca pisze
// na `/dev/shm`, który leży w pamięci: gdyby sprawdzian przed kopią zawiódł, kopia rzadkiego pliku
// zapełniłaby całą pamięć maszyny. Tak właśnie skończył się jeden mutant, zanim stanęła ta granica.
class Capped {
public:
    explicit Capped(rlim_t most) {
        held_ = ::getrlimit(RLIMIT_FSIZE, &was_) == 0;

        if (!held_) return;

        signal_ = std::signal(SIGXFSZ, SIG_IGN);

        rlimit now = was_;
        now.rlim_cur = most;

        held_ = ::setrlimit(RLIMIT_FSIZE, &now) == 0;
    }

    ~Capped() {
        if (held_) ::setrlimit(RLIMIT_FSIZE, &was_);

        std::signal(SIGXFSZ, signal_ == SIG_ERR ? SIG_DFL : signal_);
    }

    Capped(const Capped&) = delete;
    Capped& operator=(const Capped&) = delete;
    Capped(Capped&&) = delete;
    Capped& operator=(Capped&&) = delete;

    [[nodiscard]] bool held() const { return held_; }

private:
    rlimit was_{};
    void (*signal_)(int) = SIG_DFL;
    bool held_ = false;
};

}

// Kopia, która się nie zmieści, odmawia od razu i nie zostawia niczego. Plik jest rzadki — zajmuje
// na dysku tyle co nic, a mówi o sobie, że jest większy niż całe wolne miejsce celu.
TRIAL("fs: kopia większa niż wolne miejsce odmawia przed zapisem") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-miejsce");

    if (away.empty()) return;

    std::error_code trouble;

    const stood::space_info room = stood::space(away, trouble);

    if (trouble) return;

    const fs::path from = nook.made("ogromny.bin", "");

    stood::resize_file(from, room.available + (std::uintmax_t{1} << 30), trouble);

    if (trouble) return;

    const Capped capped{64 * 1024 * 1024};

    if (!capped.held()) return;

    std::string said;

    try {
        fs::copy(from, away / "ogromny.bin").await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    ought(said.find("free space") != std::string::npos);
    ought(!fs::exists(away / "ogromny.bin").await());

    said.clear();

    try {
        fs::move(from, away / "ogromny.bin").await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    ought(said.find("free space") != std::string::npos);
    ought(fs::exists(from).await());

    // Katalog liczy się sumą plików.
    const fs::path folder = nook.where() / "katalog";

    fs::make_folder(folder).await();
    fs::rename(from, folder / "ogromny.bin").await();

    said.clear();

    try {
        fs::copy(folder, away / "katalog").await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    ought(said.find("free space") != std::string::npos);
    ought(!fs::exists(away / "katalog").await());

    stood::remove_all(away, trouble);
}

#endif

#if defined(__linux__)

// Zmiana nazwy odmówiona **z innego powodu niż inny nośnik** to błąd, a nie zaproszenie do kopii.
// Plik niezmienny (`FS_IMMUTABLE_FL`) odmawia przemianowania nawet administratorowi; dawniej `move`
// kopiował go wtedy obok, kasowanie źródła po cichu się nie udawało i zostawały dwa egzemplarze
// z odpowiedzią „gotowe".
TRIAL("fs: move odmówiony przez system nie kopiuje") {
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
        fs::move(from, to).await();
    } catch (const fs::Trouble& why) {
        said = why.what();
    }

    // Z zastąpieniem tak samo: cel zostaje swój, źródło swoje.
    const fs::path taken = nook.made("zajęty.mkv", "inny");

    std::string again;

    try {
        fs::move(from, taken, fs::Conflict::overwrite).await();
    } catch (const fs::Trouble& why) {
        again = why.what();
    }

    ::ioctl(held, FS_IOC_SETFLAGS, &flags);
    ::close(held);

    ought(said.find("Cannot rename") != std::string::npos);
    ought(again.find("Cannot rename") != std::string::npos);
    ought(!fs::exists(to).await());
    same(said_of(fs::read(from).await()), std::string{"film"});
    same(said_of(fs::read(taken).await()), std::string{"inny"});
}

namespace {

// Flaga `FS_IMMUTABLE_FL` na czas próby; zdejmuje ją sama, także gdy próba padnie.
class Immutable {
public:
    explicit Immutable(const fs::path& where) : held_(::open(where.c_str(), O_RDONLY)) {
        if (held_ < 0 || ::ioctl(held_, FS_IOC_GETFLAGS, &was_) != 0) return;

        const int locked = was_ | FS_IMMUTABLE_FL;

        set_ = ::ioctl(held_, FS_IOC_SETFLAGS, &locked) == 0;
    }

    ~Immutable() {
        if (set_) ::ioctl(held_, FS_IOC_SETFLAGS, &was_);
        if (held_ >= 0) ::close(held_);
    }

    Immutable(const Immutable&) = delete;
    Immutable& operator=(const Immutable&) = delete;
    Immutable(Immutable&&) = delete;
    Immutable& operator=(Immutable&&) = delete;

    [[nodiscard]] bool set() const { return set_; }

private:
    int held_ = -1;
    int was_ = 0;
    bool set_ = false;
};

}

// Źródło, którego nie da się sprzątnąć po kopii między nośnikami: plik niezmienny zostaje i mówi
// o tym wprost, a pusty katalog niezmienny zostawia źródło i zdanie, że coś zostało.
TRIAL("fs: move między nośnikami mówi, co zostało w źródle") {
    const Nook nook;

    const fs::path away = elsewhere(nook, "talcite-zostalo");

    if (away.empty()) return;

    {
        const fs::path from = nook.where() / "z plikiem";

        fs::make_folder(from).await();

        const fs::path stuck = nook.made("z plikiem/film.mkv", "film");

        const Immutable lock{stuck};

        if (!lock.set()) return;

        std::string said;

        try {
            fs::move(from, away / "z plikiem").await();
        } catch (const fs::Trouble& why) {
            said = why.what();
        }

        ought(said.find("Cannot remove the source") != std::string::npos);
        same(said_of(fs::read(away / "z plikiem" / "film.mkv").await()), std::string{"film"});
        ought(fs::exists(stuck).await());
    }

    {
        const fs::path from = nook.where() / "z katalogiem";

        fs::make_folder(from / "pusty").await();
        (void)nook.made("z katalogiem/film.mkv", "film");

        const Immutable lock{from / "pusty"};

        if (!lock.set()) return;

        std::string said;

        try {
            fs::move(from, away / "z katalogiem").await();
        } catch (const fs::Trouble& why) {
            said = why.what();
        }

        ought(said.find("stayed behind") != std::string::npos);
        same(said_of(fs::read(away / "z katalogiem" / "film.mkv").await()), std::string{"film"});
        ought(!fs::exists(from / "film.mkv").await());
        ought(fs::exists(from / "pusty").await());
    }

    std::error_code trouble;

    stood::remove_all(away, trouble);
}

#endif

// Zmiana nazwy na samą siebie niczego nie robi i się udaje. Na systemie bez wielkości liter tą
// samą drogą idzie `heat` → `Heat`: cel „stoi", ale to ten sam wpis. Dowiązanie twarde to wpis
// osobny, choć `equivalent` — tego nadpisać nie wolno.
TRIAL("fs: rename na samego siebie się udaje, na dowiązanie twarde — odmawia") {
    const Nook nook;

    const fs::path one = nook.made("jeden.txt", "treść");

    fs::rename(one, one).await();
    fs::move(one, one).await();

    same(said_of(fs::read(one).await()), std::string{"treść"});

    const fs::path two = nook.where() / "dwa.txt";

    std::error_code trouble;

    stood::create_hard_link(one, two, trouble);

    if (trouble) return;

    threw(fs::rename(one, two).await());
    threw(fs::move(one, two).await());

    ought(fs::exists(one).await());
    ought(fs::exists(two).await());

    // Ta sama nazwa w innym katalogu to też osobny wpis.
    const fs::path other = nook.where() / "inny" / "jeden.txt";

    fs::make_folder(other.parent_path()).await();
    stood::create_hard_link(one, other, trouble);

    if (trouble) return;

    threw(fs::rename(one, other).await());

    ought(fs::exists(one).await());
    ought(fs::exists(other).await());
}

#ifdef _WIN32

// Na Windows `heat` i `Heat` to jeden wpis, więc cel „stoi" — a zmiana samej wielkości liter jest
// zwyczajną zmianą nazwy, dla pliku i dla katalogu, przy każdej polityce kolizji.
TRIAL("fs: na Windows zmiana samej wielkości liter to zmiana nazwy") {
    const Nook nook;

    const fs::path file = nook.made("heat.txt", "treść");
    const fs::path folder = nook.where() / "alien";

    fs::make_folder(folder).await();

    fs::rename(file, nook.where() / "Heat.txt").await();
    fs::move(folder, nook.where() / "Alien").await();

    std::vector<std::string> named;

    for (const fs::Entry& one : fs::list(nook.where()).await()) named.push_back(fs::to_string(one.where.filename()));

    std::sort(named.begin(), named.end());

    same(named, (std::vector<std::string>{"Alien", "Heat.txt"}));
}

#endif

TRIAL("fs: write z refuse nie rusza istniejącego") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "stare");

    threw(fs::write(one, bytes("nowe"), fs::Conflict::fail).await());

    same(said_of(fs::read(one).await()), std::string{"stare"});
}

TRIAL("fs: kopia zachowuje czas zapisu") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", ladder(5000));
    const fs::path to = nook.where() / "cel.bin";

    const auto dawno = std::chrono::sys_seconds{std::chrono::seconds{1000000000}};

    fs::modified(from, dawno).await();

    same(fs::modified(from).await(), dawno);

    fs::copy(from, to).await();

    same(fs::modified(to).await(), dawno);
}

TRIAL("fs: kopia drzewa zachowuje czasy zapisu wpisów") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).await();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    const auto dawno = std::chrono::sys_seconds{std::chrono::seconds{1000000000}};
    fs::modified(from / "plik.txt", dawno).await();

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).await();

    same(fs::modified(to / "plik.txt").await(), dawno);
}

// Kopiowanie na samego siebie **kasowało plik**: cel otwierał się do zapisu i ucinał do zera,
// a źródłem był ten sam plik. Teraz odmawia — a `beside` dalej wolno, bo tam cel jest inny.
TRIAL("fs: copy pliku na samego siebie odmawia zamiast go kasować") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    threw(fs::copy(one, one, fs::Conflict::overwrite).await());

    same(fs::read(one).await().size(), std::string{"treść"}.size());

    // Ta sama ścieżka pisana inaczej to dalej ten sam plik.
    const fs::path around = nook.where() / "." / "plik.txt";

    threw(fs::copy(one, around, fs::Conflict::overwrite).await());

    same(fs::read(one).await().size(), std::string{"treść"}.size());

    // `beside` robi kopię obok — i to jest jedyny sensowny sposób skopiowania pliku na siebie.
    fs::copy(one, one, fs::Conflict::rename).await();

    ought(stood::exists(nook.where() / "plik (2).txt"));
    same(fs::read(one).await().size(), std::string{"treść"}.size());
}

// Bez własnego strażnika katalog na samego siebie pada dopiero na pierwszym pliku w środku
// i mówi o pliku — a pytano o katalog. Stąd osobne zdanie.
TRIAL("fs: copy katalogu na samego siebie odmawia i mówi o katalogu") {
    const Nook nook;

    const fs::path one = nook.where() / "katalog";

    fs::make_folder(one).await();
    (void)nook.made("katalog/w środku.txt", "x");

    std::string said;

    try {
        fs::copy(one, one, fs::Conflict::overwrite).await();
    } catch (const std::exception& why) {
        said = why.what();
    }

    ought(said.find("directory onto itself") != std::string::npos);
    ought(stood::exists(one / "w środku.txt"));
}

TRIAL("fs: copy rozwija dowiązanie do pliku") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path link = nook.where() / "skrót.txt";

    if (!linked(link, one)) return;

    same(static_cast<int>(fs::kind(link).await()), static_cast<int>(fs::Kind::link));

    const fs::path to = nook.where() / "cel.txt";

    fs::copy(link, to).await();

    same(static_cast<int>(fs::kind(to).await()), static_cast<int>(fs::Kind::file));
    same(said_of(fs::read(to).await()), std::string{"treść"});
}

TRIAL("fs: copy rozwija dowiązanie do katalogu w drzewie") {
    const Nook nook;

    const fs::path osobno = nook.where() / "osobno";
    fs::make_folder(osobno).await();

    {
        std::ofstream making{osobno / "w środku.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).await();

    if (!linked(from / "skrót", osobno, true)) return;

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).await();

    same(static_cast<int>(fs::kind(to / "skrót").await()), static_cast<int>(fs::Kind::folder));
    same(said_of(fs::read(to / "skrót" / "w środku.txt").await()), std::string{"treść"});
}

TRIAL("fs: dowiązanie na przodka nie zapętla kopiowania") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from / "w głąb").await();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    if (!linked(from / "w głąb" / "wstecz", from, true)) return;

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).await();

    same(said_of(fs::read(to / "plik.txt").await()), std::string{"treść"});
}

TRIAL("fs: zerwane dowiązanie w drzewie nie przewraca kopiowania") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make_folder(from).await();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    if (!linked(from / "donikąd", nook.where() / "nie ma tego")) return;

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).await();

    same(said_of(fs::read(to / "plik.txt").await()), std::string{"treść"});
    ought(!fs::exists(to / "donikąd").await());
}

TRIAL("fs: kopia zerwanego dowiązania wprost odrzuca") {
    const Nook nook;

    const fs::path link = nook.where() / "donikąd";

    if (!linked(link, nook.where() / "nie ma tego")) return;

    threw(fs::copy(link, nook.where() / "cel").await());
}

TRIAL("fs: alike poznaje ten sam plik pod dwiema ścieżkami") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");
    const fs::path two = nook.where() / "." / "plik.txt";
    const fs::path three = nook.made("inny.txt", "treść");

    ought(fs::equivalent(one, two).await());
    ought(!fs::equivalent(one, three).await());
}

TRIAL("fs: alike na nieistniejącym odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    threw((void)fs::equivalent(one, nook.where() / "nie ma").await());
}

TRIAL("fs: real prostuje ścieżkę") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");
    const fs::path krzywa = nook.where() / "." / "puste" / ".." / "plik.txt";

    same(fs::canonical(krzywa).await(), fs::canonical(one).await());
}

TRIAL("fs: target oddaje to, na co wskazuje dowiązanie") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path skrót = nook.where() / "skrót.txt";

    if (!linked(skrót, one)) return;

    same(fs::target(skrót).await(), one);
    ought(fs::equivalent(skrót, one).await());
}

TRIAL("fs: target na zwykłym pliku odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    threw((void)fs::target(one).await());
}

TRIAL("fs: link zakłada dowiązanie miękkie") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path skrót = nook.where() / "skrót.txt";

    std::error_code próba;
    stood::create_symlink(one, nook.where() / "próba", próba);
    if (próba) return;

    fs::link(skrót, one).await();

    same(static_cast<int>(fs::kind(skrót).await()), static_cast<int>(fs::Kind::link));
    same(said_of(fs::read(skrót).await()), std::string{"treść"});
}

TRIAL("fs: link zakłada dowiązanie twarde") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path drugi = nook.where() / "drugi.txt";

    fs::link(drugi, one, true).await();

    same(static_cast<int>(fs::kind(drugi).await()), static_cast<int>(fs::Kind::file));
    ought(fs::equivalent(drugi, one).await());
}

TRIAL("fs: marks wracają tym, czym je ustawiono") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    ought(!fs::attributes(one).await().readonly);

    fs::Attributes what;
    what.readonly = true;

    fs::attributes(one, what).await();

    ought(fs::attributes(one).await().readonly);

    what.readonly = false;
    fs::attributes(one, what).await();

    ought(!fs::attributes(one).await().readonly);
}

TRIAL("fs: przejście przynosi atrybuty razem z wpisami") {
    const Nook nook;

    const fs::path one = nook.made("zwykły.txt", "treść");
    (void)nook.made(".skryty.txt", "treść");

    fs::Attributes what;
    what.readonly = true;
    fs::attributes(one, what).await();

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{2});

    bool widziany = false;

    for (const fs::Entry& wpis : among) {
        if (wpis.where.filename() == "zwykły.txt") {
            ought(wpis.attributes.readonly);
            widziany = true;
        }
    }

    ought(widziany);

    what.readonly = false;
    fs::attributes(one, what).await();
}

TRIAL("fs: temp i home wskazują istniejące katalogi") {
    same(static_cast<int>(fs::kind(fs::temp()).await()), static_cast<int>(fs::Kind::folder));

    const fs::path dom = fs::home();

    ought(!dom.empty());
    same(static_cast<int>(fs::kind(dom).await()), static_cast<int>(fs::Kind::folder));
}

TRIAL("fs: here czyta i przestawia katalog bieżący") {
    const Nook nook;

    const fs::path skąd = fs::current();

    fs::current(nook.where()).await();

    ought(fs::equivalent(fs::current(), nook.where()).await());

    fs::current(skąd).await();

    same(fs::current(), skąd);
}

TRIAL("fs: here na nieistniejącym odrzuca") {
    const Nook nook;

    threw(fs::current(nook.where() / "nie ma").await());
}

TRIAL("Flow: czyta po kolei bez prowadzenia rachunku") {
    const Nook nook;

    const std::string whole = ladder(20000);
    const fs::path one = nook.made("duży.bin", whole);

    fs::Cursor flow = fs::open(one).await().cursor();

    std::string made;

    while (true) {
        const fs::Bytes part = flow.read(4096).await();
        if (part.empty()) break;

        made += said_of(part);
    }

    same(made, whole);
    same(flow.at(), static_cast<std::uintmax_t>(whole.size()));
}

TRIAL("Flow: krótki odczyt na końcu przesuwa tylko o tyle, ile przyszło") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abcdefghij");

    fs::Cursor flow = fs::open(one).await().cursor();

    same(said_of(flow.read(6).await()), std::string{"abcdef"});
    same(flow.at(), std::uintmax_t{6});

    same(said_of(flow.read(100).await()), std::string{"ghij"});
    same(flow.at(), std::uintmax_t{10});

    ought(flow.read(100).await().empty());
    same(flow.at(), std::uintmax_t{10});
}

TRIAL("Flow: seek przestawia miejsce czytania") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one).await().cursor();

    flow.seek(4);

    same(said_of(flow.read(3).await()), std::string{"456"});
    same(flow.at(), std::uintmax_t{7});
}

TRIAL("Flow: dwa strumienie z jednego pliku sobie nie przeszkadzają") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    const fs::File held = fs::open(one).await();

    fs::Cursor first = held.cursor();
    fs::Cursor second = held.cursor();

    same(said_of(first.read(3).await()), std::string{"012"});
    same(said_of(second.read(3).await()), std::string{"012"});
    same(said_of(first.read(3).await()), std::string{"345"});

    same(first.at(), std::uintmax_t{6});
    same(second.at(), std::uintmax_t{3});
}

TRIAL("Flow: kopie dzielą pozycję") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor first = fs::open(one).await().cursor();
    fs::Cursor second = first;

    same(said_of(first.read(3).await()), std::string{"012"});
    same(second.at(), std::uintmax_t{3});
    same(said_of(second.read(3).await()), std::string{"345"});
    same(first.at(), std::uintmax_t{6});
}

TRIAL("Flow: drugie żądanie przed pierwszym odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", ladder(10000));

    fs::Cursor flow = fs::open(one).await().cursor();

    Promise<fs::Bytes> first = flow.read(1000);
    Promise<fs::Bytes> second = flow.read(1000);

    threw((void)second.await());

    same(said_of(first.await()).size(), std::size_t{1000});
}

TRIAL("Flow: porzucona obietnica zwalnia strumień") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one).await().cursor();

    { const Promise<fs::Bytes> nietknięta = flow.read(3); }

    same(said_of(flow.read(3).await()), std::string{"012"});
}

TRIAL("Flow: nieudana operacja też zwalnia strumień") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one, fs::Mode::read).await().cursor();

    threw((void)flow.write(bytes("x")).await());

    same(flow.at(), std::uintmax_t{0});
    same(said_of(flow.read(3).await()), std::string{"012"});
}

TRIAL("Flow: seek w trakcie odczytu odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", ladder(10000));

    fs::Cursor flow = fs::open(one).await().cursor();

    Promise<fs::Bytes> going = flow.read(1000);

    threw(flow.seek(0));

    (void)going.await();

    flow.seek(0);
    same(flow.at(), std::uintmax_t{0});
}

TRIAL("Flow: zapis przesuwa się sam") {
    const Nook nook;

    const fs::path one = nook.where() / "zapis.bin";

    fs::Cursor flow = fs::open(one, fs::Mode::truncate).await().cursor();

    same(flow.write(bytes("pierwsza ")).await(), std::size_t{9});
    same(flow.write(bytes("druga")).await(), std::size_t{5});

    same(flow.at(), std::uintmax_t{14});
    same(said_of(fs::read(one).await()), std::string{"pierwsza druga"});
}

TRIAL("Flow: dopisanie na koniec po przestawieniu") {
    const Nook nook;

    const fs::path one = nook.made("dane.txt", "stare");

    fs::Cursor flow = fs::open(one, fs::Mode::write).await().cursor();

    flow.seek(fs::size(one).await());

    (void)flow.write(bytes(" i nowe")).await();

    same(said_of(fs::read(one).await()), std::string{"stare i nowe"});
}

TRIAL("Flow: zamknięty odrzuca zamiast się wywracać") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow = fs::open(one).await().cursor();
    flow.close();

    ought(!flow.held());
    threw((void)flow.read(3).await());
    threw((void)flow.write(bytes("x")).await());
    threw(flow.seek(0));
    same(flow.at(), std::uintmax_t{0});
}

TRIAL("Flow: z zamkniętego pliku nie da się go wziąć") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::File held = fs::open(one).await();
    held.close();

    threw((void)held.cursor());
}

TRIAL("Flow: strumień żyje dłużej niż uchwyt, z którego powstał") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Cursor flow;

    {
        fs::File held = fs::open(one).await();
        flow = held.cursor();
    }

    same(said_of(flow.read(4).await()), std::string{"0123"});
}

namespace {

// Zdarzenia z systemu plików przychodzą własnym tempem, więc czekamy na nie obietnicą
// z ogranicznikiem, a nie uśpieniem. Zwraca pustkę, gdy nic nie przyszło na czas.
// Cierpliwość jest hojna z powodu, który wynika wprost z reguły odpytywania: przerwa to
// dziesięciokrotność samego zaglądania, więc na obciążonej maszynie potrafi urosnąć do
// sekund. Przypadki, które oczekują ciszy, podają krótki czas jawnie.
std::vector<fs::Change> awaited(fs::Watch& watch, std::chrono::milliseconds patience = 6000ms) {
    Promise<std::vector<fs::Change>> going = watch.next();
    Promise<void> limit = Time::limit(patience);

    bool late = false;

    try {
        Promise<void>::race(going, limit).await();
    } catch (...) {
        late = true;
    }

    // Po upływie czasu zgłoszenie trzeba anulować, inaczej await() poniżej czekałby bez
    // końca — obietnica dalej wisi w kolejce obserwacji.
    if (late) {
        going.cancel();

        return {};
    }

    limit.cancel();

    std::vector<fs::Change> made;

    try {
        made = going.await();
    } catch (...) {
    }

    return made;
}

bool among(const std::vector<fs::Change>& given, const fs::path& where, fs::Change::Kind stir) {
    for (const fs::Change& one : given)
        if (one.where == where && one.kind == stir) return true;

    return false;
}

}

TRIAL("Watch: nowy plik zgłasza się jako powstały") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).await();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(watch), one, Changed::created));
}

TRIAL("Watch: usunięty plik zgłasza się jako zniknięty") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::Watch watch = fs::watch(nook.where()).await();

    fs::erase(one).await();

    ought(among(awaited(watch), one, Changed::removed));
}

// Zapis istniejącego pliku idzie obok i przemianowaniem na cel, więc obserwator widzi przyjście
// pliku pod tę nazwę — jako przemianowanie albo utworzenie, zależnie od tego, czy system łączy obie
// połówki przemianowania. Plik pisany wprost zgłasza zmianę.
TRIAL("Watch: zmiana treści zgłasza się jako zmiana") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(nook.where()).await();

    fs::write(one, bytes("nowe")).await();

    const std::vector<fs::Change> given = awaited(watch);

    ought(among(given, one, Changed::modified) || among(given, one, Changed::created) ||
          among(given, one, Changed::renamed));

    fs::Watch again = fs::watch(nook.where()).await();

    fs::File file = fs::open(one, fs::Mode::write).await();

    (void)file.write(0, bytes("inne")).await();

    // NTFS odkłada czas zapisu do zamknięcia uchwytu, więc zapis tej samej długości przez otwarty
    // plik Windows zgłasza dopiero przy zamknięciu; inotify — od razu.
    file.close();

    ought(among(awaited(again), one, Changed::modified));
}

// Dwa zapisy tej samej długości w jednej sekundzie, a między nimi spojrzenie obserwacji: przy
// zaglądaniu sekunda i rozmiar się nie zmieniają, więc rozstrzyga czas dokładny. Próba czeka na
// początek nowej sekundy, żeby oba zapisy i spojrzenie na pewno w niej padły.
TRIAL("Watch: zapis tej samej długości w tej samej sekundzie zgłasza się") {
    const Nook nook;

    for (;;) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000 < 50) break;

        std::this_thread::sleep_for(5ms);
    }

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(nook.where()).await();

    {
        std::fstream file{one, std::ios::in | std::ios::out | std::ios::binary};

        file << "inne";
    }

    ought(among(awaited(watch), one, Changed::modified));
}

TRIAL("Watch: przemianowanie niesie poprzednią nazwę") {
    const Nook nook;

    const fs::path from = nook.made("stary.txt", "treść");
    const fs::path to = nook.where() / "nowy.txt";

    fs::Watch watch = fs::watch(nook.where()).await();

    fs::rename(from, to).await();

    const std::vector<fs::Change> given = awaited(watch);

    bool jedno = false;

    for (const fs::Change& one : given)
        if (one.kind == Changed::renamed && one.where == to && one.was == from) jedno = true;

    // Odpytywanie nie ma jak rozpoznać przemianowania — widzi tylko, że jednej nazwy nie ma,
    // a druga się pojawiła. To jest uczciwa granica tej drogi i przypadek ją dopuszcza.
    const bool parami = among(given, from, Changed::removed) && among(given, to, Changed::created);

    ought(jedno || parami);
}

TRIAL("Watch: zdarzenia sprzed zgłoszenia nie przepadają") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).await();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    Time::later(300ms).await();

    ought(among(awaited(watch), one, Changed::created));
}

TRIAL("Watch: płytka obserwacja nie widzi w głąb, głęboka widzi") {
    const Nook nook;

    const fs::path głąb = nook.where() / "w głąb";
    fs::make_folder(głąb).await();

    fs::Watch płytka = fs::watch(nook.where()).await();
    fs::Watch głęboka = fs::watch(nook.where(), true).await();

    const fs::path one = głąb / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(głęboka), one, Changed::created));
    ought(!among(awaited(płytka, 300ms), one, Changed::created));
}

TRIAL("Watch: głęboka obserwacja obejmuje katalogi powstałe później") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where(), true).await();

    const fs::path głąb = nook.where() / "później";
    fs::make_folder(głąb).await();

    (void)awaited(watch);

    const fs::path one = głąb / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(watch), one, Changed::created));
}

TRIAL("Watch: dwie obserwacje tego samego katalogu dostają obie") {
    const Nook nook;

    fs::Watch first = fs::watch(nook.where()).await();
    fs::Watch second = fs::watch(nook.where()).await();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(first), one, Changed::created));
    ought(among(awaited(second), one, Changed::created));
}

TRIAL("Watch: zamknięta obserwacja odrzuca") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).await();
    watch.close();

    ought(!watch.held());
    threw((void)watch.next().await());
}

TRIAL("Watch: obserwowanie pustki odrzuca") {
    const Nook nook;

    threw((void)fs::watch(nook.where() / "nie ma").await());
}

TRIAL("Watch: anulowane zgłoszenie nie zabiera paczki następnemu") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).await();

    Promise<std::vector<fs::Change>> porzucone = watch.next();
    porzucone.start();
    porzucone.cancel();

    Time::later(100ms).await();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(watch), one, Changed::created));
}

TRIAL("Watch: sto obserwacji to dalej jeden wątek") {
    const Nook nook;

    std::vector<fs::Watch> among;

    for (int at = 0; at < 100; ++at) {
        const fs::path one = nook.where() / ("katalog" + std::to_string(at));
        fs::make_folder(one).await();

        among.push_back(fs::watch(one).await());
    }

    const fs::path one = nook.where() / "katalog42" / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(::among(awaited(among[42]), one, Changed::created));
}

TRIAL("Flow: ciasna pętla odczytów nie słyszy zajętości") {
    const Nook nook;

    const std::string whole = ladder(20000);
    const fs::path one = nook.made("duży.bin", whole);

    fs::Cursor flow = fs::open(one).await().cursor();

    std::string made;

    for (int at = 0; at < 200; ++at) {
        const fs::Bytes part = flow.read(100).await();
        if (part.empty()) break;

        made += said_of(part);
    }

    same(made.size(), std::size_t{20000});
    same(made, whole);
}

TRIAL("Flow: ciasna pętla zapisów też nie") {
    const Nook nook;

    const fs::path one = nook.where() / "zapis.bin";

    fs::Cursor flow = fs::open(one, fs::Mode::truncate).await().cursor();

    for (int at = 0; at < 200; ++at) (void)flow.write(bytes("0123456789")).await();

    same(fs::size(one).await(), std::uintmax_t{2000});
    same(flow.at(), std::uintmax_t{2000});
}

TRIAL("Watch: obserwacja samego pliku, nie katalogu") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(one).await();

    fs::write(one, bytes("zupełnie inna treść")).await();

    const std::vector<fs::Change> given = awaited(watch);

    ought(among(given, one, Changed::modified) || among(given, one, Changed::created));
}

TRIAL("Watch: zniknięcie obserwowanego pliku zgłasza się") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::Watch watch = fs::watch(one).await();

    fs::erase(one).await();

    ought(among(awaited(watch), one, Changed::removed));
}

TRIAL("Watch: obserwacja przeżywa i przetrwa wiele paczek po kolei") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where()).await();

    for (int at = 0; at < 5; ++at) {
        const fs::path one = nook.where() / ("plik" + std::to_string(at) + ".txt");

        {
            std::ofstream making{one, std::ios::binary};
            making << at;
        }

        bool widziany = false;

        for (int próba = 0; próba < 4 && !widziany; ++próba)
            widziany = among(awaited(watch), one, Changed::created);

        ought(widziany);
    }
}

// Tak zapisuje większość edytorów: nowa treść obok, potem przemianowanie na miejsce starej.
// Obserwacja przywiązana do i-węzła zostałaby przy usuniętej treści i zamilkła na zawsze.
TRIAL("Watch: obserwacja pliku przeżywa zapis przez podmianę") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(one).await();

    const fs::path obok = nook.where() / "plik.txt.nowy";

    {
        std::ofstream making{obok, std::ios::binary};
        making << "zupełnie inna treść";
    }

    fs::rename(obok, one, fs::Conflict::overwrite).await();

    bool widziany = false;

    for (int próba = 0; próba < 4 && !widziany; ++próba) {
        const std::vector<fs::Change> given = awaited(watch);

        widziany = among(given, one, Changed::modified) || among(given, one, Changed::created);
    }

    ought(widziany);
    same(said_of(fs::read(one).await()), std::string{"zupełnie inna treść"});

    // Właściwa próba jest tutaj: po podmianie obserwacja musi widzieć **następną** zmianę.
    // Przywiązana do starego i-węzła zobaczyłaby tylko jego śmierć i zamilkła.
    fs::write(one, bytes("jeszcze inna"), fs::Conflict::overwrite).await();

    bool dalej = false;

    for (int próba = 0; próba < 4 && !dalej; ++próba) {
        const std::vector<fs::Change> given = awaited(watch);

        dalej = among(given, one, Changed::modified) || among(given, one, Changed::created);
    }

    ought(dalej);
}

TRIAL("Watch: obserwacja pliku milczy o sąsiadach") {
    const Nook nook;

    const fs::path one = nook.made("mój.txt", "treść");
    const fs::path sąsiad = nook.made("cudzy.txt", "treść");

    fs::Watch watch = fs::watch(one).await();

    fs::write(sąsiad, bytes("zupełnie inna treść"), fs::Conflict::overwrite).await();

    ought(awaited(watch, 700ms).empty());

    fs::write(one, bytes("moja nowa treść"), fs::Conflict::overwrite).await();

    bool widziany = false;

    for (int próba = 0; próba < 4 && !widziany; ++próba) {
        const std::vector<fs::Change> given = awaited(watch);

        widziany = among(given, one, Changed::modified) || among(given, one, Changed::created);

        for (const fs::Change& jeden : given) ought(jeden.where == one);
    }

    ought(widziany);
}

namespace {

// Czas zapisu w nanosekundach od 1970 roku, prosto od systemu — próby ułamka sekundy nie mogą pytać
// tej samej drogi, którą sprawdzają. Na Windows ziarno to sto nanosekund.
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

// Ustawia czas zapisu z ułamkiem: pełne sekundy plus 123 456 700 nanosekund (wielokrotność ziarna
// Windows). Katalog też — przez uchwyt z `FILE_FLAG_BACKUP_SEMANTICS`.
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

// Kopia przenosi czas zapisu co do ułamka sekundy — plik, katalog i katalog w środku. Dawniej szło
// to przez pełne sekundy, a serwer multimediów porównujący czasy z ułamkiem widział każdą kopię
// jako zmieniony plik.
TRIAL("fs: kopia przenosi czas zapisu z ułamkiem sekundy") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";

    fs::make_folder(from / "Sezon 01").await();

    const fs::path one = nook.made("drzewo/Sezon 01/S01E01.mkv", ladder(5000));
    const fs::path two = nook.made("pojedynczy.mkv", ladder(3000));

    aged(one, 1500000000);
    aged(two, 1450000000);
    aged(from / "Sezon 01", 1400000000);
    aged(from, 1300000000);

    fs::copy(from, nook.where() / "odbitka").await();
    fs::copy(two, nook.where() / "druga.mkv").await();

    same(instant(nook.where() / "odbitka" / "Sezon 01" / "S01E01.mkv"), instant(one));
    same(instant(nook.where() / "odbitka" / "Sezon 01"), instant(from / "Sezon 01"));
    same(instant(nook.where() / "odbitka"), instant(from));
    same(instant(nook.where() / "druga.mkv"), instant(two));

    same(instant(two) % 1000000000LL, std::int64_t{123456700});

    // Odczyt publiczny dalej oddaje pełne sekundy, ścięte w dół.
    same(fs::modified(two).await(), std::chrono::sys_seconds{std::chrono::seconds{1450000000}});
}

// Plik większy niż jeden kawałek kopii w jądrze (16 MiB) i z ogonem, który się w kawałek nie mieści:
// kopia na tym samym nośniku i — gdzie się da — na innym, gdzie jądro odmawia i kopiujemy sami.
TRIAL("fs: duży plik kopiuje się wiernie kawałkami") {
    const Nook nook;

    const std::string whole = ladder(17 * 1024 * 1024 + 3);

    const fs::path from = nook.made("duży.mkv", whole);

    fs::copy(from, nook.where() / "obok.mkv").await();

    same(said_of(fs::read(nook.where() / "obok.mkv").await()) == whole, true);

#ifndef _WIN32
    const fs::path away = elsewhere(nook, "talcite-duzy");

    if (away.empty()) return;

    fs::copy(from, away / "daleko.mkv").await();

    same(said_of(fs::read(away / "daleko.mkv").await()) == whole, true);

    // Przeniesienie między nośnikami idzie kopią — czas z ułamkiem jedzie razem z treścią.
    aged(from, 1200000000);

    const std::int64_t was = instant(from);

    fs::move(from, away / "przeniesiony.mkv").await();

    same(instant(away / "przeniesiony.mkv"), was);

    std::error_code trouble;

    stood::remove_all(away, trouble);
#endif
}

namespace {

// Wszystkie paczki aż do ciszy: pierwsza z hojną cierpliwością, potem krótko.
std::vector<fs::Change> gathered(fs::Watch& watch) {
    std::vector<fs::Change> made = awaited(watch);

    for (std::vector<fs::Change> more = awaited(watch, 700ms); !more.empty(); more = awaited(watch, 700ms))
        made.insert(made.end(), more.begin(), more.end());

    return made;
}

bool under(const fs::path& where, const fs::path& root) {
    const auto [left, right] = std::mismatch(root.begin(), root.end(), where.begin(), where.end());

    return left == root.end();
}

}

// Przemianowany podkatalog głębokiej obserwacji: plik w nim zgłasza się pod nową ścieżką i tylko
// raz. Dawniej inotify dopisywał ten sam katalog drugi raz — każde zdarzenie przychodziło podwójnie,
// raz pod nazwą, której już nie ma.
TRIAL("Watch: przemianowany podkatalog słychać pod nową nazwą, raz") {
    const Nook nook;

    const fs::path stary = nook.where() / "stary";

    fs::make_folder(stary / "w środku").await();

    fs::Watch watch = fs::watch(nook.where(), true).await();

    const fs::path nowy = nook.where() / "nowy";

    fs::rename(stary, nowy).await();

    (void)gathered(watch);

    {
        std::ofstream making{nowy / "w środku" / "świeży.txt", std::ios::binary};
        making << "treść";
    }

    const std::vector<fs::Change> given = gathered(watch);

    std::size_t seen = 0;

    for (const fs::Change& one : given) {
        ought(!under(one.where, stary));

        if (one.where == nowy / "w środku" / "świeży.txt" && one.kind == Changed::created) ++seen;
    }

    same(seen, std::size_t{1});
}

// Usunięty podkatalog to jedno zniknięcie. Dawniej trzy wpisy: od rodzica, od samego katalogu
// i „zmieniony" z pożegnania deskryptora.
TRIAL("Watch: usunięty podkatalog zgłasza się raz") {
    const Nook nook;

    const fs::path głąb = nook.where() / "głąb";

    fs::make_folder(głąb).await();

    fs::Watch watch = fs::watch(nook.where(), true).await();

    fs::erase(głąb).await();

    const std::vector<fs::Change> given = gathered(watch);

    std::size_t about = 0;

    for (const fs::Change& one : given)
        if (one.where == głąb) {
            ++about;
            ought(one.kind == Changed::removed);
        }

    same(about, std::size_t{1});
}

// Katalog wyniesiony poza obserwowane drzewo nie zgłasza dalej swoich zmian pod starą ścieżką.
TRIAL("Watch: podkatalog wyniesiony poza drzewo milknie") {
    const Nook nook;

    fs::make_folder(nook.where() / "drzewo" / "gałąź").await();
    fs::make_folder(nook.where() / "obok").await();

    fs::Watch watch = fs::watch(nook.where() / "drzewo", true).await();

    fs::rename(nook.where() / "drzewo" / "gałąź", nook.where() / "obok" / "gałąź").await();

    (void)gathered(watch);

    {
        std::ofstream making{nook.where() / "obok" / "gałąź" / "świeży.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path stale = nook.where() / "drzewo" / "gałąź";

    for (const fs::Change& one : awaited(watch, 1500ms)) ought(!under(one.where, stale));
}

#ifndef _WIN32

// Zniknięcie samego korzenia jest zmianą bez względu na drogę: powiadomienia mówią o nim same,
// a zaglądanie trzyma korzeń w spojrzeniu. Dawniej zaglądanie widziało tylko znikające dzieci,
// a pusty katalog znikał bez słowa.
TRIAL("Watch: usunięty pusty korzeń zgłasza się jako usunięty") {
    const Nook nook;

    const fs::path root = nook.where() / "pusty";

    fs::make_folder(root).await();

    fs::Watch watch = fs::watch(root).await();

    fs::erase(root).await();

    bool heard = false;

    for (int again = 0; again < 4 && !heard; ++again) heard = among(awaited(watch), root, Changed::removed);

    ought(heard);
}

#endif

#if defined(__linux__) && !defined(TALCITE_GENERIC_POSIX)

namespace {

// Czy jakakolwiek instancja inotify tego procesu trzyma deskryptor na tym katalogu — `fdinfo`
// wypisuje każdy jako „inotify wd:… ino:…" z numerem i-węzła szesnastkowo.
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

// Jądro oddaje deskryptory zamkniętej instancji inotify z opóźnieniem. Czekamy, aż wrócą — inaczej
// następny krok trafiłby na granicę wyczerpaną jeszcze przez poprzedni i poszedłby odpytywaniem.
void recovered(const fs::path& where) {
    const int probe = inotify_init1(IN_CLOEXEC);

    if (probe < 0) return;

    for (int tries = 0; tries < 250; ++tries) {
        if (inotify_add_watch(probe, where.c_str(), IN_MODIFY) >= 0) break;

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    ::close(probe);
}

}

// Granica `max_user_watches` wyczerpana: obserwacja nie odmawia ani nie głuchnie po cichu, tylko
// odpytuje. Wyczerpujemy ją sami, deskryptorami na tylu plikach, ile system pozwoli; przy granicy
// ponad 200 tysięcy próba byłaby za droga i się pomija. Trzy sytuacje: brak miejsca na korzeń,
// miejsce na korzeń, a brak na podkatalog, i brak miejsca na podkatalog powstały później.
TRIAL("Watch: bez deskryptorów inotify obserwacja odpytuje") {
    std::ifstream reading{"/proc/sys/fs/inotify/max_user_watches"};

    long most = 0;

    if (!(reading >> most) || most > 200000) return;

    const Nook spent;

    // Nowa instancja inotify, która zajmuje wszystko, co zostało — po deskryptorze na plik.
    // `last` to ostatni wzięty, do oddania jednego miejsca.
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

        bool made = false;

        for (int again = 0; again < 4 && !made; ++again) made = among(awaited(watch), one, Changed::created);

        return made;
    };

    const int hog = hogged();

    if (hog < 0) return;

    const Nook first;

    fs::Watch none = fs::watch(first.where(), true).await();

    ought(heard(none, first.where() / "świeży.txt"));

    // Jedno miejsce wolne: korzeń się mieści, podkatalog już nie.
    inotify_rm_watch(hog, last);

    const Nook second;

    fs::make_folder(second.where() / "głąb").await();

    fs::Watch half = fs::watch(second.where(), true).await();

    ought(heard(half, second.where() / "głąb" / "świeży.txt"));

    // Obserwacja nie jest dziurawa: albo całe drzewo stoi w inotify, albo nic z niego — korzeń, który
    // się zmieścił, nie zostaje, gdy całość idzie odpytywaniem. Które z dwojga, zależy od tego, czy
    // ktoś inny tego samego użytkownika nie zwolnił akurat deskryptora.
    ought(inotified(second.where() / "głąb") == inotified(second.where()));

    ::close(hog);

    recovered(second.where());

    // Obserwacja stoi na zdarzeniach; podkatalog powstaje, gdy deskryptorów już nie ma.
    const Nook third;

    fs::Watch later = fs::watch(third.where(), true).await();

    const int again = hogged();

    fs::make_folder(third.where() / "później").await();

    // Przejście na odpytywanie zgłasza przepełnienie dopiero po pierwszym spojrzeniu; plik pisany
    // wcześniej mógłby wejść już do niego i nie zgłosić się jako nowy — od tego jest przepełnienie.
    bool switched = false;

    for (int again = 0; again < 4 && !switched; ++again)
        switched = among(awaited(later), third.where(), Changed::overflow);

    ought(switched);

    // Po przejściu inotify nie trzyma już niczego z tego drzewa — inaczej zmiana w korzeniu
    // przychodziłaby dwa razy, raz od zdarzeń i raz od zaglądania.
    ought(!inotified(third.where()));
    ought(heard(later, third.where() / "później" / "świeży.txt"));

    ::close(again);

    recovered(third.where());
}

// Zamknięta obserwacja oddaje deskryptory inotify — także te z podkatalogów. Obserwacja mogła po
// drodze przejść na zaglądanie, więc zdejmuje się z obu dróg naraz.
TRIAL("Watch: zamknięta obserwacja oddaje deskryptory inotify") {
    const Nook nook;

    fs::make_folder(nook.where() / "głąb").await();

    fs::Watch watch = fs::watch(nook.where(), true).await();

    ought(inotified(nook.where()));
    ought(inotified(nook.where() / "głąb"));

    watch.close();

    // Zamknięcie puszcza uchwyt, a deskryptory zdejmuje ten, kto puści obserwację ostatni — bywa nim
    // praca z puli, która właśnie kończy przegląd. Pod obciążeniem to chwila po `close`, więc czekamy
    // do skutku, najwyżej dwie sekundy: deskryptor, który naprawdę wycieka, zostaje na zawsze.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};

    while ((inotified(nook.where()) || inotified(nook.where() / "głąb")) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds{20});

    ought(!inotified(nook.where()));
    ought(!inotified(nook.where() / "głąb"));
}

// Korzeń usunięty i postawiony na nowo: obserwacja nie głuchnie na zawsze, tylko przechodzi na
// zaglądanie i widzi, co powstało w nowym.
TRIAL("Watch: korzeń postawiony na nowo dalej słychać") {
    const Nook nook;

    const fs::path root = nook.where() / "korzeń";

    fs::make_folder(root).await();

    fs::Watch watch = fs::watch(root).await();

    fs::erase(root).await();

    const std::vector<fs::Change> given = gathered(watch);

    // Zniknięcie korzenia i przyznanie, że od tej chwili obserwacja idzie inną drogą.
    ought(among(given, root, Changed::removed));
    ought(among(given, root, Changed::overflow));

    fs::make_folder(root).await();

    const fs::path one = root / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    bool heard = false;

    for (int again = 0; again < 4 && !heard; ++again) heard = among(awaited(watch), one, Changed::created);

    ought(heard);
}

#endif

TRIAL("fs: mount rozpoznaje nośnik") {
    const Nook nook;

    const int told = static_cast<int>(fs::mount(nook.where()).await());

    ought(told == static_cast<int>(fs::Mount::disk) ||
          told == static_cast<int>(fs::Mount::remote) ||
          told == static_cast<int>(fs::Mount::other));
}

TRIAL("fs: rata bierze się z rozpoznanego nośnika") {
    const Nook nook;

    const fs::Mount told = fs::mount(nook.where()).await();
    const std::size_t rata = fs::chunk(nook.where()).await();

    if (told == fs::Mount::remote) same(rata, std::size_t{64 * 1024});
    else if (told == fs::Mount::removable || told == fs::Mount::optical)
        same(rata, std::size_t{256 * 1024});
    else same(rata, std::size_t{1024 * 1024});
}

TRIAL("fs: wykaz dysków nie jest pusty i niesie miejsce") {
    const std::vector<fs::Volume> among = fs::volumes().await();

    ought(!among.empty());

    bool któryś = false;

    for (const fs::Volume& one : among) {
        ought(!one.where.empty());

        if (one.space.total > 0) {
            ought(one.space.free <= one.space.total);
            któryś = true;
        }
    }

    ought(któryś);
}

TRIAL("fs: każdy dysk z wykazu da się obejrzeć") {
    const std::vector<fs::Volume> among = fs::volumes().await();

    for (const fs::Volume& one : among) {
        const fs::Kind told = fs::kind(one.where).await();

        ought(told == fs::Kind::folder || told == fs::Kind::none);
    }
}

// Wykaz nośników to korzenie i nośnik każdego z nich: te same miejsca, w tej samej kolejności.
TRIAL("fs: korzenie to miejsca z wykazu nośników") {
    const std::vector<fs::path> roots = fs::roots().await();
    const std::vector<fs::Volume> among = fs::volumes().await();

    ought(!roots.empty());
    same(roots.size(), among.size());

    for (std::size_t at = 0; at < roots.size() && at < among.size(); ++at) same(roots[at], among[at].where);
}

// Nośnik pliku to jeden z korzeni, i to ten, pod którym plik leży — najgłębszy z pasujących.
TRIAL("fs: mounted daje korzeń nośnika, na którym leży plik") {
    const Nook nook;

    fs::write(nook.where() / "a.txt", bytes("abc")).await();

    const fs::path root = fs::mounted(nook.where() / "a.txt").await();
    const std::vector<fs::path> roots = fs::roots().await();

    ought(std::find(roots.begin(), roots.end(), root) != roots.end());

    const fs::path whole = fs::canonical(nook.where() / "a.txt").await();

    // Korzeń jest początkiem ścieżki — po kawałkach, bo zapis wielkości liter i ukośników bywa różny.
    auto mine = whole.begin();

    for (const fs::path& piece : root) {
        if (piece.empty()) continue;

        ought(mine != whole.end());

        if (mine == whole.end()) break;

        ought(*mine == piece);

        ++mine;
    }

    // Żaden dłuższy korzeń nie jest początkiem ścieżki.
    for (const fs::path& other : roots) {
        if (std::distance(other.begin(), other.end()) <= std::distance(root.begin(), root.end())) continue;

        const std::string text = fs::to_string(other);
        const std::string path = fs::to_string(whole);

        ought(!(path.rfind(text, 0) == 0 && (path.size() == text.size() || path[text.size()] == '/')));
    }

    ought(fs::volume(root).await().space.total > 0);
}

#if !defined(_WIN32)

// Korzeń systemu plików zawsze jest w wykazie.
TRIAL("fs: korzeń systemu plików jest wśród korzeni") {
    const std::vector<fs::path> roots = fs::roots().await();

    ought(std::find(roots.begin(), roots.end(), fs::path{"/"}) != roots.end());
}

#endif

#if defined(__linux__) && !defined(TALCITE_GENERIC_POSIX)

// Punkt montowania mówi tym, co o nim wie tablica montowań: system plików i urządzenie albo
// etykietę — nie samą ścieżką, jak katalog, który punktem montowania nie jest.
TRIAL("fs: nośnik punktu montowania mówi z tablicy montowań") {
    const fs::Volume root = fs::volume(fs::path{"/"}).await();

    ought(!root.format.empty());
    ought(root.label != "/");
}

#endif

// Nośnik pod korzeniem mówi to samo, co jego wiersz w wykazie.
TRIAL("fs: nośnik korzenia jak w wykazie") {
    for (const fs::Volume& one : fs::volumes().await()) {
        const fs::Volume alone = fs::volume(one.where).await();

        same(alone.where, one.where);
        same(alone.format, one.format);
        same(alone.label, one.label);
        ought(alone.mount == one.mount);
        same(alone.source, one.source);
    }
}

// Ścieżka, która nie jest korzeniem, dostaje rodzaj nośnika, na którym leży, i własną nazwę.
TRIAL("fs: nośnik zwykłego katalogu") {
    const Nook nook;

    const fs::Volume made = fs::volume(nook.where()).await();

    ought(made.mount == fs::mount(nook.where()).await());
    ought(made.space.total > 0);
    ought(made.space.free <= made.space.total);

#ifndef _WIN32
    same(made.where, nook.where());
    same(made.label, nook.where().filename().string());
#endif
}

#ifdef _WIN32

// Litera bez ukośnika to na Windows „katalog bieżący na tym dysku" — nośnik dostaje korzeń.
TRIAL("fs: nośnik litery bez ukośnika") {
    const fs::path system = fs::roots().await().front();

    const std::wstring letter = system.wstring().substr(0, 2);

    same(fs::volume(fs::path{letter}).await().where, system);
}

#endif

TRIAL("fs: udział sieciowy niesie to, co za nim stoi") {
    const std::vector<fs::Volume> among = fs::volumes().await();

    for (const fs::Volume& one : among)
        if (one.mount != fs::Mount::remote) ought(one.source.empty());
}

#ifndef _WIN32

// Prawa, liczba dowiązań, właściciel i grupa — cztery pola, na których stanie wykaz „długi"
// z MC. Prawa są prawami **wpisu**, nie tego, na co wskazuje: dowiązanie ma swoje `lrwxrwxrwx`.
TRIAL("fs: list niesie prawa, liczbę dowiązań, właściciela i grupę") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    std::error_code ignored;

    stood::permissions(one, stood::perms::owner_read | stood::perms::owner_write |
                                stood::perms::group_read,
                       stood::perm_options::replace, ignored);

    stood::create_symlink(one, nook.where() / "skrót", ignored);

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{2});

    // Kolejność po ścieżce: `plik.txt`, `skrót`.
    same(among[0].where.filename().string(), std::string{"plik.txt"});
    same(among[0].rights, stood::perms::owner_read | stood::perms::owner_write |
                              stood::perms::group_read);
    same(among[0].links, std::uintmax_t{1});
    ought(among[0].owner.has_value());
    ought(among[0].group.has_value());
    same(*among[0].owner, static_cast<std::uint32_t>(::getuid()));
    same(*among[0].group, static_cast<std::uint32_t>(::getgid()));

    // Dowiązanie ma prawa swoje, a nie celu.
    same(among[1].where.filename().string(), std::string{"skrót"});
    same(among[1].rights, stood::perms::all);

    // Dowiązanie twarde widać po liczbie nazw — dopiero to pozwoli pokazać rolę `HARDLINK` z MC.
    fs::link(nook.where() / "druga nazwa", one, true).await();

    const std::vector<fs::Entry> potem = fs::list(nook.where()).await();

    for (const fs::Entry& each : potem)
        if (each.where.filename() == "plik.txt" || each.where.filename() == "druga nazwa")
            same(each.links, std::uintmax_t{2});
}

// Jeden wpis bez czytania katalogu: to samo, co przynosi wykaz, łącznie z liczbą nazw. Na tym
// stoi zapis „obok i przemianuj", który musi wiedzieć, czy nie zerwie twardego dowiązania.
TRIAL("fs: entry oddaje jeden wpis z prawami, liczbą nazw i celem dowiązania") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    std::error_code ignored;

    stood::permissions(one, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read,
                       stood::perm_options::replace, ignored);

    fs::Entry got = fs::entry(one).await();

    same(got.where, one);
    same(static_cast<int>(got.kind), static_cast<int>(fs::Kind::file));
    same(got.size, std::uintmax_t{7});
    same(got.rights, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read);
    same(got.links, std::uintmax_t{1});

    fs::link(nook.where() / "druga nazwa", one, true).await();

    same(fs::entry(one).await().links, std::uintmax_t{2});

    stood::create_symlink(one, nook.where() / "skrót", ignored);

    got = fs::entry(nook.where() / "skrót").await();

    same(static_cast<int>(got.kind), static_cast<int>(fs::Kind::link));
    ought(got.target.has_value());
    same(*got.target, one);

    // Brak wpisu to nie błąd, tylko `none`.
    same(static_cast<int>(fs::entry(nook.where() / "nie ma").await().kind), static_cast<int>(fs::Kind::none));
}

TRIAL("fs: rights zmienia prawa i idzie za dowiązaniem") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write).await();

    same(fs::entry(one).await().rights, stood::perms::owner_read | stood::perms::owner_write);

    std::error_code ignored;

    stood::create_symlink(one, nook.where() / "skrót", ignored);

    fs::rights(nook.where() / "skrót", stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read).await();

    same(fs::entry(one).await().rights, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read);

    threw(fs::rights(nook.where() / "nie ma", stood::perms::owner_read).await());
}

// Znacznik mówi o tym, **na co wpis wskazuje**, a prawa o samym wpisie: dowiązanie do pliku tylko
// do odczytu jest „tylko do odczytu", choć samo ma `lrwxrwxrwx`. To rozróżnienie kosztuje drugie
// pytanie do systemu na dowiązanie i dlatego stoi tu wprost.
TRIAL("fs: dowiązanie ma prawa swoje, a znaczniki celu") {
    const Nook nook;

    const fs::path one = nook.made("tylko-do-odczytu.txt", "treść");

    std::error_code ignored;

    stood::permissions(one, stood::perms::owner_read, stood::perm_options::replace, ignored);
    stood::create_symlink(one, nook.where() / "skrót", ignored);

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{2});
    same(among[0].where.filename().string(), std::string{"skrót"});

    same(among[0].rights, stood::perms::all);
    ought(among[0].attributes.readonly);
    ought(!among[0].attributes.stale);

    // Ten sam plik bez dowiązania: prawa i znacznik mówią to samo.
    ought(among[1].attributes.readonly);
    same(among[1].rights, stood::perms::owner_read);

    stood::permissions(one, stood::perms::owner_read | stood::perms::owner_write,
                       stood::perm_options::replace, ignored);
}

// Czas wpisu idzie teraz prosto z `lstat`, a nie przez zegar plikowy biblioteki standardowej —
// więc to, co ustawiliśmy, ma wrócić co do sekundy.
TRIAL("fs: list oddaje czas zapisu taki, jaki ustawiono") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    const std::chrono::sys_seconds when{std::chrono::seconds{1'700'000'000}};

    fs::modified(one, when).await();

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{1});
    same(among[0].modified, when);
}

TRIAL("fs: nazwa właściciela i grupy po numerze, nieznany numer bez nazwy") {
    const std::string mine = fs::owner(static_cast<std::uint32_t>(::getuid())).await();

    ought(!mine.empty());

    // Drugie pytanie o ten sam numer idzie z pamięci — odpowiedź ta sama.
    same(fs::owner(static_cast<std::uint32_t>(::getuid())).await(), mine);

    ought(!fs::group(static_cast<std::uint32_t>(::getgid())).await().empty());

    // Numer, którego system nie zna, oddaje pustkę zamiast rzucać: pokazujący wypisze wtedy numer.
    same(fs::owner(4294967294U).await(), std::string{});
    same(fs::group(4294967294U).await(), std::string{});
}

TRIAL("fs: list widzi bit wykonywania i dowiązanie bez celu") {
    const Nook nook;

    const fs::path program = nook.made("uruchom.sh", "#!/bin/sh\n");
    const fs::path plain = nook.made("dane.txt", "x");

    std::error_code ignored;

    stood::permissions(program, stood::perms::owner_exec, stood::perm_options::add, ignored);
    stood::create_symlink(nook.where() / "nie-ma", nook.where() / "wiszace", ignored);
    stood::create_symlink(plain, nook.where() / "zywe", ignored);

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{4});

    // Kolejność po ścieżce: dane.txt, uruchom.sh, wiszace, zywe.
    ought(!among[0].attributes.executable);
    ought(among[1].attributes.executable);
    ought(!among[1].attributes.stale);

    same(static_cast<int>(among[2].kind), static_cast<int>(fs::Kind::link));
    ought(among[2].attributes.stale);

    same(static_cast<int>(among[3].kind), static_cast<int>(fs::Kind::link));
    ought(!among[3].attributes.stale);

    // Cel niesie tylko dowiązanie — także wiszące, bo cel jest napisem w dowiązaniu, nie plikiem.
    ought(!among[0].target.has_value());
    ought(!among[1].target.has_value());
    ought(among[2].target.has_value());
    same(*among[2].target, nook.where() / "nie-ma");
    ought(among[3].target.has_value());
    same(*among[3].target, plain);

    // Czas dowiązania jest czasem samego dowiązania — wiszące też go ma, a nie rok 1970.
    const std::chrono::sys_seconds now =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    ought(among[2].modified > now - std::chrono::hours{1});
    ought(among[2].modified <= now + std::chrono::minutes{1});
    ought(among[3].modified > now - std::chrono::hours{1});
}

TRIAL("fs: potok to wpis specjalny, nie plik") {
    const Nook nook;

    const fs::path pipe = nook.where() / "rura";

    if (::mkfifo(pipe.c_str(), 0600) != 0) return;

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{1});
    same(static_cast<int>(among[0].kind), static_cast<int>(fs::Kind::special));
    same(static_cast<int>(fs::kind(pipe).await()), static_cast<int>(fs::Kind::special));
}

#endif

// **Ścieżka jako tekst jest zawsze UTF-8**, na każdym systemie. `path::string()` pod MSVC oddaje
// bieżącą stronę kodową ANSI — i to była przyczyna nazw plików rozsypujących się w panelach twa.
// Ta próba przechodzi na POSIX-ie zawsze; na Windows dziś **padłaby** na `string()`, a przechodzi
// na `to_string`, i to jest cała jej wartość: pilnuje różnicy, której jeden z dwóch systemów nie
// widzi wcale.
TRIAL("Pliki: ścieżka staje się tekstem w UTF-8 i wraca tą samą ścieżką") {
    const std::string said = "Zażółć gęślą jaźń.txt";

    const fs::path where = fs::from_string(said);

    same(fs::to_string(where), said);

    // Bajt po bajcie, a nie „mniej więcej": tekst ma być tym samym UTF-8, który podaliśmy.
    same(fs::to_string(where).size(), said.size());

    // I w złożeniu, bo tak używa tego program: katalog plus nazwa wpisana przez człowieka.
    const fs::path whole = fs::from_string("/dom/hubert") / fs::from_string(said);

    same(fs::to_string(whole.filename()), said);
}

#ifdef _WIN32

namespace {

// Lista dostępu pliku, tak jak ją widzi Windows: czy chroniona przed dziedziczeniem, ile ma
// wpisów i czy pierwszy należy do użytkownika, który uruchomił próbę.
struct Guard {
    bool shielded = false;
    DWORD entries = 0;
    bool mine = false;
};

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

TRIAL("fs: na Windows prawa tylko właściciela to chroniona lista z jednym wpisem") {
    const Nook nook;

    const fs::path one = nook.made("klucze.json", "tajne");

    ought(!guard(one).shielded);

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write).await();

    const Guard narrowed = guard(one);

    ought(narrowed.shielded);
    same(narrowed.entries, DWORD{1});
    ought(narrowed.mine);

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write | stood::perms::group_read).await();

    ought(!guard(one).shielded);
}

TRIAL("fs: na Windows samo prawo innych też zdejmuje ochronę") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write).await();

    ought(guard(one).shielded);

    fs::rights(one, stood::perms::owner_read | stood::perms::owner_write | stood::perms::others_read).await();

    ought(!guard(one).shielded);
}

#endif

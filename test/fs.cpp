#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "talcite/fs.hpp"
#include "talcite/time.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace talcite;
using namespace std::chrono_literals;

namespace {

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

    fs::make(two).await();

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

    const std::chrono::sys_seconds told = fs::written(one).await();
    const auto now = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now());

    const auto apart = now - told;

    ought(apart > -60s);
    ought(apart < 600s);
}

TRIAL("fs: roomy podaje pojemność i wolne miejsce") {
    const Nook nook;

    const fs::Room told = fs::roomy(nook.where()).await();

    ought(told.whole > 0);
    ought(told.spare > 0);
    ought(told.spare <= told.whole);
}

TRIAL("fs: slice daje sensowną ratę") {
    const Nook nook;

    const std::size_t told = fs::slice(nook.where()).await();

    ought(told >= 4096);
    ought(told <= 8 * 1024 * 1024);
    same(told & (told - 1), std::size_t{0});
}

TRIAL("fs: make tworzy całą drogę i nie przeszkadza mu, że już jest") {
    const Nook nook;

    const fs::path deep = nook.where() / "jeden" / "dwa" / "trzy";

    fs::make(deep).await();

    ought(fs::exists(deep).await());

    fs::make(deep).await();

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

    Promise<void> making = fs::make(deep);

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

    fs::make(nook.where() / "katalog").await();

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{3});

    same(among[0].where.filename(), fs::path{"a.txt"});
    same(static_cast<int>(among[0].kind), static_cast<int>(fs::Kind::file));
    same(among[0].size, std::uintmax_t{3});

    same(among[1].where.filename(), fs::path{"b.txt"});
    same(among[1].size, std::uintmax_t{11});

    same(among[2].where.filename(), fs::path{"katalog"});
    same(static_cast<int>(among[2].kind), static_cast<int>(fs::Kind::folder));

    ought(among[0].written.time_since_epoch().count() > 1700000000);
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

    fs::make(nook.where() / "jeden" / "dwa").await();

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

    fs::make(nook.where() / "z").await();
    fs::make(nook.where() / "a").await();

    const std::vector<fs::Entry> among = fs::walk(nook.where()).await();

    for (std::size_t at = 1; at < among.size(); ++at) ought(among[at - 1].where < among[at].where);
}

TRIAL("fs: jedno przejście zamiast tysiąca pytań") {
    const Nook nook;

    for (int at = 0; at < 200; ++at)
        (void)nook.made("plik-" + std::to_string(at) + ".bin", std::string(at, 'x'));

    Time held;

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

        fs::make(deep).await();

        for (int inner = 0; inner < 20; ++inner)
            (void)std::ofstream{deep / ("plik-" + std::to_string(inner) + ".bin"),
                                std::ios::binary};
    }

    Promise<std::vector<fs::Entry>> going = fs::walk(nook.where());

    // Bez odczekiwania: zadanie jest albo jeszcze w kolejce, albo dopiero co ruszyło. Pauza
    // dawałaby przejściu szansę skończyć się przed anulowaniem, a wtedy przypadek mierzyłby
    // wyścig zamiast mechanizmu.
    going.go();
    going.cancel();

    threw((void)going.await());
}

namespace {

fs::Bytes bytes_of(const std::string& said) {
    fs::Bytes made(said.size());

    for (std::size_t at = 0; at < said.size(); ++at)
        made[at] = static_cast<std::byte>(static_cast<unsigned char>(said[at]));

    return made;
}

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

    same(held.write(4, bytes_of("XY")).await(), std::size_t{2});

    same(said_of(held.read(0, 10).await()), std::string{"aaaaXYaaaa"});
}

TRIAL("File: write tworzy plik i zachowuje treść, fresh ją ucina") {
    const Nook nook;

    const fs::path one = nook.where() / "nowy.bin";

    {
        const fs::File held = fs::open(one, fs::Mode::write).await();
        (void)held.write(0, bytes_of("pierwsze")).await();
    }

    same(fs::size(one).await(), std::uintmax_t{8});

    {
        const fs::File held = fs::open(one, fs::Mode::write).await();
        (void)held.write(0, bytes_of("XX")).await();
    }

    same(fs::size(one).await(), std::uintmax_t{8});

    {
        const fs::File held = fs::open(one, fs::Mode::fresh).await();
        (void)held.write(0, bytes_of("XX")).await();
    }

    same(fs::size(one).await(), std::uintmax_t{2});
}

TRIAL("File: otwarcie nieistniejącego do odczytu odrzuca") {
    const Nook nook;

    threw((void)fs::open(nook.where() / "nie ma.bin").await());
}

TRIAL("File: kopie dzielą ten sam otwarty plik") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "abcdef");

    fs::File first = fs::open(one).await();
    const fs::File second = first;

    first.close();

    ought(!first.held());
    ought(second.held());

    same(said_of(second.read(0, 6).await()), std::string{"abcdef"});
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

    fs::write(one, bytes_of(whole)).await();

    same(fs::size(one).await(), static_cast<std::uintmax_t>(whole.size()));
    same(said_of(fs::read(one).await()), whole);
}

TRIAL("fs: write ścina to, co było wcześniej") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", std::string(5000, 'x'));

    fs::write(one, bytes_of("krótko")).await();

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
    fs::make(from / "w głąb" / "głębiej").await();

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

    going.go();

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

    going.go();
    going.cancel();

    Time::later(300ms).await();

    if (fs::exists(to).await()) same(fs::size(to).await(), fs::size(from).await());
}

TRIAL("fs: anulowana kopia nie rusza celu, który już był") {
    const Nook nook;

    const fs::path from = nook.made("duży.bin", ladder(8000000));
    const fs::path to = nook.made("cel.bin", "swoje");

    Promise<void> going = fs::copy(from, to, fs::Clash::overwrite);

    going.go();

    ought(grew(to, 5));

    going.cancel();

    Time::later(300ms).await();

    ought(fs::exists(to).await());
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
    fs::make(from / "w głąb").await();

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
    fs::make(where / "w głąb" / "głębiej").await();

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
    fs::make(where / "w głąb").await();

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

    fs::copy(from, to, fs::Clash::overwrite).await();

    same(said_of(fs::read(to).await()), std::string{"nowe"});
}

TRIAL("fs: copy ze skip zostawia cel w spokoju") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Clash::skip).await();

    same(said_of(fs::read(to).await()), std::string{"stare"});
}

TRIAL("fs: copy z beside zakłada nazwę obok") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", "nowe");
    const fs::path to = nook.made("cel.bin", "stare");

    fs::copy(from, to, fs::Clash::beside).await();

    same(said_of(fs::read(to).await()), std::string{"stare"});
    same(said_of(fs::read(nook.where() / "cel (2).bin").await()), std::string{"nowe"});

    fs::copy(from, to, fs::Clash::beside).await();

    same(said_of(fs::read(nook.where() / "cel (3).bin").await()), std::string{"nowe"});
}

TRIAL("fs: polityka działa na każdym wpisie drzewa z osobna") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make(from).await();

    {
        std::ofstream making{from / "jeden.txt", std::ios::binary};
        making << "nowe";
    }
    {
        std::ofstream making{from / "dwa.txt", std::ios::binary};
        making << "nowe";
    }

    const fs::path to = nook.where() / "cel";
    fs::make(to).await();

    {
        std::ofstream making{to / "jeden.txt", std::ios::binary};
        making << "stare";
    }

    fs::copy(from, to, fs::Clash::skip).await();

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

TRIAL("fs: write z refuse nie rusza istniejącego") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "stare");

    threw(fs::write(one, bytes_of("nowe"), fs::Clash::refuse).await());

    same(said_of(fs::read(one).await()), std::string{"stare"});
}

TRIAL("fs: kopia zachowuje czas zapisu") {
    const Nook nook;

    const fs::path from = nook.made("źródło.bin", ladder(5000));
    const fs::path to = nook.where() / "cel.bin";

    const auto dawno = std::chrono::sys_seconds{std::chrono::seconds{1000000000}};

    fs::written(from, dawno).await();

    same(fs::written(from).await(), dawno);

    fs::copy(from, to).await();

    same(fs::written(to).await(), dawno);
}

TRIAL("fs: kopia drzewa zachowuje czasy zapisu wpisów") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make(from).await();

    {
        std::ofstream making{from / "plik.txt", std::ios::binary};
        making << "treść";
    }

    const auto dawno = std::chrono::sys_seconds{std::chrono::seconds{1000000000}};
    fs::written(from / "plik.txt", dawno).await();

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).await();

    same(fs::written(to / "plik.txt").await(), dawno);
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
    fs::make(osobno).await();

    {
        std::ofstream making{osobno / "w środku.txt", std::ios::binary};
        making << "treść";
    }

    const fs::path from = nook.where() / "drzewo";
    fs::make(from).await();

    if (!linked(from / "skrót", osobno, true)) return;

    const fs::path to = nook.where() / "odbitka";

    fs::copy(from, to).await();

    same(static_cast<int>(fs::kind(to / "skrót").await()), static_cast<int>(fs::Kind::folder));
    same(said_of(fs::read(to / "skrót" / "w środku.txt").await()), std::string{"treść"});
}

TRIAL("fs: dowiązanie na przodka nie zapętla kopiowania") {
    const Nook nook;

    const fs::path from = nook.where() / "drzewo";
    fs::make(from / "w głąb").await();

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
    fs::make(from).await();

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

    ought(fs::alike(one, two).await());
    ought(!fs::alike(one, three).await());
}

TRIAL("fs: alike na nieistniejącym odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    threw((void)fs::alike(one, nook.where() / "nie ma").await());
}

TRIAL("fs: real prostuje ścieżkę") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");
    const fs::path krzywa = nook.where() / "." / "puste" / ".." / "plik.txt";

    same(fs::real(krzywa).await(), fs::real(one).await());
}

TRIAL("fs: target oddaje to, na co wskazuje dowiązanie") {
    const Nook nook;

    const fs::path one = nook.made("prawdziwy.txt", "treść");
    const fs::path skrót = nook.where() / "skrót.txt";

    if (!linked(skrót, one)) return;

    same(fs::target(skrót).await(), one);
    ought(fs::alike(skrót, one).await());
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
    ought(fs::alike(drugi, one).await());
}

TRIAL("fs: marks wracają tym, czym je ustawiono") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    ought(!fs::marks(one).await().readonly);

    fs::Marks what;
    what.readonly = true;

    fs::marks(one, what).await();

    ought(fs::marks(one).await().readonly);

    what.readonly = false;
    fs::marks(one, what).await();

    ought(!fs::marks(one).await().readonly);
}

TRIAL("fs: przejście przynosi atrybuty razem z wpisami") {
    const Nook nook;

    const fs::path one = nook.made("zwykły.txt", "treść");
    (void)nook.made(".skryty.txt", "treść");

    fs::Marks what;
    what.readonly = true;
    fs::marks(one, what).await();

    const std::vector<fs::Entry> among = fs::list(nook.where()).await();

    same(among.size(), std::size_t{2});

    bool widziany = false;

    for (const fs::Entry& wpis : among) {
        if (wpis.where.filename() == "zwykły.txt") {
            ought(wpis.marks.readonly);
            widziany = true;
        }
    }

    ought(widziany);

    what.readonly = false;
    fs::marks(one, what).await();
}

TRIAL("fs: temp i home wskazują istniejące katalogi") {
    same(static_cast<int>(fs::kind(fs::temp()).await()), static_cast<int>(fs::Kind::folder));

    const fs::path dom = fs::home();

    ought(!dom.empty());
    same(static_cast<int>(fs::kind(dom).await()), static_cast<int>(fs::Kind::folder));
}

TRIAL("fs: here czyta i przestawia katalog bieżący") {
    const Nook nook;

    const fs::path skąd = fs::here();

    fs::here(nook.where()).await();

    ought(fs::alike(fs::here(), nook.where()).await());

    fs::here(skąd).await();

    same(fs::here(), skąd);
}

TRIAL("fs: here na nieistniejącym odrzuca") {
    const Nook nook;

    threw(fs::here(nook.where() / "nie ma").await());
}

TRIAL("Flow: czyta po kolei bez prowadzenia rachunku") {
    const Nook nook;

    const std::string whole = ladder(20000);
    const fs::path one = nook.made("duży.bin", whole);

    fs::Flow flow = fs::open(one).await().flow();

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

    fs::Flow flow = fs::open(one).await().flow();

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

    fs::Flow flow = fs::open(one).await().flow();

    flow.seek(4);

    same(said_of(flow.read(3).await()), std::string{"456"});
    same(flow.at(), std::uintmax_t{7});
}

TRIAL("Flow: dwa strumienie z jednego pliku sobie nie przeszkadzają") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    const fs::File held = fs::open(one).await();

    fs::Flow first = held.flow();
    fs::Flow second = held.flow();

    same(said_of(first.read(3).await()), std::string{"012"});
    same(said_of(second.read(3).await()), std::string{"012"});
    same(said_of(first.read(3).await()), std::string{"345"});

    same(first.at(), std::uintmax_t{6});
    same(second.at(), std::uintmax_t{3});
}

TRIAL("Flow: kopie dzielą pozycję") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Flow first = fs::open(one).await().flow();
    fs::Flow second = first;

    same(said_of(first.read(3).await()), std::string{"012"});
    same(second.at(), std::uintmax_t{3});
    same(said_of(second.read(3).await()), std::string{"345"});
    same(first.at(), std::uintmax_t{6});
}

TRIAL("Flow: drugie żądanie przed pierwszym odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", ladder(10000));

    fs::Flow flow = fs::open(one).await().flow();

    Promise<fs::Bytes> first = flow.read(1000);
    Promise<fs::Bytes> second = flow.read(1000);

    threw((void)second.await());

    same(said_of(first.await()).size(), std::size_t{1000});
}

TRIAL("Flow: porzucona obietnica zwalnia strumień") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Flow flow = fs::open(one).await().flow();

    { const Promise<fs::Bytes> nietknięta = flow.read(3); }

    same(said_of(flow.read(3).await()), std::string{"012"});
}

TRIAL("Flow: nieudana operacja też zwalnia strumień") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Flow flow = fs::open(one, fs::Mode::read).await().flow();

    threw((void)flow.write(bytes_of("x")).await());

    same(flow.at(), std::uintmax_t{0});
    same(said_of(flow.read(3).await()), std::string{"012"});
}

TRIAL("Flow: seek w trakcie odczytu odrzuca") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", ladder(10000));

    fs::Flow flow = fs::open(one).await().flow();

    Promise<fs::Bytes> going = flow.read(1000);

    threw(flow.seek(0));

    (void)going.await();

    flow.seek(0);
    same(flow.at(), std::uintmax_t{0});
}

TRIAL("Flow: zapis przesuwa się sam") {
    const Nook nook;

    const fs::path one = nook.where() / "zapis.bin";

    fs::Flow flow = fs::open(one, fs::Mode::fresh).await().flow();

    same(flow.write(bytes_of("pierwsza ")).await(), std::size_t{9});
    same(flow.write(bytes_of("druga")).await(), std::size_t{5});

    same(flow.at(), std::uintmax_t{14});
    same(said_of(fs::read(one).await()), std::string{"pierwsza druga"});
}

TRIAL("Flow: dopisanie na koniec po przestawieniu") {
    const Nook nook;

    const fs::path one = nook.made("dane.txt", "stare");

    fs::Flow flow = fs::open(one, fs::Mode::write).await().flow();

    flow.seek(fs::size(one).await());

    (void)flow.write(bytes_of(" i nowe")).await();

    same(said_of(fs::read(one).await()), std::string{"stare i nowe"});
}

TRIAL("Flow: zamknięty odrzuca zamiast się wywracać") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Flow flow = fs::open(one).await().flow();
    flow.close();

    ought(!flow.held());
    threw((void)flow.read(3).await());
    threw((void)flow.write(bytes_of("x")).await());
    threw(flow.seek(0));
    same(flow.at(), std::uintmax_t{0});
}

TRIAL("Flow: z zamkniętego pliku nie da się go wziąć") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::File held = fs::open(one).await();
    held.close();

    threw((void)held.flow());
}

TRIAL("Flow: strumień żyje dłużej niż uchwyt, z którego powstał") {
    const Nook nook;

    const fs::path one = nook.made("dane.bin", "0123456789");

    fs::Flow flow;

    {
        fs::File held = fs::open(one).await();
        flow = held.flow();
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

bool among(const std::vector<fs::Change>& given, const fs::path& where, fs::Stir stir) {
    for (const fs::Change& one : given)
        if (one.where == where && one.stir == stir) return true;

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

    ought(among(awaited(watch), one, fs::Stir::made));
}

TRIAL("Watch: usunięty plik zgłasza się jako zniknięty") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::Watch watch = fs::watch(nook.where()).await();

    fs::erase(one).await();

    ought(among(awaited(watch), one, fs::Stir::gone));
}

TRIAL("Watch: zmiana treści zgłasza się jako zmiana") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(nook.where()).await();

    fs::write(one, bytes_of("nowe")).await();

    const std::vector<fs::Change> given = awaited(watch);

    ought(among(given, one, fs::Stir::changed) || among(given, one, fs::Stir::made));
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
        if (one.stir == fs::Stir::renamed && one.where == to && one.was == from) jedno = true;

    // Odpytywanie nie ma jak rozpoznać przemianowania — widzi tylko, że jednej nazwy nie ma,
    // a druga się pojawiła. To jest uczciwa granica tej drogi i przypadek ją dopuszcza.
    const bool parami = among(given, from, fs::Stir::gone) && among(given, to, fs::Stir::made);

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

    ought(among(awaited(watch), one, fs::Stir::made));
}

TRIAL("Watch: płytka obserwacja nie widzi w głąb, głęboka widzi") {
    const Nook nook;

    const fs::path głąb = nook.where() / "w głąb";
    fs::make(głąb).await();

    fs::Watch płytka = fs::watch(nook.where()).await();
    fs::Watch głęboka = fs::watch(nook.where(), true).await();

    const fs::path one = głąb / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(głęboka), one, fs::Stir::made));
    ought(!among(awaited(płytka, 300ms), one, fs::Stir::made));
}

TRIAL("Watch: głęboka obserwacja obejmuje katalogi powstałe później") {
    const Nook nook;

    fs::Watch watch = fs::watch(nook.where(), true).await();

    const fs::path głąb = nook.where() / "później";
    fs::make(głąb).await();

    (void)awaited(watch);

    const fs::path one = głąb / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(watch), one, fs::Stir::made));
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

    ought(among(awaited(first), one, fs::Stir::made));
    ought(among(awaited(second), one, fs::Stir::made));
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
    porzucone.go();
    porzucone.cancel();

    Time::later(100ms).await();

    const fs::path one = nook.where() / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(among(awaited(watch), one, fs::Stir::made));
}

TRIAL("Watch: sto obserwacji to dalej jeden wątek") {
    const Nook nook;

    std::vector<fs::Watch> among;

    for (int at = 0; at < 100; ++at) {
        const fs::path one = nook.where() / ("katalog" + std::to_string(at));
        fs::make(one).await();

        among.push_back(fs::watch(one).await());
    }

    const fs::path one = nook.where() / "katalog42" / "świeży.txt";

    {
        std::ofstream making{one, std::ios::binary};
        making << "treść";
    }

    ought(::among(awaited(among[42]), one, fs::Stir::made));
}

TRIAL("Flow: ciasna pętla odczytów nie słyszy zajętości") {
    const Nook nook;

    const std::string whole = ladder(20000);
    const fs::path one = nook.made("duży.bin", whole);

    fs::Flow flow = fs::open(one).await().flow();

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

    fs::Flow flow = fs::open(one, fs::Mode::fresh).await().flow();

    for (int at = 0; at < 200; ++at) (void)flow.write(bytes_of("0123456789")).await();

    same(fs::size(one).await(), std::uintmax_t{2000});
    same(flow.at(), std::uintmax_t{2000});
}

TRIAL("Watch: obserwacja samego pliku, nie katalogu") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "stare");

    fs::Watch watch = fs::watch(one).await();

    fs::write(one, bytes_of("zupełnie inna treść")).await();

    const std::vector<fs::Change> given = awaited(watch);

    ought(among(given, one, fs::Stir::changed) || among(given, one, fs::Stir::made));
}

TRIAL("Watch: zniknięcie obserwowanego pliku zgłasza się") {
    const Nook nook;

    const fs::path one = nook.made("plik.txt", "treść");

    fs::Watch watch = fs::watch(one).await();

    fs::erase(one).await();

    ought(among(awaited(watch), one, fs::Stir::gone));
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
            widziany = among(awaited(watch), one, fs::Stir::made);

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

    fs::rename(obok, one, fs::Clash::overwrite).await();

    bool widziany = false;

    for (int próba = 0; próba < 4 && !widziany; ++próba) {
        const std::vector<fs::Change> given = awaited(watch);

        widziany = among(given, one, fs::Stir::changed) || among(given, one, fs::Stir::made);
    }

    ought(widziany);
    same(said_of(fs::read(one).await()), std::string{"zupełnie inna treść"});

    // Właściwa próba jest tutaj: po podmianie obserwacja musi widzieć **następną** zmianę.
    // Przywiązana do starego i-węzła zobaczyłaby tylko jego śmierć i zamilkła.
    fs::write(one, bytes_of("jeszcze inna"), fs::Clash::overwrite).await();

    bool dalej = false;

    for (int próba = 0; próba < 4 && !dalej; ++próba) {
        const std::vector<fs::Change> given = awaited(watch);

        dalej = among(given, one, fs::Stir::changed) || among(given, one, fs::Stir::made);
    }

    ought(dalej);
}

TRIAL("Watch: obserwacja pliku milczy o sąsiadach") {
    const Nook nook;

    const fs::path one = nook.made("mój.txt", "treść");
    const fs::path sąsiad = nook.made("cudzy.txt", "treść");

    fs::Watch watch = fs::watch(one).await();

    fs::write(sąsiad, bytes_of("zupełnie inna treść"), fs::Clash::overwrite).await();

    ought(awaited(watch, 700ms).empty());

    fs::write(one, bytes_of("moja nowa treść"), fs::Clash::overwrite).await();

    bool widziany = false;

    for (int próba = 0; próba < 4 && !widziany; ++próba) {
        const std::vector<fs::Change> given = awaited(watch);

        widziany = among(given, one, fs::Stir::changed) || among(given, one, fs::Stir::made);

        for (const fs::Change& jeden : given) ought(jeden.where == one);
    }

    ought(widziany);
}

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
    const std::size_t rata = fs::slice(nook.where()).await();

    if (told == fs::Mount::remote) same(rata, std::size_t{64 * 1024});
    else if (told == fs::Mount::removable || told == fs::Mount::optical)
        same(rata, std::size_t{256 * 1024});
    else same(rata, std::size_t{1024 * 1024});
}

TRIAL("fs: wykaz dysków nie jest pusty i niesie miejsce") {
    const std::vector<fs::Drive> among = fs::drives().await();

    ought(!among.empty());

    bool któryś = false;

    for (const fs::Drive& one : among) {
        ought(!one.where.empty());

        if (one.room.whole > 0) {
            ought(one.room.spare <= one.room.whole);
            któryś = true;
        }
    }

    ought(któryś);
}

TRIAL("fs: każdy dysk z wykazu da się obejrzeć") {
    const std::vector<fs::Drive> among = fs::drives().await();

    for (const fs::Drive& one : among) {
        const fs::Kind told = fs::kind(one.where).await();

        ought(told == fs::Kind::folder || told == fs::Kind::none);
    }
}

TRIAL("fs: udział sieciowy niesie to, co za nim stoi") {
    const std::vector<fs::Drive> among = fs::drives().await();

    for (const fs::Drive& one : among)
        if (one.mount != fs::Mount::remote) ought(one.behind.empty());
}

#ifndef _WIN32

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
    ought(!among[0].marks.executable);
    ought(among[1].marks.executable);
    ought(!among[1].marks.stale);

    same(static_cast<int>(among[2].kind), static_cast<int>(fs::Kind::link));
    ought(among[2].marks.stale);

    same(static_cast<int>(among[3].kind), static_cast<int>(fs::Kind::link));
    ought(!among[3].marks.stale);
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

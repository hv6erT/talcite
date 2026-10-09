#include "proving.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/fs.hpp"
#include "talcite/listener.hpp"
#include "talcite/udp.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace talcite;

namespace {

// Wszystkie próby uruchamiają **binarkę testową**, nie cudzy program: `--pretending` każe jej udawać
// to, czego akurat potrzeba. Dzięki temu nie zależymy od tego, czy na maszynie stoi powłoka, `echo`
// czy `cmd`, i to samo działa pod Windowsem i pod Linuksem.
Command pretending(std::vector<std::string> what) {
    Command made;

    made.program = fs::path{proving::mine()};
    made.arguments.emplace_back("--pretending");

    for (std::string& one : what) made.arguments.push_back(std::move(one));

    return made;
}



}

// --- Uruchomienie i koniec -----------------------------------------------------------------------

TRIAL("Child: puste dziecko o niczym nie wie") {
    const Child one;

    ought(!one.held());
    same(one.pid(), std::int64_t{-1});
}

TRIAL("Child: uruchomiony program oddaje wyjście i kod zero") {
    const Output made = Child::run(pretending({"said", "dzień dobry"})).await();

    same(text(made.out), std::string{"dzień dobry"});
    ought(made.err.empty());
    ought(made.exit.ended == Ended::normal);
    same(made.exit.code, 0);
}

TRIAL("Child: kod wyjścia wraca taki, jaki program podał") {
    const Output made = Child::run(pretending({"code", "7"})).await();

    ought(made.exit.ended == Ended::normal);
    same(made.exit.code, 7);
}

// Dwa strumienie są **osobne**, dopóki nikt nie poprosi o zlanie ich w jeden.
TRIAL("Child: wyjście i błędy idą osobno") {
    const Output made = Child::run(pretending({"both"})).await();

    same(text(made.out), std::string{"wyjście"});
    same(text(made.err), std::string{"błąd"});
}

TRIAL("Child: merge zlewa błędy do wyjścia") {
    Command what = pretending({"both"});
    what.merge = true;

    const Output made = Child::run(std::move(what)).await();

    ought(made.err.empty());
    same(made.out.size(), std::string{"wyjście"}.size() + std::string{"błąd"}.size());
}

// Systemy mówią o tym **różnymi drogami** i obie są prawidłowe. Na POSIX rozwidlenie się udaje,
// a `exec` już nie — dziecko wychodzi wtedy z kodem 127, bo o niepowodzeniu nie ma komu powiedzieć
// wcześniej. Na Windowsie `CreateProcessW` odmawia od razu i odmowa wraca odrzuceniem obietnicy.
// Próba pilnuje jedynej rzeczy wspólnej: że żadna z tych dróg nie oddaje cichego zera.
TRIAL("Child: nieistniejący program nie udaje, że poszło") {
    Command what;
    what.program = fs::path{"nie-ma-takiego-programu-w-talcite"};

    try {
        const Output made = Child::run(std::move(what)).await();

        ought(made.exit.code != 0);
    } catch (const std::exception&) {
    }
}

TRIAL("Child: brak programu jest odmową, nie ciszą") {
    threw((void)Child::run(Command{}).await());
}

// --- Wejście -------------------------------------------------------------------------------------

TRIAL("Child: podane wejście wraca wyjściem") {
    const Output made = Child::run(pretending({"echo"}), bytes("tam i z powrotem")).await();

    same(text(made.out), std::string{"tam i z powrotem"});
    same(made.exit.code, 0);
}

// Drugi program nie dostaje w spadku rur pierwszego. Dawniej nasze końce rur nie miały znacznika
// zamknięcia przy uruchomieniu, więc program uruchomiony później trzymał wejście pierwszego
// otwarte — i pierwszy nie widział końca wejścia, dopóki drugi żył.
TRIAL("Child: drugi program nie trzyma wejścia pierwszego") {
    const Child first = Child::start(pretending({"echo"})).await();
    const Child second = Child::start(pretending({"sleeping", "8000"})).await();

    const auto began = std::chrono::steady_clock::now();

    first.close_input();

    while (!first.read(4096).await().empty()) {
    }

    same(first.wait().await().code, 0);

    ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{4});

    second.kill();
    (void)second.wait().await();
}

#ifndef _WIN32

// Proces bez wejścia standardowego (zamknięty deskryptor zero — demon, usługa) dostaje od `pipe2`
// właśnie zero na koniec rury dziecka. `dup2` na samego siebie nic nie robi i zostawia znacznik
// zamknięcia przy uruchomieniu, więc dziecko dostawało zamknięte wejście i nie czytało nic.
TRIAL("Child: rura na deskryptorze zero dochodzi do programu") {
    const int saved = ::dup(STDIN_FILENO);

    ::close(STDIN_FILENO);

    std::string got;

    try {
        const Child one = Child::start(pretending({"echo"})).await();

        (void)one.write(bytes("tam i z powrotem")).await();
        one.close_input();

        for (Bytes part = one.read(4096).await(); !part.empty(); part = one.read(4096).await()) got += text(part);

        (void)one.wait().await();
    } catch (...) {
    }

    ::dup2(saved, STDIN_FILENO);
    ::close(saved);

    same(got, std::string{"tam i z powrotem"});
}

#endif

#if defined(__linux__)

// Gniazdo otwarte przez nas nie trafia do programu potomnego — inaczej trzymałby zajęty port długo
// po tym, jak my go zamkniemy. Sprawdzamy wprost: deskryptory dziecka w /proc.
TRIAL("Child: nasze gniazda nie trafiają do programu") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Udp post{net::Address::loopback(net::Family::four, 0)};

    const Child one = Child::start(pretending({"sleeping", "3000"})).await();

    // **Liczymy po wykonaniu, nie przed nim.** Między rozwidleniem a `exec` dziecko trzyma jeszcze
    // wszystkie nasze deskryptory — zamyka je dopiero `exec` (CLOEXEC) — a start może oddać dziecko
    // wcześniej. Pod obciążeniem próba trafiała w tę szparę i widziała gniazda, których po `exec`
    // już nie ma. Gniazdo, które naprawdę przeszło, zostaje na zawsze, więc czekanie go nie ukryje.
    std::size_t sockets = 0;

    for (int round = 0; round < 200; ++round) {
        sockets = 0;

        std::error_code ignored;

        for (const std::filesystem::directory_entry& fd :
             std::filesystem::directory_iterator{"/proc/" + std::to_string(one.pid()) + "/fd", ignored})
            if (std::filesystem::read_symlink(fd.path(), ignored).string().rfind("socket:", 0) == 0) ++sockets;

        if (sockets == 0) break;

        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    one.kill();
    (void)one.wait().await();

    same(sockets, std::size_t{0});
}

#endif

// Zamknięcie wejścia jest **poleceniem**: bez niego `echo` czekałoby w nieskończoność.
TRIAL("Child: bez zamknięcia wejścia program czeka, z zamknięciem kończy") {
    const Child one = Child::start(pretending({"echo"})).await();

    (void)one.write(bytes("raz")).await();
    one.close_input();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).await();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    same(text(whole), std::string{"raz"});
    same(one.wait().await().code, 0);
}

// Rura ma dno. Program, który pisze na obie strony więcej, niż one mieszczą, zakleszcza czytającego,
// który patrzy tylko w jedną — i to jest właśnie powód, dla którego `taken` istnieje.
TRIAL("Child: taken nie zakleszcza się na obfitym wyjściu") {
    const Output made = Child::run(pretending({"much"})).await();

    same(made.out.size(), std::size_t{300 * 1000});
    same(made.err.size(), std::size_t{300 * 1000});
    same(made.exit.code, 0);
}

// --- Przepis -------------------------------------------------------------------------------------

TRIAL("Child: argumenty idą osobno, także ze spacjami") {
    const Output made = Child::run(pretending({"args", "jeden dwa", "trzy"})).await();

    same(text(made.out), std::string{"jeden dwa\ntrzy\n"});
}

TRIAL("Child: katalog roboczy jest tym, który podano") {
    Command what = pretending({"where"});
    what.where = fs::temp();

    const Output made = Child::run(std::move(what)).await();

    // **Pytamy system, a nie porównujemy napisów.** Ta sama ścieżka bywa zapisana inaczej —
    // Windows oddaje katalog tymczasowy z ukośnikiem na końcu, a katalog bieżący bez niego — więc
    // porównanie znak po znaku mówiłoby o zapisie, a nie o tym, o co pytamy.
    ought(fs::equivalent(fs::path{text(made.out)}, fs::temp()).await());
}

TRIAL("Child: zmienna środowiska dochodzi do programu") {
    Command what = pretending({"around", "TALCITE_PROBA"});
    what.environment.push_back(Variable{"TALCITE_PROBA", "wartość"});

    const Output made = Child::run(std::move(what)).await();

    same(text(made.out), std::string{"wartość"});
}

// Bez dziedziczenia program nie ma **nic** poza tym, co mu podano — a to jest jedyny sposób, żeby
// uruchomić coś w znanych warunkach.
TRIAL("Child: bez dziedziczenia zostaje samo to, co podano") {
    Command what = pretending({"around", "PATH"});
    what.inherit = false;
    what.environment.push_back(Variable{"TALCITE_PROBA", "wartość"});

    const Output made = Child::run(std::move(what)).await();

    same(text(made.out), std::string{"<brak>"});
}

// --- Zakończenie ---------------------------------------------------------------------------------

TRIAL("Child: kill kończy program, który jeszcze śpi") {
    const Child one = Child::start(pretending({"sleeping", "30000"})).await();

    Promise<Exit> waiting = one.wait();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    one.kill();

    const Exit made = waiting.await();

    ought(made.ended != Ended::lost);

#ifndef _WIN32

    ought(made.ended == Ended::killed);
    same(made.signal, SIGKILL);

#endif
}

namespace {

// Czy proces o tym numerze jeszcze żyje — czeka na jego koniec najwyżej tyle, ile podano. Zombie
// liczy się jako koniec: nie wykonuje już niczego, a pochować go może tylko jego rodzic.
bool alive(std::int64_t which, std::chrono::milliseconds patience) {
#ifdef _WIN32
    const HANDLE one = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(which));

    if (one == nullptr) return false;

    const bool still = WaitForSingleObject(one, static_cast<DWORD>(patience.count())) == WAIT_TIMEOUT;

    CloseHandle(one);

    return still;
#else
    const auto end = std::chrono::steady_clock::now() + patience;

    for (;;) {
        std::ifstream reading{"/proc/" + std::to_string(which) + "/stat"};

        std::string line;

        if (!std::getline(reading, line)) {
            if (::kill(static_cast<pid_t>(which), 0) != 0) return false;
        } else {
            const std::size_t closing = line.rfind(')');

            if (closing != std::string::npos && closing + 2 < line.size() && line[closing + 2] == 'Z') return false;
        }

        if (std::chrono::steady_clock::now() >= end) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
#endif
}

}

// `kill()` kończy całe drzewo: program i to, co on uruchomił. Rodzic tutaj uruchamia wnuka zwykłą
// drogą systemu, tak jak powłoka albo plik wsadowy uruchamiają właściwy program — dawniej wnuk
// przeżywał zabicie rodzica i spał dalej.
TRIAL("Child: kill kończy też to, co program uruchomił") {
    const Child one = Child::start(pretending({"parent", "30000"})).await();

    std::string line;

    for (int again = 0; again < 100 && line.find('\n') == std::string::npos; ++again)
        line += text(one.read(64).await());

    const std::int64_t grandchild = std::stoll(line);

    ought(alive(grandchild, std::chrono::milliseconds{0}));

    one.kill();

    (void)one.wait().await();

    ought(!alive(grandchild, std::chrono::milliseconds{3000}));
}

// `stop()` prosi o koniec całą grupę — rodzica i wnuka — a nie samego rodzica.
TRIAL("Child: stop prosi o koniec także to, co program uruchomił") {
    const Child one = Child::start(pretending({"parent", "30000"})).await();

    std::string line;

    for (int again = 0; again < 100 && line.find('\n') == std::string::npos; ++again)
        line += text(one.read(64).await());

    const std::int64_t grandchild = std::stoll(line);

    one.stop();

    (void)one.wait().await();

    ought(!alive(grandchild, std::chrono::milliseconds{3000}));
}

// Zabijanie po pochowaniu musi być **ciche i bezpieczne**: numer procesu wolno wtedy przydzielić
// komuś innemu, więc sygnał wysłany w ciemno trafiłby w obcy program.
TRIAL("Child: kill po końcu nie robi już nic") {
    const Child one = Child::start(pretending({"code", "3"})).await();

    same(one.wait().await().code, 3);

    one.kill();
    one.stop();

    same(one.wait().await().code, 3);
}

TRIAL("Child: koniec można pytać wiele razy i zawsze mówi to samo") {
    const Child one = Child::start(pretending({"code", "5"})).await();

    same(one.wait().await().code, 5);
    same(one.wait().await().code, 5);
    same(one.wait().await().code, 5);
}

// Puszczenie uchwytu nie jest rozkazem śmierci — program ma dokończyć swoje.
TRIAL("Child: close nie zabija programu") {
    Child one = Child::start(pretending({"code", "0"})).await();

    Promise<Exit> waiting = one.wait();

    one.close();

    ought(!one.held());
    same(waiting.await().code, 0);
}

#ifndef _WIN32

// Kto puścił uchwyt i **nigdy nie zapytał o koniec**, i tak nie zostawia po sobie widma: chowanie
// rusza przy uruchomieniu, a nie przy pierwszym pytaniu. Sprawdzalne tylko na POSIX, bo tylko tam
// nieodebrany koniec zostaje w tablicy procesów.
TRIAL("Child: porzucone dziecko zostaje pochowane bez pytania") {
    std::int64_t which = -1;

    {
        Child one = Child::start(pretending({"code", "0"})).await();

        which = one.pid();

        one.close();
    }

    // Pytamy sygnałem zerowym, a **nie** przez `waitpid`: `waitpid` sam by widmo sprzątnął i próba
    // przestałaby cokolwiek sprawdzać. Widmo na to pytanie wciąż odpowiada, pochowane już nie.
    for (int again = 0; again < 200; ++again) {
        if (::kill(static_cast<pid_t>(which), 0) != 0 && errno == ESRCH) return;

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    ought(!"po dziecku zostało widmo");
}

#endif

TRIAL("Child: numer procesu jest znany od razu") {
    const Child one = Child::start(pretending({"code", "0"})).await();

    ought(one.held());
    ought(one.pid() > 0);

    (void)one.wait().await();
}

// **`close_input()` przerywa zapis, który stoi na pełnej rurze.** Bez rury budzącej zamknięcie
// deskryptora nie budziło `poll`, który już na nim stał — zapis kończył się dopiero ze śmiercią
// dziecka, a robotnik puli siedział tam cały ten czas. Mierzone: 2798 ms przed poprawką, 0 ms po.
TRIAL("Child: seal() przerywa zapis stojący na pełnej rurze") {
    const Child one = Child::start(pretending({"sleeping", "5000"})).await();

    // Więcej, niż mieści rura, do dziecka, które w ogóle nie czyta — zapis stanie na pewno.
    Promise<std::size_t> wrote = one.write(bytes(std::string(1024 * 1024, 'x')));

    wrote.start();

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    const auto began = std::chrono::steady_clock::now();

    one.close_input();

    bool told = false;

    try {
        (void)wrote.await();
    } catch (const std::exception&) {
        told = true;
    }

    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();

    // Zapis kończy się na obu systemach — ale z różnych powodów, więc tylko to sprawdzamy wszędzie.
    ought(told);

    // **Natychmiastowość obowiązuje na obu systemach.** Na POSIX-ie budzi rura budząca leżąca
    // w zbiorze `poll` obok wejścia i dzwonka; pod Windowsem — zdarzenie, na które pisarz czeka
    // razem z zakończeniem własnej, nakładanej raty. Kształt jest ten sam: czekaj na dwie rzeczy
    // naraz, bo zapisu stojącego na pełnej rurze nie da się obudzić inaczej.
    //
    // Sufit z ogromnym zapasem: dziecko żyje pięć sekund, a zmierzone przerwanie zajmuje 0 ms.
    ought(took < 2000);

    one.kill();
}

// Po zapieczętowaniu nowy zapis nie ma dokąd pójść i mówi to wprost, zamiast czekać.
TRIAL("Child: zapis po seal() jest odmową") {
    const Child one = Child::start(pretending({"sleeping", "200"})).await();

    one.close_input();

    bool told = false;

    try {
        (void)one.write(bytes("raz")).await();
    } catch (const std::exception&) {
        told = true;
    }

    ought(told);

    (void)one.wait().await();
}

// Pieczętowanie dwa razy jest bez skutku, a nie błędem — tak samo jak zamykanie zamkniętego.
TRIAL("Child: seal() dwa razy nic nie psuje") {
    const Child one = Child::start(pretending({"echo"})).await();

    (void)one.write(bytes("raz")).await();

    one.close_input();
    one.close_input();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).await();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    same(text(whole), std::string{"raz"});
    same(one.wait().await().code, 0);
}

#ifdef _WIN32

namespace {

// Katalog na czas próby i PATH z nim na początku; oba wracają, jak były.
class Planted {
public:
    explicit Planted(const std::string& name) {
        where_ = std::filesystem::temp_directory_path() / std::filesystem::path{"talcite-" + name};

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
        std::filesystem::create_directories(where_, ignored);

        std::wstring room(32767, L'\0');

        const DWORD much = GetEnvironmentVariableW(L"PATH", room.data(), static_cast<DWORD>(room.size()));

        room.resize(much < room.size() ? much : 0);

        path_ = room;

        SetEnvironmentVariableW(L"PATH", (where_.wstring() + L";" + path_).c_str());
    }

    ~Planted() {
        SetEnvironmentVariableW(L"PATH", path_.c_str());

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    [[nodiscard]] const std::filesystem::path& where() const { return where_; }

private:
    std::filesystem::path where_;
    std::wstring path_;
};

}

// Plik wsadowy po samej nazwie — PATHEXT dokłada `.cmd` — i z argumentami, które cmd.exe wziąłby za
// polecenia: `&` i `|` dochodzą dosłownie, w cudzysłowie, a drugie `echo` się nie wykonuje.
TRIAL("Child: plik wsadowy po nazwie dostaje argumenty dosłownie") {
    const Planted planted{"wsad"};

    {
        std::ofstream making{planted.where() / "pomoc.cmd", std::ios::binary};
        making << "@echo off\r\necho %1\r\necho %2\r\n";
    }

    Command what;

    what.program = "pomoc";
    what.arguments = {"a & echo WSTRZYK", "b|c"};

    const Output made = Child::run(what).await();

    same(text(made.out), std::string{"\"a & echo WSTRZYK\"\r\n\"b|c\"\r\n"});
}

// Procentu cudzysłów przed cmd.exe nie chroni — `%PATH%` rozwinąłby się w środku argumentu.
TRIAL("Child: argument z procentem nie idzie do pliku wsadowego") {
    const Planted planted{"procent"};

    {
        std::ofstream making{planted.where() / "pomoc.bat", std::ios::binary};
        making << "@echo off\r\necho %1\r\n";
    }

    Command what;

    what.program = "pomoc";
    what.arguments = {"%PATH%"};

    threw((void)Child::run(what).await());
}

// Program leżący tylko w katalogu bieżącym nie uruchamia się po samej nazwie: katalog bieżący to
// katalog panelu, a w nim może leżeć cokolwiek.
TRIAL("Child: program z katalogu bieżącego nie uruchamia się po nazwie") {
    const Planted planted{"obcy"};

    const std::filesystem::path aside = planted.where() / "osobno";

    std::filesystem::create_directories(aside);
    std::filesystem::copy_file(proving::mine(), aside / "obcy.exe");

    std::wstring was(32768, L'\0');

    was.resize(GetCurrentDirectoryW(static_cast<DWORD>(was.size()), was.data()));

    SetCurrentDirectoryW(aside.c_str());

    Command what;

    what.program = "obcy";
    what.arguments = {"--pretending", "said", "x"};

    bool refused = false;

    try {
        (void)Child::run(what).await();
    } catch (const std::exception&) {
        refused = true;
    }

    SetCurrentDirectoryW(was.c_str());

    ought(refused);
}

#endif

// --- Program puszczony wolno ---------------------------------------------------------------------

namespace {

// Plik, który program puszczony wolno zapisze, kiedy wystartuje — czekamy na niego najwyżej
// kilka sekund.
std::string awaited_note(const std::filesystem::path& where) {
    for (int turn = 0; turn < 250; ++turn) {
        std::ifstream input{where, std::ios::binary};

        if (input) return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    return {};
}

}

// Program puszczony wolno żyje sam: znamy jego numer od razu, a on nie jest naszym dzieckiem —
// na POSIX-ie siedzi w innej sesji niż my i nie ma nas za rodzica.
TRIAL("Child: detached — program żyje sam, a my znamy jego numer") {
    const std::filesystem::path where = fs::temp() / "talcite-wolny.txt";

    std::error_code why;

    std::filesystem::remove(where, why);

    const std::int64_t pid = Child::detached(pretending({"noted", where.string(), "200"})).await();

    ought(pid > 0);

    const std::string told = awaited_note(where);

    ought(!told.empty());

    const std::int64_t said = std::atoll(told.c_str());

    same(said, pid);

#ifndef _WIN32
    const std::size_t first = told.find(' ');
    const std::size_t second = told.find(' ', first + 1);

    ought(first != std::string::npos && second != std::string::npos);

    if (first == std::string::npos || second == std::string::npos) return;

    const long session = std::atol(told.c_str() + first + 1);
    const long parent = std::atol(told.c_str() + second + 1);

    ought(session != static_cast<long>(::getsid(0)));
    ought(parent != static_cast<long>(::getpid()));
#endif

    std::filesystem::remove(where, why);
}

TRIAL("Child: detached — nieistniejący program to odmowa, nie cisza") {
    Command what;

    what.program = fs::temp() / "talcite-nie-ma-takiego-programu";

    threw(Child::detached(what).await());
    threw(Child::detached(Command{}).await());
}

// Katalog roboczy, którego nie ma — też odmowa, a nie program uruchomiony gdzie indziej.
TRIAL("Child: detached — katalog roboczy, którego nie ma, to odmowa") {
    Command what = pretending({"code", "0"});

    what.where = fs::temp() / "talcite-nie-ma-takiego-katalogu";

    threw(Child::detached(what).await());
}

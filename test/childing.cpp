#include "proving.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/fs.hpp"

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#endif

using namespace talcite;

namespace {

// Wszystkie próby uruchamiają **binarkę testową**, nie cudzy program: `--pretending` każe jej udawać
// to, czego akurat potrzeba. Dzięki temu nie zależymy od tego, czy na maszynie stoi powłoka, `echo`
// czy `cmd`, i to samo działa pod Windowsem i pod Linuksem.
Recipe pretending(std::vector<std::string> what) {
    Recipe made;

    made.program = fs::path{proving::mine()};
    made.arguments.emplace_back("--pretending");

    for (std::string& one : what) made.arguments.push_back(std::move(one));

    return made;
}

std::string spelled(const Bytes& what) {
    std::string made;

    made.reserve(what.size());

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

Bytes bytes(const std::string& what) {
    Bytes made;

    made.reserve(what.size());

    for (const char one : what) made.push_back(static_cast<std::byte>(one));

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
    const Done made = taken(pretending({"said", "dzień dobry"})).await();

    same(spelled(made.read), std::string{"dzień dobry"});
    ought(made.errors.empty());
    ought(made.leave.ended == Ended::normal);
    same(made.leave.code, 0);
}

TRIAL("Child: kod wyjścia wraca taki, jaki program podał") {
    const Done made = taken(pretending({"code", "7"})).await();

    ought(made.leave.ended == Ended::normal);
    same(made.leave.code, 7);
}

// Dwa strumienie są **osobne**, dopóki nikt nie poprosi o zlanie ich w jeden.
TRIAL("Child: wyjście i błędy idą osobno") {
    const Done made = taken(pretending({"both"})).await();

    same(spelled(made.read), std::string{"wyjście"});
    same(spelled(made.errors), std::string{"błąd"});
}

TRIAL("Child: merge zlewa błędy do wyjścia") {
    Recipe what = pretending({"both"});
    what.merge = true;

    const Done made = taken(std::move(what)).await();

    ought(made.errors.empty());
    same(made.read.size(), std::string{"wyjście"}.size() + std::string{"błąd"}.size());
}

// Systemy mówią o tym **różnymi drogami** i obie są prawidłowe. Na POSIX rozwidlenie się udaje,
// a `exec` już nie — dziecko wychodzi wtedy z kodem 127, bo o niepowodzeniu nie ma komu powiedzieć
// wcześniej. Na Windowsie `CreateProcessW` odmawia od razu i odmowa wraca odrzuceniem obietnicy.
// Próba pilnuje jedynej rzeczy wspólnej: że żadna z tych dróg nie oddaje cichego zera.
TRIAL("Child: nieistniejący program nie udaje, że poszło") {
    Recipe what;
    what.program = fs::path{"nie-ma-takiego-programu-w-talcite"};

    try {
        const Done made = taken(std::move(what)).await();

        ought(made.leave.code != 0);
    } catch (const std::exception&) {
    }
}

TRIAL("Child: brak programu jest odmową, nie ciszą") {
    threw((void)taken(Recipe{}).await());
}

// --- Wejście -------------------------------------------------------------------------------------

TRIAL("Child: podane wejście wraca wyjściem") {
    const Done made = taken(pretending({"echo"}), bytes("tam i z powrotem")).await();

    same(spelled(made.read), std::string{"tam i z powrotem"});
    same(made.leave.code, 0);
}

// Zamknięcie wejścia jest **poleceniem**: bez niego `echo` czekałoby w nieskończoność.
TRIAL("Child: bez zamknięcia wejścia program czeka, z zamknięciem kończy") {
    const Child one = Child::start(pretending({"echo"})).await();

    (void)one.write(bytes("raz")).await();
    one.seal();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).await();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    same(spelled(whole), std::string{"raz"});
    same(one.leave().await().code, 0);
}

// Rura ma dno. Program, który pisze na obie strony więcej, niż one mieszczą, zakleszcza czytającego,
// który patrzy tylko w jedną — i to jest właśnie powód, dla którego `taken` istnieje.
TRIAL("Child: taken nie zakleszcza się na obfitym wyjściu") {
    const Done made = taken(pretending({"much"})).await();

    same(made.read.size(), std::size_t{300 * 1000});
    same(made.errors.size(), std::size_t{300 * 1000});
    same(made.leave.code, 0);
}

// --- Przepis -------------------------------------------------------------------------------------

TRIAL("Child: argumenty idą osobno, także ze spacjami") {
    const Done made = taken(pretending({"args", "jeden dwa", "trzy"})).await();

    same(spelled(made.read), std::string{"jeden dwa\ntrzy\n"});
}

TRIAL("Child: katalog roboczy jest tym, który podano") {
    Recipe what = pretending({"where"});
    what.where = fs::temp();

    const Done made = taken(std::move(what)).await();

    // **Pytamy system, a nie porównujemy napisów.** Ta sama ścieżka bywa zapisana inaczej —
    // Windows oddaje katalog tymczasowy z ukośnikiem na końcu, a katalog bieżący bez niego — więc
    // porównanie znak po znaku mówiłoby o zapisie, a nie o tym, o co pytamy.
    ought(fs::alike(fs::path{spelled(made.read)}, fs::temp()).await());
}

TRIAL("Child: zmienna środowiska dochodzi do programu") {
    Recipe what = pretending({"around", "TALCITE_PROBA"});
    what.around.push_back(Around{"TALCITE_PROBA", "wartość"});

    const Done made = taken(std::move(what)).await();

    same(spelled(made.read), std::string{"wartość"});
}

// Bez dziedziczenia program nie ma **nic** poza tym, co mu podano — a to jest jedyny sposób, żeby
// uruchomić coś w znanych warunkach.
TRIAL("Child: bez dziedziczenia zostaje samo to, co podano") {
    Recipe what = pretending({"around", "PATH"});
    what.inherit = false;
    what.around.push_back(Around{"TALCITE_PROBA", "wartość"});

    const Done made = taken(std::move(what)).await();

    same(spelled(made.read), std::string{"<brak>"});
}

// --- Zakończenie ---------------------------------------------------------------------------------

TRIAL("Child: kill kończy program, który jeszcze śpi") {
    const Child one = Child::start(pretending({"sleeping", "30000"})).await();

    Promise<Leave> waiting = one.leave();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    one.kill();

    const Leave made = waiting.await();

    ought(made.ended != Ended::lost);

#ifndef _WIN32

    ought(made.ended == Ended::killed);
    same(made.signal, SIGKILL);

#endif
}

// Zabijanie po pochowaniu musi być **ciche i bezpieczne**: numer procesu wolno wtedy przydzielić
// komuś innemu, więc sygnał wysłany w ciemno trafiłby w obcy program.
TRIAL("Child: kill po końcu nie robi już nic") {
    const Child one = Child::start(pretending({"code", "3"})).await();

    same(one.leave().await().code, 3);

    one.kill();
    one.stop();

    same(one.leave().await().code, 3);
}

TRIAL("Child: koniec można pytać wiele razy i zawsze mówi to samo") {
    const Child one = Child::start(pretending({"code", "5"})).await();

    same(one.leave().await().code, 5);
    same(one.leave().await().code, 5);
    same(one.leave().await().code, 5);
}

// Puszczenie uchwytu nie jest rozkazem śmierci — program ma dokończyć swoje.
TRIAL("Child: close nie zabija programu") {
    Child one = Child::start(pretending({"code", "0"})).await();

    Promise<Leave> waiting = one.leave();

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

    (void)one.leave().await();
}

// **`seal()` przerywa zapis, który stoi na pełnej rurze.** Bez rury budzącej zamknięcie
// deskryptora nie budziło `poll`, który już na nim stał — zapis kończył się dopiero ze śmiercią
// dziecka, a robotnik puli siedział tam cały ten czas. Mierzone: 2798 ms przed poprawką, 0 ms po.
TRIAL("Child: seal() przerywa zapis stojący na pełnej rurze") {
    const Child one = Child::start(pretending({"sleeping", "5000"})).await();

    // Więcej, niż mieści rura, do dziecka, które w ogóle nie czyta — zapis stanie na pewno.
    Promise<std::size_t> wrote = one.write(bytes(std::string(1024 * 1024, 'x')));

    wrote.go();

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    const auto began = std::chrono::steady_clock::now();

    one.seal();

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

    one.seal();

    bool told = false;

    try {
        (void)one.write(bytes("raz")).await();
    } catch (const std::exception&) {
        told = true;
    }

    ought(told);

    (void)one.leave().await();
}

// Pieczętowanie dwa razy jest bez skutku, a nie błędem — tak samo jak zamykanie zamkniętego.
TRIAL("Child: seal() dwa razy nic nie psuje") {
    const Child one = Child::start(pretending({"echo"})).await();

    (void)one.write(bytes("raz")).await();

    one.seal();
    one.seal();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).await();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    same(spelled(whole), std::string{"raz"});
    same(one.leave().await().code, 0);
}

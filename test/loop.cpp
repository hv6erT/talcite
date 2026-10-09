#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/promise.hpp"

using namespace talcite;

TRIAL("Loop: turn wykonuje odłożone i zwraca ile") {
    Loop loop;

    int counted = 0;

    loop.post([&counted] { ++counted; });
    loop.post([&counted] { counted += 10; });

    same(loop.waiting(), std::size_t{2});
    same(loop.turn(), std::size_t{2});
    same(counted, 11);
    same(loop.turn(), std::size_t{0});
}

TRIAL("Loop: current widzi tylko własny wątek") {
    Loop loop;

    ought(Loop::current() == &loop);

    std::atomic<bool> seen{true};

    std::thread aside{[&seen] { seen.store(Loop::current() != nullptr); }};
    aside.join();

    ought(!seen.load());
}

TRIAL("Loop: dzwonek budzi czekającego") {
    Loop loop;

    ought(!loop.bell().wait(std::chrono::milliseconds{0}));

    loop.post([] {});

    ought(loop.bell().wait(std::chrono::milliseconds{0}));

    (void)loop.turn();

    ought(!loop.bell().wait(std::chrono::milliseconds{0}));
}

TRIAL("Promise: here przenosi resztę łańcucha na wątek pętli") {
    Loop loop;

    const std::thread::id mine = std::this_thread::get_id();

    std::atomic<bool> off{false};
    std::atomic<bool> home{false};
    std::atomic<bool> done{false};

    Promise<int> chain =
        Promise<int>{[](auto resolve, auto) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            resolve(1);
        }}
            .then([&off, mine](const int& given) {
                off.store(std::this_thread::get_id() != mine);
                return given;
            })
            .here()
            .then([&home, &done, mine](const int& given) {
                home.store(std::this_thread::get_id() == mine);
                done.store(true);
                return given;
            });

    chain.start();

    for (int at = 0; at < 200 && !done.load(); ++at) {
        (void)loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    ought(off.load());
    ought(home.load());
    ought(done.load());
}

// `on(pętla)` wskazuje pętlę wprost, więc łańcuch zbudowany na jednym wątku może skończyć się na
// pętli innego — na przykład pętli sieci albo podglądu, gdy będzie ich więcej. Wątek budujący nie
// ma tu pętli wcale, a ciąg dalszy i tak ląduje tam, gdzie kazano.
TRIAL("Promise: on przenosi resztę łańcucha na wskazaną pętlę") {
    std::atomic<Loop*> other{nullptr};
    std::atomic<bool> stop{false};
    std::thread::id theirs;

    std::thread aside{[&other, &stop, &theirs] {
        Loop loop;

        theirs = std::this_thread::get_id();
        other.store(&loop);

        while (!stop.load()) {
            (void)loop.turn();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        other.store(nullptr);
    }};

    while (other.load() == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    ought(Loop::current() == nullptr);

    std::thread::id ran;

    Promise<int> chain = Promise<int>{[](auto resolve, auto) { resolve(3); }}
                             .on(*other.load())
                             .then([&ran](const int& given) {
                                 ran = std::this_thread::get_id();
                                 return given;
                             });

    same(chain.await(), 3);
    ought(ran == theirs);

    stop.store(true);
    aside.join();
}

// **`here()` bez pętli to błąd wołającego**, zgłoszony od razu, przy budowaniu łańcucha. Dawniej
// było cichym przejściem: ciąg dalszy biegł wtedy na wątku, który przyniósł wynik — dokładnie ten
// wyścig o drzewo węzłów, przed którym `here()` ma chronić. Kod, który ma prawo działać bez pętli,
// sprawdza ją sam, zanim zbuduje łańcuch (tak robi pasek postępu).
TRIAL("Promise: bez pętli here rzuca, zanim cokolwiek ruszy") {
    std::atomic<bool> ran{false};

    Promise<int> source{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(2);
    }};

    threw((void)source.here());
    ought(!ran.load());
}

TRIAL("then: na rozstrzygniętej z here czeka na turn, a nie odpala od razu") {
    Loop loop;

    Promise<int> done{[](auto resolve, auto) { resolve(4); }};

    same(done.await(), 4);

    std::atomic<bool> ran{false};

    Promise<int> late = done.here().then([&ran](const int& given) {
        ran.store(true);
        return given;
    });

    late.start();

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    ought(!ran.load());

    for (int at = 0; at < 100 && !ran.load(); ++at) {
        (void)loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    ought(ran.load());
}

// **Praca puli, która skończy się po pętli, nie sięga do niej.** Ogniwo `here` trzymało gołe
// wskazanie na pętlę i po jej zniszczeniu wołało `post` na martwym obiekcie. W programie pętla żyje
// do końca, ale w próbach ginie z końcem próby — sanitizer złapał to kiedyś w twa. Tu widać to bez
// sanitizera: nowa pętla staje w tym samym miejscu pamięci co martwa, więc stare wskazanie trafiało
// do niej, a ciąg dalszy obiecany martwej pętli wykonywał się na cudzej.
TRIAL("Loop: ciąg dalszy obiecany martwej pętli przepada, a nie trafia do następnej") {
    std::atomic<bool> go{false};
    std::atomic<bool> ran{false};

    std::optional<Loop> slot;
    std::optional<Promise<void>> held;

    slot.emplace();

    held.emplace(Promise<void>{[&go](auto resolve, auto) {
                     while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds{1});

                     resolve();
                 }}
                     .here()
                     .then([&ran] { ran.store(true); }));

    held->start();

    slot.reset();
    slot.emplace();

    go.store(true);

    pooling().drain();

    (void)slot->turn();

    ought(!ran.load());
    same(slot->waiting(), std::size_t{0});
}

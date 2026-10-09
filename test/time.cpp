#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "talcite/promise.hpp"
#include "talcite/time.hpp"

using namespace talcite;
using namespace std::chrono_literals;

TRIAL("Time: stoper mierzy, co minęło") {
    Stopwatch held;

    std::this_thread::sleep_for(60ms);

    ought(held.spent() >= 50ms);
    ought(held.spent() < 400ms);

    held.reset();

    ought(held.spent() < 50ms);
}

TRIAL("Time: later spełnia się po czasie, nie wcześniej") {
    Stopwatch held;

    Time::later(120ms).await();

    ought(held.spent() >= 100ms);
    ought(held.spent() < 600ms);
}

TRIAL("Time: later jest leniwe") {
    Promise<void> waiting = Time::later(40ms);

    std::this_thread::sleep_for(120ms);

    same(waiting.progress(), 0.0);

    Stopwatch held;
    waiting.await();

    ought(held.spent() >= 30ms);
}

TRIAL("Time: postęp odliczania rośnie") {
    Promise<void> waiting = Time::later(200ms);

    waiting.start();

    std::this_thread::sleep_for(100ms);

    const double part = waiting.progress();

    ought(part > 0.2);
    ought(part < 0.9);

    waiting.await();

    same(waiting.progress(), 1.0);
}

TRIAL("Time: limit przewraca się wyjątkiem Overdue") {
    bool caught = false;

    try {
        Time::limit(40ms).await();
    } catch (const Overdue&) {
        caught = true;
    }

    ought(caught);
}

TRIAL("Time: stoper nie jest zegarem") {
    static_assert(!std::is_default_constructible_v<Time>);
    static_assert(std::is_default_constructible_v<Stopwatch>);
}

TRIAL("timeout: praca, która zdążyła, oddaje swoją wartość") {
    Promise<int> quick{[](auto resolve, auto) {
        std::this_thread::sleep_for(20ms);
        resolve(7);
    }};

    same(quick.timeout(800ms).await(), 7);
}

TRIAL("timeout: praca, która nie zdążyła, przewraca się wyjątkiem Overdue") {
    Promise<int> slow{[](auto resolve, auto) {
        std::this_thread::sleep_for(600ms);
        resolve(1);
    }};

    Stopwatch held;
    bool caught = false;

    try {
        (void)slow.timeout(60ms).await();
    } catch (const Overdue&) {
        caught = true;
    }

    ought(caught);
    ought(held.spent() < 500ms);
}

TRIAL("timeout: po przekroczeniu praca jest anulowana") {
    Promise<void> slow = Time::later(60000ms);

    Promise<void> bounded = slow.timeout(40ms);

    threw(bounded.await());

    bool cancelled = false;

    try {
        slow.await();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    ought(cancelled);
}

TRIAL("timeout: odrzucenie pracy przechodzi takie, jakie jest") {
    Promise<int> broken{[](auto, auto reject) { reject("zepsute"); }};

    std::string said;

    try {
        (void)broken.timeout(800ms).await();
    } catch (const Overdue&) {
        said = "overdue";
    } catch (const Rejected& why) {
        said = why.what();
    }

    same(said, std::string{"zepsute"});
}

TRIAL("timeout: termin liczy się od uruchomienia, nie od złożenia") {
    Promise<int> quick{[](auto resolve, auto) { resolve(3); }};

    Promise<int> bounded = quick.timeout(40ms);

    std::this_thread::sleep_for(120ms);

    same(bounded.await(), 3);
}

TRIAL("timeout: anulowanie wyniku anuluje pracę") {
    Promise<void> slow = Time::later(60000ms);

    Promise<void> bounded = slow.timeout(60000ms);

    bounded.start();
    bounded.cancel();

    bool cancelled = false;

    try {
        slow.await();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    ought(cancelled);
}

TRIAL("timeout: spełniona praca zdejmuje termin, który już nie przewróci wyniku") {
    Promise<int> quick{[](auto resolve, auto) { resolve(5); }};

    Promise<int> bounded = quick.timeout(40ms);

    same(bounded.await(), 5);

    std::this_thread::sleep_for(100ms);

    same(bounded.await(), 5);
}

TRIAL("timeout: działa na ogniwie łańcucha i anuluje je aż do źródła") {
    Promise<void> source = Time::later(60000ms);

    Promise<int> chained = source.then([] { return 1; });

    threw(chained.timeout(40ms).await());

    bool cancelled = false;

    try {
        source.await();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    ought(cancelled);
}

TRIAL("Time: sto oczekiwań nie kosztuje stu wątków") {
    std::vector<Promise<void>> among;
    among.reserve(100);

    for (int at = 0; at < 100; ++at) among.push_back(Time::later(60ms));

    Stopwatch held;

    Promise<void>::all(among).await();

    // Gdyby każde oczekiwanie spało we własnym zadaniu, sto z nich przy dwóch rdzeniach
    // ciągnęłoby się o wiele dłużej niż jedno.
    ought(held.spent() < 900ms);
}

TRIAL("Time: anulowane oczekiwanie znika z zegara") {
    Promise<void> waiting = Time::later(60000ms);

    waiting.start();

    std::this_thread::sleep_for(40ms);

    waiting.cancel();

    threw(waiting.await());

    // Zegar ma zdjąć wpis od razu, a nie trzymać go przez minutę — jeśli tego nie zrobi,
    // zakończenie procesu i tak przejdzie, ale wpis leżałby do terminu.
    Time::later(30ms).await();
}

TRIAL("Time: znacznik kalendarzowy idzie do przodu") {
    const std::chrono::sys_seconds one = Time::stamp();

    ought(one.time_since_epoch().count() > 1700000000);
}

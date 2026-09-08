#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "talcite/promise.hpp"
#include "talcite/time.hpp"

using namespace talcite;
using namespace std::chrono_literals;

TRIAL("Time: stoper mierzy, co minęło") {
    Time held;

    std::this_thread::sleep_for(60ms);

    ought(held.spent() >= 50ms);
    ought(held.spent() < 400ms);

    held.reset();

    ought(held.spent() < 50ms);
}

TRIAL("Time: later spełnia się po czasie, nie wcześniej") {
    Time held;

    Time::later(120ms).await();

    ought(held.spent() >= 100ms);
    ought(held.spent() < 600ms);
}

TRIAL("Time: later jest leniwe") {
    Promise<void> waiting = Time::later(40ms);

    std::this_thread::sleep_for(120ms);

    same(waiting.progress(), 0.0);

    Time held;
    waiting.await();

    ought(held.spent() >= 30ms);
}

TRIAL("Time: postęp odliczania rośnie") {
    Promise<void> waiting = Time::later(200ms);

    waiting.go();

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

TRIAL("Time: within puszcza pracę, która zdążyła") {
    Promise<int> quick{[](auto resolve, auto) {
        std::this_thread::sleep_for(20ms);
        resolve(7);
    }};

    Time::within(quick, 800ms).await();

    same(quick.await(), 7);
}

TRIAL("Time: within przewraca pracę, która nie zdążyła") {
    Promise<int> slow{[](auto resolve, auto) {
        std::this_thread::sleep_for(600ms);
        resolve(1);
    }};

    bool caught = false;

    try {
        Time::within(slow, 60ms).await();
    } catch (const Overdue&) {
        caught = true;
    }

    ought(caught);
}

TRIAL("Time: sto oczekiwań nie kosztuje stu wątków") {
    std::vector<Promise<void>> among;
    among.reserve(100);

    for (int at = 0; at < 100; ++at) among.push_back(Time::later(60ms));

    Time held;

    Promise<void>::all(among).await();

    // Gdyby każde oczekiwanie spało we własnym zadaniu, sto z nich przy dwóch rdzeniach
    // ciągnęłoby się o wiele dłużej niż jedno.
    ought(held.spent() < 900ms);
}

TRIAL("Time: anulowane oczekiwanie znika z zegara") {
    Promise<void> waiting = Time::later(60000ms);

    waiting.go();

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

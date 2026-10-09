#include "proving.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <deque>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "talcite/bell.hpp"
#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/promise.hpp"

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace talcite;

namespace {

template <typename T>
Promise<T> gave(T given) {
    return Promise<T>{[given](auto resolve, auto) { resolve(given); }};
}

Promise<int> slowly(int given, int wait) {
    return Promise<int>{[given, wait](auto resolve, auto) {
        std::this_thread::sleep_for(std::chrono::milliseconds{wait});
        resolve(given);
    }};
}

}

TRIAL("Promise: nie rusza, dopóki nikt nie każe") {
    std::atomic<bool> ran{false};

    Promise<int> one{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(7);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    ought(!ran.load());

    same(one.await(), 7);
    ought(ran.load());
}

TRIAL("Promise: await oddaje wynik") {
    Promise<std::string> one = gave<std::string>("gotowe");

    same(one.await(), std::string{"gotowe"});
}

TRIAL("Promise: wynik zostaje, praca dzieje się raz") {
    std::atomic<int> counted{0};

    Promise<int> one{[&counted](auto resolve, auto) {
        resolve(++counted);
    }};

    same(one.await(), 1);
    same(one.await(), 1);
    same(counted.load(), 1);
}

TRIAL("Promise: odrzucenie wraca z await jako wyjątek") {
    Promise<int> one{[](auto, auto reject) { reject("No such file."); }};

    bool caught = false;

    try {
        (void)one.await();
    } catch (const std::exception& why) {
        caught = std::string{why.what()} == "No such file.";
    }

    ought(caught);
}

TRIAL("Promise: rzucony wyjątek w ciele staje się odrzuceniem") {
    Promise<int> one{[](auto, auto) -> void { throw std::runtime_error{"padło"}; }};

    threw((void)one.await());
}

TRIAL("Promise: then podaje wynik dalej") {
    Promise<int> chain = gave(20).then([](const int& given) { return given + 2; });

    same(chain.await(), 22);
}

TRIAL("Promise: then zmienia typ") {
    Promise<std::string> chain =
        gave(7).then([](const int& given) { return std::to_string(given * 6); });

    same(chain.await(), std::string{"42"});
}

TRIAL("Promise: cały łańcuch rusza z ostatniego ogniwa") {
    std::atomic<bool> ran{false};

    Promise<int> root{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(1);
    }};

    Promise<int> chain = root.then([](const int& given) { return given + 1; })
                             .then([](const int& given) { return given + 1; });

    ought(!ran.load());

    same(chain.await(), 3);
}

TRIAL("Promise: odrzucenie przeskakuje then i trafia do fail") {
    std::atomic<int> ran{0};
    std::string said;

    Promise<int> chain = Promise<int>{[](auto, auto reject) { reject("zepsute"); }}
                             .then([&ran](const int& given) {
                                 ++ran;
                                 return given;
                             })
                             .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)chain.await());

    same(ran.load(), 0);
    same(said, std::string{"zepsute"});
}

// Dawniej łańcuch obietnicy nie rozwijał i oddawał Promise<Promise<int>>. Zmienione świadomie:
// bez spłaszczania łańcuch kroków asynchronicznych nie składa się wcale — każdy krok dokłada
// warstwę opakowania, a wołający musi ją zdejmować, czyli czekać, czyli blokować wątek.
TRIAL("Promise: obietnica oddana z then rozwija się w łańcuchu") {
    Promise<int> chain = gave(5).then([](const int& given) { return slowly(given * 2, 20); });

    same(chain.await(), 10);
}

TRIAL("Promise: rozwijanie działa też dla pustej obietnicy") {
    std::atomic<int> ran{0};

    Promise<void> chain = gave(1).then([&ran](const int&) {
        return Promise<void>{[&ran](auto resolve, auto) {
            ran.fetch_add(1);
            resolve();
        }};
    });

    chain.await();

    same(ran.load(), 1);
}

TRIAL("Promise: odrzucenie w środku łańcucha wychodzi na wierzch") {
    std::string said;

    Promise<int> chain =
        gave(2)
            .then([](const int&) {
                return Promise<int>{[](auto, auto reject) { reject("zepsute w środku"); }};
            })
            .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)chain.await());

    same(said, std::string{"zepsute w środku"});
}

TRIAL("Promise: dwa kroki asynchroniczne pod rząd") {
    Promise<int> chain = gave(3)
                             .then([](const int& given) { return slowly(given + 1, 10); })
                             .then([](const int& given) { return slowly(given * 10, 10); });

    same(chain.await(), 40);
}

TRIAL("Promise: obietnica wewnętrzna rusza sama, bez await") {
    std::atomic<int> ran{0};

    Promise<int> chain = gave(1).then([&ran](const int&) {
        return Promise<int>{[&ran](auto resolve, auto) {
            ran.fetch_add(1);
            resolve(7);
        }};
    });

    same(chain.await(), 7);
    same(ran.load(), 1);
}

TRIAL("Promise: void rozstrzyga się bez ładunku") {
    std::atomic<bool> ran{false};

    Promise<void> one{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve();
    }};

    one.await();

    ought(ran.load());
}

TRIAL("Promise: then po void woła bez parametru") {
    Promise<int> chain = Promise<void>{[](auto resolve, auto) { resolve(); }}.then([] { return 9; });

    same(chain.await(), 9);
}

TRIAL("Promise: rejestracja na rozstrzygniętej nie przepada") {
    Promise<int> one = gave(3);

    same(one.await(), 3);

    Promise<int> late = one.then([](const int& given) { return given * 3; });

    same(late.await(), 9);
}

TRIAL("Promise: kopia uchwytu dzieli wynik") {
    Promise<int> one = slowly(4, 20);
    Promise<int> two = one;

    same(one.await(), 4);
    same(two.await(), 4);
}

TRIAL("Promise: martwa gałąź milczy") {
    std::atomic<int> ran{0};

    Promise<int> root = slowly(1, 60);

    {
        Promise<int> doomed = root.then([&ran](const int& given) {
            ++ran;
            return given;
        });
        (void)doomed;
    }

    Promise<int> kept = root.then([](const int& given) { return given + 1; });

    same(kept.await(), 2);

    pooling().drain();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    same(ran.load(), 0);
}

TRIAL("Pool: własna pula bierze pracę i daje się opróżnić") {
    Pool mine;

    std::atomic<int> counted{0};

    for (int at = 0; at < 24; ++at) mine.post([&counted] { ++counted; });

    mine.drain();

    same(counted.load(), 24);
    ought(mine.staff() >= 1);
}

TRIAL("Pool: robotnicy wracają, a nie mnożą się bez końca") {
    Pool mine;

    for (int at = 0; at < 50; ++at) {
        mine.post([] {});
        mine.drain();
    }

    ought(mine.staff() <= 2);
}

TRIAL("Promise: cancel przed startem nie uruchamia pracy") {
    std::atomic<bool> ran{false};

    Promise<int> one{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(1);
    }};

    one.cancel();

    bool caught = false;

    try {
        (void)one.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);

    pooling().drain();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    ought(!ran.load());
}

TRIAL("Promise: cancel w trakcie — spóźniony wynik jest pomijany") {
    Promise<int> one = slowly(5, 80);

    one.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    one.cancel();

    threw((void)one.await());

    pooling().drain();

    bool caught = false;

    try {
        (void)one.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);
}

TRIAL("Promise: cancel przeskakuje then i trafia do fail") {
    std::atomic<int> ran{0};
    std::atomic<bool> knew{false};

    Promise<int> root = slowly(1, 60);

    Promise<int> chain = root.then([&ran](const int& given) {
                                 ++ran;
                                 return given;
                             })
                             .fail([&knew](const std::exception& why) {
                                 knew.store(dynamic_cast<const Cancelled*>(&why) != nullptr);
                             });

    root.cancel();

    threw((void)chain.await());

    same(ran.load(), 0);
    ought(knew.load());
}

// Rzucone coś, co nie jest `std::exception`, też trafia do `fail` — jako stałe `Rejected`
// z angielskim zdaniem, bo `fail` przyjmuje `const std::exception&` i nie ma czego innego podać.
TRIAL("Promise: fail dostaje także odrzucenie nie-wyjątkiem") {
    std::string said;

    Promise<int> chain = Promise<int>{[](auto, auto) { throw 7; }}.fail(
        [&said](const std::exception& why) { said = why.what(); });

    // Sam powód idzie dalej nietknięty — ogon rzuca tym, czym rzuciło ciało.
    bool rethrown = false;

    try {
        (void)chain.await();
    } catch (int) {
        rethrown = true;
    }

    ought(rethrown);
    same(said, std::string{"Unknown reason for rejection."});
}

TRIAL("Promise: finally biegnie po powodzeniu i nie rusza wyniku") {
    std::atomic<int> ran{0};

    Promise<int> chain = gave(11).finally([&ran] { ++ran; });

    same(chain.await(), 11);
    same(ran.load(), 1);
}

TRIAL("Promise: finally biegnie po odrzuceniu, a powód idzie dalej") {
    std::atomic<int> ran{0};
    std::string said;

    Promise<int> chain = Promise<int>{[](auto, auto reject) { reject("zepsute"); }}
                             .finally([&ran] { ++ran; })
                             .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)chain.await());

    same(ran.load(), 1);
    same(said, std::string{"zepsute"});
}

TRIAL("Promise: finally biegnie także po anulowaniu") {
    std::atomic<int> ran{0};

    Promise<int> root = slowly(2, 60);
    Promise<int> chain = root.finally([&ran] { ++ran; });

    root.cancel();

    threw((void)chain.await());

    same(ran.load(), 1);
}

// --- Anulowanie idzie do źródła --------------------------------------------------------------

// **Anuluje się pracę, a nie widok na nią** (przegląd, krok 6c). Wołający trzyma zwykle ogon —
// ostatnie ogniwo z `then`, `fail` i `on` — i to jego chce przerwać. Dawniej anulowanie ogona
// odrzucało sam ogon, a odczyt pod spodem biegł do końca; twa trzymało więc głowę i ogon osobno.
TRIAL("cancel: ogon anuluje źródło, zanim ono ruszy") {
    std::atomic<bool> ran{false};

    Promise<int> root{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(1);
    }};

    Promise<int> tail = root.then([](const int& given) { return given + 1; })
                            .fail([](const std::exception&) {})
                            .finally([] {});

    tail.cancel();

    threw((void)tail.await());
    threw((void)root.await());

    pooling().drain();

    ought(!ran.load());
}

TRIAL("cancel: ogon przerywa źródło, które już biegnie") {
    Promise<int> root = slowly(1, 80);
    Promise<int> tail = root.then([](const int& given) { return given; });

    tail.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    tail.cancel();

    bool caught = false;

    try {
        (void)root.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);
}

// Obietnica oddana z `then` jest pracą tak samo jak źródło — często jedyną, która jeszcze trwa,
// bo źródło dawno się rozstrzygnęło. Anulowanie ogona musi dojść i do niej.
TRIAL("cancel: dochodzi do obietnicy oddanej z then") {
    Promise<int> inner = slowly(2, 80);

    Promise<int> tail = gave(1).then([inner](const int&) { return inner; });

    tail.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    tail.cancel();

    bool caught = false;

    try {
        (void)inner.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);
}

// Ogon anulowany, zanim `then` zdążył oddać swoją obietnicę: oddana już nie rusza. Bez tego
// anulowanie trafiałoby w szczelinę między końcem ciała `then` a startem tego, co oddało.
TRIAL("cancel: obietnica oddana z then po anulowaniu już nie rusza") {
    std::atomic<bool> release{false};
    std::atomic<bool> ran{false};

    Promise<int> tail = gave(1).then([&release, &ran](const int&) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds{2});

        return Promise<int>{[&ran](auto resolve, auto) {
            ran.store(true);
            resolve(2);
        }};
    });

    tail.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    tail.cancel();
    release.store(true);

    threw((void)tail.await());

    pooling().drain();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    ought(!ran.load());
}

TRIAL("cancel: złączenie anuluje wszystkich członków") {
    Promise<int> one = slowly(1, 80);
    Promise<int> two = slowly(2, 80);

    Promise<void> both = Promise<int>::all(one, two);

    both.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    both.cancel();

    threw((void)one.await());
    threw((void)two.await());

    std::vector<Promise<int>> many{slowly(3, 80), slowly(4, 80)};

    Promise<void> all = Promise<int>::race(many);

    all.start();
    all.cancel();

    threw((void)many[0].await());
    threw((void)many[1].await());
}

// Rozstrzygniętego nie ma czego przerywać: wynik zostaje, a źródło nie dostaje anulowania,
// choć ogon wciąż na nie wskazuje.
TRIAL("cancel: na rozstrzygniętym ogonie nic nie zmienia") {
    Promise<int> root = gave(5);
    Promise<int> tail = root.then([](const int& given) { return given * 2; });

    same(tail.await(), 10);

    tail.cancel();

    same(tail.await(), 10);
    same(root.await(), 5);

    // Także złączenie: wyścig rozstrzygnięty pierwszym członkiem nie przerywa drugiego, gdy się
    // go potem anuluje — rozstrzygnięte już nie mówi niczego w dół ani w górę.
    Promise<int> fast = gave(1);
    Promise<int> slow = slowly(2, 60);
    Promise<void> first = Promise<int>::race(fast, slow);

    first.await();
    first.cancel();

    same(slow.await(), 2);
}

// **Wspólne źródło dzieli los.** Dwie gałęzie z jednej obietnicy wskazują na tę samą pracę, więc
// anulowanie jednej przerywa ją i dla drugiej. Tak jest świadomie: liczenie zainteresowanych
// gałęzi wymagałoby stanu i zamków w każdym ogniwie dla przypadku, którego nikt dziś nie ma.
TRIAL("cancel: gałęzie ze wspólnego źródła dzielą anulowanie") {
    Promise<int> root = slowly(1, 80);
    Promise<int> left = root.then([](const int& given) { return given; });
    Promise<int> right = root.then([](const int& given) { return given; });

    left.start();
    left.cancel();

    threw((void)right.await());
}

TRIAL("all: czeka na wszystkie, a wyniki zostają w obietnicach") {
    Promise<int> one = slowly(1, 60);
    Promise<int> two = slowly(2, 10);
    Promise<int> three = slowly(3, 30);

    Promise<int>::all(one, two, three).await();

    same(one.await(), 1);
    same(two.await(), 2);
    same(three.await(), 3);
}

TRIAL("all: then po nim dostaje pustkę") {
    std::atomic<bool> ran{false};

    Promise<int> one = gave(1);
    Promise<int> two = gave(2);

    Promise<void> joined = Promise<int>::all(one, two).then([&ran] { ran.store(true); });

    joined.await();

    ought(ran.load());
}

TRIAL("all: nic nie rusza, dopóki nikt nie każe") {
    std::atomic<int> ran{0};

    std::vector<Promise<int>> among;
    for (int at = 0; at < 3; ++at)
        among.push_back(Promise<int>{[&ran](auto resolve, auto) {
            ++ran;
            resolve(1);
        }});

    Promise<void> joined = Promise<int>::all(among);

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    same(ran.load(), 0);

    joined.await();

    same(ran.load(), 3);
}

TRIAL("all: biegną obok siebie, nie po kolei") {
    const auto began = std::chrono::steady_clock::now();

    std::vector<Promise<int>> among;
    for (int at = 0; at < 4; ++at) among.push_back(slowly(at, 80));

    Promise<int>::all(among).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 240);
}

TRIAL("all: pusty zbiór rozstrzyga się od razu") {
    Promise<int>::all(std::vector<Promise<int>>{}).await();
    Promise<int>::all().await();
}

TRIAL("all: jedno odrzucenie przewraca całość") {
    std::vector<Promise<int>> among;
    among.push_back(gave(1));
    among.push_back(Promise<int>{[](auto, auto reject) { reject("zepsute"); }});
    among.push_back(gave(3));

    std::string said;

    Promise<void> joined =
        Promise<int>::all(among).fail([&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    same(said, std::string{"zepsute"});
}

TRIAL("all: łączy też obietnice bez ładunku") {
    std::atomic<int> ran{0};

    std::vector<Promise<void>> among;
    for (int at = 0; at < 5; ++at)
        among.push_back(Promise<void>{[&ran](auto resolve, auto) {
            ++ran;
            resolve();
        }});

    Promise<void>::all(among).await();

    same(ran.load(), 5);
}

namespace {

struct Highest {
    std::atomic<int> now{0};
    std::atomic<int> most{0};

    void entered() {
        const int here = ++now;

        int seen = most.load();
        while (here > seen && !most.compare_exchange_weak(seen, here)) {
        }
    }

    void left() { --now; }
};

}

TRIAL("Pool: praca licząca nie przekracza sufitu") {
    Pool mine{3};

    Highest counted;

    for (int at = 0; at < 24; ++at)
        mine.post([&counted] {
            counted.entered();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            counted.left();
        }, Lane::cpu);

    mine.drain();

    same(counted.most.load() <= 3, true);
    ought(counted.most.load() >= 2);
}

TRIAL("Pool: zwykła praca sufitu nie zna") {
    Pool mine{2};

    Highest counted;

    for (int at = 0; at < 12; ++at)
        mine.post([&counted] {
            counted.entered();
            std::this_thread::sleep_for(std::chrono::milliseconds{40});
            counted.left();
        });

    mine.drain();

    ought(counted.most.load() > 2);
}

TRIAL("Pool: pracy liczącej nie głodzi zajęty zespół") {
    Pool mine{2};

    std::atomic<bool> done{false};

    for (int at = 0; at < 4; ++at)
        mine.post([] { std::this_thread::sleep_for(std::chrono::milliseconds{60}); });

    mine.post([&done] { done.store(true); }, Lane::cpu);

    mine.drain();

    ought(done.load());
}

// --- Tory ------------------------------------------------------------------------------------

// **Tor mówi, gdzie biegnie ciało źródła** (przegląd, krok 6d). Dawniej była to klasa pochodna
// EasyPromise, której nikt w bibliotece nie używał, a flagę dało się postawić także na ogniwie —
// gdzie nie robiła nic. Teraz tor podaje się w konstruktorze i dotyczy dokładnie ciała.
TRIAL("Lane: ciało z torem cpu nie przekracza sufitu, także w złączeniu") {
    Highest counted;

    std::vector<Promise<int>> among;

    for (int at = 0; at < 16; ++at)
        among.push_back(Promise<int>{[&counted](auto resolve, auto) {
                                         counted.entered();
                                         std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                         counted.left();
                                         resolve(1);
                                     },
                                     Lane::cpu});

    Promise<int>::all(among).await();

    ought(counted.most.load() <= static_cast<int>(pooling().most()));
}

TRIAL("Lane: tor nie przyspiesza startu — obietnica dalej jest leniwa") {
    std::atomic<bool> ran{false};

    Promise<int> one{[&ran](auto resolve, auto) {
                         ran.store(true);
                         resolve(1);
                     },
                     Lane::cpu};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    ought(!ran.load());

    same(one.then([](const int& given) { return given * 2; }).await(), 2);
    ought(ran.load());
}

// **Ciąg dalszy biegnie na wątku tego, kto rozstrzygnął** — więc liczenie w `then` po odczycie
// szło torem bez sufitu. `on(Lane::cpu)` przesiada resztę łańcucha na tor liczący, tak jak
// `on(pętla)` przesiada ją na pętlę.
TRIAL("Lane: on(cpu) przesiada liczenie po zwykłym źródle pod sufit") {
    Highest counted;

    const int many = static_cast<int>(pooling().most()) * 4;

    std::vector<Promise<void>> among;

    for (int at = 0; at < many; ++at)
        among.push_back(Promise<void>{[](auto resolve, auto) { resolve(); }}
                            .on(Lane::cpu)
                            .then([&counted] {
                                counted.entered();
                                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                counted.left();
                            }));

    Promise<void>::all(among).await();

    ought(counted.most.load() <= static_cast<int>(pooling().most()));
}

// Odwrotnie: czekanie po źródle liczącym zajmowało miejsce pod sufitem, czyli rdzeń, który miał
// liczyć. `on(Lane::io)` zwalnia je — czekający biegną obok siebie, ilu ich jest.
TRIAL("Lane: on(io) wyprowadza czekanie spod sufitu") {
    Highest counted;

    const int many = static_cast<int>(pooling().most()) * 4;

    std::vector<Promise<void>> among;

    for (int at = 0; at < many; ++at)
        among.push_back(Promise<void>{[](auto resolve, auto) { resolve(); }, Lane::cpu}
                            .on(Lane::io)
                            .then([&counted] {
                                counted.entered();
                                std::this_thread::sleep_for(std::chrono::milliseconds{40});
                                counted.left();
                            }));

    Promise<void>::all(among).await();

    ought(counted.most.load() > static_cast<int>(pooling().most()));
}

TRIAL("all_settled: czeka na wszystkie i nie przewraca się na odrzuceniu") {
    Promise<int> good = slowly(7, 20);
    Promise<int> bad{[](auto, auto reject) { reject("zepsute"); }};
    Promise<int> other = slowly(9, 40);

    Promise<int>::all_settled(good, bad, other).await();

    same(good.await(), 7);
    same(other.await(), 9);
    threw((void)bad.await());
}

TRIAL("all_settled: powody zostają w obietnicach, a nie w łączeniu") {
    std::atomic<bool> ran{false};

    Promise<int> bad{[](auto, auto reject) { reject("padło"); }};

    Promise<void> joined = Promise<int>::all_settled(bad).then([&ran] { ran.store(true); });

    joined.await();

    ought(ran.load());

    std::string said;

    try {
        (void)bad.await();
    } catch (const std::exception& why) {
        said = why.what();
    }

    same(said, std::string{"padło"});
}

TRIAL("all: wyliczenie zamiast wektora") {
    Promise<int> one = slowly(1, 30);
    Promise<int> two = slowly(2, 10);

    Promise<int>::all(one, two).await();

    same(one.await(), 1);
    same(two.await(), 2);
}

TRIAL("all: wyliczenie łączy obietnice różnych typów") {
    Promise<int> counted = slowly(5, 20);
    Promise<std::string> said{[](auto resolve, auto) { resolve(std::string{"gotowe"}); }};
    Promise<void> done{[](auto resolve, auto) { resolve(); }};

    Promise<void>::all(counted, said, done).await();

    same(counted.await(), 5);
    same(said.await(), std::string{"gotowe"});
}

TRIAL("all: wyliczenie przewraca się na odrzuceniu") {
    Promise<int> good = gave(1);
    Promise<std::string> bad{[](auto, auto reject) { reject("zepsute"); }};

    std::string said;

    Promise<void> joined = Promise<void>::all(good, bad).fail(
        [&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    same(said, std::string{"zepsute"});
}

TRIAL("all_settled: wyliczenie różnych typów nie przewraca się") {
    Promise<int> good = slowly(3, 20);
    Promise<std::string> bad{[](auto, auto reject) { reject("padło"); }};

    Promise<void>::all_settled(good, bad).await();

    same(good.await(), 3);
    threw((void)bad.await());
}

// **Upuszczone łączenie nie trzyma swoich wejść.** Łączenie pilnowało ogniw, które na nie czekają,
// a ogniwa trzymały łączenie — koło, które rozrywało dopiero rozstrzygnięcie ostatniego wejścia.
// Wejście, które nie rozstrzygnie się nigdy (w twa: ciąg dalszy na pętli, która zginęła wcześniej),
// zostawiało w pamięci całość razem z wynikami wejść już rozstrzygniętych. Wyszło na sanitizerze
// w próbach twa. Sprawdzamy bez zegara: wynik rozstrzygniętego wejścia ma zniknąć razem z łączeniem.
TRIAL("all i all_settled: upuszczone łączenie nie trzyma wejść, które nie przyjdą nigdy") {
    const auto dropped = [](auto joining) {
        std::weak_ptr<int> seen;

        {
            auto kept = std::make_shared<int>(7);

            seen = kept;

            Promise<std::shared_ptr<int>> came{[kept](auto resolve, auto) { resolve(kept); }};
            Promise<std::shared_ptr<int>> never{[](auto, auto) {}};

            kept.reset();

            Promise<void> joined = joining(came, never);

            joined.start();

            pooling().drain();

            ought(!seen.expired());
        }

        pooling().drain();

        ought(seen.expired());
    };

    dropped([](Promise<std::shared_ptr<int>>& came, Promise<std::shared_ptr<int>>& never) {
        return Promise<void>::all_settled(came, never);
    });

    dropped([](Promise<std::shared_ptr<int>>& came, Promise<std::shared_ptr<int>>& never) {
        return Promise<void>::all(std::vector<Promise<std::shared_ptr<int>>>{came, never});
    });
}

TRIAL("race: rozstrzyga się z pierwszą, nie czeka na resztę") {
    const auto began = std::chrono::steady_clock::now();

    Promise<int> quick = slowly(1, 20);
    Promise<int> slow = slowly(2, 400);

    Promise<void>::race(quick, slow).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);

    same(quick.await(), 1);
}

TRIAL("race: pierwsze odrzucenie też rozstrzyga") {
    Promise<int> slow = slowly(1, 400);
    Promise<std::string> bad{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        reject("zepsute");
    }};

    std::string said;

    Promise<void> joined = Promise<void>::race(slow, bad).fail(
        [&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    same(said, std::string{"zepsute"});
}

TRIAL("race: obietnica już rozstrzygnięta wygrywa od razu") {
    Promise<int> done = gave(5);
    (void)done.await();

    Promise<int> slow = slowly(2, 400);

    const auto began = std::chrono::steady_clock::now();

    Promise<int>::race(done, slow).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);
}

TRIAL("race: bierze też zbiór policzony w czasie działania") {
    std::vector<Promise<int>> among;
    among.push_back(slowly(1, 300));
    among.push_back(slowly(2, 20));
    among.push_back(slowly(3, 300));

    const auto began = std::chrono::steady_clock::now();

    Promise<int>::race(among).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);
    same(among[1].await(), 2);
}

TRIAL("all: bierze każdy zakres, nie tylko wektor") {
    std::deque<Promise<int>> among;
    among.push_back(gave(1));
    among.push_back(gave(2));

    Promise<int>::all(among).await();

    same(among[0].await(), 1);
    same(among[1].await(), 2);
}

TRIAL("all: przewraca się od razu, nie czeka na wcześniejsze") {
    const auto began = std::chrono::steady_clock::now();

    Promise<int> slow = slowly(1, 400);
    Promise<int> bad{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        reject("zepsute");
    }};

    std::string said;

    Promise<void> joined =
        Promise<int>::all(slow, bad).fail([&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);
    same(said, std::string{"zepsute"});
}

TRIAL("all: nie zajmuje wątku puli na czas czekania") {
    Pool mine{2};

    std::atomic<int> ran{0};

    // Cztery długie zadania w puli, która i tak rośnie — chodzi o to, żeby samo czekanie
    // na all nie było jednym z nich.
    std::vector<Promise<int>> among;
    for (int at = 0; at < 4; ++at) among.push_back(slowly(at, 60));

    Promise<void> joined = Promise<int>::all(among).then([&ran] { ++ran; });

    joined.start();

    pooling().drain();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    same(ran.load(), 1);
    (void)mine.staff();
}

TRIAL("any: wygrywa pierwsza udana, choćby inna padła wcześniej") {
    Promise<int> bad{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        reject("zepsute");
    }};
    Promise<int> good = slowly(8, 40);

    Promise<int>::any(bad, good).await();

    same(good.await(), 8);
}

TRIAL("any: przewraca się dopiero, gdy padną wszystkie") {
    Promise<int> one{[](auto, auto reject) { reject("pierwsze"); }};
    Promise<int> two{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        reject("drugie");
    }};

    std::string said;

    Promise<void> joined = Promise<int>::any(one, two).fail(
        [&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    ought(!said.empty());
}

TRIAL("any: bierze też zakres") {
    std::vector<Promise<int>> among;
    among.push_back(Promise<int>{[](auto, auto reject) { reject("zepsute"); }});
    among.push_back(slowly(4, 30));

    Promise<int>::any(among).await();

    same(among[1].await(), 4);
}

TRIAL("then: wywołanie nigdy nie biegnie na wątku rejestrującym") {
    Promise<int> one = gave(3);

    same(one.await(), 3);

    const std::thread::id mine = std::this_thread::get_id();
    std::atomic<bool> aside{false};

    Promise<int> late = one.then([&aside, mine](const int& given) {
        aside.store(std::this_thread::get_id() != mine);
        return given;
    });

    same(late.await(), 3);

    ought(aside.load());
}

TRIAL("Pool: drop zdejmuje zadanie z kolejki, zanim ruszy") {
    Pool mine{1};

    std::atomic<int> ran{0};
    std::atomic<bool> late{false};

    // Sufit jeden, więc drugie i trzecie stoją w kolejce i czekają na swoją kolej.
    for (int at = 0; at < 2; ++at)
        mine.post([&ran] {
            ++ran;
            std::this_thread::sleep_for(std::chrono::milliseconds{60});
        }, Lane::cpu);

    const Pool::Job doomed = mine.post([&late] { late.store(true); }, Lane::cpu);

    mine.drop(doomed);
    mine.drain();

    same(ran.load(), 2);
    ought(!late.load());
}

TRIAL("Pool: drop na biegnącym budzi je dzwonkiem") {
    Pool mine;

    std::atomic<bool> woke{false};
    std::atomic<bool> ready{false};

    const Pool::Job one = mine.post([&woke, &ready] {
        ready.store(true);

        Bell* bell = ringing();
        if (bell == nullptr) return;

        woke.store(bell->wait(std::chrono::milliseconds{3000}));
    });

    for (int at = 0; at < 200 && !ready.load(); ++at)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    mine.drop(one);
    mine.drain();

    ought(woke.load());
}

TRIAL("Pool: dzwonek nie przechodzi na następne zadanie") {
    Pool mine{1};

    std::atomic<bool> woke{true};

    const Pool::Job one =
        mine.post([] { std::this_thread::sleep_for(std::chrono::milliseconds{40}); }, Lane::cpu);

    mine.post([&woke] {
        Bell* bell = ringing();
        if (bell != nullptr) woke.store(bell->wait(std::chrono::milliseconds{0}));
    }, Lane::cpu);

    mine.drop(one);
    mine.drain();

    ought(!woke.load());
}

TRIAL("Promise: cancel nie uruchamia zadania stojącego w kolejce") {
    std::atomic<int> ran{0};

    std::vector<Promise<int>> among;

    for (int at = 0; at < 40; ++at)
        among.push_back(Promise<int>{[&ran](auto resolve, auto) {
                                         ++ran;
                                         std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                         resolve(1);
                                     },
                                     Lane::cpu});

    for (auto& one : among) one.start();

    for (std::size_t at = among.size() / 2; at < among.size(); ++at) among[at].cancel();

    pooling().drain();

    ought(ran.load() < 40);
}

namespace {

// Przykład tego, o co chodzi w wirtualnym progress: bilet, który wie, ile rat z ilu zeszło.
struct Counted : detail::Result<int> {
    std::atomic<int> done{0};
    std::atomic<int> whole{0};

    [[nodiscard]] double progress() const override {
        const int all = whole.load();
        if (all == 0) return 0.0;

        return std::min(1.0, static_cast<double>(done.load()) / static_cast<double>(all));
    }
};

class Stepping : public Promise<int> {
public:
    explicit Stepping(int steps) : Stepping(steps, std::make_shared<Counted>()) {}

private:
    Stepping(int steps, std::shared_ptr<Counted> made)
        : Promise<int>(
              [steps, made](auto resolve, auto) {
                  made->whole.store(steps);

                  for (int at = 0; at < steps; ++at) {
                      std::this_thread::sleep_for(std::chrono::milliseconds{20});
                      ++made->done;
                  }

                  resolve(steps);
              },
              made) {}
};

}

TRIAL("progress: zwykła obietnica zna tylko zero i jeden") {
    Promise<int> one = slowly(1, 60);

    same(one.progress(), 0.0);

    same(one.await(), 1);
    same(one.progress(), 1.0);
}

TRIAL("progress: odrzucona też jest skończona") {
    Promise<int> one{[](auto, auto reject) { reject("zepsute"); }};

    threw((void)one.await());

    same(one.progress(), 1.0);
}

TRIAL("progress: pochodny bilet liczy po swojemu") {
    Stepping one{5};

    same(one.progress(), 0.0);

    one.start();

    bool grew = false;

    for (int at = 0; at < 200; ++at) {
        const double part = one.progress();
        if (part > 0.0 && part < 1.0) grew = true;

        std::this_thread::sleep_for(std::chrono::milliseconds{5});

        if (part >= 1.0) break;
    }

    same(one.await(), 5);

    ought(grew);
    same(one.progress(), 1.0);
}

TRIAL("progress: przycięcie do Promise nie gubi sposobu liczenia") {
    Stepping made{4};
    Promise<int> one = made;

    one.start();

    bool grew = false;

    for (int at = 0; at < 200 && !grew; ++at) {
        const double part = one.progress();
        if (part > 0.0 && part < 1.0) grew = true;

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    same(one.await(), 4);

    ought(grew);
}

namespace {

// Obietnica, która nigdy nie rusza — służy wyłącznie do ustawiania postępu z ręki.
class Sitting : public Promise<int> {
public:
    Sitting() : Sitting(std::make_shared<Counted>()) {}

    void set(int done, int whole) const {
        held_->whole.store(whole);
        held_->done.store(done);
    }

private:
    explicit Sitting(std::shared_ptr<Counted> made)
        : Promise<int>([](auto, auto) {}, made), held_(std::move(made)) {}

    std::shared_ptr<Counted> held_;
};

}

TRIAL("progress: all uśrednia składowe") {
    const Sitting one;
    const Sitting two;
    const Sitting three;

    one.set(10, 10);
    two.set(5, 10);
    three.set(0, 10);

    const Promise<void> joined = Promise<int>::all(one, two, three);

    ought(std::abs(joined.progress() - 0.5) < 0.001);
}

TRIAL("progress: race i any biorą największą") {
    const Sitting one;
    const Sitting two;

    one.set(2, 10);
    two.set(7, 10);

    ought(std::abs(Promise<int>::race(one, two).progress() - 0.7) < 0.001);
    ought(std::abs(Promise<int>::any(one, two).progress() - 0.7) < 0.001);
}

TRIAL("progress: all_settled też uśrednia") {
    const Sitting one;
    const Sitting two;

    one.set(1, 4);
    two.set(3, 4);

    ought(std::abs(Promise<int>::all_settled(one, two).progress() - 0.5) < 0.001);
}

TRIAL("progress: rozstrzygnięte łączenie mówi jeden") {
    Promise<int> one = gave(1);
    Promise<int> two = gave(2);

    Promise<void> joined = Promise<int>::all(one, two);

    same(joined.progress(), 0.0);

    joined.await();

    same(joined.progress(), 1.0);
}

TRIAL("progress: zakresowa postać też liczy") {
    std::vector<Sitting> among(4);

    among[0].set(4, 4);
    among[1].set(2, 4);
    among[2].set(0, 4);
    among[3].set(2, 4);

    std::vector<Promise<int>> plain{among.begin(), among.end()};

    ought(std::abs(Promise<int>::all(plain).progress() - 0.5) < 0.001);
}

// **Zadanie żyje krócej niż ogłoszenie o jego końcu.**
//
// Domknięcie zadania trzyma to, co złapał wołający — obietnicę, jej wynik, cudze uchwyty. Pętla
// robotnika kasowała je dopiero na końcu obrotu, czyli **po** tym, jak pod zamkiem zmniejszyła
// `busy_` i obudziła `drain()`. Ten, kto czekał, ruszał więc dalej, podczas gdy cudzy wątek wciąż
// uruchamiał destruktory tego, co sam mu podał — i to jest ten wyścig, który zgłaszał TSan
// przy zwalnianiu wyniku obietnicy.
//
// Sprawdzamy to bez zegara i bez losowości: destruktor domknięcia śpi, więc gdyby `drain()` wracał
// przed nim, chorągiewka byłaby jeszcze opuszczona.
TRIAL("Pula: po opróżnieniu nikt już nie trzyma domknięcia zadania") {
    struct Watch {
        std::shared_ptr<std::atomic<bool>> gone;

        Watch(std::shared_ptr<std::atomic<bool>> given) : gone(std::move(given)) {}

        Watch(Watch&&) noexcept = default;
        Watch(const Watch&) = default;
        Watch& operator=(Watch&&) noexcept = default;
        Watch& operator=(const Watch&) = default;

        ~Watch() {
            if (!gone) return;

            std::this_thread::sleep_for(std::chrono::milliseconds{50});

            gone->store(true);
        }
    };

    Pool pool;

    const auto gone = std::make_shared<std::atomic<bool>>(false);
    const auto ran = std::make_shared<std::atomic<bool>>(false);

    (void)pool.post([watch = Watch{gone}, ran] { ran->store(true); });

    pool.drain();

    ought(ran->load());
    ought(gone->load());
}

// **Anulowanie z drugiego wątku, równolegle z `start()`.** Numer zadania wpisuje pula pod swoim
// zamkiem, zanim zadanie stanie się widoczne dla robotników — bez tego był tam wyścig, w którym
// `cancel()` widział zero, nie dzwonił dzwonkiem, a praca przechodziła całą robotę do końca.
// Zmierzone przed poprawką: na 1523 uruchomione ciała 467 nie usłyszało dzwonka.
//
// Sprawdzenie jest jednostronne, więc próba nie może być kapryśna: **żadne** ciało nie ma prawa
// dojść do końca po anulowaniu. Ile ich w ogóle ruszy, zależy od przeplotu i nie jest badane.
TRIAL("Promise: anulowanie równoległe ze start() zawsze dochodzi do ciała") {
    std::atomic<int> ran{0};
    std::atomic<int> heard{0};
    std::atomic<int> whole{0};

    for (int at = 0; at < 200; ++at) {
        Promise<int> one{[&ran, &heard, &whole](auto resolve, auto) {
            ran.fetch_add(1);

            // Praca ratami, tak jak robi to chodzenie po drzewie i kopiowanie: między ratami
            // pytamy dzwonek, czy jesteśmy jeszcze potrzebni. **Raty trwają razem do dwóch
            // sekund**, a nie dwóch milisekund: anulowanie biegnie w osobnym wątku, a na
            // obciążonej maszynie ten wątek potrafi stać dłużej, niż trwała cała praca. Wtedy
            // praca kończyła się uczciwie przed anulowaniem i próba pękała, choć kod był dobry.
            Bell* bell = ringing();

            for (int step = 0; step < 400; ++step) {
                if (bell != nullptr && bell->wait(std::chrono::milliseconds{5})) {
                    heard.fetch_add(1);

                    return;
                }
            }

            whole.fetch_add(1);

            resolve(1);
        }};

        std::thread other{[&one] { one.cancel(); }};

        one.start();

        other.join();

        try {
            (void)one.await();
        } catch (...) {
        }
    }

    // **Czekamy, aż pula się uspokoi**, i dopiero wtedy liczymy. `await()` wraca, gdy tylko
    // obietnica jest odrzucona — a ciało robotnika może wtedy jeszcze biec i dopiero za chwilę
    // usłyszeć dzwonek. Bez tego próba pękałaby raz na kilka przebiegów, i to nie z winy kodu.
    pooling().drain();

    same(whole.load(), 0);
    same(heard.load(), ran.load());
}

// **Długi łańcuch kosztuje tyle, ile ma ogniw.** Ogniwo trzymało dwie kopie rodzica, a każda kopia —
// swoje dwie, więc budowa kosztowała 2^n: przy dwudziestu dwóch ogniwach sześć sekund, a przy
// dwóch tysiącach nigdy by się nie skończyła. Dziś ogniwa dzielą rodzica.
TRIAL("Promise: dwa tysiące ogniw budują się i rozstrzygają od razu") {
    Promise<int> chain = gave(0);

    for (int at = 0; at < 2000; ++at) chain = chain.then([](int one) { return one + 1; });

    same(chain.await(), 2000);
}

// Trampolina budzenia nie zmienia porządku: potomek ciągu dalszego biegnie przed jego rodzeństwem,
// tak jak wtedy, gdy budzenie szło w głąb stosu.
TRIAL("Promise: ciągi dalsze budzą się w głąb — potomek przed rodzeństwem") {
    std::mutex guard;
    std::vector<std::string> order;

    const auto noted = [&guard, &order](std::string one) {
        const std::lock_guard<std::mutex> held{guard};

        order.push_back(std::move(one));
    };

    Promise<void> first{[](auto resolve, auto) { resolve(); }};

    Promise<void> left = first.then([&noted] { noted("b"); });
    Promise<void> deeper = left.then([&noted] { noted("d"); });
    Promise<void> right = first.then([&noted] { noted("c"); });

    Promise<void>::all(deeper, right).await();

    same(order, (std::vector<std::string>{"b", "d", "c"}));
}

// **Rozstrzygnięcie i rozbiórka nie schodzą w głąb stosu.** Ciąg dalszy budził następnych na tym
// samym stosie, a zwolnienie ogona zwalniało rodzica z wnętrza destruktora — piętro na ogniwo. Przy
// stosie 8 MB rozstrzygnięcie padało przy stu tysiącach ogniw, przy 1 MB (Windows) — przy kilku
// tysiącach. Dwieście tysięcy przechodzi tu na każdym stosie.
TRIAL("Promise: dwieście tysięcy ogniw rozstrzyga się i zwalnia bez głębokiego stosu") {
    std::atomic<int> passed{0};

    {
        std::optional<Promise<void>> chain;
        chain.emplace([](auto resolve, auto) { resolve(); });

        for (int at = 0; at < 200000; ++at) chain = chain->then([&passed] { passed.fetch_add(1); });

        chain->await();

        chain.reset();
    }

    same(passed.load(), 200000);
}

#ifndef _WIN32

// Rozbiórka i anulowanie na wątku z małym stosem — 256 KB, czwarta część windowsowego. Dawniej
// padały już przy kilku tysiącach ogniw; tu jest ich pięćdziesiąt tysięcy.
TRIAL("Promise: rozbiórka i anulowanie długiego łańcucha mieszczą się w małym stosie") {
    struct Job {
        int built = 0;
    };

    Job job;

    const auto work = [](void* given) -> void* {
        auto* mine = static_cast<Job*>(given);

        std::optional<Promise<void>> chain;
        chain.emplace([](auto resolve, auto) { resolve(); });

        for (int at = 0; at < 50000; ++at) chain = chain->then([] {});

        chain->await();
        chain.reset();

        std::optional<Promise<void>> idle;
        idle.emplace([](auto resolve, auto) { resolve(); });

        for (int at = 0; at < 50000; ++at) idle = idle->then([] {});

        idle->cancel();
        idle.reset();

        mine->built = 50000;

        return nullptr;
    };

    pthread_attr_t shape;
    pthread_attr_init(&shape);
    pthread_attr_setstacksize(&shape, 256 * 1024);

    pthread_t thread;

    ought(pthread_create(&thread, &shape, work, &job) == 0);

    pthread_join(thread, nullptr);
    pthread_attr_destroy(&shape);

    same(job.built, 50000);
}

#endif

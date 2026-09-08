#include "proving.hpp"

#include <atomic>
#include <chrono>
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

TRIAL("Promise: main przenosi resztę łańcucha na wątek pętli") {
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
            .main()
            .then([&home, &done, mine](const int& given) {
                home.store(std::this_thread::get_id() == mine);
                done.store(true);
                return given;
            });

    chain.go();

    for (int at = 0; at < 200 && !done.load(); ++at) {
        (void)loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    ought(off.load());
    ought(home.load());
    ought(done.load());
}

TRIAL("Promise: bez pętli main niczego nie zmienia") {
    std::atomic<bool> ran{false};

    Promise<int> chain = Promise<int>{[](auto resolve, auto) { resolve(2); }}
                             .main()
                             .then([&ran](const int& given) {
                                 ran.store(true);
                                 return given;
                             });

    same(chain.await(), 2);
    ought(ran.load());
}

TRIAL("then: na rozstrzygniętej z main czeka na turn, a nie odpala od razu") {
    Loop loop;

    Promise<int> done{[](auto resolve, auto) { resolve(4); }};

    same(done.await(), 4);

    std::atomic<bool> ran{false};

    Promise<int> late = done.main().then([&ran](const int& given) {
        ran.store(true);
        return given;
    });

    late.go();

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    ought(!ran.load());

    for (int at = 0; at < 100 && !ran.load(); ++at) {
        (void)loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    ought(ran.load());
}

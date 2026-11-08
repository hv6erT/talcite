#include "tal/testing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "talcite/fs.hpp"
#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/promise.hpp"

// A pause of a promise: the work under it rests at its places of rest (`Stop::rested`) while a
// `Pause` is held, also work linked to the chain after the pause began, and goes on when the last
// pause goes; a stop ends the rest at once. A copy of a large file rests between its pieces.

using namespace talcite;
using namespace std::chrono_literals;

namespace stood = std::filesystem;

namespace {

// What a stepping body did: the steps it made, and whether it ended, and how.
struct Steps {
    std::atomic<int> made{0};
    std::atomic<bool> over{false};
    std::atomic<bool> stopped{false};
};

// A body of `many` steps, each a little sleep, resting before every step.
Promise<void> stepping(std::shared_ptr<Steps> steps, int many = 40) {
    return run(io(), [steps, many] {
        for (int at = 0; at < many; ++at) {
            const Stop* stop = Stop::current();

            if (stop != nullptr && !stop->rested()) {
                steps->stopped.store(true);
                steps->over.store(true);

                return;
            }

            std::this_thread::sleep_for(5ms);
            steps->made.fetch_add(1);
        }

        steps->over.store(true);
    });
}

// Waits until `until` holds, at most five seconds; whether it came.
template <typename F>
bool came(F until) {
    for (int again = 0; again < 500; ++again) {
        if (until()) return true;

        std::this_thread::sleep_for(10ms);
    }

    return until();
}

// Whether the steps stand still: after a moment for a step under way, no step for a while.
bool standing(const Steps& steps) {
    std::this_thread::sleep_for(40ms);

    const int before = steps.made.load();

    std::this_thread::sleep_for(150ms);

    return steps.made.load() == before && !steps.over.load();
}

// Two steps, the second made only after the first ended: the pause comes before the second exists.
Promise<void> walked(std::shared_ptr<Steps> steps) {
    co_await run(io(), [] { std::this_thread::sleep_for(30ms); });
    co_await stepping(std::move(steps));
}

// A coroutine that goes on on the loop's thread and asks there whether it may go on.
Promise<void> looped(std::shared_ptr<std::atomic<int>> asked) {
    co_await run(io(), [] { std::this_thread::sleep_for(20ms); }).here();

    const Stop* stop = Stop::current();

    asked->store(stop != nullptr && stop->rested() ? 1 : 2);
}

}

TRIAL("pause: work on a loop's own thread does not rest, since the loop may be what lifts the pause") {
    Loop loop;

    const auto asked = std::make_shared<std::atomic<int>>(0);

    Promise<void> work = looped(asked);
    const Pause pause = work.pause();

    for (int again = 0; again < 500 && asked->load() == 0; ++again) {
        (void)loop.turn();

        std::this_thread::sleep_for(10ms);
    }

    SAME(asked->load(), 1);
}

TRIAL("pause: the body rests while the pause is held, and goes on when it goes") {
    const auto steps = std::make_shared<Steps>();

    Promise<void> work = stepping(steps);

    OUGHT(came([&steps] { return steps->made.load() >= 2; }));

    {
        const Pause pause = work.pause();

        OUGHT(pause.held());
        OUGHT(standing(*steps));
    }

    std::move(work).get();

    SAME(steps->made.load(), 40);
    OUGHT(!steps->stopped.load());
}

TRIAL("pause: a pause of the end of a chain reaches the body at its start") {
    const auto steps = std::make_shared<Steps>();

    Promise<int> work = stepping(steps).then([] { return 7; });

    OUGHT(came([&steps] { return steps->made.load() >= 2; }));

    Pause pause = work.pause();

    OUGHT(standing(*steps));

    pause = Pause{};

    SAME(std::move(work).get(), 7);
}

TRIAL("pause: work linked after the pause began rests too, in a chain and in a coroutine") {
    const auto chained = std::make_shared<Steps>();

    Promise<void> later = run(io(), [] { std::this_thread::sleep_for(30ms); }).then([chained] { return stepping(chained); });
    Pause first = later.pause();

    OUGHT(standing(*chained));
    SAME(chained->made.load(), 0);

    first = Pause{};

    std::move(later).get();

    SAME(chained->made.load(), 40);

    const auto awaited = std::make_shared<Steps>();

    Promise<void> coroutine = walked(awaited);
    Pause second = coroutine.pause();

    OUGHT(standing(*awaited));
    SAME(awaited->made.load(), 0);

    second = Pause{};

    std::move(coroutine).get();

    SAME(awaited->made.load(), 40);
}

TRIAL("pause: the members of a join rest with it, and so does work a body waits for") {
    const auto first = std::make_shared<Steps>();
    const auto second = std::make_shared<Steps>();

    std::vector<Promise<void>> both;

    both.push_back(stepping(first));
    both.push_back(stepping(second));

    Promise<void> joined = all(std::move(both));

    OUGHT(came([&first, &second] { return first->made.load() >= 2 && second->made.load() >= 2; }));

    {
        const Pause pause = joined.pause();

        OUGHT(standing(*first));
        OUGHT(standing(*second));
    }

    std::move(joined).get();

    SAME(first->made.load() + second->made.load(), 80);

    const auto inner = std::make_shared<Steps>();
    const auto waiting = std::make_shared<std::atomic<bool>>(false);

    Promise<void> outer = run(io(), [inner, waiting] {
        Promise<void> work = stepping(inner);

        waiting->store(true);

        std::move(work).get();
    });

    OUGHT(came([&inner, &waiting] { return waiting->load() && inner->made.load() >= 2; }));

    {
        const Pause pause = outer.pause();

        OUGHT(standing(*inner));
    }

    std::move(outer).get();

    SAME(inner->made.load(), 40);
}

TRIAL("pause: the work rests while any pause is held, a view's pause holds the shared work") {
    const auto steps = std::make_shared<Steps>();

    SharedPromise<void> work = stepping(steps).share();

    OUGHT(came([&steps] { return steps->made.load() >= 2; }));

    Promise<void> view = work.view();
    Pause one = view.pause();
    Pause two = work.pause();

    OUGHT(standing(*steps));

    one = Pause{};

    OUGHT(standing(*steps));

    two = Pause{};

    std::move(view).get();

    SAME(steps->made.load(), 40);
}

// One reader's pause does not stop the shared work for another reader: the work rests only while
// every view of it is paused, or the shared promise itself is.
TRIAL("pause: a shared work rests only when all its views are paused") {
    const auto steps = std::make_shared<Steps>();

    SharedPromise<void> work = stepping(steps, 80).share();

    OUGHT(came([&steps] { return steps->made.load() >= 2; }));

    Promise<void> first = work.view();
    Promise<void> second = work.view();

    Pause one = first.pause();

    const int before = steps->made.load();

    std::this_thread::sleep_for(150ms);

    OUGHT(steps->made.load() > before + 5);

    Pause two = second.pause();

    OUGHT(standing(*steps));

    one = Pause{};

    OUGHT(came([&steps, &before] { return steps->made.load() > before + 20; }) || steps->over.load());

    two = Pause{};

    std::move(first).get();
    std::move(second).get();

    SAME(steps->made.load(), 80);
}

// Work started in the continuation of a paused chain is paused from its birth, even when the
// continuation hands it on only a while later: it takes no step meanwhile.
TRIAL("pause: work started in a paused continuation is born paused") {
    const auto later = std::make_shared<Steps>();

    Promise<void> chain = run(io(), [] { std::this_thread::sleep_for(30ms); }).then([later] {
        Promise<void> made = stepping(later);

        std::this_thread::sleep_for(100ms);

        return made;
    });

    Pause pause = chain.pause();

    std::this_thread::sleep_for(250ms);

    SAME(later->made.load(), 0);

    pause = Pause{};

    std::move(chain).get();

    SAME(later->made.load(), 40);
}

TRIAL("pause: a stop ends the rest at once, and the body sees it as a stop") {
    const auto steps = std::make_shared<Steps>();

    Promise<void> work = stepping(steps);

    OUGHT(came([&steps] { return steps->made.load() >= 2; }));

    const Pause pause = work.pause();

    OUGHT(standing(*steps));

    work.cancel();

    OUGHT(came([&steps] { return steps->over.load(); }));
    OUGHT(steps->stopped.load());
    OUGHT(steps->made.load() < 40);
}

TRIAL("pause: the body knows whether it is paused, and an empty promise gives no pause") {
    const auto asked = std::make_shared<std::atomic<int>>(0);
    const auto go = std::make_shared<std::atomic<bool>>(false);

    Promise<void> work = run(io(), [asked, go] {
        while (!go->load()) std::this_thread::sleep_for(5ms);

        const Stop* stop = Stop::current();

        asked->store(stop != nullptr && stop->paused() ? 1 : 2);
    });

    {
        const Pause pause = work.pause();

        go->store(true);

        OUGHT(came([&asked] { return asked->load() != 0; }));
    }

    SAME(asked->load(), 1);

    std::move(work).get();

    const Promise<void> none;

    OUGHT(!none.pause().held());
}

TRIAL("pause: a copy of a large file rests between its pieces") {
    const stood::path where = stood::temp_directory_path() / ("talcite-pause-" + std::to_string(reinterpret_cast<std::uintptr_t>(&where)));

    std::error_code ignored;

    stood::remove_all(where, ignored);
    stood::create_directories(where, ignored);

    const stood::path from = where / "duzy.bin";
    const stood::path to = where / "kopia.bin";
    constexpr std::uintmax_t whole = 128ULL * 1024 * 1024;

    {
        std::ofstream making{from, std::ios::binary};
        const std::string piece(1024 * 1024, 'x');

        for (std::uintmax_t at = 0; at < whole; at += piece.size()) making.write(piece.data(), static_cast<std::streamsize>(piece.size()));
    }

    Promise<void> copying = fs::copy(from, to);
    Pause pause = copying.pause();

    std::this_thread::sleep_for(200ms);

    const Progress before = copying.progress();

    std::this_thread::sleep_for(200ms);

    OUGHT(!copying.ready());
    OUGHT(copying.progress() == before);
    OUGHT(before.done < whole);

    pause = Pause{};

    std::move(copying).get();

    SAME(stood::file_size(to), whole);

    stood::remove_all(where, ignored);
}

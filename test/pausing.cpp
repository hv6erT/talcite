#include "proving.hpp"

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

}

TRIAL("pause: the body rests while the pause is held, and goes on when it goes") {
    const auto steps = std::make_shared<Steps>();

    Promise<void> work = stepping(steps);

    ought(came([&steps] { return steps->made.load() >= 2; }));

    {
        const Pause pause = work.pause();

        ought(pause.held());
        ought(standing(*steps));
    }

    std::move(work).get();

    same(steps->made.load(), 40);
    ought(!steps->stopped.load());
}

TRIAL("pause: a pause of the end of a chain reaches the body at its start") {
    const auto steps = std::make_shared<Steps>();

    Promise<int> work = stepping(steps).then([] { return 7; });

    ought(came([&steps] { return steps->made.load() >= 2; }));

    Pause pause = work.pause();

    ought(standing(*steps));

    pause = Pause{};

    same(std::move(work).get(), 7);
}

TRIAL("pause: work linked after the pause began rests too, in a chain and in a coroutine") {
    const auto chained = std::make_shared<Steps>();

    Promise<void> later = run(io(), [] { std::this_thread::sleep_for(30ms); }).then([chained] { return stepping(chained); });
    Pause first = later.pause();

    ought(standing(*chained));
    same(chained->made.load(), 0);

    first = Pause{};

    std::move(later).get();

    same(chained->made.load(), 40);

    const auto awaited = std::make_shared<Steps>();

    Promise<void> coroutine = walked(awaited);
    Pause second = coroutine.pause();

    ought(standing(*awaited));
    same(awaited->made.load(), 0);

    second = Pause{};

    std::move(coroutine).get();

    same(awaited->made.load(), 40);
}

TRIAL("pause: the members of a join rest with it, and so does work a body waits for") {
    const auto first = std::make_shared<Steps>();
    const auto second = std::make_shared<Steps>();

    std::vector<Promise<void>> both;

    both.push_back(stepping(first));
    both.push_back(stepping(second));

    Promise<void> joined = all(std::move(both));

    ought(came([&first, &second] { return first->made.load() >= 2 && second->made.load() >= 2; }));

    {
        const Pause pause = joined.pause();

        ought(standing(*first));
        ought(standing(*second));
    }

    std::move(joined).get();

    same(first->made.load() + second->made.load(), 80);

    const auto inner = std::make_shared<Steps>();
    const auto waiting = std::make_shared<std::atomic<bool>>(false);

    Promise<void> outer = run(io(), [inner, waiting] {
        Promise<void> work = stepping(inner);

        waiting->store(true);

        std::move(work).get();
    });

    ought(came([&inner, &waiting] { return waiting->load() && inner->made.load() >= 2; }));

    {
        const Pause pause = outer.pause();

        ought(standing(*inner));
    }

    std::move(outer).get();

    same(inner->made.load(), 40);
}

TRIAL("pause: the work rests while any pause is held, a view's pause holds the shared work") {
    const auto steps = std::make_shared<Steps>();

    SharedPromise<void> work = stepping(steps).share();

    ought(came([&steps] { return steps->made.load() >= 2; }));

    Promise<void> view = work.view();
    Pause one = view.pause();
    Pause two = work.pause();

    ought(standing(*steps));

    one = Pause{};

    ought(standing(*steps));

    two = Pause{};

    std::move(view).get();

    same(steps->made.load(), 40);
}

TRIAL("pause: a stop ends the rest at once, and the body sees it as a stop") {
    const auto steps = std::make_shared<Steps>();

    Promise<void> work = stepping(steps);

    ought(came([&steps] { return steps->made.load() >= 2; }));

    const Pause pause = work.pause();

    ought(standing(*steps));

    work.cancel();

    ought(came([&steps] { return steps->over.load(); }));
    ought(steps->stopped.load());
    ought(steps->made.load() < 40);
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

        ought(came([&asked] { return asked->load() != 0; }));
    }

    same(asked->load(), 1);

    std::move(work).get();

    const Promise<void> none;

    ought(!none.pause().held());
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

    ought(!copying.ready());
    ought(copying.progress() == before);
    ought(before.done < whole);

    pause = Pause{};

    std::move(copying).get();

    same(stood::file_size(to), whole);

    stood::remove_all(where, ignored);
}

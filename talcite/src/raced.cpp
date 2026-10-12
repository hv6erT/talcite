#include "raced.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// The address race (RFC 8305, "Happy Eyeballs"): connecting to the first address that answers.
// A name usually gives IPv6 and IPv4, and a network in which IPv6 is announced but does not get
// through does not refuse: the packets are lost and every attempt waits out its whole patience.
// Trying the addresses one after another, a server behind such a router would answer after ten
// seconds instead of a fraction of one. So the next address starts when the previous one has been
// silent for longer than `gap`, or at once when all attempts started so far have refused; the first
// one to connect wins, and the rest are cancelled. The failure of all is the failure of the last.
namespace talcite::net {

// The order of attempts: families alternate, starting with the one the system gave first.
std::vector<Address> interleaved(std::vector<Address> found) {
    if (found.empty()) return found;

    const Family first = found.front().family();

    std::vector<Address> own;
    std::vector<Address> other;

    for (Address& one : found) (one.family() == first ? own : other).push_back(std::move(one));

    std::vector<Address> made;

    made.reserve(own.size() + other.size());

    for (std::size_t at = 0; at < own.size() || at < other.size(); ++at) {
        if (at < own.size()) made.push_back(std::move(own[at]));
        if (at < other.size()) made.push_back(std::move(other[at]));
    }

    return made;
}

namespace {

// The state of one race. The callbacks of the attempts hold it, so it lives while an attempt is
// running; the race holds the tails of the attempts and the clocks, so that dropping them cancels
// them. The cycle this makes ends with the race: `ended` takes both lists out. The clocks and the
// stop listener reach for the race weakly, because they alone must not keep it alive.
struct Race {
    std::vector<Address> order;
    Patience patience{};
    std::chrono::milliseconds gap{};

    std::optional<Settle<Stream>> settle;

    std::mutex guard;
    std::size_t next = 0;
    std::size_t lost = 0;
    bool over = false;
    std::exception_ptr last;
    std::vector<Promise<void>> tails;
    std::vector<Promise<void>> clocks;
};

using Held = std::shared_ptr<Race>;

void launched(const Held& race);

// Ends the race and gives back its settle. The attempts still running and the clocks are dropped,
// and so cancelled, outside the lock: dropping the last owner of a promise releases what it
// waited for, and nothing of that should run under our lock.
std::optional<Settle<Stream>> ended(const Held& race, std::unique_lock<std::mutex>& kept) {
    race->over = true;

    std::optional<Settle<Stream>> settle = std::move(race->settle);
    std::vector<Promise<void>> tails = std::move(race->tails);
    std::vector<Promise<void>> clocks = std::move(race->clocks);

    race->settle.reset();

    kept.unlock();

    tails.clear();
    clocks.clear();

    return settle;
}

// An attempt connected: the first one settles the race, and a stream that comes too late is
// closed.
void won(const Held& race, Stream stream) {
    std::unique_lock<std::mutex> kept{race->guard};

    if (race->over) {
        kept.unlock();

        stream.close();

        return;
    }

    std::optional<Settle<Stream>> settle = ended(race, kept);

    if (settle) (void)settle->value(std::move(stream));
}

// An attempt failed with `why`. When it was the last address, the race fails with that error;
// otherwise the next attempt may start early.
void missed(const Held& race, std::exception_ptr why) {
    std::unique_lock<std::mutex> kept{race->guard};

    if (race->over) return;

    race->last = std::move(why);
    ++race->lost;

    if (race->lost == race->order.size()) {
        const std::exception_ptr last = race->last;

        std::optional<Settle<Stream>> settle = ended(race, kept);

        if (settle) (void)settle->fail(last);

        return;
    }

    // All attempts started so far have refused, so the next one starts at once, without waiting
    // for the clock.
    const bool idle = race->lost == race->next && race->next < race->order.size();

    kept.unlock();

    if (idle) launched(race);
}

// Starts the attempt on the next address and, when more addresses remain, the clock for the one
// after it. The promises are built outside the lock, because an attempt may end on another thread
// at any moment, and its callback takes the same lock.
void launched(const Held& race) {
    std::size_t at = 0;
    Address where;
    bool more = false;

    {
        const std::lock_guard<std::mutex> kept{race->guard};

        if (race->over || race->next >= race->order.size()) return;

        at = race->next++;
        where = race->order[at];
        more = race->next < race->order.size();
    }

    Promise<void> tail = Stream::connect(where, race->patience).outcome().then([race](Outcome<Stream> got) {
        if (got.ok()) {
            won(race, std::move(got).value());
        } else if (!got.cancelled()) {
            missed(race, got.error());
        }
    });

    Promise<void> clock;

    if (more) {
        // The clock for the next attempt. When another one has started in the meantime (a refusal
        // sped up the queue), the clock does nothing: `next` is then further than it was for
        // whoever set the clock.
        clock = later(race->gap).then([weak = std::weak_ptr<Race>{race}, after = at + 1] {
            const Held alive = weak.lock();

            if (!alive) return;

            {
                const std::lock_guard<std::mutex> held{alive->guard};

                if (alive->over || alive->next != after) return;
            }

            launched(alive);
        });
    }

    // When the race was decided before this attempt was recorded, the promises stay here and are
    // dropped on return, which cancels them: there is nothing left to run them for.
    const std::lock_guard<std::mutex> kept{race->guard};

    if (race->over) return;

    race->tails.push_back(std::move(tail));

    if (clock) race->clocks.push_back(std::move(clock));
}

}

// Connects to the first of `found` that answers, starting the next attempt after `gap` of silence
// (`stagger`, 250 ms, by default). This is private to the library: the HTTP client uses it, and the
// tests pass a gap shorter than the default. Giving up the promise ends the race and cancels the
// attempts.
Promise<Stream> raced(std::vector<Address> found, Patience patience, std::chrono::milliseconds gap) {
    if (found.empty()) return failed<Stream>(std::make_exception_ptr(Trouble{"There is no address."}));

    auto race = std::make_shared<Race>();

    race->order = interleaved(std::move(found));
    race->patience = patience;
    race->gap = gap;

    auto [made, settle] = promised<Stream>();

    race->settle.emplace(std::move(settle));

    (void)detail::stopping(race->settle->stop(), [weak = std::weak_ptr<Race>{race}] {
        const Held alive = weak.lock();

        if (!alive) return;

        std::unique_lock<std::mutex> kept{alive->guard};

        if (!alive->over) (void)ended(alive, kept);
    });

    launched(race);

    return std::move(made);
}

}

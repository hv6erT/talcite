#include "raced.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "talcite/time.hpp"

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

// The state of one race. The body of the promise returned from `raced` holds it; the links of the
// attempts and the clocks reach for it weakly, so nothing holds itself in a cycle. The state holds
// the attempts' tails (`tails`) strongly, because a parent holds its links weakly, and a tail
// without an owner would die before the attempt ends.
struct Race {
    std::vector<Address> order;
    Patience patience{};
    std::chrono::milliseconds gap{};

    Promise<Stream>::Resolve resolve;
    std::optional<Reject> reject;

    std::mutex guard;
    std::size_t next = 0;
    std::size_t lost = 0;
    bool over = false;
    std::exception_ptr last;
    std::vector<Promise<Stream>> tries;
    std::vector<Promise<void>> clocks;
    std::vector<Promise<void>> tails;
};

using Held = std::shared_ptr<Race>;

void launched(const Held& race);

// Ends the race: attempts still running and the clocks are cancelled outside the lock, because
// cancelling rejects, and the rejection comes back to `failed`, which takes the same lock.
void ended(const Held& race, std::unique_lock<std::mutex>& kept) {
    race->over = true;

    std::vector<Promise<Stream>> tries = std::move(race->tries);
    std::vector<Promise<void>> clocks = std::move(race->clocks);

    kept.unlock();

    for (Promise<Stream>& one : tries) one.cancel();
    for (Promise<void>& one : clocks) one.cancel();
}

// An attempt connected: the first one resolves the race, and a stream that comes too late is
// closed.
void won(const Held& race, Stream stream) {
    std::unique_lock<std::mutex> kept{race->guard};

    if (race->over) {
        kept.unlock();

        stream.close();

        return;
    }

    const Promise<Stream>::Resolve resolve = race->resolve;

    ended(race, kept);

    resolve(std::move(stream));
}

// An attempt failed with `why`. When it was the last address, the race is rejected with that
// error; otherwise the next attempt may start early.
void failed(const Held& race, std::exception_ptr why) {
    std::unique_lock<std::mutex> kept{race->guard};

    if (race->over) return;

    race->last = why;
    ++race->lost;

    if (race->lost == race->order.size()) {
        const Reject reject = *race->reject;
        const std::exception_ptr last = race->last;

        ended(race, kept);

        reject(last);

        return;
    }

    // All attempts started so far have refused, so the next one starts at once, without waiting
    // for the clock.
    const bool idle = race->lost == race->next && race->next < race->order.size();

    kept.unlock();

    if (idle) launched(race);
}

// Starts the attempt on the next address and, when more addresses remain, the clock for the one
// after it. The links of the attempt and the clock are built outside the lock: an immediate
// refusal (the socket cannot be opened) settles the attempt before `fail` returns, and its body
// takes the same lock.
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

    Promise<Stream> attempt = Stream::connect(where, race->patience);

    Promise<void> tail = attempt
                             .fail([weak = std::weak_ptr<Race>{race}](const std::exception&) {
                                 if (const Held alive = weak.lock()) failed(alive, std::current_exception());
                             })
                             .then([weak = std::weak_ptr<Race>{race}](Stream stream) {
                                 if (const Held alive = weak.lock()) won(alive, std::move(stream));
                                 else stream.close();
                             });

    std::optional<Promise<void>> clock;

    if (more) {
        // The clock for the next attempt. When another one has started in the meantime (a refusal
        // sped up the queue), the clock does nothing: `next` is then further than it was for
        // whoever set the clock.
        clock = Time::later(race->gap).then([weak = std::weak_ptr<Race>{race}, after = at + 1] {
            const Held alive = weak.lock();

            if (!alive) return;

            {
                const std::lock_guard<std::mutex> held{alive->guard};

                if (alive->over || alive->next != after) return;
            }

            launched(alive);
        });
    }

    bool late = false;

    {
        const std::lock_guard<std::mutex> kept{race->guard};

        late = race->over;

        if (!late) {
            race->tries.push_back(attempt);
            race->tails.push_back(tail);

            if (clock) race->clocks.push_back(*clock);
        }
    }

    // The race was decided before this attempt was recorded in the state, so there is nothing left
    // to run it for.
    if (late) {
        attempt.cancel();

        return;
    }

    tail.start();

    if (clock) clock->start();
}

}

// Connects to the first of `found` that answers, starting the next attempt after `gap` of silence
// (`stagger`, 250 ms, by default). This is private to the library: the HTTP client uses it, and the
// tests pass a gap shorter than the default.
Promise<Stream> raced(std::vector<Address> found, Patience patience, std::chrono::milliseconds gap) {
    if (found.empty())
        return Promise<Stream>{[](auto, auto reject) { reject(std::make_exception_ptr(Trouble{"There is no address."})); }};

    auto race = std::make_shared<Race>();

    race->order = interleaved(std::move(found));
    race->patience = patience;
    race->gap = gap;

    // Like every network promise, it starts at once, not at the first wait.
    Promise<Stream> made{[race](auto resolve, auto reject) {
        {
            const std::lock_guard<std::mutex> kept{race->guard};

            race->resolve = std::move(resolve);
            race->reject.emplace(std::move(reject));
        }

        launched(race);
    }};

    made.start();

    return made;
}

}

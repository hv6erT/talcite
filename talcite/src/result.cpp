#include "talcite/detail/result.hpp"

#include <deque>
#include <functional>
#include <iterator>
#include <vector>

#include "talcite/promise.hpp"

namespace talcite::detail {

// The insides of a promise live in talcite::detail: the ticket, the result, the links and the
// joins. The public promise.hpp carries only what the caller uses (`Promise`, `Reject`, `Rejected`
// and `Cancelled`), and the parts of the library that assemble their own tickets (copy progress,
// network requests) reach into `detail` explicitly.

// A rejection with something that is not a `std::exception` reaches `fail` as this one constant
// `Rejected`. It stands here, not in the header, because the links in `detail` are defined before
// `Rejected`, and this way they do not need to know it.
const std::exception& unknown() {
    static const Rejected made{"Unknown reason for rejection."};

    return made;
}


// Progress from 0 to 1. A plain promise has nothing to measure: either it is still running or it is
// not, so it reports 0 or 1. Only derived tickets bring a measure, those that know how many bytes
// out of how many they have copied or how many items out of how many they have gone through. That
// is why it is virtual and why it sits in the ticket, not in the handle: slicing a promise to the
// base class must not lose the way of counting.
double Ticket::progress() const { return over(*this) ? 1.0 : 0.0; }

// Abandoning a ticket, which only a ticket that really holds something underneath knows how to do.
// A plain promise has nothing to take down: cancellation removes its work from the pool and that is
// all. But a network ticket is at the same time a request lying in the hub; rejection alone would
// leave it there forever, with the socket's direction occupied and the handle held by `kept`. It is
// virtual for the same reason as `progress()`: the promise holds the ticket sliced to the base and
// knows nothing of the derived one.
void Ticket::dropped() {}

// Whether the ticket is settled, resolved or rejected.
bool over(const Ticket& ticket) {
    const State now = ticket.state.load();

    return now == State::resolved || now == State::rejected;
}

namespace {

// The waking trampoline. A continuation that settles something wakes the next ones. Done directly,
// on the same stack, settling a long chain would go one stack frame deeper per link, and with a
// 1 MB stack (Windows) it would crash at a few thousand links. So only the outermost `woken` on a
// thread wakes; a nested one puts its observers aside into `batch`, and the loop, after returning
// from `woke()`, puts them at the head of the queue, in the order they were put aside. The order is
// depth-first, siblings after descendants; a descendant runs after its parent returns, not during
// it.
//
// The queue lies on the stack of the outer call, and the thread knows it only through a pointer: a
// thread-local variable with a nontrivial destructor would be destroyed at thread exit before
// others that still wake.
struct Wakes {
    std::deque<std::shared_ptr<Waiter>> pending;
    std::vector<std::shared_ptr<Waiter>> batch;
};

thread_local Wakes* waking = nullptr;

// An exception from `woke()` would leave a pointer to a dead queue; the destructor clears it,
// however the loop ends.
struct Draining {
    Draining(const Draining&) = delete;
    Draining& operator=(const Draining&) = delete;
    Draining(Draining&&) = delete;
    Draining& operator=(Draining&&) = delete;

    // Publishes `mine` as this thread's waking queue.
    explicit Draining(Wakes& mine) { waking = &mine; }

    // Withdraws the thread's waking queue.
    ~Draining() { waking = nullptr; }
};

// The release trampoline: the same for teardown. A link holds its parent (`upstream_`), so
// releasing the tail of a chain would release the parent from inside the child's destructor, one
// stack frame per link. So only the outermost `buried` releases; a nested one puts what it held
// onto the outer one's stack.
thread_local std::vector<Upward>* burying = nullptr;

struct Burying {
    Burying(const Burying&) = delete;
    Burying& operator=(const Burying&) = delete;
    Burying(Burying&&) = delete;
    Burying& operator=(Burying&&) = delete;

    // Publishes `mine` as this thread's release stack.
    explicit Burying(std::vector<Upward>& mine) { burying = &mine; }

    // Withdraws the thread's release stack, however the release loop ends.
    ~Burying() { burying = nullptr; }
};

// The climbing trampoline. `start` and `cancel` go up the chain, from the tail to the source;
// done recursively, one stack frame per link, this would be a third way for a long chain to
// exhaust the stack. A nested step goes into the outer call's queue; the caller sees the same as
// with recursion, because all the steps are done before the outer call returns. The order of the
// steps does not matter here, because it cannot be observed: `start` of the source only puts work
// into the pool, and the pool takes it with many threads, so the queue simply runs from the front.
using Climbs = std::deque<std::function<void()>>;

thread_local Climbs* climbing = nullptr;

struct Climbing {
    Climbing(const Climbing&) = delete;
    Climbing& operator=(const Climbing&) = delete;
    Climbing(Climbing&&) = delete;
    Climbing& operator=(Climbing&&) = delete;

    // Publishes `mine` as this thread's climbing queue.
    explicit Climbing(Climbs& mine) { climbing = &mine; }

    // Withdraws the thread's climbing queue, however the climbing loop ends.
    ~Climbing() { climbing = nullptr; }
};

}

// Runs one step up the chain through the climbing trampoline: inside another climb the step is
// queued, otherwise it runs here together with every step it queues.
void climbed(const std::function<void()>& step) {
    if (climbing != nullptr) {
        climbing->push_back(step);

        return;
    }

    Climbs mine;
    mine.push_back(step);

    const Climbing held{mine};

    while (!mine.empty()) {
        const std::function<void()> next = std::move(mine.front());
        mine.pop_front();

        next();
    }
}

namespace {

// Swaps, not moves, the functions of `from` into `into`. The standard does not promise that a
// moved-from `std::function` is empty: libc++ copies a small object and the source keeps its copy.
// The last reference to a link would then be released in the member's destructor, after the
// trampoline, one stack frame per link. A swap leaves the source empty on every library.
void exchanged(Upward& into, Upward& from) {
    into.start.swap(from.start);
    into.cancel.swap(from.cancel);
}

}

// Releases what a link holds upward through the release trampoline, so that a long chain is torn
// down in a loop rather than by recursion.
void buried(Upward& gone) {
    if (!gone.start && !gone.cancel) return;

    if (burying != nullptr) {
        exchanged(burying->emplace_back(), gone);

        return;
    }

    std::vector<Upward> graves;

    const Burying held{graves};

    {
        Upward first;

        exchanged(first, gone);
    }

    while (!graves.empty()) {
        Upward next;

        exchanged(next, graves.back());
        graves.pop_back();
    }
}

// Wakes everyone waiting on a settled ticket, through the waking trampoline. Observers are held
// weakly, so one that has already died simply drops out. That is the whole cutting off of callbacks
// when their owner dies: there is no separate mechanism, only the absence of a pointer.
void woken(Ticket& ticket) {
    std::vector<std::shared_ptr<Waiter>> live;

    {
        const std::lock_guard<std::mutex> held{ticket.guard};

        live.reserve(ticket.watching.size());
        for (const std::weak_ptr<Waiter>& one : ticket.watching)
            if (std::shared_ptr<Waiter> kept = one.lock()) live.push_back(std::move(kept));

        ticket.watching.clear();
    }

    ticket.stirred.notify_all();

    // Outside the lock: user callbacks can take a while and can reach back in here.
    if (waking != nullptr) {
        waking->batch.insert(waking->batch.end(), std::make_move_iterator(live.begin()),
                             std::make_move_iterator(live.end()));

        return;
    }

    Wakes mine;
    mine.pending.assign(std::make_move_iterator(live.begin()), std::make_move_iterator(live.end()));

    const Draining held{mine};

    while (!mine.pending.empty()) {
        const std::shared_ptr<Waiter> one = std::move(mine.pending.front());
        mine.pending.pop_front();

        one->woke();

        if (mine.batch.empty()) continue;

        mine.pending.insert(mine.pending.begin(), std::make_move_iterator(mine.batch.begin()),
                            std::make_move_iterator(mine.batch.end()));
        mine.batch.clear();
    }
}

// Rejects the ticket with `why` and wakes its observers; a settled ticket stays as it is.
void spoiled(Ticket& ticket, std::exception_ptr why) {
    {
        const std::lock_guard<std::mutex> held{ticket.guard};
        if (over(ticket)) return;

        ticket.why = why;
        ticket.state.store(State::rejected);
    }

    woken(ticket);
}

// Registers an observer on the ticket; returns true, without registering, when the ticket is
// already settled. A registration on something already settled must not be lost, otherwise a then
// attached a fraction of a second too late would stay silent forever. But it must not be run on
// the spot either: the callback would run sometimes before returning from then and sometimes
// after, depending on whether the work managed to finish. That is why we return true and leave the
// firing to the caller, which knows where to defer it.
bool hooked(Ticket& ticket, const std::shared_ptr<Waiter>& one) {
    const std::lock_guard<std::mutex> held{ticket.guard};

    if (over(ticket)) return true;

    ticket.watching.push_back(one);

    return false;
}

}

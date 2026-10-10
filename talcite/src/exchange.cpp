#include "exchange.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"

#include "inside.hpp"
#include "sealed.hpp"
#include "starting.hpp"

#ifndef TALCITE_IOCP

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>

#ifdef TALCITE_EPOLL
#include <sys/epoll.h>
#else
#include <poll.h>
#endif
#endif

namespace talcite::net {

namespace {

// The exchange's wake-up call. On Unix a pipe is enough, but WSAPoll accepts only sockets, so on
// Windows the wake-up is a pair of sockets on the loopback. It is the same trick that every
// network library does on that system, and the only one that works there.
class Waking {
public:
    // Opens both ends of the wake-up: a connected loopback socket pair on Windows, a pipe
    // elsewhere. When that fails, both ends stay `nothing`.
    Waking() {
#ifdef _WIN32
        starting();

        const Handle gate = opened(Family::four, true);

        if (gate == nothing) return;

        sharing(gate);

        if (!listening(gate, Address::loopback(Family::four, 0), 1)) {
            ending(gate);

            return;
        }

        const Address where = named(gate, false);

        told_ = opened(Family::four, true);

        (void)joined(told_, where);

        for (int tried = 0; tried < 200 && heard_ == nothing; ++tried) {
            (void)taken(gate, heard_);

            if (heard_ == nothing) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        ending(gate);
#else
        int both[2] = {-1, -1};

        // Both ends are non-blocking, because draining the wake-up reads until it is empty, and a
        // blocking read from an empty pipe would stop the exchange forever. The wake-up must not
        // leak into a child program.
        if (sealed::piped(both, true)) {
            heard_ = both[0];
            told_ = both[1];
        }
#endif
    }

    // Closes both ends of the wake-up.
    ~Waking() {
        ending(heard_);
        ending(told_);
    }

    Waking(const Waking&) = delete;
    Waking& operator=(const Waking&) = delete;

    // The end the exchange waits on; it becomes readable when someone rings.
    [[nodiscard]] Handle heard() const { return heard_; }

    // Wakes the exchange thread from its wait by writing one byte.
    void ring() const {
        const char one = 'x';

#ifdef _WIN32
        (void)putting(told_, &one, 1);
#else
        const ssize_t sent = ::write(told_, &one, 1);

        (void)sent;
#endif
    }

    // Reads everything the wake-up has collected, so that the next wait blocks again.
    void drain() const {
        char much[64];

        for (;;) {
#ifdef _WIN32
            if (getting(heard_, much, sizeof much) <= 0) return;
#else
            const ssize_t got = ::read(heard_, much, sizeof much);

            if (got > 0) continue;

            // An interruption by a signal does not mean "empty"; otherwise a byte stays behind
            // and the wake-up keeps ringing idly.
            if (got < 0 && errno == EINTR) continue;

            return;
#endif
        }
    }

private:
    Handle heard_ = nothing;
    Handle told_ = nothing;
};

// A request ready to be handed to the pool: what it is, with which code, and whether readiness
// carried the error flag.
struct Given {
    std::shared_ptr<Doing> what;
    int code = 0;
    bool amiss = false;
};

struct Stirred {
    Handle which = nothing;
    bool reading = false;
    bool writing = false;
    bool amiss = false;
};

}

struct Exchange::Held {
    struct Watching {
        std::shared_ptr<Doing> reading;
        std::shared_ptr<Doing> writing;
    };

    mutable std::mutex guard;
    std::unordered_map<Handle, Watching> watching;

    // Sockets handed over to be closed. The exchange thread closes them, not the one who handed
    // them over: a descriptor closed under a waiting poll can come back as the number of a freshly
    // opened socket, and then the exchange watches something other than it thinks.
    std::vector<Handle> ending;
    Waking waking;
#ifdef TALCITE_EPOLL
    int rings = -1;
#endif
    std::thread staff;
    bool closing = false;
};

namespace {

std::atomic<Exchange::Held*> standing{nullptr};

#ifdef TALCITE_EPOLL

// Arming is one-shot (EPOLLONESHOT): after reporting readiness the kernel disables the socket on
// its own and stays silent until it is armed again. Without it a level-triggered epoll would spin:
// it reports readiness for as long as nobody has taken the data, and the data is to be taken only
// by the request that has just been removed.
//
// As a result removing a request costs no system call at all: the socket is already disabled.
// One call is paid when a request is made and nothing when it ends.
bool tuned(Exchange::Held& held, Handle one) {
    const auto found = held.watching.find(one);

    std::uint32_t wanted = 0;

    if (found != held.watching.end()) {
        if (found->second.reading) wanted |= EPOLLIN;
        if (found->second.writing) wanted |= EPOLLOUT;
    }

    if (wanted == 0) return true;

    epoll_event what{};

    what.events = wanted | EPOLLONESHOT;
    what.data.fd = static_cast<int>(one);

    // Modify first, add only when the socket was not there yet: that is one call cheaper in the
    // more frequent case.
    if (::epoll_ctl(held.rings, EPOLL_CTL_MOD, static_cast<int>(one), &what) == 0) return true;

    // A failed arming must not get lost. With the watch limit exhausted the kernel never reports
    // this socket ready, and a request left in the map would hang until its deadline, or without
    // end when the patience is zero.
    return ::epoll_ctl(held.rings, EPOLL_CTL_ADD, static_cast<int>(one), &what) == 0;
}

// Removes the socket from the epoll set.
void untuned(Exchange::Held& held, Handle one) {
    (void)::epoll_ctl(held.rings, EPOLL_CTL_DEL, static_cast<int>(one), nullptr);
}

// Waits up to `patience` milliseconds (-1 without limit) for the sockets that are ready. The
// wake-up is drained here and does not show in the result.
std::vector<Stirred> awaited(Exchange::Held& held, int patience) {
    epoll_event seen[256];

    const int many = ::epoll_wait(held.rings, seen, 256, patience);

    std::vector<Stirred> made;

    for (int at = 0; at < many; ++at) {
        const Handle which = static_cast<Handle>(seen[at].data.fd);
        const std::uint32_t what = seen[at].events;

        if (which == held.waking.heard()) {
            held.waking.drain();

            continue;
        }

        // A hangup and an error wake both directions, because neither will get what it waits for.
        const bool over = (what & (EPOLLERR | EPOLLHUP)) != 0;

        made.push_back(Stirred{which, (what & EPOLLIN) != 0 || over,
                               (what & EPOLLOUT) != 0 || over, (what & EPOLLERR) != 0});
    }

    return made;
}

#else

// With poll there is nothing to arm: the set is built anew before every wait.
bool tuned(Exchange::Held&, Handle) { return true; }

void untuned(Exchange::Held&, Handle) {}

// Waits up to `patience` milliseconds (-1 without limit) for the sockets that are ready, building
// the poll set from the watched map. The wake-up is drained here and does not show in the result.
std::vector<Stirred> awaited(Exchange::Held& held, int patience) {
    std::vector<pollfd> watched;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        watched.push_back(
            pollfd{static_cast<decltype(pollfd::fd)>(held.waking.heard()), POLLIN, 0});

        for (const auto& [which, pair] : held.watching) {
            short wanted = 0;

            if (pair.reading) wanted |= POLLIN;
            if (pair.writing) wanted |= POLLOUT;

            if (wanted == 0) continue;

            watched.push_back(pollfd{static_cast<decltype(pollfd::fd)>(which), wanted, 0});
        }
    }

#ifdef _WIN32
    const int stirred = ::WSAPoll(watched.data(), static_cast<ULONG>(watched.size()), patience);
#else
    const int stirred = ::poll(watched.data(), static_cast<nfds_t>(watched.size()), patience);
#endif

    std::vector<Stirred> made;

    if (stirred <= 0) return made;

    if ((watched.front().revents & POLLIN) != 0) held.waking.drain();

    for (std::size_t at = 1; at < watched.size(); ++at) {
        const short what = watched[at].revents;

        if (what == 0) continue;

        const bool over = (what & (POLLERR | POLLNVAL | POLLHUP)) != 0;

        made.push_back(Stirred{static_cast<Handle>(watched[at].fd), (what & POLLIN) != 0 || over,
                               (what & POLLOUT) != 0 || over,
                               (what & (POLLERR | POLLNVAL)) != 0});
    }

    return made;
}

#endif

// A refusal for a request on a socket that no longer exists. It goes through the pool, not
// directly, because done() wakes the waiting caller and runs its continuation, and that is not
// done on the caller's thread.
void aborted(std::shared_ptr<Doing> one) {
    pooling().post([one = std::move(one)] { one->done(0, ECONNABORTED); });
}

// The step and the completion go to a pool thread, not to the exchange thread. The continuation of
// a promise is someone else's code: if it ran on the only thread that watches all the sockets, one
// slow then would stop the whole network. So the exchange thread does nothing but wait.
void handed(std::shared_ptr<Doing> one, int what, bool amiss = false) {
    pooling().post([one = std::move(one), what, amiss]() mutable {
        if (what > 0) {
            one->done(0, what);

            return;
        }

        if (what < 0) {
            one->late();

            return;
        }

        // The system said "ready", and yet the step could not proceed. It happens when the event
        // was spurious or someone else was first. Then the request simply waits again.
        //
        // Unless readiness came with the error flag: then waiting again would be spinning, because
        // a broken socket reports itself at once and without end.
        if (!stepped(*one)) {
            if (amiss) one->done(0, EPIPE);
            else submit(std::move(one));
        }
    });
}

}

// Starts the exchange thread, which waits for readiness and deadlines and hands the requests that
// are due to the pool.
Exchange::Exchange() : held_(std::make_unique<Held>()) {
    // The pool must exist before the exchange, although nothing here needs it. Static objects are
    // destroyed in reverse order of construction, and the exchange thread hands work to the pool,
    // so if the pool were created later, it would be destroyed earlier and the thread would reach
    // into freed memory. Tests touch the pool earlier, so only a program that starts with the
    // network would show it.
    (void)pooling();

    // The pointer is set in the constructor and not at every running(): the one-time creation of a
    // static object is thread-safe, while repeated writes of the same value from several threads
    // would be a race, even when the value does not change.
    standing = held_.get();

#ifdef TALCITE_EPOLL
    held_->rings = ::epoll_create1(EPOLL_CLOEXEC);

    epoll_event what{};

    what.events = EPOLLIN;
    what.data.fd = static_cast<int>(held_->waking.heard());

    (void)::epoll_ctl(held_->rings, EPOLL_CTL_ADD, static_cast<int>(held_->waking.heard()), &what);
#endif

    held_->staff = std::thread{[this] {
        Held& held = *held_;

        for (;;) {
            std::chrono::steady_clock::time_point soonest{};
            bool timed = false;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                for (const Handle one : held.ending) net::ending(one);

                held.ending.clear();

                for (const auto& [which, pair] : held.watching)
                    for (const std::shared_ptr<Doing>& one : {pair.reading, pair.writing})
                        if (one && one->ever && (!timed || one->until < soonest)) {
                            soonest = one->until;
                            timed = true;
                        }
            }

            int patience = -1;

            if (timed) {
                // Round up, not down. Truncating to zero for a deadline closer than a millisecond
                // would make poll return at once, the sweep would find nothing expired, and the
                // loop would spin, rebuilding the whole descriptor array each time with the poll
                // backend.
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                    soonest - std::chrono::steady_clock::now());

                // And clamp to the range: the patience comes from the caller, and above roughly
                // twenty-five days the number would wrap to a negative one, which poll reads as
                // "wait forever", so such a deadline would never fire.
                const std::int64_t much =
                    std::min<std::int64_t>(std::max<std::int64_t>(left.count(), 0),
                                           std::numeric_limits<int>::max());

                patience = static_cast<int>(much);
            }

            const std::vector<Stirred> stirred = awaited(held, patience);

            std::vector<Given> giving;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                const auto now = std::chrono::steady_clock::now();

                std::vector<Handle> touched;

                for (const Stirred& one : stirred) {
                    const auto found = held.watching.find(one.which);

                    if (found == held.watching.end()) continue;

                    Held::Watching& pair = found->second;

                    // The error flag does not cut the way short. The step still runs, so bytes
                    // received before the hangup are not lost while the kernel still lets them be
                    // read, and every reason for a failed connect does not collapse into the same
                    // code. Only the step gets the real reason: recv when reading, SO_ERROR when
                    // connecting.
                    if (pair.reading && one.reading) {
                        giving.push_back(Given{std::move(pair.reading), 0, one.amiss});
                        pair.reading.reset();
                    }

                    if (pair.writing && one.writing) {
                        giving.push_back(Given{std::move(pair.writing), 0, one.amiss});
                        pair.writing.reset();
                    }

                    touched.push_back(one.which);
                }

                // Expired requests are collected separately, because a deadline also passes when
                // nothing happens on the socket, and then nobody reports it.
                for (auto& [which, pair] : held.watching)
                    for (std::shared_ptr<Doing>* one : {&pair.reading, &pair.writing})
                        if (*one && (*one)->ever && (*one)->until <= now) {
                            giving.push_back(Given{std::move(*one), -1, false});
                            one->reset();

                            touched.push_back(which);
                        }

                // Only the sockets touched in this turn are re-armed. Going over the whole map on
                // every turn would mean, with a thousand connections, a thousand system calls for
                // every single event.
                for (const Handle which : touched) {
                    const auto found = held.watching.find(which);

                    if (found == held.watching.end()) continue;

                    if (found->second.reading || found->second.writing) {
                        if (tuned(held, which)) continue;

                        for (std::shared_ptr<Doing>* one :
                             {&found->second.reading, &found->second.writing})
                            if (*one) giving.push_back(Given{std::move(*one), ENOSPC, false});

                        held.watching.erase(found);

                        continue;
                    }

                    held.watching.erase(found);
                }
            }

            for (Given& one : giving) handed(std::move(one.what), one.code, one.amiss);
        }
    }};
}

// Teardown has an order, and every step of it has a reason.
Exchange::~Exchange() {
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->closing = true;
    }

    held_->waking.ring();

    // 1. The exchange thread stops; from now on nobody hands work to the pool.
    if (held_->staff.joinable()) held_->staff.join();

    // 2. The exchange ceases to exist for the world, before the pool is drained. A task finishing
    //    in the pool can submit itself again, and a request accepted after the exchange thread has
    //    stopped would have no one to serve it and would hang until the program ends.
    standing.store(nullptr);

    // 3. The pool finishes what it was given; otherwise a completion in flight would reach in here
    //    a moment later.
    pooling().drain();

    // 4. Only now the requests that still hang. Each of them holds the socket and calls forget()
    //    in its destructor; if they died together with the map, they would reach into the exchange
    //    in the middle of its own destruction.
    //
    //    They are refused, not silently abandoned. A request is also the result of the caller's
    //    promise: an abandoned one would stay fresh, a thread standing on `await` would never wake
    //    up, and the program would never end.
    for (auto& [which, pair] : held_->watching)
        for (const std::shared_ptr<Doing>& one : {pair.reading, pair.writing})
            if (one) one->done(0, ECONNABORTED);

    held_->watching.clear();

    for (const Handle one : held_->ending) net::ending(one);

    held_->ending.clear();

#ifdef TALCITE_EPOLL
    if (held_->rings >= 0) ::close(held_->rings);
#endif
}

// The number of sockets the exchange is watching right now.
std::size_t Exchange::waiting() const {
    const std::lock_guard<std::mutex> kept{held_->guard};

    return held_->watching.size();
}

// The exchange is one per program and starts with the first socket. The thread that watches all
// connections is not started in advance.
Exchange& Exchange::running() {
    static Exchange one;

    return one;
}

namespace {

// Starting an operation. With readiness backends only a connect needs to be started; the rest
// waits for the signal from the system and only then calls stepped(). False means "already over".
bool began(Doing& one) {
    if (one.kind != Kind::joining) return true;

    // On the loopback a connect finishes at once and there is nothing to wait for.
    if (joined(one.held, one.where)) {
        one.done(0, 0);

        return false;
    }

    if (!underway()) {
        one.done(0, amiss());

        return false;
    }

    return true;
}

}

// Hands a request to the exchange: it is watched until its socket is ready or its deadline
// passes. Without a running exchange or on a closed socket the request is refused with
// ECONNABORTED.
void submit(std::shared_ptr<Doing> one) {
    (void)Exchange::running();

    Exchange::Held* held = standing.load();

    if (held == nullptr) {
        one->done(0, ECONNABORTED);

        return;
    }

    const Handle which = one->held;

    // A request on a socket that the caller has already closed. The handle is spent then, and
    // without this refusal the request would hang forever: the exchange would keep it under a
    // number nobody ever touches, and the thread waiting on the promise would stand until the
    // program ends.
    if (which == nothing) {
        aborted(std::move(one));

        return;
    }

    if (!began(*one)) return;

    const bool writing = outward(one->kind);

    std::shared_ptr<Doing> given;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        Exchange::Held::Watching& pair = held->watching[which];

        if (writing) pair.writing = std::move(one);
        else pair.reading = std::move(one);

        if (!tuned(*held, which)) {
            std::shared_ptr<Doing>& slot = writing ? pair.writing : pair.reading;

            given = std::move(slot);
            slot.reset();

            if (!pair.reading && !pair.writing) held->watching.erase(which);
        }
    }

    if (given) {
        aborted(std::move(given));

        return;
    }

    held->waking.ring();
}

// Withdrawing one request is what a cancelled promise needs. Without it an interrupted read would
// stay in the exchange with its direction taken: the caller would stop waiting, but the socket
// would answer "busy" until the program ends, and `kept` would hold the descriptor open.
//
// It all rests on an identity check under the lock. The exchange thread takes a request out of
// the map under the same lock before it hands it to the pool, so either the request is still at
// the socket or the thread has already taken it and is about to run done(). There is no third
// possibility and no race to chase.
bool withdraw(const std::shared_ptr<Doing>& one) {
    Exchange::Held* held = standing.load();

    if (held == nullptr || !one || one->held == nothing) return false;

    const Handle which = one->held;
    const bool writing = outward(one->kind);

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        const auto found = held->watching.find(which);

        if (found == held->watching.end()) return false;

        std::shared_ptr<Doing>& slot = writing ? found->second.writing : found->second.reading;

        if (slot != one) return false;

        slot.reset();

        // A failed re-arm is not turned into a refusal here. The other direction is someone
        // else's request, and cancelling one read is no place to break off a write in progress:
        // it stays in the map and ends with its deadline or as it always does.
        if (found->second.reading || found->second.writing) {
            (void)tuned(*held, which);
        } else {
            untuned(*held, which);

            held->watching.erase(found);
        }
    }

    held->waking.ring();

    return true;
}

// Closing a descriptor, handed to the exchange. It is called only when the last handle to the
// socket dies, not when someone closes it. The reason is that the system hands the number of a
// closed socket to the next one at once: if anyone could still submit that number or read on it,
// they would reach into someone else's connection. Requests in flight hold the handle through
// `kept`, so while any of them lives, this is not reached.
void retiring(Handle one) {
    if (one == nothing) return;

    Exchange::Held* held = standing.load();

    // Before the first socket and after the exchange is torn down there is no one to hand the
    // number to, and it has to be closed anyway.
    if (held == nullptr) {
        ending(one);

        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->ending.push_back(one);
    }

    held->waking.ring();
}

// Forgetting removes the socket from watching and refuses everything that hung on it. It does
// not close the descriptor; that is what retiring is for.
void forget(Handle one) {
    Exchange::Held* held = standing.load();

    // Before the exchange has started and after it has gone there is nothing to remove.
    if (held == nullptr) return;

    std::shared_ptr<Doing> reading;
    std::shared_ptr<Doing> writing;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (const auto found = held->watching.find(one); found != held->watching.end()) {
            reading = std::move(found->second.reading);
            writing = std::move(found->second.writing);

            held->watching.erase(found);
        }

        untuned(*held, one);
    }

    // Shutting the conversation down is safe at once: it does not give the number back to the
    // system, so nobody can take the descriptor from under a running step. The close comes
    // separately, through retiring, and only when the last handle to the socket is gone.
    shutting(one);

    held->waking.ring();

    if (reading) handed(std::move(reading), ECONNABORTED);
    if (writing) handed(std::move(writing), ECONNABORTED);
}

}

#endif

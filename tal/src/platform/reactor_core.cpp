#include "../netting.hpp"

#ifndef TAL_NET_IOCP

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "sealed.hpp"

#if defined(TAL_NET_EPOLL)
#include <sys/epoll.h>
#elif defined(TAL_NET_KQUEUE)
#include <sys/event.h>
#else
#include <poll.h>
#endif

// The reactor of readiness, ported from talcite's exchange.cpp: one thread for the whole program,
// started with the first request, which waits until the system says a socket is ready and then
// runs the request's step on the spot. A step is one call of the system that does not block, so
// the thread never waits on anyone's work; what the request was for goes on in its task, which
// `Doing::done` wakes on the task's own executor. Linux waits through epoll, macOS and the BSDs
// through kqueue, both armed for one report at a time, and other systems through poll.
namespace tal::net::netting {

namespace {

// The reactor's wake-up call: a pipe whose reading end it watches beside the sockets.
class Waking {
public:
    // Opens both ends, non-blocking and sealed from child programs; both stay `nothing` when that
    // fails.
    Waking() {
        int both[2] = {-1, -1};

        if (sealed::piped(both, true)) {
            heard_ = both[0];
            told_ = both[1];
        }
    }

    // Closes both ends.
    ~Waking() {
        if (heard_ >= 0) (void)::close(heard_);
        if (told_ >= 0) (void)::close(told_);
    }

    Waking(const Waking&) = delete;
    Waking& operator=(const Waking&) = delete;

    // The end the reactor waits on.
    [[nodiscard]] int heard() const { return heard_; }

    // Wakes the reactor from its wait with one byte; a full pipe already wakes it.
    void ring() const {
        const char one = 'x';

        (void)!::write(told_, &one, 1);
    }

    // Reads all that was rung, so that the next wait blocks again.
    void drain() const {
        char much[64];

        while (::read(heard_, much, sizeof much) > 0) {
        }
    }

private:
    int heard_ = -1;
    int told_ = -1;
};

// What the system said of one socket: ready to read, to write, whether it carried an error, the
// error when the system said which, and whether it refused to watch those directions at all.
struct Stirred {
    Handle which = nothing;
    bool reading = false;
    bool writing = false;
    bool amiss = false;
    int told = 0;
    bool refused = false;
};

// A request taken out to be stepped, or refused with `code`, with what the system said of it.
struct Given {
    std::shared_ptr<Doing> what;
    int code = 0;
    bool amiss = false;
    int told = 0;
};

// The reactor: the requests waiting at each socket, one for each direction, with whether each
// direction is armed where that is known; the sockets handed over to be closed; and the changes of
// kqueue that wait for the reactor's thread.
struct Reactor {
    struct Watching {
        std::shared_ptr<Doing> reading;
        std::shared_ptr<Doing> writing;
        bool reads = false;
        bool writes = false;
    };

    struct Change {
        Handle which = nothing;
        bool reading = false;
        bool adding = false;
    };

    Reactor();
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    std::mutex guard;
    std::unordered_map<Handle, Watching> watching;
    std::vector<Handle> ending;
    std::vector<Change> changes;
    Waking waking;
    int rings = -1;
    std::thread staff;
    std::size_t turns = 0;
    bool closing = false;
};

std::atomic<Reactor*> standing{nullptr};

// Whether a request of the kind `kind` waits for its socket to be writable; connecting does too.
bool outward(Step kind) { return kind == Step::joining || kind == Step::putting || kind == Step::sending; }

#if defined(TAL_NET_EPOLL)

// Arms `one`, which has a request, for the directions that have one, once (EPOLLONESHOT): after a
// report the system leaves the socket silent until it is armed again, so a socket nobody reads
// does not wake the reactor over and over, and a request that ends costs no call. False when the
// system refused to watch it, so that its requests are refused instead of waiting for ever.
bool tuned(Reactor& held, Handle one) {
    const Reactor::Watching& pair = held.watching.at(one);
    epoll_event what{};

    what.events = EPOLLONESHOT;

    if (pair.reading) what.events |= EPOLLIN;
    if (pair.writing) what.events |= EPOLLOUT;

    what.data.fd = one;

    if (::epoll_ctl(held.rings, EPOLL_CTL_MOD, one, &what) == 0) return true;

    return ::epoll_ctl(held.rings, EPOLL_CTL_ADD, one, &what) == 0;
}

// Takes `one` out of the set.
void untuned(Reactor& held, Handle one) { (void)::epoll_ctl(held.rings, EPOLL_CTL_DEL, one, nullptr); }

// Opens the set with the wake-up in it.
void founded(Reactor& held) {
    held.rings = ::epoll_create1(EPOLL_CLOEXEC);

    epoll_event what{};

    what.events = EPOLLIN;
    what.data.fd = held.waking.heard();

    (void)::epoll_ctl(held.rings, EPOLL_CTL_ADD, held.waking.heard(), &what);
}

// Waits for the sockets that are ready; the wake-up is drained here and not reported. A hangup or
// an error wakes both directions, since neither will get what it waits for, and marks the report
// amiss: a step that still cannot go on then fails instead of waiting for ever.
std::vector<Stirred> awaited(Reactor& held) {
    epoll_event seen[256];
    const int many = ::epoll_wait(held.rings, seen, 256, -1);
    std::vector<Stirred> made;

    for (int at = 0; at < many; ++at) {
        const Handle which = seen[at].data.fd;
        const std::uint32_t what = seen[at].events;

        if (which == held.waking.heard()) {
            held.waking.drain();

            continue;
        }

        const bool over = (what & (EPOLLERR | EPOLLHUP)) != 0;

        made.push_back(Stirred{which, (what & EPOLLIN) != 0 || over, (what & EPOLLOUT) != 0 || over, over, 0, false});
    }

    return made;
}

#elif defined(TAL_NET_KQUEUE)

// Arms `one`, which has a request, for the directions that have one, each filter once
// (EV_ONESHOT), as epoll is armed, and only a filter not armed already: libkqueue refuses to
// change a filter it has. The change waits for the reactor's thread, which makes it before its
// next wait (`awaited`), since libkqueue holds the queue for the whole of a wait. A filter left
// armed without a request may report; nobody is there, and the report only disarms it.
bool tuned(Reactor& held, Handle one) {
    Reactor::Watching& pair = held.watching.at(one);

    if (pair.reading && !pair.reads) held.changes.push_back(Reactor::Change{one, true, true});
    if (pair.writing && !pair.writes) held.changes.push_back(Reactor::Change{one, false, true});

    pair.reads = pair.reads || pair.reading != nullptr;
    pair.writes = pair.writes || pair.writing != nullptr;

    return true;
}

// Takes both filters of `one` out, on the reactor's thread too; a filter that already reported is
// gone, and so is one of a socket closed meanwhile, and their refusals say nothing.
void untuned(Reactor& held, Handle one) {
    held.changes.push_back(Reactor::Change{one, true, false});
    held.changes.push_back(Reactor::Change{one, false, false});
}

// Opens the queue with the wake-up in it, sealed from child programs. libkqueue (kqueue on Linux)
// cleans its queues up in a child after fork, and to do it its fork handler takes the lock of
// every queue, which `kevent` holds while it waits: a program started while the reactor waits
// would wait with it, for good. A child of ours runs another program at once and uses no queue, so
// the clean-up is turned off.
void founded(Reactor& held) {
    held.rings = ::kqueue();

    if (held.rings >= 0) (void)::fcntl(held.rings, F_SETFD, FD_CLOEXEC);

    struct kevent wanted;

    EV_SET(&wanted, held.waking.heard(), EVFILT_READ, EV_ADD, 0, 0, nullptr);

    (void)::kevent(held.rings, &wanted, 1, nullptr, 0, nullptr);

#ifdef EVFILT_LIBKQUEUE
    struct kevent unclean;

    EV_SET(&unclean, 0, EVFILT_LIBKQUEUE, EV_ADD, NOTE_FORK_CLEANUP, 0, nullptr);

    (void)::kevent(held.rings, &unclean, 1, nullptr, 0, nullptr);
#endif
}

// Makes the changes that wait, one a call, since libkqueue refuses two without room for their
// answers; a direction the system refused to watch is reported refused, before any wait. Then
// waits for the sockets that are ready; the wake-up is drained here and not reported. The end of
// the other side, or an error, wakes the direction that reported it and marks the report amiss,
// with the error the system gave: libkqueue takes a failed connect's error out of the socket.
std::vector<Stirred> awaited(Reactor& held) {
    std::vector<Reactor::Change> changes;
    std::vector<Stirred> made;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        changes.swap(held.changes);
    }

    for (const Reactor::Change& one : changes) {
        struct kevent change;

        EV_SET(&change, one.which, one.reading ? EVFILT_READ : EVFILT_WRITE, one.adding ? EV_ADD | EV_ONESHOT : EV_DELETE, 0, 0, nullptr);

        if (::kevent(held.rings, &change, 1, nullptr, 0, nullptr) != 0 && one.adding)
            made.push_back(Stirred{one.which, one.reading, !one.reading, false, 0, true});
    }

    if (!made.empty()) return made;

    struct kevent seen[256];
    const int many = ::kevent(held.rings, nullptr, 0, seen, 256, nullptr);

    for (int at = 0; at < many; ++at) {
        const auto which = static_cast<Handle>(seen[at].ident);

        if (which == held.waking.heard()) {
            held.waking.drain();

            continue;
        }

        const bool amiss = (seen[at].flags & (EV_ERROR | EV_EOF)) != 0;
        const bool reading = seen[at].filter == EVFILT_READ;
        const auto told = static_cast<int>((seen[at].flags & EV_ERROR) != 0 ? seen[at].data : (seen[at].flags & EV_EOF) != 0 ? seen[at].fflags : 0);

        made.push_back(Stirred{which, reading, !reading, amiss, told, false});
    }

    return made;
}

#else

// With poll there is nothing to arm: the set is built before every wait.
bool tuned(Reactor& held, Handle one) {
    (void)held;
    (void)one;

    return true;
}

// Nor anything to take out.
void untuned(Reactor& held, Handle one) {
    (void)held;
    (void)one;
}

// Nor a set to open.
void founded(Reactor& held) { (void)held; }

// Waits for the sockets that are ready, building the set out of the requests; the wake-up is
// drained here and not reported. A hangup or an error wakes both directions and marks the report
// amiss.
std::vector<Stirred> awaited(Reactor& held) {
    std::vector<pollfd> watched;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        watched.push_back(pollfd{held.waking.heard(), POLLIN, 0});

        for (const auto& [which, pair] : held.watching) {
            short wanted = 0;

            if (pair.reading) wanted |= POLLIN;
            if (pair.writing) wanted |= POLLOUT;

            if (wanted != 0) watched.push_back(pollfd{which, wanted, 0});
        }
    }

    const int stirred = ::poll(watched.data(), static_cast<nfds_t>(watched.size()), -1);
    std::vector<Stirred> made;

    if (stirred <= 0) return made;

    if ((watched.front().revents & POLLIN) != 0) held.waking.drain();

    for (std::size_t at = 1; at < watched.size(); ++at) {
        const short what = watched[at].revents;

        if (what == 0) continue;

        const bool over = (what & (POLLERR | POLLNVAL | POLLHUP)) != 0;

        made.push_back(Stirred{watched[at].fd, (what & POLLIN) != 0 || over, (what & POLLOUT) != 0 || over, over, 0, false});
    }

    return made;
}

#endif

// Takes the socket at `found` out of the reactor, disarming the filters it still has armed.
void dropped(Reactor& held, std::unordered_map<Handle, Reactor::Watching>::iterator found) {
    if (found->second.reads || found->second.writes) untuned(held, found->first);

    held.watching.erase(found);
}

// One step of the request `one` on a socket the system said is ready; true when the request is
// over, with `done` told, false when the system could not do it yet and it waits again. A write
// goes on until all of it is out, remembering between turns how much was sent. A connection that
// gave up before it was accepted is no failure of the listener, so it too waits again.
bool stepped(Doing& one) {
    switch (one.kind) {
        case Step::joining: one.done(0, settling(one.held)); return true;

        case Step::meeting: {
            Handle made = nothing;
            const Address from = taken(one.held, made);

            if (made == nothing) {
                const int why = amiss();

                if (again() || why == ECONNABORTED) return false;

                one.done(0, why);

                return true;
            }

            one.made = made;
            one.where = from;
            one.done(0, 0);

            return true;
        }

        case Step::getting: {
            const long much = getting(one.held, one.buffer.data(), one.buffer.size());

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }

        case Step::putting: {
            while (one.much < one.buffer.size()) {
                const long much = putting(one.held, one.buffer.data() + one.much, one.buffer.size() - one.much);

                if (much < 0) {
                    if (again()) return false;

                    one.done(one.much, amiss());

                    return true;
                }

                one.much += static_cast<std::size_t>(much);
            }

            one.done(one.much, 0);

            return true;
        }

        case Step::fetching: {
            bool cut = false;
            const long much = fetching(one.held, one.buffer.data(), one.buffer.size(), one.where, cut);

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.cut = cut;
            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }

        case Step::sending: {
            const long much = sending(one.held, one.buffer.data(), one.buffer.size(), one.where);

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }
    }

    return true;
}

// One turn of the reactor: what is to be closed is closed, the reactor waits, and the requests
// whose sockets are ready are taken out under the lock and stepped outside it; a connect the
// system said failed fails with what it said. Only the sockets touched in the turn are armed
// again. False once the reactor closes.
bool turned(Reactor& held) {
    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (held.closing) return false;

        for (const Handle one : held.ending) ending(one);

        held.ending.clear();
    }

    const std::vector<Stirred> stirred = awaited(held);
    std::vector<Given> giving;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (held.closing) return false;

        ++held.turns;

        std::vector<Handle> touched;

        for (const Stirred& one : stirred) {
            const auto found = held.watching.find(one.which);

            if (found == held.watching.end()) continue;

            Reactor::Watching& pair = found->second;

            const int code = one.refused ? ENOSPC : 0;

            if (one.reading) pair.reads = false;
            if (one.writing) pair.writes = false;

            if (pair.reading && one.reading) giving.push_back(Given{std::exchange(pair.reading, {}), code, one.amiss, one.told});
            if (pair.writing && one.writing) giving.push_back(Given{std::exchange(pair.writing, {}), code, one.amiss, one.told});

            touched.push_back(one.which);
        }

        for (const Handle which : touched) {
            const auto found = held.watching.find(which);

            if (found == held.watching.end()) continue;

            if (!found->second.reading && !found->second.writing) {
                dropped(held, found);

                continue;
            }

            if (tuned(held, which)) continue;

            for (std::shared_ptr<Doing>* one : {&found->second.reading, &found->second.writing})
                if (*one) giving.push_back(Given{std::exchange(*one, {}), ENOSPC, false, 0});

            dropped(held, found);
        }
    }

    for (Given& one : giving) {
        if (one.code != 0 || (one.told != 0 && one.what->kind == Step::joining)) {
            one.what->done(0, one.code != 0 ? one.code : one.told);

            continue;
        }

        if (stepped(*one.what)) continue;

        if (one.amiss) one.what->done(0, EPIPE);
        else submit(std::move(one.what));
    }

    return true;
}

// Opens the set and starts the reactor's thread.
Reactor::Reactor() {
    founded(*this);

    standing.store(this);

    staff = std::thread{[this] {
        while (turned(*this)) {
        }
    }};
}

// Stops the thread, and only then lets the world see no reactor: the requests that still wait are
// let go, not refused, since the executors they would wake may be gone with the program, and the
// sockets they held close at once, now that nobody hands them over.
Reactor::~Reactor() {
    {
        const std::lock_guard<std::mutex> kept{guard};

        closing = true;
    }

    waking.ring();

    if (staff.joinable()) staff.join();

    standing.store(nullptr);

    for (const Handle one : ending) netting::ending(one);

    ending.clear();

    std::unordered_map<Handle, Watching> left = std::move(watching);

    watching.clear();
    left.clear();

    if (rings >= 0) (void)::close(rings);
}

// The reactor, started with the first request.
Reactor* running() {
    static Reactor one;

    return standing.load();
}

// Starts the request `one`: only a connect is started here, the rest waits for readiness. False
// when it is over already, as a connect on the loopback can be.
bool began(Doing& one) {
    if (one.kind != Step::joining) return true;

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

// Hands the request `one` to the reactor, which steps it when its socket is ready. A request on a
// closed socket, one whose awaiter is gone already, and one the system refuses to watch are
// refused at once. Both checks are under the reactor's lock: a withdrawal, or a close, either
// finds the request or the request never gets in, even one the reactor stepped and hands back.
void submit(std::shared_ptr<Doing> one) {
    Reactor* held = running();

    if (held == nullptr || one->held == nothing) {
        one->done(0, aborted());

        return;
    }

    if (!began(*one)) return;

    const Handle which = one->held;
    const bool writing = outward(one->kind);
    std::shared_ptr<Doing> refused;
    int why = ENOSPC;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (one->gone() || one->closed()) {
            refused = std::move(one);
            why = aborted();
        } else {
            Reactor::Watching& pair = held->watching[which];
            std::shared_ptr<Doing>& slot = writing ? pair.writing : pair.reading;

            slot = std::move(one);

            if (!tuned(*held, which)) {
                refused = std::exchange(slot, {});

                if (!pair.reading && !pair.writing) dropped(*held, held->watching.find(which));
            }
        }
    }

    if (refused) {
        refused->done(0, why);

        return;
    }

    held->waking.ring();
}

// Takes the request `one` back, when its awaiter goes before it ended; false when the reactor has
// already taken it out to step it, and then `done` comes. Both happen under the reactor's lock,
// so there is no third case.
bool withdraw(const std::shared_ptr<Doing>& one) {
    Reactor* held = standing.load();

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

// Hands the socket `one` over to be closed by the reactor's thread, between two waits: closed
// under a wait, its number could come back as a new socket's, and the reactor would watch the
// wrong one. Called with the last holder of the socket, so nobody submits it any more.
void retiring(Handle one) {
    if (one == nothing) return;

    Reactor* held = standing.load();

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

// Stops watching the socket `one` and refuses what waits on it; what comes for it later finds its
// socket closed (`Doing::closed`). Its conversation is shut, which does not give its number back;
// that is for `retiring`.
void forget(Handle one) {
    Reactor* held = standing.load();

    if (held == nullptr) {
        shutting(one);

        return;
    }

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

    shutting(one);
    held->waking.ring();

    if (reading) reading->done(0, aborted());
    if (writing) writing->done(0, aborted());
}

// How many sockets the reactor watches now.
std::size_t watching() {
    Reactor* held = standing.load();

    if (held == nullptr) return 0;

    const std::lock_guard<std::mutex> kept{held->guard};

    return held->watching.size();
}

// How many times the reactor has waited.
std::size_t turns() {
    Reactor* held = standing.load();

    if (held == nullptr) return 0;

    const std::lock_guard<std::mutex> kept{held->guard};

    return held->turns;
}

}

#endif

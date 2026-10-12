#include "talcite/listener.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "handing.hpp"
#include "inside.hpp"
#include "piping.hpp"
#include "starting.hpp"

namespace talcite::net {

struct Listener::Held {
    // The last handle to the gate is gone: close it if that has not happened yet, and only now
    // hand the descriptor number to the exchange to be closed.
    ~Held() {
        close();

        retiring(numbered);
    }

    // Closes the gate: stops listening and cancels a pending accept.
    //
    // A local gate removes its file. A file left behind misleads the next run: it looks like a live
    // socket but is not, and although `listening` can recognize it by the refused connection,
    // leaving litter in the user's directory is not our business. The file is removed at close,
    // not with the last handle: that one dies only when the cancelled accepts come back, and by
    // then a new gate may already stand at that path and would lose its file. Removing the path
    // does no harm to the descriptor.
    //
    // A pipe gate has no listening socket and no file: the spare instance is closed, and a pending
    // accept gets cancelled on its own instance.
    void close() {
        if (const Handle one = which.exchange(nothing); one != nothing) {
            forget(one);

            if (mine.family() == Family::local) (void)std::remove(mine.path().c_str());
        }

        if (!gate) return;

        pipes::shut(*gate);

        const std::lock_guard<std::mutex> kept{guard};

        if (waiting != nothing) forget(waiting);
    }

    // The number outlives the close and dies only with the handle. Requests in flight hold the
    // handle through `kept`, so the descriptor is closed only when nobody can submit it or read
    // from it any more. Closed earlier, the number could come back as a freshly opened socket, and
    // someone would read from another connection.
    Handle numbered = nothing;

    std::atomic<Handle> which{nothing};
    Address mine;

    // The exchange holds one request per socket per direction, so only one accept may run at a
    // time. A second accept started during the first would overwrite it without a trace: the
    // first would get neither a completion nor its deadline, it would vanish from the map, and its
    // promise would never settle, keeping the gate and its descriptor until the end of the program.
    //
    // We remember who holds the place, not whether someone holds it. Without the identity the
    // place could not be released at cancellation, and cancellation is the only moment the caller
    // learns about it: on IOCP a cancelled operation does not disappear but comes back through the
    // port much later, so a bare flag could be cleared only then, and it would clear someone
    // else's if another accept had started in the meantime.
    //
    // A raw pointer, because it is only an identity tag: we never reach the object through it. The
    // request lives by its own handle in the exchange and through `kept`.
    std::mutex guard;
    const void* meeting = nullptr;

    // A pipe (`piping.hpp`): the gate and the instance on which an accept waits, under the same
    // lock.
    std::shared_ptr<pipes::Gate> gate;
    Handle waiting = nothing;
};

namespace {

struct MeetPromise : Handing<Stream> {
    std::shared_ptr<Listener::Held> mine;

    // Releases the accepting place, but only while it is still ours. A late completion of a
    // cancelled operation arrives here when the place already belongs to the next one, and then
    // it must do nothing.
    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->meeting != this) return;

        mine->meeting = nullptr;
        mine->waiting = nothing;
    }

    // A pipe instance that did not become a stream is closed through the exchange, because it is
    // bound to it.
    void unused() {
        if (instance) retiring(held);
    }

    // Gives the pipe gate a spare instance again, however this accept ended.
    void refilled() {
        if (instance) pipes::refill(*mine->gate, held);
    }

    bool instance = false;

    // Cancellation releases the place at once, regardless of what `withdraw` said. On POSIX the
    // request has just left the map, and true proves it. On Windows `withdraw` can never say true,
    // because the cancelled operation comes back through the port, but there the exchange keys
    // requests by OVERLAPPED, not by socket, and a second AcceptEx on the same listening socket is
    // an ordinary thing. Either way the place is free.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    // The accept has completed: the pipe gate is refilled, the place released, and the promise
    // settled with the new stream or with a `Trouble`. After a cancellation nothing is settled.
    void done(std::size_t, int code) override {
        refilled();
        yielded();

        if (settle->stop().stopped()) {
            // The accepted socket has nobody to go to. A cancelled accept may still succeed, and
            // it would then leave an open descriptor that nobody would ever close, so it is
            // closed here. One cancelled a moment later is closed with the stream that its
            // promise refuses.
            if (instance) {
                unused();
            } else if (made != nothing) {
                ending(made);

                made = nothing;
            }

            return;
        }

        if (code != 0) {
            unused();

            (void)settle->fail(std::make_exception_ptr(
                               Trouble{"Cannot accept a connection: " + spoken(code) + ".", code,
                                       mine->mine}));

            return;
        }

        (void)settle->value(greeting(made, where));
    }

    // The deadline passed: the place is released and the promise fails with `Overdue`, unless it
    // was cancelled.
    void late() override {
        refilled();
        yielded();
        unused();

        (void)settle->fail(std::make_exception_ptr(Overdue{}));
    }
};

}

// Opens a gate listening at the given address, with the given backlog; failures throw `Trouble`.
// The constructor returns no promise because there is nothing to wait for: opening the socket,
// binding the port and listening finish at once. A promise that never waits would be a lie about
// the shape of the operation.
Listener::Listener(Address where, int waiting) : held_(std::make_shared<Held>()) {
    if (!where.held()) throw Trouble{"An empty address is no place to listen."};

    if (where.family() == Family::local && pipes::chosen()) {
        held_->gate = pipes::gated(where);
        held_->mine = std::move(where);

        return;
    }

    const Handle one = opened(where.family(), true);

    if (one == nothing)
        throw Trouble{"Cannot open a socket: " + spoken(amiss()) + ".", amiss(), where};

    held_->which = one;
    held_->numbered = one;

    // SO_REUSEADDR is about a port in the TIME_WAIT state, and a local socket has no port, so for
    // it the call means nothing and would only fail silently.
    if (where.family() != Family::local) sharing(one);

    if (!listening(one, where, waiting)) {
        const int why = amiss();

        held_->close();
        held_.reset();

        throw Trouble{"Cannot listen: " + spoken(why) + ".", why, where};
    }

    // The system may answer nothing, and we know the address anyway. With port zero the system
    // assigns the number, and without getsockname we would not know it at all, but for a local
    // socket the address is the path we gave ourselves. The system's silence must not erase it:
    // closing the gate removes the file by exactly this path.
    held_->mine = named(one, false);

    if (!held_->mine.held()) held_->mine = where;
}

namespace {

// The empty address returned once the handle is gone. All the other methods answer politely after
// close(), so this one must too; reaching for a missing handle would be a memory violation, and
// the class is meant to stay usable after close().
const Address& nowhere() {
    static const Address one;

    return one;
}

}

// The address the gate listens at, or an empty address after close().
const Address& Listener::mine() const { return held_ ? held_->mine : nowhere(); }

// Waits for the next connection and promises the stream. A closed gate, or one already accepting,
// gives a promise that has already failed with `Trouble`. A positive patience sets a deadline.
Promise<Stream> Listener::accept(Patience patience) const {
    if (!held_) return refused<Stream>("The gate is closed.");

    auto one = std::make_shared<MeetPromise>();

    // Taking the place and recording whose it is happen under one lock; otherwise two accepts
    // started at once would register one after the other, and the second would be left with a
    // place the first never released.
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->meeting != nullptr) return refused<Stream>("The gate is already accepting.", 0, held_->mine);

        held_->meeting = one.get();

        if (held_->gate) {
            held_->waiting = pipes::spare(*held_->gate);

            if (held_->waiting == nothing) {
                held_->meeting = nullptr;

                return refused<Stream>("The gate is closed.", 0, held_->mine);
            }

            one->instance = true;
            one->held = held_->waiting;
        }
    }

    one->mine = held_;
    one->kept = held_;

    if (!one->instance) one->held = held_->which.load();

    one->kind = Kind::meeting;
    one->ever = patience.count() > 0;
    one->until = std::chrono::steady_clock::now() + patience;

    Promise<Stream> made = handed<Stream>(one);

    submit(one);

    return made;
}

// Stops listening. Pending accepts are cancelled; the gate can still be asked questions afterward.
void Listener::close() {
    if (!held_) return;

    held_->close();
    held_.reset();
}

}

#include "talcite/udp.hpp"

#include <atomic>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "handing.hpp"
#include "inside.hpp"
#include "starting.hpp"

namespace talcite::net {

struct Udp::Held {
    // Closes the socket if it is still open and only then retires its number.
    ~Held() {
        close();

        retiring(numbered);
    }

    // Takes the handle away, so that no new work starts on it, and tells the hub to forget it.
    void close() {
        if (const Handle one = which.exchange(nothing); one != nothing) forget(one);
    }

    // The number outlives closing and goes away only together with the handle. Requests in
    // flight hold the handle through `kept`, so the descriptor is closed only when nobody can
    // submit it or read from it any more. Closed earlier, the number could come back as that of a
    // freshly opened socket, and someone would read from another connection.
    Handle numbered = nothing;

    std::atomic<Handle> which{nothing};
    Address mine;

    // Who holds each direction, not whether someone holds it; the reason and the whole pattern
    // are described with withdrawal in the stream. Raw pointers, because these are only identity
    // markers: we never reach the object through them. The requests live by their own handle in
    // the hub and through `kept`.
    std::mutex guard;
    const void* fetching = nullptr;
    const void* sending = nullptr;
};

namespace {

// A promise of `T` that has already failed with `Trouble`.
template <typename T>
Promise<T> amiss_now(const std::string& said, int code = 0, Address where = {}) {
    return refused<T>(said, code, std::move(where));
}

// A receive request: the hub fills `buffer` and `where`, and the promise gets the datagram.
struct FetchPromise : Handing<Datagram> {
    std::shared_ptr<Udp::Held> mine;

    // Frees the receiving direction, if this request still holds it.
    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->fetching == this) mine->fetching = nullptr;
    }

    // Withdraws the request from the hub and frees the direction. The withdrawal pattern, the
    // marker identity and the late completion are described with connecting in the stream.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    // The hub has received `much` bytes, or failed with `code`: settles the promise with the
    // datagram or with the trouble.
    void done(std::size_t much, int code) override {
        yielded();

        if (code != 0) {
            (void)settle->fail(std::make_exception_ptr(
                               Trouble{"Cannot receive: " + spoken(code) + ".", code,
                                       mine->mine}));

            return;
        }

        // Truncation is reported as a trouble instead of handing out a piece posing as the whole.
        if (cut) {
            (void)settle->fail(std::make_exception_ptr(
                               Trouble{"The datagram did not fit in the buffer.", 0, where}));

            return;
        }

        Datagram made;

        made.from = where;
        made.bytes = std::move(buffer);
        made.bytes.resize(much);

        (void)settle->value(std::move(made));
    }

    // The patience ran out: the promise fails with `Overdue`.
    void late() override {
        yielded();

        (void)settle->fail(std::make_exception_ptr(Overdue{}));
    }
};

// A send request: the hub sends `buffer` to `where`.
struct SendPromise : Handing<void> {
    std::shared_ptr<Udp::Held> mine;

    // Frees the sending direction, if this request still holds it.
    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->sending == this) mine->sending = nullptr;
    }

    // Withdraws the request from the hub and frees the direction, as in `FetchPromise`.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    // The hub has sent `much` bytes, or failed with `code`: settles the promise accordingly.
    void done(std::size_t much, int code) override {
        yielded();

        if (code != 0) {
            (void)settle->fail(std::make_exception_ptr(
                               Trouble{"Cannot send: " + spoken(code) + ".", code, where}));

            return;
        }

        // A datagram shorter than the one given does not happen; if it did, it would mean that a
        // packet nobody asked for went out. Better to say so plainly.
        if (much != buffer.size()) {
            (void)settle->fail(std::make_exception_ptr(Trouble{"The datagram went out truncated.", 0, where}));

            return;
        }

        (void)settle->value();
    }

    // The patience ran out: the promise fails with `Overdue`.
    void late() override {
        yielded();

        (void)settle->fail(std::make_exception_ptr(Overdue{}));
    }
};

}

// Opens a datagram socket bound to `where`. Like the gate, the post returns no promise: opening
// the socket and binding the port finish at once. An address without a port means "give me any",
// which is how a sending side is made.
Udp::Udp(Address where) : held_(std::make_shared<Held>()) {
    if (!where.held()) throw Trouble{"An empty address is no place for post."};

    // The post works with datagrams, and a local socket has none here: Windows does not offer
    // them at all, so we offer them nowhere. We refuse plainly instead of passing on a system
    // error number, which would not show that the thing is impossible by design rather than
    // failed this time.
    if (where.family() == Family::local)
        throw Trouble{"Udp does not work on a local socket — it is stream-only."};

    const Handle one = opened(where.family(), false);

    if (one == nothing)
        throw Trouble{"Cannot open a socket: " + spoken(amiss()) + ".", amiss(), where};

    held_->which = one;
    held_->numbered = one;

    sharing(one);

    if (!bound(one, where)) {
        const int why = amiss();

        held_->close();
        held_.reset();

        throw Trouble{"Cannot bind the address: " + spoken(why) + ".", why, where};
    }

    held_->mine = named(one, false);
}

namespace {

// The empty address returned when there is no handle any more. All the other methods answer
// politely after closing, so these must too: reaching for a missing handle would be a memory
// violation, and the class is meant to stay usable after close().
const Address& nowhere() {
    static const Address one;

    return one;
}

}

// The local address the socket is bound to, or an empty one after closing.
const Address& Udp::mine() const { return held_ ? held_->mine : nowhere(); }

// Receives one datagram of at most `most` bytes within `patience`. Only one receive may be in
// progress at a time; a larger datagram fails instead of arriving truncated.
Promise<Datagram> Udp::receive(std::size_t most, Patience patience) const {
    if (!held_) return amiss_now<Datagram>("The post is closed.");

    // The buffer is allocated before the marker is taken. The caller gives the size, so resize
    // can throw, and an exception thrown after taking the marker would leave it set forever: the
    // socket would never read again, since no request would exist to clear it.
    auto one = std::make_shared<FetchPromise>();

    one->buffer.resize(most);

    // Taking the direction and remembering whose it is happen under one lock; otherwise two
    // receives started at once would both record themselves, one after the other.
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->fetching != nullptr) return amiss_now<Datagram>("The post is already receiving.");

        held_->fetching = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::fetching;
    one->ever = patience.count() > 0;
    one->until = std::chrono::steady_clock::now() + patience;

    Promise<Datagram> made = handed<Datagram>(one);

    submit(one);

    return made;
}

// Sends `what` as one datagram to `to` within `patience`. Only one send may be in progress at a
// time.
Promise<void> Udp::send(Address to, Bytes what, Patience patience) const {
    if (!held_) return amiss_now<void>("The post is closed.");

    if (!to.held()) return amiss_now<void>("An empty address is no destination.");

    auto one = std::make_shared<SendPromise>();

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->sending != nullptr) return amiss_now<void>("The post is already sending.");

        held_->sending = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::sending;
    one->where = std::move(to);
    one->buffer = std::move(what);
    one->ever = patience.count() > 0;
    one->until = std::chrono::steady_clock::now() + patience;

    Promise<void> made = handed<void>(one);

    submit(one);

    return made;
}

// Allows broadcast sending, only for this socket and only on request (a beacon in the local
// network). Without it, sending to 255.255.255.255 is refused by the system.
void Udp::broadcast(bool given) const {
    if (!held_) return;

    broadcasting(held_->which.load(), given);
}

// Closes the socket; afterwards every method answers as on a closed post.
void Udp::close() {
    if (!held_) return;

    held_->close();
    held_.reset();
}

}

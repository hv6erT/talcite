#include "talcite/stream.hpp"

#include <atomic>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "handing.hpp"
#include "inside.hpp"
#include "piping.hpp"
#include "starting.hpp"
#include "netting.hpp"

namespace talcite::net {

// The state of a stream shared by its copies and by the operations in flight.
struct Stream::Held {
    // The last owner closes the stream and only then retires the handle number.
    ~Held() {
        close();

        retiring(numbered);
    }

    // Closes the stream once. Closing must happen only once: a handle given to the hub twice would
    // be closed twice, and the second close would hit someone else's freshly opened socket with
    // the same number.
    void close() {
        const Handle one = which.exchange(nothing);

        if (one == nothing) return;

        forget(one);

        // A pipe cannot be half-closed: there is no equivalent of shutdown, and the other side
        // sees the end only when the handle is closed. So it is retired right now, not at the last
        // copy: the hub closes it once the completions in flight have returned. There will be no
        // new requests on this number (`which` is empty), and a write to a pipe does not return in
        // pieces, so nothing resumes it.
        if (pipes::piped(one)) retiring(std::exchange(numbered, nothing));
    }

    // The number outlives the close and dies only together with this state. Requests in flight
    // hold the state through `kept`, so the descriptor is closed only when nobody can submit it or
    // read on it any more. Closed earlier, the number could come back as that of a freshly opened
    // socket, and someone would read from another connection.
    Handle numbered = nothing;

    std::atomic<Handle> which{nothing};
    Address peer;
    Address mine;

    // Who holds each direction, not whether someone holds it; the reason is described below, at the
    // withdrawal pattern. Raw pointers, because they are only identity markers: we never reach the
    // object through them. Requests live by their own handle in the hub and through `kept`.
    std::mutex guard;
    const void* reading = nullptr;
    const void* writing = nullptr;
};

namespace {

// The deadline that `patience` sets from now.
std::chrono::steady_clock::time_point until(Patience patience) {
    return std::chrono::steady_clock::now() + patience;
}

// A promise already rejected because the direction is busy. One read and one write at a time: this
// is not thrift but protection, because two reads on one stream would interleave the bytes and
// derail everything after them. The second request is refused instead of silently corrupting
// data.
template <typename T>
Promise<T> busy() {
    return refused<T>("The stream is already busy in that direction.");
}

// A promise already rejected with `Trouble` carrying the message, code and address.
template <typename T>
Promise<T> amiss_now(const std::string& said, int code = 0, Address where = {}) {
    return refused<T>(said, code, std::move(where));
}

// A network request is at the same time the promise's ticket, so cancelling has to withdraw it
// from the hub, not just reject it. The pattern is the same in all six.
//
// Cancelling frees the direction at once, regardless of what `withdraw` said, because that is the
// only moment the caller learns of it. Right after cancelling, the caller may ask for the next read
// and must get it, not hear "busy".
//
// The two backends finish differently here. On Unix `withdraw` takes the request out of the map
// and returns true, so freeing on the spot is obvious. On Windows CancelIoEx only asks (the
// completion still arrives through the port), so `withdraw` never returns true, and the marker
// would be cleared only by the late `done()` on a pool thread. Until then the direction would stay
// busy and the next request would be refused for no reason.
//
// That is why the marker is not a flag but an identity: only whoever took it releases it. The late
// completion of a cancelled operation then finds another identity and touches nothing; otherwise
// it would clear the marker that already belongs to the next operation.
//
// A late completion of a cancelled request finds its promise already ended, and settling it then
// changes nothing: the caller keeps `Cancelled`, not a `Trouble` with the abort error number.
struct JoinPromise : Handing<Stream> {
    std::shared_ptr<Stream::Held> mine;

    // Withdraws an abandoned connect from the hub. Connecting has no direction marker, yet it must
    // be withdrawn: a request lying in the hub holds `kept`, that is, an open socket. An abandoned
    // connect without a deadline would leave it open until the program ends.
    void dropped() override { (void)withdraw(shared_from_this()); }

    // The connect finished: the stream with both addresses, or `Trouble` with the error.
    void done(std::size_t, int code) override {
        if (code != 0) {
            (void)settle->fail(std::make_exception_ptr(Trouble{"Cannot connect: " + spoken(code) + ".", code, mine->peer}));

            return;
        }

        // We know the peer's address from the start: it is the one we connected to. We ask the
        // system, because with a domain name or a dual-stack family it may turn out different,
        // but its silence must not erase what we already know.
        if (const Address said = named(held, true); said.held()) mine->peer = said;

        mine->mine = named(held, false);

        (void)settle->value(Stream{mine});
    }

    // The deadline passed before the connection was made: rejects with `Overdue`.
    void late() override { (void)settle->fail(std::make_exception_ptr(Overdue{})); }
};

// One read in flight.
struct ReadPromise : Handing<Bytes> {
    std::shared_ptr<Stream::Held> mine;

    // Releases the read direction, but only if this read still holds it.
    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->reading == this) mine->reading = nullptr;
    }

    // Withdraws an abandoned read from the hub and frees the direction at once.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    // The read finished. The bytes that arrived matter more than how the operation ended. With
    // IOCP an interrupted read may already have copied part of the content; returning it instead
    // of an exception means no byte is lost and the stream never enters an unknown state.
    //
    // Cancelling is the only exception, and a deliberate one: whoever interrupted the read said the
    // content no longer matters, and the received bytes have nowhere to go.
    void done(std::size_t much, int code) override {
        yielded();

        if (much > 0) {
            buffer.resize(much);

            (void)settle->value(std::move(buffer));

            return;
        }

        if (code != 0) {
            (void)settle->fail(std::make_exception_ptr(Trouble{"Cannot read: " + spoken(code) + ".", code, mine->peer}));

            return;
        }

        // Zero bytes without an error means exactly one thing: the other side has closed.
        buffer.clear();

        (void)settle->value(std::move(buffer));
    }

    // A deadline with empty hands rejects with `Overdue`. If it resolved with nothing, silence
    // could not be told from the other side closing, and those are two entirely different messages.
    void late() override {
        yielded();

        (void)settle->fail(std::make_exception_ptr(Overdue{}));
    }
};

// One write in flight.
struct WritePromise : Handing<void> {
    std::shared_ptr<Stream::Held> mine;

    // Releases the write direction, but only if this write still holds it.
    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->writing == this) mine->writing = nullptr;
    }

    // Withdraws an abandoned write from the hub and frees the direction at once.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    // The write finished: resolves, or rejects with `Trouble` saying how much was written.
    void done(std::size_t much, int code) override {
        yielded();

        if (code != 0) {
            // How many bytes got out before the break is said explicitly. The buffer was moved and
            // is gone, so without this number the caller does not know whether to resume from zero
            // or not at all, and resending everything means doubled content for the other side.
            (void)settle->fail(std::make_exception_ptr(Trouble{"Cannot write: " + spoken(code) + ". Took " + std::to_string(much) +
                                                                   " of " + std::to_string(buffer.size()) + " bytes.",
                                                               code, mine->peer}));

            return;
        }

        (void)settle->value();
    }

    // The deadline passed before the write finished: rejects with `Overdue`.
    void late() override {
        yielded();

        (void)settle->fail(std::make_exception_ptr(Overdue{}));
    }
};

}

// Connects to `where` within `patience`. The connect does not start here. On Unix it is a
// non-blocking connect, on Windows ConnectEx with completion through the port, and that is
// knowledge of the backend, not of the stream. We submit the intent, and the hub knows how to set
// it going. A local address that goes through a pipe is connected by the pool (`pipes::dialed`):
// a pipe has no overlapped connect.
Promise<Stream> Stream::connect(Address where, Patience patience) {
    if (!where.held()) return amiss_now<Stream>("An empty address is no destination.");

    if (where.family() == Family::local && pipes::chosen()) return pipes::dialed(std::move(where), patience);

    auto mine = std::make_shared<Held>();

    mine->which = opened(where.family(), true);
    mine->numbered = mine->which.load();

    if (mine->which.load() == nothing) return amiss_now<Stream>("Cannot open a socket: " + spoken(net::amiss()) + ".",
                                 net::amiss(), where);

    mine->peer = where;

    auto one = std::make_shared<JoinPromise>();

    one->mine = mine;
    one->kept = mine;
    one->held = mine->which.load();
    one->kind = Kind::joining;
    one->where = where;
    one->ever = patience.count() > 0;
    one->until = until(patience);

    Promise<Stream> made = handed<Stream>(one);

    submit(one);

    return made;
}

namespace {

}

// Wraps a connection accepted by the gate. The gate has a bare handle, but only this file knows
// `Held`, hence a free function instead of a method. The public `Stream` constructor harms nobody:
// `Held` is an incomplete type, so it cannot be built from outside.
Stream greeting(Handle one, Address from) {
    auto mine = std::make_shared<Stream::Held>();

    mine->which = one;
    mine->numbered = one;
    mine->peer = std::move(from);
    mine->mine = named(one, false);

    return Stream{mine};
}

// The address of the other side, or an empty one after closing.
const Address& Stream::peer() const { return held_ ? held_->peer : nowhere(); }

// The address of our side, or an empty one after closing.
const Address& Stream::mine() const { return held_ ? held_->mine : nowhere(); }

// Reads up to `most` bytes within `patience`; an empty result means the other side has closed.
Promise<Bytes> Stream::read(std::size_t most, Patience patience) const {
    if (!held_) return amiss_now<Bytes>("The stream is closed.");

    // A zero-length read is refused, because it would look like the end of the stream: recv with
    // zero length returns zero on a healthy socket too, and zero without an error means "the other
    // side has closed" here. A framing layer can hit this at a frame boundary.
    if (most == 0) return amiss_now<Bytes>("A zero-length read says nothing.");

    // The buffer is allocated before the marker is taken. The caller gives the size, so resize can
    // throw, and an exception thrown after taking the marker would leave it set forever: the socket
    // would never read again, because no request would exist to clear it.
    auto one = std::make_shared<ReadPromise>();

    one->buffer.resize(most);

    // Taking the direction and recording whose it is happen under one lock; otherwise two reads
    // started at once would each record themselves, one after the other.
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->reading != nullptr) return busy<Bytes>();

        held_->reading = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::getting;
    one->ever = patience.count() > 0;
    one->until = until(patience);

    Promise<Bytes> made = handed<Bytes>(one);

    submit(one);

    return made;
}

// Writes all the bytes within `patience`.
Promise<void> Stream::write(Bytes what, Patience patience) const {
    if (!held_) return amiss_now<void>("The stream is closed.");

    auto one = std::make_shared<WritePromise>();

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->writing != nullptr) return busy<void>();

        held_->writing = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::putting;
    one->buffer = std::move(what);
    one->ever = patience.count() > 0;
    one->until = until(patience);

    Promise<void> made = handed<void>(one);

    submit(one);

    return made;
}

// Whether the other side runs as the same user on this computer. Only a local socket can say so;
// about a network connection the system knows nothing of the kind, so the answer is nothing.
std::optional<bool> Stream::kindred() const {
    // An accepted local connection has no name on the other side; our end tells the family then.
    if (!held_ || (held_->peer.family() != Family::local && held_->mine.family() != Family::local)) return std::nullopt;

    return net::kindred(held_->which.load());
}

// Closes the stream; this copy becomes empty, and operations in flight finish on their own.
void Stream::close() {
    if (!held_) return;

    held_->close();
    held_.reset();
}

}

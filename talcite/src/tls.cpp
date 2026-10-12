#include "talcite/tls.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "outbox.hpp"

namespace talcite::net {

TlsTrouble::TlsTrouble(Why why, const std::string& said, std::string host, Address where)
    : Trouble(said, 0, std::move(where)), why_(why), host_(std::move(host)) {}

struct TlsStream::Held {
    Stream one;
    std::unique_ptr<TlsSession> how;

    // The name under which the client asked for the server, for handshake troubles, because a
    // person knows a server by its name, not by its address. Empty on the server side.
    std::string host;

    // The session is not thread-safe and has no reason to be: we touch it only in short, whole
    // portions ("push and take", "write and take"). The lock is held only for such a portion,
    // never across an asynchronous step.
    std::mutex minding;

    Bytes spare;

    // Everything that goes out to the socket lines up in one queue, because the stream accepts
    // one write at a time, and the handshake can write in the middle of a read.
    Outbox outbox;

    std::atomic<bool> reading{false};
    std::atomic<bool> writing{false};
};

namespace {

// A promise rejected with `Trouble` and the given text.
template <typename T>
Promise<T> refused(const char* said) {
    return failed<T>(std::make_exception_ptr(Trouble{said}));
}

// The session hands out ciphertext in portions, so we take everything it has.
Bytes drained(TlsSession& how) {
    Bytes made;

    for (;;) {
        Bytes one = how.pull();

        if (one.empty()) return made;

        made.insert(made.end(), one.begin(), one.end());
    }
}

// Throws the session's trouble as `TlsTrouble`: with the reason the adapter recognized (ciphers,
// trust, expiry, name), the server name and the address of the other side. It is called under
// `minding`, so reading the stream here is safe too.
void amiss(const TlsStream::Held& held) {
    if (const std::string said = held.how->error(); !said.empty())
        throw TlsTrouble{held.how->why(), said, held.host, held.one.peer()};
}

// A copy of the stream taken under the lock. `Stream` itself is a value holding a `shared_ptr`,
// so the copy points at the same object and costs one counter increment. The reason:
// `TlsStream::close()` calls a method on the member `held->one` that reassigns its `shared_ptr`,
// and that is a plain pointer, not an atomic one. A pool worker reading the same member at the
// same moment would be an unsynchronized read next to a write, which is UB: a torn read of the
// pointer, or a decrement of the counter of a block that the other thread is just freeing.
//
// Since every touch of the member goes under `minding`, asynchronous work runs on a private copy
// and the lock is not held across any asynchronous step, so the rule written above `minding`
// holds. Closing the original still reaches the copy, because both share one `Stream::Held`: the
// handle is swapped for an empty one, so the copy gets a clean rejection on its next use, not a
// hang.
Stream borrowed(const std::shared_ptr<TlsStream::Held>& held) {
    const std::lock_guard<std::mutex> kept{held->minding};

    return held->one;
}

// Puts the ciphertext into the common write queue, or gives a settled promise when there is none.
// The caller holds `minding`: the session hands out its records numbered and in order, and they
// must join the queue in the same order. Queued after the lock is released, the records of a
// write and of a read draining the session at the same time on two threads could swap places, and
// the other side would refuse the connection. The writer is started afterwards, without the lock
// (`Outbox::flush`).
Promise<void> queued(const std::shared_ptr<TlsStream::Held>& held, Bytes out, Patience patience) {
    if (out.empty()) return ready();

    return held->outbox.queued(std::move(out), patience, [held] { return borrowed(held); });
}

// Takes at most `most` bytes of the plaintext already decrypted and kept in `spare`.
Bytes taken(TlsStream::Held& held, std::size_t most) {
    Bytes made;

    if (held.spare.size() <= most) {
        made = std::move(held.spare);

        held.spare.clear();

        return made;
    }

    made.assign(held.spare.begin(), held.spare.begin() + static_cast<std::ptrdiff_t>(most));

    held.spare.erase(held.spare.begin(), held.spare.begin() + static_cast<std::ptrdiff_t>(most));

    return made;
}

using Clock = std::chrono::steady_clock;

// The time left until `until`, at least one millisecond, so that a deadline just passed still
// gives a valid patience.
Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// How many handshake rounds may run. A real handshake fits in a few; a hundred leaves a huge
// margin. The limit exists because every round keeps the previous one alive: a then chain holds
// its whole history, so a peer that kept answering just before the deadline would add tens of
// kilobytes per round, without end.
constexpr int rounds = 100;

// The handshake. We ask the session what it has to send, send it, read the answer, push it in,
// and start over until the session says it is ready. Calling pull() is what drives the handshake:
// its first question starts the whole exchange.
//
// The deadline is a moment, not a patience. A patience given to each round separately would be a
// per-operation deadline, renewable forever, so the handshake as a whole would have no end.
Promise<void> turning(const std::shared_ptr<TlsStream::Held>& held, Clock::time_point until,
                      int turn) {
    Promise<void> sent;
    bool done = false;

    {
        const std::lock_guard<std::mutex> kept{held->minding};

        Bytes out = drained(*held->how);
        done = held->how->ready();

        amiss(*held);

        sent = queued(held, std::move(out), left(until));
    }

    held->outbox.flush();

    if (done) return sent;

    if (turn >= rounds) throw Trouble{"The handshake will not finish."};

    return std::move(sent).then([held, until, turn] {
        return borrowed(held).read(65536, left(until)).then([held, until, turn](const Bytes& came) {
            if (came.empty()) throw Trouble{"The handshake broke off halfway."};

            {
                const std::lock_guard<std::mutex> kept{held->minding};

                held->how->push(came);

                amiss(*held);
            }

            return turning(held, until, turn + 1);
        });
    });
}

// Reads at most `most` bytes of plaintext: first what is left over, then what the session can
// decrypt, and only then from the socket, sending on the way whatever the session wants to send.
// An empty result means the other side has closed.
Promise<Bytes> reading(const std::shared_ptr<TlsStream::Held>& held, std::size_t most,
                       Patience patience) {
    Bytes plain;
    Promise<void> sent;

    {
        const std::lock_guard<std::mutex> kept{held->minding};

        if (!held->spare.empty()) return ready(taken(*held, most));

        plain = held->how->read();
        Bytes out = drained(*held->how);

        amiss(*held);

        if (!plain.empty()) {
            held->spare.insert(held->spare.end(), plain.begin(), plain.end());

            plain = taken(*held, most);
        }

        sent = queued(held, std::move(out), patience);
    }

    held->outbox.flush();

    if (!plain.empty()) return std::move(sent).then([plain = std::move(plain)]() mutable {
        return std::move(plain);
    });

    return std::move(sent).then([held, most, patience] {
        return borrowed(held).read(65536, patience).then([held, most, patience](const Bytes& came) {
            // Emptiness means the same as on a bare stream: the other side has closed.
            if (came.empty()) return ready(Bytes{});

            {
                const std::lock_guard<std::mutex> kept{held->minding};

                held->how->push(came);

                amiss(*held);
            }

            return reading(held, most, patience);
        });
    });
}

// Takes the stream by value and never gives it back. Once it is wrapped, nobody may touch the
// socket underneath; the handshake would end up in the middle of someone else's data.
//
// A missing session ends in a rejection with a reason, so that an empty session never reaches
// the handshake through a null pointer.
Promise<TlsStream> opened(Stream one, std::unique_ptr<TlsSession> how, std::string host, Patience patience) {
    if (!how) return refused<TlsStream>("The provider gave no session.");

    auto held = std::make_shared<TlsStream::Held>();

    held->one = std::move(one);
    held->how = std::move(how);
    held->host = std::move(host);

    return turning(held, Clock::now() + patience, 0).then([held] { return TlsStream{held}; });
}

// A missing provider rejects the promise instead of throwing at the caller: a function that
// returns a promise reports errors the same way as the rest of the module. The type is the same
// one `Seam::provider()` throws.
Promise<TlsStream> unbound() {
    return failed<TlsStream>(std::make_exception_ptr(Unbound{TlsProvider::kind()}));
}

}

// Wraps `one` as the client side of a TLS connection to `host` and runs the handshake within
// `patience`.
Promise<TlsStream> TlsStream::client(Stream one, Seam<TlsProvider> tls, std::string host, Patience patience) {
    if (!tls.bound()) return unbound();

    std::unique_ptr<TlsSession> how = tls.provider().client(host);

    return opened(std::move(one), std::move(how), std::move(host), patience);
}

// Wraps `one` as the server side of a TLS connection and runs the handshake within `patience`.
Promise<TlsStream> TlsStream::server(Stream one, Seam<TlsProvider> tls, Patience patience) {
    if (!tls.bound()) return unbound();

    return opened(std::move(one), tls.provider().server(), {}, patience);
}

namespace {

// An empty handle gives an empty address, not a memory violation: the same rule and the same
// reason as in the stream. `close()` clears the handle, and the class is meant to stay usable
// after closing.
const Address& nowhere() {
    static const Address one;

    return one;
}

}

// The address of the other side. These readers touch the same member, so they work under the
// lock too. The returned reference leads into `Stream::Held`, which lives as long as our `Held`,
// and the caller holds that.
const Address& TlsStream::peer() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.peer();
}

// The local address of the connection, under the same lock and with the same lifetime as `peer`.
const Address& TlsStream::mine() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.mine();
}

// Reads at most `most` bytes of plaintext within `patience`. Only one read may be in progress at
// a time; an empty result means the other side has closed.
Promise<Bytes> TlsStream::read(std::size_t most, Patience patience) const {
    if (!held_) return refused<Bytes>("The connection is closed.");

    bool was = false;

    if (!held_->reading.compare_exchange_strong(was, true)) return refused<Bytes>("Already reading.");

    auto held = held_;

    // `reading` can throw synchronously; it always does when the session already has a trouble
    // recorded. An exception let out from here would skip `finally`, leave the flag set, and the
    // connection would never read again. A function that returns a promise reports errors through
    // the promise, not by throwing.
    try {
        return reading(held, most, patience).finally([held] { held->reading.store(false); });
    } catch (...) {
        held->reading.store(false);

        return failed<Bytes>(std::current_exception());
    }
}

// Encrypts `what` and sends it within `patience`. Only one write may be in progress at a time;
// the ciphertext joins the common queue, so it never interleaves with the handshake.
Promise<void> TlsStream::write(Bytes what, Patience patience) const {
    if (!held_) return refused<void>("The connection is closed.");

    bool was = false;

    if (!held_->writing.compare_exchange_strong(was, true)) return refused<void>("Already writing.");

    auto held = held_;

    Promise<void> sent;

    try {
        const std::lock_guard<std::mutex> kept{held->minding};

        held->how->write(std::move(what));

        Bytes out = drained(*held->how);

        amiss(*held);

        sent = queued(held, std::move(out), patience);
    } catch (...) {
        held->writing.store(false);

        return failed<void>(std::current_exception());
    }

    held->outbox.flush();

    return std::move(sent).finally([held] { held->writing.store(false); });
}

// Closes the session and the stream under the same lock as the session, not because closing is
// dangerous in itself, but because it reassigns a `shared_ptr` in a member that a pool worker may
// be reading at the same moment. Since readers take their copy under `minding`, it is enough that
// the write happens under it too.
void TlsStream::close() {
    if (!held_) return;

    {
        const std::lock_guard<std::mutex> kept{held_->minding};

        held_->how->close();
        held_->one.close();
    }

    held_.reset();
}

}

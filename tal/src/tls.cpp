#include "tal/tls.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "tal/executor.hpp"

// TLS over any `Stream`, through the port `Tls`: the adapter's session turns plaintext into
// records and back, and this file carries the records. Every record goes out through one queue in
// the order the session made it, since the handshake, a read and a write may all have some to
// send; whoever finds the queue idle writes it, the others wait for their bytes to be out. One
// read and one write at a time. The end of the other side without its close is an error, as it
// may be someone cutting the connection short; our own close says goodbye, for a second at most.
namespace tal::net {

namespace {

// What a secure connection shares with its tasks.
struct Secure {
    Stream plain;
    std::unique_ptr<Session> session;
    std::string host;
    std::mutex guard;
    Bytes spare;
    Bytes queue;
    std::uint64_t queued = 0;
    std::uint64_t sent = 0;
    bool flushing = false;
    std::vector<Pending<void>> waiting;
    std::exception_ptr broke;
    bool reading = false;
    bool writing = false;
    bool shut = false;
};

// Throws the session's trouble, with the name of the server asked for; called under the lock.
void checked(const Secure& held) {
    const std::optional<Trouble> why = held.session->failed();

    if (why) throw Trouble{why->kind(), why->what(), held.host.empty() ? why->where() : held.host, why->code()};
}

// All the session has to send now; called under the lock.
Bytes drained(Session& session) {
    Bytes made;

    for (Bytes one = session.pull(); !one.empty(); one = session.pull()) made.insert(made.end(), one.begin(), one.end());

    return made;
}

// Puts `out` in the queue after what is there; gives how many bytes will have gone out once it
// has, 0 for nothing. Called under the lock, so records keep the session's order.
std::uint64_t enqueued(Secure& held, const Bytes& out) {
    if (out.empty()) return 0;

    held.queue.insert(held.queue.end(), out.begin(), out.end());
    held.queued += out.size();

    return held.queued;
}

// The writer of the queue while it writes. Ended, failed or cancelled, it lets the next one in and
// wakes the waiting; a writer that did not finish breaks the connection, which no longer knows what
// the other side got.
class Flushing {
public:
    // The writer for `held`.
    explicit Flushing(std::shared_ptr<Secure> held) : held_(std::move(held)) {}

    // Lets go, broken unless `done` came first.
    ~Flushing() {
        std::vector<Pending<void>> woken;

        {
            const std::lock_guard<std::mutex> kept{held_->guard};

            if (!finished_ && !held_->broke) held_->broke = std::make_exception_ptr(Trouble{Kind::broken, "A write of the secure connection was cut off.", held_->host});

            held_->flushing = false;
            woken.swap(held_->waiting);
        }

        for (Pending<void>& one : woken) one.settle();
    }

    Flushing(const Flushing&) = delete;
    Flushing& operator=(const Flushing&) = delete;

    // All up to `upto` bytes went out.
    void done(std::uint64_t upto) {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->sent = upto;
        finished_ = true;
    }

private:
    std::shared_ptr<Secure> held_;
    bool finished_ = false;
};

// Waits until the queue is out up to `ticket` bytes, writing it when nobody does.
Task<void> flushed(std::shared_ptr<Secure> held, std::uint64_t ticket) {
    for (;;) {
        Bytes batch;
        std::uint64_t upto = 0;
        std::optional<Pending<void>> waiting;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->broke) std::rethrow_exception(held->broke);

            if (held->sent >= ticket) co_return;

            if (held->flushing) {
                waiting.emplace();
                held->waiting.push_back(*waiting);
            } else {
                held->flushing = true;
                batch.swap(held->queue);
                upto = held->queued;
            }
        }

        if (waiting) {
            co_await waiting->task();

            continue;
        }

        Flushing writer{held};

        co_await held->plain.write(std::move(batch));
        writer.done(upto);
    }
}

// Takes at most `most` bytes of the plaintext kept in `spare`.
Bytes taken(Bytes& spare, std::size_t most) {
    if (spare.size() <= most) return std::exchange(spare, {});

    Bytes made(spare.begin(), spare.begin() + static_cast<std::ptrdiff_t>(most));

    spare.erase(spare.begin(), spare.begin() + static_cast<std::ptrdiff_t>(most));

    return made;
}

// A direction of the connection taken by a task while it waits; a second is refused as busy.
class Marking {
public:
    // Takes `slot` of `held`, or throws `Trouble` saying `busy`.
    Marking(Secure& held, bool& slot, const char* busy) : held_(held), slot_(slot) {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (slot_) throw Trouble{Kind::busy, busy};

        slot_ = true;
    }

    // Lets the direction go.
    ~Marking() {
        const std::lock_guard<std::mutex> kept{held_.guard};

        slot_ = false;
    }

    Marking(const Marking&) = delete;
    Marking& operator=(const Marking&) = delete;

private:
    Secure& held_;
    bool& slot_;
};

// The tasks that say goodbye for connections closed, on the io pool.
Scope& leaving() {
    static Scope one{io()};

    return one;
}

// At most `most` bytes of plaintext: what is left over, what the session has, and only then what
// comes from the other side, sending on the way what the session wants sent. Nothing at the end
// the other side said; the end without it is broken.
Task<Bytes> reading(std::shared_ptr<Secure> held, std::size_t most) {
    if (most == 0) throw Trouble{Kind::other, "A read of nothing says nothing."};

    const Marking marking{*held, held->reading, "The connection has a reader already."};

    for (;;) {
        Bytes made;
        bool ended = false;
        std::uint64_t ticket = 0;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->shut) throw Trouble{Kind::broken, "The connection is closed.", held->host};

            if (held->spare.empty()) {
                Bytes plain = held->session->read();
                const Bytes out = drained(*held->session);

                checked(*held);

                held->spare.insert(held->spare.end(), plain.begin(), plain.end());
                ticket = enqueued(*held, out);
            }

            made = taken(held->spare, most);
            ended = made.empty() && held->session->closed();
        }

        if (ticket != 0) co_await flushed(held, ticket);

        if (!made.empty() || ended) co_return made;

        const Bytes came = co_await held->plain.read(65536);
        const std::lock_guard<std::mutex> kept{held->guard};

        if (came.empty()) throw Trouble{Kind::broken, "The secure connection ended without saying so.", held->host};

        held->session->push(came);
        checked(*held);
    }
}

// Sends `what`, waiting until its records are out.
Task<void> writing(std::shared_ptr<Secure> held, Bytes what) {
    const Marking marking{*held, held->writing, "The connection has a writer already."};
    std::uint64_t ticket = 0;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->shut) throw Trouble{Kind::broken, "The connection is closed.", held->host};

        held->session->write(std::move(what));

        const Bytes out = drained(*held->session);

        checked(*held);
        ticket = enqueued(*held, out);
    }

    if (ticket != 0) co_await flushed(held, ticket);
}

// Says goodbye and closes the stream under it: the session's close goes out on a task of its own,
// for a second at most of the program's loop; once that loop is gone, until its write ends.
void closing(const std::shared_ptr<Secure>& held) {
    std::uint64_t ticket = 0;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->shut) return;

        held->shut = true;
        held->session->close();
        ticket = enqueued(*held, drained(*held->session));
    }

    leaving().run([](std::shared_ptr<Secure> one, std::uint64_t upto) -> Task<void> {
        try {
            if (upto != 0) co_await within(std::chrono::seconds{1}, flushed(one, upto));
        } catch (...) {
        }

        one->plain.close();
    }(held, ticket));
}

// A secure connection as a channel of a `Stream`; its last stream closes it.
class Secured final : public Channel {
public:
    // The connection `held`, its handshake done.
    explicit Secured(std::shared_ptr<Secure> held) : held_(std::move(held)) {}

    // The last stream is gone.
    ~Secured() override { closing(held_); }

    Secured(const Secured&) = delete;
    Secured& operator=(const Secured&) = delete;

    // At most `most` bytes of plaintext.
    Task<Bytes> read(std::size_t most) override { return reading(held_, most); }

    // All of `what`.
    Task<void> write(Bytes what) override { return writing(held_, std::move(what)); }

    // Says goodbye and closes.
    void close() override { closing(held_); }

private:
    std::shared_ptr<Secure> held_;
};

// The handshake of `session` over `plain`, for the server `host` (empty on the server's side): send
// what the session has, read what comes, give it to the session, until it is ready. Its own
// deadline is the caller's (`within`).
Task<Stream> opened(Stream plain, std::unique_ptr<Session> session, std::string host) {
    if (session == nullptr) throw Trouble{Kind::broken, "The adapter gave no session.", host};

    auto held = std::make_shared<Secure>();

    held->plain = std::move(plain);
    held->session = std::move(session);
    held->host = std::move(host);

    for (;;) {
        bool ready = false;
        std::uint64_t ticket = 0;

        {
            const std::lock_guard<std::mutex> kept{held->guard};
            const Bytes out = drained(*held->session);

            checked(*held);
            ready = held->session->ready();
            ticket = enqueued(*held, out);
        }

        if (ticket != 0) co_await flushed(held, ticket);

        if (ready) break;

        const Bytes came = co_await held->plain.read(65536);
        const std::lock_guard<std::mutex> kept{held->guard};

        if (came.empty()) throw Trouble{Kind::broken, "The handshake broke off halfway.", held->host};

        held->session->push(came);
        checked(*held);
    }

    co_return Stream{std::make_shared<Secured>(std::move(held))};
}

}

// `plain` made secure as the client of the server `host`, whose certificate must bear that name.
Task<Stream> secured(Stream plain, std::string host, Port<Tls> tls) {
    std::unique_ptr<Session> session = tls->client(host);

    co_return co_await opened(std::move(plain), std::move(session), std::move(host));
}

// `plain` made secure as the server, with the adapter's identity.
Task<Stream> serving(Stream plain, Port<Tls> tls) { co_return co_await opened(std::move(plain), tls->server(), {}); }

#ifndef _WIN32

// No TLS of the system outside Windows; an adapter brings one (OpenSSL).
std::shared_ptr<Tls> Tls::builtin() { return nullptr; }

// Nor with credentials.
std::shared_ptr<Tls> Tls::builtin(Credentials given) {
    (void)given;

    return nullptr;
}

#endif

}

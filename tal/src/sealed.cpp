#include "tal/peer.hpp"

#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "tal/trouble.hpp"

// A channel of sealed messages over any stream, ported from talcite's channel.cpp: first a key
// agreement (any `Handshake`), then messages sealed with AES-256-GCM from the cipher's port. Every
// message is a frame: four bytes of its length, most significant first, then the content. The
// frames of the handshake go in the clear, guarded by the handshake itself; the frames of messages
// are sealed with their length header as the additional data, so a changed length is refused. The
// nonce is the count of messages in that direction (four zero bytes and eight of the count), so a
// message repeated, reordered or lost breaks the seal of the next. The channel speaks in messages,
// not bytes: exactly what was sent comes out on the other side.
//
// Frames go out through one queue in the order they were sealed, as the records of TLS do: whoever
// finds it idle writes it, the others wait for their bytes. One reader at a time. A forged frame or
// a failed handshake closes the stream, since the counts already went apart and the other side
// should know at once.
namespace tal::net {

namespace {

// The longest frame: the longest message and its tag. A longer announcement breaks the channel; a
// stranger may not make us keep gigabytes.
constexpr std::uint32_t biggest = Sealed::longest + 16;

// The four bytes of the length `much`, most significant first.
Bytes headed(std::size_t much) {
    Bytes made(4);

    for (std::size_t at = 0; at < 4; ++at) made[at] = static_cast<std::byte>((much >> (24 - 8 * at)) & 0xff);

    return made;
}

// The nonce of the message `counter`: four zero bytes, then the count, most significant first.
Bytes nonce(std::uint64_t counter) {
    Bytes made(12);

    for (std::size_t at = 0; at < 8; ++at) made[4 + at] = static_cast<std::byte>((counter >> (56 - 8 * at)) & 0xff);

    return made;
}

// `what` after its length.
Bytes frame(const Bytes& what) {
    Bytes made = headed(what.size());

    made.insert(made.end(), what.begin(), what.end());

    return made;
}

}

// What a channel's tasks share.
struct Sealed::Held {
    Stream stream;
    std::shared_ptr<crypto::Cipher> cipher;
    Keying keys;
    std::mutex guard;
    Bytes spare;
    std::size_t start = 0;
    std::uint64_t sent = 0;
    std::uint64_t taken = 0;
    bool reading = false;
    bool shut = false;
    Bytes queue;
    std::uint64_t queued = 0;
    std::uint64_t out = 0;
    bool flushing = false;
    std::vector<Pending<void>> waiting;
    std::exception_ptr broke;

    // Wipes the keys.
    ~Held() {
        crypto::wiped(keys.sending);
        crypto::wiped(keys.receiving);
    }
};

namespace {

using Kept = std::shared_ptr<Sealed::Held>;

// The trouble of a channel that is closed.
Trouble gone() { return Trouble{Kind::broken, "The channel is closed."}; }

// Closes the stream of `held` for good; called under its lock.
void shutting(Sealed::Held& held) {
    held.shut = true;
    held.stream.close();
}

// Puts `out` in the queue after what is there; how many bytes will have gone once it has. Called
// under the lock, so frames keep the order of their seals.
std::uint64_t enqueued(Sealed::Held& held, const Bytes& out) {
    held.queue.insert(held.queue.end(), out.begin(), out.end());
    held.queued += out.size();

    return held.queued;
}

// The writer of the queue while it writes; ended or cut off, it lets the next one in and wakes the
// waiting, and a write cut off breaks the channel, which no longer knows what the other side got.
class Flushing {
public:
    // The writer for `held`.
    explicit Flushing(Kept held) : held_(std::move(held)) {}

    // Lets go, broken unless `done` came first.
    ~Flushing() {
        std::vector<Pending<void>> woken;

        {
            const std::lock_guard<std::mutex> kept{held_->guard};

            if (!finished_ && !held_->broke) held_->broke = std::make_exception_ptr(Trouble{Kind::broken, "A write of the channel was cut off."});

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

        held_->out = upto;
        finished_ = true;
    }

private:
    Kept held_;
    bool finished_ = false;
};

// Waits until the queue is out up to `ticket` bytes, writing it when nobody does.
Task<void> flushed(Kept held, std::uint64_t ticket) {
    for (;;) {
        Bytes batch;
        std::uint64_t upto = 0;
        std::optional<Pending<void>> waiting;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->broke) std::rethrow_exception(held->broke);

            if (held->out >= ticket) co_return;

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

        co_await held->stream.write(std::move(batch));
        writer.done(upto);
    }
}

// A whole frame from what is kept, or nothing yet. A frame announced longer than `biggest` closes
// the stream. What was taken is cut off once it is half of what is kept, so a read of many small
// frames does not move the rest once for each. Called under the lock.
std::optional<Bytes> whole(Sealed::Held& held) {
    const std::size_t left = held.spare.size() - held.start;

    if (left < 4) return std::nullopt;

    const auto from = held.spare.begin() + static_cast<std::ptrdiff_t>(held.start);
    std::uint32_t much = 0;

    for (std::ptrdiff_t at = 0; at < 4; ++at) much = (much << 8) | std::to_integer<std::uint32_t>(from[at]);

    if (much > biggest) {
        shutting(held);

        throw Trouble{Kind::malformed, "The other side announced a frame too long."};
    }

    if (left < 4 + std::size_t{much}) return std::nullopt;

    Bytes made(from + 4, from + 4 + static_cast<std::ptrdiff_t>(much));

    held.start += 4 + std::size_t{much};

    if (held.start * 2 >= held.spare.size()) {
        held.spare.erase(held.spare.begin(), held.spare.begin() + static_cast<std::ptrdiff_t>(held.start));
        held.start = 0;
    }

    return made;
}

// The next frame: from what is kept, or after reading more. The end of the stream is the end of the
// channel, broken for both directions from then on.
Task<Bytes> framed(Kept held) {
    for (;;) {
        {
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->shut) throw gone();

            if (std::optional<Bytes> ready = whole(*held)) co_return std::move(*ready);
        }

        const Bytes came = co_await held->stream.read(65536);
        const std::lock_guard<std::mutex> kept{held->guard};

        if (came.empty()) {
            shutting(*held);

            throw Trouble{Kind::broken, "The other side closed the channel."};
        }

        held->spare.insert(held->spare.end(), came.begin(), came.end());
    }
}

// The handshake `how` over `held`: what it has to say goes out, what comes goes to it, until it is
// ready; more than 16 turns means it never will be.
Task<void> shaking(Kept held, std::shared_ptr<Handshake> how) {
    for (int turn = 0;; ++turn) {
        Bytes out;

        while (std::optional<Bytes> one = how->said()) {
            const Bytes made = frame(*one);

            out.insert(out.end(), made.begin(), made.end());
        }

        if (!out.empty()) {
            std::uint64_t ticket = 0;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                ticket = enqueued(*held, out);
            }

            co_await flushed(held, ticket);
        }

        if (how->ready()) co_return;

        if (turn >= 16) throw Trouble{Kind::malformed, "The handshake will not end."};

        how->heard(co_await framed(held));
    }
}

// The reader of the channel while it reads; a second is refused as busy.
class Reading {
public:
    // Takes the reading of `held`, or throws `Trouble` (busy).
    explicit Reading(Sealed::Held& held) : held_(held) {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (held_.reading) throw Trouble{Kind::busy, "The channel has a reader already."};

        held_.reading = true;
    }

    // Lets the reading go.
    ~Reading() {
        const std::lock_guard<std::mutex> kept{held_.guard};

        held_.reading = false;
    }

    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;

private:
    Sealed::Held& held_;
};

}

// A channel over `one` once the handshake `how` is over, its seals from `cipher`. A failed
// handshake closes the stream.
Task<Sealed> Sealed::opened(Stream one, std::shared_ptr<Handshake> how, Port<crypto::Cipher> cipher) {
    if (how == nullptr) throw Trouble{Kind::other, "A channel needs a handshake."};

    auto held = std::make_shared<Held>();

    held->stream = std::move(one);
    held->cipher = cipher.shared();

    std::exception_ptr failed;

    try {
        co_await shaking(held, how);

        held->keys = how->keys();
    } catch (const Trouble&) {
        failed = std::current_exception();
    }

    if (failed) {
        const std::lock_guard<std::mutex> kept{held->guard};

        shutting(*held);
        std::rethrow_exception(failed);
    }

    co_return Sealed{std::move(held)};
}

// Sends the message `what`. Sealed and queued under one lock, so the count goes up in the order the
// frames enter the queue; sealed and queued apart, a send from another thread could come between
// with the next number and go first, and the other side would refuse both.
Task<void> Sealed::send(Bytes what) const {
    if (held_ == nullptr) throw gone();

    if (what.size() > longest) throw Trouble{Kind::other, "The message is too long for the channel."};

    const Kept held = held_;
    std::uint64_t ticket = 0;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->shut) throw gone();

        const Bytes head = headed(what.size() + 16);
        const Bytes sealed = held->cipher->sealed(held->keys.sending, nonce(held->sent++), head, what);

        ticket = enqueued(*held, head);
        ticket = enqueued(*held, sealed);
    }

    co_await flushed(held, ticket);
}

// The next message, opened and checked; a forged or lost one closes the channel.
Task<Bytes> Sealed::received() const {
    if (held_ == nullptr) throw gone();

    const Kept held = held_;
    const Reading reading{*held};
    const Bytes came = co_await framed(held);
    const std::lock_guard<std::mutex> kept{held->guard};
    std::optional<Bytes> plain = held->cipher->opened(held->keys.receiving, nonce(held->taken++), headed(came.size()), came);

    if (!plain) {
        shutting(*held);

        throw Trouble{Kind::broken, "A message of the channel was forged or lost."};
    }

    co_return std::move(*plain);
}

// Closes the stream; what comes after is refused.
void Sealed::close() {
    if (held_ == nullptr) return;

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        shutting(*held_);
    }

    held_.reset();
}

}

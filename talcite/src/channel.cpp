#include "talcite/channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

#include "talcite/trouble.hpp"

#include "outbox.hpp"

namespace talcite::net {

// An encrypted channel over a stream: first key agreement (any implementation of the `Handshake`
// seam), then messages sealed with AES-256-GCM from the cipher seam. Every message is a frame: four
// bytes of length, most significant first, then the content. Handshake frames go in the clear,
// protected by the handshake itself; data frames are sealed, and the length header is the AEAD
// additional data, so tampering with the length is a refusal. The nonce is the message counter in
// the given direction (four zero bytes and eight of the counter), so a repeated, reordered or lost
// message breaks the seal of the next one. The channel speaks in messages, not bytes: exactly what
// was sent arrives on the other side.
//
// Writes go in order through one queue (`Outbox`, as in `TlsStream`), and a read takes from the
// spare buffer whatever came beyond the current frame. The stream is read and closed under
// `minding`, and asynchronous work runs on a copy: the same rule and the same reason as in
// `TlsStream` (`close()` replaces the `shared_ptr` in the member). A forged frame or a failed
// handshake closes the stream: the counters have already diverged, and the other side should learn
// at once, not after its patience runs out. The channel has one reader at a time; two `receive`
// calls in flight could take the counters out of order.
namespace {

// The longest frame: a four-megabyte message plus the tag. A longer announcement breaks the
// channel: a stranger may not make us reserve gigabytes.
constexpr std::uint32_t biggest = (4u << 20) + 16;

using Clock = std::chrono::steady_clock;

// The time left until the deadline, but at least one millisecond, so that a late call still gets a
// nonzero patience.
Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// A promise already rejected with `Trouble` and the given text.
template <typename T>
Promise<T> refused(const char* said) {
    return failed<T>(std::make_exception_ptr(Trouble{said}));
}

// The empty address a closed channel reports as its peer.
const Address& nowhere() {
    static const Address one;

    return one;
}

// The four-byte length header of a frame, most significant byte first.
Bytes headed(std::size_t much) {
    Bytes made(4);

    for (int at = 0; at < 4; ++at) made[static_cast<std::size_t>(at)] = static_cast<std::byte>((much >> (24 - 8 * at)) & 0xff);

    return made;
}

// The 12-byte GCM nonce for a message number: four zero bytes, then the counter, most significant
// byte first.
Bytes nonce(std::uint64_t counter) {
    Bytes made(12);

    for (int at = 0; at < 8; ++at) made[static_cast<std::size_t>(4 + at)] = static_cast<std::byte>((counter >> (56 - 8 * at)) & 0xff);

    return made;
}

}

struct Channel::Held {
    Stream one;
    Seam<CipherProvider> cipher;
    Keying keys;

    std::mutex minding;
    Bytes spare;
    std::size_t start = 0;
    std::uint64_t sent = 0;
    std::uint64_t taken = 0;

    std::mutex sending;
    Outbox outbox;
};

namespace {

// A copy of the stream taken under the lock, for asynchronous work.
Stream borrowed(const std::shared_ptr<Channel::Held>& held) {
    const std::lock_guard<std::mutex> kept{held->minding};

    return held->one;
}

// Closes the stream under the lock.
void shut(Channel::Held& held) {
    const std::lock_guard<std::mutex> kept{held.minding};

    held.one.close();
}

// Writing in order: the bytes join the write queue, and each write waits for the previous one.
// The caller holds no lock (`Outbox::flush`).
Promise<void> posting(const std::shared_ptr<Channel::Held>& held, Bytes what, Patience patience) {
    Promise<void> made = held->outbox.queued(std::move(what), patience, [held] { return borrowed(held); });

    held->outbox.flush();

    return made;
}

// A whole frame from the spare buffer, if it is already there, or nothing. Throws and closes the
// stream when the announced length is above `biggest`. The buffer is read from `start`, and what
// was taken is cut off only once it is half of the buffer: a read that brought hundreds of small
// frames would otherwise move the rest of the buffer once for each of them.
std::optional<Bytes> whole(Channel::Held& held) {
    const std::lock_guard<std::mutex> kept{held.minding};

    const std::size_t left = held.spare.size() - held.start;

    if (left < 4) return std::nullopt;

    const auto from = held.spare.begin() + static_cast<std::ptrdiff_t>(held.start);

    std::uint32_t much = 0;

    for (int at = 0; at < 4; ++at) much = (much << 8) | static_cast<std::uint32_t>(from[at]);

    if (much > biggest) {
        held.one.close();

        throw Trouble{"The other side announced a frame that is too long."};
    }

    if (left < 4 + static_cast<std::size_t>(much)) return std::nullopt;

    Bytes made(from + 4, from + 4 + static_cast<std::ptrdiff_t>(much));

    held.start += 4 + static_cast<std::size_t>(much);

    if (held.start * 2 >= held.spare.size()) {
        held.spare.erase(held.spare.begin(), held.spare.begin() + static_cast<std::ptrdiff_t>(held.start));
        held.start = 0;
    }

    return made;
}

// The next frame: from the spare buffer or after reading more. The end of the stream in the middle
// of a frame breaks the channel. A frame announced too long already in the buffer rejects the
// promise, as one read later does: a throw from here would reach a caller who expects a promise.
Promise<Bytes> framed(const std::shared_ptr<Channel::Held>& held, Clock::time_point until) {
    std::optional<Bytes> ready;

    try {
        ready = whole(*held);
    } catch (...) {
        return failed<Bytes>(std::current_exception());
    }

    if (ready) return run(io(), [made = std::move(*ready)]() mutable -> Bytes { return std::move(made); });

    return borrowed(held).read(65536, left(until)).then([held, until](const Bytes& came) {
        if (came.empty()) throw Trouble{"The other side closed the channel."};

        {
            const std::lock_guard<std::mutex> kept{held->minding};

            held->spare.insert(held->spare.end(), came.begin(), came.end());
        }

        return framed(held, until);
    });
}

// The content with its length header in front.
Bytes frame(const Bytes& what) {
    Bytes made = headed(what.size());

    made.insert(made.end(), what.begin(), what.end());

    return made;
}

// The handshake: while it has something to say, we say it; when it is ready, we are done; otherwise
// we wait for a frame. More than 16 turns means it will never finish.
Promise<void> shaking(const std::shared_ptr<Channel::Held>& held, const std::shared_ptr<Handshake>& how,
                      Clock::time_point until, int turn) {
    Bytes out;

    while (std::optional<Bytes> one = how->said()) {
        const Bytes made = frame(*one);

        out.insert(out.end(), made.begin(), made.end());
    }

    Promise<void> sent = out.empty() ? ready() : posting(held, std::move(out), left(until));

    if (how->ready()) return sent;

    if (turn >= 16) throw Trouble{"The handshake will not finish."};

    return std::move(sent).then([held, how, until, turn] {
        return framed(held, until).then([held, how, until, turn](const Bytes& came) {
            how->heard(came);

            return shaking(held, how, until, turn + 1);
        });
    });
}

}

// A channel over the stream once the handshake has finished. On failure the stream is closed and
// the promise rejected.
Promise<Channel> Channel::opened(Stream one, std::shared_ptr<Handshake> how, Seam<CipherProvider> cipher,
                                 Patience patience) {
    if (!how || !cipher.bound()) return refused<Channel>("A channel needs a handshake and a cipher.");

    auto held = std::make_shared<Held>();

    held->one = std::move(one);
    held->cipher = std::move(cipher);

    Promise<Channel> made = shaking(held, how, Clock::now() + patience, 0)
                                .then([held, how] {
                                    held->keys = how->keys();

                                    return Channel{held};
                                })
                                .recover([held](const std::exception&) -> Channel {
                                    shut(*held);

                                    throw;
                                });

    return made;
}

// The address of the other side, or an empty address once the channel is closed.
const Address& Channel::peer() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.peer();
}

// Sends one message. The seal is computed right away, and the frame joins the write queue under the
// same lock, so the counter advances in the same order in which the frames enter the queue. Taking
// the lock twice, once to seal and once to queue, would let a send from another thread squeeze in
// between with the next number and go out first, and the other side would refuse both.
Promise<void> Channel::send(Bytes what, Patience patience) const {
    if (!held_) return refused<void>("The channel is closed.");

    if (what.size() + 16 > biggest) return refused<void>("The message is too long for the channel.");

    const std::shared_ptr<Held> held = held_;

    std::optional<Promise<void>> queued;

    {
        const std::lock_guard<std::mutex> kept{held->sending};

        const Bytes head = headed(what.size() + 16);

        Bytes made = head;

        const Bytes sealed = held->cipher.provider().sealed(held->keys.sending, nonce(held->sent++), head, what);

        made.insert(made.end(), sealed.begin(), sealed.end());

        queued = held->outbox.queued(std::move(made), patience, [held] { return borrowed(held); });
    }

    held->outbox.flush();

    return std::move(*queued);
}

// The next message, opened and checked. A forged or lost message closes the channel and rejects the
// promise.
Promise<Bytes> Channel::receive(Patience patience) const {
    if (!held_) return refused<Bytes>("The channel is closed.");

    const std::shared_ptr<Held> held = held_;

    return framed(held, Clock::now() + patience).then([held](const Bytes& came) {
        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> kept{held->minding};

            which = held->taken++;
        }

        const std::optional<Bytes> plain =
            held->cipher.provider().opened(held->keys.receiving, nonce(which), headed(came.size()), came);

        if (!plain) {
            shut(*held);

            throw Trouble{"A message in the channel was forged or lost."};
        }

        return *plain;
    });
}

// Closes the stream; later calls refuse.
void Channel::close() {
    if (!held_) return;

    shut(*held_);

    held_.reset();
}

}

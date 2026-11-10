#include "tal/stream.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "tal/trouble.hpp"

namespace tal {

namespace {

// The most bytes one read of `drained` asks for.
constexpr std::size_t drained_piece = 64 * 1024;

// The channel a task works on, or a refusal when the stream holds none.
const std::shared_ptr<Channel>& channel(const std::shared_ptr<Channel>& held) {
    if (held == nullptr) throw Trouble{Kind::missing, "The stream holds nothing."};

    return held;
}

// A pipe in memory, one way: what the writing end writes the reading end reads, in order, as soon
// as it is written. Closing the writing end, or letting it go, ends the stream for the reader;
// closing the reading end makes later writes fail as broken.
struct Pipe {
    std::mutex guard;
    std::deque<std::byte> buffer;
    std::optional<Pending<void>> reader;
    std::uint64_t reading = 0;
    std::uint64_t readers = 0;
    bool ended = false;
    bool deaf = false;

    // Takes at most `most` bytes from the buffer; called with the lock held.
    Bytes taken(std::size_t most) {
        const auto much = static_cast<std::ptrdiff_t>(std::min(most, buffer.size()));
        Bytes made{buffer.begin(), buffer.begin() + much};

        buffer.erase(buffer.begin(), buffer.begin() + much);

        return made;
    }

    // Wakes the reader that waits, if any; called with the lock held, the waking done by the caller
    // once the lock is let go.
    std::optional<Pending<void>> woken() {
        reading = 0;

        return std::exchange(reader, std::nullopt);
    }

    // A reader that went away while it waited (cancelled) no longer waits: the next reader is not
    // refused, and what comes stays in the buffer for it.
    void forgotten(std::uint64_t who) {
        const std::lock_guard<std::mutex> kept{guard};

        if (reading != who) return;

        reader.reset();
        reading = 0;
    }

    // Ends the stream for the reader, who gets what is left and then nothing.
    void end() {
        std::optional<Pending<void>> waking;

        {
            const std::lock_guard<std::mutex> kept{guard};

            ended = true;
            waking = woken();
        }

        if (waking) waking->settle();
    }
};

// The end of a pipe that reads.
struct Reading : Channel {
    std::shared_ptr<Pipe> pipe;

    // The reading end of the pipe.
    explicit Reading(std::shared_ptr<Pipe> given) : pipe(std::move(given)) {}

    // Nobody reads any longer: later writes fail.
    ~Reading() override { close(); }

    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;

    // What is in the pipe, at most `most` bytes, waiting for some when it is empty; nothing once
    // the writer has ended. One reader at a time. The reader is woken when bytes come and takes
    // them itself, so a reader that goes away while it waits (cancelled and destroyed) takes
    // nothing with it; it takes itself off the pipe on the way (`Forgetting`).
    Task<Bytes> read(std::size_t most) override {
        return [](std::shared_ptr<Pipe> held, std::size_t much) -> Task<Bytes> {
            struct Forgetting {
                std::shared_ptr<Pipe> pipe;
                std::uint64_t who;

                // Keeps the pipe and the reader's number.
                Forgetting(std::shared_ptr<Pipe> given, std::uint64_t number) : pipe(std::move(given)), who(number) {}

                Forgetting(const Forgetting&) = delete;
                Forgetting& operator=(const Forgetting&) = delete;

                // Takes the reader off the pipe, if it still waits there.
                ~Forgetting() { pipe->forgotten(who); }
            };

            std::uint64_t number = 0;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                number = ++held->readers;
            }

            const Forgetting forgetting{held, number};

            while (true) {
                const Pending<void> waiter;

                {
                    const std::lock_guard<std::mutex> kept{held->guard};

                    if (!held->buffer.empty() || held->ended || held->deaf || much == 0) {
                        if (held->reading == number) (void)held->woken();

                        co_return held->taken(much);
                    }

                    if (held->reader && held->reading != number) throw Trouble{Kind::busy, "The stream has a reader already."};

                    held->reader = waiter;
                    held->reading = number;
                }

                co_await waiter.task();
            }
        }(pipe, most);
    }

    // The reading end does not write.
    Task<void> write(Bytes) override {
        return []() -> Task<void> {
            throw Trouble{Kind::unsupported, "This end of the stream only reads."};

            co_return;
        }();
    }

    // Stops reading: what is buffered goes, later writes fail, and a read that waits gets nothing.
    void close() override {
        std::optional<Pending<void>> waking;

        {
            const std::lock_guard<std::mutex> kept{pipe->guard};

            pipe->deaf = true;
            pipe->buffer.clear();
            waking = pipe->woken();
        }

        if (waking) waking->settle();
    }
};

// The end of a pipe that writes.
struct Writing : Channel {
    std::shared_ptr<Pipe> pipe;

    // The writing end of the pipe.
    explicit Writing(std::shared_ptr<Pipe> given) : pipe(std::move(given)) {}

    // The writer is gone: the stream ends for the reader.
    ~Writing() override { pipe->end(); }

    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;

    // The writing end does not read.
    Task<Bytes> read(std::size_t) override {
        return []() -> Task<Bytes> {
            throw Trouble{Kind::unsupported, "This end of the stream only writes."};

            co_return Bytes{};
        }();
    }

    // Puts the bytes in the pipe and wakes a reader that waits; fails once either end has closed.
    Task<void> write(Bytes what) override {
        return [](std::shared_ptr<Pipe> held, Bytes given) -> Task<void> {
            std::optional<Pending<void>> waking;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->deaf || held->ended) throw Trouble{Kind::broken, "Nobody reads the stream any longer."};

                held->buffer.insert(held->buffer.end(), given.begin(), given.end());

                waking = held->woken();
            }

            if (waking) waking->settle();

            co_return;
        }(pipe, std::move(what));
    }

    // Ends the stream for the reader.
    void close() override { pipe->end(); }
};

}

// A stream over what the adapter gave.
Stream::Stream(std::shared_ptr<Channel> held) : held_(std::move(held)) {}

// At most `most` bytes, as soon as some are there; nothing at the end of the stream.
Task<Bytes> Stream::read(std::size_t most) const {
    return [](std::shared_ptr<Channel> held, std::size_t much) -> Task<Bytes> {
        co_return co_await channel(held)->read(much);
    }(held_, most);
}

// Writes the bytes, all of them.
Task<void> Stream::write(Bytes what) const {
    return [](std::shared_ptr<Channel> held, Bytes given) -> Task<void> {
        co_await channel(held)->write(std::move(given));
    }(held_, std::move(what));
}

// Ends writing: the other side reads to the end of the stream. Reading goes on.
void Stream::close() const {
    if (held_) held_->close();
}

// Everything the stream gives until its end.
Task<Bytes> drained(Stream from) {
    Bytes all;

    for (;;) {
        const Bytes piece = co_await from.read(drained_piece);

        if (piece.empty()) co_return all;

        all.insert(all.end(), piece.begin(), piece.end());
    }
}

namespace detail {

// A pipe in memory: the reading end, then the writing end.
std::pair<Stream, Stream> piped() {
    const auto pipe = std::make_shared<Pipe>();

    return {Stream{std::make_shared<Reading>(pipe)}, Stream{std::make_shared<Writing>(pipe)}};
}

}

}

#include "tal/stream.hpp"

#include <algorithm>
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
    std::optional<Pending<Bytes>> reader;
    std::size_t asked = 0;
    bool ended = false;
    bool deaf = false;

    // Takes at most `most` bytes from the buffer; called with the lock held.
    Bytes taken(std::size_t most) {
        const auto much = static_cast<std::ptrdiff_t>(std::min(most, buffer.size()));
        Bytes made{buffer.begin(), buffer.begin() + much};

        buffer.erase(buffer.begin(), buffer.begin() + much);

        return made;
    }

    // Ends the stream for the reader, who gets what is left and then nothing.
    void end() {
        std::optional<Pending<Bytes>> woken;

        {
            const std::lock_guard<std::mutex> kept{guard};

            ended = true;
            woken = std::exchange(reader, std::nullopt);
        }

        if (woken) woken->settle(Bytes{});
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
    // the writer has ended. One reader at a time.
    Task<Bytes> read(std::size_t most) override {
        return [](std::shared_ptr<Pipe> held, std::size_t much) -> Task<Bytes> {
            Pending<Bytes> waiter;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (!held->buffer.empty() || held->ended || much == 0) co_return held->taken(much);

                if (held->reader) throw Trouble{Kind::busy, "The stream has a reader already."};

                held->reader = waiter;
                held->asked = much;
            }

            co_return co_await waiter.task();
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
        std::optional<Pending<Bytes>> woken;

        {
            const std::lock_guard<std::mutex> kept{pipe->guard};

            pipe->deaf = true;
            pipe->buffer.clear();
            woken = std::exchange(pipe->reader, std::nullopt);
        }

        if (woken) woken->settle(Bytes{});
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
            std::optional<Pending<Bytes>> woken;
            Bytes handed;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->deaf || held->ended) throw Trouble{Kind::broken, "Nobody reads the stream any longer."};

                held->buffer.insert(held->buffer.end(), given.begin(), given.end());

                if (held->reader) {
                    woken = std::exchange(held->reader, std::nullopt);
                    handed = held->taken(held->asked);
                }
            }

            if (woken) woken->settle(std::move(handed));

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

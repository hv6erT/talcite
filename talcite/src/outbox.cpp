#include "outbox.hpp"

#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

namespace talcite::net {

// The write queue of a stream that takes one write at a time (`Channel`, `TlsStream`). Whatever
// goes out lines up here, in the order of `queued`, and one writer on the pool, started by
// `flush`, takes the pieces out one by one while there are any (`served`).
//
// It replaces a chain of promises in which every write was attached to the previous one with
// `then`. The chain held the whole history of writes as long as new ones kept coming, and
// cancelling one write climbed the chain and cancelled the writes before it, the one going to the
// socket at that moment among them; half a frame on the wire breaks the stream for good. Here a
// cancelled write only lets go of its result: its bytes, already sealed and counted by the
// caller, still go out whole and in their place.
//
// A failed write fails the pieces queued behind it as well, with the same reason: the stream is
// broken in the middle of something, and whatever followed would only be read as rubbish. A piece
// queued after the queue has run dry starts afresh.
struct Outbox::State {
    struct Piece {
        Bytes what;
        Patience patience{};
        Source source;
        std::optional<Settle<void>> made;
    };

    std::mutex guard;
    std::deque<Piece> waiting;
    bool writing = false;
};

namespace {

// Settles a piece: with the error the queue broke on, or fulfilled. Settling runs nothing in this
// call, so the writer goes on at once.
void told(Settle<void>& made, const std::exception_ptr& why) {
    if (why) (void)made.fail(why);
    else (void)made.value();
}

}

// An empty queue.
Outbox::Outbox() : state_(std::make_shared<State>()) {}

// Queues `what` to be written with `patience` to the stream `source` gives at the piece's turn.
// Pieces go out in the order of the calls, so a caller that seals or numbers its bytes calls this
// under the same lock under which it numbers them. It neither writes nor settles anything, so it is
// safe under any lock; `flush` then starts the writer, after the caller has let go of its lock.
Promise<void> Outbox::queued(Bytes what, Patience patience, Source source) {
    auto [made, settle] = promised<void>();

    const std::lock_guard<std::mutex> kept{state_->guard};

    state_->waiting.push_back(State::Piece{std::move(what), patience, std::move(source), std::move(settle)});

    return std::move(made);
}

// Starts the writer when there is something to write and no writer is running. A writer that cannot
// be started (the pool takes no more work) fails every piece waiting, as a failed write would; left
// in the queue, they would wait for a writer that may never come.
void Outbox::flush() {
    {
        const std::lock_guard<std::mutex> kept{state_->guard};

        if (state_->writing || state_->waiting.empty()) return;

        state_->writing = true;
    }

    if (!io().post([state = state_] { served(state); })) {
        const std::exception_ptr why = std::make_exception_ptr(Trouble{"The pool takes no more work."});

        std::deque<State::Piece> failed;

        {
            const std::lock_guard<std::mutex> kept{state_->guard};

            failed.swap(state_->waiting);

            state_->writing = false;
        }

        for (State::Piece& one : failed) told(*one.made, why);
    }
}

// The writer: takes the pieces off one by one and writes each to its stream, waiting for the write
// to end before the next one, until the queue is empty. It holds only the queue, not its owner, so
// the owner can go while the last pieces are still being written.
void Outbox::served(const std::shared_ptr<State>& state) {
    std::exception_ptr broken;

    for (;;) {
        State::Piece next;

        {
            const std::lock_guard<std::mutex> kept{state->guard};

            if (state->waiting.empty()) {
                state->writing = false;

                return;
            }

            next = std::move(state->waiting.front());
            state->waiting.pop_front();
        }

        if (!broken) {
            try {
                next.source().write(std::move(next.what), next.patience).get();
            } catch (...) {
                broken = std::current_exception();
            }
        }

        told(*next.made, broken);
    }
}

}

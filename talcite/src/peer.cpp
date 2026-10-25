#include "talcite/peer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/executor.hpp"

#include "hearing.hpp"
#include "stunning.hpp"

namespace talcite::net {

// What the receive loop sees. This is not the same object the caller holds; see `Held` below.
struct Living {
    Udp post;

    // The relay is someone else's property: the peer uses it but never closes it. Three threads
    // touch the same instance (the post loop when answering, the relay loop when receiving and the
    // caller when sending), and they may, because none of them calls `close` on it. It is `close`
    // that clears the handle, and that would be the race, not the reading itself.
    Relay relay;

    mutable std::mutex guard;
    std::condition_variable stirring;

    std::vector<Candidate> mine;
    std::vector<Candidate> theirs;
    Address met;

    // Which channel the winning path went through. The channel is part of the conversation's
    // identity: the same address seen through the relay and directly are two different paths, and
    // data is taken only from the one that won.
    bool relayed = false;

    // Addresses we have already granted a permission on our own relay, without ports, because a
    // permission covers an address. Without this memory every punching round would grant them
    // again.
    std::vector<Address> allowed;

    Number number{};
    std::deque<Bytes> spare;

    // The post accepts one send at a time and rejects a second one. Three things send here: probes
    // from reach, answers and keepalives from the receive loop, and data from the caller. Without
    // this lock they would step on each other and lose datagrams.
    std::mutex speaking;

    std::atomic<bool> reaching{false};
    std::atomic<bool> living{false};
    std::atomic<bool> fetching{false};
    std::atomic<bool> shutting{false};
};

// The caller holds a separate object, which the loop never holds. Thanks to that, letting go of
// the last peer closes the socket immediately, not only when the loop itself looks at its weak
// handle.
//
// This is not decoration. A pool task that outlives its owner and hangs on the hub deadlocks the
// whole program at exit: tearing down the hub waits for the pool to drain, and the task in the pool
// waits for a hub that no longer exists.
struct Peer::Held {
    std::shared_ptr<Living> alive;

    // Our own copy of the post, used only for closing. Copies share the socket but not the object,
    // and `Udp`, like every value type in talcite, does not tolerate the same instance being
    // touched from two threads at once. Closing the copy the loop reads from would race on the
    // loop's own handle.
    Udp closing;

    // Shuts the conversation down: marks it as shutting, closes the socket and wakes everyone who
    // waits, so that both loops and any pending receive end promptly.
    ~Held() {
        {
            // The flag is set under the same lock the waiter holds while checking its condition.
            // Set outside it, it could fall into the gap between the check and falling asleep, and
            // then the notification would be lost and the waiter would sleep through its whole
            // patience.
            const std::lock_guard<std::mutex> kept{alive->guard};

            alive->shutting.store(true);
        }

        closing.close();
        alive->stirring.notify_all();
    }
};

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t roomy = 65535;

constexpr Patience beating{15000};

constexpr Patience pausing{250};

constexpr Patience listening{1000};

// The queue of received parcels nobody has collected yet. Without a bound, a peer sending faster
// than the caller reads would drive the process out of memory. On overflow the oldest one is
// dropped: with datagrams older content is worth less than fresh.
constexpr std::size_t plenty = 256;

// The time left until `until`, at least one millisecond, so that it can be passed as a patience.
Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// How long we wait for one permission from our own relay.
constexpr Patience granting{1000};

// Sends through the chosen channel. Probes and keepalives may be lost: this is a network, and the
// loop above does the repeating. That is why a failure here does not leave this function.
//
// Sending through the relay goes without our lock: the relay guards its own post, while our lock
// protects our socket against a second send at the same moment.
void posting(const std::shared_ptr<Living>& held, const Address& to, Bytes what, Patience patience,
             bool through = false) {
    try {
        if (held->shutting.load()) return;

        if (through) {
            held->relay.send(to, std::move(what), patience).get();

            return;
        }

        const std::lock_guard<std::mutex> kept{held->speaking};

        if (held->shutting.load()) return;

        held->post.send(to, std::move(what), patience).get();
    } catch (const std::exception&) {
    }
}

// How many foreign addresses may be added from probes alone. A NAT usually gives the other side
// one different port, sometimes a few; sixteen leaves room to spare, and without a bound the list
// itself would be a place into which any sender could pour as much as it liked.
constexpr std::size_t roomier = 16;

// Whether `where` is already among the other side's paths. The caller holds `guard`.
bool knowing(const Living& held, const Address& where) {
    for (const Candidate& one : held.theirs)
        if (one.where == where) return true;

    return false;
}

// Adds a path we learn about only from the other side's probe. This always happens when the NAT
// gave the other side a port different from the one it managed to announce, and that is the rule,
// not the exception. Only an address whose host we already know from a path given by signaling is
// added; otherwise a single unsolicited request would put a stranger on the list and earn it a
// reply from us.
void noted(const std::shared_ptr<Living>& held, const Address& where) {
    const std::lock_guard<std::mutex> kept{held->guard};

    if (knowing(*held, where)) return;

    if (held->theirs.size() >= roomier) return;

    for (const Candidate& one : held->theirs)
        if (one.where.family() == where.family() && one.where.bytes() == where.bytes()) {
            held->theirs.push_back(Candidate{where, Candidate::Sort::seen});

            return;
        }
}

// Handles one received datagram. `through` says which channel it came by, our socket or our relay,
// and the answer goes back the same channel, because only that one is proven to pass.
void taking(const std::shared_ptr<Living>& held, Datagram came, bool through) {
    Message made;

    if (understood(came.bytes, made)) {
        // The other side's probe proves that the hole on its side is open. We answer at once and
        // probe back, because the passage has to work in both directions.
        if (made.sort == Sort::asking) {
            posting(held, came.from, told(made.number, came.from), listening, through);

            noted(held, came.from);

            Number number;

            bool met = false;

            bool known = false;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                number = held->number;
                met = held->met.held();
                known = knowing(*held, came.from);
            }

            // We probe back only until we have a winning path (later every foreign probe would
            // multiply traffic for no reason) and only a path we know. Our transaction number is
            // the only proof of an answer's identity; sent back to anyone who spoke up, it would
            // let a stranger answer with it and win the punching.
            //
            // This is also the only place where we go out through our own relay: we do not knock
            // with it on our own, we only answer whoever came to it by themselves.
            if (!met && known) posting(held, came.from, asked(number), listening, through);

            return;
        }

        if (made.sort == Sort::told) {
            bool won = false;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                // The transaction number alone is not enough: the answer must also come from a
                // path we proposed ourselves or learned from a host we know.
                if (made.number == held->number && !held->met.held() &&
                    knowing(*held, came.from)) {
                    held->met = came.from;
                    held->relayed = through;

                    won = true;
                }
            }

            if (won) held->stirring.notify_all();
        }

        // A STUN message never reaches the caller's data. That is the whole point of
        // demultiplexing: one socket carries both the connectivity checks and the conversation.
        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        // Data is taken only from the side we punched through to, and only by that channel.
        // The caller gets bare bytes without a sender, so otherwise anyone who learned the address
        // and port could inject content into the conversation.
        if (!held->met.held() || !(came.from == held->met) || held->relayed != through) return;

        if (held->spare.size() >= plenty) held->spare.pop_front();

        held->spare.push_back(std::move(came.bytes));
    }

    held->stirring.notify_all();
}

// One round of the receive loop. Returns false when the socket has closed and there is no point in
// going on; on a quiet second it sends the keepalive when one is due.
bool turning(const std::shared_ptr<Living>& held, Clock::time_point& beat) {
    Datagram came;

    try {
        if (held->shutting.load()) return false;

        came = held->post.receive(roomy, listening).get();
    } catch (const Overdue&) {
        Address met;

        bool through = false;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            met = held->met;
            through = held->relayed;
        }

        // A NAT mapping usually lives thirty seconds from the last traffic. The keepalive is not
        // decoration but the condition for silence not to break the connection. It goes through
        // the conversation's channel: on a relayed path it is the other side that has to be kept
        // alive, not a mapping the conversation does not use at all.
        if (met.held() && Clock::now() - beat >= beating) {
            Number number;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                number = held->number;
            }

            posting(held, met, asked(number), listening, through);

            beat = Clock::now();
        }

        return true;
    } catch (const std::exception&) {
        return false;
    }

    taking(held, std::move(came), false);

    return true;
}

// One round of the loop that reads from the relay; returns false when the relay has failed. It is
// separate because both loops wait blocking, and one thread cannot wait on two sockets at once.
bool drawing(const std::shared_ptr<Living>& held) {
    Datagram came;

    try {
        if (held->shutting.load()) return false;

        came = held->relay.receive(roomy, listening).get();
    } catch (const Overdue&) {
        return true;
    } catch (const std::exception&) {
        return false;
    }

    taking(held, std::move(came), true);

    return true;
}

// The receive loop. It holds a pool thread for the whole life of the connection, deliberately,
// because the pool for ordinary work grows without limit and there is one peer. With a thousand
// peers this would have to be rewritten as a chain of promises.
//
// It holds a weak handle and takes a strong one only for a single round. Thanks to that, an
// abandoned peer (one nobody closed but only let go of) ends the loop by itself within a second,
// instead of spinning forever and keeping both the thread and the socket alive.
void minding(std::weak_ptr<Living> weak) {
    Clock::time_point beat = Clock::now();

    for (;;) {
        const std::shared_ptr<Living> held = weak.lock();

        if (!held || held->shutting.load()) break;

        if (!turning(held, beat)) break;
    }

    if (const std::shared_ptr<Living> held = weak.lock()) {
        held->living.store(false);

        held->stirring.notify_all();
    }
}

// The relay loop, with the same weak handle and the same rule as above. The peer does not close
// the relay, because it does not own it; this loop ends at most a second after the peer is closed,
// and the relay goes away when its last handle lets go of it, including the one held here.
void watching(std::weak_ptr<Living> weak) {
    for (;;) {
        const std::shared_ptr<Living> held = weak.lock();

        if (!held || held->shutting.load()) break;

        if (!drawing(held)) break;
    }

    if (const std::shared_ptr<Living> held = weak.lock()) {
        held->fetching.store(false);

        held->stirring.notify_all();
    }
}

// Starts the receive loop, and the relay loop when there is a relay, unless they already run.
void waking(const std::shared_ptr<Living>& held) {
    bool was = false;

    if (held->living.compare_exchange_strong(was, true)) {
        run(io(), [weak = std::weak_ptr<Living>{held}]() {
            minding(weak);
        }).detach();
    }

    if (!held->relay.held()) return;

    was = false;

    if (held->fetching.compare_exchange_strong(was, true)) {
        run(io(), [weak = std::weak_ptr<Living>{held}]() {
            watching(weak);
        }).detach();
    }
}

// Grants permissions on our own relay. Without them its server drops every datagram from the other
// side and our lent address would be announced in vain. A permission covers an address, not a
// port, so one is enough for all the ports the NAT gives the other side, and that is why they are
// remembered with the port set to zero.
//
// A refusal does not stop the punching: direct paths work without the relay and they take
// precedence.
void letting(const std::shared_ptr<Living>& held) {
    if (!held->relay.held()) return;

    // A permission without an allocation makes no sense: there is nothing to guard and a real
    // server answers with a refusal. A relay that could not be opened while gathering simply does
    // not exist.
    if (!held->relay.lent().held()) return;

    std::vector<Address> whom;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        for (const Candidate& one : held->theirs) {
            Address bare = one.where;

            bare.port(0);

            if (std::find(held->allowed.begin(), held->allowed.end(), bare) !=
                held->allowed.end())
                continue;

            if (std::find(whom.begin(), whom.end(), bare) != whom.end()) continue;

            whom.push_back(bare);
        }
    }

    for (const Address& one : whom) {
        if (held->shutting.load()) return;

        try {
            held->relay.allow(one, granting).get();
        } catch (const std::exception&) {
        }

        // The address is recorded even after a refusal. Repeating a failed permission in every
        // punching round would eat the punching's patience on a path that does not work anyway.
        const std::lock_guard<std::mutex> kept{held->guard};

        held->allowed.push_back(one);
    }
}

// A promise already rejected with `Trouble` carrying `said`.
template <typename T>
Promise<T> amiss_now(const std::string& said) {
    return failed<T>(std::make_exception_ptr(Trouble{said}));
}

}

// A peer talking over `post`, without a relay, with a fresh transaction number.
Peer::Peer(Udp post) : held_(std::make_shared<Held>()) {
    held_->alive = std::make_shared<Living>();
    held_->alive->post = post;
    held_->alive->number = numbered();
    held_->closing = std::move(post);
}

// A peer that can also be reached through `relay`. The relay is given at creation or not at all.
// Plugged in later, it would leave a moment in which the paths already gathered say nothing about
// the lent address and the other side has nothing to knock on.
Peer::Peer(Udp post, Relay relay) : Peer(std::move(post)) {
    held_->alive->relay = std::move(relay);
}

// Our own paths found by `gather`; empty for an empty peer.
std::vector<Candidate> Peer::candidates() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->mine;
}

// The other side's known paths, in the order they are tried.
std::vector<Candidate> Peer::theirs() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->theirs;
}

// The other side's address on the winning path; empty until punching succeeds.
Address Peer::met() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->met;
}

// Whether the winning path goes through our relay.
bool Peer::relayed() const {
    if (!held_) return false;

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->relayed;
}

// Adds a path to the other side, as received through signaling. Paths are tried in the order in
// which they lie, and they lie by kind: one's own network card before the reflection from the
// server, the reflection before the relay. Two machines in one network have no reason to go out to
// the world, and a relay costs someone else's bandwidth and is the last resort. Empty addresses,
// our own socket's address and duplicates are ignored.
void Peer::add(Candidate one) {
    if (!held_) return;

    if (!one.where.held()) return;

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    if (one.where == held_->alive->post.mine()) return;

    std::vector<Candidate>& theirs = held_->alive->theirs;

    if (std::find(theirs.begin(), theirs.end(), one) != theirs.end()) return;

    const auto at =
        std::upper_bound(theirs.begin(), theirs.end(), one,
                         [](const Candidate& left, const Candidate& right) {
                             return left.sort < right.sort;
                         });

    theirs.insert(at, one);
}

// Gathers our own candidates. The addresses of our own network cards get the port of our socket,
// because that is what the other side looks for. Loopback is left out: sent to the other side it
// would make it knock on itself, and an answer from its own socket would look like a successful
// punch.
Promise<std::vector<Candidate>> Peer::gather(StunClient scout, Patience patience) {
    if (!held_) return amiss_now<std::vector<Candidate>>("The peer is empty.");

    if (held_->alive->living.load())
        return amiss_now<std::vector<Candidate>>("Gathering must finish before punching starts.");

    auto held = held_->alive;

    return run(io(), [held, scout, patience]() -> std::vector<Candidate> {
        const Clock::time_point until = Clock::now() + patience;

        const std::uint16_t port = held->post.mine().port();

        std::vector<Candidate> found;

        for (Address one : mine(held->post.mine().family()).get()) {
            if (one == Address::loopback(one.family(), 0)) continue;

            one.port(port);

            found.push_back(Candidate{one, Candidate::Sort::here});
        }

        // The reflection from the server may be missing: no scout, no answer or past the
        // deadline. That is no reason to fail the gathering, since within one local network
        // the cards alone are enough. The deadline is ours, not the scout's: the scout has its
        // own patience from its constructor, usually an order of magnitude longer, and without
        // this check it would use all of it.
        if (scout.held() && Clock::now() < until) {
            try {
                if (const Address seen = scout.seen().timeout(left(until)).get(); seen.held())
                    found.push_back(Candidate{seen, Candidate::Sort::seen});
            } catch (const std::exception&) {
            }
        }

        // The relay is the last resort, so it is asked last, and its address goes to the list
        // as a `lent` path, that is, after all the others. It is opened only when it is not
        // open yet: the caller has the right to do that itself and hand it over ready. A failed
        // relay does not fail the gathering, just like a failed scout.
        if (held->relay.held() && Clock::now() < until) {
            try {
                Address lent = held->relay.lent();

                if (!lent.held()) lent = held->relay.open(left(until)).get();

                if (lent.held()) found.push_back(Candidate{lent, Candidate::Sort::lent});
            } catch (const std::exception&) {
            }
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            held->mine = found;
        }

        return found;
    });
}

// Punches through to the other side; resolves with the address on the winning path, or rejects
// with `Overdue` when the patience runs out. Both sides do the same thing and must do it at the
// same time: my datagram dies on the other NAT but opens a hole in mine, and theirs dies on mine,
// opening a hole in theirs. Only the second datagram of each side meets an open passage. Cancelling
// the promise ends the punching within one round.
Promise<Address> Peer::reach(Patience patience) {
    if (!held_) return amiss_now<Address>("The peer is empty.");

    bool was = false;

    if (!held_->alive->reaching.compare_exchange_strong(was, true))
        return amiss_now<Address>("The peer is already punching.");

    auto held = held_->alive;

    return run(io(), [held, patience]() -> Address {
        try {
            const Hearing heard{held};

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->theirs.empty())
                    throw Trouble{"No known way to the other side."};
            }

            waking(held);

            const Clock::time_point until = Clock::now() + patience;

            while (Clock::now() < until && !held->shutting.load() && !heard.rang()) {
                // Permissions are granted in every round, because paths may arrive from signaling
                // while the punching is already under way. An address already handled costs
                // nothing here.
                letting(held);

                std::vector<Candidate> ways;

                Number number;

                {
                    const std::lock_guard<std::mutex> kept{held->guard};

                    ways = held->theirs;
                    number = held->number;
                }

                for (const Candidate& one : ways) {
                    if (held->shutting.load() || heard.rang()) break;

                    // The deadline is checked at every path, not once per round. With a dozen or
                    // so paths one round takes a dozen quarter-seconds, so reach would otherwise
                    // overrun its own patience by an order of magnitude.
                    if (Clock::now() >= until) break;

                    posting(held, one.where, asked(number), pausing);
                }

                std::unique_lock<std::mutex> kept{held->guard};

                if (held->stirring.wait_for(kept, pausing, [&held, &heard] { return held->met.held() || heard.rang(); }) &&
                    held->met.held()) {
                    const Address won = held->met;

                    kept.unlock();

                    held->reaching.store(false);

                    return won;
                }
            }

            if (heard.rang()) throw Cancelled{};

            throw Overdue{};
        } catch (...) {
            held->reaching.store(false);

            throw;
        }
    });
}

// Sends one parcel to the other side on the winning path; rejects when no path has won yet or the
// peer is closed.
Promise<void> Peer::send(Bytes what, Patience patience) {
    if (!held_) return amiss_now<void>("The peer is empty.");

    Address met;

    bool through = false;

    {
        const std::lock_guard<std::mutex> kept{held_->alive->guard};

        met = held_->alive->met;
        through = held_->alive->relayed;
    }

    if (!met.held()) return amiss_now<void>("No one to talk to yet.");

    auto held = held_->alive;

    return run(io(), [held, met, through, what = std::move(what), patience]() mutable {
        if (held->shutting.load()) throw Trouble{"The peer is closed."};

        // We speak through the channel we punched through. Sending directly to an address that
        // spoke up through the relay would hit the other side's NAT and be lost.
        if (through) {
            held->relay.send(met, std::move(what), patience).get();

            return;
        }

        const std::lock_guard<std::mutex> kept{held->speaking};

        if (held->shutting.load()) throw Trouble{"The peer is closed."};

        held->post.send(met, std::move(what), patience).get();
    });
}

// Receives the next parcel from the other side, at most `most` bytes; rejects with `Overdue` when
// nothing arrives within the patience, and with `Trouble` when the peer is closed or the parcel
// does not fit. Cancelling the promise ends the wait at once.
Promise<Bytes> Peer::receive(std::size_t most, Patience patience) {
    if (!held_) return amiss_now<Bytes>("The peer is empty.");

    auto held = held_->alive;

    return run(io(), [held, most, patience]() -> Bytes {
        waking(held);

        const Hearing heard{held};

        std::unique_lock<std::mutex> kept{held->guard};

        // The death of the loops wakes the waiter too. Without this condition a notification
        // about a closed socket could never become true, so the caller would sleep through its
        // whole patience and get a timeout instead of the real reason, holding a pool thread
        // all the while. It takes the death of both loops, and with a relay there are two: as
        // long as one of them lives, silence is only silence and the caller has to keep
        // waiting.
        if (!held->stirring.wait_for(kept, patience, [&held, &heard] {
                return !held->spare.empty() || held->shutting.load() ||
                       (!held->living.load() && !held->fetching.load()) || heard.rang();
            }))
            throw Overdue{};

        if (heard.rang()) throw Cancelled{};

        if (held->spare.empty()) throw Trouble{"The peer is closed."};

        // A datagram is a whole. Handing over a piece pretending to be the whole would be worse
        // than refusing, and `Udp` decides it the same way. But the refusal must not eat the
        // datagram: the size is checked before it is taken off the queue, so a retry with more
        // room still finds it.
        if (held->spare.front().size() > most)
            throw Trouble{"The parcel does not fit in the given space."};

        Bytes one = std::move(held->spare.front());

        held->spare.pop_front();

        kept.unlock();

        return one;
    });
}

// Closes the peer. Closing explicitly does exactly what letting go of the last peer does: the
// destructor of `Held` does all the work. Two ways, one place.
void Peer::close() { held_.reset(); }

}

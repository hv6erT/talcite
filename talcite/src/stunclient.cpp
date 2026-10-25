#include "talcite/stunclient.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/executor.hpp"

#include "stunning.hpp"

namespace talcite::net {

// The "asking" marker is cleared by the death of the promise, not by its body. If the body cleared
// it, a question cancelled or abandoned before it ran would block the scout for good. This way it
// goes at the latest when the caller lets go of the promise.
struct Asking {
    explicit Asking(std::shared_ptr<StunClient::Held> given) : held(std::move(given)) {}

    ~Asking();

    Asking(const Asking&) = delete;
    Asking& operator=(const Asking&) = delete;
    Asking(Asking&&) = delete;
    Asking& operator=(Asking&&) = delete;

    std::shared_ptr<StunClient::Held> held;
};

struct StunClient::Held {
    Udp post;
    std::vector<Address> servers;
    Patience patience{0};

    mutable std::mutex guard;
    Address last;
    Mapping known = Mapping::unknown;

    std::atomic<bool> asking{false};
};

// Lets the scout ask again.
Asking::~Asking() { held->asking.store(false); }

namespace {

using Clock = std::chrono::steady_clock;

// The time left until `until`, at least one unit, so that a deadline just reached still gives a
// valid wait.
Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// A promise rejected with `Trouble` and the given text.
template <typename T>
Promise<T> refused(const char* said) {
    return failed<T>(std::make_exception_ptr(Trouble{said}));
}

// One question to one server, retransmissions included; the reflexive address, or an empty one when
// the deadline passes, and `Trouble` when the server refuses. It runs on a pool thread and blocks
// it, on purpose. For TLS that choice would be bad, because there the waiting is per connection and
// grows with their number; here there is one question per socket, asked once, before the
// conversation. And the pool for ordinary work grows without limit, so the hub's completions cannot
// starve.
Address trying(StunClient::Held& held, const Address& where, Clock::time_point until) {
    const Number number = numbered();
    const Bytes what = asked(number);

    // The schedule from the RFC: half a second, then doubling. The deadline for the whole is the
    // `Patience`, so the caller need not know about this schedule.
    Patience wait{500};

    while (Clock::now() < until) {
        held.post.send(where, what, left(until)).get();

        const Clock::time_point edge = std::min(Clock::now() + wait, until);

        while (Clock::now() < edge) {
            Datagram came;

            try {
                came = held.post.receive(1500, left(edge)).get();
            } catch (const Overdue&) {
                break;
            } catch (const Trouble&) {
                // A datagram that is too large, or another problem on the socket, is not the
                // server's answer and must not overturn the question. This socket later carries the
                // peer's traffic too, so one ordinary large packet must not end the whole scouting.
                continue;
            }

            Message made;

            // Not STUN, or someone else's answer: we keep waiting. The transaction ID is the only
            // proof of a response's identity, and it alone decides.
            if (!understood(came.bytes, made)) continue;

            if (made.number != number) continue;

            if (made.sort == Sort::amiss)
                throw Trouble{"The STUN server refused: " +
                                  (made.said.empty() ? std::to_string(made.code) : made.said) + ".",
                              made.code, where};

            if (made.seen.held()) return made.seen;
        }

        wait *= 2;
    }

    return {};
}

// Asks the servers in turn until one gives the reflexive address, within the scout's patience.
// Throws the first server's `Trouble` when all fail that way, or `Overdue` when none answered.
Address hunting(StunClient::Held& held) {
    // The list is copied under the lock. Read in place, it could be reallocated under the range
    // loop if someone called add() at the same time.
    std::vector<Address> servers;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        servers = held.servers;
    }

    if (servers.empty()) throw Trouble{"The scout knows no server."};

    const Clock::time_point until = Clock::now() + held.patience;

    // A refusal or a problem on one server does not end the round. Otherwise a single misconfigured
    // or malicious server at the head of the list would keep every later one from getting a single
    // packet.
    std::exception_ptr first;

    for (const Address& where : servers) {
        if (Clock::now() >= until) break;

        try {
            if (const Address made = trying(held, where, until); made.held()) return made;
        } catch (const Trouble&) {
            if (!first) first = std::current_exception();
        }
    }

    if (first) std::rethrow_exception(first);

    throw Overdue{};
}

}

// A scout that asks through `post` and gives each round `patience`.
StunClient::StunClient(Udp post, Patience patience) : held_(std::make_shared<Held>()) {
    held_->post = std::move(post);
    held_->patience = patience;
}

// Adds a server to ask. An empty scout accepts everything and returns nothing, instead of crashing
// the program; like the other methods, it checks for the state first.
void StunClient::add(Address server) {
    if (!held_) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->servers.push_back(std::move(server));
}

// The servers to ask, by reference, as the header declares. A caller that adds servers from another
// thread while reading this reference cannot hold on to it anyway: the same contract as for every
// value type in talcite.
const std::vector<Address>& StunClient::servers() const {
    static const std::vector<Address> none;

    return held_ ? held_->servers : none;
}

// The reflexive address from the last successful question, or an empty one.
Address StunClient::last() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->guard};

    return held_->last;
}

// The verdict of the last `mapping`, `Mapping::unknown` before one.
StunClient::Mapping StunClient::known() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->guard};

    return held_->known;
}

// Our address as the servers see it. All questions go through the same socket, and the post
// receives one at a time, so a second question started during the first would eat the other's
// answer. We refuse instead of corrupting.
Promise<Address> StunClient::seen() const {
    if (!held_) return refused<Address>("The scout is empty.");

    bool was = false;

    if (!held_->asking.compare_exchange_strong(was, true)) return refused<Address>("The scout is already asking.");

    auto held = held_;

    return run(io(), [held, kept = std::make_shared<Asking>(held)]() -> Address {
        const Address one = hunting(*held);

        {
            const std::lock_guard<std::mutex> also{held->guard};

            held->last = one;
        }

        return one;
    });
}

// Two questions to two different servers, from the same socket. The same address and port in both
// answers mean the NAT assigns the mapping independently of the destination, and hole punching
// makes sense. Different ones mean a symmetric NAT: the address learned from a server is useless
// to the other side, and hole punching will never work, however long one tries.
Promise<StunClient::Mapping> StunClient::mapping() const {
    if (!held_) return refused<Mapping>("The scout is empty.");

    bool was = false;

    if (!held_->asking.compare_exchange_strong(was, true)) return refused<Mapping>("The scout is already asking.");

    auto held = held_;

    return run(io(), [held, kept = std::make_shared<Asking>(held)]() -> Mapping {
        Mapping what = Mapping::unknown;

        std::vector<Address> servers;

        {
            const std::lock_guard<std::mutex> also{held->guard};

            servers = held->servers;
        }

        // The verdict needs two different servers. The same address twice would give two
        // identical answers and from them the verdict "alike", a lie in the dangerous
        // direction: that hole punching will work, although the NAT may be symmetric.
        Address first;
        Address other;

        for (const Address& one : servers) {
            if (!first.held()) {
                first = one;

                continue;
            }

            if (!(one == first)) {
                other = one;

                break;
            }
        }

        // With a single server, "unknown" is a true answer, not a failure.
        if (first.held() && other.held()) {
            // Each question gets its own half of the patience. With a shared deadline a silent
            // first server would eat all of it, the second would not get a single packet, and
            // the mapping would end in a successful "unknown".
            const Patience half{held->patience.count() / 2 + 1};

            const Address one = trying(*held, first, Clock::now() + half);
            const Address two = trying(*held, other, Clock::now() + half);

            if (one.held() && two.held())
                what = one == two ? Mapping::alike : Mapping::symmetric;

            if (one.held()) {
                const std::lock_guard<std::mutex> also{held->guard};

                held->last = one;
            }
        }

        {
            const std::lock_guard<std::mutex> also{held->guard};

            held->known = what;
        }

        return what;
    });
}

}

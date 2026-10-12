#include "talcite/relay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/bell.hpp"
#include "talcite/pool.hpp"
#include "talcite/time.hpp"

#include "stunning.hpp"

namespace talcite::net {

// A channel bound to one peer. `ready` says that the server has confirmed it; until then sending
// goes by indication, because a channel the server has not acknowledged is simply lost.
struct Bound {
    Address peer;
    std::uint16_t number = 0;
    std::chrono::steady_clock::time_point until{};
    std::chrono::steady_clock::time_point tried{};
    bool ready = false;
    bool binding = false;
};

// What the receive loop sees: the same split as in the peer and for the same reason. A pool task
// that outlives its owner and hangs on the hub deadlocks the program at exit.
//
// The name is not `Living`, although the role is the same: that one stands in peer.cpp, also in
// the talcite::net namespace and also at its top level. Two different structures with one name in
// one namespace violate the one definition rule: the linker picks either of them, and the
// destructor of one runs over the fields of the other.
struct Lasting {
    Udp post;
    Address server;
    std::string user;
    std::string word;

    mutable std::mutex guard;
    std::condition_variable stirring;

    std::string realm;
    std::string nonce;
    Bytes key;

    Address lent;
    Address seen;

    std::deque<Datagram> spare;

    // The request in progress. The relay conducts one conversation at a time (allocation, refresh,
    // permission), so instead of a table one place for the number and one for the answer are
    // enough.
    Number number{};
    Message answer;
    bool waiting = false;
    bool answered = false;

    std::chrono::steady_clock::time_point until{};
    std::chrono::steady_clock::time_point tried{};
    std::chrono::steady_clock::time_point beat{};
    bool renewing = false;
    bool lost = false;

    // Permissions expire separately from the allocation: the server keeps them five minutes from
    // the last grant, and traffic does not extend them. So we keep a list of the addresses we have
    // permitted, to renew them together, with one request.
    std::vector<Address> allowed;
    std::chrono::steady_clock::time_point granted{};
    bool permitting = false;

    // Channels. The number is our choice, not the server's: we ask for it, and from then on both
    // sides understand four bytes instead of thirty-six.
    std::vector<Bound> bound;
    std::uint16_t next = 0x4000;

    std::mutex speaking;
    std::mutex talking;

    std::atomic<bool> living{false};
    std::atomic<bool> shutting{false};
};

struct Relay::Held {
    std::shared_ptr<Lasting> alive;

    // Our own copy of the post, used only for closing: copies share the socket but not the object.
    Udp closing;

    // Shuts the relay down: marks it as shutting under the lock the waiters hold, closes the
    // socket and wakes everyone who waits.
    ~Held() {
        {
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

constexpr Patience listening{1000};

// Lends the bell of the pool task running on this thread an ear on the relay's condition variable
// for the length of a wait, so that cancelling the task wakes the wait at once instead of after its
// whole patience. Outside a pool task there is no bell and it does nothing. It must be made before
// the relay's lock is taken and outlive it, because `Bell::heed` must not be called under the
// mutex it is lent.
class Hearing {
public:
    // Lends the ear, remembering the one heard before.
    explicit Hearing(Lasting& held) : bell_(ringing()) {
        if (bell_ != nullptr) was_ = bell_->heed(Bell::Ear{&held.guard, &held.stirring});
    }

    // Gives the previous ear back.
    ~Hearing() {
        if (bell_ != nullptr) (void)bell_->heed(was_);
    }

    Hearing(const Hearing&) = delete;
    Hearing& operator=(const Hearing&) = delete;
    Hearing(Hearing&&) = delete;
    Hearing& operator=(Hearing&&) = delete;

    // Whether the task has been cancelled.
    [[nodiscard]] bool rang() const { return bell_ != nullptr && bell_->rang(); }

private:
    Bell* bell_ = nullptr;
    Bell::Ear was_{};
};

// How much life we ask for the allocation (seconds) and how long before its end we renew it. The
// server may give less than we ask, and then its word counts, which is why the end time is taken
// from the answer, not from the request.
constexpr int living_for = 600;

constexpr int early = 60;

// How many seconds we wait before repeating a failed refresh. Without this gap every round of the
// loop, that is, every second, would start another attempt, because the condition "time to renew"
// stays true forever from the moment of a failed attempt, and tasks would pile up on the
// conversation lock until the end of the allocation.
constexpr int again = 10;

// The server keeps a permission for five minutes. We renew it with a minute to spare.
constexpr int allowing = 240;

// How often (seconds) we knock on the server, so that the path to it does not close up.
constexpr int beating = 15;

// The server keeps a channel for ten minutes. We renew it with a minute to spare.
constexpr int holding = 600;

constexpr int sooner = 60;

// How many channels at once. A peer has one path, so one channel is enough; the margin is for the
// case of one socket serving several peers.
constexpr std::size_t roomiest = 16;

constexpr std::size_t plenty = 256;

// For how many peers we remember a permission in order to renew it. Above this limit a permission
// is granted once and expires after five minutes. Without a limit the list itself would be a place
// into which the caller could pour as much as it liked, and every entry adds to every renewing
// request.
constexpr std::size_t roomier = 64;

// Sends a datagram to `to`, swallowing any failure. Requests and keepalives may be lost: the same
// rule and the same reason as in `Peer`. Otherwise a single rejection from the post (even "the post
// is already sending") would overturn the whole request instead of waiting for the next round of
// retries, and would leave the relay in a waiting state in which it would accept a stranger's
// answer to an old number.
void posting(const std::shared_ptr<Lasting>& held, const Address& to, Bytes what,
             Patience patience) {
    try {
        const std::lock_guard<std::mutex> kept{held->speaking};

        if (held->shutting.load()) return;

        held->post.send(to, std::move(what), patience).await();
    } catch (const std::exception&) {
    }
}

// The time left until `until`, at least one millisecond, so that it can be passed as a patience.
Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

void renewed(const std::shared_ptr<Lasting>& held);

void granting(const std::shared_ptr<Lasting>& held);

void keeping(const std::shared_ptr<Lasting>& held);

void binding(const std::shared_ptr<Lasting>& held, const Address& to);

void channeling(const std::shared_ptr<Lasting>& held);

// One round of the receive loop. A quiet second runs the periodic work (keepalive, refreshing the
// allocation, permissions and channels); a datagram from the server is sorted into channel data,
// indicated data or an answer to the request in progress. Returns false when the socket has
// closed.
bool turning(const std::shared_ptr<Lasting>& held) {
    Datagram came;

    try {
        if (held->shutting.load()) return false;

        came = held->post.receive(roomy, listening).await();
    } catch (const Overdue&) {
        keeping(held);
        renewed(held);
        granting(held);
        channeling(held);

        return true;
    } catch (const std::exception&) {
        return false;
    }

    // An answer comes only from the server. A foreign datagram on this socket may neither pose as
    // an answer nor pass itself off as content from a peer; that is what the address attribute
    // inside the message is for, not where the packet came from.
    if (!(came.from == held->server)) return true;

    // The channel is checked before the message, because the same two bits decide it: STUN has them
    // zero, a channel has 01 there. There is no guessing here.
    {
        std::uint16_t channel = 0;

        Bytes data;

        if (unwrapped(came.bytes, channel, data)) {
            Address whom;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                // We assigned the channel number ourselves, so we take it even when the
                // confirmation has not come back yet: the server may start speaking on it before it
                // answers.
                for (const Bound& one : held->bound)
                    if (one.number == channel) whom = one.peer;

                if (!whom.held()) return true;

                if (held->spare.size() >= plenty) held->spare.pop_front();

                held->spare.push_back(Datagram{whom, std::move(data)});
            }

            held->stirring.notify_all();

            return true;
        }
    }

    Message made;

    if (!understood(came.bytes, made)) return true;

    if (made.sort == Sort::telling && made.task == Task::data) {
        if (!made.peer.held()) return true;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->spare.size() >= plenty) held->spare.pop_front();

            held->spare.push_back(Datagram{made.peer, std::move(made.data)});
        }

        held->stirring.notify_all();

        return true;
    }

    if (made.sort != Sort::told && made.sort != Sort::amiss) return true;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (!held->waiting || made.number != held->number) return true;

        held->answer = std::move(made);
        held->answered = true;
    }

    held->stirring.notify_all();

    return true;
}

// The receive loop. It holds a weak handle and takes a strong one only for a single round, so an
// abandoned relay ends the loop by itself within a second.
void minding(std::weak_ptr<Lasting> weak) {
    for (;;) {
        const std::shared_ptr<Lasting> held = weak.lock();

        if (!held || held->shutting.load()) break;

        if (!turning(held)) break;
    }

    if (const std::shared_ptr<Lasting> held = weak.lock()) {
        held->living.store(false);

        held->stirring.notify_all();
    }
}

// Starts the receive loop on a pool thread, unless it already runs.
void waking(const std::shared_ptr<Lasting>& held) {
    bool was = false;

    if (!held->living.compare_exchange_strong(was, true)) return;

    Promise<void> made{[weak = std::weak_ptr<Lasting>{held}](auto resolve, auto) {
        minding(weak);

        resolve();
    }};

    made.start();
}

// One request with retries, on the STUN schedule: half a second, then doubling. Returns the
// answer; throws `Overdue` when the patience runs out and `Trouble` when the relay closes. It runs
// on a pool thread and blocks it, just like the scout and for the same reason: there is a handful
// of requests for the whole life of an allocation, not one per packet. A cancelled task stops
// waiting at once and throws `Cancelled`.
Message asking(const std::shared_ptr<Lasting>& held, const Bytes& what, const Number& number,
               Patience patience) {
    const std::lock_guard<std::mutex> alone{held->talking};

    const Hearing heard{*held};

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->number = number;
        held->waiting = true;
        held->answered = false;
    }

    const Clock::time_point until = Clock::now() + patience;

    Patience wait{500};

    while (Clock::now() < until) {
        posting(held, held->server, what, left(until));

        std::unique_lock<std::mutex> kept{held->guard};

        const Patience edge = std::min(wait, left(until));

        if (held->stirring.wait_for(kept, edge, [&held, &heard] {
                return held->answered || held->shutting.load() || heard.rang();
            })) {
            if (heard.rang()) {
                held->waiting = false;

                throw Cancelled{};
            }

            if (held->shutting.load()) {
                held->waiting = false;

                throw Trouble{"The relay is closed."};
            }

            Message made = std::move(held->answer);

            held->waiting = false;
            held->answered = false;

            return made;
        }

        wait *= 2;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->waiting = false;
    }

    throw Overdue{};
}

// Adds the identity (USERNAME, REALM, NONCE and MESSAGE-INTEGRITY) to a request. Without
// credentials nothing is added: some servers, including all the fake ones in the tests, allocate
// without authentication.
void naming(const std::shared_ptr<Lasting>& held, Bytes& made) {
    const std::lock_guard<std::mutex> kept{held->guard};

    if (held->key.empty()) return;

    texting(made, 0x0006, held->user);
    texting(made, 0x0014, held->realm);
    texting(made, 0x0015, held->nonce);

    stamping(made, held->key);
}

// Refreshes the allocation when it is due. An allocation expires if nobody renews it, usually
// after ten minutes. We renew it with a minute to spare, on a separate pool task, not here: a
// refresh is a request, and it is exactly this loop that waits for the answer. Refreshing from
// inside the loop would deadlock it on itself.
void renewed(const std::shared_ptr<Lasting>& held) {
    bool gone = false;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->until == Clock::time_point{}) return;

        // A refresh in progress excludes a second one. A flag is used rather than moving the end
        // time, because the end time has to tell the truth about the allocation.
        if (held->renewing) return;

        if (Clock::now() < held->until - std::chrono::seconds{early}) return;

        // An allocation that could not be renewed before its own end is lost. Sending through it
        // any further would be sending into the void, so we say so plainly instead of staying
        // silent.
        if (Clock::now() >= held->until) {
            held->until = Clock::time_point{};
            held->lost = true;

            gone = true;
        } else if (held->tried != Clock::time_point{} &&
                   Clock::now() - held->tried < std::chrono::seconds{again}) {
            return;
        } else {
            held->tried = Clock::now();
            held->renewing = true;
        }
    }

    if (gone) {
        held->stirring.notify_all();

        return;
    }

    Promise<void> made{[held](auto resolve, auto) {
        int lasting = 0;

        try {
            const Number number = numbered();

            Bytes what = begun(Task::refresh, Sort::asking, number);

            wording(what, 0x000D, static_cast<std::uint32_t>(living_for));

            naming(held, what);

            const Message told = asking(held, what, number, Patience{5000});

            if (told.sort == Sort::told)
                lasting = told.lifetime > 0 ? told.lifetime : living_for;
        } catch (const std::exception&) {
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            // The end time is taken from the answer, not from the request. After a failed attempt
            // the old one stays, because it tells the truth about when the server will drop us.
            if (lasting > 0) {
                held->until = Clock::now() + std::chrono::seconds{lasting};
                held->tried = Clock::time_point{};
            }

            held->renewing = false;
        }

        resolve();
    }};

    made.start();
}

// Renews the permissions when they are due, all at once: one request carries as many peer
// addresses as needed, and the server restarts five minutes for each of them. Traffic does not
// extend a permission; without renewal a quiet conversation would die after five minutes, and it
// would look like a network fault, not an expired entry.
void granting(const std::shared_ptr<Lasting>& held) {
    std::vector<Address> whom;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->lost || held->permitting || held->allowed.empty()) return;

        if (Clock::now() - held->granted < std::chrono::seconds{allowing}) return;

        held->granted = Clock::now();
        held->permitting = true;

        whom = held->allowed;
    }

    Promise<void> made{[held, whom](auto resolve, auto) {
        bool done = false;

        try {
            const Number number = numbered();

            Bytes what = begun(Task::permission, Sort::asking, number);

            for (const Address& one : whom) placing(what, 0x0012, one, true);

            naming(held, what);

            done = asking(held, what, number, Patience{5000}).sort == Sort::told;
        } catch (const std::exception&) {
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            // A failed renewal is repeated shortly, not after the whole period; otherwise one lost
            // request would cost the rest of the permission.
            if (!done)
                held->granted = Clock::now() - std::chrono::seconds{allowing - again};

            held->permitting = false;
        }

        resolve();
    }};

    made.start();
}

// Binds a channel to peer `to`, or renews its binding, in the background. We choose the number
// ourselves and keep it at once, even before the answer: thanks to that, content the server
// manages to send on it already has someone to belong to.
//
// A channel binds to an address together with its port, unlike a permission. That is why this is
// not done in `allow` but at the first send: only then is the port known and real.
void binding(const std::shared_ptr<Lasting>& held, const Address& to) {
    std::uint16_t number = 0;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->lost) return;

        bool known = false;

        for (Bound& one : held->bound)
            if (one.peer == to) {
                known = true;

                if (one.binding) return;

                if (one.ready && Clock::now() < one.until - std::chrono::seconds{sooner}) return;

                // A failed binding is repeated every few seconds, not at every send; otherwise a
                // conversation with a server that gives no channels would send a request per
                // datagram.
                if (Clock::now() - one.tried < std::chrono::seconds{again}) return;

                one.binding = true;
                one.tried = Clock::now();
                number = one.number;

                break;
            }

        if (!known) {
            if (held->bound.size() >= roomiest) return;

            number = held->next++;

            if (held->next > 0x7FFF) held->next = 0x4000;

            held->bound.push_back(Bound{to, number, {}, Clock::now(), false, true});
        }
    }

    Promise<void> made{[held, to, number](auto resolve, auto) {
        bool done = false;

        try {
            const Number one = numbered();

            Bytes what = begun(Task::channel, Sort::asking, one);

            wording(what, 0x000C, static_cast<std::uint32_t>(number) << 16);

            placing(what, 0x0012, to, true);

            naming(held, what);

            done = asking(held, what, one, Patience{5000}).sort == Sort::told;
        } catch (const std::exception&) {
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            for (Bound& one : held->bound)
                if (one.number == number) {
                    one.binding = false;

                    if (done) {
                        one.ready = true;
                        one.until = Clock::now() + std::chrono::seconds{holding};
                    }
                }
        }

        resolve();
    }};

    made.start();
}

// Renews the channels that are close to expiring. A channel lives ten minutes and traffic does not
// extend it, just like a permission. An expired one stops working silently, so we renew it in
// advance.
void channeling(const std::shared_ptr<Lasting>& held) {
    std::vector<Address> whom;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->lost) return;

        for (const Bound& one : held->bound)
            if (one.ready && Clock::now() >= one.until - std::chrono::seconds{sooner})
                whom.push_back(one.peer);
    }

    for (const Address& one : whom) binding(held, one);
}

// Knocks on the server every `beating` seconds. The NAT mapping between us and the server usually
// lives thirty seconds from the last traffic, and allocation refreshes go once every nine minutes.
// Without this knocking, after half a minute of silence the server would have no way back to us
// and data from a peer would be lost on the way: the allocation would be alive and yet deaf. A
// Binding indication is made for this: nobody answers it, so there is nothing to wait for and
// nothing to match.
void keeping(const std::shared_ptr<Lasting>& held) {
    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->until == Clock::time_point{}) return;

        if (Clock::now() - held->beat < std::chrono::seconds{beating}) return;

        held->beat = Clock::now();
    }

    try {
        posting(held, held->server, begun(Task::binding, Sort::telling, numbered()), listening);
    } catch (const std::exception&) {
    }
}

// A promise already rejected with `Trouble` carrying `said`.
template <typename T>
Promise<T> amiss_now(const std::string& said) {
    return Promise<T>{
        [said](auto, auto reject) { reject(std::make_exception_ptr(Trouble{said})); }};
}

}

// A relay talking over `post` to the TURN `server`, with the given user name and password (empty
// for a server without authentication). Nothing is sent until `open`.
Relay::Relay(Udp post, Address server, std::string user, std::string word)
    : held_(std::make_shared<Held>()) {
    held_->alive = std::make_shared<Lasting>();
    held_->alive->post = post;
    held_->alive->server = std::move(server);
    held_->alive->user = std::move(user);
    held_->alive->word = std::move(word);
    held_->closing = std::move(post);
}

// The relayed address the server lent us; empty before `open`. An empty relay returns an empty
// address: the relay has a default constructor and `held()`, so calling it on an empty one is
// expected.
Address Relay::lent() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->lent;
}

// Our address as the server sees it (the reflexive address from the allocation); empty before
// `open` and for an empty relay.
Address Relay::seen() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->seen;
}

// Allocates a relayed address and resolves with it. A server that requires authentication first
// answers with a 401 refusal carrying a realm and a nonce, and only from them can the key be
// computed, so the first request always goes unsigned and the second signed. That is what the RFC
// prescribes and how every TURN server works.
Promise<Address> Relay::open(Patience patience) {
    if (!held_) return amiss_now<Address>("The relay is empty.");

    auto held = held_->alive;

    Promise<Address> made{[held, patience](auto resolve, auto reject) {
        try {
            waking(held);

            const Clock::time_point until = Clock::now() + patience;

            Number number = numbered();

            Bytes what = begun(Task::allocate, Sort::asking, number);

            // The relay has to give us an address for UDP; 17 is the number of that protocol.
            wording(what, 0x0019, 17u << 24);

            Message told = asking(held, what, number, left(until));

            if (told.sort == Sort::amiss && (told.code == 401 || told.code == 438)) {
                if (held->user.empty())
                    throw Trouble{"The relay demands credentials and there are none.", told.code};

                {
                    const std::lock_guard<std::mutex> kept{held->guard};

                    held->realm = told.realm;
                    held->nonce = told.nonce;
                    held->key = keyed(held->user, told.realm, held->word);
                }

                number = numbered();

                what = begun(Task::allocate, Sort::asking, number);

                wording(what, 0x0019, 17u << 24);

                naming(held, what);

                told = asking(held, what, number, left(until));
            }

            if (told.sort == Sort::amiss)
                throw Trouble{"The relay refused allocation: " +
                                  (told.said.empty() ? std::to_string(told.code) : told.said) + ".",
                              told.code, held->server};

            if (!told.lent.held()) throw Trouble{"The relay gave no relayed address."};

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                held->lent = told.lent;
                held->seen = told.seen;

                // The end time is taken from the answer, not from the request. The server may give
                // less, and a refresh counted from what we asked for would be late by the
                // difference.
                held->until =
                    Clock::now() +
                    std::chrono::seconds{told.lifetime > 0 ? told.lifetime : living_for};

                held->beat = Clock::now();
            }

            resolve(told.lent);
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Grants a permission for `peer`. The relay does not let through a single byte from someone it has
// not been told about. That is not politeness but the relay's own defense against becoming an open
// amplifier.
Promise<void> Relay::allow(Address peer, Patience patience) {
    if (!held_) return amiss_now<void>("The relay is empty.");

    if (!peer.held()) return amiss_now<void>("An empty address is not a peer.");

    auto held = held_->alive;

    Promise<void> made{[held, peer, patience](auto resolve, auto reject) {
        try {
            // A permission is a request, and the receive loop recognizes the answer to it, so the
            // loop has to run. It usually runs since the allocation, but that cannot be relied on:
            // otherwise a permission granted as the first move would wait for an answer nobody
            // reads.
            waking(held);

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->lost) throw Trouble{"The allocation is lost."};
            }

            const Number number = numbered();

            Bytes what = begun(Task::permission, Sort::asking, number);

            placing(what, 0x0012, peer, true);

            naming(held, what);

            const Message told = asking(held, what, number, patience);

            if (told.sort == Sort::amiss)
                throw Trouble{"The relay refused permission: " +
                                  (told.said.empty() ? std::to_string(told.code) : told.said) + ".",
                              told.code, held->server};

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                bool known = false;

                for (const Address& one : held->allowed)
                    if (one.family() == peer.family() && one.bytes() == peer.bytes()) known = true;

                // A permission covers an address, not a port, as the RFC prescribes. That is why it
                // is remembered by address: a second port of the same host is already let through.
                if (!known && held->allowed.size() < roomier) {
                    // The renewal clock starts only from the first entry. Resetting it at every
                    // following one would postpone the renewal of those that have waited long.
                    if (held->allowed.empty()) held->granted = Clock::now();

                    held->allowed.push_back(peer);
                }
            }

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Sends `what` to `to` through the relay, on a channel when one is bound. Sending goes by
// indication, not by request: there is no answer to it and nothing to wait for. It gets lost just
// like an ordinary datagram, because it is an ordinary datagram, only wrapped.
Promise<void> Relay::send(Address to, Bytes what, Patience patience) {
    if (!held_) return amiss_now<void>("The relay is empty.");

    if (!to.held()) return amiss_now<void>("An empty address is no destination.");

    auto held = held_->alive;

    Promise<void> made{[held, to, what = std::move(what), patience](auto resolve,
                                                                   auto reject) mutable {
        try {
            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->lost) throw Trouble{"The allocation is lost."};
            }

            std::uint16_t channel = 0;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                for (const Bound& one : held->bound)
                    if (one.peer == to && one.ready && Clock::now() < one.until)
                        channel = one.number;
            }

            if (channel != 0) {
                posting(held, held->server, wrapped(channel, what), patience);

                resolve();

                return;
            }

            // There is no channel yet, so this parcel goes by indication, and the channel is
            // ordered in the background. Waiting for it would cost the whole conversation as much
            // as one request to the server, while an indication works from the first byte.
            binding(held, to);

            Bytes one = begun(Task::sending, Sort::telling, numbered());

            placing(one, 0x0012, to, true);

            giving(one, what);

            posting(held, held->server, std::move(one), patience);

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Receives the next datagram relayed from a peer, at most `most` bytes; rejects with `Overdue`
// when nothing arrives within the patience, and with `Trouble` when the allocation is lost, the
// relay is closed or the datagram does not fit. Cancelling the promise ends the wait at once.
Promise<Datagram> Relay::receive(std::size_t most, Patience patience) {
    if (!held_) return amiss_now<Datagram>("The relay is empty.");

    auto held = held_->alive;

    Promise<Datagram> made{[held, most, patience](auto resolve, auto reject) {
        try {
            waking(held);

            const Hearing heard{*held};

            std::unique_lock<std::mutex> kept{held->guard};

            if (!held->stirring.wait_for(kept, patience, [&held, &heard] {
                    return !held->spare.empty() || held->lost || held->shutting.load() ||
                           !held->living.load() || heard.rang();
                }))
                throw Overdue{};

            if (heard.rang()) throw Cancelled{};

            // What was received earlier is handed over even when the allocation is already lost:
            // those bytes really arrived and nobody invalidated them.
            if (held->spare.empty() && held->lost) throw Trouble{"The allocation is lost."};

            if (held->spare.empty()) throw Trouble{"The relay is closed."};

            if (held->spare.front().bytes.size() > most)
                throw Trouble{"The parcel does not fit in the given space."};

            Datagram one = std::move(held->spare.front());

            held->spare.pop_front();

            kept.unlock();

            resolve(std::move(one));
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Closes the relay; the destructor of `Held` does the work, as when the last handle lets go.
void Relay::close() { held_.reset(); }

}

#include "tal/net.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

#include "tal/trouble.hpp"

#include "netting.hpp"
#include "platform/offload.hpp"

// The network of tal: addresses, names, TCP as a `Stream`, listeners and UDP, on the reactor of
// netting.hpp. Every operation is a task that waits for one request; a task cancelled while it
// waits takes its request back, so its socket is not held busy for ever. A socket closed gives
// its number and its port back at once; since the system gives that number to the next socket,
// a request starts only while its socket is open, which the reactor checks under its lock.
namespace tal::net {

namespace netting {

// Holds the socket `one`, open.
Socket::Socket(Handle one) : which_(one) {}

// The last holder: closed, if it was not.
Socket::~Socket() { close(); }

// Closes the socket once: its waiting requests are refused, and so are those made before that come
// later; its conversation is shut, and its number goes to the reactor, which gives it back to the
// system once nothing of the socket is in flight.
void Socket::close() {
    const Handle one = which_.exchange(nothing);

    if (one == nothing) return;

    forget(one);
    retiring(one);
}

// A request of the kind `step` on the socket `given`, which it holds, or on none.
Doing::Doing(std::shared_ptr<Socket> given, Step step) : socket(std::move(given)), held(socket != nullptr ? socket->now() : nothing), kind(step) {}

// The task at `handle` of `chain` waits for the request; a chain cancelled meanwhile is doomed at
// its suspension, as at any leaf.
void Doing::waited(const std::shared_ptr<detail::Chain>& chain, std::coroutine_handle<> handle) {
    const std::lock_guard<std::mutex> locked{guard_};

    chain_ = chain;
    handle_ = handle;
    chain->suspended();
    armed_ = true;
}

// The request ended with `moved` bytes and the code `why` (0 when it went well), told once by
// whoever had it last; the task waiting is woken on its executor. One whose awaiter has gone lets
// go of what it made.
void Doing::done(std::size_t moved, int why) {
    bool lost = false;

    {
        const std::lock_guard<std::mutex> locked{guard_};

        over_ = true;
        got = moved;
        code = why;

        if (gone_) {
            lost = true;
        } else if (armed_) {
            armed_ = false;
            chain_->resume(handle_);
        }
    }

    if (lost) abandoned();
}

// The awaiter is gone, with the result `taken` or not; whether the request was over. One that
// ended but whose result nobody took lets go of what it made.
bool Doing::left(bool taken) {
    bool over = false;

    {
        const std::lock_guard<std::mutex> locked{guard_};

        gone_ = true;
        armed_ = false;
        over = over_;
    }

    if (over && !taken) abandoned();

    return over;
}

// Whether the awaiter is gone.
bool Doing::gone() {
    const std::lock_guard<std::mutex> locked{guard_};

    return gone_;
}

// Whether the socket of the request was closed since the request was made.
bool Doing::closed() const { return socket != nullptr && socket->now() != held; }

// Closes the connection an accept made for nobody.
void Doing::abandoned() { ending(std::exchange(made, nothing)); }

// The awaiter goes: a request that did not end is taken back from the reactor.
Awaited::~Awaited() {
    if (!one_->left(finished_)) (void)withdraw(one_);
}

// The addresses `all` in the order to try them: the families by turns, starting with the family
// of the first, each family in its own order.
std::vector<Address> ordered(std::vector<Address> all) {
    if (all.empty()) return all;

    const Family first = all.front().family();
    std::vector<Address> ours;
    std::vector<Address> others;

    for (Address& one : all) (one.family() == first ? ours : others).push_back(std::move(one));

    std::vector<Address> made;

    for (std::size_t at = 0; at < ours.size() || at < others.size(); ++at) {
        if (at < ours.size()) made.push_back(std::move(ours[at]));
        if (at < others.size()) made.push_back(std::move(others[at]));
    }

    return made;
}

// Connects to the first of `all` that answers, in the order of `ordered`; the trouble of the last
// one when none does.
Task<Connection> tried(std::vector<Address> all) {
    std::exception_ptr last;

    for (Address& one : ordered(std::move(all))) {
        try {
            co_return co_await connect(std::move(one));
        } catch (const Trouble&) {
            last = std::current_exception();
        }
    }

    if (last) std::rethrow_exception(last);

    throw Trouble{Kind::missing, "There is no address to connect to."};
}

}

namespace {

using netting::Doing;
using netting::Handle;
using netting::nothing;
using netting::Socket;
using netting::Step;

// A direction of a socket taken by a task, for as long as the task waits on it: one reader and one
// writer at a time, since two would interleave their bytes. The task lets it go when it ends or is
// cancelled, so the next one is not refused.
class Marking {
public:
    // Takes the direction `slot` under `guard`; throws `Trouble` saying `busy` when it is taken.
    Marking(std::mutex& guard, bool& slot, const char* busy) : guard_(guard), slot_(slot) {
        const std::lock_guard<std::mutex> kept{guard_};

        if (slot_) throw Trouble{Kind::busy, busy};

        slot_ = true;
    }

    // Lets the direction go.
    ~Marking() {
        const std::lock_guard<std::mutex> kept{guard_};

        slot_ = false;
    }

    Marking(const Marking&) = delete;
    Marking& operator=(const Marking&) = delete;

private:
    std::mutex& guard_;
    bool& slot_;
};

// What a connection shares with its tasks: its socket, the other side, and who has each direction.
struct Wire {
    std::shared_ptr<Socket> socket;
    Address peer;
    std::mutex guard;
    bool reading = false;
    bool writing = false;
};

// Throws `Trouble` that the socket of `peer` is closed.
[[noreturn]] void closed(const Address& peer) { throw Trouble{Kind::broken, "The connection is closed.", peer.spelled()}; }

// At most `most` bytes from the connection `held`, as soon as some come; nothing once the other
// side has ended.
Task<Bytes> reading(std::shared_ptr<Wire> held, std::size_t most) {
    if (most == 0) throw Trouble{Kind::other, "A read of nothing says nothing."};

    if (held->socket->now() == nothing) closed(held->peer);

    const std::shared_ptr<Doing> one = std::make_shared<Doing>(held->socket, Step::getting);

    one->buffer.resize(most);

    const Marking marking{held->guard, held->reading, "The connection has a reader already."};

    co_await netting::Awaited{one};

    if (one->code != 0) netting::failed("Cannot read", one->code, held->peer);

    one->buffer.resize(one->got);

    co_return std::move(one->buffer);
}

// Writes all of `what` to the connection `held`; a break says how much got out.
Task<void> writing(std::shared_ptr<Wire> held, Bytes what) {
    if (held->socket->now() == nothing) closed(held->peer);

    const std::shared_ptr<Doing> one = std::make_shared<Doing>(held->socket, Step::putting);
    const std::size_t all = what.size();

    one->buffer = std::move(what);

    const Marking marking{held->guard, held->writing, "The connection has a writer already."};

    co_await netting::Awaited{one};

    if (one->code != 0) {
        const std::string sentence = netting::spoken(one->code);

        throw Trouble{netting::kind_of(one->code),
                      "Cannot write: " + sentence + ". " + std::to_string(one->got) + " of " + std::to_string(all) + " bytes went out.",
                      held->peer.spelled(), one->code};
    }
}

// A TCP connection, or one of a local socket, as a channel of a `Stream`. Its last stream closes
// it, and the other side reads the end.
class Connected : public Channel {
public:
    // The connection on `socket` to `peer`.
    Connected(std::shared_ptr<Socket> socket, Address peer) : held_(std::make_shared<Wire>()) {
        held_->socket = std::move(socket);
        held_->peer = std::move(peer);
    }

    Connected(const Connected&) = delete;
    Connected& operator=(const Connected&) = delete;

    // At most `most` bytes, as soon as some come.
    Task<Bytes> read(std::size_t most) override { return reading(held_, most); }

    // All of `what`.
    Task<void> write(Bytes what) override { return writing(held_, std::move(what)); }

    // Closes the connection; what waits on it is refused.
    void close() override { held_->socket->close(); }

private:
    std::shared_ptr<Wire> held_;
};

// The connection on `socket` to `peer`, with the system's word on our end, and on a local socket
// whether the other side runs as our user.
Connection connection(std::shared_ptr<Socket> socket, const Address& peer) {
    const Handle one = socket->now();
    Connection made;

    made.mine = netting::named(one, false);
    made.peer = peer;

    if (peer.family() == Family::local || made.mine.family() == Family::local) made.kindred = netting::kindred(one);

    made.stream = Stream{std::make_shared<Connected>(std::move(socket), made.peer)};

    return made;
}

}

// The addresses of the name `host` with the port `port`, of `family` or any; the system's resolver
// blocks, so it is asked on the io pool, and a cancelled task wakes from it.
Task<std::vector<Address>> resolve(std::string host, std::uint16_t port, Family family) {
    co_return co_await platform::offloaded([&host, port, family] { return netting::looked(host, port, family); });
}

// Connects to `where`.
Task<Connection> connect(Address where) {
    if (!where.held()) throw Trouble{Kind::other, "An empty address is no destination."};

    const Handle made = netting::opened(where.family(), true);

    if (made == nothing) {
        const int why = netting::amiss();

        netting::failed("Cannot open a socket", why, where);
    }

    auto socket = std::make_shared<Socket>(made);
    const std::shared_ptr<Doing> one = std::make_shared<Doing>(socket, Step::joining);

    one->where = where;

    co_await netting::Awaited{one};

    if (one->code != 0) netting::failed("Cannot connect", one->code, where);

    co_return connection(std::move(socket), where);
}

// Connects to the name `host` at the port `port`: its addresses one after another, IPv6 and IPv4 by
// turns, until one answers.
Task<Connection> connect(std::string host, std::uint16_t port) {
    std::vector<Address> all = co_await resolve(std::move(host), port);

    co_return co_await netting::tried(std::move(all));
}

// What a listener shares with its tasks: its socket, its address, and who accepts now.
struct Listener::Held {
    // The last holder closes it.
    ~Held() { close(); }

    // Closes the socket once, and takes the file of a local socket away.
    void close() {
        socket->close();

        if (!removed.exchange(true)) netting::removed(mine);
    }

    std::shared_ptr<Socket> socket;
    Address mine;
    std::mutex guard;
    bool accepting = false;
    std::atomic<bool> removed{false};
};

// A listener at `where`, with at most `waiting` connections waiting to be accepted. A server's
// port can be taken again at once after it closes; the stale file of a local socket whose program
// is gone is taken away.
Task<Listener> Listener::opened(Address where, int waiting) {
    if (!where.held()) throw Trouble{Kind::other, "An empty address is nowhere to listen."};

    const Handle made = netting::opened(where.family(), true);

    if (made == nothing) {
        const int why = netting::amiss();

        netting::failed("Cannot open a socket", why, where);
    }

    if (where.family() != Family::local) netting::sharing(made);

    if (!netting::listening(made, where, waiting)) {
        const int why = netting::amiss();

        netting::ending(made);
        netting::failed("Cannot listen", why, where);
    }

    auto held = std::make_shared<Held>();

    held->socket = std::make_shared<Socket>(made);
    held->mine = where.family() == Family::local ? where : netting::named(made, false);

    co_return Listener{std::move(held)};
}

// The next connection; one accept at a time. A connection accepted for a task that went away
// meanwhile is closed.
Task<Connection> Listener::accepted() const {
    return [](std::shared_ptr<Held> held) -> Task<Connection> {
        if (held == nullptr) throw Trouble{Kind::missing, "The listener holds nothing."};

        if (held->socket->now() == nothing) throw Trouble{Kind::broken, "The listener is closed.", held->mine.spelled()};

        const std::shared_ptr<Doing> one = std::make_shared<Doing>(held->socket, Step::meeting);
        const Marking marking{held->guard, held->accepting, "The listener accepts already."};

        co_await netting::Awaited{one};

        if (one->code != 0) netting::failed("Cannot accept a connection", one->code, held->mine);

        auto socket = std::make_shared<Socket>(std::exchange(one->made, nothing));

        co_return connection(std::move(socket), one->where);
    }(held_);
}

// The address it listens at, with the port the system chose for port 0.
Address Listener::mine() const { return held_ == nullptr ? Address{} : held_->mine; }

// Closes it: a waiting accept is refused, and connections accepted before live on.
void Listener::close() {
    if (held_ != nullptr) held_->close();
}

// What a UDP socket shares with its tasks: its socket, its address, and who sends and who receives.
struct Udp::Held {
    // The last holder closes it.
    ~Held() { socket->close(); }

    std::shared_ptr<Socket> socket;
    Address mine;
    std::mutex guard;
    bool sending = false;
    bool receiving = false;
};

// A UDP socket bound to `where`.
Task<Udp> Udp::bound(Address where) {
    if (where.family() != Family::four && where.family() != Family::six) throw Trouble{Kind::other, "UDP binds to IPv4 or IPv6 only."};

    const Handle made = netting::opened(where.family(), false);

    if (made == nothing) {
        const int why = netting::amiss();

        netting::failed("Cannot open a socket", why, where);
    }

    if (!netting::bound(made, where)) {
        const int why = netting::amiss();

        netting::ending(made);
        netting::failed("Cannot bind", why, where);
    }

    auto held = std::make_shared<Held>();

    held->socket = std::make_shared<Socket>(made);
    held->mine = netting::named(made, false);

    co_return Udp{std::move(held)};
}

// Sends `what` to `to` as one datagram; one send at a time.
Task<void> Udp::send(Address to, Bytes what) const {
    return [](std::shared_ptr<Held> held, Address target, Bytes given) -> Task<void> {
        if (held == nullptr) throw Trouble{Kind::missing, "The socket holds nothing."};

        if (held->socket->now() == nothing) throw Trouble{Kind::broken, "The socket is closed.", held->mine.spelled()};

        const std::shared_ptr<Doing> one = std::make_shared<Doing>(held->socket, Step::sending);

        one->where = std::move(target);
        one->buffer = std::move(given);

        const Marking marking{held->guard, held->sending, "The socket sends already."};

        co_await netting::Awaited{one};

        if (one->code != 0) netting::failed("Cannot send", one->code, one->where);
    }(held_, std::move(to), std::move(what));
}

// The next datagram, at most `most` bytes of it, with who sent it; a longer one comes cut, and
// says so. One receive at a time.
Task<Datagram> Udp::received(std::size_t most) const {
    return [](std::shared_ptr<Held> held, std::size_t room) -> Task<Datagram> {
        if (held == nullptr) throw Trouble{Kind::missing, "The socket holds nothing."};

        if (held->socket->now() == nothing) throw Trouble{Kind::broken, "The socket is closed.", held->mine.spelled()};

        const std::shared_ptr<Doing> one = std::make_shared<Doing>(held->socket, Step::fetching);

        one->buffer.resize(room);

        const Marking marking{held->guard, held->receiving, "The socket receives already."};

        co_await netting::Awaited{one};

        if (one->code != 0) netting::failed("Cannot receive", one->code, held->mine);

        one->buffer.resize(one->got);

        co_return Datagram{std::move(one->where), std::move(one->buffer), one->cut};
    }(held_, most);
}

// The address it is bound to, with the port the system chose for port 0.
Address Udp::mine() const { return held_ == nullptr ? Address{} : held_->mine; }

// Closes it: what waits on it is refused.
void Udp::close() {
    if (held_ != nullptr) held_->socket->close();
}

}

#include "tal/testing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "talcite/address.hpp"
#include "exchange.hpp"
#include "piping.hpp"
#include "talcite/fs.hpp"
#include "talcite/listener.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"

#include <iostream>

#ifndef _WIN32
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace talcite;

namespace {

// The patience every operation gets unless a trial needs a different one, in milliseconds.
const net::Patience soon{4000};


// A gate on the loopback with a port from the system, so that two tests running at once never
// fight over the same number.
net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

// Both sides start at once, because a network promise begins in its constructor. Only then do we
// wait; otherwise waiting for the accept would hold up the connect that is supposed to trigger it.
struct Pair {
    net::Stream server;
    net::Stream client;
};

// Connects a client to `gate` and returns both ends of the connection.
Pair joined(const net::Listener& gate) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream server = std::move(meeting).get();
    net::Stream client = std::move(joining).get();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Gate: gets a port from the system") {
    const net::Listener gate = standing();

    OUGHT(gate.held());
    OUGHT(gate.mine().port() != 0);
    SAME(gate.mine().text(), std::string{"127.0.0.1"});
}

TRIAL("Gate: rejects an empty address") {
    bool threw = false;

    try {
        const net::Listener gate{net::Address{}};

        (void)gate;
    } catch (const net::Trouble&) {
        threw = true;
    }

    OUGHT(threw);
}

TRIAL("Stream: connect and accept meet") {
    const net::Listener gate = standing();

    const Pair both = joined(gate);

    OUGHT(both.server.held());
    OUGHT(both.client.held());

    SAME(both.client.peer().port(), gate.mine().port());
    SAME(both.server.mine().port(), gate.mine().port());
    SAME(both.server.peer().port(), both.client.mine().port());
}

TRIAL("Stream: bytes pass both ways") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(bytes("witaj"), soon).get();

    SAME(text(both.server.read(64, soon).get()), std::string{"witaj"});

    both.server.write(bytes("i wzajemnie"), soon).get();

    SAME(text(both.client.read(64, soon).get()), std::string{"i wzajemnie"});
}

TRIAL("Stream: a larger batch arrives whole") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::string much(200000, 'x');

    auto writing = both.client.write(bytes(much), soon);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).get();

        if (one.empty()) break;

        made += text(one);
    }

    std::move(writing).get();

    SAME(made.size(), much.size());
    OUGHT(made == much);
}

TRIAL("Stream: the other side closing gives emptiness") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.close();

    SAME(static_cast<int>(both.server.read(64, soon).get().size()), 0);
}

TRIAL("Stream: silence ends with the deadline, not with emptiness") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    bool late = false;

    try {
        (void)both.server.read(64, net::Patience{80}).get();
    } catch (const Overdue&) {
        late = true;
    }

    OUGHT(late);
}

TRIAL("Stream: a second read at the same time is refused") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    auto first = both.server.read(64, net::Patience{300});

    bool busy = false;

    try {
        (void)both.server.read(64, net::Patience{300}).get();
    } catch (const net::Trouble&) {
        busy = true;
    }

    OUGHT(busy);

    try {
        (void)std::move(first).get();
    } catch (const Overdue&) {
    }
}

TRIAL("Stream: after a read one can read again") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(bytes("raz"), soon).get();

    SAME(text(both.server.read(64, soon).get()), std::string{"raz"});

    both.client.write(bytes("dwa"), soon).get();

    SAME(text(both.server.read(64, soon).get()), std::string{"dwa"});
}

TRIAL("Stream: closing interrupts a waiting read") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    auto reading = both.server.read(64, soon);

    both.server.close();

    bool amiss = false;

    try {
        (void)std::move(reading).get();
    } catch (const net::Trouble&) {
        amiss = true;
    } catch (const Overdue&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Stream: connecting to a closed port is rejected") {
    net::Address where;

    {
        const net::Listener gate = standing();

        where = gate.mine();
    }

    bool amiss = false;

    try {
        (void)net::Stream::connect(where, net::Patience{1000}).get();
    } catch (const net::Trouble&) {
        amiss = true;
    } catch (const Overdue&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Stream: an empty address is not a target") {
    bool amiss = false;

    try {
        (void)net::Stream::connect(net::Address{}, soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Stream: a closed one accepts no requests") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.close();

    bool amiss = false;

    try {
        (void)both.client.read(8, soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Gate: accepts several connections in turn") {
    const net::Listener gate = standing();

    for (int which = 0; which < 3; ++which) {
        Pair both = joined(gate);

        both.client.write(bytes(std::to_string(which)), soon).get();

        SAME(text(both.server.read(8, soon).get()), std::to_string(which));
    }
}

TRIAL("Exchange: when all is done nobody waits") {
    const net::Listener gate = standing();

    {
        Pair both = joined(gate);

        both.client.write(bytes("hej"), soon).get();

        (void)both.server.read(8, soon).get();
    }

    SOON(net::Exchange::running().waiting() <= 1);
}

// The exchange holds one request per socket per direction, so a second accept must be refused
// rather than overwrite the first: otherwise the first promise would never settle, its deadline
// would cease to exist, and the gate and its descriptor would stay until the end of the program.
TRIAL("Gate: a second accept is refused and the first lives on") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto first = gate.accept(net::Patience{3000});

    bool busy = false;

    try {
        (void)gate.accept(net::Patience{3000}).get();
    } catch (const net::Trouble&) {
        busy = true;
    }

    OUGHT(busy);

    const net::Stream client = net::Stream::connect(gate.mine(), net::Patience{3000}).get();

    const net::Stream server = std::move(first).get();

    SAME(server.peer(), client.mine());
}

// The caller gives the size, so allocating the buffer can throw. An exception thrown after the
// busy flag is taken must not leave the flag set, or the socket would never read again.
TRIAL("Stream: an exception from allocation does not block reading forever") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(net::Patience{3000});

    const net::Stream client = net::Stream::connect(gate.mine(), net::Patience{3000}).get();
    const net::Stream server = std::move(waiting).get();

    bool threw = false;

    try {
        (void)server.read(static_cast<std::size_t>(-1), net::Patience{1000});
    } catch (const std::exception&) {
        threw = true;
    }

    OUGHT(threw);

    client.write(bytes("po wyjątku"), net::Patience{3000}).get();

    SAME(text(server.read(64, net::Patience{3000}).get()), std::string{"po wyjątku"});
}

// A zero-length read must be refused: recv with zero length returns zero even on a healthy socket,
// and zero without an error means "the other side closed" here.
TRIAL("Stream: a zero-length read does not pretend to be the end of the stream") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(soon);

    const net::Stream client = net::Stream::connect(gate.mine(), soon).get();
    const net::Stream server = std::move(waiting).get();

    client.write(bytes("osiem!!!"), soon).get();

    bool amiss = false;

    try {
        (void)server.read(0, soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);

    SAME(text(server.read(64, soon).get()), std::string{"osiem!!!"});
}

// After closing, every method answers politely; these have to as well, instead of violating memory.
TRIAL("Stream: addresses after closing are empty and do not blow up") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(soon);

    net::Stream client = net::Stream::connect(gate.mine(), soon).get();

    const net::Stream server = std::move(waiting).get();

    client.close();

    OUGHT(!client.peer().held());
    OUGHT(!client.mine().held());

    net::Listener other{net::Address::loopback(net::Family::four, 0)};

    other.close();

    OUGHT(!other.mine().held());
}

// --- Cancelling -------------------------------------------------------------------------------
//
// A network request is also the promise's ticket, so `cancel()` has to remove it from the exchange,
// not just reject it. Otherwise an interrupted read would leave the socket's direction busy
// forever: the caller would stop waiting, while the stream answered "busy" until the end of the
// program and kept the descriptor open through `kept`.

namespace {

// On Unix systems the direction is freed within `cancel()` itself. On Windows only once the
// interrupted request comes back from the completion port, so we keep trying for a while instead of
// assuming either one. Returns empty bytes if no read succeeds within 200 tries.
Bytes patiently(const net::Stream& one, std::size_t most) {
    for (int tried = 0; tried < 200; ++tried) {
        auto next = one.read(most, soon);

        try {
            return std::move(next).get();
        } catch (const net::Trouble&) {
            later(std::chrono::milliseconds{5}).get();
        }
    }

    return Bytes{};
}

// Waits, for up to ten seconds, until the exchange has at most `below` requests waiting.
void settling(std::size_t below) {
    (void)tal::testing::came([below] { return net::Exchange::running().waiting() <= below; });
}

}

TRIAL("Stream: a cancelled read frees the direction") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    auto first = both.server.read(64, net::Patience{0});

    first.cancel();

    both.client.write(bytes("hej"), soon).get();

    SAME(text(patiently(both.server, 8)), std::string{"hej"});
}

TRIAL("Exchange: cancelling removes the request") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::size_t before = net::Exchange::running().waiting();

    auto one = both.server.read(64, net::Patience{0});

    SOON(net::Exchange::running().waiting() > before);

    one.cancel();

    settling(before);

    SAME(net::Exchange::running().waiting(), before);
}

// A gate has one request for itself, so an abandoned accept must not shut it for good: every later
// accept would hear "busy", and connections would stand in the system's queue.
TRIAL("Gate: a cancelled accept does not shut the gate") {
    const net::Listener gate = standing();

    const std::size_t before = net::Exchange::running().waiting();

    auto first = gate.accept(net::Patience{0});

    first.cancel();

    settling(before);

    Pair both = joined(gate);

    both.client.write(bytes("znowu"), soon).get();

    SAME(text(both.server.read(8, soon).get()), std::string{"znowu"});
}

// --- Cancelling frees the place at once ----------------------------------------------------------

// Cancelling is the only moment the caller learns about it, so right after it the caller may ask
// for the next operation and must get it. There is deliberately no waiting in between: waiting
// would hide a flag that is cleared only by a late completion on a pool thread.
//
// On Unix systems `withdraw` takes the request out of the exchange and reports success, so there
// these trials always pass. On Windows CancelIoEx only asks and never reports success, and only
// there does it show whether the place was really freed.
TRIAL("Gate: after cancelling one can accept again at once") {
    const net::Listener gate = standing();

    auto first = gate.accept(net::Patience{0});

    first.cancel();

    Pair both = joined(gate);

    both.client.write(bytes("znowu"), soon).get();

    SAME(text(both.server.read(8, soon).get()), std::string{"znowu"});
}

// Reading, not writing: a short write on the loopback finishes before it can be cancelled, so it
// would check nothing here. A read stands until something arrives, and that is why the
// cancellation hits an operation that is really in flight.
TRIAL("Stream: a hundred cancelled reads do not occupy the direction for good") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    for (int again = 0; again < 100; ++again) {
        auto one = both.server.read(64, net::Patience{0});

        one.cancel();
    }

    both.client.write(bytes("po stu"), soon).get();

    SAME(text(patiently(both.server, 6)), std::string{"po stu"});
}

// A hundred turns, because the window is narrow. If any cancellation left the place occupied, the
// next accept would hear "The gate is already accepting.", and that is the only thing this trial
// watches for.
TRIAL("Gate: a hundred cancellations in a row do not occupy it for good") {
    const net::Listener gate = standing();

    for (int again = 0; again < 100; ++again) {
        auto one = gate.accept(net::Patience{0});

        one.cancel();
    }

    Pair both = joined(gate);

    both.client.write(bytes("po stu"), soon).get();

    SAME(text(both.server.read(8, soon).get()), std::string{"po stu"});
}

// A write after disconnection must not kill the process. The default reaction to SIGPIPE is death,
// and a network library that kills the caller's program because the peer hung up is unusable;
// unprotected, the program dies with exit code 141 before it can report a single write.
//
// The trial is also the check that the process survived: if the signal came back, the whole test
// binary would die here and never reach the end.
TRIAL("Stream: a write after disconnection is refused instead of killing the process") {
    const net::Listener gate = standing();

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = std::move(meeting).get();
    net::Stream two = std::move(joining).get();

    one.close();

    // More than the socket buffer holds; otherwise the first write would go into the buffer and
    // never touch the broken connection.
    const Bytes much(4 * 1024 * 1024, std::byte{0x41});

    bool told = false;

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         !told && std::chrono::steady_clock::now() < latest;) {
        try {
            two.write(much, soon).get();
        } catch (const std::exception&) {
            told = true;
        }
    }

    OUGHT(told);
}

// --- Local socket ------------------------------------------------------------------------------

namespace {

// A path in the temporary directory, unique within the run. Not in the current directory, because
// the trial set may run from a read-only place.
std::string nearby(const std::string& what) {
    static std::atomic<unsigned> counted{0};

    fs::path where = fs::temp();

    where /= "talcite-" + what + "-" + std::to_string(counted.fetch_add(1)) + ".sock";

    return where.string();
}

}

TRIAL("Local socket: the conversation goes by path, not by port") {
    const std::string said = nearby("rozmowa");

    const net::Listener gate{net::Address::local(said)};

    SAME(gate.mine().family(), net::Family::local);
    SAME(gate.mine().path(), said);

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = std::move(meeting).get();
    net::Stream client = std::move(joining).get();

    client.write(bytes("dzień dobry"), soon).get();

    SAME(text(server.read(64, soon).get()), std::string{"dzień dobry"});

    // The peer on this side knows the gate's path; our own end has no name, and that is the truth,
    // not something missing.
    SAME(client.peer().path(), said);
}

// On the other side of a local socket is the same user: the system says so, and the sharing daemon
// asks about it before it answers anything. A network connection has nothing to say here: empty.
// Windows tells through the peer's process number; a system that does not give it (old ones, Wine)
// also gives empty, not false.
TRIAL("Local socket: the peer is the same user, and on the network it is empty") {
    const std::string said = nearby("swoj");

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    const net::Stream server = std::move(meeting).get();
    const net::Stream client = std::move(joining).get();

#ifdef _WIN32
    OUGHT(!server.kindred().has_value() || *server.kindred());

    // A pipe tells about the peer on both sides.
    if (net::pipes::chosen()) {
        OUGHT(server.kindred() == std::optional<bool>{true});
        OUGHT(client.kindred() == std::optional<bool>{true});
    }
#else
    OUGHT(server.kindred() == std::optional<bool>{true});
    OUGHT(client.kindred() == std::optional<bool>{true});
#endif

    const net::Listener far = standing();
    const Pair both = joined(far);

    OUGHT(!both.server.kindred().has_value());
    OUGHT(!both.client.kindred().has_value());
    OUGHT(!net::Stream{}.kindred().has_value());
}

#ifndef _WIN32

// A peer of another user: a child that becomes someone else before connecting. Only an
// administrator can change the user, so without those rights the trial skips itself.
TRIAL("Local socket: a peer of another user is not us") {
    if (::geteuid() != 0) {
        std::cout << "        without administrator rights one cannot become another user, trial skipped\n";

        return;
    }

    const std::string said = nearby("obcy");

    const net::Listener gate{net::Address::local(said)};

    ::chmod(said.c_str(), 0777);

    auto meeting = gate.accept(soon);

    const pid_t child = ::fork();

    if (child == 0) {
        if (::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(3);

        const int one = ::socket(AF_UNIX, SOCK_STREAM, 0);

        sockaddr_un where{};

        where.sun_family = AF_UNIX;

        std::memcpy(where.sun_path, said.c_str(), said.size() + 1);

        if (::connect(one, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0) ::_exit(4);

        char got = 0;

        (void)!::read(one, &got, 1);

        ::_exit(0);
    }

    const net::Stream server = std::move(meeting).get();

    OUGHT(server.kindred() == std::optional<bool>{false});

    server.write(bytes("k"), soon).get();

    int status = 0;

    ::waitpid(child, &status, 0);

    OUGHT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#endif

// A named pipe creates no file at all, and then there is nothing to clean up.
TRIAL("Local socket: the gate cleans up its file") {
    const std::string said = nearby("sprzatanie");

    {
        const net::Listener gate{net::Address::local(said)};

        SAME(gate.mine().path(), said);
        OUGHT(fs::exists(said).get() != net::pipes::chosen());
    }

    OUGHT(!fs::exists(said).get());
}

// Several clients connect before the gate starts accepting, and all of them get through. With a
// pipe, a client for which there is no free instance waits for one instead of being refused,
// because the name does not disappear.
TRIAL("Local socket: several peers at once, also before accepting") {
    const std::string said = nearby("kilku");

    const net::Listener gate{net::Address::local(said)};

    auto first = net::Stream::connect(net::Address::local(said), soon);
    auto second = net::Stream::connect(net::Address::local(said), soon);
    auto third = net::Stream::connect(net::Address::local(said), soon);

    std::vector<net::Stream> servers;

    for (int turn = 0; turn < 3; ++turn) servers.push_back(gate.accept(soon).get());

    std::vector<net::Stream> clients{std::move(first).get(), std::move(second).get(), std::move(third).get()};

    for (std::size_t at = 0; at < clients.size(); ++at) clients[at].write(bytes(std::to_string(at)), soon).get();

    std::vector<std::string> heard;

    for (net::Stream& one : servers) heard.push_back(text(one.read(16, soon).get()));

    std::sort(heard.begin(), heard.end());

    SAME(heard, (std::vector<std::string>{"0", "1", "2"}));

    // An answer and the end: for the client a closed server side is emptiness after reading the
    // rest.
    servers.front().write(bytes("pa"), soon).get();
    servers.front().close();
    servers.erase(servers.begin());

    bool answered = false;

    for (net::Stream& one : clients) {
        Bytes got;

        try {
            got = one.read(16, net::Patience{300}).get();
        } catch (const std::exception&) {
        }

        if (text(got) == "pa") {
            answered = true;

            OUGHT(one.read(16, soon).get().empty());
        }
    }

    OUGHT(answered);
}

// Closing one copy of a stream ends the conversation even while the other copy still lives, like
// `shutdown` on a socket. An empty write along the way does not pretend to be the end.
TRIAL("Local socket: closing a copy ends the conversation, and an empty write does not") {
    const std::string said = nearby("koniec");

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = std::move(meeting).get();
    net::Stream client = std::move(joining).get();

    client.write(Bytes{}, soon).get();
    client.write(bytes("x"), soon).get();

    SAME(text(server.read(16, soon).get()), std::string{"x"});

    const net::Stream twin = client;

    // The read waits before the other side closes; the end arrives to the one waiting.
    auto waiting = server.read(16, soon);

    client.close();

    OUGHT(std::move(waiting).get().empty());
    OUGHT(twin.held());
}

// An accept cancelled, then a new one: the client reaches the new one, not an instance beside it
// that nobody waits on. Several times, because with a pipe it is a matter of which instance the
// client hits. The pause after cancelling is for the pipe: a cancelled instance keeps listening
// until the system gives it back, and a client that hits it at that moment gets a broken pipe and
// connects again.
TRIAL("Local socket: after a cancelled accept the client reaches the next one") {
    const std::string said = nearby("anulowane");

    const net::Listener gate{net::Address::local(said)};

    for (int turn = 0; turn < 6; ++turn) {
        auto dropped = gate.accept(soon);

        dropped.cancel();

        std::this_thread::sleep_for(std::chrono::milliseconds{100});

        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(net::Address::local(said), soon);

        net::Stream server = std::move(meeting).get();
        net::Stream client = std::move(joining).get();

        client.write(bytes("tak"), soon).get();

        SAME(text(server.read(16, soon).get()), std::string{"tak"});
    }
}

// A closed gate gives up its path at once, also with an accept in progress, and a conversation
// connected a moment ago does not, once closed, leave the gate without a free place: the next
// client waits to be accepted.
TRIAL("Local socket: a closed gate gives up its path, and between accepts there is room for a client") {
    const std::string said = nearby("oddana");

    {
        net::Listener gate{net::Address::local(said)};

        auto waiting = gate.accept(soon);
        const auto closed = std::chrono::steady_clock::now();

        gate.close();

        bool told = false;

        try {
            (void)std::move(waiting).get();
        } catch (const std::exception&) {
            told = true;
        }

        // At once, not after the deadline: closing interrupts the waiting accept.
        OUGHT(told);
        OUGHT(std::chrono::steady_clock::now() - closed < std::chrono::seconds{2});
    }

    const net::Listener again{net::Address::local(said)};

    {
        auto meeting = again.accept(soon);
        auto joining = net::Stream::connect(net::Address::local(said), soon);

        net::Stream server = std::move(meeting).get();
        net::Stream client = std::move(joining).get();

        server.close();
        client.close();
    }

    // Between accepts, after closing the only conversation: the client connects before the gate
    // starts accepting.
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    auto meeting = again.accept(soon);

    net::Stream client = std::move(joining).get();
    net::Stream server = std::move(meeting).get();

    client.write(bytes("jestem"), soon).get();

    SAME(text(server.read(16, soon).get()), std::string{"jestem"});
}

#ifdef _WIN32

// Windows paths ignore case, and so does a pipe for the same path spelled differently.
TRIAL("Local socket: on Windows the path ignores case") {
    const std::string said = nearby("Wielkie");

    std::string lowered = said;

    for (char& one : lowered)
        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(lowered), soon);

    (void)std::move(meeting).get();
    (void)std::move(joining).get();
}

#endif

TRIAL("Local socket: a dead file does not block listening") {
    const std::string said = nearby("martwy");

    // A file nobody listens under, exactly what a program that crashed leaves behind.
    fs::write(said, Bytes{}).get();

    OUGHT(fs::exists(said).get());

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = std::move(meeting).get();
    net::Stream client = std::move(joining).get();

    client.write(bytes("stoję"), soon).get();

    SAME(text(server.read(16, soon).get()), std::string{"stoję"});
}

TRIAL("Local socket: a second gate does not cover a live one") {
    const std::string said = nearby("zajete");

    const net::Listener first{net::Address::local(said)};

    bool told = false;

    try {
        const net::Listener second{net::Address::local(said)};
    } catch (const net::Trouble&) {
        told = true;
    }

    OUGHT(told);

    // The first gate has to keep working; refusing the second must not clean up its file.
    auto meeting = first.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    (void)std::move(meeting).get();
    (void)std::move(joining).get();
}

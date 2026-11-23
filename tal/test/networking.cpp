#include <array>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/net.hpp"
#include "tal/process.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "netting.hpp"
#include "quieting.hpp"

// The network of tal::net, all of it on the loopback: addresses, names, TCP as a stream with its
// ends, breaks and cancellations, listeners, UDP, local sockets and who is on their other side,
// and many connections waited for by one thread. IPv6 is tried where the system has it.

namespace {

using tal::net::Address;
using tal::net::Connection;
using tal::net::Family;
using tal::net::Listener;
using tal::net::Udp;
using tal::Path;

using namespace std::chrono_literals;

// The kind of trouble a task fails with, or none when it does not fail.
template <typename T>
std::optional<tal::Kind> refused(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// What the trouble a task fails with says, or nothing when it does not fail.
template <typename T>
std::string said(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return why.what();
    }

    return {};
}

// Waits `much` on the loop.
void paused(tal::Loop& loop, std::chrono::milliseconds much) {
    tal::wait(loop, [](std::chrono::milliseconds wait) -> tal::Task<void> { co_await tal::after(wait); }(much));
}

// A request of the kind `kind` on `socket`, with room for `much` bytes, made by hand.
std::shared_ptr<tal::net::netting::Doing> asked(const std::shared_ptr<tal::net::netting::Socket>& socket, tal::net::netting::Step kind,
                                                std::size_t much = 16) {
    auto one = std::make_shared<tal::net::netting::Doing>(socket, kind);

    one->buffer.resize(much);

    return one;
}

// A UDP socket of IPv4 on the loopback, made by hand.
std::shared_ptr<tal::net::netting::Socket> datagrams() {
    namespace netting = tal::net::netting;

    const netting::Handle raw = netting::opened(Family::four, false);

    OUGHT(netting::bound(raw, Address::loopback(Family::four, 0)));

    return std::make_shared<netting::Socket>(raw);
}

// The kind of trouble making an address throws, or none.
template <typename Making>
std::optional<tal::Kind> unmade(Making making) {
    try {
        (void)making();
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// A listener on the loopback of `family` at a port the system picks.
Listener listening(tal::Loop& loop, Family family = Family::four) { return tal::wait(loop, Listener::opened(Address::loopback(family, 0))); }

// Both ends of one connection to `listener`: the one that connected, then the one accepted.
std::pair<Connection, Connection> paired(tal::Loop& loop, const Listener& listener) {
    auto [near, far] = tal::wait(loop, tal::all(tal::net::connect(listener.mine()), listener.accepted()));

    return {std::move(near), std::move(far)};
}

// Whether the system has IPv6 on its loopback.
bool sixed(tal::Loop& loop) { return refused(loop, Listener::opened(Address::loopback(Family::six, 0))) == std::nullopt; }

// `much` bytes of a pattern that does not repeat within a read, starting at `seed`.
tal::Bytes patterned(std::size_t much, int seed) {
    tal::Bytes made(much);

    for (std::size_t at = 0; at < much; ++at) made[at] = static_cast<std::byte>((at * 7 + static_cast<std::size_t>(seed)) % 251);

    return made;
}

// Reads from `from` until `much` bytes came or the stream ended.
tal::Task<tal::Bytes> gathered(tal::Stream from, std::size_t much) {
    tal::Bytes all;

    while (all.size() < much) {
        const tal::Bytes piece = co_await from.read(65536);

        if (piece.empty()) break;

        all.insert(all.end(), piece.begin(), piece.end());
    }

    co_return all;
}

// Writes `what` to `into` and gives how many bytes it wrote.
tal::Task<std::size_t> written(tal::Stream into, tal::Bytes what) {
    const std::size_t much = what.size();

    co_await into.write(std::move(what));

    co_return much;
}

// A path for a local socket in a fresh folder of the system's temporary folder.
Path socketed(const std::string& name) {
    static std::atomic<int> numbered{0};
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("tal-net-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) +
                                                  "-" + std::to_string(++numbered));

    std::filesystem::create_directories(folder);

    const std::u8string spelled = (folder / name).generic_u8string();

    return Path::restored(std::string{spelled.begin(), spelled.end()});
}

// Whether a file stands at `where`.
bool standing(const Path& where) {
    const std::string said{where.stored()};

    return std::filesystem::exists(std::filesystem::path{std::u8string{said.begin(), said.end()}});
}

// How many threads the program has; 0 where the trials cannot tell.
std::size_t threads() {
#ifdef __linux__
    std::size_t many = 0;

    for (const auto& one : std::filesystem::directory_iterator{"/proc/self/task"}) {
        (void)one;
        ++many;
    }

    return many;
#else
    return 0;
#endif
}

}

TRIAL("Address: IPv4 and IPv6 are read and written back, with the port and the interface") {
    const Address four{"192.168.1.10", 8080};

    SAME(four.family(), Family::four);
    SAME(four.text(), std::string{"192.168.1.10"});
    SAME(four.spelled(), std::string{"192.168.1.10:8080"});
    SAME(four.port(), std::uint16_t{8080});
    SAME(Address(four.text(), four.port()), four);

    const Address six{"[::1]", 80};

    SAME(six.family(), Family::six);
    SAME(six.text(), std::string{"::1"});
    SAME(six.spelled(), std::string{"[::1]:80"});
    SAME(Address("::1", 80), six);

    const Address scoped{"fe80::1%3", 5};

    SAME(scoped.scope(), std::uint32_t{3});
    SAME(scoped.text(), std::string{"fe80::1%3"});
    SAME(scoped.spelled(), std::string{"[fe80::1%3]:5"});
    SAME(Address("[fe80::1%3]", 5), scoped);
    OUGHT(scoped != Address("fe80::1", 5));

    Address moved = four;

    moved.port(9);

    SAME(moved.spelled(), std::string{"192.168.1.10:9"});
    OUGHT(moved < four);
}

TRIAL("Address: the loopback, every interface, mapped IPv4 and an empty address") {
    SAME(Address::loopback(Family::four, 5).spelled(), std::string{"127.0.0.1:5"});
    SAME(Address::loopback(Family::six, 5).spelled(), std::string{"[::1]:5"});
    SAME(Address::any(Family::four, 0).text(), std::string{"0.0.0.0"});
    SAME(Address::any(Family::six, 7).spelled(), std::string{"[::]:7"});

    SAME(Address("::ffff:10.0.0.1", 7).unmapped(), Address("10.0.0.1", 7));
    SAME(Address("::1", 7).unmapped(), Address("::1", 7));
    SAME(Address("::fffe:10.0.0.1", 7).unmapped(), Address("::fffe:10.0.0.1", 7));
    SAME(Address("10.0.0.1", 7).unmapped(), Address("10.0.0.1", 7));

    const Address none;

    OUGHT(!none.held());
    SAME(none.family(), Family::none);
    SAME(none.text(), std::string{});
    SAME(none.spelled(), std::string{});
    SAME(Address(Family::none, {}, 5), none);

    std::array<std::byte, 16> raw{};

    raw[0] = std::byte{10};
    raw[3] = std::byte{1};
    raw[9] = std::byte{9};

    SAME(Address(Family::four, raw, 80), Address("10.0.0.1", 80));
}

TRIAL("Address: a local one is its path; names, garbage and what has no meaning are refused") {
    const Path where{"/tmp/tal.sock"};
    const Address local = Address::local(where);

    SAME(local.family(), Family::local);
    SAME(local.path(), tal::net::netting::pathed(where));
    SAME(local.text(), local.path());
    SAME(local.spelled(), local.path());
    SAME(local.port(), std::uint16_t{0});

    OUGHT(unmade([] { return Address{"example.com", 80}; }) == tal::Kind::malformed);
    OUGHT(unmade([] { return Address{std::string_view{"1.2.3.4\0x", 9}, 80}; }) == tal::Kind::malformed);
    OUGHT(unmade([] { return Address{"1.2.3.4%3", 80}; }) == tal::Kind::malformed);
    OUGHT(unmade([] { return Address{"fe80::1%nosuchinterface", 80}; }) == tal::Kind::malformed);
    OUGHT(unmade([] { return Address{"[::1", 80}; }) == tal::Kind::malformed);
    OUGHT(unmade([] { return Address{"", 80}; }) == tal::Kind::malformed);
    OUGHT(unmade([] { return Address::local(Path{""}); }) == tal::Kind::other);
    OUGHT(unmade([] { return Address::local(Path{"/" + std::string(103, 'x')}); }) == tal::Kind::other);
    OUGHT(unmade([] { return Address::local(Path{"/" + std::string(102, 'x')}); }) == std::nullopt);
    OUGHT(unmade([] { return Address(Family::local, {}, 0); }) == tal::Kind::other);
    OUGHT(unmade([] { return Address::any(Family::local, 0); }) == tal::Kind::other);
    OUGHT(unmade([] { return Address::loopback(Family::none, 0); }) == tal::Kind::other);
    OUGHT(unmade([&] {
              Address one = local;

              one.port(1);

              return one;
          }) == tal::Kind::other);
    OUGHT(unmade([] {
              Address one{"1.2.3.4", 1};

              one.scope(1);

              return one;
          }) == tal::Kind::other);
    OUGHT(unmade([] {
              Address one;

              one.port(1);

              return one;
          }) == tal::Kind::other);
}

// A name nobody knows is missing. The answer comes from the system's server of names, which may
// fail for now when many ask at once; that is busy, and it is asked again, a little later each
// time. A server that still cannot answer has said all it can, and busy is then the answer. A
// wrong answer is told with the system's words and code.
TRIAL("Names: a number is its own address, a name nobody knows is missing, families by turns") {
    tal::Loop loop;

    const auto unknown = [&loop] {
        std::string made = "found";

        for (int round = 1; round <= 5; ++round) {
            try {
                (void)tal::wait(loop, tal::net::resolve("nosuch.invalid", 80));

                return std::string{"found"};
            } catch (const tal::Trouble& why) {
                made = std::string{tal::spoken(why.kind())};

                if (why.kind() == tal::Kind::missing) return made;

                if (why.kind() != tal::Kind::busy) return made + ": " + why.what() + " (" + std::to_string(why.code()) + ")";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{200 * round});
        }

        return made;
    };

    SAME(tal::wait(loop, tal::net::resolve("127.0.0.1", 80)), std::vector<Address>{Address("127.0.0.1", 80)});
    SAME(tal::wait(loop, tal::net::resolve("127.0.0.1", 80, Family::four)), std::vector<Address>{Address("127.0.0.1", 80)});
    if (const std::string answer = unknown(); answer != tal::spoken(tal::Kind::busy)) SAME(answer, std::string{tal::spoken(tal::Kind::missing)});
    OUGHT(refused(loop, tal::net::resolve("localhost", 80, Family::local)) == tal::Kind::unsupported);

    for (const Address& one : tal::wait(loop, tal::net::resolve("localhost", 81))) SAME(one.port(), std::uint16_t{81});

    SAME(tal::wait(loop, tal::net::resolve("fe80::1%1", 80)), std::vector<Address>{Address("fe80::1%1", 80)});

    const Address a6{"::1", 1};
    const Address b6{"::2", 1};
    const Address c6{"::3", 1};
    const Address a4{"10.0.0.1", 1};
    const Address b4{"10.0.0.2", 1};

    SAME(tal::net::netting::ordered({a6, b6, c6, a4, b4}), (std::vector<Address>{a6, a4, b6, b4, c6}));
    SAME(tal::net::netting::ordered({a4, a6, b6, b4}), (std::vector<Address>{a4, a6, b4, b6}));
    SAME(tal::net::netting::ordered({}), std::vector<Address>{});
}

TRIAL("TCP: what one end writes the other reads, both ways, and the addresses match") {
    tal::Loop loop;
    const Listener listener = listening(loop);

    OUGHT(listener.held());
    OUGHT(listener.mine().port() != 0);
    SAME(listener.mine().text(), std::string{"127.0.0.1"});

    auto [near, far] = paired(loop, listener);

    SAME(near.peer, listener.mine());
    SAME(far.mine, listener.mine());
    SAME(far.peer, near.mine);
    OUGHT(!near.kindred.has_value());
    OUGHT(!far.kindred.has_value());

    tal::wait(loop, near.stream.write(tal::bytes("ping")));
    SAME(tal::text::of(tal::wait(loop, gathered(far.stream, 4))), std::string{"ping"});

    tal::wait(loop, far.stream.write(tal::bytes("pong")));
    SAME(tal::text::of(tal::wait(loop, gathered(near.stream, 4))), std::string{"pong"});

    tal::wait(loop, near.stream.write({}));
}

TRIAL("TCP: IPv6 on its loopback, where the system has it") {
    tal::Loop loop;

    if (!sixed(loop)) return;

    const Listener listener = listening(loop, Family::six);

    SAME(listener.mine().text(), std::string{"::1"});

    auto [near, far] = paired(loop, listener);

    tal::wait(loop, near.stream.write(tal::bytes("six")));
    SAME(tal::text::of(tal::wait(loop, gathered(far.stream, 3))), std::string{"six"});
    SAME(far.peer.family(), Family::six);

    const Listener both = tal::wait(loop, Listener::opened(Address::any(Family::six, 0)));
    const Connection four = tal::wait(loop, tal::net::connect(Address::loopback(Family::four, both.mine().port())));
    const Connection taken = tal::wait(loop, both.accepted());

    SAME(taken.peer.unmapped(), four.mine);
}

TRIAL("TCP: large transfers go both ways at once, whole and in order") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);
    constexpr std::size_t much = 8 * 1024 * 1024;
    const tal::Bytes there = patterned(much, 1);
    const tal::Bytes back = patterned(much, 2);

    auto [sent, returned, got, gotten] = tal::wait(loop, tal::all(written(near.stream, there), written(far.stream, back), gathered(far.stream, much),
                                                                  gathered(near.stream, much)));

    SAME(sent, much);
    SAME(returned, much);
    OUGHT(got == there);
    OUGHT(gotten == back);
}

TRIAL("TCP: a close is the end for the other side; a closed stream refuses, and so does a busy one") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);

    tal::wait(loop, near.stream.write(tal::bytes("last")));
    near.stream.close();

    SAME(tal::text::of(tal::wait(loop, gathered(far.stream, 100))), std::string{"last"});
    SAME(tal::wait(loop, far.stream.read(10)).size(), std::size_t{0});

    OUGHT(refused(loop, near.stream.read(1)) == tal::Kind::broken);
    OUGHT(refused(loop, near.stream.write(tal::bytes("x"))) == tal::Kind::broken);
    SAME(said(loop, near.stream.read(1)), std::string{"The connection is closed."});
    OUGHT(refused(loop, far.stream.read(0)) == tal::Kind::other);

    auto [one, two] = paired(loop, listener);
    tal::Scope scope{loop};
    std::atomic<int> ended{0};

    scope.run([](tal::Stream from, std::atomic<int>& into) -> tal::Task<void> {
        try {
            (void)co_await from.read(1);
        } catch (const tal::Trouble& why) {
            if (why.kind() == tal::Kind::broken) ++into;
        }
    }(one.stream, ended));

    QUIETED(loop);

    OUGHT(refused(loop, one.stream.read(1)) == tal::Kind::busy);

    one.stream.close();

    UNTIL(loop, ended.load() == 1);
}

TRIAL("TCP: the last stream gone closes the connection") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);

    far.stream = tal::Stream{};

    SAME(tal::wait(loop, near.stream.read(10)).size(), std::size_t{0});
}

TRIAL("TCP: a port nobody listens at refuses, and a side that went away breaks the writer") {
    tal::Loop loop;
    Address gone;

    {
        Listener listener = listening(loop);

        gone = listener.mine();
        listener.close();
    }

    OUGHT(refused(loop, tal::net::connect(gone)) == tal::Kind::refused);
    OUGHT(refused(loop, tal::net::connect(Address{})) == tal::Kind::other);

    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);

    far.stream.close();

    std::optional<tal::Kind> why;

    for (int turn = 0; turn < 200 && !why; ++turn) {
        why = refused(loop, near.stream.write(tal::bytes(std::string(1024, 'x'))));

        if (!why) tal::wait(loop, []() -> tal::Task<void> { co_await tal::after(5ms); }());
    }

    OUGHT(why == tal::Kind::broken);
}

// The addresses of a name are tried as Happy Eyeballs tries them: one that refuses holds the next
// back for the stagger of 250 ms at most (Windows takes two seconds to give up on a refused
// connection), and not at all where the refusal comes at once; the first that answers ends the
// trying, so a later address is never asked.
TRIAL("TCP: of several addresses a refusing one holds the next back briefly, and the first to answer wins") {
    tal::Loop loop;
    Address gone;

    {
        Listener listener = listening(loop);

        gone = listener.mine();
        listener.close();
    }

    Listener asked = listening(loop);
    Listener spare = listening(loop);
    const auto refusing = std::chrono::steady_clock::now();

    OUGHT(refused(loop, tal::net::connect(gone)) == tal::Kind::refused);

    const auto refusal = std::chrono::steady_clock::now() - refusing;
    const auto started = std::chrono::steady_clock::now();
    const Connection made = tal::wait(loop, tal::net::netting::tried({gone, asked.mine()}));
    const auto took = std::chrono::steady_clock::now() - started;

    OUGHT(took < 1500ms);
    OUGHT(made.peer == asked.mine());

    // Where a refusal comes at once, the next address does not wait out the stagger.
    if (refusal < 50ms) OUGHT(took < 200ms);

    const Connection again = tal::wait(loop, tal::net::netting::tried({asked.mine(), spare.mine()}));
    bool waited_out = false;

    OUGHT(again.peer == asked.mine());

    try {
        (void)tal::wait(loop, tal::within(400ms, spare.accepted()));
    } catch (const tal::Overdue&) {
        waited_out = true;
    }

    OUGHT(waited_out);

    std::string said;

    try {
        (void)tal::wait(loop, tal::net::netting::tried({}));
    } catch (const tal::Trouble& why) {
        said = why.what();
    }

    SAME(said, std::string{"There is no address to connect to."});
}

#ifdef __linux__
// An address that does not answer at all, a listener whose queue is full (Linux drops the requests
// over it, and the one asking waits), holds the next address back for the stagger and no longer;
// two of them hold it back for two staggers, each counted from the start of the one before.
TRIAL("TCP: an address that does not answer gives way to the next after the stagger, each in turn") {
    tal::Loop loop;
    const Listener full = tal::wait(loop, Listener::opened(Address::loopback(Family::four, 0), 0));
    std::vector<Connection> filling;
    bool hangs = false;

    for (int one = 0; one < 4 && !hangs; ++one) {
        try {
            filling.push_back(tal::wait(loop, tal::within(200ms, tal::net::connect(full.mine()))));
        } catch (const tal::Overdue&) {
            hangs = true;
        }
    }

    OUGHT(hangs);

    const Listener asked = listening(loop);
    auto started = std::chrono::steady_clock::now();
    const Connection one = tal::wait(loop, tal::net::netting::tried({full.mine(), asked.mine()}));
    const auto first = std::chrono::steady_clock::now() - started;

    OUGHT(one.peer == asked.mine());
    OUGHT(first >= 250ms && first < 1500ms);

    started = std::chrono::steady_clock::now();

    const Connection two = tal::wait(loop, tal::net::netting::tried({full.mine(), full.mine(), asked.mine()}));
    const auto second = std::chrono::steady_clock::now() - started;

    OUGHT(two.peer == asked.mine());
    OUGHT(second >= 500ms && second < 1500ms);
}
#endif

TRIAL("TCP: a read cut by within() is taken back; the connection works on, the bytes come to the next") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);
    const std::size_t before = tal::net::netting::watching();

    OUGHT(refused(loop, tal::within(30ms, far.stream.read(10))) == tal::Kind::overdue);
    UNTIL(loop, tal::net::netting::watching() == before);

    tal::wait(loop, near.stream.write(tal::bytes("after")));
    SAME(tal::text::of(tal::wait(loop, gathered(far.stream, 5))), std::string{"after"});

    OUGHT(refused(loop, tal::within(30ms, far.stream.read(10))) == tal::Kind::overdue);
    OUGHT(refused(loop, tal::within(30ms, near.stream.read(10))) == tal::Kind::overdue);

    tal::wait(loop, far.stream.write(tal::bytes("still")));
    SAME(tal::text::of(tal::wait(loop, gathered(near.stream, 5))), std::string{"still"});
}

TRIAL("TCP: a task waits on the loop it runs on, and comes back to it") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);

    const bool same = tal::wait(loop, [](tal::Stream from, tal::Stream into) -> tal::Task<bool> {
        const std::thread::id before = std::this_thread::get_id();

        co_await tal::all(written(into, tal::bytes("x")), gathered(from, 1));

        co_return std::this_thread::get_id() == before;
    }(far.stream, near.stream));

    OUGHT(same);
}

// The server counts a conversation after its own write, which may end after the client has its
// echo; both counts are waited for.
TRIAL("Listener: many connections at once, each with its own conversation") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    constexpr int many = 20;
    std::atomic<int> served{0};
    std::atomic<int> answered{0};
    tal::Scope scope{loop};

    scope.run([](Listener from, std::atomic<int>& into) -> tal::Task<void> {
        for (int at = 0; at < many; ++at) {
            Connection one = co_await from.accepted();
            tal::Bytes got = co_await one.stream.read(64);

            co_await one.stream.write(std::move(got));
            ++into;
        }
    }(listener, served));

    for (int at = 0; at < many; ++at)
        scope.run([](Address where, int number, std::atomic<int>& into) -> tal::Task<void> {
            Connection one = co_await tal::net::connect(where);
            const std::string said = "number " + std::to_string(number);

            co_await one.stream.write(tal::bytes(said));

            if (tal::text::of(co_await gathered(one.stream, said.size())) == said) ++into;
        }(listener.mine(), at, answered));

    UNTIL(loop, answered.load() == many && served.load() == many);
}

TRIAL("Listener: a close refuses the waiting accept and frees the port; a taken port is busy; one accept at a time") {
    tal::Loop loop;
    Listener listener = listening(loop);

    OUGHT(refused(loop, Listener::opened(listener.mine())) == tal::Kind::busy);
    OUGHT(refused(loop, Listener::opened(Address{})) == tal::Kind::other);

    tal::Scope scope{loop};
    std::atomic<int> refusals{0};

    scope.run([](Listener from, std::atomic<int>& into) -> tal::Task<void> {
        try {
            (void)co_await from.accepted();
        } catch (const tal::Trouble& why) {
            if (why.kind() == tal::Kind::broken) ++into;
        }
    }(listener, refusals));

    QUIETED(loop);

    OUGHT(refused(loop, listener.accepted()) == tal::Kind::busy);

    listener.close();

    UNTIL(loop, refusals.load() == 1);
    OUGHT(refused(loop, listener.accepted()) == tal::Kind::broken);
    OUGHT(refused(loop, Listener{}.accepted()) == tal::Kind::missing);
    OUGHT(!Listener{}.mine().held());
    UNTIL(loop, !refused(loop, Listener::opened(listener.mine())));
}

TRIAL("Listener: its port can be taken again at once, while its connections wait out their end") {
    tal::Loop loop;
    Address where;

    {
        Listener listener = listening(loop);

        where = listener.mine();

        auto [near, far] = paired(loop, listener);

        far.stream.close();
        SAME(tal::wait(loop, near.stream.read(10)).size(), std::size_t{0});
        near.stream.close();
        paused(loop, 50ms);
        listener.close();
    }

    const Listener again = tal::wait(loop, Listener::opened(where));

    SAME(again.mine(), where);
}

TRIAL("Listener: a cancelled accept is taken back, and the next accept is not refused") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    const std::size_t before = tal::net::netting::watching();

    OUGHT(refused(loop, tal::within(30ms, listener.accepted())) == tal::Kind::overdue);
    UNTIL(loop, tal::net::netting::watching() == before);

    auto [near, far] = paired(loop, listener);

    tal::wait(loop, near.stream.write(tal::bytes("in")));
    SAME(tal::text::of(tal::wait(loop, gathered(far.stream, 2))), std::string{"in"});
}

// The side of a connection a raw listener `gate` accepted, waiting until it comes.
tal::net::netting::Handle met(tal::net::netting::Handle gate) {
    tal::net::netting::Handle made = tal::net::netting::nothing;

    SOON([&] {
        (void)tal::net::netting::taken(gate, made);

        return made != tal::net::netting::nothing;
    }());

    return made;
}

TRIAL("Listener: a connection accepted for nobody is closed, whichever comes first") {
    namespace netting = tal::net::netting;

    tal::Loop loop;
    const netting::Handle gate = netting::opened(Family::four, true);

    OUGHT(netting::listening(gate, Address::loopback(Family::four, 0), 4));

    const Address where = netting::named(gate, false);

    for (const bool late : {false, true}) {
        const Connection near = tal::wait(loop, tal::net::connect(where));
        const auto one = std::make_shared<netting::Doing>(nullptr, netting::Step::meeting);

        one->made = met(gate);

        if (late) {
            OUGHT(!one->left(false));
            one->done(0, 0);
        } else {
            one->done(0, 0);
            OUGHT(one->left(false));
        }

        SAME(one->made, netting::nothing);
        SAME(tal::wait(loop, near.stream.read(10)).size(), std::size_t{0});
    }

    const Connection near = tal::wait(loop, tal::net::connect(where));
    const auto taken = std::make_shared<netting::Doing>(nullptr, netting::Step::meeting);

    taken->made = met(gate);
    taken->done(0, 0);

    OUGHT(taken->left(true));
    OUGHT(taken->made != netting::nothing);
    OUGHT(refused(loop, tal::within(30ms, near.stream.read(10))) == tal::Kind::overdue);

    netting::ending(taken->made);
    netting::ending(gate);
}

TRIAL("UDP: a datagram goes and comes with its sender; a longer one is cut and says so") {
    tal::Loop loop;
    const Udp one = tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)));
    const Udp two = tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)));

    OUGHT(one.held());
    OUGHT(one.mine().port() != 0);

    tal::wait(loop, one.send(two.mine(), tal::bytes("hello")));

    const tal::net::Datagram got = tal::wait(loop, two.received());

    SAME(tal::text::of(got.bytes), std::string{"hello"});
    SAME(got.from, one.mine());
    OUGHT(!got.cut);

    tal::wait(loop, two.send(one.mine(), tal::bytes(std::string(100, 'y'))));

    const tal::net::Datagram cut = tal::wait(loop, one.received(10));

    SAME(cut.bytes.size(), std::size_t{10});
    OUGHT(cut.cut);
    SAME(cut.from, two.mine());

    tal::wait(loop, one.send(two.mine(), {}));

    const tal::net::Datagram empty = tal::wait(loop, two.received());

    OUGHT(empty.bytes.empty());
    OUGHT(!empty.cut);
    OUGHT(refused(loop, one.send(two.mine(), tal::Bytes(70000))) == tal::Kind::other);
}

TRIAL("UDP: a cancelled receive is taken back; one receive at a time; a closed socket refuses and frees its port") {
    tal::Loop loop;
    Udp one = tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)));
    const Udp two = tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)));
    const std::size_t before = tal::net::netting::watching();

    OUGHT(refused(loop, tal::within(30ms, one.received())) == tal::Kind::overdue);
    UNTIL(loop, tal::net::netting::watching() == before);

    tal::Scope scope{loop};
    std::atomic<int> refusals{0};

    scope.run([](Udp from, std::atomic<int>& into) -> tal::Task<void> {
        try {
            (void)co_await from.received();
        } catch (const tal::Trouble& why) {
            if (why.kind() == tal::Kind::broken) ++into;
        }
    }(one, refusals));

    QUIETED(loop);

    OUGHT(refused(loop, one.received()) == tal::Kind::busy);

    one.close();

    UNTIL(loop, refusals.load() == 1);
    UNTIL(loop, !refused(loop, Udp::bound(one.mine())));
    OUGHT(refused(loop, one.send(two.mine(), tal::bytes("x"))) == tal::Kind::broken);
    OUGHT(refused(loop, one.received()) == tal::Kind::broken);
    OUGHT(refused(loop, Udp{}.received()) == tal::Kind::missing);
    OUGHT(refused(loop, Udp{}.send(two.mine(), {})) == tal::Kind::missing);
    OUGHT(refused(loop, Udp::bound(Address::local(Path{"/tmp/x"}))) == tal::Kind::other);
    OUGHT(!Udp{}.mine().held());
}

TRIAL("Local socket: a conversation with our own user, whose file goes with the listener") {
    tal::Loop loop;
    const Path where = socketed("talk.sock");
    const Address here = Address::local(where);
    Listener listener = tal::wait(loop, Listener::opened(here));

    SAME(listener.mine(), here);
    OUGHT(standing(where));

    auto [near, far] = tal::wait(loop, tal::all(tal::net::connect(here), listener.accepted()));

    SAME(near.peer, here);
    SAME(near.kindred, std::optional<bool>{true});
    SAME(far.kindred, std::optional<bool>{true});

    tal::wait(loop, near.stream.write(tal::bytes("local")));
    SAME(tal::text::of(tal::wait(loop, gathered(far.stream, 5))), std::string{"local"});

    OUGHT(refused(loop, Listener::opened(here)) == tal::Kind::busy);

    listener.close();

    OUGHT(!standing(where));

    try {
        (void)tal::wait(loop, tal::net::connect(here));
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::missing)
            ::tal::testing::fell(__FILE__, __LINE__, "not missing: " + std::string{tal::spoken(why.kind())} + ", " + why.what() + " (" + std::to_string(why.code()) + ")");
    }
}

TRIAL("Local socket: the file a program that is gone left behind is taken away") {
    namespace netting = tal::net::netting;

    tal::Loop loop;
    const Path where = socketed("stale.sock");
    const Address here = Address::local(where);
    const netting::Handle stale = netting::opened(Family::local, true);

    OUGHT(netting::listening(stale, here, 1));
    netting::ending(stale);
    OUGHT(standing(where));

    const Listener listener = tal::wait(loop, Listener::opened(here));
    auto [near, far] = tal::wait(loop, tal::all(tal::net::connect(here), listener.accepted()));

    tal::wait(loop, far.stream.write(tal::bytes("fresh")));
    SAME(tal::text::of(tal::wait(loop, gathered(near.stream, 5))), std::string{"fresh"});
}

TRIAL("Connect by name: the next address after one that refused, and the last trouble of all") {
    tal::Loop loop;
    Address gone;

    {
        Listener listener = listening(loop);

        gone = listener.mine();
    }

    const Listener listener = listening(loop);
    Connection one = tal::wait(loop, tal::net::netting::tried({gone, listener.mine()}));

    SAME(one.peer, listener.mine());

    const Connection far = tal::wait(loop, listener.accepted());

    (void)far;

    const Listener local = tal::wait(loop, Listener::opened(Address::local(socketed("order.sock"))));
    const Address nowhere = Address::local(socketed("none.sock"));
    const Connection first = tal::wait(loop, tal::net::netting::tried({nowhere, local.mine(), listener.mine()}));

    SAME(first.peer, listener.mine());

    OUGHT(refused(loop, tal::net::netting::tried({gone, gone})) == tal::Kind::refused);
    OUGHT(refused(loop, tal::net::netting::tried({})) == tal::Kind::missing);

    auto [named, accepted] = tal::wait(loop, tal::all(tal::net::connect("127.0.0.1", listener.mine().port()), listener.accepted()));

    SAME(named.peer, listener.mine());
    (void)accepted;
}

TRIAL("Reactor: a thousand connections at once, and no thread more for them") {
    tal::Loop loop;
    const Listener listener = tal::wait(loop, Listener::opened(Address::loopback(Family::four, 0), 1024));

    (void)paired(loop, listener);

    const std::size_t before = threads();
    constexpr int many = 1000;
    std::vector<Connection> near;
    std::vector<Connection> far;
    std::atomic<int> answered{0};

    {
        tal::Scope scope{loop};

        scope.run([](Listener from, std::vector<Connection>& into) -> tal::Task<void> {
            for (int at = 0; at < many; ++at) into.push_back(co_await from.accepted());
        }(listener, far));

        for (int at = 0; at < many; ++at)
            scope.run([](Address where, std::vector<Connection>& into) -> tal::Task<void> { into.push_back(co_await tal::net::connect(where)); }(
                listener.mine(), near));

        UNTIL(loop, near.size() == static_cast<std::size_t>(many) && far.size() == static_cast<std::size_t>(many));
    }

    {
        tal::Scope scope{loop};

        for (Connection& one : far)
            scope.run([](tal::Stream from) -> tal::Task<void> {
                const tal::Bytes got = co_await from.read(16);

                co_await from.write(got);
            }(one.stream));

        for (std::size_t at = 0; at < near.size(); ++at)
            scope.run([](tal::Stream into, std::size_t number, std::atomic<int>& count) -> tal::Task<void> {
                const std::string said = std::to_string(number);

                co_await into.write(tal::bytes(said));

                if (tal::text::of(co_await gathered(into, said.size())) == said) ++count;
            }(near[at].stream, at, answered));

        UNTIL(loop, answered.load() == many);
    }

    SAME(threads(), before);
}

TRIAL("Reactor: a socket nobody reads does not wake it over and over") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);
    const std::size_t start = tal::net::netting::turns();

    tal::wait(loop, near.stream.write(tal::bytes("abcdef")));
    SAME(tal::text::of(tal::wait(loop, far.stream.read(1))), std::string{"a"});

    const std::size_t before = tal::net::netting::turns();

    OUGHT(before > start);

    std::this_thread::sleep_for(100ms);

    OUGHT(tal::net::netting::turns() - before < 5);
}

TRIAL("Reactor: a gone awaiter's request and one on a closed socket are refused; a withdrawal is its own") {
    namespace netting = tal::net::netting;

    const auto raw = datagrams();
    const std::size_t before = netting::watching();
    const auto gone = asked(raw, netting::Step::fetching);

    OUGHT(!gone->left(false));
    netting::submit(gone);
    SAME(netting::watching(), before);
    SAME(gone->code, netting::aborted());

    const auto waiting = asked(raw, netting::Step::fetching);

    netting::submit(waiting);
    SAME(netting::watching(), before + 1);
    OUGHT(!netting::withdraw(asked(raw, netting::Step::fetching)));
    SAME(netting::watching(), before + 1);

    (void)netting::withdraw(waiting);
    SOON(netting::watching() == before);

    const auto late = asked(raw, netting::Step::fetching);

    raw->close();
    netting::submit(late);
    SAME(late->code, netting::aborted());
    SAME(netting::watching(), before);
}

#ifndef _WIN32
// A direction given up while the other waits stays armed with nobody there; once the other is
// done too, the socket leaves the reactor whole, and a request of that direction later is armed
// anew instead of meeting the old arming.
TRIAL("Reactor: a direction given up while the other waits can wait again later") {
    tal::Loop loop;
    const Listener listener = listening(loop);
    auto [near, far] = paired(loop, listener);

    OUGHT(refused(loop, tal::within(50ms, far.stream.write(tal::Bytes(64 * 1024 * 1024)))) == tal::Kind::overdue);

    auto [got, sent] = tal::wait(loop, tal::all(far.stream.read(1), written(near.stream, tal::bytes("x"))));

    SAME(tal::text::of(got), std::string{"x"});
    SAME(sent, std::size_t{1});

    bool wrote = false;
    tal::Scope scope{loop};

    scope.run([](tal::Stream into, bool& done) -> tal::Task<void> {
        co_await into.write(tal::bytes("y"));

        done = true;
    }(far.stream, wrote));

    QUIETED(loop);

    while (!wrote) (void)tal::wait(loop, near.stream.read(1024 * 1024));

    bool sending = false;

    scope.run([](tal::Stream into, bool& done) -> tal::Task<void> {
        co_await into.write(tal::Bytes(16 * 1024 * 1024));

        done = true;
    }(far.stream, sending));

    QUIETED(loop);

    OUGHT(refused(loop, tal::within(30ms, far.stream.read(10))) == tal::Kind::overdue);

    while (!sending) (void)tal::wait(loop, near.stream.read(1024 * 1024));

    std::string last;

    scope.run([](tal::Stream from, std::string& into) -> tal::Task<void> { into = tal::text::of(co_await from.read(10)); }(far.stream, last));

    QUIETED(loop);

    tal::wait(loop, near.stream.write(tal::bytes("z")));

    UNTIL(loop, last == "z");
}

TRIAL("Reactor: a socket the system says is over, whose step cannot go on, fails instead of waiting") {
    namespace netting = tal::net::netting;

    tal::Loop loop;
    const auto raw = datagrams();
    int code = -1;
    tal::Scope scope{loop};

    scope.run([](std::shared_ptr<netting::Doing> one, int& into) -> tal::Task<void> {
        co_await netting::Awaited{one};

        into = one->code;
    }(asked(raw, netting::Step::fetching), code));

    QUIETED(loop);

    netting::shutting(raw->now());

    UNTIL(loop, code != -1);
    SAME(code, EPIPE);
}
#endif

TRIAL("Reactor: a program starts while the reactor waits for a socket, which libkqueue's fork handler once held up") {
    tal::Loop loop;
    const Listener listener = tal::wait(loop, Listener::opened(Address::loopback(Family::four, 0)));
    tal::Command what;

    what.program = tal::testing::mine();
    what.arguments = {"--pretending", "code", "7"};

    const auto ran = [](tal::Command one, Address to) -> tal::Task<int> {
        const tal::Output out = co_await tal::Process::run(std::move(one));
        const Connection near = co_await tal::net::connect(to);

        (void)near;

        co_return out.exit.code;
    };

    auto [far, code] = tal::wait(loop, tal::within(20s, tal::all(listener.accepted(), ran(std::move(what), listener.mine()))));

    (void)far;

    SAME(code, 7);
}

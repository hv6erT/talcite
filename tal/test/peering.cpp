#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/net.hpp"
#include "tal/peer.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "routing.hpp"
#include "stunning.hpp"

// What connects users, against servers the trials write on the loopback: STUN servers, beacons,
// and routers that speak PCP or NAT-PMP alone. Nothing goes beyond this computer.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::net::Address;
using tal::net::Family;
using tal::net::Udp;

// A UDP server on the loopback that answers each datagram with what its script gives, and keeps
// what it heard.
class Fake {
public:
    using Script = std::function<std::vector<Bytes>(const Bytes& heard, const Address& from)>;

    // Listens on the loopback of `loop` and answers by `script`; what `forged` gives goes first,
    // from another socket of its own.
    Fake(tal::Loop& loop, Script script, Script forged = {})
        : script_(std::move(script)), forged_(std::move(forged)), post_(tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)))),
          aside_(tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)))), scope_(loop) {
        scope_.run(answering());
    }

    // Stops answering.
    ~Fake() {
        post_.close();
        aside_.close();
    }

    Fake(const Fake&) = delete;
    Fake& operator=(const Fake&) = delete;

    // Its address.
    [[nodiscard]] Address mine() const { return post_.mine(); }

    std::vector<Bytes> heard;

private:
    // Answers datagram after datagram.
    tal::Task<void> answering() {
        try {
            for (;;) {
                const tal::net::Datagram got = co_await post_.received();

                heard.push_back(got.bytes);

                if (forged_)
                    for (Bytes& one : forged_(got.bytes, got.from)) co_await aside_.send(got.from, std::move(one));

                for (Bytes& one : script_(got.bytes, got.from)) co_await post_.send(got.from, std::move(one));
            }
        } catch (const tal::Trouble&) {
        }
    }

    Script script_;
    Script forged_;
    Udp post_;
    Udp aside_;
    tal::Scope scope_;
};

// The transaction number of the STUN request `heard`.
tal::net::stunning::Number number_of(const Bytes& heard) {
    tal::net::stunning::Number made{};

    std::copy(heard.begin() + 8, heard.begin() + 20, made.begin());

    return made;
}

// A STUN server that tells each asker where it stands.
Fake::Script reflecting() {
    return [](const Bytes& heard, const Address& from) { return std::vector<Bytes>{tal::net::stunning::told(number_of(heard), from)}; };
}

// The kind of trouble `work` fails with, as spoken, or "none".
template <typename T>
std::string kind_of(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())};
    }

    return "none";
}

// The kind `kind` as spoken.
std::string named(tal::Kind kind) { return std::string{tal::spoken(kind)}; }

// The big-endian 16-bit number at `at` of `from`.
unsigned got16(const Bytes& from, std::size_t at) { return (std::to_integer<unsigned>(from[at]) << 8) | std::to_integer<unsigned>(from[at + 1]); }

// The big-endian 32-bit number at `at` of `from`.
unsigned long got32(const Bytes& from, std::size_t at) { return (static_cast<unsigned long>(got16(from, at)) << 16) | got16(from, at + 2); }

// Appends `value` to `into` as 16 bits, most significant first.
void put16(Bytes& into, unsigned value) {
    into.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    into.push_back(static_cast<std::byte>(value & 0xff));
}

// Appends `value` to `into` as 32 bits, most significant first.
void put32(Bytes& into, unsigned long value) {
    put16(into, static_cast<unsigned>(value >> 16));
    put16(into, static_cast<unsigned>(value & 0xffff));
}

// The PCP answer to the MAP request `heard` with the result `code`, the outside port `port` of
// 203.0.113.5, and the lifetime asked for.
Bytes pcp_answer(const Bytes& heard, unsigned code, unsigned port) {
    Bytes made{std::byte{2}, std::byte{0x81}, std::byte{0}, static_cast<std::byte>(code)};

    put32(made, got32(heard, 4));
    put32(made, 77);
    made.insert(made.end(), 12, std::byte{0});
    made.insert(made.end(), heard.begin() + 24, heard.begin() + 36);
    made.push_back(heard[36]);
    made.insert(made.end(), 3, std::byte{0});
    put16(made, got16(heard, 40));
    put16(made, port);
    made.insert(made.end(), 10, std::byte{0});
    made.push_back(std::byte{0xff});
    made.push_back(std::byte{0xff});

    for (const int one : {203, 0, 113, 5}) made.push_back(static_cast<std::byte>(one));

    return made;
}

// A router of NAT-PMP alone: PCP gets "unsupported version" in NAT-PMP's form, a mapping the
// outside port `port`, and the question for the outside address 198.51.100.7.
Fake::Script old_router(unsigned port) {
    return [port](const Bytes& heard, const Address&) {
        Bytes made{std::byte{0}};

        if (heard[0] == std::byte{2}) {
            made.push_back(static_cast<std::byte>(128 + std::to_integer<int>(heard[1])));
            put16(made, 1);
            put32(made, 77);

            return std::vector<Bytes>{made};
        }

        made.push_back(static_cast<std::byte>(128 + std::to_integer<int>(heard[1])));
        put16(made, 0);
        put32(made, 77);

        if (heard[1] == std::byte{0}) {
            for (const int one : {198, 51, 100, 7}) made.push_back(static_cast<std::byte>(one));
        } else {
            put16(made, got16(heard, 4));
            put16(made, got32(heard, 8) == 0 ? 0 : port);
            put32(made, got32(heard, 8));
        }

        return std::vector<Bytes>{made};
    };
}

// The routing of the router `fake`, as seen from the loopback, with `turn` for each way.
tal::net::Routing routing_of(const Fake& fake, std::chrono::milliseconds turn = 1000ms) {
    return tal::net::Routing{fake.mine(), Address::loopback(Family::four, 0), turn};
}

}

TRIAL("STUN: a Binding request and its answers, XOR-ed, plain and refused, are read back") {
    namespace stunning = tal::net::stunning;

    const stunning::Number number = stunning::numbered();
    const Bytes asked = stunning::asked(number);

    SAME(asked.size(), std::size_t{20});
    SAME(got16(asked, 0), 1U);
    SAME(got32(asked, 4), 0x2112A442UL);

    const Address four{"192.0.2.7", 3478};
    const Address six{"2001:db8::9", 61000};

    for (const Address& where : {four, six}) {
        for (const bool xored : {true, false}) {
            const std::optional<stunning::Heard> one = stunning::heard(stunning::told(number, where, xored));

            OUGHT(one.has_value() && one->answer && !one->error);
            OUGHT(one->number == number);
            SAME(one->seen.spelled(), where.spelled());
        }
    }

    const std::optional<stunning::Heard> no = stunning::heard(stunning::refused(number, 438, "Stale\x1b[2J"));

    OUGHT(no.has_value() && no->error);
    SAME(no->code, 438);
    SAME(no->said, std::string{"Stale?[2J"});

    Bytes broken = stunning::told(number, four);

    broken[3] = static_cast<std::byte>(std::to_integer<int>(broken[3]) + 4);

    OUGHT(!stunning::heard(broken).has_value());

    Bytes request = stunning::told(number, four);

    request[0] = std::byte{0};
    request[1] = std::byte{1};

    OUGHT(!stunning::heard(request).has_value());
    OUGHT(!stunning::heard(Bytes(19)).has_value());

    Bytes long_one = stunning::told(number, four);

    long_one[23] = static_cast<std::byte>(40);

    OUGHT(!stunning::heard(long_one)->seen.held());

    Bytes stranger = stunning::told(number, four);

    stranger[7] ^= std::byte{1};

    OUGHT(!stunning::heard(stranger).has_value());

    Bytes both = stunning::told(number, four);
    const Bytes plain = stunning::told(number, Address{"198.51.100.1", 1}, false);

    both.insert(both.end(), plain.begin() + 20, plain.end());
    both[3] = static_cast<std::byte>(both.size() - 20);

    SAME(stunning::heard(both)->seen.spelled(), four.spelled());

    const Bytes vector = tal::bytes(std::string{"\x01\x01\x00\x18\x21\x12\xa4\x42\xb7\xe7\xa7\x01\xbc\x34\xd6\x86\xfa\x87\xdf\xae"
                                                "\x00\x20\x00\x14\x00\x02\xa1\x47\x01\x13\xa9\xfa\xa5\xd3\xf1\x79\xbc\x25\xf4\xb5\xbe\xd2\xb9\xd9",
                                                44});

    SAME(stunning::heard(vector)->seen.spelled(), (Address{"2001:db8:1234:5678:11:2233:4455:6677", 32853}.spelled()));
}

TRIAL("STUN: the address a server sees; strangers' answers pass by; a refusal is refused, silence asks the next") {
    tal::Loop loop;
    Fake noisy{loop, [](const Bytes& heard, const Address& from) {
                   tal::net::stunning::Number other = number_of(heard);

                   other[0] ^= std::byte{1};

                   return std::vector<Bytes>{tal::bytes("garbage"), tal::net::stunning::told(other, Address{"1.2.3.4", 5}),
                                             tal::net::stunning::told(number_of(heard), from)};
               }};
    Fake refusing{loop, [](const Bytes& heard, const Address&) { return std::vector<Bytes>{tal::net::stunning::refused(number_of(heard), 403, "Forbidden")}; }};
    Fake silent{loop, [](const Bytes&, const Address&) { return std::vector<Bytes>{}; }};
    Fake good{loop, reflecting()};

    SAME(tal::wait(loop, tal::net::Stun{{noisy.mine()}, 2s}.seen()).text(), std::string{"127.0.0.1"});
    SAME(tal::wait(loop, tal::net::Stun({silent.mine(), good.mine()}, 300ms).seen()).text(), std::string{"127.0.0.1"});
    SAME(kind_of(loop, tal::net::Stun({refusing.mine(), silent.mine()}, 300ms).seen()), named(tal::Kind::refused));
    SAME(kind_of(loop, tal::net::Stun({silent.mine()}, 300ms).seen()), named(tal::Kind::overdue));
    SAME(kind_of(loop, tal::net::Stun({}).seen()), named(tal::Kind::other));

    const std::size_t before = silent.heard.size();

    SAME(kind_of(loop, tal::net::Stun({silent.mine()}, 1900ms).seen()), named(tal::Kind::overdue));
    OUGHT(silent.heard.size() - before >= 2 && silent.heard.size() - before <= 3);
}

TRIAL("STUN: alike when two servers see one socket at one place, symmetric when not, unknown otherwise") {
    tal::Loop loop;
    Fake one{loop, reflecting()};
    Fake two{loop, reflecting()};
    Fake moved{loop, [](const Bytes& heard, const Address& from) {
                   Address shifted = from;

                   shifted.port(static_cast<std::uint16_t>(from.port() + 1));

                   return std::vector<Bytes>{tal::net::stunning::told(number_of(heard), shifted)};
               }};
    Fake silent{loop, [](const Bytes&, const Address&) { return std::vector<Bytes>{}; }};

    OUGHT(tal::wait(loop, tal::net::Stun({one.mine(), two.mine()}, 2s).mapping()) == tal::net::Mapping::alike);
    OUGHT(tal::wait(loop, tal::net::Stun({one.mine(), moved.mine()}, 2s).mapping()) == tal::net::Mapping::symmetric);
    OUGHT(tal::wait(loop, tal::net::Stun({one.mine(), one.mine()}, 2s).mapping()) == tal::net::Mapping::unknown);
    OUGHT(tal::wait(loop, tal::net::Stun({one.mine(), silent.mine()}, 300ms).mapping()) == tal::net::Mapping::unknown);
    OUGHT(tal::wait(loop, tal::net::Stun({}).mapping()) == tal::net::Mapping::unknown);
}

TRIAL("Beacon: a question about an offered tag, broadcast or sent, is answered once per sender; one withdrawn or unknown is not") {
    tal::Loop loop;
    tal::net::Beacon beacon = tal::wait(loop, tal::net::Beacon::opened(0));
    const std::uint16_t port = beacon.mine().port();
    const std::vector<Address> here{Address::loopback(Family::four, port)};

    beacon.offer(tal::bytes("films"), tal::bytes("port 9000"));
    beacon.offer(tal::bytes("books"), {});

    const std::vector<tal::net::Sighting> found = tal::wait(loop, tal::net::Beacon::sought(port, tal::bytes("films"), 300ms, here));

    SAME(found.size(), std::size_t{1});
    SAME(tal::text::of(found.at(0).payload), std::string{"port 9000"});
    SAME(found.at(0).from.port(), port);
    SAME(tal::wait(loop, tal::net::Beacon::sought(port, tal::bytes("books"), 300ms, here)).size(), std::size_t{1});
    OUGHT(tal::wait(loop, tal::net::Beacon::sought(port, tal::bytes("music"), 300ms, here)).empty());

    const std::vector<tal::net::Sighting> broadcast = tal::wait(loop, tal::net::Beacon::sought(port, tal::bytes("films"), 300ms));

    SAME(broadcast.size(), std::size_t{1});
    SAME(tal::text::of(broadcast.at(0).payload), std::string{"port 9000"});

    beacon.withdraw(tal::bytes("films"));

    OUGHT(tal::wait(loop, tal::net::Beacon::sought(port, tal::bytes("films"), 300ms, here)).empty());

    beacon.close();

    OUGHT(!beacon.held());
    SAME(kind_of(loop, [](tal::net::Beacon one) -> tal::Task<void> {
             one.offer(tal::bytes("x"), {});

             co_return;
         }(beacon)),
         named(tal::Kind::other));
}

TRIAL("Beacon: what has not its shape passes by; tags and answers have their limits; a flood gets 32 answers a second") {
    tal::Loop loop;
    tal::net::Beacon beacon = tal::wait(loop, tal::net::Beacon::opened(0));
    const Address there = Address::loopback(Family::four, beacon.mine().port());
    Udp asker = tal::wait(loop, Udp::bound(Address::loopback(Family::four, 0)));

    beacon.offer(tal::bytes("t"), tal::bytes("p"));

    Bytes question = tal::bytes("TLCB");

    question.push_back(std::byte{1});
    question.push_back(std::byte{1});
    question.insert(question.end(), 8, std::byte{7});
    question.push_back(std::byte{1});
    question.push_back(std::byte{'t'});

    Bytes longer = question;

    longer.push_back(std::byte{0});

    Bytes other = question;

    other[4] = std::byte{2};

    Bytes answer = question;

    answer[5] = std::byte{2};
    answer.push_back(std::byte{0});
    answer.push_back(std::byte{0});

    for (const Bytes& wrong : {tal::bytes("TLCB"), longer, other, answer, Bytes(40)}) tal::wait(loop, asker.send(there, wrong));

    SAME(kind_of(loop, tal::within(300ms, asker.received())), named(tal::Kind::overdue));

    for (int one = 0; one < 40; ++one) tal::wait(loop, asker.send(there, question));

    int answers = 0;

    for (;;) {
        try {
            const tal::net::Datagram got = tal::wait(loop, tal::within(300ms, asker.received()));

            OUGHT(got.bytes.size() == question.size() + 3);
            ++answers;
        } catch (const tal::Overdue&) {
            break;
        }
    }

    SAME(answers, 32);

    SAME(kind_of(loop, [](tal::net::Beacon one) -> tal::Task<void> {
             one.offer(Bytes(65), {});

             co_return;
         }(beacon)),
         named(tal::Kind::other));
    SAME(kind_of(loop, [](tal::net::Beacon one) -> tal::Task<void> {
             one.offer(tal::bytes("t"), Bytes(1025));

             co_return;
         }(beacon)),
         named(tal::Kind::other));
    SAME(kind_of(loop, tal::net::Beacon::sought(1, {}, 10ms)), named(tal::Kind::other));

    Fake liar{loop, [](const Bytes& heard, const Address&) {
                  Bytes made = heard;

                  made[5] = std::byte{2};
                  made[6] ^= std::byte{1};
                  made.push_back(std::byte{0});
                  made.push_back(std::byte{0});

                  return std::vector<Bytes>{made};
              }};

    OUGHT(tal::wait(loop, tal::net::Beacon::sought(1, tal::bytes("t"), 300ms, {liar.mine()})).empty());
    OUGHT(!liar.heard.empty());
}

TRIAL("Router: PCP opens, renews with the same nonce and closes; a mapping it did not open is missing") {
    tal::Loop loop;
    Fake router{loop, [](const Bytes& heard, const Address&) { return std::vector<Bytes>{pcp_answer(heard, 0, got32(heard, 4) == 0 ? 0 : 40000)}; }};
    const tal::net::Router routed{routing_of(router)};

    const tal::net::Opening one = tal::wait(loop, routed.open(tal::net::Carrier::tcp, 8080, 3600s));

    OUGHT(one.by == tal::net::Opener::pcp);
    SAME(one.outside.spelled(), (Address{"203.0.113.5", 40000}.spelled()));
    SAME(one.lasting.count(), 3600LL);
    SAME(one.inside, std::uint16_t{8080});
    SAME(router.heard.at(0)[36], std::byte{6});
    SAME(router.heard.at(0)[18], std::byte{0xff});
    SAME(router.heard.at(0)[19], std::byte{0xff});
    SAME(router.heard.at(0)[23], std::byte{1});

    (void)tal::wait(loop, routed.open(tal::net::Carrier::tcp, 8080, 3600s));

    OUGHT(Bytes(router.heard.at(0).begin() + 24, router.heard.at(0).begin() + 36) == Bytes(router.heard.at(1).begin() + 24, router.heard.at(1).begin() + 36));

    (void)tal::wait(loop, routed.open(tal::net::Carrier::udp, 8080, 60s));

    SAME(router.heard.at(2)[36], std::byte{17});
    OUGHT(Bytes(router.heard.at(0).begin() + 24, router.heard.at(0).begin() + 36) != Bytes(router.heard.at(2).begin() + 24, router.heard.at(2).begin() + 36));

    tal::wait(loop, routed.close(one));

    SAME(got32(router.heard.back(), 4), 0UL);
    SAME(got16(router.heard.back(), 42), 0U);
    OUGHT(Bytes(router.heard.at(0).begin() + 24, router.heard.at(0).begin() + 36) == Bytes(router.heard.back().begin() + 24, router.heard.back().begin() + 36));
    SAME(kind_of(loop, routed.close(one)), named(tal::Kind::missing));

    Fake tricked{loop,
                 [](const Bytes& heard, const Address&) {
                     Bytes wrong = pcp_answer(heard, 0, 1111);

                     wrong[41] ^= std::byte{1};

                     return std::vector<Bytes>{wrong, pcp_answer(heard, 0, 40000)};
                 },
                 [](const Bytes& heard, const Address&) { return std::vector<Bytes>{pcp_answer(heard, 0, 2222)}; }};

    SAME(tal::wait(loop, tal::net::Router{routing_of(tricked)}.open(tal::net::Carrier::tcp, 8080, 60s)).outside.port(), std::uint16_t{40000});
}

TRIAL("Router: one of NAT-PMP alone is spoken to so, its outside address asked; the next time straight") {
    tal::Loop loop;
    Fake router{loop, old_router(41000)};
    const tal::net::Router routed{routing_of(router)};

    const tal::net::Opening one = tal::wait(loop, routed.open(tal::net::Carrier::udp, 9000, 600s));

    OUGHT(one.by == tal::net::Opener::pmp);
    SAME(one.outside.spelled(), (Address{"198.51.100.7", 41000}.spelled()));
    SAME(one.lasting.count(), 600LL);
    SAME(router.heard.size(), std::size_t{3});
    SAME(router.heard.at(1)[1], std::byte{1});

    (void)tal::wait(loop, routed.open(tal::net::Carrier::tcp, 9000, 600s));

    SAME(router.heard.size(), std::size_t{5});
    SAME(router.heard.at(3)[1], std::byte{2});

    tal::wait(loop, routed.close(one));

    SAME(got32(router.heard.back(), 8), 0UL);
    SAME(router.heard.size(), std::size_t{6});
}

TRIAL("Router: a refusal says its kind; silence is unsupported; port zero and no gateway are the caller's") {
    tal::Loop loop;
    Fake forbidding{loop, [](const Bytes& heard, const Address&) { return std::vector<Bytes>{pcp_answer(heard, 2, 0)}; }};
    Fake full{loop, [](const Bytes& heard, const Address&) { return std::vector<Bytes>{pcp_answer(heard, 8, 0)}; }};
    Fake silent{loop, [](const Bytes&, const Address&) { return std::vector<Bytes>{}; }};

    SAME(kind_of(loop, tal::net::Router{routing_of(forbidding)}.open(tal::net::Carrier::tcp, 1, 1s)), named(tal::Kind::denied));
    SAME(kind_of(loop, tal::net::Router{routing_of(full)}.open(tal::net::Carrier::tcp, 1, 1s)), named(tal::Kind::refused));
    SAME(kind_of(loop, tal::net::Router{routing_of(silent, 1900ms)}.open(tal::net::Carrier::tcp, 1, 1s)), named(tal::Kind::unsupported));
    OUGHT(silent.heard.size() >= 3 && silent.heard.size() <= 4);

    for (const Bytes& one : silent.heard) SAME(one[0], std::byte{2});

    Fake strange{loop, [](const Bytes& heard, const Address&) {
                     if (heard[0] == std::byte{2}) return std::vector<Bytes>{Bytes{std::byte{0}, std::byte{0x80}, std::byte{0}, std::byte{3}}};

                     return old_router(5)(heard, Address{});
                 }};

    SAME(kind_of(loop, tal::net::Router{routing_of(strange, 300ms)}.open(tal::net::Carrier::tcp, 1, 1s)), named(tal::Kind::unsupported));
    THREW((void)tal::net::Router{routing_of(silent)}.open(tal::net::Carrier::tcp, 0, 1s));
    THREW((void)tal::net::Router{tal::net::Routing{}}.open(tal::net::Carrier::tcp, 1, 1s));
}

TRIAL("Router: the default route of Linux's table, the one of the lowest metric") {
    const std::string head = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";

    OUGHT(!tal::net::routing::routed(head).has_value());
    OUGHT(!tal::net::routing::routed(head + "eth0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n").has_value());
    OUGHT(!tal::net::routing::routed(head + "eth0\t00000000\t00000000\t0003\t0\t0\t0\t00000000\t0\t0\t0\n").has_value());
    OUGHT(!tal::net::routing::routed(head + "eth0\t00000000\tXYZ\t0003\t0\t0\t0\t00000000\t0\t0\t0\n").has_value());

    OUGHT(!tal::net::routing::routed(head + "eth0\t0000A8C0\t0101A8C0\t0003\t0\t0\t0\t00FFFFFF\t0\t0\t0\n").has_value());

    const std::string lower = head + "eth0\t00000000\t01000A0A\t0003\t0\t0\t100\t00000000\t0\t0\t0\n" + "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n";

    SAME(tal::net::routing::routed(lower)->text(), std::string{"10.10.0.1"});

    const std::string two = head + "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n" + "eth0\t00000000\t01000A0A\t0003\t0\t0\t100\t00000000\t0\t0\t0\n";

    SAME(tal::net::routing::routed(two)->text(), std::string{"10.10.0.1"});
}

TRIAL("Router: the way toward an address and the default gateway, as the system tells them") {
    tal::Loop loop;
    const Address way = tal::wait(loop, tal::net::leaving(Address{"127.0.0.1", 0}));

    SAME(way.text(), std::string{"127.0.0.1"});
    SAME(way.port(), std::uint16_t{0});
    SAME(kind_of(loop, tal::net::leaving(Address{})), named(tal::Kind::other));

    try {
        const Address router = tal::wait(loop, tal::net::gateway());

        OUGHT(router.family() == Family::four);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::missing || why.kind() == tal::Kind::unsupported);
    }
}

#include "tal/testing.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/beacon.hpp"
#include "talcite/bytes.hpp"
#include "talcite/promise.hpp"
#include "talcite/time.hpp"
#include "talcite/udp.hpp"

#include "exchange.hpp"

using namespace talcite;

// A beacon in the local network: the sharing side answers a question about a tag it knows, the
// asking side collects the answers. The trials ask the loopback directly; broadcast depends on the
// network the machine stands in, so it has a separate trial that can skip itself.
namespace {

const net::Patience briefly{400};

// The tag with the given text.
Bytes tagged(const std::string& said) { return bytes(said); }

// The loopback address of the beacon's port, to ask it directly instead of by broadcast.
std::vector<net::Address> here(const net::Beacon& one) {
    return {net::Address::loopback(net::Family::four, one.mine().port())};
}

}

TRIAL("Beacon: answers a tag it knows with the payload it was given") {
    const net::Beacon one{0};

    OUGHT(one.held());
    OUGHT(one.mine().port() != 0);

    one.offer(tagged("etykieta"), bytes("port 4123"));

    const std::vector<net::Sighting> found =
        net::Beacon::sought(one.mine().port(), tagged("etykieta"), briefly, here(one)).get();

    SAME(found.size(), std::size_t{1});

    if (found.empty()) return;

    SAME(text(found.front().payload), std::string{"port 4123"});
    SAME(found.front().from.port(), one.mine().port());

    // The same beacon asked twice (two addresses lead to it) counts once.
    const std::vector<net::Sighting> twice = net::Beacon::sought(one.mine().port(), tagged("etykieta"), briefly,
                                                                 {here(one).front(), here(one).front()})
                                                 .get();

    SAME(twice.size(), std::size_t{1});
}

// The question goes out again while the asking side waits: a beacon that begins to offer the tag
// after the first question came still answers within the patience, once.
TRIAL("Beacon: a tag offered a moment after the question is still found") {
    const net::Beacon one{0};

    Promise<std::vector<net::Sighting>> seeking = net::Beacon::sought(one.mine().port(), tagged("później"), briefly, here(one));

    std::this_thread::sleep_for(std::chrono::milliseconds{briefly.count() / 3 / 2});

    one.offer(tagged("później"), bytes("jest"));

    const std::vector<net::Sighting> found = std::move(seeking).get();

    SAME(found.size(), std::size_t{1});
    OUGHT(!found.empty() && text(found.front().payload) == "jest");
}

// An unknown tag and a withdrawn tag give an empty list. "Nobody here" is a result, not a refusal.
TRIAL("Beacon: an unknown or withdrawn tag gets no answer") {
    const net::Beacon one{0};

    one.offer(tagged("jest"), bytes("x"));

    OUGHT(net::Beacon::sought(one.mine().port(), tagged("nie ma"), briefly, here(one)).get().empty());

    one.withdraw(tagged("jest"));

    OUGHT(net::Beacon::sought(one.mine().port(), tagged("jest"), briefly, here(one)).get().empty());
}

// A foreign datagram on the beacon's port (garbage, a truncated message, another version, an
// answer instead of a question) changes nothing: the next real question gets an answer.
TRIAL("Beacon: foreign datagrams break nothing") {
    const net::Beacon one{0};

    one.offer(tagged("etykieta"), bytes("ok"));

    const net::Udp stranger{net::Address::loopback(net::Family::four, 0)};
    const net::Address target = here(one).front();

    const auto from_text = [](const std::string& said) { return bytes(said); };

    for (const Bytes& junk : {from_text("śmieci"), from_text("TLCB"), from_text(std::string{"TLCB\x02\x01"} + std::string(8, 'n') + "\x08" "etykieta"),
                              from_text(std::string{"TLCB\x01\x02"} + std::string(8, 'n') + "\x08" + std::string{"etykieta\x00\x02ok", 12}),
                              from_text(std::string{"TLCB\x01\x01"} + std::string(8, 'n') + "\x08" "etykietaXX"),
                              from_text(std::string{"TLCB\x01\x01"} + std::string(8, 'n') + "\x50" "etykieta")})
        stranger.send(target, junk, briefly).get();

    const std::vector<net::Sighting> found = net::Beacon::sought(one.mine().port(), tagged("etykieta"), briefly, here(one)).get();

    SAME(found.size(), std::size_t{1});

    // A question right in everything but one thing (the magic, the version, the kind, a surplus
    // byte or the tag length) gets no answer. The same question done right gets one.
    const std::string luck(8, 'n');
    const std::string asked = std::string{"TLCB\x01\x01"} + luck + "\x08" "etykieta";

    for (const std::string& wrong : {std::string{"XLCB\x01\x01"} + luck + "\x08" "etykieta",
                                     std::string{"TLCB\x02\x01"} + luck + "\x08" "etykieta",
                                     std::string{"TLCB\x01\x03"} + luck + "\x08" "etykieta", asked + "X",
                                     std::string{"TLCB\x01\x01"} + luck + "\x41" "etykieta" + std::string(57, 'x'),
                                     std::string{"TLCB\x01\x02"} + luck + "\x08" "etykieta" + std::string{"\x00\x02ok", 4}}) {
        stranger.send(target, from_text(wrong), briefly).get();

        THREW(stranger.receive(256, net::Patience{250}).get());
    }

    stranger.send(target, from_text(asked), briefly).get();

    const net::Datagram answer = stranger.receive(256, briefly).get();

    OUGHT(answer.bytes.size() > 5 && answer.bytes[5] == std::byte{2});
}

// An answer to someone else's question (other random bytes) or with another tag is not an answer
// for us. A fake beacon answers everything its own way; only the real one counts.
TRIAL("Beacon: an answer with someone else's nonce or tag does not count") {
    const net::Beacon real{0};

    real.offer(tagged("etykieta"), bytes("prawdziwy"));

    const net::Udp fake{net::Address::loopback(net::Family::four, 0)};

    // The fake one listens for one question and answers twice: with another nonce and with
    // another tag.
    auto heard = fake.receive(256, net::Patience{2000});

    auto seeking = net::Beacon::sought(real.mine().port(), tagged("etykieta"), net::Patience{600},
                                       {here(real).front(), fake.mine()});

    // A chained promise starts only on request; here, before waiting for the question at the
    // fake one.

    const net::Datagram asked = std::move(heard).get();

    OUGHT(asked.bytes.size() > 14);

    if (asked.bytes.size() > 14) {
        Bytes other_luck = asked.bytes;

        other_luck[5] = std::byte{2};
        other_luck[6] = static_cast<std::byte>(static_cast<unsigned char>(other_luck[6]) ^ 0xff);

        for (const char each : std::string{"\x00\x05obcy!", 7}) other_luck.push_back(static_cast<std::byte>(each));

        fake.send(asked.from, other_luck, briefly).get();

        Bytes other_tag = asked.bytes;

        other_tag[5] = std::byte{2};
        other_tag[other_tag.size() - 1] = std::byte{'X'};

        for (const char each : std::string{"\x00\x05obcy!", 7}) other_tag.push_back(static_cast<std::byte>(each));

        fake.send(asked.from, other_tag, briefly).get();

        // Our own question bounced back: the same nonce and tag, but it is not an answer.
        fake.send(asked.from, asked.bytes, briefly).get();

        // An answer to our question, but with a payload length that does not match the rest.
        Bytes crooked = asked.bytes;

        crooked[5] = std::byte{2};

        for (const char each : std::string{"\x00\x05obcy!!", 8}) crooked.push_back(static_cast<std::byte>(each));

        fake.send(asked.from, crooked, briefly).get();
    }

    const std::vector<net::Sighting> found = std::move(seeking).get();

    SAME(found.size(), std::size_t{1});
    OUGHT(!found.empty() && text(found.front().payload) == "prawdziwy");
}

TRIAL("Beacon: limits of the tag and the payload") {
    const net::Beacon one{0};

    THREW(one.offer(Bytes{}, bytes("x")));
    THREW(one.offer(Bytes(65, std::byte{'t'}), bytes("x")));
    THREW(one.offer(tagged("t"), Bytes(1025, std::byte{'p'})));

    one.offer(Bytes(64, std::byte{'t'}), Bytes(1024, std::byte{'p'}));

    const std::vector<net::Sighting> found =
        net::Beacon::sought(one.mine().port(), Bytes(64, std::byte{'t'}), briefly, here(one)).get();

    SAME(found.size(), std::size_t{1});
    OUGHT(!found.empty() && found.front().payload.size() == 1024);

    THREW(net::Beacon::sought(one.mine().port(), Bytes{}, briefly).get());
    THREW(net::Beacon::sought(one.mine().port(), Bytes(65, std::byte{'t'}), briefly).get());
}

// A closed beacon is silent and accepts no offers.
TRIAL("Beacon: a closed one is silent") {
    net::Beacon one{0};

    const std::uint16_t port = one.mine().port();

    one.offer(tagged("etykieta"), bytes("x"));
    one.close();

    OUGHT(!one.held());
    THREW(one.offer(tagged("etykieta"), bytes("x")));
    OUGHT(net::Beacon::sought(port, tagged("etykieta"), briefly, {net::Address::loopback(net::Family::four, port)})
              .get()
              .empty());
}

// Broadcast depends on the machine's network: without a broadcast route the question does not go
// out and nobody answers. Then the trial says it skips itself instead of posing as a failure.
TRIAL("Beacon: broadcast in the local network") {
    const net::Beacon one{0};

    one.offer(tagged("rozgloszenie"), bytes("tu"));

    const std::vector<net::Sighting> found =
        net::Beacon::sought(one.mine().port(), tagged("rozgloszenie"), net::Patience{600}).get();

    if (found.empty()) {
        std::cout << "        broadcast does not come back on this machine, trial skipped\n";

        return;
    }

    SAME(text(found.front().payload), std::string{"tu"});
}

// A beacon abandoned without `close()` leaves no receive without a deadline in the hub;
// otherwise it would wait until the end of the program, holding the socket.
TRIAL("Beacon: an abandoned one leaves no wait in the hub") {
    // First, quiet after the earlier trials: a socket that is just going away would change the
    // counter meanwhile, and the comparison would speak of it, not of the beacon.
    (void)tal::testing::came([] { return net::Exchange::running().waiting() == 0; });

    const std::size_t before = net::Exchange::running().waiting();

    {
        const net::Beacon one{0};

        one.offer(tagged("etykieta"), bytes("x"));

        OUGHT(net::Exchange::running().waiting() > before);
    }

    SOON(net::Exchange::running().waiting() <= before);
}

// A flood of questions does not make the beacon an amplifier: at most 32 answers in one second.
TRIAL("Beacon: at most 32 answers per second") {
    const net::Beacon one{0};

    one.offer(tagged("etykieta"), bytes("x"));

    const net::Udp stranger{net::Address::loopback(net::Family::four, 0)};
    const Bytes asked = bytes(std::string{"TLCB\x01\x01"} + std::string(8, 'n') + "\x08" "etykieta");

    for (int turn = 0; turn < 48; ++turn) stranger.send(here(one).front(), asked, briefly).get();

    int answers = 0;

    for (;;) {
        try {
            (void)stranger.receive(256, net::Patience{300}).get();
        } catch (const Overdue&) {
            break;
        }

        ++answers;
    }

    OUGHT(answers >= 1);
    OUGHT(answers <= 32);
}

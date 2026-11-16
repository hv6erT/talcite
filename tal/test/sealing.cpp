#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/executor.hpp"
#include "tal/net.hpp"
#include "tal/peer.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "agreeing.hpp"
#include "cpacing.hpp"
#include "quieting.hpp"
#include "toying.hpp"

// The key agreements and the channel of sealed messages on the toy cipher of `toying.cpp`, the
// pieces of CPace on the vectors of the draft, and the cipher built into the system, where there is
// one, through the trials every cipher passes.

namespace {

using tal::Bytes;
using tal::net::Side;

// The bytes of the hexadecimal digits `text`.
Bytes unhexed(std::string_view text) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < text.size(); at += 2) made.push_back(static_cast<std::byte>(std::stoi(std::string{text.substr(at, 2)}, nullptr, 16)));

    return made;
}

// `given` in hexadecimal digits, lowercase.
std::string hexed(const Bytes& given) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string made;

    for (const std::byte one : given) {
        made += digits[std::to_integer<unsigned>(one) >> 4];
        made += digits[std::to_integer<unsigned>(one) & 15];
    }

    return made;
}

// Whether this build has digests: the system's, or our own built in; without them the agreements
// cannot be tried, and those trials pass over.
bool digested() {
    try {
        (void)tal::crypto::summed(tal::crypto::Sum::sha256, {});

        return true;
    } catch (const tal::Trouble&) {
        return false;
    }
}

// The toy cipher as a port.
tal::Port<tal::crypto::Cipher> toyed() { return tal::Port<tal::crypto::Cipher>{tal::testing::toy()}; }

// The kind of trouble `work` throws, as spoken, or "none".
template <typename Work>
std::string thrown(Work work) {
    try {
        work();
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())};
    }

    return "none";
}

// The kind of trouble a task fails with, as spoken, or "none".
template <typename T>
std::string kind_of(tal::Loop& loop, tal::Task<T> work) {
    return thrown([&] { (void)tal::wait(loop, std::move(work)); });
}

// The kind `kind` as spoken.
std::string named(tal::Kind kind) { return std::string{tal::spoken(kind)}; }

// Both ends of one TCP connection on the loopback of `loop`.
std::pair<tal::Stream, tal::Stream> plain(tal::Loop& loop) {
    const tal::net::Listener listener = tal::wait(loop, tal::net::Listener::opened(tal::net::Address::loopback(tal::net::Family::four, 0)));
    auto [near, far] = tal::wait(loop, tal::all(tal::net::connect(listener.mine()), listener.accepted()));

    return {near.stream, far.stream};
}

// Both ends of a channel over the loopback of `loop`, agreed on a fresh long key.
std::pair<tal::net::Sealed, tal::net::Sealed> sealed(tal::Loop& loop) {
    const Bytes key = tal::crypto::random(32);
    auto [near, far] = plain(loop);
    auto [one, two] = tal::wait(loop, tal::all(tal::net::Sealed::opened(near, tal::net::keyed(Side::calling, key, "t", toyed()), toyed()),
                                               tal::net::Sealed::opened(far, tal::net::keyed(Side::answering, key, "t", toyed()), toyed())));

    return {one, two};
}

// A channel and, at its other side, a bare stream that went through the handshake by hand, with
// that side's keys.
struct Raw {
    tal::net::Sealed sealed;
    tal::Stream stream;
    tal::net::Keying keys;
};

// A channel over the loopback of `loop` whose other side is a bare stream, for writing and reading
// frames of its own.
Raw rawed(tal::Loop& loop) {
    const Bytes key = tal::crypto::random(32);
    auto [near, far] = plain(loop);
    auto [one, raw] = tal::wait(loop, tal::all(tal::net::Sealed::opened(near, tal::net::keyed(Side::calling, key, "t", toyed()), toyed()),
                                               [](tal::Stream stream, Bytes shared) -> tal::Task<std::pair<tal::Stream, tal::net::Keying>> {
                                                   auto how = tal::net::keyed(Side::answering, shared, "t", toyed());
                                                   Bytes kept;

                                                   while (!how->ready()) {
                                                       while (kept.size() < 4 || kept.size() < 4 + std::to_integer<std::size_t>(kept[3])) {
                                                           const Bytes came = co_await stream.read(4096);

                                                           kept.insert(kept.end(), came.begin(), came.end());
                                                       }

                                                       const std::size_t much = std::to_integer<std::size_t>(kept[3]);

                                                       how->heard(Bytes(kept.begin() + 4, kept.begin() + 4 + static_cast<std::ptrdiff_t>(much)));
                                                       kept.erase(kept.begin(), kept.begin() + 4 + static_cast<std::ptrdiff_t>(much));

                                                       while (std::optional<Bytes> said = how->said()) {
                                                           Bytes framed{std::byte{0}, std::byte{0}, std::byte{0}, static_cast<std::byte>(said->size())};

                                                           framed.insert(framed.end(), said->begin(), said->end());
                                                           co_await stream.write(std::move(framed));
                                                       }
                                                   }

                                                   co_return std::pair{stream, how->keys()};
                                               }(far, key)));

    return Raw{one, raw.first, raw.second};
}

}

TRIAL("Keyed: both sides end with crossed keys; another key or context is denied, and so is anything out of turn") {
    if (!digested()) return;

    tal::testing::agreed(tal::testing::toy());

    const Bytes key = tal::crypto::random(32);

    SAME(thrown([&] { (void)tal::net::keyed(Side::calling, Bytes(31), "t", toyed()); }), named(tal::Kind::other));

    auto calling = tal::net::keyed(Side::calling, key, "t", toyed());
    auto answering = tal::net::keyed(Side::answering, key, "t", toyed());

    SAME(thrown([&] { (void)calling->keys(); }), named(tal::Kind::other));
    OUGHT(!answering->said().has_value());
    SAME(thrown([&] { answering->heard(Bytes(31)); }), named(tal::Kind::denied));

    auto early = tal::net::keyed(Side::calling, key, "t", toyed());

    SAME(thrown([&] { early->heard(Bytes(64)); }), named(tal::Kind::denied));

    auto first = tal::net::keyed(Side::calling, key, "t", toyed());
    auto second = tal::net::keyed(Side::answering, key, "t", toyed());

    second->heard(*first->said());

    Bytes answer = *second->said();
    Bytes longer = answer;

    longer.push_back(std::byte{0});
    answer.back() ^= std::byte{1};

    SAME(thrown([&] { first->heard(answer); }), named(tal::Kind::denied));

    SAME(thrown([&] { first->heard(longer); }), named(tal::Kind::denied));

    auto sixth = tal::net::keyed(Side::answering, key, "t", toyed());

    SAME(thrown([&] { sixth->heard(Bytes(32)); }), named(tal::Kind::denied));

    auto third = tal::net::keyed(Side::calling, key, "t", toyed());
    auto fourth = tal::net::keyed(Side::answering, key, "t", toyed());

    fourth->heard(*third->said());
    third->heard(*fourth->said());

    Bytes confirmation = *third->said();

    confirmation.front() ^= std::byte{1};

    SAME(thrown([&] { fourth->heard(confirmation); }), named(tal::Kind::denied));
    OUGHT(!fourth->ready());
    OUGHT(third->ready());
}

#ifdef TAL_OWN_CURVE

TRIAL("CPace: LEB128, the generator string, the transcript and ISK on the vectors of the draft") {
    if (!digested()) return;

    namespace cpacing = tal::net::cpacing;

    const Bytes ci = unhexed("0b415f696e69746961746f720b425f726573706f6e646572");
    const Bytes sid = unhexed("7e4b4791d6a8ef019b936c79fb7f2c57");

    SAME(hexed(cpacing::prepended({})), std::string{"00"});
    SAME(hexed(cpacing::prepended(tal::bytes("ab"))), std::string{"026162"});
    const Bytes edge = cpacing::prepended(Bytes(127));
    const Bytes past = cpacing::prepended(Bytes(128));
    const Bytes long_length = cpacing::prepended(Bytes(300));

    SAME(hexed(Bytes(edge.begin(), edge.begin() + 1)), std::string{"7f"});
    SAME(hexed(Bytes(past.begin(), past.begin() + 2)), std::string{"8001"});
    SAME(hexed(Bytes(long_length.begin(), long_length.begin() + 2)), std::string{"ac02"});

    SAME(hexed(cpacing::generator_string(tal::bytes("CPace255"), tal::bytes("Password"), ci, sid, 128)),
         std::string{"0843506163653235350850617373776f72646d000000000000000000"
                     "00000000000000000000000000000000000000000000000000000000"
                     "00000000000000000000000000000000000000000000000000000000"
                     "00000000000000000000000000000000000000000000000000000000"
                     "00000000000000000000000000000000180b415f696e69746961746f"
                     "720b425f726573706f6e646572107e4b4791d6a8ef019b936c79fb7f"
                     "2c57"});

    const Bytes long_one = cpacing::generator_string(tal::bytes("CPace255"), Bytes(200, std::byte{'x'}), {}, {}, 128);

    SAME(long_one.size(), std::size_t{9 + 2 + 200 + 1 + 1 + 1});

    const Bytes ya = unhexed("1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32");
    const Bytes yb = unhexed("248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623");
    const Bytes told = cpacing::transcript(ya, tal::bytes("ADa"), yb, tal::bytes("ADb"));

    SAME(hexed(told), std::string{"201d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc9"
                                  "89ce0c9f320341446120248cccf6d5cdc3646f0ad593f9e6cef4e69d"
                                  "4945f8372e623512ecea3218562303414462"});
    SAME(hexed(cpacing::intermediate(sid, unhexed("5b067effbdc0b2a0e1d907b21ebb25cfedb96a852179a847c37e43ee71322c6b"), told)),
         std::string{"6e19b875f7a561d6b3ca3dbb9ef42ac55de3e717881018204b8922b4"
                     "d5e53bb2aa82c300bea7b65d2b671da71922ddf6472301b79bc270ad"
                     "fa8bf413285f2263"});
}

TRIAL("CPace: Elligator2 and the generator on the vectors of the draft, at the edges of the field and beyond") {
    if (!digested()) return;

    namespace cpacing = tal::net::cpacing;

    const std::string generator = "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f";

    SAME(hexed(cpacing::elligator2(unhexed("03998087bdb1a2617bbe25ef5a7c18cd4f84f902328701790958755ee4aed1d3"))), generator);
    SAME(hexed(cpacing::elligator2(unhexed("03998087bdb1a2617bbe25ef5a7c18cd4f84f902328701790958755ee4aed153"))), generator);
    SAME(hexed(cpacing::generator(tal::bytes("Password"), unhexed("0b415f696e69746961746f720b425f726573706f6e646572"), unhexed("7e4b4791d6a8ef019b936c79fb7f2c57"))),
         generator);

    const std::string zero(64, '0');

    SAME(hexed(cpacing::elligator2(Bytes(32))), zero);
    SAME(hexed(cpacing::elligator2(unhexed("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"))), zero);
    SAME(hexed(cpacing::elligator2(unhexed("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"))),
         std::string{"9cdb525555555555555555555555555555555555555555555555555555555555"});
    SAME(hexed(cpacing::elligator2(Bytes(32, std::byte{0xff}))), std::string{"1e5942dd97c756040d27755f1e5b11349cd47d796c45d07052f7e5b11541c349"});
    SAME(hexed(cpacing::elligator2(unhexed("b0536fe171551bd06194c986f4f68dab5a2925ea13d3b597a60af5fff91c067f"))),
         std::string{"83297175f3042779d57c3e2044026e9f1ac81828cc0951208972abaf5db6f37e"});
    SAME(hexed(cpacing::elligator2(unhexed("c2595004278b88f4395b85c62a1234c0333d85a93a13d632b02874c55e5bcb97"))),
         std::string{"fe116d99766cf12de5328932f69c8daf67633f1eea55ded3b1f053865f3da550"});
    SAME(hexed(cpacing::elligator2(unhexed("94832161f3dd13cbbb8265b8876a8df9806a920e325777bd4d3623b997297129"))),
         std::string{"bd188d56b4dd95fdc8dda4872265b3dafe78a304f84d79e8ad3734435db9ff7f"});
    SAME(thrown([] { (void)tal::net::cpacing::elligator2(Bytes(31)); }), named(tal::Kind::other));
}

TRIAL("CPace: one password agrees, another is denied, and so is a message out of its size or turn") {
    if (!digested()) return;

    auto calling = tal::net::cpace(Side::calling, tal::bytes("1234"), "t", toyed());
    auto answering = tal::net::cpace(Side::answering, tal::bytes("1234"), "t", toyed());

    SAME(thrown([&] { (void)calling->keys(); }), named(tal::Kind::other));

    tal::testing::talked(*calling, *answering);

    OUGHT(calling->ready() && answering->ready());
    OUGHT(calling->keys().sending == answering->keys().receiving);
    OUGHT(calling->keys().receiving == answering->keys().sending);

    auto other = tal::net::cpace(Side::calling, tal::bytes("1234"), "u", toyed());
    auto wrong = tal::net::cpace(Side::answering, tal::bytes("1234"), "t", toyed());

    SAME(thrown([&] { tal::testing::talked(*other, *wrong); }), named(tal::Kind::denied));

    auto short_one = tal::net::cpace(Side::answering, tal::bytes("1234"), "t", toyed());

    SAME(thrown([&] { short_one->heard(Bytes(31)); }), named(tal::Kind::denied));

    auto early = tal::net::cpace(Side::calling, tal::bytes("1234"), "t", toyed());

    SAME(thrown([&] { early->heard(Bytes(96)); }), named(tal::Kind::denied));

    auto zero = tal::net::cpace(Side::answering, tal::bytes("1234"), "t", toyed());

    SAME(thrown([&] { zero->heard(Bytes(32)); }), named(tal::Kind::denied));

    auto brief = tal::net::cpace(Side::calling, tal::bytes("1234"), "t", toyed());

    (void)brief->said();

    SAME(thrown([&] { brief->heard(Bytes(40)); }), named(tal::Kind::denied));
    SAME(thrown([&] { brief->heard(Bytes(10)); }), named(tal::Kind::denied));

    auto caller = tal::net::cpace(Side::calling, tal::bytes("1234"), "t", toyed());
    auto answerer = tal::net::cpace(Side::answering, tal::bytes("1234"), "t", toyed());

    answerer->heard(*caller->said());

    Bytes answer = *answerer->said();

    answer.back() ^= std::byte{1};

    SAME(thrown([&] { caller->heard(answer); }), named(tal::Kind::denied));

    auto last = tal::net::cpace(Side::calling, tal::bytes("1234"), "t", toyed());
    auto lasting = tal::net::cpace(Side::answering, tal::bytes("1234"), "t", toyed());

    lasting->heard(*last->said());
    last->heard(*lasting->said());

    Bytes confirmation = *last->said();

    confirmation.front() ^= std::byte{1};

    SAME(thrown([&] { lasting->heard(confirmation); }), named(tal::Kind::denied));
    SAME(thrown([] { (void)tal::net::cpacing::made(Side::calling, tal::bytes("1234"), "t", toyed(), Bytes(31)); }), named(tal::Kind::other));
}

TRIAL("CPace: the keys of the channel are HKDF of ISK with the label and the context, crossed between the sides") {
    if (!digested()) return;

    namespace cpacing = tal::net::cpacing;

    const Bytes a = unhexed("0101010101010101010101010101010101010101010101010101010101010101");
    const Bytes b = unhexed("0202020202020202020202020202020202020202020202020202020202020202");
    auto calling = cpacing::made(Side::calling, tal::bytes("1234"), "context", toyed(), a);
    auto answering = cpacing::made(Side::answering, tal::bytes("1234"), "context", toyed(), b);

    tal::testing::talked(*calling, *answering);

    const Bytes g = cpacing::generator(tal::bytes("1234"), tal::bytes("context"), {});
    const Bytes ya = *cpacing::multiplied(*toyed(), a, g);
    const Bytes yb = *cpacing::multiplied(*toyed(), b, g);
    const Bytes isk = cpacing::intermediate({}, *cpacing::multiplied(*toyed(), a, yb), cpacing::transcript(ya, {}, yb, {}));
    Bytes info = tal::bytes("talcite cpace 1");

    for (const Bytes& one : {Bytes{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{7}}, tal::bytes("context")}) info.insert(info.end(), one.begin(), one.end());

    const Bytes made = tal::crypto::derived(tal::crypto::Sum::sha512, {}, isk, info, 64);

    OUGHT(calling->keys().sending == Bytes(made.begin(), made.begin() + 32));
    OUGHT(calling->keys().receiving == Bytes(made.begin() + 32, made.end()));
    OUGHT(answering->keys().sending == calling->keys().receiving);
}

#else

TRIAL("CPace: without our own curve it is unsupported") {
    SAME(thrown([] { (void)tal::net::cpace(Side::calling, tal::bytes("1234"), "t", toyed()); }), named(tal::Kind::unsupported));
}

#endif

TRIAL("Sealed: messages both ways in order, empty ones too; the end of the other side is broken") {
    if (!digested()) return;

    tal::Loop loop;
    auto [one, two] = sealed(loop);

    for (int turn = 0; turn < 5; ++turn) tal::wait(loop, one.send(tal::bytes("m" + std::to_string(turn))));

    for (int turn = 0; turn < 5; ++turn) SAME(tal::text::of(tal::wait(loop, two.received())), "m" + std::to_string(turn));

    tal::wait(loop, two.send({}));
    OUGHT(tal::wait(loop, one.received()).empty());

    tal::Scope scope{loop};

    for (int turn = 0; turn < 20; ++turn) scope.run(one.send(tal::bytes(std::to_string(turn))));

    for (int turn = 0; turn < 20; ++turn) OUGHT(!tal::wait(loop, two.received()).empty());

    one.close();

    OUGHT(!one.held());
    SAME(kind_of(loop, two.received()), named(tal::Kind::broken));
    SAME(kind_of(loop, two.received()), named(tal::Kind::broken));
    SAME(kind_of(loop, two.send(tal::bytes("x"))), named(tal::Kind::broken));
    SAME(kind_of(loop, one.send(tal::bytes("x"))), named(tal::Kind::broken));
    SAME(kind_of(loop, one.received()), named(tal::Kind::broken));
}

TRIAL("Sealed: a forged frame and one announced too long close the channel; a message too long is the caller's") {
    if (!digested()) return;

    tal::Loop loop;
    auto [one, raw, unused] = rawed(loop);

    tal::wait(loop, raw.write(Bytes{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{20}}));
    tal::wait(loop, raw.write(Bytes(20, std::byte{7})));

    SAME(kind_of(loop, one.received()), named(tal::Kind::broken));
    SAME(kind_of(loop, one.send(tal::bytes("x"))), named(tal::Kind::broken));
    OUGHT(tal::wait(loop, raw.read(16)).empty());

    auto [two, wide, kept] = rawed(loop);

    tal::wait(loop, wide.write(Bytes{std::byte{0}, std::byte{0x40}, std::byte{0}, std::byte{0x11}}));

    SAME(kind_of(loop, two.received()), named(tal::Kind::malformed));
    OUGHT(tal::wait(loop, wide.read(16)).empty());

    auto [three, four] = sealed(loop);

    SAME(kind_of(loop, three.send(Bytes(tal::net::Sealed::longest + 1))), named(tal::Kind::other));
    const auto [sent, got] = tal::wait(loop, tal::all(
                                                 [](tal::net::Sealed by) -> tal::Task<std::size_t> {
                                                     co_await by.send(Bytes(tal::net::Sealed::longest));

                                                     co_return tal::net::Sealed::longest;
                                                 }(three),
                                                 four.received()));

    SAME(got.size(), sent);
}

TRIAL("Sealed: on the wire a frame is its length, then the message sealed with that length and its count as the nonce") {
    if (!digested()) return;

    tal::Loop loop;
    auto [one, raw, keys] = rawed(loop);

    tal::wait(loop, one.send(tal::bytes("a")));
    tal::wait(loop, one.send(tal::bytes("bc")));

    Bytes wire;

    while (wire.size() < 4 + 17 + 4 + 18) {
        const Bytes came = tal::wait(loop, raw.read(4096));

        wire.insert(wire.end(), came.begin(), came.end());
    }

    const Bytes first_head(wire.begin(), wire.begin() + 4);
    const Bytes first(wire.begin() + 4, wire.begin() + 21);
    const Bytes second_head(wire.begin() + 21, wire.begin() + 25);
    const Bytes second(wire.begin() + 25, wire.end());
    Bytes nonce(12);

    SAME(hexed(first_head), std::string{"00000011"});
    SAME(hexed(second_head), std::string{"00000012"});
    SAME(tal::text::of(*tal::testing::toy()->opened(keys.receiving, nonce, first_head, first)), std::string{"a"});
    OUGHT(!tal::testing::toy()->opened(keys.receiving, nonce, second_head, second).has_value());

    nonce.back() = std::byte{1};

    SAME(tal::text::of(*tal::testing::toy()->opened(keys.receiving, nonce, second_head, second)), std::string{"bc"});
    OUGHT(!tal::testing::toy()->opened(keys.receiving, nonce, first_head, second).has_value());
}

TRIAL("Sealed: a handshake that never ends is malformed after sixteen turns") {
    if (!digested()) return;

    class Endless final : public tal::net::Handshake {
    public:
        std::optional<Bytes> said() override { return std::nullopt; }

        void heard(const Bytes& one) override { (void)one; }

        [[nodiscard]] bool ready() const override { return false; }

        [[nodiscard]] const tal::net::Keying& keys() const override { return keys_; }

    private:
        tal::net::Keying keys_;
    };

    tal::Loop loop;
    auto [near, far] = plain(loop);
    Bytes frames;

    for (int one = 0; one < 20; ++one) frames.insert(frames.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{9}});

    tal::wait(loop, far.write(frames));

    SAME(kind_of(loop, tal::net::Sealed::opened(near, std::make_shared<Endless>(), toyed())), named(tal::Kind::malformed));
}

TRIAL("Sealed: one reader at a time; a failed handshake closes the stream; no handshake is the caller's") {
    if (!digested()) return;

    tal::Loop loop;
    auto [one, two] = sealed(loop);
    tal::Scope scope{loop};

    scope.run([](tal::net::Sealed reader) -> tal::Task<void> { (void)co_await reader.received(); }(two));

    QUIETED(loop);

    SAME(kind_of(loop, two.received()), named(tal::Kind::busy));

    auto [near, far] = plain(loop);
    auto pair = tal::wait(loop, tal::all(
                                    [](tal::Stream stream) -> tal::Task<std::string> {
                                        try {
                                            (void)co_await tal::net::Sealed::opened(stream, tal::net::keyed(Side::calling, tal::crypto::random(32), "t", toyed()), toyed());
                                        } catch (const tal::Trouble& why) {
                                            co_return std::string{tal::spoken(why.kind())};
                                        }

                                        co_return std::string{"none"};
                                    }(near),
                                    [](tal::Stream stream) -> tal::Task<std::string> {
                                        try {
                                            (void)co_await tal::net::Sealed::opened(stream, tal::net::keyed(Side::answering, tal::crypto::random(32), "t", toyed()), toyed());
                                        } catch (const tal::Trouble& why) {
                                            co_return std::string{tal::spoken(why.kind())};
                                        }

                                        co_return std::string{"none"};
                                    }(far)));

    OUGHT(std::get<0>(pair) == named(tal::Kind::denied) || std::get<0>(pair) == named(tal::Kind::broken));
    OUGHT(std::get<1>(pair) == named(tal::Kind::denied) || std::get<1>(pair) == named(tal::Kind::broken));
    SAME(kind_of(loop, tal::net::Sealed::opened(plain(loop).first, nullptr, toyed())), named(tal::Kind::other));
}

TRIAL("Sealed: the built-in cipher passes the trials of every cipher; a build without one says so") {
    if (!digested()) return;

    const std::shared_ptr<tal::crypto::Cipher> builtin = tal::crypto::Cipher::builtin();

    if (builtin == nullptr) return;

    tal::testing::agreed(builtin);
}

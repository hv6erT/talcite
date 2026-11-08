#include "tal/testing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/channel.hpp"
#include "talcite/cipher.hpp"
#include "talcite/digest.hpp"
#include "talcite/handshake.hpp"
#include "talcite/listener.hpp"
#include "talcite/random.hpp"
#include "talcite/seam.hpp"
#include "talcite/stream.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

// The handshake and the encrypted channel. The talcite core does not link the adapters, so the
// ciphers here come from a fake that is not cryptography and does not pretend to be: Diffie-Hellman
// in the field modulo 2^61−1 and a "seal" made by mixing splitmix and FNV. The fake has only the
// properties the trials check (both sides reach the same secret, another key or a changed byte
// breaks the seal), and the real X25519 and AES-GCM are checked against vectors in
// talcite-adapters.
//
// The channel is checked on a fake handshake with fixed keys, so these trials run everywhere. The
// long-key path is computed by HKDF from the system's digests; where the system does not provide
// them, its trials are skipped just like the digest trials.
namespace {

const net::Patience soon{4000};

constexpr std::uint64_t prime = (std::uint64_t{1} << 61) - 1;

// The product of two numbers modulo `prime`, by doubling and adding: every partial sum stays below
// 2^62, so 64 bits are enough and no 128-bit type is needed (MSVC has none).
std::uint64_t times(std::uint64_t one, std::uint64_t two) {
    std::uint64_t made = 0;

    one %= prime;

    for (; two != 0; two >>= 1) {
        if ((two & 1) != 0) made = (made + one) % prime;

        one = (one + one) % prime;
    }

    return made;
}

// `base` raised to `exponent` modulo `prime`, by square and multiply.
std::uint64_t power(std::uint64_t base, std::uint64_t exponent) {
    std::uint64_t made = 1;

    for (; exponent != 0; exponent >>= 1) {
        if ((exponent & 1) != 0) made = times(made, base);

        base = times(base, base);
    }

    return made;
}

// The value as 32 bytes, little-endian in the first eight and zero in the rest.
Bytes spread(std::uint64_t value) {
    Bytes made(32);

    for (std::size_t at = 0; at < 8; ++at) made[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);

    return made;
}

// The value back from the 32 bytes made by `spread`, or nothing when the length is wrong or a byte
// past the eighth is not zero.
std::optional<std::uint64_t> gathered(const Bytes& one) {
    if (one.size() != 32) return std::nullopt;

    std::uint64_t made = 0;

    for (std::size_t at = 0; at < 32; ++at) {
        const auto each = std::to_integer<std::uint64_t>(one[at]);

        if (at >= 8 && each != 0) return std::nullopt;

        if (at < 8) made |= each << (8 * at);
    }

    return made;
}

class Agreeing : public Agreement {
public:
    // Takes the private exponent from the first eight bytes of `secret`, kept within [2, prime − 2].
    explicit Agreeing(const Bytes& secret) {
        for (std::size_t at = 0; at < 8 && at < secret.size(); ++at)
            secret_ |= std::to_integer<std::uint64_t>(secret[at]) << (8 * at);

        secret_ = secret_ % (prime - 3) + 2;
    }

    // The public value, 3 to the power of the secret.
    [[nodiscard]] Bytes mine() const override { return spread(power(3, secret_)); }

    // The shared secret from the other side's public value, or nothing when that value is malformed
    // or outside [2, prime − 2].
    [[nodiscard]] std::optional<Bytes> shared(const Bytes& theirs) const override {
        const std::optional<std::uint64_t> value = gathered(theirs);

        if (!value || *value < 2 || *value > prime - 2) return std::nullopt;

        return spread(power(*value, secret_));
    }

private:
    std::uint64_t secret_ = 0;
};

// The splitmix64 finalizer: scrambles the state into a well-mixed 64-bit value.
std::uint64_t mixed(std::uint64_t state) {
    state += 0x9e3779b97f4a7c15ULL;
    state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
    state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;

    return state ^ (state >> 31);
}

// Pours the bytes into the state FNV-style and mixes in their length.
void poured(std::uint64_t& into, const Bytes& what) {
    for (const std::byte one : what) into = (into ^ std::to_integer<std::uint64_t>(one)) * 0x100000001b3ULL;

    into = mixed(into ^ what.size());
}

class Ciphers : public CipherProvider {
public:
    // The provider name that the seam reports.
    [[nodiscard]] std::string_view name() const override { return "fake ciphers"; }

    // An agreement with a random ephemeral secret.
    [[nodiscard]] std::unique_ptr<Agreement> agreement() override { return std::make_unique<Agreeing>(random(8)); }

    // An agreement with the given secret.
    [[nodiscard]] std::unique_ptr<Agreement> agreement(const Bytes& secret) override {
        return std::make_unique<Agreeing>(secret);
    }

    // The plain text XOR-ed with the key stream, followed by a 16-byte stamp over everything.
    [[nodiscard]] Bytes sealed(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& plain) override {
        Bytes made = streamed(key, nonce, plain);
        const Bytes stamp = stamped(key, nonce, extra, made);

        made.insert(made.end(), stamp.begin(), stamp.end());

        return made;
    }

    // The plain text back, or nothing when the sealed text is shorter than the stamp or the stamp
    // does not match.
    [[nodiscard]] std::optional<Bytes> opened(const Bytes& key, const Bytes& nonce, const Bytes& extra,
                                              const Bytes& sealed) override {
        if (sealed.size() < 16) return std::nullopt;

        const Bytes body(sealed.begin(), sealed.end() - 16);

        if (!(stamped(key, nonce, extra, body) == Bytes(sealed.end() - 16, sealed.end()))) return std::nullopt;

        return streamed(key, nonce, body);
    }

private:
    // Throws unless the key has 32 bytes and the nonce 12, as a real AEAD would require.
    static void sized(const Bytes& key, const Bytes& nonce) {
        if (key.size() != 32 || nonce.size() != 12) throw Trouble{"fake ciphers want a 32-byte key and a 12-byte nonce"};
    }

    // The bytes XOR-ed with a key stream derived from the key and the nonce; the same call undoes it.
    static Bytes streamed(const Bytes& key, const Bytes& nonce, const Bytes& what) {
        sized(key, nonce);

        std::uint64_t seed = 0xcbf29ce484222325ULL;

        poured(seed, key);
        poured(seed, nonce);

        Bytes made = what;

        for (std::size_t at = 0; at < made.size(); ++at)
            made[at] ^= static_cast<std::byte>(mixed(seed + at / 8) >> (8 * (at % 8)));

        return made;
    }

    // A 16-byte stamp over the key, the nonce, the extra data and the body.
    static Bytes stamped(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& body) {
        sized(key, nonce);

        Bytes made;

        for (const std::uint64_t start : {0x84222325cbf29ce4ULL, 0x1b3100000001ULL}) {
            std::uint64_t into = start;

            poured(into, key);
            poured(into, nonce);
            poured(into, extra);
            poured(into, body);

            for (std::size_t at = 0; at < 8; ++at) made.push_back(static_cast<std::byte>(into >> (8 * at)));
        }

        return made;
    }
};

// A seam with the fake ciphers.
Seam<CipherProvider> ciphers() { return Seam<CipherProvider>{std::make_shared<Ciphers>()}; }

// A fake with a fixed ephemeral secret, so that a trial can compute the messages and keys itself,
// from the description of the protocol, and compare them byte for byte.
class Fixed : public Ciphers {
public:
    // A provider whose ephemeral secret is always `secret`.
    explicit Fixed(std::byte secret) : secret_(secret) {}

    // An agreement with the fixed secret instead of a random one.
    [[nodiscard]] std::unique_ptr<Agreement> agreement() override { return std::make_unique<Agreeing>(Bytes{secret_}); }

private:
    std::byte secret_;
};

// A seam with the fake ciphers whose ephemeral secret is fixed to `secret`.
Seam<CipherProvider> fixed(int secret) {
    return Seam<CipherProvider>{std::make_shared<Fixed>(static_cast<std::byte>(secret))};
}

// A fake handshake: the caller asks "talcite?", the answerer replies "talcite!", the keys are fixed.
// Anything else is a refusal; that is how we check that a failed handshake closes the channel.
const Bytes calling_key(32, std::byte{'c'});
const Bytes answering_key(32, std::byte{'a'});

class Greeting : public net::Handshake {
public:
    // A handshake for the given side, with the fixed keys in that side's order.
    explicit Greeting(net::Side side) : side_(side) {
        keys_ = side == net::Side::calling ? net::Keying{calling_key, answering_key}
                                           : net::Keying{answering_key, calling_key};
    }

    // The caller's question at the start, the answerer's reply after hearing it, and nothing else.
    std::optional<Bytes> said() override {
        if (side_ == net::Side::calling && step_ == 0) {
            step_ = 1;

            return bytes("talcite?");
        }

        if (side_ == net::Side::answering && step_ == 1) {
            step_ = 2;
            ready_ = true;

            return bytes("talcite!");
        }

        return std::nullopt;
    }

    // Accepts only the expected message for the side and step; anything else throws.
    void heard(const Bytes& one) override {
        if (side_ == net::Side::answering && step_ == 0 && one == bytes("talcite?")) {
            step_ = 1;

            return;
        }

        if (side_ == net::Side::calling && step_ == 1 && one == bytes("talcite!")) {
            step_ = 2;
            ready_ = true;

            return;
        }

        throw Trouble{"fake handshake heard something else"};
    }

    // Whether the exchange is finished.
    [[nodiscard]] bool ready() const override { return ready_; }

    // The fixed keys of this side.
    [[nodiscard]] const net::Keying& keys() const override { return keys_; }

private:
    net::Side side_;
    net::Keying keys_;
    int step_ = 0;
    bool ready_ = false;
};

// A handshake that never ends: every turn it speaks and listens, and the channel has to stop it by
// itself.
class Endless : public net::Handshake {
public:
    // Says something once after each thing it hears.
    std::optional<Bytes> said() override {
        if (spoke_) return std::nullopt;

        spoke_ = true;

        return bytes("jeszcze");
    }

    // Hearing anything makes it ready to speak again.
    void heard(const Bytes&) override { spoke_ = false; }

    // Never ready.
    [[nodiscard]] bool ready() const override { return false; }

    // Throws, since there are never any keys.
    [[nodiscard]] const net::Keying& keys() const override { throw Trouble{"never ready"}; }

private:
    bool spoke_ = false;
};

struct Pair {
    net::Stream answering;
    net::Stream calling;
};

// Accepts one connection on `gate` while connecting to it, and returns both ends.
Pair joined(const net::Listener& gate) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream answering = std::move(meeting).get();
    net::Stream calling = std::move(joining).get();

    return Pair{std::move(answering), std::move(calling)};
}

// A listener on an IPv4 loopback address with a port chosen by the system.
net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

struct Ends {
    net::Channel answering;
    net::Channel calling;
};

// Two channels opened with the fake handshake and fake ciphers over a fresh connection.
Ends greeted(const net::Listener& gate) {
    Pair pair = joined(gate);

    auto answering = net::Channel::opened(std::move(pair.answering), std::make_shared<Greeting>(net::Side::answering),
                                          ciphers(), soon);
    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                        ciphers(), soon);

    return Ends{std::move(answering).get(), std::move(calling).get()};
}

// The 4-byte big-endian length that starts a frame.
Bytes header(std::size_t much) {
    Bytes made(4);

    for (std::size_t at = 0; at < 4; ++at) made[at] = static_cast<std::byte>((much >> (24 - 8 * at)) & 0xff);

    return made;
}

// The bytes with their length header in front, as an unencrypted handshake frame.
Bytes frame(const Bytes& what) {
    Bytes made = header(what.size());

    made.insert(made.end(), what.begin(), what.end());

    return made;
}

// The 12-byte nonce for a frame counter: four zero bytes, then the counter big-endian.
Bytes counted(std::uint64_t counter) {
    Bytes made(12);

    for (std::size_t at = 0; at < 8; ++at) made[4 + at] = static_cast<std::byte>((counter >> (56 - 8 * at)) & 0xff);

    return made;
}

// A data frame as the channel seals it; this is how the raw stream plays the other side.
Bytes sealed_frame(const Bytes& key, std::uint64_t counter, const Bytes& plain) {
    Ciphers one;

    const Bytes head = header(plain.size() + 16);
    Bytes made = head;
    const Bytes body = one.sealed(key, counted(counter), head, plain);

    made.insert(made.end(), body.begin(), body.end());

    return made;
}

// The raw stream takes in everything, up to the end: as much as was sent to it before closing.
Bytes drained(const net::Stream& one) {
    Bytes made;

    for (;;) {
        const Bytes came = one.read(65536, soon).get();

        if (came.empty()) return made;

        made.insert(made.end(), came.begin(), came.end());
    }
}

// A raw answerer: it takes "talcite?", replies "talcite!", and then talks in frames.
struct Raw {
    net::Stream raw;
    net::Channel calling;
};

// Opens the calling channel against a raw stream that plays the answering side of the fake
// handshake, and returns both.
Raw half(const net::Listener& gate) {
    Pair pair = joined(gate);

    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                        ciphers(), soon);

    Bytes heard;

    while (heard.size() < 12) {
        const Bytes came = pair.answering.read(12 - heard.size(), soon).get();

        heard.insert(heard.end(), came.begin(), came.end());
    }

    pair.answering.write(frame(bytes("talcite!")), soon).get();

    return Raw{std::move(pair.answering), std::move(calling).get()};
}

// Whether `what` occurs anywhere in `in`.
bool found(const Bytes& in, const Bytes& what) {
    for (std::size_t at = 0; at + what.size() <= in.size(); ++at)
        if (Bytes(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + what.size())) ==
            what)
            return true;

    return false;
}

// Whether this system lacks the digests; if so, prints a note that the trial is skipped.
bool missing() {
    if (summing()) return false;

    std::cout << "        this system provides no digests; trial skipped\n";

    return true;
}

// The shared long key of the long-key handshake trials.
Bytes long_key() { return Bytes(32, std::byte{7}); }

// A handshake in memory: messages from one side straight to the other, without a network.
void talked(net::Handshake& calling, net::Handshake& answering) {
    for (int turn = 0; turn < 8 && !(calling.ready() && answering.ready()); ++turn) {
        while (const std::optional<Bytes> one = calling.said()) answering.heard(*one);
        while (const std::optional<Bytes> one = answering.said()) calling.heard(*one);
    }
}

}

TRIAL("Channel: messages go both ways, in order and unchanged") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    const Bytes big = random(1 << 20);

    ends.calling.send(bytes("pierwsza"), soon).get();
    ends.calling.send(Bytes{}, soon).get();
    ends.calling.send(big, soon).get();
    ends.answering.send(bytes("odpowiedź"), soon).get();

    OUGHT(ends.answering.receive(soon).get() == bytes("pierwsza"));
    OUGHT(ends.answering.receive(soon).get().empty());
    OUGHT(ends.answering.receive(soon).get() == big);
    OUGHT(ends.calling.receive(soon).get() == bytes("odpowiedź"));
}

TRIAL("Channel: many sends at once arrive in order") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    std::vector<Promise<void>> sending;

    for (int at = 0; at < 50; ++at) sending.push_back(ends.calling.send(bytes(std::to_string(at)), soon));

    for (int at = 0; at < 50; ++at) OUGHT(ends.answering.receive(soon).get() == bytes(std::to_string(at)));

    for (auto& one : sending) std::move(one).get();
}

// Sends from many threads at once: each frame is sealed with the next number, and the frames must
// reach the wire in the order of their numbers, or the other side refuses them. Every thread's
// messages arrive, each one whole, and in the order that thread sent them.
TRIAL("Channel: sends from many threads at once keep the order of their seals") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    constexpr int threads = 8;
    constexpr int each = 40;

    std::vector<std::thread> sending;
    std::vector<std::vector<Promise<void>>> sent(threads);

    for (int who = 0; who < threads; ++who)
        sending.emplace_back([&ends, &sent, who] {
            for (int at = 0; at < each; ++at)
                sent[static_cast<std::size_t>(who)].push_back(
                    ends.calling.send(bytes(std::to_string(who) + ":" + std::to_string(at)), soon));
        });

    for (std::thread& one : sending) one.join();

    std::vector<int> next(threads, 0);

    for (int got = 0; got < threads * each; ++got) {
        const std::string came = text(ends.answering.receive(soon).get());
        const std::size_t colon = came.find(':');

        OUGHT(colon != std::string::npos);

        const int who = std::stoi(came.substr(0, colon));
        const int at = std::stoi(came.substr(colon + 1));

        SAME(at, next[static_cast<std::size_t>(who)]);

        ++next[static_cast<std::size_t>(who)];
    }

    for (auto& among : sent)
        for (auto& one : among) std::move(one).get();
}

// Cancelling one send lets go of its result only: the sends before it are not cancelled with it,
// and its frame, already sealed, still goes out whole and in its place, so the channel goes on.
TRIAL("Channel: a cancelled send cancels nothing before it and breaks nothing after it") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    const Bytes big = random(4 << 20);

    Promise<void> first = ends.calling.send(big, soon);
    Promise<void> second = ends.calling.send(bytes("druga"), soon);

    second.cancel();

    Promise<void> third = ends.calling.send(bytes("trzecia"), soon);

    OUGHT(ends.answering.receive(soon).get() == big);
    OUGHT(ends.answering.receive(soon).get() == bytes("druga"));
    OUGHT(ends.answering.receive(soon).get() == bytes("trzecia"));

    std::move(first).get();
    THREW(std::move(second).get());
    std::move(third).get();
}

TRIAL("Channel: there is no plain text on the wire") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    const Bytes secret = bytes("TAJNE TAJNE TAJNE");

    raw.calling.send(secret, soon).get();
    raw.calling.close();

    const Bytes wire = drained(raw.raw);

    SAME(wire.size(), static_cast<std::size_t>(4 + 17 + 16));
    OUGHT(!found(wire, secret));
    OUGHT(wire == sealed_frame(calling_key, 0, secret));
}

TRIAL("Channel: a frame from the other side opens with the receiving key") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    raw.raw.write(sealed_frame(answering_key, 0, bytes("raz")), soon).get();
    raw.raw.write(sealed_frame(answering_key, 1, bytes("dwa")), soon).get();

    OUGHT(raw.calling.receive(soon).get() == bytes("raz"));
    OUGHT(raw.calling.receive(soon).get() == bytes("dwa"));
}

TRIAL("Channel: a forged frame is a refusal and a close") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    Bytes forged = sealed_frame(answering_key, 0, bytes("prawda"));

    forged[6] ^= std::byte{1};

    raw.raw.write(forged, soon).get();

    THREW(raw.calling.receive(soon).get());

    OUGHT(drained(raw.raw).empty());
}

TRIAL("Channel: a repeated or reordered frame is a refusal") {
    {
        const net::Listener gate = standing();
        Raw raw = half(gate);

        const Bytes once = sealed_frame(answering_key, 0, bytes("raz"));

        raw.raw.write(once, soon).get();
        raw.raw.write(once, soon).get();

        OUGHT(raw.calling.receive(soon).get() == bytes("raz"));
        THREW(raw.calling.receive(soon).get());
    }
    {
        const net::Listener gate = standing();
        Raw raw = half(gate);

        raw.raw.write(sealed_frame(answering_key, 1, bytes("dwa")), soon).get();

        THREW(raw.calling.receive(soon).get());
    }
}

TRIAL("Channel: a frame sealed with the same side's key is a refusal") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    raw.raw.write(sealed_frame(calling_key, 0, bytes("odbite")), soon).get();

    THREW(raw.calling.receive(soon).get());
}

TRIAL("Channel: announcing a too long frame is a refusal") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    raw.raw.write(header((4u << 20) + 17), soon).get();

    THREW(raw.calling.receive(soon).get());

    OUGHT(drained(raw.raw).empty());
}

TRIAL("Channel: a too long frame already read with the previous one rejects the promise, not the call") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    Bytes both = sealed_frame(answering_key, 0, bytes("raz"));
    const Bytes over = header((4u << 20) + 17);

    both.insert(both.end(), over.begin(), over.end());

    raw.raw.write(both, soon).get();

    OUGHT(raw.calling.receive(soon).get() == bytes("raz"));

    Promise<Bytes> next;

    bool called = false;

    try {
        next = raw.calling.receive(soon);
        called = true;
    } catch (...) {
    }

    OUGHT(called);
    THREW(std::move(next).get());
}

TRIAL("Channel: many frames read at once come out whole and in order") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    Bytes many;

    for (std::uint64_t at = 0; at < 300; ++at) {
        const Bytes one = sealed_frame(answering_key, at, bytes("ramka " + std::to_string(at)));

        many.insert(many.end(), one.begin(), one.end());
    }

    raw.raw.write(many, soon).get();

    for (std::uint64_t at = 0; at < 300; ++at) OUGHT(raw.calling.receive(soon).get() == bytes("ramka " + std::to_string(at)));
}

TRIAL("Channel: the longest message passes, a longer one is refused") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    const Bytes most = random(4u << 20);

    auto sending = ends.calling.send(most, net::Patience{20000});

    OUGHT(ends.answering.receive(net::Patience{20000}).get() == most);

    std::move(sending).get();

    THREW(ends.calling.send(Bytes((4u << 20) + 1), soon).get());
}

TRIAL("Channel: the end of the stream in the middle of a frame is trouble") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    const Bytes whole = sealed_frame(answering_key, 0, bytes("urwana wiadomość"));

    raw.raw.write(Bytes(whole.begin(), whole.begin() + 10), soon).get();
    raw.raw.close();

    THREW(raw.calling.receive(soon).get());
}

// A refusal at once, not after the patience runs out: the other side is silent, so only the check
// on entry can end the matter quickly.
TRIAL("Channel: without a handshake or a cipher it refuses at once") {
    const net::Listener gate = standing();
    const net::Patience long_enough{60000};
    const auto started = std::chrono::steady_clock::now();

    {
        Pair pair = joined(gate);

        THREW(net::Channel::opened(std::move(pair.calling), nullptr, ciphers(), long_enough).get());
    }
    {
        Pair pair = joined(gate);

        THREW(net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                   Seam<CipherProvider>{}, long_enough)
                  .get());
    }

    OUGHT(std::chrono::steady_clock::now() - started < std::chrono::seconds{20});
}

TRIAL("Channel: a failed handshake closes the stream") {
    const net::Listener gate = standing();
    Pair pair = joined(gate);

    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                        ciphers(), soon);

    Bytes heard;

    while (heard.size() < 12) {
        const Bytes came = pair.answering.read(12 - heard.size(), soon).get();

        heard.insert(heard.end(), came.begin(), came.end());
    }

    OUGHT(heard == frame(bytes("talcite?")));

    pair.answering.write(frame(bytes("obcy!")), soon).get();

    THREW(std::move(calling).get());

    OUGHT(drained(pair.answering).empty());
}

TRIAL("Channel: an endless handshake stops by itself") {
    const net::Listener gate = standing();
    Pair pair = joined(gate);

    // A long patience: the limit on turns has to stop it, not the passing of time.
    const net::Patience long_enough{60000};
    const auto started = std::chrono::steady_clock::now();

    auto answering =
        net::Channel::opened(std::move(pair.answering), std::make_shared<Endless>(), ciphers(), long_enough);
    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Endless>(), ciphers(), long_enough);

    THREW(std::move(answering).get());
    THREW(std::move(calling).get());

    OUGHT(std::chrono::steady_clock::now() - started < std::chrono::seconds{20});
}

TRIAL("Channel: a closed one refuses and has no address") {
    const net::Listener gate = standing();
    Ends ends = greeted(gate);

    OUGHT(ends.calling.held());
    OUGHT(ends.calling.peer().port() == gate.mine().port());

    ends.calling.close();

    OUGHT(!ends.calling.held());
    SAME(ends.calling.peer().port(), static_cast<std::uint16_t>(0));
    THREW(ends.calling.send(bytes("po"), soon).get());
    THREW(ends.calling.receive(soon).get());
    THREW(ends.answering.receive(soon).get());
}

TRIAL("Long-key handshake: both sides have the same keys, crosswise") {
    if (missing()) return;

    const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
    const auto answering = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());

    talked(*calling, *answering);

    OUGHT(calling->ready());
    OUGHT(answering->ready());
    SAME(calling->keys().sending.size(), static_cast<std::size_t>(32));
    OUGHT(calling->keys().sending == answering->keys().receiving);
    OUGHT(calling->keys().receiving == answering->keys().sending);
    OUGHT(!(calling->keys().sending == calling->keys().receiving));
    OUGHT(!(calling->keys().sending == long_key()));
}

// Messages and keys computed here straight from the description of the protocol: HKDF-SHA-512 with
// the long key as the salt, the result of the agreement as the secret, and as the info the label,
// the context and both e messages, each with its length in front; the confirmations are
// HMAC-SHA-256 of the side's label. A change to any detail makes a different protocol, and two
// versions of twa would stop understanding each other, so the trial has to see it.
TRIAL("Long-key handshake: bytes agree with the description of the protocol") {
    if (missing()) return;

    const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", fixed(5));
    const auto answering = net::keyed(net::Side::answering, long_key(), "twa 1", fixed(9));

    const Agreeing mine{Bytes{std::byte{5}}};
    const Agreeing theirs{Bytes{std::byte{9}}};

    const Bytes e_c = mine.mine();
    const Bytes e_r = theirs.mine();

    Bytes info = bytes("talcite keyed 1");

    for (const Bytes& one : {bytes("twa 1"), e_c, e_r}) {
        const Bytes head = header(one.size());

        info.insert(info.end(), head.begin(), head.end());
        info.insert(info.end(), one.begin(), one.end());
    }

    const Bytes made = derived(Sum::sha512, long_key(), *mine.shared(e_r), info, 128);

    const auto part = [&made](std::size_t which) {
        return Bytes(made.begin() + static_cast<std::ptrdiff_t>(32 * which),
                     made.begin() + static_cast<std::ptrdiff_t>(32 * which + 32));
    };

    Bytes second = e_r;
    const Bytes confirm_answering = stamped(Sum::sha256, part(2), bytes("answering"));

    second.insert(second.end(), confirm_answering.begin(), confirm_answering.end());

    const Bytes first = *calling->said();

    OUGHT(first == e_c);

    answering->heard(first);

    OUGHT(*answering->said() == second);

    calling->heard(second);

    OUGHT(*calling->said() == stamped(Sum::sha256, part(3), bytes("calling")));
    OUGHT(calling->keys().sending == part(0));
    OUGHT(calling->keys().receiving == part(1));
}

TRIAL("Long-key handshake: three messages and nothing more") {
    if (missing()) return;

    const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
    const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

    OUGHT(!answering->said());

    const std::optional<Bytes> first = calling->said();

    OUGHT(first.has_value());
    SAME(first->size(), static_cast<std::size_t>(32));
    OUGHT(!calling->said());

    answering->heard(*first);

    const std::optional<Bytes> second = answering->said();

    OUGHT(second.has_value());
    SAME(second->size(), static_cast<std::size_t>(64));
    OUGHT(!answering->ready());

    calling->heard(*second);

    OUGHT(!calling->ready());

    const std::optional<Bytes> third = calling->said();

    OUGHT(third.has_value());
    SAME(third->size(), static_cast<std::size_t>(32));
    OUGHT(calling->ready());
    OUGHT(!calling->said());

    answering->heard(*third);

    OUGHT(answering->ready());
    OUGHT(!answering->said());
}

TRIAL("Long-key handshake: fresh keys every time") {
    if (missing()) return;

    const auto one = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
    const auto two = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());
    const auto three = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
    const auto four = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());

    talked(*one, *two);
    talked(*three, *four);

    OUGHT(one->ready() && three->ready());
    OUGHT(!(one->keys().sending == three->keys().sending));
}

TRIAL("Long-key handshake: another key or context is a refusal") {
    if (missing()) return;

    Bytes other = long_key();

    other[31] ^= std::byte{1};

    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
        const auto answering = net::keyed(net::Side::answering, other, "twa 1", ciphers());

        answering->heard(*calling->said());

        THREW(calling->heard(*answering->said()));
        OUGHT(!calling->ready());
        THREW((void)calling->keys());
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "twa 2", ciphers());

        answering->heard(*calling->said());

        THREW(calling->heard(*answering->said()));
    }
}

TRIAL("Long-key handshake: a changed byte in any message is a refusal") {
    if (missing()) return;

    for (int which = 0; which < 3; ++which) {
        const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());

        bool refused = false;

        try {
            Bytes first = *calling->said();

            if (which == 0) first[0] ^= std::byte{1};

            answering->heard(first);

            Bytes second = *answering->said();

            if (which == 1) second[40] ^= std::byte{1};

            calling->heard(second);

            Bytes third = *calling->said();

            if (which == 2) third[5] ^= std::byte{1};

            answering->heard(third);
        } catch (const Trouble&) {
            refused = true;
        }

        OUGHT(refused);
        OUGHT(!answering->ready());
    }
}

TRIAL("Long-key handshake: a short key, a wrong length, a bad point and a wrong turn are refusals") {
    if (missing()) return;

    THREW((void)net::keyed(net::Side::calling, Bytes(31, std::byte{7}), "", ciphers()));
    THREW((void)net::keyed(net::Side::calling, long_key(), "", Seam<CipherProvider>{}));

    {
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        THREW(answering->heard(Bytes(31, std::byte{9})));
    }
    {
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        THREW(answering->heard(Bytes(32)));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());

        THREW(calling->heard(Bytes(64)));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        answering->heard(*calling->said());

        const Bytes second = *answering->said();

        THREW(calling->heard(Bytes(second.begin(), second.begin() + 63)));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        answering->heard(*calling->said());

        Bytes second = *answering->said();

        second.push_back(std::byte{0});

        THREW(calling->heard(second));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        answering->heard(*calling->said());
        calling->heard(*answering->said());

        Bytes third = *calling->said();

        third.push_back(std::byte{0});

        THREW(answering->heard(third));
        OUGHT(!answering->ready());
    }
}

TRIAL("Long-key channel: talks over the network, does not start with another key") {
    if (missing()) return;

    const net::Listener gate = standing();

    {
        Pair pair = joined(gate);

        auto answering = net::Channel::opened(std::move(pair.answering),
                                              net::keyed(net::Side::answering, long_key(), "twa 1", ciphers()),
                                              ciphers(), soon);
        auto calling = net::Channel::opened(std::move(pair.calling),
                                            net::keyed(net::Side::calling, long_key(), "twa 1", ciphers()), ciphers(),
                                            soon);

        const net::Channel one = std::move(answering).get();
        const net::Channel two = std::move(calling).get();

        two.send(bytes("dzień dobry"), soon).get();
        one.send(bytes("dobry wieczór"), soon).get();

        OUGHT(one.receive(soon).get() == bytes("dzień dobry"));
        OUGHT(two.receive(soon).get() == bytes("dobry wieczór"));
    }
    {
        Pair pair = joined(gate);

        Bytes other = long_key();

        other[0] ^= std::byte{1};

        auto answering = net::Channel::opened(std::move(pair.answering),
                                              net::keyed(net::Side::answering, other, "twa 1", ciphers()), ciphers(),
                                              soon);
        auto calling = net::Channel::opened(std::move(pair.calling),
                                            net::keyed(net::Side::calling, long_key(), "twa 1", ciphers()), ciphers(),
                                            soon);

        THREW(std::move(calling).get());
        THREW(std::move(answering).get());
    }
}

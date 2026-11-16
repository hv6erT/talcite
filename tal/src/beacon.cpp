#include "tal/peer.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "tal/crypto.hpp"
#include "tal/executor.hpp"
#include "tal/trouble.hpp"

// A beacon in the local network, ported from talcite's beacon.cpp. The offering side listens on a
// known UDP port and answers questions about a tag it offers; the asking side broadcasts the
// question and gathers the answers. So a joining side finds an invitation by its code alone. The
// tag and the answer are the caller's bytes, whose meaning the beacon does not know.
//
// A message: `TLCB`, the version, the kind (question, answer), eight random bytes of the question
// (an answer repeats them, so the asking side knows answers to its own question), the tag with
// its length, and in an answer the content with its length. Anything of another shape is passed
// over without a word, since the port stands open to the network. Broadcast goes over IPv4 alone,
// which every home network has; an answer goes straight back to the one who asked, and at most
// `answers_per_second` of them, so that a flood of questions makes no amplifier of the beacon.
namespace tal::net {

namespace {

constexpr std::byte magic[4]{std::byte{'T'}, std::byte{'L'}, std::byte{'C'}, std::byte{'B'}};

constexpr std::byte version{1};

constexpr std::byte asking{1};

constexpr std::byte answering{2};

constexpr std::size_t once = 8;

constexpr std::size_t widest_datagram = 4 + 2 + once + 1 + Beacon::widest_tag + 2 + Beacon::widest_payload;

constexpr int answers_per_second = 32;

constexpr int askings = 3;

// A message taken apart.
struct Read {
    std::byte kind{};
    Bytes nonce;
    Bytes tag;
    Bytes payload;
};

// The message of the kind `kind` with the nonce `nonce` and the tag `tag`, and an answer's
// `payload` (none for a question).
Bytes written(std::byte kind, const Bytes& nonce, const Bytes& tag, const Bytes* payload) {
    Bytes made(magic, magic + 4);

    made.push_back(version);
    made.push_back(kind);
    made.insert(made.end(), nonce.begin(), nonce.end());
    made.push_back(static_cast<std::byte>(tag.size()));
    made.insert(made.end(), tag.begin(), tag.end());

    if (payload != nullptr) {
        made.push_back(static_cast<std::byte>(payload->size() >> 8));
        made.push_back(static_cast<std::byte>(payload->size() & 0xff));
        made.insert(made.end(), payload->begin(), payload->end());
    }

    return made;
}

// The message `got` taken apart, or nothing when it has not exactly its shape.
std::optional<Read> read(const Bytes& got) {
    if (got.size() < 4 + 2 + once + 1 || std::memcmp(got.data(), magic, 4) != 0 || got[4] != version) return std::nullopt;

    Read made;
    std::size_t at = 6;

    made.kind = got[5];
    made.nonce.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.begin() + static_cast<std::ptrdiff_t>(at + once));
    at += once;

    const auto tagged = std::to_integer<std::size_t>(got[at++]);

    if (tagged > Beacon::widest_tag || at + tagged > got.size()) return std::nullopt;

    made.tag.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.begin() + static_cast<std::ptrdiff_t>(at + tagged));
    at += tagged;

    if (made.kind == asking) return at == got.size() ? std::optional{made} : std::nullopt;

    if (made.kind != answering || at + 2 > got.size()) return std::nullopt;

    const std::size_t much = (std::to_integer<std::size_t>(got[at]) << 8) | std::to_integer<std::size_t>(got[at + 1]);

    at += 2;

    if (much > Beacon::widest_payload || at + much != got.size()) return std::nullopt;

    made.payload.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.end());

    return made;
}

// Throws `Trouble` (other) for a tag of no bytes or of more than `Beacon::widest_tag`.
void tagged(const Bytes& tag) {
    if (tag.empty() || tag.size() > Beacon::widest_tag) throw Trouble{Kind::other, "A beacon's tag takes 1 to 64 bytes."};
}

// The tag `tag` as the key of the offers.
std::string keyed(const Bytes& tag) { return std::string{reinterpret_cast<const char*>(tag.data()), tag.size()}; }

// The tasks that listen for the beacons of the program, on the io pool.
Scope& listening() {
    static Scope one{io()};

    return one;
}

}

// A beacon: its socket, what it offers, and how many answers went this second.
struct Beacon::Held {
    // Closes the socket, which ends the listening.
    ~Held() { post.close(); }

    Udp post;
    Address mine;
    std::mutex guard;
    std::map<std::string, Bytes> offers;
    std::chrono::steady_clock::time_point second{};
    int answered = 0;

    // The answer to the question `got` about something offered, or nothing; nothing too beyond
    // `answers_per_second` in one second.
    std::optional<Bytes> answer(const Bytes& got) {
        const std::optional<Read> asked = read(got);

        if (!asked || asked->kind != asking) return std::nullopt;

        const std::lock_guard<std::mutex> kept{guard};
        const auto found = offers.find(keyed(asked->tag));

        if (found == offers.end()) return std::nullopt;

        const auto now = std::chrono::steady_clock::now();

        if (now - second >= std::chrono::seconds{1}) {
            second = now;
            answered = 0;
        }

        if (++answered > answers_per_second) return std::nullopt;

        return written(answering, asked->nonce, asked->tag, &found->second);
    }
};

namespace {

// The listening of the beacon `weak` on `post`: a datagram, its answer, the next. It holds the
// beacon only for a step, so a beacon gone ends it, as does its socket closed; a failed answer
// loses that answer alone.
Task<void> listened(Udp post, std::weak_ptr<Beacon::Held> weak) {
    for (;;) {
        Datagram got;

        try {
            got = co_await post.received(widest_datagram + 1);
        } catch (const Trouble&) {
            co_return;
        }

        std::optional<Bytes> said;

        if (const std::shared_ptr<Beacon::Held> self = weak.lock()) said = got.cut ? std::nullopt : self->answer(got.bytes);
        else co_return;

        if (!said) continue;

        try {
            co_await post.send(got.from, std::move(*said));
        } catch (const Trouble&) {
        }
    }
}

}

// A beacon on `port` of every IPv4 address of this computer, answering at once.
Task<Beacon> Beacon::opened(std::uint16_t port) {
    auto held = std::make_shared<Held>();

    held->post = co_await Udp::bound(Address::any(Family::four, port));
    held->mine = held->post.mine();

    listening().run(listened(held->post, held));

    co_return Beacon{std::move(held)};
}

// The address it listens on; empty once closed.
Address Beacon::mine() const { return held_ == nullptr ? Address{} : held_->mine; }

// Answers questions about `tag` with `payload` from now on, instead of an earlier offer of `tag`.
// Throws `Trouble` (other) for a tag or a payload too long, and on a closed beacon.
void Beacon::offer(Bytes tag, Bytes payload) const {
    tagged(tag);

    if (payload.size() > widest_payload) throw Trouble{Kind::other, "A beacon's answer takes at most 1024 bytes."};

    if (held_ == nullptr) throw Trouble{Kind::other, "The beacon is closed."};

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->offers[keyed(tag)] = std::move(payload);
}

// Answers no more questions about `tag`.
void Beacon::withdraw(const Bytes& tag) const {
    if (held_ == nullptr) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->offers.erase(keyed(tag));
}

// Closes the socket and drops every offer.
void Beacon::close() {
    if (held_ == nullptr) return;

    held_->post.close();
    held_.reset();
}

// The answers to a question about `tag`: broadcast on `port` (or sent to `to`, as on one computer
// and in the trials), asked again each third of `lasting`, gathered until `lasting` ends. One
// answer of one sender counts once; nobody answering is an empty list, not a trouble.
Task<std::vector<Sighting>> Beacon::sought(std::uint16_t port, Bytes tag, std::chrono::milliseconds lasting, std::vector<Address> to) {
    tagged(tag);

    Udp post = co_await Udp::bound(Address::any(Family::four, 0));
    const Bytes nonce = crypto::random(once);
    const Bytes question = written(asking, nonce, tag, nullptr);
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + lasting;
    const auto step = lasting / askings;
    std::vector<Sighting> found;
    int asked = 0;

    post.broadcast(true);

    if (to.empty()) to.push_back(Address{"255.255.255.255", port});

    for (auto now = start; now < until; now = std::chrono::steady_clock::now()) {
        if (asked < askings && now >= start + asked * step) {
            for (const Address& one : to) {
                try {
                    co_await post.send(one, question);
                } catch (const Trouble&) {
                }
            }

            ++asked;
        }

        const auto edge = asked < askings ? std::min(until, start + asked * step) : until;
        Datagram got;

        try {
            got = co_await within(std::chrono::duration_cast<std::chrono::milliseconds>(edge - std::chrono::steady_clock::now()) + std::chrono::milliseconds{1},
                                  post.received(widest_datagram + 1));
        } catch (const Overdue&) {
            continue;
        }

        const std::optional<Read> told = got.cut ? std::nullopt : read(got.bytes);

        if (!told || told->kind != answering || told->nonce != nonce || told->tag != tag) continue;

        Sighting one{got.from, told->payload};

        if (std::find(found.begin(), found.end(), one) == found.end()) found.push_back(std::move(one));
    }

    post.close();

    co_return found;
}

}

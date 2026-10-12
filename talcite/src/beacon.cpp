#include "talcite/beacon.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "talcite/random.hpp"
#include "talcite/udp.hpp"

// A beacon in the local network. The offering side listens on a well-known UDP port and answers
// questions about a tag it knows; the asking side broadcasts the question in its network and
// collects the answers. This is how a joining side finds an invitation without an address: the code
// is enough. The tag and the answer's content are the caller's bytes: the beacon does not know what
// they mean and should not (twa puts a hash of the label here, not the label itself, and a port
// number).
//
// The message: `TLCB`, the version, the kind (question, answer), eight random bytes from the
// question (the answer repeats them, so the asking side recognizes answers to its own question and
// not to someone else's), the length and the tag, and in an answer also a length and the content.
// Anything that does not fit this shape (another version, truncated, too long, extra bytes) is
// ignored without a word: the port stands in the network, and a stranger's datagram may not change
// anything.
//
// We broadcast only over IPv4 (255.255.255.255): a home network always has IPv4, and IPv6 without
// it is rare. The answer goes straight to the asking side, not by broadcast.
namespace talcite::net {

namespace {

constexpr std::byte magic[4]{std::byte{'T'}, std::byte{'L'}, std::byte{'C'}, std::byte{'B'}};
constexpr std::byte version{1};
constexpr std::byte asking{1};
constexpr std::byte answering{2};
constexpr std::size_t nonce = 8;
constexpr std::size_t widest_datagram = 4 + 2 + nonce + 1 + Beacon::widest_tag + 2 + Beacon::widest_payload;

// The most answers per second the beacon gives, so that a flood of questions does not turn it into
// an amplifier.
constexpr int answers_per_second = 32;

struct Read {
    std::byte kind{};
    Bytes nonce;
    Bytes tag;
    Bytes payload;
};

// A beacon message of the given kind; `payload` is null for a question, which carries no content.
Bytes written(std::byte kind, const Bytes& once, const Bytes& tag, const Bytes* payload) {
    Bytes made(magic, magic + 4);

    made.push_back(version);
    made.push_back(kind);
    made.insert(made.end(), once.begin(), once.end());
    made.push_back(static_cast<std::byte>(tag.size()));
    made.insert(made.end(), tag.begin(), tag.end());

    if (payload != nullptr) {
        made.push_back(static_cast<std::byte>(payload->size() >> 8));
        made.push_back(static_cast<std::byte>(payload->size() & 0xff));
        made.insert(made.end(), payload->begin(), payload->end());
    }

    return made;
}

// A beacon message taken apart, or nothing when the bytes do not have exactly its shape.
std::optional<Read> read(const Bytes& got) {
    if (got.size() < 4 + 2 + nonce + 1 || std::memcmp(got.data(), magic, 4) != 0 || got[4] != version) return std::nullopt;

    Read made;

    // The kind and the tag's length are checked by the reader: the beacon answers only questions
    // about its own tag, and the asking side takes only answers with its own, so neither knows any
    // other kind or a longer tag.
    made.kind = got[5];

    std::size_t at = 6;

    made.nonce.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.begin() + static_cast<std::ptrdiff_t>(at + nonce));
    at += nonce;

    const auto tagged = static_cast<std::size_t>(got[at]);

    ++at;

    if (at + tagged > got.size()) return std::nullopt;

    made.tag.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.begin() + static_cast<std::ptrdiff_t>(at + tagged));
    at += tagged;

    if (made.kind == asking) return at == got.size() ? std::optional{made} : std::nullopt;

    if (at + 2 > got.size()) return std::nullopt;

    const std::size_t much = (static_cast<std::size_t>(got[at]) << 8) | static_cast<std::size_t>(got[at + 1]);

    at += 2;

    if (much > Beacon::widest_payload || at + much != got.size()) return std::nullopt;

    made.payload.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.end());

    return made;
}

}

struct Beacon::Held : std::enable_shared_from_this<Beacon::Held> {
    // Closes the socket on the way out. Receiving in the hub keeps the socket alive, so the
    // beacon's death alone would not close it, and a receive without a deadline would then wait
    // until the end of the program. We close the socket directly: once closed, it refuses receiving
    // and sending, and the chain ends by itself. Canceling the chain alone would not reach an
    // answer being sent in `settled` at that moment.
    ~Held() { post.close(); }

    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

    Udp post;
    Address mine;

    std::mutex guard;
    std::map<Bytes, Bytes> offers;

    std::optional<Promise<void>> running;

    std::chrono::steady_clock::time_point second{};
    int answered = 0;

    // The answer to a question that matches something we offer, or nothing. Answers beyond
    // `answers_per_second` in one second are dropped.
    std::optional<Bytes> answer(const Bytes& got) {
        const std::optional<Read> asked = read(got);

        if (!asked || asked->kind != asking) return std::nullopt;

        const std::lock_guard<std::mutex> kept{guard};

        const auto found = offers.find(asked->tag);

        if (found == offers.end()) return std::nullopt;

        const auto now = std::chrono::steady_clock::now();

        if (now - second >= std::chrono::seconds{1}) {
            second = now;
            answered = 0;
        }

        if (++answered > answers_per_second) return std::nullopt;

        return written(answering, asked->nonce, asked->tag, &found->second);
    }

    // The listening loop: a datagram, an answer, the next datagram. The loop holds the beacon
    // weakly and only for the moment of each step, so a beacon that is gone or closed ends it: a
    // closed socket refuses to receive. A send that fails loses one answer, not the loop.
    static Promise<void> listened(std::weak_ptr<Held> weak) {
        for (;;) {
            Promise<Datagram> coming;

            {
                const std::shared_ptr<Held> self = weak.lock();

                if (!self || !self->post.held()) co_return;

                coming = self->post.receive(widest_datagram + 1, Patience{0});
            }

            Datagram got;

            try {
                got = co_await std::move(coming);
            } catch (...) {
                co_return;
            }

            Promise<void> sent;

            {
                const std::shared_ptr<Held> self = weak.lock();

                if (!self) co_return;

                std::optional<Bytes> said = self->answer(got.bytes);

                if (!said) continue;

                sent = self->post.send(got.from, std::move(*said), Patience{1000});
            }

            try {
                co_await std::move(sent);
            } catch (const Cancelled&) {
                throw;
            } catch (...) {
            }
        }
    }
};

namespace {

// The empty address a closed beacon reports.
const Address& nowhere() {
    static const Address one;

    return one;
}

}

// The beacon listens on all IPv4 addresses of this computer, on the given port, and starts
// answering at once.
Beacon::Beacon(std::uint16_t port) : held_(std::make_shared<Held>()) {
    held_->post = Udp{Address::any(Family::four, port)};
    held_->mine = held_->post.mine();
    held_->running.emplace(Held::listened(held_));
}

// The address the beacon listens on, or an empty address once it is closed.
const Address& Beacon::mine() const { return held_ ? held_->mine : nowhere(); }

// Starts answering questions about `tag` with `payload`, replacing an earlier offer of the same
// tag. Throws for a tag outside 1 to 64 bytes, a payload over 1024 bytes, or a closed beacon.
void Beacon::offer(Bytes tag, Bytes payload) const {
    if (tag.empty() || tag.size() > widest_tag) throw Trouble{"A beacon tag takes 1 to 64 bytes."};

    if (payload.size() > widest_payload) throw Trouble{"A beacon answer takes at most 1024 bytes."};

    if (!held_) throw Trouble{"The beacon is closed."};

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->offers[std::move(tag)] = std::move(payload);
}

// Stops answering questions about `tag`. Does nothing on a closed beacon.
void Beacon::withdraw(const Bytes& tag) const {
    if (!held_) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->offers.erase(tag);
}

// Closes the socket and drops every offer; the listening loop ends by itself.
void Beacon::close() {
    if (!held_) return;

    held_->post.close();
    held_.reset();
}

namespace {

struct Seeking {
    Udp post;
    Bytes once;
    Bytes tag;
    std::chrono::steady_clock::time_point until;
    std::vector<Sighting> found;
};

// Collects answers to our question until the patience runs out, each sender and content once; ends
// with the error of the receive that waited past the deadline.
Promise<void> gathered(std::shared_ptr<Seeking> seeking) {
    for (;;) {
        const auto left = std::chrono::duration_cast<Patience>(seeking->until - std::chrono::steady_clock::now());

        if (left <= Patience{0}) co_return;

        const Datagram got = co_await seeking->post.receive(widest_datagram + 1, left);

        const std::optional<Read> told = read(got.bytes);

        if (told && told->kind == answering && told->nonce == seeking->once && told->tag == seeking->tag) {
            Sighting one{got.from, told->payload};

            if (std::find(seeking->found.begin(), seeking->found.end(), one) == seeking->found.end())
                seeking->found.push_back(std::move(one));
        }
    }
}

// The question sent to each address in turn. An address that the send refuses (no network, no
// broadcast route) does not stop the others.
Promise<void> asked(std::shared_ptr<Seeking> seeking, std::vector<Address> to, Bytes question) {
    for (const Address& one : to) {
        try {
            co_await seeking->post.send(one, question, Patience{1000});
        } catch (const Cancelled&) {
            throw;
        } catch (...) {
        }
    }
}

}

// Seeking: a question to the broadcast address (or to the given addresses: the same computer,
// tests), then answers collected until the patience runs out. The same answer from the same sender
// counts once. No answer is an empty list, not a refusal: "nobody is here" is a result.
Promise<std::vector<Sighting>> Beacon::sought(std::uint16_t port, Bytes tag, Patience patience,
                                             std::vector<Address> to) {
    // Refuse with a promise, not with an exception from the call, also when the socket cannot be
    // opened.
    const auto refused = [](std::exception_ptr why) { return failed<std::vector<Sighting>>(std::move(why)); };

    if (tag.empty() || tag.size() > widest_tag)
        return refused(std::make_exception_ptr(Trouble{"A beacon tag takes 1 to 64 bytes."}));

    auto seeking = std::make_shared<Seeking>();

    try {
        seeking->post = Udp{Address::any(Family::four, 0)};
    } catch (...) {
        return refused(std::current_exception());
    }

    seeking->post.broadcast(true);
    seeking->once = talcite::random(nonce);
    seeking->tag = std::move(tag);
    seeking->until = std::chrono::steady_clock::now() + patience;

    if (to.empty()) to.push_back(Address{"255.255.255.255", port});

    Bytes question = written(asking, seeking->once, seeking->tag, nullptr);

    return asked(seeking, std::move(to), std::move(question))
        .then([seeking] { return gathered(seeking).outcome(); })
        .then([seeking](const Outcome<void>&) {
            seeking->post.close();

            return std::move(seeking->found);
        });
}

}

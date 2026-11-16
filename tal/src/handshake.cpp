#include "tal/peer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "tal/trouble.hpp"

// A key agreement on a long key, ported from talcite's handshake.cpp. `Handshake` is the port of
// every agreement: a state machine whose driver asks `said()` for the next message, gives what came
// to `heard()` and watches `ready()`; `Sealed` frames the messages and knows no method.
//
// Both ends know the same long key of full entropy (32 random bytes, a code to paste, not to read
// out) and add ephemeral X25519, so every conversation has fresh keys, and the long key found out
// later opens none from before. Noise NNpsk0, made three messages with explicit confirmation:
//
//     caller → e_c
//     answerer → e_r, the answerer's confirmation
//     caller → the caller's confirmation
//
// The keys come from HKDF-SHA-512: the long key as the salt, the X25519 result as the secret, and
// the label of the method, the context and both messages as the info. A confirmation is
// HMAC-SHA-256 of the side's name with that side's key; a wrong key or context fails it before any
// content flows. The confirmation lets a guessed key be checked offline, so a short code to read
// out goes through CPace instead.
namespace tal::net {

namespace {

constexpr std::size_t wide = 32;

// Appends `what` to `into`.
void appended(Bytes& into, const Bytes& what) { into.insert(into.end(), what.begin(), what.end()); }

// Appends `what` to `into` after its length in four bytes, so that two ways of cutting one text
// give HKDF different input.
void framed(Bytes& into, const Bytes& what) {
    const auto much = static_cast<std::uint32_t>(what.size());

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((much >> shift) & 0xff));

    appended(into, what);
}

// The refusal of every failure alike, so that the other side learns nothing of what went wrong.
[[noreturn]] void refused() { throw Trouble{Kind::denied, "The other side does not share the key."}; }

// `much` bytes of `from` at `at`; a message too short is the refusal.
Bytes slice(const Bytes& from, std::size_t at, std::size_t much) {
    if (at + much > from.size()) refused();

    return Bytes(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + much));
}

// The handshake on a long key described above.
class Keyed final : public Handshake {
public:
    // The side `side` of the long key `key` (32 bytes at least) in `context`, with its ephemeral
    // key pair from `cipher`.
    Keyed(Side side, Bytes key, std::string context, Port<crypto::Cipher> cipher) : side_(side), key_(std::move(key)), context_(std::move(context)) {
        if (key_.size() < wide) {
            crypto::wiped(key_);

            throw Trouble{Kind::other, "A long key has 32 bytes at least."};
        }

        mine_ = cipher->agreement();
    }

    // Wipes the long key and the confirmations.
    ~Keyed() override {
        crypto::wiped(key_);
        crypto::wiped(confirm_calling_);
        crypto::wiped(confirm_answering_);
    }

    Keyed(const Keyed&) = delete;
    Keyed& operator=(const Keyed&) = delete;

    // The next message of this side, or nothing in the other side's turn and at the end; the caller
    // is ready once its confirmation is out.
    std::optional<Bytes> said() override {
        if (side_ == Side::calling && step_ == 0) {
            step_ = 1;
            calling_ = mine_->mine();

            return calling_;
        }

        if (side_ == Side::answering && step_ == 1) {
            step_ = 2;

            Bytes made = answering_;

            appended(made, confirm_answering_);

            return made;
        }

        if (side_ == Side::calling && step_ == 2) {
            step_ = 3;
            ready_ = true;

            return confirm_calling_;
        }

        return std::nullopt;
    }

    // The other side's message `one`: the keys from both ephemeral halves, and the confirmation
    // checked. Anything out of turn, malformed or unconfirmed is the refusal.
    void heard(const Bytes& one) override {
        if (side_ == Side::answering && step_ == 0) {
            if (one.size() != wide) refused();

            calling_ = one;
            answering_ = mine_->mine();
            derived_from(one);
            step_ = 1;

            return;
        }

        if (side_ == Side::calling && step_ == 1) {
            if (one.size() != 2 * wide) refused();

            answering_ = slice(one, 0, wide);
            derived_from(answering_);

            if (!crypto::same(slice(one, wide, wide), confirm_answering_)) refused();

            step_ = 2;

            return;
        }

        if (side_ == Side::answering && step_ == 2) {
            if (!crypto::same(one, confirm_calling_)) refused();

            step_ = 3;
            ready_ = true;

            return;
        }

        refused();
    }

    // Whether the handshake is over and the keys may be read.
    [[nodiscard]] bool ready() const override { return ready_; }

    // The keys of sending and of receiving; throws `Trouble` (other) before the end.
    [[nodiscard]] const Keying& keys() const override {
        if (!ready_) throw Trouble{Kind::other, "The handshake is not over."};

        return keys_;
    }

private:
    // The keys and confirmations from the other side's ephemeral half `theirs`.
    void derived_from(const Bytes& theirs) {
        std::optional<Bytes> shared = mine_->shared(theirs);

        if (!shared) refused();

        Bytes info = bytes("talcite keyed 1");

        framed(info, bytes(context_));
        framed(info, calling_);
        framed(info, answering_);

        Bytes made = crypto::derived(crypto::Sum::sha512, key_, *shared, info, 4 * wide);
        const Bytes calling = slice(made, 0, wide);
        const Bytes answering = slice(made, wide, wide);
        Bytes confirming_answering = slice(made, 2 * wide, wide);
        Bytes confirming_calling = slice(made, 3 * wide, wide);

        keys_ = side_ == Side::calling ? Keying{calling, answering} : Keying{answering, calling};
        confirm_answering_ = crypto::stamped(crypto::Sum::sha256, confirming_answering, bytes("answering"));
        confirm_calling_ = crypto::stamped(crypto::Sum::sha256, confirming_calling, bytes("calling"));

        crypto::wiped(*shared);
        crypto::wiped(made);
        crypto::wiped(confirming_answering);
        crypto::wiped(confirming_calling);
    }

    Side side_;
    Bytes key_;
    std::string context_;
    std::unique_ptr<crypto::Agreement> mine_;
    Bytes calling_;
    Bytes answering_;
    Bytes confirm_calling_;
    Bytes confirm_answering_;
    Keying keys_;
    int step_ = 0;
    bool ready_ = false;
};

}

// The handshake on the long key `key` for `side` in `context`, its X25519 from `cipher`.
std::shared_ptr<Handshake> keyed(Side side, Bytes key, std::string context, Port<crypto::Cipher> cipher) {
    return std::make_shared<Keyed>(side, std::move(key), std::move(context), std::move(cipher));
}

}

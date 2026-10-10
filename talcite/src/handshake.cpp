#include "talcite/handshake.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/digest.hpp"
#include "talcite/trouble.hpp"

namespace talcite::net {

// Key agreement as a seam: an exchange of messages that ends with two keys, one for sending and one
// for receiving. The encrypted channel (`Channel`) does not know which method produced them, so new
// methods can be added without touching the channel; talcite is meant to be general, and CPace is
// one of the options. The interface is a state machine, like `TlsSession`: whoever drives the
// conversation asks `said()` for the next message, passes what arrived to `heard()`, and watches
// `ready()`. Framing and sockets belong to the channel, so a handshake can be tested in memory,
// without a network.
//
// The long-key method (`keyed`): both ends know the same full-entropy key (32 random bytes, a code
// to paste, not to dictate) and add ephemeral X25519 to it, so the session keys are fresh every
// time, and revealing the long key later does not expose conversations from before. The pattern is
// Noise NNpsk0, simplified to three messages with explicit confirmation:
//
//     caller → e_c
//     answerer → e_r, the answerer's confirmation
//     caller → the caller's confirmation
//
// The keys come from HKDF-SHA-512: the salt is the long key, the secret is the X25519 result, and
// the info is the method label, the context (for example the protocol name and the invitation
// label) and both messages. A confirmation is HMAC-SHA-256 over the side's label, keyed with that
// side's confirmation key. With a wrong key or a different context the confirmation does not match
// and the handshake refuses before a single byte of content flows.
//
// The key must have full entropy: the confirmation allows guessed keys to be checked offline, so a
// short code to dictate goes through PAKE (CPace), not this method.
namespace {

// The size of an X25519 public value, of each session key and of each confirmation, in bytes.
constexpr std::size_t wide = 32;

// The bytes of a text, one per char, for labels and the context.
Bytes bytes_of(std::string_view said) {
    Bytes made;

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    return made;
}

// Appends `what` to the end of `into`.
void appended(Bytes& into, const Bytes& what) { into.insert(into.end(), what.begin(), what.end()); }

// Appends a text preceded by its 32-bit big-endian length, so that two different splits of the same
// concatenation give different HKDF inputs.
void framed(Bytes& into, const Bytes& what) {
    const auto much = static_cast<std::uint32_t>(what.size());

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((much >> shift) & 0xff));

    appended(into, what);
}

// A piece of a message; a message that is too short is a refusal, not a read past its end.
Bytes slice(const Bytes& from, std::size_t at, std::size_t much) {
    if (at + much > from.size()) throw Trouble{"The other side does not share the key."};

    return Bytes(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + much));
}

// Constant-time comparison: a confirmation is a secret whose bytes must not be guessable from
// timing. The length of the first message is checked by `Agreement::shared`, where a wrong size is
// a refusal.
bool alike(const Bytes& one, const Bytes& two) {
    if (one.size() != two.size()) return false;

    std::byte differ{0};

    for (std::size_t at = 0; at < two.size(); ++at) differ |= one[at] ^ two[at];

    return differ == std::byte{0};
}

// The long-key handshake described at the top of this file.
class Keyed : public Handshake {
public:
    // Takes the side, the long key (at least 32 bytes, otherwise it throws), the context and the
    // cipher provider, and creates the ephemeral X25519 key pair.
    Keyed(Side side, Bytes key, std::string context, Seam<CipherProvider> cipher)
        : side_(side), key_(std::move(key)), context_(std::move(context)), cipher_(std::move(cipher)) {
        if (key_.size() < wide) throw Trouble{"A long key has at least 32 bytes."};

        mine_ = cipher_.provider().agreement();
    }

    // Overwrites the long key before the memory is released.
    ~Keyed() override { wiped(key_); }

    Keyed(const Keyed&) = delete;
    Keyed& operator=(const Keyed&) = delete;
    Keyed(Keyed&&) = delete;
    Keyed& operator=(Keyed&&) = delete;

    // The next message this side has to send, or nothing when it is the other side's turn or the
    // handshake is over. The caller becomes ready once its confirmation is out.
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

    // Takes a message from the other side: derives the keys from the ephemeral values and checks
    // the confirmation. Anything out of order, malformed or not confirmed throws "The other side
    // does not share the key.".
    void heard(const Bytes& one) override {
        if (side_ == Side::answering && step_ == 0) {
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

            if (!alike(slice(one, wide, wide), confirm_answering_)) refused();

            step_ = 2;

            return;
        }

        if (side_ == Side::answering && step_ == 2) {
            if (!alike(one, confirm_calling_)) refused();

            step_ = 3;
            ready_ = true;

            return;
        }

        refused();
    }

    // Whether the handshake has finished and `keys()` may be read.
    [[nodiscard]] bool ready() const override { return ready_; }

    // The sending and receiving keys; throws until the handshake has finished.
    [[nodiscard]] const Keying& keys() const override {
        if (!ready_) throw Trouble{"The handshake is not finished."};

        return keys_;
    }

private:
    // Throws the single refusal shown for every failure, so that the other side learns nothing
    // about what went wrong.
    [[noreturn]] static void refused() { throw Trouble{"The other side does not share the key."}; }

    // Derives the keys from both e messages: the X25519 result as the secret, the long key as the
    // salt, and the label, the context and the transcript as the info.
    void derived_from(const Bytes& theirs) {
        const std::optional<Bytes> shared = mine_->shared(theirs);

        if (!shared) refused();

        Bytes info = bytes_of("talcite keyed 1");

        framed(info, bytes_of(context_));
        framed(info, calling_);
        framed(info, answering_);

        const Bytes made = derived(Sum::sha512, key_, *shared, info, 4 * wide);

        const Bytes calling = slice(made, 0, wide);
        const Bytes answering = slice(made, wide, wide);

        keys_ = side_ == Side::calling ? Keying{calling, answering} : Keying{answering, calling};

        confirm_answering_ = stamped(Sum::sha256, slice(made, 2 * wide, wide), bytes_of("answering"));
        confirm_calling_ = stamped(Sum::sha256, slice(made, 3 * wide, wide), bytes_of("calling"));
    }

    // Zeroes the bytes through a volatile pointer, so that the compiler cannot drop the writes.
    static void wiped(Bytes& one) {
        volatile std::byte* at = one.data();

        for (std::size_t i = 0; i < one.size(); ++i) at[i] = std::byte{0};
    }

    Side side_;
    Bytes key_;
    std::string context_;
    Seam<CipherProvider> cipher_;
    std::unique_ptr<Agreement> mine_;
    Bytes calling_;
    Bytes answering_;
    Bytes confirm_calling_;
    Bytes confirm_answering_;
    Keying keys_;
    int step_ = 0;
    bool ready_ = false;
};

}

// A handshake by the long-key method for the given side.
std::shared_ptr<Handshake> keyed(Side side, Bytes key, std::string context, Seam<CipherProvider> cipher) {
    return std::make_shared<Keyed>(side, std::move(key), std::move(context), std::move(cipher));
}

}

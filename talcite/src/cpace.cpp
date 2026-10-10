#include "cpacing.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/digest.hpp"
#include "talcite/random.hpp"
#include "talcite/trouble.hpp"

#include "curving.hpp"

// CPace (draft-irtf-cfrg-cpace-21, the X25519 + SHA-512 suite) is a key agreement on a short shared
// password: a code to be read out. The password cannot be checked offline: whoever overheard the
// conversation cannot guess the code from it. Guessing is possible only inside a conversation, one
// attempt per conversation (twa counts the failed ones and suspends the label).
//
// X25519 comes from the system (the cipher seam), and SHA-512, HMAC and HKDF from the talcite
// digests. Only what systems do not provide is written here: the Elligator2 mapping of a hash to a
// curve point (RFC 9380), on constant-time arithmetic in the field 2²⁵⁵ − 19 (`curving`, shared
// with the fallback X25519). The vectors from appendix B.1 of the draft check the result.
//
// The conversation: the caller sends Ya, the answerer sends Yb and its confirmation tag, and the
// caller sends its own tag. That is the explicit confirmation from §10.4, so a wrong password is a
// refusal before a single byte of content flows. A shared point of zero (a low-order point, also
// one written non-canonically) aborts the handshake (§9). CI is the context from the caller (in
// twa: the conversation version and the invitation label); sid, ADa and ADb are empty. The channel
// keys come from HKDF-SHA-512 over ISK, with the label of this path and the context.
namespace talcite::net::cpacing {

namespace {

constexpr std::size_t wide = 32;

constexpr std::size_t block = 128;

// The bytes of a string, one by one.
Bytes bytes_of(std::string_view said) {
    Bytes made;

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    return made;
}

// Appends `what` to the end of `into`.
void appended(Bytes& into, const Bytes& what) { into.insert(into.end(), what.begin(), what.end()); }

// Zeroing that the compiler does not remove, for secrets and what is derived from them.
void wiped(Bytes& one) {
    volatile std::byte* at = one.data();

    for (std::size_t i = 0; i < one.size(); ++i) at[i] = std::byte{0};
}

// Equality in constant time: the time does not depend on where the bytes differ.
bool alike(const Bytes& one, const Bytes& two) {
    if (one.size() != two.size()) return false;

    std::byte differ{0};

    for (std::size_t at = 0; at < two.size(); ++at) differ |= one[at] ^ two[at];

    return differ == std::byte{0};
}

// Appends `what` preceded by its length as four big-endian bytes.
void framed(Bytes& into, const Bytes& what) {
    const auto much = static_cast<std::uint32_t>(what.size());

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((much >> shift) & 0xff));

    appended(into, what);
}

class Cpace : public Handshake {
public:
    // Computes this side's share from the password and the context right away and wipes the
    // password. Throws `Trouble` when the given scalar is not 32 bytes long or the share is zero.
    Cpace(Side side, Bytes prs, std::string context, Seam<CipherProvider> cipher, std::optional<Bytes> scalar)
        : side_(side), context_(std::move(context)), cipher_(std::move(cipher)),
          scalar_(scalar ? std::move(*scalar) : random(wide)) {
        if (scalar_.size() != wide) throw Trouble{"A CPace scalar has 32 bytes."};

        const Bytes g = generator(prs, bytes_of(context_), {});

        wiped(prs);

        const std::optional<Bytes> share = multiplied(cipher_, scalar_, g);

        if (!share) refused();

        mine_ = *share;
    }

    // Wipes the scalar and the confirmation key.
    ~Cpace() override {
        wiped(scalar_);
        wiped(mac_key_);
    }

    Cpace(const Cpace&) = delete;
    Cpace& operator=(const Cpace&) = delete;
    Cpace(Cpace&&) = delete;
    Cpace& operator=(Cpace&&) = delete;

    // The next message of this side, or nothing when it is the other side's turn: Ya from the
    // caller, then Yb with the answerer's tag, then the caller's tag.
    std::optional<Bytes> said() override {
        if (side_ == Side::calling && step_ == 0) {
            step_ = 1;

            return mine_;
        }

        if (side_ == Side::answering && step_ == 1) {
            step_ = 2;

            Bytes made = mine_;

            appended(made, tag(mine_));

            return made;
        }

        if (side_ == Side::calling && step_ == 2) {
            step_ = 3;
            ready_ = true;

            return tag(mine_);
        }

        return std::nullopt;
    }

    // Takes the other side's message for the current step. A message of the wrong size, a tag
    // that does not match or a message out of turn throws `Trouble`, as a wrong password does.
    void heard(const Bytes& one) override {
        if (side_ == Side::answering && step_ == 0) {
            if (one.size() != wide) refused();

            theirs_ = one;

            derived(transcript(theirs_, {}, mine_, {}));

            step_ = 1;

            return;
        }

        if (side_ == Side::calling && step_ == 1) {
            if (one.size() != wide + 64) refused();

            theirs_ = Bytes(one.begin(), one.begin() + wide);

            derived(transcript(mine_, {}, theirs_, {}));

            if (!alike(Bytes(one.begin() + wide, one.end()), tag(theirs_))) refused();

            step_ = 2;

            return;
        }

        if (side_ == Side::answering && step_ == 2) {
            if (!alike(one, tag(theirs_))) refused();

            step_ = 3;
            ready_ = true;

            return;
        }

        refused();
    }

    // True once both confirmation tags have been checked.
    [[nodiscard]] bool ready() const override { return ready_; }

    // The channel keys; throws `Trouble` before the handshake is finished.
    [[nodiscard]] const Keying& keys() const override {
        if (!ready_) throw Trouble{"The handshake is not finished."};

        return keys_;
    }

private:
    // Throws the refusal; every failure of the handshake looks like a wrong password.
    [[noreturn]] static void refused() { throw Trouble{"The other side does not share the password."}; }

    // K, ISK, the confirmation key and the channel keys. K never leaves this function (§10.3).
    void derived(const Bytes& told) {
        std::optional<Bytes> k = multiplied(cipher_, scalar_, theirs_);

        if (!k) refused();

        Bytes isk = intermediate({}, *k, told);

        wiped(*k);

        Bytes mac_input = bytes_of("CPaceMac");

        appended(mac_input, isk);

        mac_key_ = summed(Sum::sha512, mac_input);

        wiped(mac_input);

        Bytes info = bytes_of("talcite cpace 1");

        framed(info, bytes_of(context_));

        const Bytes made = talcite::derived(Sum::sha512, {}, isk, info, 2 * wide);

        wiped(isk);

        const Bytes calling(made.begin(), made.begin() + wide);
        const Bytes answering(made.begin() + wide, made.end());

        keys_ = side_ == Side::calling ? Keying{calling, answering} : Keying{answering, calling};
    }

    // The confirmation tag (§10.4): HMAC over the side's message, with the key derived from ISK.
    [[nodiscard]] Bytes tag(const Bytes& y) const {
        Bytes message = prepended(y);

        appended(message, prepended({}));

        return stamped(Sum::sha512, mac_key_, message);
    }

    Side side_;
    std::string context_;
    Seam<CipherProvider> cipher_;
    Bytes scalar_;
    Bytes mine_;
    Bytes theirs_;
    Bytes mac_key_;
    Keying keys_;
    int step_ = 0;
    bool ready_ = false;
};

}

// The length before the data, in LEB128 (§A.1).
Bytes prepended(const Bytes& data) {
    Bytes made;

    std::size_t length = data.size();

    do {
        const auto low = static_cast<std::uint8_t>(length & 0x7f);

        length >>= 7;

        made.push_back(static_cast<std::byte>(length != 0 ? low | 0x80 : low));
    } while (length != 0);

    appended(made, data);

    return made;
}

// DSI, the password, zeros up to a full first hash block, CI and sid, each with its length (§8.1).
Bytes generator_string(const Bytes& dsi, const Bytes& prs, const Bytes& ci, const Bytes& sid, std::size_t block_size) {
    const std::size_t taken = prepended(prs).size() + prepended(dsi).size() + 1;
    const std::size_t padding = block_size > taken ? block_size - taken : 0;

    Bytes made = prepended(dsi);

    appended(made, prepended(prs));
    appended(made, prepended(Bytes(padding)));
    appended(made, prepended(ci));
    appended(made, prepended(sid));

    return made;
}

// Elligator2 for Curve25519 (RFC 9380, Z = 2): u → v = −A / (1 + Z·u²), ε = χ(v³ + A·v² + v),
// x = ε·v − (1 − ε)·A/2. The inverse of zero is zero, so that case too goes through the formula,
// without a branch.
Bytes elligator2(const Bytes& u) {
    using namespace curving;

    if (u.size() != wide) throw Trouble{"Elligator2 takes a 32-byte coordinate."};

    const Limbs a = small(486662);
    const Limbs one = small(1);
    const Limbs r = unpacked(u);

    const Limbs t = sum(one, product(small(2), product(r, r)));
    const Limbs v = difference(small(0), product(a, inverse(t)));

    const Limbs v2 = product(v, v);
    const Limbs curve = sum(sum(product(v2, v), product(a, v2)), v);

    const Limbs epsilon = powered(curve, halving);

    const Limbs half_a = product(a, inverse(small(2)));
    const Limbs x = difference(product(epsilon, v), product(difference(one, epsilon), half_a));

    return packed(x);
}

// The generator from the password, CI and sid: the SHA-512 hash of the generator string, with the
// first 32 bytes taken as u (§8.2).
Bytes generator(const Bytes& prs, const Bytes& ci, const Bytes& sid) {
    Bytes text = generator_string(bytes_of("CPace255"), prs, ci, sid, block);

    Bytes hash = summed(Sum::sha512, text);

    wiped(text);

    hash.resize(wide);

    const Bytes made = elligator2(hash);

    wiped(hash);

    return made;
}

// X25519(scalar, point) through the cipher seam; nothing when the result is the zero point
// (§8.2, §9).
std::optional<Bytes> multiplied(const Seam<CipherProvider>& cipher, const Bytes& scalar, const Bytes& point) {
    const std::unique_ptr<Agreement> mine = cipher.provider().agreement(scalar);

    return mine->shared(point);
}

// The transcript of the two messages and their associated data, each with its length.
Bytes transcript(const Bytes& ya, const Bytes& ada, const Bytes& yb, const Bytes& adb) {
    Bytes made = prepended(ya);

    appended(made, prepended(ada));
    appended(made, prepended(yb));
    appended(made, prepended(adb));

    return made;
}

// ISK = H(lv_cat(DSI ‖ "_ISK", sid, K) ‖ transcript) (§7.2).
Bytes intermediate(const Bytes& sid, const Bytes& k, const Bytes& told) {
    Bytes input = prepended(bytes_of("CPace255_ISK"));

    appended(input, prepended(sid));
    appended(input, prepended(k));
    appended(input, told);

    const Bytes made = summed(Sum::sha512, input);

    wiped(input);

    return made;
}

// A CPace handshake for one side; the scalar is given only by tests, otherwise it is random.
std::shared_ptr<Handshake> made(Side side, Bytes prs, std::string context, Seam<CipherProvider> cipher,
                                std::optional<Bytes> scalar) {
    return std::make_shared<Cpace>(side, std::move(prs), std::move(context), std::move(cipher), std::move(scalar));
}

}

namespace talcite::net {

// A CPace handshake for one side, on the given password and context, with a random scalar.
std::shared_ptr<Handshake> cpace(Side side, Bytes password, std::string context, Seam<CipherProvider> cipher) {
    return cpacing::made(side, std::move(password), std::move(context), std::move(cipher), std::nullopt);
}

}

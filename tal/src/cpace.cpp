#include "cpacing.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "tal/trouble.hpp"

#include "curving.hpp"

// CPace (draft-irtf-cfrg-cpace, the suite of X25519 and SHA-512), ported from talcite's cpace.cpp:
// a key agreement on a short shared password, a code to read out. Whoever overheard the
// conversation cannot check guesses of the code against it; a guess costs a conversation (twa
// counts the failed ones and suspends the label).
//
// X25519 comes from the cipher's port and SHA-512 from the system's digests. The one thing no
// system gives, the Elligator2 mapping of a hash to a point of the curve (RFC 9380), computes on
// our own field arithmetic (`curving`), and only when the programmer built it in (TAL_OWN_CURVE);
// without it `cpace()` is unsupported. The vectors of the draft's appendix B.1 check the result.
//
// The conversation: the caller sends Ya, the answerer Yb with its confirmation, and the caller its
// own; the explicit confirmation of §10.4, so a wrong password is refused before any content flows.
// A shared point of zero (a point of low order, also written out of the canonical form) ends the
// handshake (§9). CI is the caller's context; sid, ADa and ADb are empty. The keys of the channel
// come from HKDF-SHA-512 over ISK with the label of this way and the context.
namespace tal::net::cpacing {

namespace {

constexpr std::size_t wide = 32;

// Appends `what` to `into`.
void appended(Bytes& into, const Bytes& what) { into.insert(into.end(), what.begin(), what.end()); }

// Appends `what` to `into` after its length in four bytes.
void framed(Bytes& into, const Bytes& what) {
    const auto much = static_cast<std::uint32_t>(what.size());

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((much >> shift) & 0xff));

    appended(into, what);
}

}

// `data` after its length in LEB128 (§A.1).
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

// The generator string (§8.1): `dsi`, the password `prs`, zeros up to a whole first block of
// `block` bytes, `ci` and `sid`, each after its length.
Bytes generator_string(const Bytes& dsi, const Bytes& prs, const Bytes& ci, const Bytes& sid, std::size_t block) {
    const std::size_t taken = prepended(prs).size() + prepended(dsi).size() + 1;
    const std::size_t padding = block > taken ? block - taken : 0;
    Bytes made = prepended(dsi);

    appended(made, prepended(prs));
    appended(made, prepended(Bytes(padding)));
    appended(made, prepended(ci));
    appended(made, prepended(sid));

    return made;
}

// The transcript of the messages `ya` and `yb` with their data `ada` and `adb`, each after its
// length.
Bytes transcript(const Bytes& ya, const Bytes& ada, const Bytes& yb, const Bytes& adb) {
    Bytes made = prepended(ya);

    appended(made, prepended(ada));
    appended(made, prepended(yb));
    appended(made, prepended(adb));

    return made;
}

// ISK = H(lv_cat(DSI ‖ "_ISK", `sid`, `k`) ‖ `told`) (§7.2).
Bytes intermediate(const Bytes& sid, const Bytes& k, const Bytes& told) {
    Bytes input = prepended(bytes("CPace255_ISK"));

    appended(input, prepended(sid));
    appended(input, prepended(k));
    appended(input, told);

    Bytes made = crypto::summed(crypto::Sum::sha512, input);

    crypto::wiped(input);

    return made;
}

// X25519 of `scalar` and `point` through `cipher`; nothing for the point of zero (§8.2, §9).
std::optional<Bytes> multiplied(crypto::Cipher& cipher, const Bytes& scalar, const Bytes& point) { return cipher.agreement(scalar)->shared(point); }

#ifdef TAL_OWN_CURVE

namespace {

// The refusal of every failure alike: each looks like a wrong password.
[[noreturn]] void refused() { throw Trouble{Kind::denied, "The other side does not share the password."}; }

// One side of CPace.
class Cpace final : public Handshake {
public:
    // The side `side` of the password `prs` in `context`, its share computed at once and the
    // password wiped; `scalar` is given by the trials alone, otherwise it is random.
    Cpace(Side side, Bytes prs, std::string context, Port<crypto::Cipher> cipher, std::optional<Bytes> scalar)
        : side_(side), context_(std::move(context)), cipher_(std::move(cipher)), scalar_(scalar ? std::move(*scalar) : crypto::random(wide)) {
        Bytes g = generator(prs, bytes(context_), {});

        crypto::wiped(prs);

        if (scalar_.size() != wide) {
            crypto::wiped(g);

            throw Trouble{Kind::other, "A CPace scalar has 32 bytes."};
        }

        const std::optional<Bytes> share = multiplied(*cipher_, scalar_, g);

        crypto::wiped(g);

        if (!share) refused();

        mine_ = *share;
    }

    // Wipes the scalar and the key of the confirmations.
    ~Cpace() override {
        crypto::wiped(scalar_);
        crypto::wiped(mac_key_);
    }

    Cpace(const Cpace&) = delete;
    Cpace& operator=(const Cpace&) = delete;

    // The next message of this side: Ya from the caller, Yb with the answerer's confirmation, then
    // the caller's confirmation; nothing in the other side's turn.
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

    // The other side's message `one` of this step; one of the wrong size, a confirmation that does
    // not match and anything out of turn are the refusal, as a wrong password is.
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

            if (!crypto::same(Bytes(one.begin() + wide, one.end()), tag(theirs_))) refused();

            step_ = 2;

            return;
        }

        if (side_ == Side::answering && step_ == 2) {
            if (!crypto::same(one, tag(theirs_))) refused();

            step_ = 3;
            ready_ = true;

            return;
        }

        refused();
    }

    // Whether both confirmations were checked.
    [[nodiscard]] bool ready() const override { return ready_; }

    // The keys of the channel; throws `Trouble` (other) before the end.
    [[nodiscard]] const Keying& keys() const override {
        if (!ready_) throw Trouble{Kind::other, "The handshake is not over."};

        return keys_;
    }

private:
    // K, ISK, the key of the confirmations and the keys of the channel from the transcript `told`;
    // K never leaves here (§10.3).
    void derived(const Bytes& told) {
        std::optional<Bytes> k = multiplied(*cipher_, scalar_, theirs_);

        if (!k) refused();

        Bytes isk = intermediate({}, *k, told);
        Bytes mac_input = bytes("CPaceMac");

        crypto::wiped(*k);
        appended(mac_input, isk);
        mac_key_ = crypto::summed(crypto::Sum::sha512, mac_input);
        crypto::wiped(mac_input);

        Bytes info = bytes("talcite cpace 1");

        framed(info, bytes(context_));

        Bytes made = crypto::derived(crypto::Sum::sha512, {}, isk, info, 2 * wide);
        const Bytes calling(made.begin(), made.begin() + wide);
        const Bytes answering(made.begin() + wide, made.end());

        crypto::wiped(isk);
        crypto::wiped(made);
        keys_ = side_ == Side::calling ? Keying{calling, answering} : Keying{answering, calling};
    }

    // The confirmation of the message `y` (§10.4): HMAC with the key from ISK.
    [[nodiscard]] Bytes tag(const Bytes& y) const {
        Bytes message = prepended(y);

        appended(message, prepended({}));

        return crypto::stamped(crypto::Sum::sha512, mac_key_, message);
    }

    Side side_;
    std::string context_;
    Port<crypto::Cipher> cipher_;
    Bytes scalar_;
    Bytes mine_;
    Bytes theirs_;
    Bytes mac_key_;
    Keying keys_;
    int step_ = 0;
    bool ready_ = false;
};

}

// Elligator2 for Curve25519 (RFC 9380, Z = 2) of the coordinate `u`: v = −A / (1 + Z·u²),
// ε = χ(v³ + A·v² + v), x = ε·v − (1 − ε)·A/2. The inverse of zero is zero, so that case too goes
// through the formula, without a branch.
Bytes elligator2(const Bytes& u) {
    using namespace crypto::curving;

    if (u.size() != wide) throw Trouble{Kind::other, "Elligator2 takes a coordinate of 32 bytes."};

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

// The generator of the password `prs`, `ci` and `sid`: the first 32 bytes of the SHA-512 of the
// generator string, mapped by Elligator2 (§8.2).
Bytes generator(const Bytes& prs, const Bytes& ci, const Bytes& sid) {
    Bytes text = generator_string(bytes("CPace255"), prs, ci, sid, 128);
    Bytes hash = crypto::summed(crypto::Sum::sha512, text);

    crypto::wiped(text);
    hash.resize(wide);

    Bytes made = elligator2(hash);

    crypto::wiped(hash);

    return made;
}

// The CPace of `side`; `scalar` is given by the trials alone.
std::shared_ptr<Handshake> made(Side side, Bytes prs, std::string context, Port<crypto::Cipher> cipher, std::optional<Bytes> scalar) {
    return std::make_shared<Cpace>(side, std::move(prs), std::move(context), std::move(cipher), std::move(scalar));
}

#endif

}

namespace tal::net {

// CPace for `side` on `password` in `context`, with a random scalar; unsupported unless our own
// curve arithmetic is built in (TAL_OWN_CURVE).
std::shared_ptr<Handshake> cpace(Side side, Bytes password, std::string context, Port<crypto::Cipher> cipher) {
#ifdef TAL_OWN_CURVE
    return cpacing::made(side, std::move(password), std::move(context), std::move(cipher), std::nullopt);
#else
    (void)side;
    (void)context;
    (void)cipher;

    crypto::wiped(password);

    throw Trouble{Kind::unsupported, "CPace needs our own arithmetic of the curve (TAL_OWN_CURVE)."};
#endif
}

}

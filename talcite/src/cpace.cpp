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

// **CPace** (draft-irtf-cfrg-cpace-21, zestaw X25519 + SHA-512) — droga uzgodnienia klucza na
// krótkim, wspólnym haśle: kod do podyktowania. Hasła nie da się sprawdzać poza siecią — kto
// podsłuchał rozmowę, nie zgadnie z niej kodu; zgadywać można tylko w rozmowie, po jednej próbie na
// rozmowę (twa liczy nieudane i wstrzymuje etykietę).
//
// Z systemu bierzemy X25519 (szew szyfrów), SHA-512, HMAC i HKDF (skróty talcite). Sami piszemy tylko
// to, czego systemy nie dają: odwzorowanie Elligator2 skrótu na punkt krzywej (RFC 9380), na
// arytmetyce ciała 2²⁵⁵ − 19 w czasie stałym (`curving`, wspólnej z zapasowym X25519). Wynik
// sprawdzają wektory z dodatku B.1 dokumentu.
//
// Rozmowa: dzwoniący wysyła Ya, odbierający Yb i swój znacznik potwierdzenia, dzwoniący swój znacznik
// — jawne potwierdzenie z §10.4, więc złe hasło to odmowa, zanim popłynie choć bajt treści. Wspólny
// punkt zerowy (punkt małego rzędu, także zapisany niekanonicznie) to przerwanie (§9). CI to kontekst
// od wołającego (u twa: wersja rozmowy i etykieta zaproszenia), sid pusty, ADa i ADb puste. Klucze
// kanału z HKDF-SHA-512 po ISK, z etykietą drogi i kontekstem.
namespace talcite::net::cpacing {

namespace {

constexpr std::size_t wide = 32;

constexpr std::size_t block = 128;

Bytes bytes_of(std::string_view said) {
    Bytes made;

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    return made;
}

void appended(Bytes& into, const Bytes& what) { into.insert(into.end(), what.begin(), what.end()); }

void wiped(Bytes& one) {
    volatile std::byte* at = one.data();

    for (std::size_t i = 0; i < one.size(); ++i) at[i] = std::byte{0};
}

bool alike(const Bytes& one, const Bytes& two) {
    if (one.size() != two.size()) return false;

    std::byte differ{0};

    for (std::size_t at = 0; at < two.size(); ++at) differ |= one[at] ^ two[at];

    return differ == std::byte{0};
}

void framed(Bytes& into, const Bytes& what) {
    const auto much = static_cast<std::uint32_t>(what.size());

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((much >> shift) & 0xff));

    appended(into, what);
}

class Cpace : public Handshake {
public:
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

    ~Cpace() override {
        wiped(scalar_);
        wiped(mac_key_);
    }

    Cpace(const Cpace&) = delete;
    Cpace& operator=(const Cpace&) = delete;
    Cpace(Cpace&&) = delete;
    Cpace& operator=(Cpace&&) = delete;

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

    [[nodiscard]] bool ready() const override { return ready_; }

    [[nodiscard]] const Keying& keys() const override {
        if (!ready_) throw Trouble{"The handshake is not finished."};

        return keys_;
    }

private:
    [[noreturn]] static void refused() { throw Trouble{"The other side does not share the password."}; }

    // K, ISK, klucz potwierdzenia i klucze kanału. K nie wychodzi nigdzie (§10.3).
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

    // Znacznik potwierdzenia (§10.4): HMAC po wiadomości strony, kluczem z ISK.
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

// Długość przed danymi w LEB128 (§A.1).
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

// DSI, hasło, zera do pełnego pierwszego bloku skrótu, CI i sid — każde z długością (§8.1).
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

// Elligator2 dla Curve25519 (RFC 9380, Z = 2): u → v = −A / (1 + Z·u²), ε = χ(v³ + A·v² + v),
// x = ε·v − (1 − ε)·A/2. Odwrotność zera to zero, więc i ten przypadek idzie wzorem, bez gałęzi.
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

// Generator z hasła, CI i sid: skrót SHA-512 ciągu generatora, pierwsze 32 bajty jako u (§8.2).
Bytes generator(const Bytes& prs, const Bytes& ci, const Bytes& sid) {
    Bytes text = generator_string(bytes_of("CPace255"), prs, ci, sid, block);

    Bytes hash = summed(Sum::sha512, text);

    wiped(text);

    hash.resize(wide);

    const Bytes made = elligator2(hash);

    wiped(hash);

    return made;
}

// X25519(scalar, point) przez szew szyfrów — pustka, gdy wynikiem jest punkt zerowy (§8.2, §9).
std::optional<Bytes> multiplied(const Seam<CipherProvider>& cipher, const Bytes& scalar, const Bytes& point) {
    const std::unique_ptr<Agreement> mine = cipher.provider().agreement(scalar);

    return mine->shared(point);
}

Bytes transcript(const Bytes& ya, const Bytes& ada, const Bytes& yb, const Bytes& adb) {
    Bytes made = prepended(ya);

    appended(made, prepended(ada));
    appended(made, prepended(yb));
    appended(made, prepended(adb));

    return made;
}

// ISK = H(lv_cat(DSI ‖ "_ISK", sid, K) ‖ transkrypt) (§7.2).
Bytes intermediate(const Bytes& sid, const Bytes& k, const Bytes& told) {
    Bytes input = prepended(bytes_of("CPace255_ISK"));

    appended(input, prepended(sid));
    appended(input, prepended(k));
    appended(input, told);

    const Bytes made = summed(Sum::sha512, input);

    wiped(input);

    return made;
}

std::shared_ptr<Handshake> made(Side side, Bytes prs, std::string context, Seam<CipherProvider> cipher,
                                std::optional<Bytes> scalar) {
    return std::make_shared<Cpace>(side, std::move(prs), std::move(context), std::move(cipher), std::move(scalar));
}

}

namespace talcite::net {

std::shared_ptr<Handshake> cpace(Side side, Bytes password, std::string context, Seam<CipherProvider> cipher) {
    return cpacing::made(side, std::move(password), std::move(context), std::move(cipher), std::nullopt);
}

}

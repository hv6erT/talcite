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

// **Uzgodnienie klucza jako szew** (S1 w twa): wymiana wiadomości, a na końcu dwa klucze — do
// wysyłania i do odbierania. Kanał szyfrowany (`Channel`) nie wie, która to droga, więc drogi da się
// dokładać bez ruszania kanału — Hubert: talcite ma być uniwersalne, CPace to jedna z opcji.
// Interfejs jest maszyną stanów, jak `TlsSession`: kto prowadzi rozmowę, pyta `said()` o następną
// wiadomość, podaje `heard()` to, co przyszło, i patrzy na `ready()`. Ramkowanie i gniazda należą do
// kanału — uzgodnienie da się więc sprawdzić w pamięci, bez sieci.
//
// **Droga z długim kluczem** (`keyed`): oba końce znają ten sam klucz o pełnej entropii (32 bajty
// losowe — kod do wklejenia, nie do podyktowania) i dokładają do niego efemeryczne X25519, więc
// klucze sesji są świeże za każdym razem, a ujawnienie długiego klucza później nie odsłania rozmów
// sprzed niego. Wzór: Noise NNpsk0, uproszczony do trzech wiadomości z jawnym potwierdzeniem:
//
//     dzwoniący → e_c
//     odbierający → e_r, potwierdzenie odbierającego
//     dzwoniący → potwierdzenie dzwoniącego
//
// Klucze z HKDF-SHA-512: sól — długi klucz, sekret — wynik X25519, treść — etykieta drogi, kontekst
// (np. nazwa protokołu i etykieta zaproszenia) i obie wiadomości. Potwierdzenie to HMAC-SHA-256 po
// etykiecie strony kluczem potwierdzenia tej strony. Klucz zły albo kontekst inny — potwierdzenie się
// nie zgadza i uzgodnienie odmawia, zanim popłynie choć bajt treści.
//
// Klucz **musi** mieć pełną entropię: potwierdzenie pozwala sprawdzać zgadywane klucze poza siecią,
// więc krótki kod do podyktowania idzie drogą PAKE (CPace), nie tą.
namespace {

constexpr std::size_t wide = 32;

Bytes bytes_of(std::string_view said) {
    Bytes made;

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    return made;
}

void appended(Bytes& into, const Bytes& what) { into.insert(into.end(), what.begin(), what.end()); }

// Napis z długością przed nim — dwa różne podziały tej samej sklejki dają różne wejścia HKDF.
void framed(Bytes& into, const Bytes& what) {
    const auto much = static_cast<std::uint32_t>(what.size());

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((much >> shift) & 0xff));

    appended(into, what);
}

// Kawałek wiadomości — za krótka wiadomość to odmowa, nie czytanie za końcem.
Bytes slice(const Bytes& from, std::size_t at, std::size_t much) {
    if (at + much > from.size()) throw Trouble{"The other side does not share the key."};

    return Bytes(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + much));
}

// Porównanie w czasie stałym — potwierdzenie to sekret, którego bajtów nie zgaduje się po czasie.
// Długość pierwszej wiadomości sprawdza `Agreement::shared` — zły rozmiar to tam odmowa.
bool alike(const Bytes& one, const Bytes& two) {
    if (one.size() != two.size()) return false;

    std::byte differ{0};

    for (std::size_t at = 0; at < two.size(); ++at) differ |= one[at] ^ two[at];

    return differ == std::byte{0};
}

class Keyed : public Handshake {
public:
    Keyed(Side side, Bytes key, std::string context, Seam<CipherProvider> cipher)
        : side_(side), key_(std::move(key)), context_(std::move(context)), cipher_(std::move(cipher)) {
        if (key_.size() < wide) throw Trouble{"A long key has at least 32 bytes."};

        mine_ = cipher_.provider().agreement();
    }

    ~Keyed() override { wiped(key_); }

    Keyed(const Keyed&) = delete;
    Keyed& operator=(const Keyed&) = delete;
    Keyed(Keyed&&) = delete;
    Keyed& operator=(Keyed&&) = delete;

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

    [[nodiscard]] bool ready() const override { return ready_; }

    [[nodiscard]] const Keying& keys() const override {
        if (!ready_) throw Trouble{"The handshake is not finished."};

        return keys_;
    }

private:
    [[noreturn]] static void refused() { throw Trouble{"The other side does not share the key."}; }

    // Klucze z obu wiadomości e: wynik X25519, sól — długi klucz, treść — etykieta, kontekst i transkrypt.
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

std::shared_ptr<Handshake> keyed(Side side, Bytes key, std::string context, Seam<CipherProvider> cipher) {
    return std::make_shared<Keyed>(side, std::move(key), std::move(context), std::move(cipher));
}

}

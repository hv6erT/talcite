#include "proving.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

#include "talcite/bytes.hpp"
#include "talcite/digest.hpp"

#include "cpacing.hpp"

using namespace talcite;

// **CPace** — części drogi bez krzywej, sprawdzane wektorami z draft-irtf-cfrg-cpace-21 (dodatek
// B.1, X25519 + SHA-512): ciąg generatora, Elligator2, transkrypt i ISK. Mnożenie na krzywej daje
// system, więc pełną rozmowę z wektorami Ya, Yb i K sprawdza talcite-adapters na prawdziwym X25519.
// Dodatkowe wejścia Elligator2 (zero, u ≥ p, p − 1) policzono niezależnie, w Pythonie, wprost ze
// wzorów RFC 9380.
namespace {

Bytes lump(std::string_view hex) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(static_cast<std::byte>(std::stoi(std::string{hex.substr(at, 2)}, nullptr, 16)));

    return made;
}

std::string spelled(const Bytes& one) {
    static constexpr char digits[] = "0123456789abcdef";

    std::string made;

    for (const std::byte each : one) {
        made += digits[static_cast<unsigned>(each) >> 4];
        made += digits[static_cast<unsigned>(each) & 15];
    }

    return made;
}

Bytes text(std::string_view said) {
    Bytes made;

    for (const char one : said) made.push_back(static_cast<std::byte>(one));

    return made;
}

bool missing() {
    if (summing()) return false;

    std::cout << "        ten system nie daje skrótów — próba pominięta\n";

    return true;
}

const Bytes ci = lump("0b415f696e69746961746f720b425f726573706f6e646572");
const Bytes sid = lump("7e4b4791d6a8ef019b936c79fb7f2c57");
const std::string generator_g = "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f";

}

TRIAL("CPace: długość przed danymi w LEB128") {
    same(spelled(net::cpacing::prepended({})), std::string{"00"});
    same(spelled(net::cpacing::prepended(text("ab"))), std::string{"026162"});

    const Bytes edge = net::cpacing::prepended(Bytes(127));

    same(edge.size(), std::size_t{128});
    same(spelled(Bytes(edge.begin(), edge.begin() + 1)), std::string{"7f"});

    const Bytes past = net::cpacing::prepended(Bytes(128));

    same(past.size(), std::size_t{130});
    same(spelled(Bytes(past.begin(), past.begin() + 2)), std::string{"8001"});

    const Bytes long_one = net::cpacing::prepended(Bytes(300));

    same(spelled(Bytes(long_one.begin(), long_one.begin() + 2)), std::string{"ac02"});
}

// B.1.1: 170 bajtów — DSI, hasło, 109 zer dopełnienia, CI i sid.
TRIAL("CPace: ciąg generatora według wektora B.1.1") {
    same(spelled(net::cpacing::generator_string(text("CPace255"), text("Password"), ci, sid, 128)),
         std::string{"0843506163653235350850617373776f72646d000000000000000000"
                     "00000000000000000000000000000000000000000000000000000000"
                     "00000000000000000000000000000000000000000000000000000000"
                     "00000000000000000000000000000000000000000000000000000000"
                     "00000000000000000000000000000000180b415f696e69746961746f"
                     "720b425f726573706f6e646572107e4b4791d6a8ef019b936c79fb7f"
                     "2c57"});
}

// Długie hasło nie dostaje dopełnienia — pole zer zostaje, puste.
TRIAL("CPace: ciąg generatora bez dopełnienia przy długim haśle") {
    const Bytes made = net::cpacing::generator_string(text("CPace255"), Bytes(200, std::byte{'x'}), {}, {}, 128);

    same(made.size(), std::size_t{9 + 2 + 200 + 1 + 1 + 1});
    same(spelled(Bytes(made.begin() + 211, made.end())), std::string{"000000"});
}

// B.1.1: skrót ciągu z ustawionym bitem 255 i ten sam po jego wyczyszczeniu dają ten sam generator.
TRIAL("CPace: Elligator2 według wektora B.1.1, bit 255 odrzucony") {
    same(spelled(net::cpacing::elligator2(lump("03998087bdb1a2617bbe25ef5a7c18cd4f84f902328701790958755ee4aed1d3"))),
         generator_g);
    same(spelled(net::cpacing::elligator2(lump("03998087bdb1a2617bbe25ef5a7c18cd4f84f902328701790958755ee4aed153"))),
         generator_g);
}

// Brzegi ciała: zero, p (to samo co zero), p − 1 i 2²⁵⁵ − 1 (niekanoniczne, ≡ 18), plus dwa zwykłe.
TRIAL("CPace: Elligator2 na brzegach ciała i zwykłych wejściach") {
    const std::string zero(64, '0');

    same(spelled(net::cpacing::elligator2(Bytes(32))), zero);
    same(spelled(net::cpacing::elligator2(lump("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"))), zero);
    same(spelled(net::cpacing::elligator2(lump("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"))),
         std::string{"9cdb525555555555555555555555555555555555555555555555555555555555"});
    same(spelled(net::cpacing::elligator2(Bytes(32, std::byte{0xff}))),
         std::string{"1e5942dd97c756040d27755f1e5b11349cd47d796c45d07052f7e5b11541c349"});
    same(spelled(net::cpacing::elligator2(lump("b0536fe171551bd06194c986f4f68dab5a2925ea13d3b597a60af5fff91c067f"))),
         std::string{"83297175f3042779d57c3e2044026e9f1ac81828cc0951208972abaf5db6f37e"});
    same(spelled(net::cpacing::elligator2(lump("c2595004278b88f4395b85c62a1234c0333d85a93a13d632b02874c55e5bcb97"))),
         std::string{"fe116d99766cf12de5328932f69c8daf67633f1eea55ded3b1f053865f3da550"});

    // Wejście wyszukane tak, by wynik przed postacią kanoniczną leżał tuż ponad p (ostatnie odjęcie p
    // i jego przeniesienie mają znaczenie).
    same(spelled(net::cpacing::elligator2(lump("94832161f3dd13cbbb8265b8876a8df9806a920e325777bd4d3623b997297129"))),
         std::string{"bd188d56b4dd95fdc8dda4872265b3dafe78a304f84d79e8ad3734435db9ff7f"});

    threw(static_cast<void>(net::cpacing::elligator2(Bytes(31))));
}

// B.1.5: transkrypt dzwoniący–odbierający, 74 bajty.
TRIAL("CPace: transkrypt według wektora B.1.5") {
    const Bytes ya = lump("1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32");
    const Bytes yb = lump("248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623");

    same(spelled(net::cpacing::transcript(ya, text("ADa"), yb, text("ADb"))),
         std::string{"201d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc9"
                     "89ce0c9f320341446120248cccf6d5cdc3646f0ad593f9e6cef4e69d"
                     "4945f8372e623512ecea3218562303414462"});
}

// B.1.1 i B.1.5 przez SHA-512 z systemu: generator wprost z hasła i ISK z K i transkryptu.
TRIAL("CPace: generator i ISK według wektorów B.1.1 i B.1.5") {
    if (missing()) return;

    same(spelled(net::cpacing::generator(text("Password"), ci, sid)), generator_g);

    const Bytes k = lump("5b067effbdc0b2a0e1d907b21ebb25cfedb96a852179a847c37e43ee71322c6b");
    const Bytes told = net::cpacing::transcript(lump("1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32"),
                                           text("ADa"),
                                           lump("248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623"),
                                           text("ADb"));

    same(spelled(net::cpacing::intermediate(sid, k, told)),
         std::string{"6e19b875f7a561d6b3ca3dbb9ef42ac55de3e717881018204b8922b4"
                     "d5e53bb2aa82c300bea7b65d2b671da71922ddf6472301b79bc270ad"
                     "fa8bf413285f2263"});
}

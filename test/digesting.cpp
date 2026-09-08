#include "proving.hpp"

#include <cstddef>
#include <iostream>
#include <string>

#include "talcite/digest.hpp"
#include "talcite/random.hpp"
#include "talcite/trouble.hpp"

#include "stunning.hpp"

using namespace talcite;

// Losowość i skróty bierze się **od systemu**, nie z żadnej biblioteki i nie z własnego kodu.
// Losowość jest wszędzie; skróty potrafi zablokować filtr wywołań systemowych w kontenerze — i tam
// próby na wektorach pomijają się zamiast wywracać, bo brak systemowej możliwości nie jest wadą
// biblioteki. `summing()` mówi wprost, którą sytuację mamy.
namespace {

Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

std::string spelled(const Bytes& what) {
    static const char* signs = "0123456789abcdef";

    std::string made;

    for (const std::byte one : what) {
        const unsigned sign = std::to_integer<unsigned>(one);

        made += signs[sign >> 4];
        made += signs[sign & 0xF];
    }

    return made;
}

bool missing() {
    if (summing()) return false;

    std::cout << "        ten system nie daje skrótów — próba pominięta\n";

    return true;
}

}

TRIAL("Losowość: oddaje tyle bajtów, ile poproszono") {
    same(random(0).size(), static_cast<std::size_t>(0));
    same(random(1).size(), static_cast<std::size_t>(1));
    same(random(12).size(), static_cast<std::size_t>(12));

    // Duża prośba nie może być ucięta: getrandom bez flag oddaje najwyżej 32 MiB naraz i potrafi
    // oddać mniej, niż poproszono, więc pętla dobierająca resztę musi tam być.
    same(random(70000).size(), static_cast<std::size_t>(70000));
}

TRIAL("Losowość: dwa wywołania się różnią") {
    const Bytes one = random(32);
    const Bytes two = random(32);

    ought(!(one == two));
}

// Nie jest to sprawdzian jakości losowości — tej nie da się sprawdzić testem — tylko tego, że coś
// w ogóle miesza. Same zera na trzydziestu dwóch bajtach znaczyłyby, że nikt nic nie wpisał.
TRIAL("Losowość: nie oddaje samych zer") {
    bool something = false;

    for (const std::byte one : random(32))
        if (one != std::byte{0}) something = true;

    ought(something);
}

// Wartość kontrolna CRC-32 znana od zawsze: suma z „123456789" wynosi 0xCBF43926. To jest suma
// kontrolna, nie kryptografia, więc liczymy ją sami i sprawdzamy do końca.
TRIAL("Suma: CRC-32 zgadza się z wartością kontrolną") {
    same(net::tallied(spelt("123456789"), 9), 0xCBF43926u);
    same(net::tallied(Bytes{}, 0), 0u);
}

TRIAL("Skrót: SHA-1 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::sha1, spelt("abc"))),
         std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});

    same(spelled(summed(Sum::sha1, Bytes{})),
         std::string{"da39a3ee5e6b4b0d3255bfef95601890afd80709"});
}

TRIAL("Skrót: MD5 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::md5, spelt("abc"))), std::string{"900150983cd24fb0d6963f7d28e17f72"});

    same(spelled(summed(Sum::md5, Bytes{})), std::string{"d41d8cd98f00b204e9800998ecf8427e"});
}

TRIAL("Skrót: SHA-256 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::sha256, spelt("abc"))),
         std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
}

// RFC 2202, przypadki pierwszy i drugi.
TRIAL("Podpis: HMAC-SHA1 zgadza się z wektorami z RFC 2202") {
    if (missing()) return;

    same(spelled(stamped(Sum::sha1, Bytes(20, std::byte{0x0b}), spelt("Hi There"))),
         std::string{"b617318655057264e28bc0b6fb378c8ef146be00"});

    same(spelled(stamped(Sum::sha1, spelt("Jefe"), spelt("what do ya want for nothing?"))),
         std::string{"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"});
}

// Klucz długoterminowy TURN to skrót z nazwy, dziedziny i hasła sklejonych dwukropkami.
TRIAL("Podpis: klucz długoterminowy jest skrótem sklejenia") {
    if (missing()) return;

    same(spelled(net::keyed("user", "realm", "pass")),
         spelled(summed(Sum::md5, spelt("user:realm:pass"))));

    same(net::keyed("user", "realm", "pass").size(), static_cast<std::size_t>(16));
}

// Podpisana wiadomość musi sprawdzić się własnym kluczem i **nie sprawdzić cudzym**. Pułapką jest
// tu pole długości: podpis liczy się po treści, w której długość już udaje, że atrybut podpisu tam
// stoi — policzony inaczej nie zgodziłby się z żadną drugą stroną.
TRIAL("Podpis: wiadomość sprawdza się własnym kluczem") {
    if (missing()) return;

    const Bytes key = spelt("tajemnica");

    Bytes made = net::asked(net::numbered());

    net::texting(made, 0x0006, "kto");

    net::stamping(made, key);

    ought(net::trusted(made, key));
    ought(!net::trusted(made, spelt("inna")));

    // Zmiana choćby jednego bajtu treści unieważnia podpis.
    Bytes other = made;

    other[24] = ~other[24];

    ought(!net::trusted(other, key));
}

TRIAL("Podpis: wiadomość bez podpisu nie jest zaufana") {
    if (missing()) return;

    const Bytes made = net::asked(net::numbered());

    ought(!net::trusted(made, spelt("tajemnica")));
}

// Sam odcisk **nie potrzebuje skrótów** — to suma kontrolna — więc ta próba biegnie wszędzie
// i pilnuje pułapki z polem długości: liczy się po treści, w której długość już udaje, że atrybut
// odcisku tam stoi.
TRIAL("Odcisk: sam odcisk zgadza się bez żadnych skrótów") {
    Bytes made = net::asked(net::numbered());

    net::printing(made);

    same(made.size(), static_cast<std::size_t>(28));

    // Pole długości musi liczyć wszystko po nagłówku, razem z odciskiem.
    same(static_cast<unsigned>(std::to_integer<unsigned>(made[2]) << 8 |
                               std::to_integer<unsigned>(made[3])),
         8u);

    const std::uint32_t sum = net::tallied(made, made.size() - 8) ^ 0x5354554Eu;

    const std::size_t at = made.size() - 4;

    same(static_cast<unsigned>(std::to_integer<unsigned>(made[at]) << 24 |
                               std::to_integer<unsigned>(made[at + 1]) << 16 |
                               std::to_integer<unsigned>(made[at + 2]) << 8 |
                               std::to_integer<unsigned>(made[at + 3])),
         sum);

    net::Message told;

    ought(net::understood(made, told));
}

// Odcisk idzie na sam koniec i obejmuje także podpis, więc obie rzeczy muszą się zgadzać naraz.
TRIAL("Odcisk: wiadomość z odciskiem i podpisem trzyma się kupy") {
    if (missing()) return;

    const Bytes key = spelt("tajemnica");

    Bytes made = net::asked(net::numbered());

    net::stamping(made, key);
    net::printing(made);

    ought(net::trusted(made, key));

    net::Message told;

    ought(net::understood(made, told));

    // Odcisk sprawdza się tak, jak zrobi to druga strona: suma po wszystkim przed nim.
    const std::uint32_t sum = net::tallied(made, made.size() - 8) ^ 0x5354554Eu;

    const std::size_t at = made.size() - 4;

    same(static_cast<unsigned>(std::to_integer<unsigned>(made[at]) << 24 |
                               std::to_integer<unsigned>(made[at + 1]) << 16 |
                               std::to_integer<unsigned>(made[at + 2]) << 8 |
                               std::to_integer<unsigned>(made[at + 3])),
         sum);
}

TRIAL("Skrót: gdy system nie daje skrótów, mówi to wprost") {
    if (summing()) {
        // Tam, gdzie skróty są, sprawdzamy odwrotność: nie rzuca i oddaje właściwą długość.
        same(summed(Sum::sha1, Bytes{}).size(), static_cast<std::size_t>(20));

        return;
    }

    bool amiss = false;

    try {
        (void)summed(Sum::sha1, Bytes{});
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

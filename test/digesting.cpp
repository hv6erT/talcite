#include "proving.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <iostream>
#include <string>
#include <utility>

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
    same(net::tallied(bytes("123456789"), 9), 0xCBF43926u);
    same(net::tallied(Bytes{}, 0), 0u);
}

TRIAL("Skrót: SHA-1 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::sha1, bytes("abc"))),
         std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});

    same(spelled(summed(Sum::sha1, Bytes{})),
         std::string{"da39a3ee5e6b4b0d3255bfef95601890afd80709"});
}

TRIAL("Skrót: MD5 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::md5, bytes("abc"))), std::string{"900150983cd24fb0d6963f7d28e17f72"});

    same(spelled(summed(Sum::md5, Bytes{})), std::string{"d41d8cd98f00b204e9800998ecf8427e"});
}

TRIAL("Skrót: SHA-256 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::sha256, bytes("abc"))),
         std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
}

TRIAL("Skrót: SHA-512 zgadza się z wektorem") {
    if (missing()) return;

    same(spelled(summed(Sum::sha512, bytes("abc"))),
         std::string{"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"});
}

// RFC 4231, przypadek drugi.
TRIAL("Podpis: HMAC-SHA-512 zgadza się z wektorem z RFC 4231") {
    if (missing()) return;

    same(spelled(stamped(Sum::sha512, bytes("Jefe"), bytes("what do ya want for nothing?"))),
         std::string{"164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
                     "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"});
}

// RFC 5869, przypadki pierwszy i trzeci: z solą i treścią, potem bez obu (sól to zera).
TRIAL("Wywód: HKDF zgadza się z wektorami z RFC 5869") {
    if (missing()) return;

    Bytes salt;

    for (int at = 0; at <= 0x0c; ++at) salt.push_back(static_cast<std::byte>(at));

    Bytes info;

    for (int at = 0xf0; at <= 0xf9; ++at) info.push_back(static_cast<std::byte>(at));

    same(spelled(derived(Sum::sha256, salt, Bytes(22, std::byte{0x0b}), info, 42)),
         std::string{"3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                     "34007208d5b887185865"});

    same(spelled(derived(Sum::sha256, {}, Bytes(22, std::byte{0x0b}), {}, 42)),
         std::string{"8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                     "9d201395faa4b61a96c8"});

    // Dłuższy wywód zaczyna się krótszym — bloki idą po kolei.
    const Bytes longer = derived(Sum::sha512, {}, bytes("x"), {}, 100);

    same(longer.size(), std::size_t{100});
    ought(derived(Sum::sha512, {}, bytes("x"), {}, 64) == Bytes(longer.begin(), longer.begin() + 64));
    threw((void)derived(Sum::sha256, {}, bytes("x"), {}, 255 * 32 + 1));
}

// RFC 2202, przypadki pierwszy i drugi.
TRIAL("Podpis: HMAC-SHA1 zgadza się z wektorami z RFC 2202") {
    if (missing()) return;

    same(spelled(stamped(Sum::sha1, Bytes(20, std::byte{0x0b}), bytes("Hi There"))),
         std::string{"b617318655057264e28bc0b6fb378c8ef146be00"});

    same(spelled(stamped(Sum::sha1, bytes("Jefe"), bytes("what do ya want for nothing?"))),
         std::string{"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"});
}

// Klucz długoterminowy TURN to skrót z nazwy, dziedziny i hasła sklejonych dwukropkami.
TRIAL("Podpis: klucz długoterminowy jest skrótem sklejenia") {
    if (missing()) return;

    same(spelled(net::keyed("user", "realm", "pass")),
         spelled(summed(Sum::md5, bytes("user:realm:pass"))));

    same(net::keyed("user", "realm", "pass").size(), static_cast<std::size_t>(16));
}

// Podpisana wiadomość musi sprawdzić się własnym kluczem i **nie sprawdzić cudzym**. Pułapką jest
// tu pole długości: podpis liczy się po treści, w której długość już udaje, że atrybut podpisu tam
// stoi — policzony inaczej nie zgodziłby się z żadną drugą stroną.
TRIAL("Podpis: wiadomość sprawdza się własnym kluczem") {
    if (missing()) return;

    const Bytes key = bytes("tajemnica");

    Bytes made = net::asked(net::numbered());

    net::texting(made, 0x0006, "kto");

    net::stamping(made, key);

    ought(net::trusted(made, key));
    ought(!net::trusted(made, bytes("inna")));

    // Zmiana choćby jednego bajtu treści unieważnia podpis.
    Bytes other = made;

    other[24] = ~other[24];

    ought(!net::trusted(other, key));
}

TRIAL("Podpis: wiadomość bez podpisu nie jest zaufana") {
    if (missing()) return;

    const Bytes made = net::asked(net::numbered());

    ought(!net::trusted(made, bytes("tajemnica")));
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

    const Bytes key = bytes("tajemnica");

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

// Skrót ratami daje to samo co w całości — dla każdego skrótu, z kluczem i bez, a także wtedy, gdy
// któraś rata jest pusta albo gdy rat nie ma wcale.
TRIAL("Skrót: ratami wychodzi to samo co w całości") {
    if (missing()) return;

    std::string whole;

    for (int at = 0; at < 300000; ++at) whole += static_cast<char>('a' + at % 26);

    for (const Sum which : {Sum::md5, Sum::sha1, Sum::sha256, Sum::sha512}) {
        Digest plain{which};
        Digest keyed{which, bytes("klucz")};

        for (std::size_t at = 0; at < whole.size(); at += 70001) {
            const Bytes part = bytes(whole.substr(at, 70001));

            plain.feed(part);
            plain.feed(Bytes{});
            keyed.feed(part);
        }

        same(spelled(plain.done()), spelled(summed(which, bytes(whole))));
        same(spelled(keyed.done()), spelled(stamped(which, bytes("klucz"), bytes(whole))));

        Digest empty{which};

        same(spelled(empty.done()), spelled(summed(which, Bytes{})));
    }
}

// Skończony skrót nie liczy dalej: druga odpowiedź byłaby skrótem czegoś innego niż to, o co
// pytano. To samo skrót, z którego zabrano stan przeniesieniem.
TRIAL("Skrót: skończony i przeniesiony odmawiają") {
    if (missing()) return;

    Digest one{Sum::sha1};

    one.feed(bytes("abc"));

    same(spelled(one.done()), std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});

    const auto refused = [](auto doing) {
        try {
            doing();
        } catch (const Trouble&) {
            return true;
        }

        return false;
    };

    ought(refused([&] { (void)one.done(); }));
    ought(refused([&] { one.feed(bytes("x")); }));

    Digest two{Sum::sha256};

    two.feed(bytes("ab"));

    Digest three = std::move(two);

    three.feed(bytes("c"));

    ought(refused([&] { two.feed(bytes("x")); }));

    same(spelled(three.done()), std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
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
    } catch (const Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

namespace {

// Zapas, który liczy „skrót" po swojemu: same bajty o wartości podanej, w długości, jaką każe —
// tak widać, kto liczył, i że zła długość nie przechodzi.
class Pretend : public DigestProvider {
public:
    Pretend(std::byte value, std::size_t wide) : value_(value), wide_(wide) {}

    [[nodiscard]] std::string_view name() const override { return "pretend digests"; }

    [[nodiscard]] std::unique_ptr<Digesting> started(Sum, const Bytes* key) override {
        return std::make_unique<Running>(key != nullptr ? std::byte{0x77} : value_, wide_, fed_);
    }

    [[nodiscard]] std::size_t fed() const { return *fed_; }

private:
    class Running : public Digesting {
    public:
        Running(std::byte value, std::size_t wide, std::shared_ptr<std::size_t> fed) : value_(value), wide_(wide), fed_(std::move(fed)) {}

        void feed(const Bytes& what) override { *fed_ += what.size(); }

        [[nodiscard]] Bytes done() override { return Bytes(wide_, value_); }

    private:
        std::byte value_;
        std::size_t wide_;
        std::shared_ptr<std::size_t> fed_;
    };

    std::byte value_;
    std::size_t wide_;
    std::shared_ptr<std::size_t> fed_ = std::make_shared<std::size_t>(0);
};

}

// Zapas idzie w ruch tylko wtedy, gdy system skrótów nie daje. Na systemie, który je daje, liczy
// system i zapas niczego nie zmienia; tam, gdzie nie daje (tak jest na maszynie, na której ta próba
// powstała), liczy zapas — a skrót o złej długości to wyjątek.
TRIAL("Skrót: zapas liczy tylko wtedy, gdy system nie daje") {
    const bool system = summing();

    const auto spare = std::make_shared<Pretend>(std::byte{0x11}, 32);

    spared(spare);

    ought(summing());

    if (system) {
        same(spelled(summed(Sum::sha256, bytes("abc"))),
             std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
        same(spare->fed(), std::size_t{0});
    } else {
        same(summed(Sum::sha256, bytes("abc")), Bytes(32, std::byte{0x11}));
        same(stamped(Sum::sha256, bytes("klucz"), bytes("abc")), Bytes(32, std::byte{0x77}));
        same(spare->fed(), std::size_t{6});

        spared(std::make_shared<Pretend>(std::byte{0x11}, 31));

        threw(static_cast<void>(summed(Sum::sha256, bytes("abc"))));
    }

    spared(nullptr);

    ought(summing() == system);

    if (!system) threw(static_cast<void>(summed(Sum::sha256, bytes("abc"))));
}

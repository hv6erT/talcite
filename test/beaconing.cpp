#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/beacon.hpp"
#include "talcite/bytes.hpp"
#include "talcite/time.hpp"
#include "talcite/udp.hpp"

#include "exchange.hpp"

using namespace talcite;

// **Majak w sieci lokalnej** (S1): udostępniający odpowiada na pytanie o znacznik, który zna,
// pytający zbiera odpowiedzi. Próby pytają wprost pętlę zwrotną — rozgłoszenie zależy od sieci,
// w której stoi maszyna, więc ma osobną próbę, która umie się pominąć.
namespace {

const net::Patience briefly{400};

Bytes tagged(const std::string& said) { return bytes(said); }

std::vector<net::Address> here(const net::Beacon& one) {
    return {net::Address::loopback(net::Family::four, one.mine().port())};
}

}

TRIAL("Majak: odpowiada na znacznik, który zna, treścią, którą mu dano") {
    const net::Beacon one{0};

    ought(one.held());
    ought(one.mine().port() != 0);

    one.offer(tagged("etykieta"), bytes("port 4123"));

    const std::vector<net::Sighting> found =
        net::Beacon::sought(one.mine().port(), tagged("etykieta"), briefly, here(one)).await();

    same(found.size(), std::size_t{1});

    if (found.empty()) return;

    same(text(found.front().payload), std::string{"port 4123"});
    same(found.front().from.port(), one.mine().port());

    // Ten sam majak zapytany dwa razy (dwa adresy prowadzą do niego) liczy się raz.
    const std::vector<net::Sighting> twice = net::Beacon::sought(one.mine().port(), tagged("etykieta"), briefly,
                                                                 {here(one).front(), here(one).front()})
                                                 .await();

    same(twice.size(), std::size_t{1});
}

// Nieznany znacznik i znacznik wycofany — pusta lista. „Nikogo tu nie ma" to wynik, a nie odmowa.
TRIAL("Majak: nieznany albo wycofany znacznik — nikt nie odpowiada") {
    const net::Beacon one{0};

    one.offer(tagged("jest"), bytes("x"));

    ought(net::Beacon::sought(one.mine().port(), tagged("nie ma"), briefly, here(one)).await().empty());

    one.withdraw(tagged("jest"));

    ought(net::Beacon::sought(one.mine().port(), tagged("jest"), briefly, here(one)).await().empty());
}

// Obcy datagram na porcie majaka — śmieci, ucięta wiadomość, inna wersja, odpowiedź zamiast
// pytania — nie zmienia niczego: następne prawdziwe pytanie dostaje odpowiedź.
TRIAL("Majak: obce datagramy niczego nie psują") {
    const net::Beacon one{0};

    one.offer(tagged("etykieta"), bytes("ok"));

    const net::Udp stranger{net::Address::loopback(net::Family::four, 0)};
    const net::Address target = here(one).front();

    const auto from_text = [](const std::string& said) { return bytes(said); };

    for (const Bytes& junk : {from_text("śmieci"), from_text("TLCB"), from_text(std::string{"TLCB\x02\x01"} + std::string(8, 'n') + "\x08" "etykieta"),
                              from_text(std::string{"TLCB\x01\x02"} + std::string(8, 'n') + "\x08" + std::string{"etykieta\x00\x02ok", 12}),
                              from_text(std::string{"TLCB\x01\x01"} + std::string(8, 'n') + "\x08" "etykietaXX"),
                              from_text(std::string{"TLCB\x01\x01"} + std::string(8, 'n') + "\x50" "etykieta")})
        stranger.send(target, junk, briefly).await();

    const std::vector<net::Sighting> found = net::Beacon::sought(one.mine().port(), tagged("etykieta"), briefly, here(one)).await();

    same(found.size(), std::size_t{1});

    // Pytanie dobre we wszystkim poza jednym — znakiem, wersją, rodzajem, nadmiarowym bajtem albo
    // długością znacznika — nie dostaje odpowiedzi. To samo pytanie poprawne dostaje.
    const std::string luck(8, 'n');
    const std::string asked = std::string{"TLCB\x01\x01"} + luck + "\x08" "etykieta";

    for (const std::string& wrong : {std::string{"XLCB\x01\x01"} + luck + "\x08" "etykieta",
                                     std::string{"TLCB\x02\x01"} + luck + "\x08" "etykieta",
                                     std::string{"TLCB\x01\x03"} + luck + "\x08" "etykieta", asked + "X",
                                     std::string{"TLCB\x01\x01"} + luck + "\x41" "etykieta" + std::string(57, 'x'),
                                     std::string{"TLCB\x01\x02"} + luck + "\x08" "etykieta" + std::string{"\x00\x02ok", 4}}) {
        stranger.send(target, from_text(wrong), briefly).await();

        threw(stranger.receive(256, net::Patience{250}).await());
    }

    stranger.send(target, from_text(asked), briefly).await();

    const net::Datagram answer = stranger.receive(256, briefly).await();

    ought(answer.bytes.size() > 5 && answer.bytes[5] == std::byte{2});
}

// Odpowiedź na cudze pytanie — inne bajty losowe — albo z innym znacznikiem nie jest odpowiedzią
// dla nas. Fałszywy majak odpowiada na wszystko po swojemu; liczy się tylko prawdziwy.
TRIAL("Majak: odpowiedź z cudzym losem albo cudzym znacznikiem się nie liczy") {
    const net::Beacon real{0};

    real.offer(tagged("etykieta"), bytes("prawdziwy"));

    const net::Udp fake{net::Address::loopback(net::Family::four, 0)};

    // Fałszywy słucha jednego pytania i odpowiada dwa razy: z innym losem i z innym znacznikiem.
    auto heard = fake.receive(256, net::Patience{2000});

    auto seeking = net::Beacon::sought(real.mine().port(), tagged("etykieta"), net::Patience{600},
                                       {here(real).front(), fake.mine()});

    // Obietnica łańcucha rusza dopiero na prośbę — tu przed czekaniem na pytanie u fałszywego.
    seeking.start();

    const net::Datagram asked = heard.await();

    ought(asked.bytes.size() > 14);

    if (asked.bytes.size() > 14) {
        Bytes other_luck = asked.bytes;

        other_luck[5] = std::byte{2};
        other_luck[6] = static_cast<std::byte>(static_cast<unsigned char>(other_luck[6]) ^ 0xff);

        for (const char each : std::string{"\x00\x05obcy!", 7}) other_luck.push_back(static_cast<std::byte>(each));

        fake.send(asked.from, other_luck, briefly).await();

        Bytes other_tag = asked.bytes;

        other_tag[5] = std::byte{2};
        other_tag[other_tag.size() - 1] = std::byte{'X'};

        for (const char each : std::string{"\x00\x05obcy!", 7}) other_tag.push_back(static_cast<std::byte>(each));

        fake.send(asked.from, other_tag, briefly).await();

        // Nasze własne pytanie odbite z powrotem — ten sam los i znacznik, ale to nie odpowiedź.
        fake.send(asked.from, asked.bytes, briefly).await();

        // Odpowiedź na nasze pytanie, ale z długością treści, która nie zgadza się z resztą.
        Bytes crooked = asked.bytes;

        crooked[5] = std::byte{2};

        for (const char each : std::string{"\x00\x05obcy!!", 8}) crooked.push_back(static_cast<std::byte>(each));

        fake.send(asked.from, crooked, briefly).await();
    }

    const std::vector<net::Sighting> found = seeking.await();

    same(found.size(), std::size_t{1});
    ought(!found.empty() && text(found.front().payload) == "prawdziwy");
}

TRIAL("Majak: granice znacznika i treści") {
    const net::Beacon one{0};

    threw(one.offer(Bytes{}, bytes("x")));
    threw(one.offer(Bytes(65, std::byte{'t'}), bytes("x")));
    threw(one.offer(tagged("t"), Bytes(1025, std::byte{'p'})));

    one.offer(Bytes(64, std::byte{'t'}), Bytes(1024, std::byte{'p'}));

    const std::vector<net::Sighting> found =
        net::Beacon::sought(one.mine().port(), Bytes(64, std::byte{'t'}), briefly, here(one)).await();

    same(found.size(), std::size_t{1});
    ought(!found.empty() && found.front().payload.size() == 1024);

    threw(net::Beacon::sought(one.mine().port(), Bytes{}, briefly).await());
    threw(net::Beacon::sought(one.mine().port(), Bytes(65, std::byte{'t'}), briefly).await());
}

// Zamknięty majak milczy i nie przyjmuje ofert.
TRIAL("Majak: zamknięty milczy") {
    net::Beacon one{0};

    const std::uint16_t port = one.mine().port();

    one.offer(tagged("etykieta"), bytes("x"));
    one.close();

    ought(!one.held());
    threw(one.offer(tagged("etykieta"), bytes("x")));
    ought(net::Beacon::sought(port, tagged("etykieta"), briefly, {net::Address::loopback(net::Family::four, port)})
              .await()
              .empty());
}

// Rozgłoszenie zależy od sieci maszyny: bez trasy rozgłoszeniowej pytanie nie wyjdzie i nikt nie
// odpowie. Wtedy próba mówi, że się pomija, zamiast udawać porażkę.
TRIAL("Majak: rozgłoszenie w sieci lokalnej") {
    const net::Beacon one{0};

    one.offer(tagged("rozgloszenie"), bytes("tu"));

    const std::vector<net::Sighting> found =
        net::Beacon::sought(one.mine().port(), tagged("rozgloszenie"), net::Patience{600}).await();

    if (found.empty()) {
        std::cout << "        rozgłoszenie nie wraca na tej maszynie — próba pominięta\n";

        return;
    }

    same(text(found.front().payload), std::string{"tu"});
}

// Majak porzucony bez `close()` nie zostawia w centrali odbioru bez terminu — inaczej czekałby on
// do końca programu, trzymając gniazdo.
TRIAL("Majak: porzucony nie zostawia czekania w centrali") {
    // Najpierw cisza po próbach wcześniejszych: gniazdo, które właśnie schodzi, zmieniłoby licznik
    // w trakcie i porównanie mówiłoby o nim, a nie o majaku.
    for (int tried = 0; tried < 200 && net::Exchange::running().waiting() > 0; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();

    const std::size_t before = net::Exchange::running().waiting();

    {
        const net::Beacon one{0};

        one.offer(tagged("etykieta"), bytes("x"));

        ought(net::Exchange::running().waiting() > before);
    }

    for (int tried = 0; tried < 200 && net::Exchange::running().waiting() > before; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();

    ought(net::Exchange::running().waiting() <= before);
}

// Zalew pytań nie robi z majaka wzmacniacza: w jednej sekundzie najwyżej 32 odpowiedzi.
TRIAL("Majak: najwyżej 32 odpowiedzi na sekundę") {
    const net::Beacon one{0};

    one.offer(tagged("etykieta"), bytes("x"));

    const net::Udp stranger{net::Address::loopback(net::Family::four, 0)};
    const Bytes asked = bytes(std::string{"TLCB\x01\x01"} + std::string(8, 'n') + "\x08" "etykieta");

    for (int turn = 0; turn < 48; ++turn) stranger.send(here(one).front(), asked, briefly).await();

    int answers = 0;

    for (;;) {
        try {
            (void)stranger.receive(256, net::Patience{300}).await();
        } catch (const Overdue&) {
            break;
        }

        ++answers;
    }

    ought(answers >= 1);
    ought(answers <= 32);
}

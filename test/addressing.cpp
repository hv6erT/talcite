#include "proving.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

TRIAL("Adres: świeży nic nie trzyma") {
    const net::Address one;

    ought(!one.held());
    same(one.family(), net::Family::none);
    same(one.port(), static_cast<std::uint16_t>(0));
    same(one.text(), std::string{});
    same(one.spelled(), std::string{});
}

TRIAL("Adres: czwórka rozbiera się i składa z powrotem") {
    const net::Address one{"192.0.2.17", 8080};

    ought(one.held());
    same(one.family(), net::Family::four);
    same(one.port(), static_cast<std::uint16_t>(8080));
    same(one.text(), std::string{"192.0.2.17"});
    same(one.spelled(), std::string{"192.0.2.17:8080"});
}

TRIAL("Adres: szóstka staje w nawiasach dopiero z portem") {
    const net::Address one{"2001:db8::1", 443};

    same(one.family(), net::Family::six);
    same(one.text(), std::string{"2001:db8::1"});
    same(one.spelled(), std::string{"[2001:db8::1]:443"});
}

TRIAL("Adres: nawiasy kwadratowe wolno podać") {
    const net::Address one{"[::1]", 22};

    same(one.family(), net::Family::six);
    same(one.text(), std::string{"::1"});
    same(one.spelled(), std::string{"[::1]:22"});
}

TRIAL("Adres: numer łącza przeżywa rozbiór i zapis") {
    const net::Address one{"fe80::1%7", 100};

    same(one.family(), net::Family::six);
    same(one.scope(), static_cast<std::uint32_t>(7));
    same(one.text(), std::string{"fe80::1%7"});
    same(one.spelled(), std::string{"[fe80::1%7]:100"});
}

TRIAL("Adres: nazwa domenowa to nie adres") {
    bool threw = false;

    try {
        const net::Address one{"przyklad.pl", 80};

        (void)one;
    } catch (const net::Trouble& why) {
        threw = true;

        ought(std::string{why.what()}.find("przyklad.pl") != std::string::npos);
    }

    ought(threw);
}

TRIAL("Adres: śmieć nie staje się pustym adresem po cichu") {
    bool threw = false;

    try {
        const net::Address one{"999.1.2.3", 80};

        (void)one;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Adres: dowolny i pętla zwrotna") {
    const net::Address four = net::Address::any(net::Family::four, 1234);
    const net::Address six = net::Address::any(net::Family::six, 1234);

    same(four.text(), std::string{"0.0.0.0"});
    same(six.text(), std::string{"::"});

    same(net::Address::loopback(net::Family::four, 80).text(), std::string{"127.0.0.1"});
    same(net::Address::loopback(net::Family::six, 80).text(), std::string{"::1"});
}

TRIAL("Adres: porównanie patrzy na bajty, rodzinę i port") {
    const net::Address one{"192.0.2.1", 80};
    const net::Address two{"192.0.2.1", 80};
    const net::Address three{"192.0.2.1", 81};
    const net::Address four{"192.0.2.2", 80};

    ought(one == two);
    ought(!(one == three));
    ought(!(one == four));
}

TRIAL("Adres: port da się przestawić po fakcie") {
    net::Address one{"::1", 0};

    one.port(9000);

    same(one.spelled(), std::string{"[::1]:9000"});
}

TRIAL("Adres: da się zbudować z gołych bajtów") {
    std::array<std::byte, 16> raw{};

    raw[0] = std::byte{10};
    raw[3] = std::byte{7};

    const net::Address one{net::Family::four, raw, 25};

    same(one.text(), std::string{"10.0.0.7"});
    same(one.port(), static_cast<std::uint16_t>(25));
}

TRIAL("Nazwy: pętla zwrotna ma adres") {
    const std::vector<net::Address> made = net::resolve("localhost", 80).await();

    ought(!made.empty());

    for (const net::Address& one : made) {
        ought(one.held());
        same(one.port(), static_cast<std::uint16_t>(80));
    }
}

TRIAL("Nazwy: adres wpisany wprost też przechodzi") {
    const std::vector<net::Address> made = net::resolve("127.0.0.1", 443).await();

    same(static_cast<int>(made.size()), 1);
    same(made.front().text(), std::string{"127.0.0.1"});
    same(made.front().port(), static_cast<std::uint16_t>(443));
}

TRIAL("Nazwy: można poprosić o jedną rodzinę") {
    const std::vector<net::Address> made =
        net::resolve("localhost", 80, net::Family::four).await();

    for (const net::Address& one : made) same(one.family(), net::Family::four);
}

TRIAL("Nazwy: nieznana nazwa odrzuca obietnicę") {
    bool threw = false;

    try {
        (void)net::resolve("nie.ma.takiej.nazwy.talcite.test", 80).await();
    } catch (const net::Trouble& why) {
        threw = true;

        ought(why.code() != 0);
    }

    ought(threw);
}

TRIAL("Własne: wykaz nie jest pusty i ma pętlę zwrotną") {
    const std::vector<net::Address> made = net::mine().await();

    ought(!made.empty());

    bool loopback = false;

    for (const net::Address& one : made) {
        ought(one.held());

        if (one == net::Address::loopback(one.family(), 0)) loopback = true;
    }

    ought(loopback);
}

TRIAL("Własne: wykaz nie ma portów") {
    for (const net::Address& one : net::mine().await())
        same(one.port(), static_cast<std::uint16_t>(0));
}

TRIAL("Własne: można poprosić o jedną rodzinę") {
    const std::vector<net::Address> made = net::mine(net::Family::four).await();

    ought(!made.empty());

    for (const net::Address& one : made) same(one.family(), net::Family::four);
}

TRIAL("Własne: ten sam adres nie powtarza się dwa razy") {
    const std::vector<net::Address> made = net::mine().await();

    for (std::size_t at = 0; at < made.size(); ++at)
        for (std::size_t which = at + 1; which < made.size(); ++which)
            ought(!(made[at] == made[which]));
}

TRIAL("Własne: każdy adres daje się wypisać i wczytać z powrotem") {
    for (const net::Address& one : net::mine().await()) {
        // Link-local niesie numer łącza po procencie, a ten wraca tylko na systemach, które go
        // nazywają — dlatego porównujemy sam adres, bez numeru.
        const net::Address again{one.text(), one.port()};

        same(again.family(), one.family());
        same(again.bytes(), one.bytes());
    }
}

// Postać, którą sami wypisujemy, musi dać się wczytać z powrotem. Rozcięcie po procencie szło
// przed zdjęciem nawiasów, więc zamykający nawias wpadał do tekstu numeru łącza.
TRIAL("Adres: nawiasy razem z numerem łącza") {
    const net::Address made{"[fe80::1%1]", 546};

    same(made.family(), net::Family::six);
    same(made.port(), static_cast<std::uint16_t>(546));
    same(made.scope(), 1u);

    const net::Address again{made.text(), made.port()};

    same(again.bytes(), made.bytes());
}

// Zero w środku ucinało wejście po cichu i „1.2.3.4" sklejone z bzdurą budowało się pomyślnie.
TRIAL("Adres: zero w środku tekstu nie ucina wejścia") {
    bool threw = false;

    try {
        const net::Address made{std::string_view{"1.2.3.4\0bzdura", 14}, 80};

        (void)made;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

// Nieznana nazwa łącza cichła do zera, a adres link-local bez numeru jest bezużyteczny — błąd
// wychodził dopiero przy połączeniu, daleko od literówki.
TRIAL("Adres: nieznana nazwa łącza jest kłopotem, nie zerem") {
    bool threw = false;

    try {
        const net::Address made{"fe80::1%takiejniema", 80};

        (void)made;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

// Numer łącza należy się wyłącznie adresom łączowym. Doklejany wszystkim robił z pętli zwrotnej
// `::1%1`, a ta przestawała równać się `::1` i wychodziła jako kandydat do przebijania.
TRIAL("Własne: numer łącza tylko przy adresach łączowych") {
    for (const net::Address& one : net::mine().await()) {
        if (one.family() != net::Family::six) continue;

        if (one.scope() == 0) continue;

        const unsigned first = std::to_integer<unsigned>(one.bytes()[0]);
        const unsigned second = std::to_integer<unsigned>(one.bytes()[1]);

        ought(first == 0xFE && (second & 0xC0) == 0x80);
    }
}

// --- gniazdo lokalne ---------------------------------------------------------------------------

TRIAL("Adres lokalny: ścieżka zamiast adresu i portu") {
    const net::Address one = net::Address::local("/tmp/talcite-proba.sock");

    ought(one.held());
    same(one.family(), net::Family::local);
    same(one.path(), std::string{"/tmp/talcite-proba.sock"});
    same(one.port(), static_cast<std::uint16_t>(0));
    same(one.scope(), 0u);

    // Ani text(), ani spelled() nie doklejają portu: gniazdo lokalne go nie ma, a "…sock:0"
    // byłoby kłamstwem o tym, czym ten adres jest.
    same(one.text(), std::string{"/tmp/talcite-proba.sock"});
    same(one.spelled(), std::string{"/tmp/talcite-proba.sock"});
}

TRIAL("Adres lokalny: równość idzie po ścieżce") {
    const net::Address one = net::Address::local("/tmp/a.sock");
    const net::Address two = net::Address::local("/tmp/a.sock");
    const net::Address three = net::Address::local("/tmp/b.sock");

    ought(one == two);
    ought(!(one == three));
}

TRIAL("Adres lokalny: pusta ścieżka odmawia") {
    bool told = false;

    try {
        (void)net::Address::local("");
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

TRIAL("Adres lokalny: zero w środku ścieżki odmawia") {
    bool told = false;

    try {
        (void)net::Address::local(std::string{"/tmp/a\0b.sock", 13});
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

TRIAL("Adres lokalny: za długa ścieżka odmawia przy budowaniu, nie przy gnieździe") {
    bool told = false;

    try {
        (void)net::Address::local("/tmp/" + std::string(120, 'x'));
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

TRIAL("Adres lokalny: port i numer łącza odmawiają") {
    net::Address one = net::Address::local("/tmp/a.sock");

    bool port = false;
    bool scope = false;

    try {
        one.port(80);
    } catch (const net::Trouble&) {
        port = true;
    }

    try {
        one.scope(3);
    } catch (const net::Trouble&) {
        scope = true;
    }

    ought(port);
    ought(scope);

    // Odmowa nie zostawia po sobie połowicznego stanu.
    same(one.port(), static_cast<std::uint16_t>(0));
    same(one.scope(), 0u);
}

TRIAL("Adres lokalny: nie powstaje żadną z pozostałych dróg") {
    const std::array<std::byte, 16> raw{};

    bool bytes = false;
    bool any = false;
    bool loop = false;

    try {
        (void)net::Address{net::Family::local, raw, 0};
    } catch (const net::Trouble&) {
        bytes = true;
    }

    try {
        (void)net::Address::any(net::Family::local, 0);
    } catch (const net::Trouble&) {
        any = true;
    }

    try {
        (void)net::Address::loopback(net::Family::local, 0);
    } catch (const net::Trouble&) {
        loop = true;
    }

    ought(bytes);
    ought(any);
    ought(loop);
}

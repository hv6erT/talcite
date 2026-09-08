#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "talcite/post.hpp"
#include "talcite/scout.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

// Udawany serwer STUN składa odpowiedzi **bajt po bajcie**, nie moim kodekiem. Dzięki temu to
// jest prawdziwy test zgodności z formatem, a nie sprawdzanie się z samym sobą.
namespace {

const net::Patience soon{6000};

void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

Bytes addressed(std::uint16_t kind, const std::string& one, std::uint16_t port,
                const Bytes& number, bool xored) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{1});

    pushed(value, xored ? static_cast<std::uint16_t>(port ^ 0x2112) : port);

    std::uint32_t raw = 0;

    std::size_t at = 0;

    for (int which = 0; which < 4; ++which) {
        std::string part;

        while (at < one.size() && one[at] != '.') part += one[at++];

        ++at;

        raw = (raw << 8) | static_cast<std::uint32_t>(std::stoul(part));
    }

    const std::uint32_t cookie = 0x2112A442u;

    if (xored) raw ^= cookie;

    for (int which = 0; which < 4; ++which)
        value.push_back(static_cast<std::byte>((raw >> (24 - 8 * which)) & 0xFF));

    Bytes made;

    pushed(made, 0x0101);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte what : number) made.push_back(what);

    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte what : value) made.push_back(what);

    return made;
}

Bytes sixed(const Bytes& number, std::uint16_t port) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{2});

    pushed(value, static_cast<std::uint16_t>(port ^ 0x2112));

    // 2001:db8::1 przekłamane ciasteczkiem i numerem transakcji.
    const std::uint8_t plain[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    const std::uint8_t cookie[4] = {0x21, 0x12, 0xA4, 0x42};

    for (std::size_t at = 0; at < 16; ++at) {
        const std::byte mask =
            at < 4 ? static_cast<std::byte>(cookie[at]) : number[at - 4];

        value.push_back(static_cast<std::byte>(plain[at]) ^ mask);
    }

    Bytes made;

    pushed(made, 0x0101);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte what : number) made.push_back(what);

    pushed(made, 0x0020);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte what : value) made.push_back(what);

    return made;
}

Bytes refused(const Bytes& number) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{20});

    for (const char one : std::string{"Nie ma"}) value.push_back(static_cast<std::byte>(one));

    while (value.size() % 4 != 0) value.push_back(std::byte{0});

    Bytes made;

    pushed(made, 0x0111);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte what : number) made.push_back(what);

    pushed(made, 0x0009);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte what : value) made.push_back(what);

    return made;
}

Bytes numbered(const Bytes& what) {
    return Bytes{what.begin() + 8, what.begin() + 20};
}

net::Post standing() { return net::Post{net::Address::loopback(net::Family::four, 0)}; }

using Server = std::pair<std::string, std::uint16_t>;

// Publiczne serwery STUN wbudowane w test, żeby zgodność z prawdziwym światem dało się sprawdzić
// jednym poleceniem. Kolejność nie jest przypadkowa: `mapping()` pyta **dwa pierwsze**, a te muszą
// należeć do różnych operatorów. Oba adresy Google rozwiązują się dziś na ten sam adres, więc
// postawione obok siebie dałyby werdykt „zgodne" z jednego serwera — czyli nieprawdę.
std::vector<Server> known() {
    return {{"stun.l.google.com", 19302},
            {"stun.cloudflare.com", 3478},
            {"stun1.l.google.com", 19302},
            {"stun.nextcloud.com", 443}};
}

}

TRIAL("Zwiad: odbity adres z przekłamanego atrybutu") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    server.send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true),
                soon)
        .await();

    const net::Address made = asking.await();

    same(made.text(), std::string{"203.0.113.7"});
    same(made.port(), static_cast<std::uint16_t>(40404));
    same(scout.last().text(), std::string{"203.0.113.7"});
}

TRIAL("Zwiad: stary atrybut bez przekłamania też jest rozumiany") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    server.send(came.from, addressed(0x0001, "198.51.100.9", 1234, numbered(came.bytes), false),
                soon)
        .await();

    same(asking.await().text(), std::string{"198.51.100.9"});
}

TRIAL("Zwiad: adres szóstej rodziny") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    server.send(came.from, sixed(numbered(came.bytes), 9999), soon).await();

    const net::Address made = asking.await();

    same(made.family(), net::Family::six);
    same(made.text(), std::string{"2001:db8::1"});
    same(made.port(), static_cast<std::uint16_t>(9999));
}

TRIAL("Zwiad: cudza odpowiedź nie jest przyjmowana") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes other = numbered(came.bytes);

    other[0] = ~other[0];

    // Najpierw odpowiedź z cudzym numerem transakcji, potem właściwa.
    server.send(came.from, addressed(0x0020, "1.2.3.4", 1, other, true), soon).await();
    server
        .send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true), soon)
        .await();

    same(asking.await().text(), std::string{"203.0.113.7"});
}

TRIAL("Zwiad: śmieć na gnieździe nie przeszkadza") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes trash(40, std::byte{0xFF});

    server.send(came.from, trash, soon).await();
    server
        .send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true), soon)
        .await();

    same(asking.await().text(), std::string{"203.0.113.7"});
}

TRIAL("Zwiad: odmowa serwera wychodzi jako kłopot") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    server.send(came.from, refused(numbered(came.bytes)), soon).await();

    bool amiss = false;

    try {
        (void)asking.await();
    } catch (const net::Trouble& why) {
        amiss = true;

        same(why.code(), 420);
    }

    ought(amiss);
}

TRIAL("Zwiad: powtórka po zgubionym zapytaniu") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, net::Patience{5000}};

    scout.add(server.mine());

    auto asking = scout.seen();

    // Pierwsze zapytanie przepada — odpowiadamy dopiero na drugie.
    (void)server.receive(1024, soon).await();

    const net::Parcel came = server.receive(1024, soon).await();

    server.send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true),
                soon)
        .await();

    same(asking.await().text(), std::string{"203.0.113.7"});
}

TRIAL("Zwiad: cisza kończy się terminem") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, net::Patience{300}};

    scout.add(server.mine());

    bool late = false;

    try {
        (void)scout.seen().await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Zwiad: bez serwerów nie ma czego pytać") {
    net::Post mine = standing();

    net::Scout scout{mine, soon};

    bool amiss = false;

    try {
        (void)scout.seen().await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Zwiad: dwa pytania naraz dostają odmowę") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, net::Patience{400}};

    scout.add(server.mine());

    auto first = scout.seen();

    bool busy = false;

    try {
        (void)scout.seen().await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }
}

TRIAL("Zwiad: zgodne odbicia znaczą, że przebijanie ma sens") {
    net::Post mine = standing();
    net::Post one = standing();
    net::Post two = standing();

    net::Scout scout{mine, soon};

    scout.add(one.mine());
    scout.add(two.mine());

    auto asking = scout.mapping();

    const net::Parcel first = one.receive(1024, soon).await();

    one.send(first.from, addressed(0x0020, "203.0.113.7", 40404, numbered(first.bytes), true),
             soon)
        .await();

    const net::Parcel second = two.receive(1024, soon).await();

    two.send(second.from, addressed(0x0020, "203.0.113.7", 40404, numbered(second.bytes), true),
             soon)
        .await();

    same(asking.await(), net::Scout::Mapping::alike);
    same(scout.known(), net::Scout::Mapping::alike);
}

TRIAL("Zwiad: różne odbicia znaczą NAT symetryczny") {
    net::Post mine = standing();
    net::Post one = standing();
    net::Post two = standing();

    net::Scout scout{mine, soon};

    scout.add(one.mine());
    scout.add(two.mine());

    auto asking = scout.mapping();

    const net::Parcel first = one.receive(1024, soon).await();

    one.send(first.from, addressed(0x0020, "203.0.113.7", 40404, numbered(first.bytes), true),
             soon)
        .await();

    const net::Parcel second = two.receive(1024, soon).await();

    two.send(second.from, addressed(0x0020, "203.0.113.7", 51515, numbered(second.bytes), true),
             soon)
        .await();

    same(asking.await(), net::Scout::Mapping::symmetric);
}

TRIAL("Zwiad: przy jednym serwerze werdyktu nie ma") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    same(scout.mapping().await(), net::Scout::Mapping::unknown);
}

// Jedyna próba w całym zestawie, która wychodzi w internet. **Brak sieci nie jest tu porażką** —
// zestaw ma przechodzić na maszynie odciętej, a odcięcia od wady kodu odróżnić się nie da. Próba
// wypisuje więc, co zastała, i wywraca się wyłącznie wtedy, gdy serwer odpowiedział **źle**:
// pustym adresem albo werdyktem bez treści. Sam odczyt z prawdziwego świata trzeba przeczytać
// oczami, a najwygodniej tak:
//
//     talcite-test --only "Zwiad: prawdziwe serwery w internecie"
TRIAL("Zwiad: prawdziwe serwery w internecie") {
    net::Post mine{net::Address::any(net::Family::four, 0)};

    net::Scout scout{mine, net::Patience{4000}};

    for (const Server& one : known()) {
        try {
            const std::vector<net::Address> found =
                net::resolve(one.first, one.second, net::Family::four).await();

            if (!found.empty()) scout.add(found.front());
        } catch (const std::exception&) {
        }
    }

    if (scout.servers().empty()) {
        std::cout << "        bez sieci — nazw serwerów nie dało się rozwiązać, próba pominięta\n";

        return;
    }

    net::Address seen;

    try {
        seen = scout.seen().await();
    } catch (const std::exception& why) {
        std::cout << "        bez odpowiedzi z sieci (" << why.what() << ") — próba pominięta\n";

        return;
    }

    std::cout << "        widzą mnie jako " << seen.spelled() << "\n";

    ought(seen.held());
    ought(seen.port() != 0);

    const net::Scout::Mapping what = scout.mapping().await();

    std::cout << "        odwzorowanie: "
              << (what == net::Scout::Mapping::alike
                      ? "zgodne"
                      : what == net::Scout::Mapping::symmetric ? "symetryczne" : "nieznane")
              << "\n";

    // Werdykt jest **wypisany, nie sprawdzany**. „Nieznane" znaczy tu, że któryś z dwóch pierwszych
    // serwerów zamilkł — a to jest stan sieci, nie wada kodu, i test nie ma prawa się o to
    // przewracać. Prawdziwym sprawdzianem zgodności jest adres wyżej: obcy serwer odpowiedział
    // tak, że nasz kodek wyjął z tego sensowny adres i port.
}

// Znacznik „już pytam" zerowało samo ciało obietnicy, więc pytanie anulowane albo porzucone
// nieruszone unieruchamiało zwiadowcę **na zawsze**. Teraz znika najpóźniej wraz z obietnicą.
// Anulowanie nie zatrzymuje pytania, które już biegnie — to jest umowa całego talcite — więc
// zwiadowca odzyskuje głos dopiero, gdy tamto się skończy.
TRIAL("Zwiad: anulowane pytanie nie unieruchamia zwiadowcy na zawsze") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, net::Patience{300}};

    scout.add(server.mine());

    {
        auto first = scout.seen();

        first.cancel();

        try {
            (void)first.await();
        } catch (const std::exception&) {
        }
    }

    bool again = false;

    for (int turn = 0; turn < 40 && !again; ++turn) {
        try {
            (void)scout.seen().await();
        } catch (const Overdue&) {
            again = true;
        } catch (const net::Trouble&) {
            Time::later(std::chrono::milliseconds{100}).await();
        }
    }

    ought(again);
}

// To samo gniazdo niesie potem ruch rozmówcy, więc jeden zwykły duży pakiet nie może kończyć zwiadu.
TRIAL("Zwiad: za duży datagram nie wywraca pytania") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    server.send(came.from, Bytes(4000, std::byte{0x41}), soon).await();

    server.send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true),
                soon)
        .await();

    same(asking.await().text(), std::string{"203.0.113.7"});
}

// Jeden źle ustawiony serwer na początku listy wykluczał wszystkie następne.
TRIAL("Zwiad: odmowa jednego serwera nie kończy obchodu") {
    net::Post mine = standing();
    net::Post bad = standing();
    net::Post good = standing();

    net::Scout scout{mine, soon};

    scout.add(bad.mine());
    scout.add(good.mine());

    auto asking = scout.seen();

    const net::Parcel first = bad.receive(1024, soon).await();

    bad.send(first.from, refused(numbered(first.bytes)), soon).await();

    const net::Parcel second = good.receive(1024, soon).await();

    good.send(second.from, addressed(0x0020, "198.51.100.9", 1234, numbered(second.bytes), true),
              soon)
        .await();

    same(asking.await().text(), std::string{"198.51.100.9"});
}

// Ten sam adres dodany dwa razy dawał dwie identyczne odpowiedzi, a z nich werdykt „zgodne" —
// czyli kłamstwo w stronę niebezpieczną.
TRIAL("Zwiad: ten sam serwer dwa razy nie daje werdyktu") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, net::Patience{600}};

    scout.add(server.mine());
    scout.add(server.mine());

    same(scout.mapping().await(), net::Scout::Mapping::unknown);
}

// Popsuty atrybut przekłamany nie ma prawa skasować poprawnie odczytanego starego.
TRIAL("Zwiad: zły atrybut nie kasuje dobrego") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes made = addressed(0x0001, "198.51.100.9", 1234, numbered(came.bytes), false);

    // Dopisujemy przekłamany atrybut z niemożliwą rodziną i poprawiamy długość wiadomości.
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{7});
    pushed(value, 0);
    pushed(value, 0);
    pushed(value, 0);

    pushed(made, 0x0020);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    const std::uint16_t much = static_cast<std::uint16_t>(made.size() - 20);

    made[2] = static_cast<std::byte>(much >> 8);
    made[3] = static_cast<std::byte>(much & 0xFF);

    server.send(came.from, made, soon).await();

    same(asking.await().text(), std::string{"198.51.100.9"});
}

// Powód odmowy pisze obcy, a zdanie idzie na terminal.
TRIAL("Zwiad: powód odmowy nie niesie znaków sterujących") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Scout scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{20});

    for (const char one : std::string{"\x1b[2J\x07 nic"}) value.push_back(static_cast<std::byte>(one));

    while (value.size() % 4 != 0) value.push_back(std::byte{0});

    Bytes made;

    pushed(made, 0x0111);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte one : numbered(came.bytes)) made.push_back(one);

    pushed(made, 0x0009);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    server.send(came.from, made, soon).await();

    bool amiss = false;

    try {
        (void)asking.await();
    } catch (const net::Trouble& why) {
        amiss = true;

        const std::string said{why.what()};

        for (const char one : said) ought(one >= 0x20 || one < 0);
    }

    ought(amiss);
}

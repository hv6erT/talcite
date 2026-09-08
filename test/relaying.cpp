#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "talcite/digest.hpp"
#include "talcite/peer.hpp"
#include "talcite/post.hpp"
#include "talcite/relay.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

#include "stunning.hpp"

using namespace talcite;

// Udawany przekaźnik składa odpowiedzi **bajt po bajcie**, nie moim kodekiem — inaczej sprawdzałbym
// się z samym sobą. Prawdziwy serwer TURN wymaga uwierzytelnienia, a to wymaga skrótów, których ta
// piaskownica nie daje; tutejszy przydziela bez poświadczeń, więc cała reszta rozmowy — przydział,
// pozwolenie, wysyłka, odbiór, odświeżenie — sprawdza się wszędzie.
namespace {

const net::Patience soon{4000};

void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

void minded(Bytes& into) {
    const std::uint16_t much = static_cast<std::uint16_t>(into.size() - 20);

    into[2] = static_cast<std::byte>(much >> 8);
    into[3] = static_cast<std::byte>(much & 0xFF);
}

Bytes headed(std::uint16_t kind, const Bytes& what) {
    Bytes made;

    pushed(made, kind);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    // Numer transakcji przepisujemy z zapytania — po nim i tylko po nim klient poznaje odpowiedź.
    for (std::size_t at = 8; at < 20; ++at) made.push_back(what[at]);

    return made;
}

void addressed(Bytes& made, std::uint16_t kind, const std::string& one, std::uint16_t port) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{1});

    pushed(value, static_cast<std::uint16_t>(port ^ 0x2112));

    std::uint32_t raw = 0;

    std::size_t at = 0;

    for (int which = 0; which < 4; ++which) {
        std::string part;

        while (at < one.size() && one[at] != '.') part += one[at++];

        ++at;

        raw = (raw << 8) | static_cast<std::uint32_t>(std::stoul(part));
    }

    raw ^= 0x2112A442u;

    for (int which = 0; which < 4; ++which)
        value.push_back(static_cast<std::byte>((raw >> (24 - 8 * which)) & 0xFF));

    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    minded(made);
}

void numbered(Bytes& made, std::uint16_t kind, std::uint32_t what) {
    pushed(made, kind);
    pushed(made, 4);

    pushed(made, static_cast<std::uint16_t>(what >> 16));
    pushed(made, static_cast<std::uint16_t>(what & 0xFFFF));

    minded(made);
}

void bearing(Bytes& made, const Bytes& what) {
    pushed(made, 0x0013);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const std::byte one : what) made.push_back(one);

    while (made.size() % 4 != 0) made.push_back(std::byte{0});

    minded(made);
}

std::string spelling(const Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

void bearing(Bytes& made, const std::string& what) { bearing(made, spelt(what)); }

net::Post standing() { return net::Post{net::Address::loopback(net::Family::four, 0)}; }

// Zapytanie i odpowiedź wiążąca składane **bajt po bajcie**, bo udają drugą stronę. Gdyby składał
// je mój kodek, sprawdzałbym się z samym sobą — a tu chodzi o to, czy rozmówca rozumie kogoś obcego.
Bytes knocking(const net::Number& number) {
    Bytes made;

    pushed(made, 0x0001);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte one : number) made.push_back(one);

    return made;
}

Bytes answering(const net::Number& number) {
    Bytes made;

    pushed(made, 0x0101);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// Oznajmienie z treścią od rozmówcy, czyli to, czym serwer podaje klientowi cudze bajty.
Bytes carrying(const net::Address& from, const Bytes& what) {
    Bytes made;

    pushed(made, 0x0017);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (int which = 0; which < 12; ++which) made.push_back(static_cast<std::byte>(which + 1));

    addressed(made, 0x0012, from.text(), from.port());
    bearing(made, what);

    return made;
}

// Numer kanału z zapytania wiążącego — wyłuskany z atrybutów **ręcznie**, bo `Message` go nie nosi.
std::uint16_t channelled(const Bytes& what) {
    std::size_t at = 20;

    while (at + 4 <= what.size()) {
        const std::uint16_t kind = static_cast<std::uint16_t>(
            (std::to_integer<unsigned>(what[at]) << 8) | std::to_integer<unsigned>(what[at + 1]));

        const std::uint16_t wide = static_cast<std::uint16_t>(
            (std::to_integer<unsigned>(what[at + 2]) << 8) |
            std::to_integer<unsigned>(what[at + 3]));

        if (kind == 0x000C && at + 6 <= what.size())
            return static_cast<std::uint16_t>((std::to_integer<unsigned>(what[at + 4]) << 8) |
                                              std::to_integer<unsigned>(what[at + 5]));

        at += 4u + wide + ((4u - wide % 4u) % 4u);
    }

    return 0;
}

// Treść w kanale, składana bajt po bajcie: numer, długość, treść. Żadnego nagłówka STUN.
Bytes channelling(std::uint16_t channel, const std::string& what) {
    Bytes made;

    pushed(made, channel);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const char one : what) made.push_back(static_cast<std::byte>(one));

    return made;
}

// Przydział na udawanym serwerze: przyjmuje zapytanie i oddaje użyczony adres. Oddaje przy okazji
// adres gniazda klienta, bo wszystko dalsze idzie właśnie tam.
net::Address allotted(net::Post& server, const net::Patience& patience) {
    const net::Parcel came = server.receive(1024, patience).await();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, patience).await();

    return came.from;
}

// Przekaźnik z otwartym przydziałem i **związanym kanałem** do podanego rozmówcy. Kanał zamawia się
// sam przy pierwszej wysyłce, więc tu tylko go potwierdzamy i zapamiętujemy numer.
struct Ready {
    net::Address whence;
    std::uint16_t channel = 0;
};

Ready readied(net::Relay& relay, net::Post& server, const net::Address& them) {
    Ready made;

    auto asking = relay.open(soon);

    made.whence = allotted(server, soon);

    (void)asking.await();

    relay.send(them, spelt("raz"), soon).await();

    for (int turn = 0; turn < 6 && made.channel == 0; ++turn) {
        const net::Parcel came = server.receive(1024, soon).await();

        net::Message told;

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::channel || told.sort != net::Sort::asking) continue;

        // Kanał wiąże się z adresem **razem z portem**, inaczej niż pozwolenie.
        same(told.peer, them);

        made.channel = channelled(came.bytes);

        server.send(made.whence, headed(0x0109, came.bytes), soon).await();
    }

    return made;
}

}

TRIAL("Przekaźnik: przydział oddaje użyczony adres") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Parcel came = server.receive(1024, soon).await();

    // Zapytanie musi być przydziałem i musi prosić o UDP.
    net::Message told;

    ought(net::understood(came.bytes, told));
    same(static_cast<int>(told.task), static_cast<int>(net::Task::allocate));
    same(static_cast<int>(told.sort), static_cast<int>(net::Sort::asking));

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    addressed(back, 0x0020, "198.51.100.9", 40404);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).await();

    const net::Address lent = asking.await();

    same(lent.text(), std::string{"203.0.113.5"});
    same(lent.port(), static_cast<std::uint16_t>(49152));
    same(relay.lent(), lent);
    same(relay.seen().text(), std::string{"198.51.100.9"});
}

TRIAL("Przekaźnik: odmowa przydziału wychodzi jako kłopot") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes back = headed(0x0113, came.bytes);

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{86});

    for (const char one : std::string{"Za duzo"}) value.push_back(static_cast<std::byte>(one));

    while (value.size() % 4 != 0) value.push_back(std::byte{0});

    pushed(back, 0x0009);
    pushed(back, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) back.push_back(one);

    minded(back);

    server.send(came.from, back, soon).await();

    bool amiss = false;

    try {
        (void)asking.await();
    } catch (const net::Trouble& why) {
        amiss = true;

        same(why.code(), 486);
    }

    ought(amiss);
}

// Serwer żądający poświadczeń odpowiada najpierw odmową z dziedziną i znacznikiem. Bez poświadczeń
// nie mamy z czego policzyć klucza i musimy powiedzieć to wprost, zamiast próbować bez końca.
TRIAL("Przekaźnik: żądanie poświadczeń bez poświadczeń kończy się wprost") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes back = headed(0x0113, came.bytes);

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{1});

    pushed(back, 0x0009);
    pushed(back, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) back.push_back(one);

    minded(back);

    pushed(back, 0x0014);
    pushed(back, 4);

    for (const char one : std::string{"dom "}) back.push_back(static_cast<std::byte>(one));

    minded(back);

    pushed(back, 0x0015);
    pushed(back, 4);

    for (const char one : std::string{"raz "}) back.push_back(static_cast<std::byte>(one));

    minded(back);

    server.send(came.from, back, soon).await();

    bool amiss = false;

    try {
        (void)asking.await();
    } catch (const net::Trouble& why) {
        amiss = true;

        same(why.code(), 401);
    }

    ought(amiss);
}

TRIAL("Przekaźnik: cisza serwera kończy się terminem") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    bool late = false;

    try {
        (void)relay.open(net::Patience{500}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Przekaźnik: pozwolenie idzie z adresem rozmówcy") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    net::Parcel came = server.receive(1024, soon).await();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).await();

    (void)asking.await();

    auto second = relay.allow(net::Address{"198.51.100.77", 5555}, soon);

    came = server.receive(1024, soon).await();

    net::Message told;

    ought(net::understood(came.bytes, told));
    same(static_cast<int>(told.task), static_cast<int>(net::Task::permission));
    same(told.peer.text(), std::string{"198.51.100.77"});
    same(told.peer.port(), static_cast<std::uint16_t>(5555));

    server.send(came.from, headed(0x0108, came.bytes), soon).await();

    second.await();
}

TRIAL("Przekaźnik: wysyłka idzie oznajmieniem z treścią") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    net::Parcel came = server.receive(1024, soon).await();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).await();

    (void)asking.await();

    relay.send(net::Address{"198.51.100.77", 5555}, spelt("przez przekaźnik"), soon).await();

    // Pierwsza wysyłka idzie oznajmieniem, a **obok niej** leci zamówienie kanału — kolejność
    // dwóch datagramów nie jest niczyją obietnicą, więc szukamy tego, o który chodzi.
    net::Message told;

    bool sent = false;

    for (int turn = 0; turn < 6 && !sent; ++turn) {
        came = server.receive(1024, soon).await();

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::sending) continue;

        same(static_cast<int>(told.sort), static_cast<int>(net::Sort::telling));
        same(told.peer.text(), std::string{"198.51.100.77"});
        same(spelling(told.data), std::string{"przez przekaźnik"});

        sent = true;
    }

    ought(sent);
}

TRIAL("Przekaźnik: treść od rozmówcy przychodzi z jego adresem") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).await();

    (void)asking.await();

    auto waiting = relay.receive(1024, soon);

    Bytes data = headed(0x0017, came.bytes);

    addressed(data, 0x0012, "198.51.100.77", 5555);
    bearing(data, "od obcego");

    server.send(came.from, data, soon).await();

    const net::Parcel given = waiting.await();

    same(given.from.text(), std::string{"198.51.100.77"});
    same(given.from.port(), static_cast<std::uint16_t>(5555));
    same(spelling(given.bytes), std::string{"od obcego"});
}

// Odpowiedź przychodzi **tylko od serwera**. Cudzy datagram na tym gnieździe nie ma prawa ani
// udawać odpowiedzi, ani podać się za treść od rozmówcy.
TRIAL("Przekaźnik: obcy datagram nie udaje przekaźnika") {
    net::Post mine = standing();
    net::Post server = standing();
    net::Post evil = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(net::Patience{1500});

    const net::Parcel came = server.receive(1024, soon).await();

    // Obcy podszywa się pod przydział, z prawidłowym numerem transakcji.
    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "1.2.3.4", 1);
    numbered(back, 0x000D, 600);

    evil.send(mine.mine(), back, soon).await();

    bool late = false;

    try {
        (void)asking.await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
    ought(!relay.lent().held());
}

TRIAL("Przekaźnik: pusty nie robi niczego po cichu") {
    net::Relay relay;

    bool amiss = false;

    try {
        (void)relay.open(soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Przekaźnik: za duża przesyłka nie ginie przy odmowie") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Parcel came = server.receive(1024, soon).await();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).await();

    (void)asking.await();

    Bytes data = headed(0x0017, came.bytes);

    addressed(data, 0x0012, "198.51.100.77", 5555);
    bearing(data, "dłuższe niż cztery bajty");

    server.send(came.from, data, soon).await();

    bool amiss = false;

    for (int turn = 0; turn < 40 && !amiss; ++turn) {
        try {
            (void)relay.receive(4, net::Patience{100}).await();
        } catch (const Overdue&) {
            continue;
        } catch (const net::Trouble&) {
            amiss = true;
        }
    }

    ought(amiss);

    same(spelling(relay.receive(1024, soon).await().bytes),
         std::string{"dłuższe niż cztery bajty"});
}

// Rozmówca z przekaźnikiem. Prawdziwego NAT-u tu nie ma, więc sprawdza się **rozmowa**: czy
// użyczony adres wychodzi do sygnalizacji, czy pozwolenia lecą po adresie bez portu, czy odpowiedź
// wraca tym kanałem, którym przyszła próba, i czy dane potem idą tamtędy, a nie wprost.
TRIAL("Rozmówca: przekaźnik dokłada użyczoną drogę do własnych kandydatów") {
    net::Post server = standing();

    net::Peer left{standing(), net::Relay{standing(), server.mine()}};

    auto gathering = left.gather(net::Scout{}, soon);

    (void)allotted(server, soon);

    const std::vector<net::Way> ours = gathering.await();

    bool lent = false;

    for (const net::Way& one : ours)
        if (one.sort == net::Way::Sort::lent) {
            lent = true;

            same(one.where.text(), std::string{"203.0.113.5"});
            same(one.where.port(), static_cast<std::uint16_t>(49152));
        }

    ought(lent);

    // Droga użyczona stoi **na końcu**: własna karta przed odbiciem, odbicie przed przekaźnikiem.
    same(ours.back().sort, net::Way::Sort::lent);

    same(left.ways().size(), ours.size());
}

TRIAL("Rozmówca: bez przekaźnika nie ma użyczonej drogi ani rozmowy przez przekaźnik") {
    net::Peer left{standing()};

    for (const net::Way& one : left.gather(net::Scout{}, soon).await())
        ought(one.sort != net::Way::Sort::lent);

    ought(!left.relayed());
}

TRIAL("Rozmówca: pozwolenie idzie po adresie, bez portu") {
    net::Post server = standing();

    net::Peer left{standing(), net::Relay{standing(), server.mine()}};

    auto gathering = left.gather(net::Scout{}, soon);

    (void)allotted(server, soon);

    (void)gathering.await();

    left.add(net::Way{net::Address{"198.51.100.77", 5555}, net::Way::Sort::here});

    auto reaching = left.reach(net::Patience{3000});

    const net::Parcel came = server.receive(1024, soon).await();

    net::Message told;

    ought(net::understood(came.bytes, told));
    same(static_cast<int>(told.task), static_cast<int>(net::Task::permission));
    same(told.peer.text(), std::string{"198.51.100.77"});

    // Pozwolenie sięga adresu, a nie portu — port, który NAT nada drugiej stronie, i tak będzie
    // inny niż ten z sygnalizacji.
    same(told.peer.port(), static_cast<std::uint16_t>(0));

    server.send(came.from, headed(0x0108, came.bytes), soon).await();

    try {
        (void)reaching.await();
    } catch (const std::exception&) {
    }
}

TRIAL("Rozmówca: przez przekaźnik odpowiada tym samym kanałem i nim rozmawia") {
    net::Post server = standing();

    net::Peer left{standing(), net::Relay{standing(), server.mine()}};

    auto gathering = left.gather(net::Scout{}, soon);

    const net::Address whence = allotted(server, soon);

    (void)gathering.await();

    const net::Address them{"198.51.100.77", 5555};

    left.add(net::Way{them, net::Way::Sort::here});

    auto reaching = left.reach(net::Patience{10000});

    // Pozwolenie dla drugiej strony.
    net::Parcel came = server.receive(1024, soon).await();

    net::Message told;

    ought(net::understood(came.bytes, told));
    same(static_cast<int>(told.task), static_cast<int>(net::Task::permission));

    server.send(whence, headed(0x0108, came.bytes), soon).await();

    // Druga strona dobija się **pod użyczony adres**, więc jej próba przychodzi przekaźnikiem.
    net::Number theirs{};

    for (int which = 0; which < 12; ++which) theirs[which] = static_cast<std::byte>(100 + which);

    server.send(whence, carrying(them, knocking(theirs)), soon).await();

    // Odpowiedź musi wyjść **tym samym kanałem**, a razem z nią nasza własna próba.
    bool answered = false;

    bool asked = false;

    net::Number number{};

    for (int turn = 0; turn < 8 && !(answered && asked); ++turn) {
        came = server.receive(1024, soon).await();

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::sending) continue;

        same(told.peer, them);

        net::Message inside;

        if (!net::understood(told.data, inside)) continue;

        if (inside.sort == net::Sort::told) {
            answered = true;

            same(inside.number, theirs);
        }

        if (inside.sort == net::Sort::asking) {
            asked = true;

            number = inside.number;
        }
    }

    ought(answered);
    ought(asked);

    // Ich odpowiedź na naszą próbę — też przekaźnikiem — kończy przebijanie.
    server.send(whence, carrying(them, answering(number)), soon).await();

    same(reaching.await(), them);

    same(left.met(), them);
    ought(left.relayed());

    // Od tej chwili mówimy przekaźnikiem, a nie wprost.
    auto waiting = left.receive(1024, soon);

    left.send(spelt("przez cudze pasmo"), soon).await();

    bool sent = false;

    for (int turn = 0; turn < 8 && !sent; ++turn) {
        came = server.receive(1024, soon).await();

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::sending) continue;

        if (spelling(told.data) == "przez cudze pasmo") sent = true;
    }

    ought(sent);

    server.send(whence, carrying(them, spelt("z drugiej strony")), soon).await();

    same(spelling(waiting.await()), std::string{"z drugiej strony"});
}

TRIAL("Rozmówca: pusty nie wywraca się przy pytaniach") {
    net::Peer left;

    ought(left.ways().empty());
    ought(left.theirs().empty());
    ought(!left.met().held());
    ought(!left.relayed());

    left.add(net::Way{net::Address{"203.0.113.9", 5000}, net::Way::Sort::here});

    ought(left.theirs().empty());
}

TRIAL("Kanał: główkę poznaje się po dwóch bitach, nie po zgadywaniu") {
    std::uint16_t channel = 0;

    Bytes data;

    // Wiadomość STUN ma te bity zerowe i kanałem nie jest.
    ought(!net::unwrapped(net::asked(net::numbered()), channel, data));

    // Za krótkie na samą główkę.
    ought(!net::unwrapped(Bytes{std::byte{0x40}, std::byte{0x01}}, channel, data));

    // Ucięta treść: długość obiecuje więcej, niż przyszło.
    Bytes cut = net::wrapped(0x4001, spelt("cztery"));

    cut.pop_back();

    ought(!net::unwrapped(cut, channel, data));

    const Bytes whole = net::wrapped(0x4001, spelt("cztery"));

    // Cztery bajty główki i ani jednego dopełnienia — po UDP datagram sam wyznacza koniec.
    same(whole.size(), static_cast<std::size_t>(10));

    ought(net::unwrapped(whole, channel, data));

    same(channel, static_cast<std::uint16_t>(0x4001));
    same(spelling(data), std::string{"cztery"});
}

TRIAL("Przekaźnik: kanał zamawia się sam, a potem główka ma cztery bajty") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    const net::Address them{"198.51.100.77", 5555};

    const Ready ready = readied(relay, server, them);

    ought(ready.channel >= 0x4000);
    ought(ready.channel <= 0x7FFF);

    bool tight = false;

    for (int turn = 0; turn < 40 && !tight; ++turn) {
        relay.send(them, spelt("dwa"), soon).await();

        try {
            const net::Parcel came = server.receive(1024, net::Patience{200}).await();

            std::uint16_t channel = 0;

            Bytes data;

            if (!net::unwrapped(came.bytes, channel, data)) continue;

            same(channel, ready.channel);
            same(spelling(data), std::string{"dwa"});

            // Trzy bajty treści i cztery główki — nie trzydzieści dziewięć.
            same(came.bytes.size(), static_cast<std::size_t>(7));

            tight = true;
        } catch (const Overdue&) {
        }
    }

    ought(tight);
}

TRIAL("Przekaźnik: treść z kanału przychodzi z adresem rozmówcy") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    const net::Address them{"198.51.100.77", 5555};

    const Ready ready = readied(relay, server, them);

    auto waiting = relay.receive(1024, soon);

    server.send(ready.whence, channelling(ready.channel, "z kanału"), soon).await();

    const net::Parcel given = waiting.await();

    same(given.from, them);
    same(spelling(given.bytes), std::string{"z kanału"});
}

// Numer kanału nadajemy my, więc obcy numer nie ma prawa niczego znaczyć — nie wiadomo nawet, komu
// miałby należeć nadawca.
TRIAL("Przekaźnik: obcy numer kanału nie wchodzi do rozmowy") {
    net::Post mine = standing();
    net::Post server = standing();

    net::Relay relay{mine, server.mine()};

    const net::Address them{"198.51.100.77", 5555};

    const Ready ready = readied(relay, server, them);

    server.send(ready.whence, channelling(static_cast<std::uint16_t>(ready.channel + 1), "obce"),
                soon)
        .await();

    bool late = false;

    try {
        (void)relay.receive(1024, net::Patience{600}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

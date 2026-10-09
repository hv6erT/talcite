#include "proving.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "exchange.hpp"
#include "talcite/listener.hpp"
#include "talcite/udp.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"

using namespace talcite;

// Te przypadki celują w miejsca, w których zaplecza najbardziej się różnią: przyjmowanie
// połączeń, nawiązywanie, dwie operacje naraz na jednym gnieździe, przerwanie jednej z nich
// i pisanie większe niż bufor jądra. Na uniksach sprawdzają poll i epoll, na Windows — IOCP,
// gdzie każda z tych rzeczy jest zupełnie innym wywołaniem systemowym.
namespace {

const net::Patience soon{15000};



net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

struct Pair {
    net::Stream server;
    net::Stream client;
};

Pair joined(const net::Listener& gate) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Obciążenie: pięćdziesiąt połączeń po kolei") {
    const net::Listener gate = standing();

    for (int which = 0; which < 50; ++which) {
        Pair both = joined(gate);

        both.client.write(bytes(std::to_string(which)), soon).await();

        same(text(both.server.read(16, soon).await()), std::to_string(which));
    }
}

TRIAL("Obciążenie: pięćdziesiąt połączeń naraz") {
    const net::Listener gate = standing();

    std::vector<net::Stream> servers;
    std::vector<net::Stream> clients;

    for (int which = 0; which < 50; ++which) {
        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(gate.mine(), soon);

        servers.push_back(meeting.await());
        clients.push_back(joining.await());
    }

    std::vector<Promise<Bytes>> reading;

    for (net::Stream& one : servers) reading.push_back(one.read(16, soon));

    for (int which = 0; which < 50; ++which)
        clients[static_cast<std::size_t>(which)].write(bytes(std::to_string(which)), soon).await();

    for (int which = 0; which < 50; ++which)
        same(text(reading[static_cast<std::size_t>(which)].await()), std::to_string(which));
}

TRIAL("Obciążenie: czytanie i pisanie naraz na jednym strumieniu") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    auto reading = both.client.read(64, soon);

    both.server.write(bytes("z drugiej strony"), soon).await();
    both.client.write(bytes("i z tej"), soon).await();

    same(text(reading.await()), std::string{"z drugiej strony"});
    same(text(both.server.read(64, soon).await()), std::string{"i z tej"});
}

// Najważniejszy przypadek dla IOCP: przerwanie **jednej** operacji, a nie całego gniazda.
// Na jednym strumieniu wisi czytanie z terminem i pisanie bez — po upływie terminu czytanie ma
// paść, a pisanie dojść do końca.
TRIAL("Obciążenie: termin przerywa jedną operację, nie gniazdo") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::string much(400000, 'x');

    auto writing = both.client.write(bytes(much), soon);
    auto reading = both.client.read(64, net::Patience{200});

    bool late = false;

    try {
        (void)reading.await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).await();

        if (one.empty()) break;

        made += text(one);
    }

    writing.await();

    same(made.size(), much.size());

    // Strumień żyje dalej: przerwane czytanie nie zabrało ze sobą połączenia.
    both.server.write(bytes("dalej stoję"), soon).await();

    same(text(both.client.read(64, soon).await()), std::string{"dalej stoję"});
}

TRIAL("Obciążenie: pusty zapis nic nie psuje") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(Bytes{}, soon).await();
    both.client.write(bytes("po pustym"), soon).await();

    same(text(both.server.read(64, soon).await()), std::string{"po pustym"});
}

TRIAL("Obciążenie: czytanie mniejsze niż to, co przyszło") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(bytes("abcdefgh"), soon).await();

    same(text(both.server.read(3, soon).await()), std::string{"abc"});
    same(text(both.server.read(3, soon).await()), std::string{"def"});
    same(text(both.server.read(3, soon).await()), std::string{"gh"});
}

TRIAL("Obciążenie: zamknięcie przerywa obie strony naraz") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::string much(400000, 'y');

    auto writing = both.client.write(bytes(much), soon);
    auto reading = both.client.read(64, soon);

    both.client.close();

    int amiss = 0;

    try {
        (void)reading.await();
    } catch (const std::exception&) {
        ++amiss;
    }

    try {
        writing.await();
    } catch (const std::exception&) {
        ++amiss;
    }

    // Pisanie mogło zdążyć zejść do bufora jądra przed zamknięciem, więc wymagamy tylko, żeby
    // czytanie padło — i żeby nic nie zawisło.
    ought(amiss >= 1);
}

TRIAL("Obciążenie: brama zamknięta w trakcie czekania na połączenie") {
    net::Listener gate = standing();

    auto meeting = gate.accept(soon);

    gate.close();

    bool amiss = false;

    try {
        (void)meeting.await();
    } catch (const std::exception&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Obciążenie: nawiązanie do martwego portu pada, a nie wisi") {
    net::Address where;

    {
        const net::Listener gate = standing();

        where = gate.mine();
    }

    bool amiss = false;

    try {
        (void)net::Stream::connect(where, net::Patience{3000}).await();
    } catch (const std::exception&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Obciążenie: dwadzieścia datagramów pod rząd") {
    const net::Udp here{net::Address::loopback(net::Family::four, 0)};
    const net::Udp there{net::Address::loopback(net::Family::four, 0)};

    for (int which = 0; which < 20; ++which) {
        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), bytes(std::to_string(which)), soon).await();

        same(text(waiting.await().bytes), std::to_string(which));
    }
}

TRIAL("Obciążenie: po wszystkim centrala nie trzyma gniazd") {
    {
        const net::Listener gate = standing();

        for (int which = 0; which < 5; ++which) {
            Pair both = joined(gate);

            both.client.write(bytes("raz"), soon).await();

            (void)both.server.read(16, soon).await();
        }
    }

    for (int tried = 0; tried < 400 && net::Exchange::running().waiting() > 0; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();

    same(static_cast<int>(net::Exchange::running().waiting()), 0);
}

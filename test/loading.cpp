#include "proving.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "talcite/exchange.hpp"
#include "talcite/gate.hpp"
#include "talcite/post.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

// Te przypadki celują w miejsca, w których zaplecza najbardziej się różnią: przyjmowanie
// połączeń, nawiązywanie, dwie operacje naraz na jednym gnieździe, przerwanie jednej z nich
// i pisanie większe niż bufor jądra. Na uniksach sprawdzają poll i epoll, na Windows — IOCP,
// gdzie każda z tych rzeczy jest zupełnie innym wywołaniem systemowym.
namespace {

const net::Patience soon{15000};

Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

std::string read(const Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

net::Gate standing() { return net::Gate{net::Address::loopback(net::Family::four, 0)}; }

struct Pair {
    net::Stream server;
    net::Stream client;
};

Pair joined(const net::Gate& gate) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Obciążenie: pięćdziesiąt połączeń po kolei") {
    const net::Gate gate = standing();

    for (int which = 0; which < 50; ++which) {
        Pair both = joined(gate);

        both.client.write(spelt(std::to_string(which)), soon).await();

        same(read(both.server.read(16, soon).await()), std::to_string(which));
    }
}

TRIAL("Obciążenie: pięćdziesiąt połączeń naraz") {
    const net::Gate gate = standing();

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
        clients[static_cast<std::size_t>(which)].write(spelt(std::to_string(which)), soon).await();

    for (int which = 0; which < 50; ++which)
        same(read(reading[static_cast<std::size_t>(which)].await()), std::to_string(which));
}

TRIAL("Obciążenie: czytanie i pisanie naraz na jednym strumieniu") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    auto reading = both.client.read(64, soon);

    both.server.write(spelt("z drugiej strony"), soon).await();
    both.client.write(spelt("i z tej"), soon).await();

    same(read(reading.await()), std::string{"z drugiej strony"});
    same(read(both.server.read(64, soon).await()), std::string{"i z tej"});
}

// Najważniejszy przypadek dla IOCP: przerwanie **jednej** operacji, a nie całego gniazda.
// Na jednym strumieniu wisi czytanie z terminem i pisanie bez — po upływie terminu czytanie ma
// paść, a pisanie dojść do końca.
TRIAL("Obciążenie: termin przerywa jedną operację, nie gniazdo") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    const std::string much(400000, 'x');

    auto writing = both.client.write(spelt(much), soon);
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

        made += read(one);
    }

    writing.await();

    same(made.size(), much.size());

    // Strumień żyje dalej: przerwane czytanie nie zabrało ze sobą połączenia.
    both.server.write(spelt("dalej stoję"), soon).await();

    same(read(both.client.read(64, soon).await()), std::string{"dalej stoję"});
}

TRIAL("Obciążenie: pusty zapis nic nie psuje") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    both.client.write(Bytes{}, soon).await();
    both.client.write(spelt("po pustym"), soon).await();

    same(read(both.server.read(64, soon).await()), std::string{"po pustym"});
}

TRIAL("Obciążenie: czytanie mniejsze niż to, co przyszło") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    both.client.write(spelt("abcdefgh"), soon).await();

    same(read(both.server.read(3, soon).await()), std::string{"abc"});
    same(read(both.server.read(3, soon).await()), std::string{"def"});
    same(read(both.server.read(3, soon).await()), std::string{"gh"});
}

TRIAL("Obciążenie: zamknięcie przerywa obie strony naraz") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    const std::string much(400000, 'y');

    auto writing = both.client.write(spelt(much), soon);
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
    net::Gate gate = standing();

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
        const net::Gate gate = standing();

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
    const net::Post here{net::Address::loopback(net::Family::four, 0)};
    const net::Post there{net::Address::loopback(net::Family::four, 0)};

    for (int which = 0; which < 20; ++which) {
        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), spelt(std::to_string(which)), soon).await();

        same(read(waiting.await().bytes), std::to_string(which));
    }
}

TRIAL("Obciążenie: po wszystkim centrala nie trzyma gniazd") {
    {
        const net::Gate gate = standing();

        for (int which = 0; which < 5; ++which) {
            Pair both = joined(gate);

            both.client.write(spelt("raz"), soon).await();

            (void)both.server.read(16, soon).await();
        }
    }

    for (int tried = 0; tried < 400 && net::Exchange::running().waiting() > 0; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();

    same(static_cast<int>(net::Exchange::running().waiting()), 0);
}

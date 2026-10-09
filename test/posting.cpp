#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <string>

#include "talcite/address.hpp"
#include "talcite/udp.hpp"
#include "talcite/time.hpp"

using namespace talcite;

namespace {

const net::Patience soon{4000};



net::Udp standing() { return net::Udp{net::Address::loopback(net::Family::four, 0)}; }

}

TRIAL("Poczta: dostaje port od systemu") {
    const net::Udp one = standing();

    ought(one.held());
    ought(one.mine().port() != 0);
    same(one.mine().text(), std::string{"127.0.0.1"});
}

TRIAL("Poczta: pusty adres odrzuca") {
    bool threw = false;

    try {
        const net::Udp one{net::Address{}};

        (void)one;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Poczta: datagram dochodzi i wie, od kogo") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(1024, soon);

    there.send(here.mine(), bytes("kartka"), soon).await();

    const net::Datagram made = waiting.await();

    same(text(made.bytes), std::string{"kartka"});
    same(made.from.port(), there.mine().port());
    same(made.from.text(), std::string{"127.0.0.1"});
}

TRIAL("Poczta: pusty datagram to nie to samo, co brak datagramu") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(1024, soon);

    there.send(here.mine(), Bytes{}, soon).await();

    const net::Datagram made = waiting.await();

    same(static_cast<int>(made.bytes.size()), 0);
    ought(made.from.held());
}

TRIAL("Poczta: obcięty datagram zgłasza kłopot, a nie kawałek") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(4, soon);

    there.send(here.mine(), bytes("dużo dłuższa treść"), soon).await();

    bool cut = false;

    try {
        (void)waiting.await();
    } catch (const net::Trouble& why) {
        cut = true;

        ought(std::string{why.what()}.find("buffer") != std::string::npos);
    }

    ought(cut);
}

TRIAL("Poczta: cisza kończy się terminem") {
    const net::Udp here = standing();

    bool late = false;

    try {
        (void)here.receive(1024, net::Patience{80}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Poczta: drugie odbieranie naraz dostaje odmowę") {
    const net::Udp here = standing();

    auto first = here.receive(1024, net::Patience{300});

    bool busy = false;

    try {
        (void)here.receive(1024, net::Patience{300}).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const Overdue&) {
    }
}

TRIAL("Poczta: pusty adres nie jest celem") {
    const net::Udp here = standing();

    bool amiss = false;

    try {
        here.send(net::Address{}, bytes("nigdzie"), soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Poczta: kilka datagramów po kolei zachowuje kolejność na pętli") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    for (int which = 0; which < 5; ++which) {
        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), bytes(std::to_string(which)), soon).await();

        same(text(waiting.await().bytes), std::to_string(which));
    }
}

TRIAL("Poczta: odpowiedź wraca pod adres nadawcy") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(1024, soon);

    there.send(here.mine(), bytes("pytanie"), soon).await();

    const net::Datagram made = waiting.await();

    auto back = there.receive(1024, soon);

    here.send(made.from, bytes("odpowiedź"), soon).await();

    same(text(back.await().bytes), std::string{"odpowiedź"});
}

TRIAL("Poczta: zamknięta nie przyjmuje zgłoszeń") {
    net::Udp here = standing();

    here.close();

    bool amiss = false;

    try {
        (void)here.receive(8, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Poczta: zamknięcie przerywa czekające odbieranie") {
    net::Udp here = standing();

    auto waiting = here.receive(1024, soon);

    here.close();

    bool amiss = false;

    try {
        (void)waiting.await();
    } catch (const net::Trouble&) {
        amiss = true;
    } catch (const Overdue&) {
        amiss = true;
    }

    ought(amiss);
}

// Poczta po zamknięciu odpowiada grzecznie na wszystko — również na pytanie o własny adres.
TRIAL("Poczta: adres po zamknięciu jest pusty, a nie wybucha") {
    net::Udp one{net::Address::loopback(net::Family::four, 0)};

    one.close();

    ought(!one.mine().held());
}

// Anulowane odbieranie zostawiało gniazdo poczty zajęte na zawsze — ta sama wada, co
// w strumieniu, bo mechanizm jest jeden.
TRIAL("Poczta: anulowane odbieranie zwalnia gniazdo") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto first = here.receive(1024, net::Patience{0});

    first.cancel();

    bool quit = false;

    try {
        (void)first.await();
    } catch (const Cancelled&) {
        quit = true;
    }

    ought(quit);

    there.send(here.mine(), bytes("paczka"), soon).await();

    for (int tried = 0; tried < 200; ++tried) {
        try {
            same(text(here.receive(1024, soon).await().bytes), std::string{"paczka"});

            return;
        } catch (const net::Trouble&) {
            Time::later(std::chrono::milliseconds{5}).await();
        }
    }

    ought(false);
}

// **Anulowanie zwalnia kierunek od razu** — ta sama reguła i ten sam powód co przy bramie
// i strumieniu, opisane w `streaming.cpp`. Bez odczekiwania pośrodku, bo to ono maskowało usterkę.
TRIAL("Poczta: po anulowaniu można od razu odbierać znowu") {
    const net::Udp one = standing();
    const net::Udp other = standing();

    auto first = one.receive(64, net::Patience{0});

    first.cancel();

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }

    auto waiting = one.receive(64, soon);

    other.send(one.mine(), bytes("znowu"), soon).await();

    same(text(waiting.await().bytes), std::string{"znowu"});
}

TRIAL("Poczta: sto anulowań pod rząd nie zajmuje jej na stałe") {
    const net::Udp one = standing();
    const net::Udp other = standing();

    for (int again = 0; again < 100; ++again) {
        auto waiting = one.receive(64, net::Patience{0});

        waiting.cancel();

        try {
            (void)waiting.await();
        } catch (const std::exception&) {
        }
    }

    auto waiting = one.receive(64, soon);

    other.send(one.mine(), bytes("po stu"), soon).await();

    same(text(waiting.await().bytes), std::string{"po stu"});
}

TRIAL("Poczta: gniazdo lokalne odmawia, bo datagramów tam nie ma") {
    bool told = false;

    try {
        const net::Udp one{net::Address::local("/tmp/talcite-poczta.sock")};
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "talcite/exchange.hpp"
#include "talcite/fs.hpp"
#include "talcite/gate.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

namespace {

const net::Patience soon{4000};

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

// Brama na pętli zwrotnej z portem od systemu — dwa testy naraz nigdy się wtedy nie pobiją
// o ten sam numer.
net::Gate standing() { return net::Gate{net::Address::loopback(net::Family::four, 0)}; }

// Obie strony ruszają od razu, bo obietnica sieciowa zaczyna się w konstruktorze. Dopiero
// potem czekamy — inaczej czekanie na przyjęcie zatrzymałoby nawiązanie, które ma je wywołać.
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

TRIAL("Brama: dostaje port od systemu") {
    const net::Gate gate = standing();

    ought(gate.held());
    ought(gate.mine().port() != 0);
    same(gate.mine().text(), std::string{"127.0.0.1"});
}

TRIAL("Brama: pusty adres odrzuca") {
    bool threw = false;

    try {
        const net::Gate gate{net::Address{}};

        (void)gate;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Strumień: nawiązanie i przyjęcie spotykają się") {
    const net::Gate gate = standing();

    const Pair both = joined(gate);

    ought(both.server.held());
    ought(both.client.held());

    same(both.client.peer().port(), gate.mine().port());
    same(both.server.mine().port(), gate.mine().port());
    same(both.server.peer().port(), both.client.mine().port());
}

TRIAL("Strumień: bajty przechodzą w obie strony") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    both.client.write(spelt("witaj"), soon).await();

    same(read(both.server.read(64, soon).await()), std::string{"witaj"});

    both.server.write(spelt("i wzajemnie"), soon).await();

    same(read(both.client.read(64, soon).await()), std::string{"i wzajemnie"});
}

TRIAL("Strumień: większa paczka schodzi w całości") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    const std::string much(200000, 'x');

    auto writing = both.client.write(spelt(much), soon);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).await();

        if (one.empty()) break;

        made += read(one);
    }

    writing.await();

    same(made.size(), much.size());
    ought(made == much);
}

TRIAL("Strumień: zamknięcie drugiej strony daje pustkę") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    both.client.close();

    same(static_cast<int>(both.server.read(64, soon).await().size()), 0);
}

TRIAL("Strumień: cisza kończy się terminem, a nie pustką") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    bool late = false;

    try {
        (void)both.server.read(64, net::Patience{80}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Strumień: drugie czytanie naraz dostaje odmowę") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    auto first = both.server.read(64, net::Patience{300});

    bool busy = false;

    try {
        (void)both.server.read(64, net::Patience{300}).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const Overdue&) {
    }
}

TRIAL("Strumień: po czytaniu można czytać znowu") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    both.client.write(spelt("raz"), soon).await();

    same(read(both.server.read(64, soon).await()), std::string{"raz"});

    both.client.write(spelt("dwa"), soon).await();

    same(read(both.server.read(64, soon).await()), std::string{"dwa"});
}

TRIAL("Strumień: zamknięcie przerywa czekające czytanie") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    auto reading = both.server.read(64, soon);

    both.server.close();

    bool amiss = false;

    try {
        (void)reading.await();
    } catch (const net::Trouble&) {
        amiss = true;
    } catch (const Overdue&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Strumień: nawiązanie do zamkniętego portu odrzuca") {
    net::Address where;

    {
        const net::Gate gate = standing();

        where = gate.mine();
    }

    bool amiss = false;

    try {
        (void)net::Stream::connect(where, net::Patience{1000}).await();
    } catch (const net::Trouble&) {
        amiss = true;
    } catch (const Overdue&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Strumień: pusty adres nie jest celem") {
    bool amiss = false;

    try {
        (void)net::Stream::connect(net::Address{}, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Strumień: zamknięty nie przyjmuje zgłoszeń") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    both.client.close();

    bool amiss = false;

    try {
        (void)both.client.read(8, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Brama: przyjmuje kilka połączeń po kolei") {
    const net::Gate gate = standing();

    for (int which = 0; which < 3; ++which) {
        Pair both = joined(gate);

        both.client.write(spelt(std::to_string(which)), soon).await();

        same(read(both.server.read(8, soon).await()), std::to_string(which));
    }
}

TRIAL("Centrala: po wszystkim nikt nie czeka") {
    const net::Gate gate = standing();

    {
        Pair both = joined(gate);

        both.client.write(spelt("hej"), soon).await();

        (void)both.server.read(8, soon).await();
    }

    for (int tried = 0; tried < 200 && net::Exchange::running().waiting() > 1; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();

    ought(net::Exchange::running().waiting() <= 1);
}

// Centrala trzyma jedno zgłoszenie na gniazdo na kierunek, więc drugie przyjmowanie nadpisywało
// pierwsze: obietnica nie rozstrzygała się nigdy, termin przestawał istnieć, a brama i jej
// deskryptor zostawały do końca programu.
TRIAL("Brama: drugie przyjmowanie dostaje odmowę, a pierwsze żyje") {
    const net::Gate gate{net::Address::loopback(net::Family::four, 0)};

    auto first = gate.accept(net::Patience{3000});

    bool busy = false;

    try {
        (void)gate.accept(net::Patience{3000}).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    const net::Stream client = net::Stream::connect(gate.mine(), net::Patience{3000}).await();

    const net::Stream server = first.await();

    same(server.peer(), client.mine());
}

// Rozmiar podaje wołający, więc przydział bufora potrafi rzucić. Wyjątek rzucony po zajęciu
// znacznika zostawiał go ustawionym na zawsze i gniazdo już nigdy nie czytało.
TRIAL("Strumień: wyjątek z przydziału nie blokuje czytania na zawsze") {
    const net::Gate gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(net::Patience{3000});

    const net::Stream client = net::Stream::connect(gate.mine(), net::Patience{3000}).await();
    const net::Stream server = waiting.await();

    bool threw = false;

    try {
        (void)server.read(static_cast<std::size_t>(-1), net::Patience{1000});
    } catch (const std::exception&) {
        threw = true;
    }

    ought(threw);

    client.write(spelt("po wyjątku"), net::Patience{3000}).await();

    same(read(server.read(64, net::Patience{3000}).await()), std::string{"po wyjątku"});
}

// Zerowe czytanie wyglądało jak koniec strumienia: recv z zerową długością oddaje zero także na
// zdrowym gnieździe, a zero bez błędu znaczy u nas „druga strona zamknęła".
TRIAL("Strumień: zerowe czytanie nie udaje końca strumienia") {
    const net::Gate gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(soon);

    const net::Stream client = net::Stream::connect(gate.mine(), soon).await();
    const net::Stream server = waiting.await();

    client.write(spelt("osiem!!!"), soon).await();

    bool amiss = false;

    try {
        (void)server.read(0, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);

    same(read(server.read(64, soon).await()), std::string{"osiem!!!"});
}

// Po zamknięciu wszystkie metody odpowiadają grzecznie — te też muszą, zamiast naruszać pamięć.
TRIAL("Strumień: adresy po zamknięciu są puste, a nie wybuchają") {
    const net::Gate gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(soon);

    net::Stream client = net::Stream::connect(gate.mine(), soon).await();

    const net::Stream server = waiting.await();

    client.close();

    ought(!client.peer().held());
    ought(!client.mine().held());

    net::Gate other{net::Address::loopback(net::Family::four, 0)};

    other.close();

    ought(!other.mine().held());
}

// --- Anulowanie -------------------------------------------------------------------------------
//
// Zgłoszenie sieciowe jest jednocześnie biletem obietnicy, więc `cancel()` musiał je zdjąć
// z centrali, a nie tylko odrzucić. Zanim to zrobił, przerwane czytanie zostawiało kierunek
// gniazda zajęty **na zawsze**: wołający przestawał czekać, a strumień do końca programu
// odpowiadał „zajęty" i przez `kept` trzymał otwarty deskryptor.

namespace {

// Na uniksach kierunek zwalnia się w samym `cancel()`. Na Windows dopiero wtedy, gdy przerwane
// zgłoszenie wróci z portu zakończeń — dlatego próbujemy chwilę, zamiast zakładać jedno z dwojga.
Bytes patiently(const net::Stream& one, std::size_t most) {
    for (int tried = 0; tried < 200; ++tried) {
        auto next = one.read(most, soon);

        try {
            return next.await();
        } catch (const net::Trouble&) {
            Time::later(std::chrono::milliseconds{5}).await();
        }
    }

    return Bytes{};
}

void settling(std::size_t below) {
    for (int tried = 0; tried < 200 && net::Exchange::running().waiting() > below; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();
}

}

TRIAL("Strumień: anulowane czytanie zwalnia kierunek") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    auto first = both.server.read(64, net::Patience{0});

    first.cancel();

    bool quit = false;

    try {
        (void)first.await();
    } catch (const Cancelled&) {
        quit = true;
    }

    ought(quit);

    both.client.write(spelt("hej"), soon).await();

    same(read(patiently(both.server, 8)), std::string{"hej"});
}

TRIAL("Centrala: anulowanie zdejmuje zgłoszenie") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    const std::size_t before = net::Exchange::running().waiting();

    auto one = both.server.read(64, net::Patience{0});

    for (int tried = 0; tried < 200 && net::Exchange::running().waiting() <= before; ++tried)
        Time::later(std::chrono::milliseconds{5}).await();

    ought(net::Exchange::running().waiting() > before);

    one.cancel();

    try {
        (void)one.await();
    } catch (const Cancelled&) {
    }

    settling(before);

    same(net::Exchange::running().waiting(), before);
}

// Brama ma jedno zgłoszenie na siebie, więc porzucone przyjmowanie zamykało ją na głucho:
// każde następne słyszało „zajęta", a połączenia stały w kolejce systemu.
TRIAL("Brama: anulowane przyjmowanie nie zamyka bramy") {
    const net::Gate gate = standing();

    const std::size_t before = net::Exchange::running().waiting();

    auto first = gate.accept(net::Patience{0});

    first.cancel();

    bool quit = false;

    try {
        (void)first.await();
    } catch (const Cancelled&) {
        quit = true;
    }

    ought(quit);

    settling(before);

    Pair both = joined(gate);

    both.client.write(spelt("znowu"), soon).await();

    same(read(both.server.read(8, soon).await()), std::string{"znowu"});
}

// --- Anulowanie zwalnia miejsce od razu ----------------------------------------------------------

// **Anulowanie jest jedyną chwilą, w której wołający się o tym dowiaduje**, więc zaraz po nim ma
// prawo poprosić o następną operację i ma ją dostać. Celowo bez żadnego odczekiwania pośrodku: to
// właśnie odczekanie maskowało usterkę, bo znacznik zdejmowało dopiero spóźnione zakończenie na
// wątku puli.
//
// Na uniksach `withdraw` wyjmuje zgłoszenie z centrali i oddaje prawdę, więc te próby przechodziły
// tu zawsze. Na Windows `CancelIoEx` tylko prosi i prawdy nie ma nigdy — i tam dopiero widać, czy
// miejsce naprawdę zwolniono.
TRIAL("Brama: po anulowaniu można od razu przyjmować znowu") {
    const net::Gate gate = standing();

    auto first = gate.accept(net::Patience{0});

    first.cancel();

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }

    Pair both = joined(gate);

    both.client.write(spelt("znowu"), soon).await();

    same(read(both.server.read(8, soon).await()), std::string{"znowu"});
}

// Czytanie, a **nie** pisanie: krótki zapis na pętli zwrotnej kończy się, zanim zdąży się go
// anulować, więc niczego by tu nie sprawdzał. Czytanie stoi, dopóki coś nie przyjdzie — i dlatego
// anulowanie trafia w operację naprawdę będącą w locie.
TRIAL("Strumień: sto anulowanych czytań nie zajmuje kierunku na stałe") {
    const net::Gate gate = standing();

    Pair both = joined(gate);

    for (int again = 0; again < 100; ++again) {
        auto one = both.server.read(64, net::Patience{0});

        one.cancel();

        try {
            (void)one.await();
        } catch (const std::exception&) {
        }
    }

    both.client.write(spelt("po stu"), soon).await();

    same(read(patiently(both.server, 6)), std::string{"po stu"});
}

// Sto obrotów, bo chodzi o wąskie okno. Gdyby którekolwiek anulowanie zostawiało miejsce zajęte,
// następne przyjmowanie usłyszałoby „Brama już przyjmuje." — i to jest jedyna rzecz, której ta
// próba pilnuje.
TRIAL("Brama: sto anulowań pod rząd nie zajmuje jej na stałe") {
    const net::Gate gate = standing();

    for (int again = 0; again < 100; ++again) {
        auto one = gate.accept(net::Patience{0});

        one.cancel();

        try {
            (void)one.await();
        } catch (const std::exception&) {
        }
    }

    Pair both = joined(gate);

    both.client.write(spelt("po stu"), soon).await();

    same(read(both.server.read(8, soon).await()), std::string{"po stu"});
}

// **Zapis po rozłączeniu nie ma prawa zabić procesu.** Domyślną reakcją na SIGPIPE jest śmierć,
// a biblioteka sieciowa, która ubija program wołającego dlatego, że rozmówca się rozłączył, jest
// nie do użycia. Zmierzone przed poprawką: program ginął z kodem 141, nie zdążywszy zgłosić ani
// jednego zapisu.
//
// Próba jest zarazem sprawdzeniem, że **proces przeżył** — gdyby sygnał wrócił, cała binarka
// próbna umarłaby tutaj i nie doszłaby do końca.
TRIAL("Strumień: zapis po rozłączeniu odmawia, zamiast zabijać proces") {
    const net::Gate gate = standing();

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    one.close();

    // Więcej, niż zmieści bufor gniazda — inaczej pierwszy zapis zszedłby do bufora i nigdy nie
    // dotknąłby zerwanego połączenia.
    const Bytes much(4 * 1024 * 1024, std::byte{0x41});

    bool told = false;

    for (int turn = 0; turn < 3 && !told; ++turn) {
        try {
            two.write(much, soon).await();
        } catch (const std::exception&) {
            told = true;
        }
    }

    ought(told);
}

// --- gniazdo lokalne ---------------------------------------------------------------------------

namespace {

// Ścieżka w katalogu tymczasowym, niepowtarzalna w obrębie biegu. Nie w bieżącym katalogu, bo
// zestaw prób ma prawo biec z miejsca tylko do odczytu.
std::string nearby(const std::string& what) {
    static std::atomic<unsigned> counted{0};

    fs::path where = fs::temp();

    where /= "talcite-" + what + "-" + std::to_string(counted.fetch_add(1)) + ".sock";

    return where.string();
}

}

TRIAL("Gniazdo lokalne: rozmowa idzie ścieżką, nie portem") {
    const std::string said = nearby("rozmowa");

    const net::Gate gate{net::Address::local(said)};

    same(gate.mine().family(), net::Family::local);
    same(gate.mine().path(), said);

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    client.write(spelt("dzień dobry"), soon).await();

    same(read(server.read(64, soon).await()), std::string{"dzień dobry"});

    // Rozmówca po tej stronie zna ścieżkę bramy; nasz własny koniec nazwy nie ma i to jest
    // prawda, a nie brak.
    same(client.peer().path(), said);
}

TRIAL("Gniazdo lokalne: brama sprząta po sobie plik") {
    const std::string said = nearby("sprzatanie");

    {
        const net::Gate gate{net::Address::local(said)};

        same(gate.mine().path(), said);
        ought(fs::exists(said).await());
    }

    ought(!fs::exists(said).await());
}

TRIAL("Gniazdo lokalne: martwy plik nie blokuje nasłuchiwania") {
    const std::string said = nearby("martwy");

    // Plik, pod którym nikt nie słucha — dokładnie to, co zostaje po programie, który padł.
    fs::write(said, Bytes{}).await();

    ought(fs::exists(said).await());

    const net::Gate gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    client.write(spelt("stoję"), soon).await();

    same(read(server.read(16, soon).await()), std::string{"stoję"});
}

TRIAL("Gniazdo lokalne: żywej bramy druga nie przykrywa") {
    const std::string said = nearby("zajete");

    const net::Gate first{net::Address::local(said)};

    bool told = false;

    try {
        const net::Gate second{net::Address::local(said)};
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);

    // Pierwsza brama ma dalej działać — odmowa drugiej nie może jej sprzątnąć pliku.
    auto meeting = first.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    (void)meeting.await();
    (void)joining.await();
}

#include "proving.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "talcite/address.hpp"
#include "exchange.hpp"
#include "piping.hpp"
#include "talcite/fs.hpp"
#include "talcite/listener.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"

#include <iostream>

#ifndef _WIN32
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace talcite;

namespace {

const net::Patience soon{4000};



// Brama na pętli zwrotnej z portem od systemu — dwa testy naraz nigdy się wtedy nie pobiją
// o ten sam numer.
net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

// Obie strony ruszają od razu, bo obietnica sieciowa zaczyna się w konstruktorze. Dopiero
// potem czekamy — inaczej czekanie na przyjęcie zatrzymałoby nawiązanie, które ma je wywołać.
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

TRIAL("Brama: dostaje port od systemu") {
    const net::Listener gate = standing();

    ought(gate.held());
    ought(gate.mine().port() != 0);
    same(gate.mine().text(), std::string{"127.0.0.1"});
}

TRIAL("Brama: pusty adres odrzuca") {
    bool threw = false;

    try {
        const net::Listener gate{net::Address{}};

        (void)gate;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Strumień: nawiązanie i przyjęcie spotykają się") {
    const net::Listener gate = standing();

    const Pair both = joined(gate);

    ought(both.server.held());
    ought(both.client.held());

    same(both.client.peer().port(), gate.mine().port());
    same(both.server.mine().port(), gate.mine().port());
    same(both.server.peer().port(), both.client.mine().port());
}

TRIAL("Strumień: bajty przechodzą w obie strony") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(bytes("witaj"), soon).await();

    same(text(both.server.read(64, soon).await()), std::string{"witaj"});

    both.server.write(bytes("i wzajemnie"), soon).await();

    same(text(both.client.read(64, soon).await()), std::string{"i wzajemnie"});
}

TRIAL("Strumień: większa paczka schodzi w całości") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::string much(200000, 'x');

    auto writing = both.client.write(bytes(much), soon);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).await();

        if (one.empty()) break;

        made += text(one);
    }

    writing.await();

    same(made.size(), much.size());
    ought(made == much);
}

TRIAL("Strumień: zamknięcie drugiej strony daje pustkę") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.close();

    same(static_cast<int>(both.server.read(64, soon).await().size()), 0);
}

TRIAL("Strumień: cisza kończy się terminem, a nie pustką") {
    const net::Listener gate = standing();

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
    const net::Listener gate = standing();

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
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(bytes("raz"), soon).await();

    same(text(both.server.read(64, soon).await()), std::string{"raz"});

    both.client.write(bytes("dwa"), soon).await();

    same(text(both.server.read(64, soon).await()), std::string{"dwa"});
}

TRIAL("Strumień: zamknięcie przerywa czekające czytanie") {
    const net::Listener gate = standing();

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
        const net::Listener gate = standing();

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
    const net::Listener gate = standing();

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
    const net::Listener gate = standing();

    for (int which = 0; which < 3; ++which) {
        Pair both = joined(gate);

        both.client.write(bytes(std::to_string(which)), soon).await();

        same(text(both.server.read(8, soon).await()), std::to_string(which));
    }
}

TRIAL("Centrala: po wszystkim nikt nie czeka") {
    const net::Listener gate = standing();

    {
        Pair both = joined(gate);

        both.client.write(bytes("hej"), soon).await();

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
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

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
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

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

    client.write(bytes("po wyjątku"), net::Patience{3000}).await();

    same(text(server.read(64, net::Patience{3000}).await()), std::string{"po wyjątku"});
}

// Zerowe czytanie wyglądało jak koniec strumienia: recv z zerową długością oddaje zero także na
// zdrowym gnieździe, a zero bez błędu znaczy u nas „druga strona zamknęła".
TRIAL("Strumień: zerowe czytanie nie udaje końca strumienia") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(soon);

    const net::Stream client = net::Stream::connect(gate.mine(), soon).await();
    const net::Stream server = waiting.await();

    client.write(bytes("osiem!!!"), soon).await();

    bool amiss = false;

    try {
        (void)server.read(0, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);

    same(text(server.read(64, soon).await()), std::string{"osiem!!!"});
}

// Po zamknięciu wszystkie metody odpowiadają grzecznie — te też muszą, zamiast naruszać pamięć.
TRIAL("Strumień: adresy po zamknięciu są puste, a nie wybuchają") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto waiting = gate.accept(soon);

    net::Stream client = net::Stream::connect(gate.mine(), soon).await();

    const net::Stream server = waiting.await();

    client.close();

    ought(!client.peer().held());
    ought(!client.mine().held());

    net::Listener other{net::Address::loopback(net::Family::four, 0)};

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
    const net::Listener gate = standing();

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

    both.client.write(bytes("hej"), soon).await();

    same(text(patiently(both.server, 8)), std::string{"hej"});
}

TRIAL("Centrala: anulowanie zdejmuje zgłoszenie") {
    const net::Listener gate = standing();

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
    const net::Listener gate = standing();

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

    both.client.write(bytes("znowu"), soon).await();

    same(text(both.server.read(8, soon).await()), std::string{"znowu"});
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
    const net::Listener gate = standing();

    auto first = gate.accept(net::Patience{0});

    first.cancel();

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }

    Pair both = joined(gate);

    both.client.write(bytes("znowu"), soon).await();

    same(text(both.server.read(8, soon).await()), std::string{"znowu"});
}

// Czytanie, a **nie** pisanie: krótki zapis na pętli zwrotnej kończy się, zanim zdąży się go
// anulować, więc niczego by tu nie sprawdzał. Czytanie stoi, dopóki coś nie przyjdzie — i dlatego
// anulowanie trafia w operację naprawdę będącą w locie.
TRIAL("Strumień: sto anulowanych czytań nie zajmuje kierunku na stałe") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    for (int again = 0; again < 100; ++again) {
        auto one = both.server.read(64, net::Patience{0});

        one.cancel();

        try {
            (void)one.await();
        } catch (const std::exception&) {
        }
    }

    both.client.write(bytes("po stu"), soon).await();

    same(text(patiently(both.server, 6)), std::string{"po stu"});
}

// Sto obrotów, bo chodzi o wąskie okno. Gdyby którekolwiek anulowanie zostawiało miejsce zajęte,
// następne przyjmowanie usłyszałoby „Brama już przyjmuje." — i to jest jedyna rzecz, której ta
// próba pilnuje.
TRIAL("Brama: sto anulowań pod rząd nie zajmuje jej na stałe") {
    const net::Listener gate = standing();

    for (int again = 0; again < 100; ++again) {
        auto one = gate.accept(net::Patience{0});

        one.cancel();

        try {
            (void)one.await();
        } catch (const std::exception&) {
        }
    }

    Pair both = joined(gate);

    both.client.write(bytes("po stu"), soon).await();

    same(text(both.server.read(8, soon).await()), std::string{"po stu"});
}

// **Zapis po rozłączeniu nie ma prawa zabić procesu.** Domyślną reakcją na SIGPIPE jest śmierć,
// a biblioteka sieciowa, która ubija program wołającego dlatego, że rozmówca się rozłączył, jest
// nie do użycia. Zmierzone przed poprawką: program ginął z kodem 141, nie zdążywszy zgłosić ani
// jednego zapisu.
//
// Próba jest zarazem sprawdzeniem, że **proces przeżył** — gdyby sygnał wrócił, cała binarka
// próbna umarłaby tutaj i nie doszłaby do końca.
TRIAL("Strumień: zapis po rozłączeniu odmawia, zamiast zabijać proces") {
    const net::Listener gate = standing();

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

    const net::Listener gate{net::Address::local(said)};

    same(gate.mine().family(), net::Family::local);
    same(gate.mine().path(), said);

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    client.write(bytes("dzień dobry"), soon).await();

    same(text(server.read(64, soon).await()), std::string{"dzień dobry"});

    // Rozmówca po tej stronie zna ścieżkę bramy; nasz własny koniec nazwy nie ma i to jest
    // prawda, a nie brak.
    same(client.peer().path(), said);
}

// Po drugiej stronie gniazda lokalnego jest ten sam użytkownik — system to mówi, a demon
// udostępniania pyta o to, zanim cokolwiek odpowie. Połączenie sieciowe nie ma tu nic do
// powiedzenia: pustka. Windows mówi przez numer procesu rozmówcy; system, który go nie da
// (stary, Wine), też daje pustkę, a nie fałsz.
TRIAL("Gniazdo lokalne: rozmówca to ten sam użytkownik, a w sieci — pustka") {
    const std::string said = nearby("swoj");

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    const net::Stream server = meeting.await();
    const net::Stream client = joining.await();

#ifdef _WIN32
    ought(!server.kindred().has_value() || *server.kindred());

    // Rura mówi o rozmówcy po obu stronach.
    if (net::pipes::chosen()) {
        ought(server.kindred() == std::optional<bool>{true});
        ought(client.kindred() == std::optional<bool>{true});
    }
#else
    ought(server.kindred() == std::optional<bool>{true});
    ought(client.kindred() == std::optional<bool>{true});
#endif

    const net::Listener far = standing();
    const Pair both = joined(far);

    ought(!both.server.kindred().has_value());
    ought(!both.client.kindred().has_value());
    ought(!net::Stream{}.kindred().has_value());
}

#ifndef _WIN32

// Rozmówca innego użytkownika — dziecko, które przed połączeniem zostaje kimś innym. Zmienić
// użytkownika umie tylko administrator, więc bez tych praw próba się pomija.
TRIAL("Gniazdo lokalne: rozmówca innego użytkownika to nie my") {
    if (::geteuid() != 0) {
        std::cout << "        bez praw administratora nie da się być kimś innym — próba pominięta\n";

        return;
    }

    const std::string said = nearby("obcy");

    const net::Listener gate{net::Address::local(said)};

    ::chmod(said.c_str(), 0777);

    auto meeting = gate.accept(soon);

    const pid_t child = ::fork();

    if (child == 0) {
        if (::setgid(65534) != 0 || ::setuid(65534) != 0) ::_exit(3);

        const int one = ::socket(AF_UNIX, SOCK_STREAM, 0);

        sockaddr_un where{};

        where.sun_family = AF_UNIX;

        std::memcpy(where.sun_path, said.c_str(), said.size() + 1);

        if (::connect(one, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0) ::_exit(4);

        char got = 0;

        (void)!::read(one, &got, 1);

        ::_exit(0);
    }

    const net::Stream server = meeting.await();

    ought(server.kindred() == std::optional<bool>{false});

    server.write(bytes("k"), soon).await();

    int status = 0;

    ::waitpid(child, &status, 0);

    ought(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#endif

// Rura nazwana pliku nie zakłada wcale — wtedy nie ma czego sprzątać.
TRIAL("Gniazdo lokalne: brama sprząta po sobie plik") {
    const std::string said = nearby("sprzatanie");

    {
        const net::Listener gate{net::Address::local(said)};

        same(gate.mine().path(), said);
        ought(fs::exists(said).await() != net::pipes::chosen());
    }

    ought(!fs::exists(said).await());
}

// Kilku klientów łączy się, zanim brama zacznie przyjmować, i wszyscy dochodzą. Przy rurze klient,
// dla którego nie ma wolnej instancji, czeka na nią — a nie dostaje odmowy, bo nazwa nie znika.
TRIAL("Gniazdo lokalne: kilku rozmówców naraz, także przed przyjęciem") {
    const std::string said = nearby("kilku");

    const net::Listener gate{net::Address::local(said)};

    auto first = net::Stream::connect(net::Address::local(said), soon);
    auto second = net::Stream::connect(net::Address::local(said), soon);
    auto third = net::Stream::connect(net::Address::local(said), soon);

    std::vector<net::Stream> servers;

    for (int turn = 0; turn < 3; ++turn) servers.push_back(gate.accept(soon).await());

    std::vector<net::Stream> clients{first.await(), second.await(), third.await()};

    for (std::size_t at = 0; at < clients.size(); ++at) clients[at].write(bytes(std::to_string(at)), soon).await();

    std::vector<std::string> heard;

    for (net::Stream& one : servers) heard.push_back(text(one.read(16, soon).await()));

    std::sort(heard.begin(), heard.end());

    same(heard, (std::vector<std::string>{"0", "1", "2"}));

    // Odpowiedź i koniec: zamknięta strona serwera to dla klienta pustka po przeczytaniu reszty.
    servers.front().write(bytes("pa"), soon).await();
    servers.front().close();
    servers.erase(servers.begin());

    bool answered = false;

    for (net::Stream& one : clients) {
        Bytes got;

        try {
            got = one.read(16, net::Patience{300}).await();
        } catch (const std::exception&) {
        }

        if (text(got) == "pa") {
            answered = true;

            ought(one.read(16, soon).await().empty());
        }
    }

    ought(answered);
}

// Zamknięcie jednej kopii strumienia kończy rozmowę także wtedy, gdy druga kopia jeszcze żyje — tak
// jak `shutdown` gniazda. Pusty zapis po drodze nie udaje końca.
TRIAL("Gniazdo lokalne: zamknięcie kopii kończy rozmowę, a pusty zapis jej nie kończy") {
    const std::string said = nearby("koniec");

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    client.write(Bytes{}, soon).await();
    client.write(bytes("x"), soon).await();

    same(text(server.read(16, soon).await()), std::string{"x"});

    const net::Stream twin = client;

    // Czytanie czeka, zanim druga strona się zamknie — koniec przychodzi do czekającego.
    auto waiting = server.read(16, soon);

    client.close();

    ought(waiting.await().empty());
    ought(twin.held());
}

// Przyjmowanie anulowane, potem nowe: klient trafia do nowego, a nie do instancji obok, na którą nikt
// nie czeka. Kilka razy, bo przy rurze to kwestia tego, w którą instancję trafi klient. Chwila po
// anulowaniu jest dla rury: anulowana instancja słucha, dopóki system jej nie odda, a klient, który
// trafi w nią w tej chwili, dostaje zerwaną rurę i łączy się od nowa.
TRIAL("Gniazdo lokalne: po anulowanym przyjmowaniu klient trafia do następnego") {
    const std::string said = nearby("anulowane");

    const net::Listener gate{net::Address::local(said)};

    for (int turn = 0; turn < 6; ++turn) {
        auto dropped = gate.accept(soon);

        dropped.cancel();

        std::this_thread::sleep_for(std::chrono::milliseconds{100});

        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(net::Address::local(said), soon);

        net::Stream server = meeting.await();
        net::Stream client = joining.await();

        client.write(bytes("tak"), soon).await();

        same(text(server.read(16, soon).await()), std::string{"tak"});
    }
}

// Zamknięta brama oddaje ścieżkę od razu — także z przyjmowaniem w toku — a połączona przed chwilą
// rozmowa po zamknięciu nie zostawia bramy bez wolnego miejsca: następny klient czeka na przyjęcie.
TRIAL("Gniazdo lokalne: zamknięta brama oddaje ścieżkę, a między przyjęciami jest miejsce dla klienta") {
    const std::string said = nearby("oddana");

    {
        net::Listener gate{net::Address::local(said)};

        auto waiting = gate.accept(soon);
        const auto closed = std::chrono::steady_clock::now();

        gate.close();

        bool told = false;

        try {
            (void)waiting.await();
        } catch (const std::exception&) {
            told = true;
        }

        // Od razu, nie po terminie: zamknięcie przerywa czekające przyjmowanie.
        ought(told);
        ought(std::chrono::steady_clock::now() - closed < std::chrono::seconds{2});
    }

    const net::Listener again{net::Address::local(said)};

    {
        auto meeting = again.accept(soon);
        auto joining = net::Stream::connect(net::Address::local(said), soon);

        net::Stream server = meeting.await();
        net::Stream client = joining.await();

        server.close();
        client.close();
    }

    // Między przyjęciami, po zamknięciu jedynej rozmowy: klient łączy się, zanim brama zacznie przyjmować.
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    auto meeting = again.accept(soon);

    net::Stream client = joining.await();
    net::Stream server = meeting.await();

    client.write(bytes("jestem"), soon).await();

    same(text(server.read(16, soon).await()), std::string{"jestem"});
}

#ifdef _WIN32

// Ścieżki Windows nie rozróżniają wielkości liter — rura z tej samej ścieżki inaczej zapisanej też.
TRIAL("Gniazdo lokalne: na Windows ścieżka bez względu na wielkość liter") {
    const std::string said = nearby("Wielkie");

    std::string lowered = said;

    for (char& one : lowered)
        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(lowered), soon);

    (void)meeting.await();
    (void)joining.await();
}

#endif

TRIAL("Gniazdo lokalne: martwy plik nie blokuje nasłuchiwania") {
    const std::string said = nearby("martwy");

    // Plik, pod którym nikt nie słucha — dokładnie to, co zostaje po programie, który padł.
    fs::write(said, Bytes{}).await();

    ought(fs::exists(said).await());

    const net::Listener gate{net::Address::local(said)};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(net::Address::local(said), soon);

    net::Stream server = meeting.await();
    net::Stream client = joining.await();

    client.write(bytes("stoję"), soon).await();

    same(text(server.read(16, soon).await()), std::string{"stoję"});
}

TRIAL("Gniazdo lokalne: żywej bramy druga nie przykrywa") {
    const std::string said = nearby("zajete");

    const net::Listener first{net::Address::local(said)};

    bool told = false;

    try {
        const net::Listener second{net::Address::local(said)};
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

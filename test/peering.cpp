#include "proving.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <chrono>

#include "talcite/address.hpp"
#include "talcite/peer.hpp"
#include "talcite/udp.hpp"
#include "talcite/stunclient.hpp"
#include "talcite/time.hpp"

using namespace talcite;

// Prawdziwego NAT-a w piaskownicy nie ma i nie da się go udać. Sprawdzamy więc **protokół**: czy
// obie strony dobijające się naraz dochodzą do siebie, czy wybierają tę samą drogę, czy dane nie
// mieszają się ze sprawdzaniem łączności i czy cisza kończy się terminem. Przejście przez dwa
// prawdziwe NAT-y sprawdza się tylko w prawdziwej sieci.
namespace {

const net::Patience soon{6000};

net::Udp standing() { return net::Udp{net::Address::loopback(net::Family::four, 0)}; }



}

TRIAL("Rozmówca: dwie strony dobijające się naraz dochodzą do siebie") {
    net::Udp one = standing();
    net::Udp two = standing();

    const net::Address where = one.mine();
    const net::Address there = two.mine();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{there, net::Candidate::Sort::here});
    right.add(net::Candidate{where, net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    same(first.await(), there);
    same(second.await(), where);

    same(left.met(), there);
    same(right.met(), where);
}

TRIAL("Rozmówca: strona bierna odpowiada i sama dochodzi do rozmówcy") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});

    // Prawa strona zna lewą z sygnalizacji, ale sama nie woła reach. Budzimy jej pętlę czekaniem
    // na dane, których nikt nie przyśle — ma odpowiadać na cudze próby i dojść do rozmówcy sama.
    // Tak właśnie wygląda strona, do której ktoś się dobija.
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto waiting = right.receive(1024, net::Patience{2000});

    same(left.reach(soon).await(), two.mine());

    try {
        (void)waiting.await();
    } catch (const std::exception&) {
    }

    same(right.met(), one.mine());
}

// Zapytanie STUN złożone bajt po bajcie, żeby udawać obcego, który odzywa się nieproszony.
Bytes knocking() {
    Bytes made(20, std::byte{0});

    made[1] = std::byte{0x01};
    made[4] = std::byte{0x21};
    made[5] = std::byte{0x12};
    made[6] = std::byte{0xA4};
    made[7] = std::byte{0x42};

    for (std::size_t at = 8; at < 20; ++at) made[at] = static_cast<std::byte>(at);

    return made;
}

// NAT prawie zawsze nadaje drugiej stronie inny port niż ten, który zdążyła zgłosić. Drogę od tego
// samego gospodarza wolno więc dopisać — bez tego przebijanie nie zadziałałoby prawie nigdy.
TRIAL("Rozmówca: inny port znanego gospodarza dopisuje się sam") {
    net::Udp one = standing();
    net::Udp two = standing();
    net::Udp three = standing();

    net::Peer left{one};

    // Sygnalizacja podała port drugiego gniazda, a próba przyjdzie z trzeciego — ten sam adres,
    // inny port, dokładnie jak zza NAT-u.
    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});

    auto waking = left.receive(1024, net::Patience{1500});

    bool knows = false;

    for (int turn = 0; turn < 30 && !knows; ++turn) {
        three.send(one.mine(), knocking(), soon).await();

        Time::later(std::chrono::milliseconds{50}).await();

        for (const net::Candidate& which : left.theirs())
            if (which.where == three.mine()) knows = true;
    }

    ought(knows);

    try {
        (void)waking.await();
    } catch (const std::exception&) {
    }
}

// Bez tego wystarczyło jedno nieproszone zapytanie, żeby obcy trafił na listę dróg i dostał od nas
// numer transakcji — a numer jest jedynym dowodem tożsamości odpowiedzi, więc odesłany wracał jako
// wygrane przebijanie.
TRIAL("Rozmówca: gospodarz, o którym nikt nie mówił, nie dopisuje się") {
    net::Udp one = standing();
    net::Udp evil = standing();

    net::Peer left{one};

    // Jedyna znana droga prowadzi gdzie indziej i nigdy się nie odezwie.
    left.add(net::Candidate{net::Address{"203.0.113.9", 5000}, net::Candidate::Sort::here});

    auto waking = left.receive(1024, net::Patience{900});

    for (int turn = 0; turn < 6; ++turn) {
        evil.send(one.mine(), knocking(), soon).await();

        Time::later(std::chrono::milliseconds{60}).await();
    }

    ought(!left.met().held());

    same(static_cast<int>(left.theirs().size()), 1);

    for (const net::Candidate& which : left.theirs()) ought(!(which.where == evil.mine()));

    try {
        (void)waking.await();
    } catch (const std::exception&) {
    }
}

TRIAL("Rozmówca: po przebiciu da się rozmawiać") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    (void)first.await();
    (void)second.await();

    auto waiting = right.receive(1024, soon);

    left.send(bytes("witaj po drugiej stronie dziury"), soon).await();

    same(text(waiting.await()), std::string{"witaj po drugiej stronie dziury"});
}

TRIAL("Rozmówca: sprawdzanie łączności nie miesza się z danymi") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    (void)first.await();
    (void)second.await();

    // Podtrzymania i odpowiedzi lecą tym samym gniazdem przez cały czas. Do danych nie ma prawa
    // trafić ani jeden bajt STUN — inaczej wołający dostałby śmieć zamiast swojej wiadomości.
    for (int turn = 0; turn < 5; ++turn) {
        auto waiting = right.receive(1024, soon);

        left.send(bytes("rata " + std::to_string(turn)), soon).await();

        same(text(waiting.await()), "rata " + std::to_string(turn));
    }
}

TRIAL("Rozmówca: bez drogi nie ma się gdzie dobijać") {
    net::Udp one = standing();

    net::Peer left{one};

    bool amiss = false;

    try {
        (void)left.reach(soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Rozmówca: droga donikąd kończy się terminem") {
    net::Udp one = standing();
    net::Udp two = standing();

    const net::Address nobody = two.mine();

    // Zamykamy drugie gniazdo, więc adres jest poprawny, ale nikt za nim nie stoi.
    two.close();

    net::Peer left{one};

    left.add(net::Candidate{nobody, net::Candidate::Sort::here});

    bool late = false;

    try {
        (void)left.reach(net::Patience{700}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Rozmówca: własnego adresu nie przyjmujemy za cudzą drogę") {
    net::Udp one = standing();

    net::Peer left{one};

    left.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    ought(left.theirs().empty());
}

TRIAL("Rozmówca: drogi ustawiają się wedle rodzaju") {
    net::Udp one = standing();

    net::Peer left{one};

    left.add(net::Candidate{net::Address{"203.0.113.9", 5000}, net::Candidate::Sort::lent});
    left.add(net::Candidate{net::Address{"198.51.100.4", 4000}, net::Candidate::Sort::seen});
    left.add(net::Candidate{net::Address{"192.168.1.7", 3000}, net::Candidate::Sort::here});

    const std::vector<net::Candidate> made = left.theirs();

    same(static_cast<int>(made.size()), 3);
    same(made[0].sort, net::Candidate::Sort::here);
    same(made[1].sort, net::Candidate::Sort::seen);
    same(made[2].sort, net::Candidate::Sort::lent);
}

TRIAL("Rozmówca: ta sama droga nie wchodzi dwa razy") {
    net::Udp one = standing();

    net::Peer left{one};

    const net::Candidate where{net::Address{"192.168.1.7", 3000}, net::Candidate::Sort::here};

    left.add(where);
    left.add(where);

    same(static_cast<int>(left.theirs().size()), 1);
}

TRIAL("Rozmówca: zbieranie kandydatów bez zwiadowcy daje same karty") {
    net::Udp one{net::Address::any(net::Family::four, 0)};

    net::Peer left{one};

    const std::vector<net::Candidate> made = left.gather(net::StunClient{}, soon).await();

    for (const net::Candidate& which : made) {
        same(which.sort, net::Candidate::Sort::here);

        // Kandydat niesie port **naszego** gniazda, bo to jego szuka druga strona.
        same(which.where.port(), one.mine().port());

        // Pętla zwrotna nie może wyjść na zewnątrz: kazałaby drugiej stronie dobijać się do samej
        // siebie, a odpowiedź od własnego gniazda wyglądałaby jak udane przebicie.
        ought(!(which.where == net::Address::loopback(net::Family::four, which.where.port())));
    }

    same(left.candidates().size(), made.size());
}

TRIAL("Rozmówca: nie ma do kogo mówić przed przebiciem") {
    net::Udp one = standing();

    net::Peer left{one};

    bool amiss = false;

    try {
        left.send(bytes("za wcześnie"), soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Rozmówca: cisza na odbiorze kończy się terminem") {
    net::Udp one = standing();

    net::Peer left{one};

    bool late = false;

    try {
        (void)left.receive(1024, net::Patience{300}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Rozmówca: zamknięty nie przyjmuje niczego") {
    net::Udp one = standing();

    net::Peer left{one};

    left.close();

    bool amiss = false;

    try {
        (void)left.reach(soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Rozmówca: dwa dobijania naraz dostają odmowę") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});

    auto first = left.reach(net::Patience{500});

    bool busy = false;

    try {
        (void)left.reach(soon).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }
}

// Odmowa nie ma prawa zjeść przesyłki: sprawdzenie rozmiaru idzie przed zdjęciem jej z kolejki,
// więc powtórzenie z większym miejscem jeszcze ją zastanie.
TRIAL("Rozmówca: za duża przesyłka nie ginie przy odmowie") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    (void)first.await();
    (void)second.await();

    left.send(bytes("sto bajtów by tu nie weszło"), soon).await();

    bool amiss = false;

    for (int turn = 0; turn < 40 && !amiss; ++turn) {
        try {
            (void)right.receive(4, net::Patience{100}).await();
        } catch (const Overdue&) {
            continue;
        } catch (const net::Trouble&) {
            amiss = true;
        }
    }

    ought(amiss);

    same(text(right.receive(1024, soon).await()), std::string{"sto bajtów by tu nie weszło"});
}

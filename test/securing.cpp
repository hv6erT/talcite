#include "proving.hpp"

#include "faking.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/listener.hpp"
#include "talcite/tls.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"

using namespace talcite;

namespace {

const net::Patience soon{15000};



using faking::Watching;

net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

struct Pair {
    net::TlsStream server;
    net::TlsStream client;
};

Pair guarded(const net::Listener& gate, const talcite::Seam<net::TlsProvider>& guard) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    auto there = net::TlsStream::server(std::move(one), guard, soon);
    auto here = net::TlsStream::client(std::move(two), guard, "gdziekolwiek", soon);

    net::TlsStream server = there.await();
    net::TlsStream client = here.await();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Szew: uścisk dłoni dochodzi do skutku po obu stronach") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    ought(both.server.held());
    ought(both.client.held());

    same(both.client.peer().port(), gate.mine().port());
}

TRIAL("Szew: treść przechodzi w obie strony") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("tajna wiadomość"), soon).await();

    same(text(both.server.read(1024, soon).await()), std::string{"tajna wiadomość"});

    both.server.write(bytes("i odpowiedź"), soon).await();

    same(text(both.client.read(1024, soon).await()), std::string{"i odpowiedź"});
}

TRIAL("Szew: sto tysięcy bajtów przechodzi w kawałkach") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    const std::string much(100000, 'z');

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

TRIAL("Szew: czytanie mniejsze niż rekord zostawia resztę na później") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("abcdefghij"), soon).await();

    same(text(both.server.read(4, soon).await()), std::string{"abcd"});
    same(text(both.server.read(4, soon).await()), std::string{"efgh"});
    same(text(both.server.read(4, soon).await()), std::string{"ij"});
}

TRIAL("Szew: zamknięcie drugiej strony daje pustkę") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.close();

    same(static_cast<int>(both.server.read(64, soon).await().size()), 0);
}

TRIAL("Szew: cisza kończy się terminem") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    bool late = false;

    try {
        (void)both.server.read(64, net::Patience{200}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Szew: drugie czytanie naraz dostaje odmowę") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    auto first = both.server.read(64, net::Patience{300});

    bool busy = false;

    try {
        (void)both.server.read(64, net::Patience{300}).await();
    } catch (const std::exception&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }
}

TRIAL("Szew: kłopot zgłoszony przez zabezpieczenie wychodzi na wierzch") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>(true)};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("cokolwiek"), soon).await();

    bool amiss = false;

    try {
        (void)both.server.read(64, soon).await();
    } catch (const net::Trouble& why) {
        amiss = true;

        ought(std::string{why.what()}.find("Trucizna") != std::string::npos);
    }

    ought(amiss);
}

// Kłopot, którego przejściówka nie rozpoznała, też jest `TlsTrouble` — z powodem „inny"; po stronie
// serwera bez nazwy, bo serwer o nią nie pyta.
TRIAL("Szew: kłopot bez rozpoznanego powodu to TlsTrouble z powodem other") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>(true)};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("cokolwiek"), soon).await();

    bool amiss = false;

    try {
        (void)both.server.read(64, soon).await();
    } catch (const net::TlsTrouble& why) {
        amiss = true;

        same(why.why(), net::TlsTrouble::Why::other);
        ought(why.host().empty());
        ought(why.where().held());
    }

    ought(amiss);
}

// Odmowa uścisku z powodem od przejściówki: każdy powód dochodzi do wołającego taki, jaki był, razem
// z nazwą serwera, o którą pytał klient, i z adresem drugiej strony.
TRIAL("Szew: odmowa uścisku niesie powód, nazwę serwera i adres") {
    for (const net::TlsTrouble::Why one : {net::TlsTrouble::Why::ciphers, net::TlsTrouble::Why::untrusted,
                                           net::TlsTrouble::Why::expired, net::TlsTrouble::Why::misnamed}) {
        const net::Listener gate = standing();

        const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>(false, one)};

        auto meeting = gate.accept(soon);
        net::Stream two = net::Stream::connect(gate.mine(), soon).await();
        const net::Stream there = meeting.await();

        bool refused = false;

        try {
            (void)net::TlsStream::client(std::move(two), guard, "talcite.test", soon).await();
        } catch (const net::TlsTrouble& why) {
            refused = true;

            same(why.why(), one);
            same(why.host(), std::string{"talcite.test"});
            ought(why.where() == gate.mine());
            same(std::string{why.what()}, std::string{"Uścisk odrzucony."});
        }

        ought(refused);
    }
}

TRIAL("Szew: uścisk urwany w połowie odrzuca obietnicę") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    auto here = net::TlsStream::client(std::move(two), guard, "gdziekolwiek", soon);

    // Druga strona nie odpowiada, tylko zamyka.
    one.close();

    bool amiss = false;

    try {
        (void)here.await();
    } catch (const std::exception&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Szew: pisanie i czytanie naraz nie mieszają się na gnieździe") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    auto reading = both.client.read(1024, soon);

    both.server.write(bytes("od serwera"), soon).await();
    both.client.write(bytes("od klienta"), soon).await();

    same(text(reading.await()), std::string{"od serwera"});
    same(text(both.server.read(1024, soon).await()), std::string{"od klienta"});
}

// Strażnik jest wartością, więc **można go nie mieć**. Pusty musi powiedzieć to wprost, a nie
// sięgnąć przez pusty wskaźnik przy pierwszej rundzie uścisku dłoni.
class Nothing : public net::TlsProvider {
public:
    [[nodiscard]] std::string_view name() const override { return "nothing"; }

    [[nodiscard]] std::unique_ptr<net::TlsSession> client(std::string) override { return nullptr; }

    [[nodiscard]] std::unique_ptr<net::TlsSession> server() override { return nullptr; }
};

// Pusty szew to brak dostawcy — błąd talcite, więc idzie językiem talcite: `Unbound`, ten sam typ,
// który rzuca `Seam::provider()`. Obietnica go odrzuca, zamiast rzucać wołającemu w twarz.
TRIAL("Szew: pusty szew odmawia Unbound, a provider() na nim rzuca to samo") {
    const net::Listener gate = standing();

    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream two = joining.await();

    const talcite::Seam<net::TlsProvider> none;

    ought(!none.bound());

    bool amiss = false;

    try {
        (void)none.provider();
    } catch (const talcite::Unbound& why) {
        amiss = std::string{why.what()} == "No provider for TLS.";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)net::TlsStream::client(std::move(two), none, "gdziekolwiek", soon).await();
    } catch (const talcite::Unbound&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Szew: dostawca ma nazwę, a szew go oddaje") {
    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    ought(guard.bound());
    same(std::string{guard.provider().name()}, std::string{"flipping"});
}

TRIAL("Szew: strażnik oddający pustkę odmawia z powodem") {
    const net::Listener gate = standing();

    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream two = joining.await();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Nothing>()};

    ought(guard.bound());
    ought(guard.provider().server() == nullptr);

    bool amiss = false;

    try {
        (void)net::TlsStream::server(std::move(two), guard, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

// Odmowy z `TlsStream` wychodziły **napisem**, nie kłopotem: `reject` opakowuje napis w `Rejected`,
// więc żadnej z nich nie łapało `catch (const Trouble&)`. Reszta biblioteki odmawia kłopotem
// i te cztery miejsca muszą robić tak samo.
TRIAL("Szew: odmowy wychodzą kłopotem, a nie samym napisem") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.close();

    bool amiss = false;

    try {
        (void)both.client.read(1024, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);

    amiss = false;

    try {
        both.client.write(bytes("do zamkniętego"), soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

// Drugie czytanie w locie ma odmówić **kłopotem**, tak jak wszystko inne. Para musi tu być świeża
// i niezamknięta: przy zamkniętej pierwsze czytanie kończy się natychmiast, znacznik wraca na
// miejsce i drugie zastaje wolne — próba przechodziła wtedy z przypadku, raz na kilkanaście
// przebiegów sprawdzając co innego, niż mówi jej nazwa.
TRIAL("Szew: drugie czytanie naraz odmawia kłopotem") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    // Nikt nic nie przyśle, więc pierwsze czytanie czeka całą swoją cierpliwość.
    auto reading = both.server.read(1024, net::Patience{1500});

    bool busy = false;

    try {
        (void)both.server.read(1024, net::Patience{300}).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)reading.await();
    } catch (const std::exception&) {
    }
}

// **Zamknięcie w trakcie pisania.** `TlsStream::close()` dotyka składowej, którą w tej samej chwili
// czyta robotnik puli prowadzący zapis — a `Stream` trzyma zwykły `shared_ptr`, nie atomowy.
// Zapis obok odczytu tego samego wskazania to UB, więc czytelnicy biorą dziś kopię spod zamka,
// a zamknięcie idzie pod tym samym zamkiem.
//
// Próba nie sprawdza wyniku zapisu — ten wolno odrzucić, i o to właśnie chodzi. Sprawdza, że
// **nikt się nie wywraca ani nie wiesza**, a pod TSan-em pokrywa ścieżkę, której cała reszta prób
// nie dotykała: bez niej sanitizer nie miał czego zobaczyć.
TRIAL("Szew: zamknięcie w trakcie zapisu nikogo nie wywraca") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    for (int turn = 0; turn < 8; ++turn) {
        Pair both = guarded(gate, guard);

        // Więcej, niż zejdzie jednym ruchem — zapis ma jeszcze biec, gdy przyjdzie zamknięcie.
        Bytes much;

        much.reserve(512 * 1024);

        for (std::size_t at = 0; at < 512 * 1024; ++at) much.push_back(std::byte{0x41});

        Promise<void> wrote = both.client.write(std::move(much), soon);

        wrote.start();

        std::thread other{[&both] { both.client.close(); }};

        try {
            wrote.await();
        } catch (...) {
        }

        other.join();

        both.server.close();
    }

    ought(true);
}

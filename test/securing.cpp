#include "proving.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "talcite/gate.hpp"
#include "talcite/secured.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

namespace {

const net::Patience soon{15000};

Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

std::string spelling(const Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

// **To nie jest kryptografia i nie udaje nią być.** To atrapa: odwraca bity jednym bajtem, żeby
// dało się zobaczyć, że treść naprawdę przechodzi przez szew, i wymienia powitanie, żeby pompa
// miała uścisk dłoni do przeprowadzenia. Cały sens: sprawdzić `Secured` bez żadnej biblioteki.
class Turning : public net::Secure {
public:
    explicit Turning(bool poison = false) : poison_(poison) {}

    [[nodiscard]] bool ready() const override { return said_ && heard_; }

    void push(Bytes given) override {
        came_.insert(came_.end(), given.begin(), given.end());

        for (;;) {
            if (came_.empty()) return;

            const std::size_t much = static_cast<std::size_t>(came_.front());

            if (came_.size() < much + 1) return;

            Bytes one(came_.begin() + 1, came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            came_.erase(came_.begin(), came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            for (std::byte& what : one) what ^= std::byte{0x5A};

            if (!heard_) {
                if (spelling(one) != "CZESC") {
                    amiss_ = "Powitanie nie tak brzmi.";

                    return;
                }

                heard_ = true;

                continue;
            }

            if (spelling(one) == "TRUCIZNA") {
                amiss_ = "Trucizna w rekordzie.";

                return;
            }

            plain_.insert(plain_.end(), one.begin(), one.end());
        }
    }

    // Pierwsze pytanie o szyfrogram **napędza** uścisk dłoni — bez tego nikt by go nie zaczął.
    [[nodiscard]] Bytes pull() override {
        if (!said_) {
            said_ = true;

            framed(spelt("CZESC"));
        }

        Bytes made = std::move(out_);

        out_.clear();

        return made;
    }

    void write(Bytes given) override {
        if (poison_) given = spelt("TRUCIZNA");

        for (std::size_t at = 0; at < given.size(); at += 255) {
            const std::size_t much = std::min<std::size_t>(255, given.size() - at);

            framed(Bytes{given.begin() + static_cast<std::ptrdiff_t>(at),
                         given.begin() + static_cast<std::ptrdiff_t>(at + much)});
        }
    }

    [[nodiscard]] Bytes read() override {
        Bytes made = std::move(plain_);

        plain_.clear();

        return made;
    }

    void close() override {}

    [[nodiscard]] std::string amiss() const override { return amiss_; }

private:
    void framed(Bytes what) {
        out_.push_back(static_cast<std::byte>(what.size()));

        for (std::byte one : what) out_.push_back(one ^ std::byte{0x5A});
    }

    Bytes came_;
    Bytes plain_;
    Bytes out_;
    std::string amiss_;
    bool said_ = false;
    bool heard_ = false;
    bool poison_ = false;
};

class Watching : public net::Keeper {
public:
    explicit Watching(bool poison = false) : poison_(poison) {}

    [[nodiscard]] std::unique_ptr<net::Secure> client(std::string) override {
        return std::make_unique<Turning>(poison_);
    }

    [[nodiscard]] std::unique_ptr<net::Secure> server() override {
        return std::make_unique<Turning>();
    }

private:
    bool poison_ = false;
};

net::Gate standing() { return net::Gate{net::Address::loopback(net::Family::four, 0)}; }

struct Pair {
    net::Secured server;
    net::Secured client;
};

Pair guarded(const net::Gate& gate, const net::Guard& guard) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    auto there = net::Secured::server(std::move(one), guard, soon);
    auto here = net::Secured::client(std::move(two), guard, "gdziekolwiek", soon);

    net::Secured server = there.await();
    net::Secured client = here.await();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Szew: uścisk dłoni dochodzi do skutku po obu stronach") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    ought(both.server.held());
    ought(both.client.held());

    same(both.client.peer().port(), gate.mine().port());
}

TRIAL("Szew: treść przechodzi w obie strony") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.write(spelt("tajna wiadomość"), soon).await();

    same(spelling(both.server.read(1024, soon).await()), std::string{"tajna wiadomość"});

    both.server.write(spelt("i odpowiedź"), soon).await();

    same(spelling(both.client.read(1024, soon).await()), std::string{"i odpowiedź"});
}

TRIAL("Szew: sto tysięcy bajtów przechodzi w kawałkach") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    const std::string much(100000, 'z');

    auto writing = both.client.write(spelt(much), soon);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).await();

        if (one.empty()) break;

        made += spelling(one);
    }

    writing.await();

    same(made.size(), much.size());
    ought(made == much);
}

TRIAL("Szew: czytanie mniejsze niż rekord zostawia resztę na później") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.write(spelt("abcdefghij"), soon).await();

    same(spelling(both.server.read(4, soon).await()), std::string{"abcd"});
    same(spelling(both.server.read(4, soon).await()), std::string{"efgh"});
    same(spelling(both.server.read(4, soon).await()), std::string{"ij"});
}

TRIAL("Szew: zamknięcie drugiej strony daje pustkę") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.close();

    same(static_cast<int>(both.server.read(64, soon).await().size()), 0);
}

TRIAL("Szew: cisza kończy się terminem") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

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
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

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
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>(true)};

    Pair both = guarded(gate, guard);

    both.client.write(spelt("cokolwiek"), soon).await();

    bool amiss = false;

    try {
        (void)both.server.read(64, soon).await();
    } catch (const net::Trouble& why) {
        amiss = true;

        ought(std::string{why.what()}.find("Trucizna") != std::string::npos);
    }

    ought(amiss);
}

TRIAL("Szew: uścisk urwany w połowie odrzuca obietnicę") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    auto here = net::Secured::client(std::move(two), guard, "gdziekolwiek", soon);

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
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    auto reading = both.client.read(1024, soon);

    both.server.write(spelt("od serwera"), soon).await();
    both.client.write(spelt("od klienta"), soon).await();

    same(spelling(reading.await()), std::string{"od serwera"});
    same(spelling(both.server.read(1024, soon).await()), std::string{"od klienta"});
}

// Strażnik jest wartością, więc **można go nie mieć**. Pusty musi powiedzieć to wprost, a nie
// sięgnąć przez pusty wskaźnik przy pierwszej rundzie uścisku dłoni.
class Nothing : public net::Keeper {
public:
    [[nodiscard]] std::unique_ptr<net::Secure> client(std::string) override { return nullptr; }

    [[nodiscard]] std::unique_ptr<net::Secure> server() override { return nullptr; }
};

TRIAL("Szew: pusty strażnik odmawia z powodem") {
    const net::Gate gate = standing();

    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream two = joining.await();

    bool amiss = false;

    try {
        (void)net::Secured::client(std::move(two), net::Guard{}, "gdziekolwiek", soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Szew: strażnik oddający pustkę odmawia z powodem") {
    const net::Gate gate = standing();

    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream two = joining.await();

    const net::Guard guard{std::make_shared<Nothing>()};

    ought(guard.held());
    ought(guard.server() == nullptr);

    bool amiss = false;

    try {
        (void)net::Secured::server(std::move(two), guard, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

// Odmowy z `Secured` wychodziły **napisem**, nie kłopotem: `reject` opakowuje napis w `Rejected`,
// więc żadnej z nich nie łapało `catch (const Trouble&)`. Reszta biblioteki odmawia kłopotem
// i te cztery miejsca muszą robić tak samo.
TRIAL("Szew: odmowy wychodzą kłopotem, a nie samym napisem") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

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
        both.client.write(spelt("do zamkniętego"), soon).await();
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
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

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

// **Zamknięcie w trakcie pisania.** `Secured::close()` dotyka składowej, którą w tej samej chwili
// czyta robotnik puli prowadzący zapis — a `Stream` trzyma zwykły `shared_ptr`, nie atomowy.
// Zapis obok odczytu tego samego wskazania to UB, więc czytelnicy biorą dziś kopię spod zamka,
// a zamknięcie idzie pod tym samym zamkiem.
//
// Próba nie sprawdza wyniku zapisu — ten wolno odrzucić, i o to właśnie chodzi. Sprawdza, że
// **nikt się nie wywraca ani nie wiesza**, a pod TSan-em pokrywa ścieżkę, której cała reszta prób
// nie dotykała: bez niej sanitizer nie miał czego zobaczyć.
TRIAL("Szew: zamknięcie w trakcie zapisu nikogo nie wywraca") {
    const net::Gate gate = standing();

    const net::Guard guard{std::make_shared<Watching>()};

    for (int turn = 0; turn < 8; ++turn) {
        Pair both = guarded(gate, guard);

        // Więcej, niż zejdzie jednym ruchem — zapis ma jeszcze biec, gdy przyjdzie zamknięcie.
        Bytes much;

        much.reserve(512 * 1024);

        for (std::size_t at = 0; at < 512 * 1024; ++at) much.push_back(std::byte{0x41});

        Promise<void> wrote = both.client.write(std::move(much), soon);

        wrote.go();

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

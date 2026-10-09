#include "raced.hpp"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "talcite/time.hpp"

// **Wyścig adresów (RFC 8305).** Nazwa daje zwykle IPv6 i IPv4, a sieć, w której IPv6 jest
// zapowiedziane, ale nie dochodzi, nie odmawia — pakiety giną i każda próba czeka do końca
// cierpliwości. Dawniej adresy szły po kolei, więc TMDB za takim routerem odpowiadało po dziesięciu
// sekundach zamiast po ułamku. Teraz następny adres rusza, gdy poprzedni milczy dłużej niż `gap`,
// albo od razu, gdy wszystkie dotąd zaczęte odmówiły; wygrywa pierwszy, który się połączy,
// a reszta jest przerywana. Porażka wszystkich jest porażką ostatniego, jak dawniej.
namespace talcite::net {

std::vector<Address> interleaved(std::vector<Address> found) {
    if (found.empty()) return found;

    const Family first = found.front().family();

    std::vector<Address> own;
    std::vector<Address> other;

    for (Address& one : found) (one.family() == first ? own : other).push_back(std::move(one));

    std::vector<Address> made;

    made.reserve(own.size() + other.size());

    for (std::size_t at = 0; at < own.size() || at < other.size(); ++at) {
        if (at < own.size()) made.push_back(std::move(own[at]));
        if (at < other.size()) made.push_back(std::move(other[at]));
    }

    return made;
}

namespace {

// Stan jednego wyścigu. Trzyma go ciało obietnicy oddanej z `raced`; ogniwa prób i zegary sięgają
// po niego słabo, więc nic nie trzyma się w kółko. Ogony prób (`tails`) stan trzyma mocno, bo
// rodzic trzyma swoje ogniwa słabo i ogon bez właściciela zginąłby, zanim próba się skończy.
struct Race {
    std::vector<Address> order;
    Patience patience{};
    std::chrono::milliseconds gap{};

    Promise<Stream>::Resolve resolve;
    std::optional<Reject> reject;

    std::mutex guard;
    std::size_t next = 0;
    std::size_t lost = 0;
    bool over = false;
    std::exception_ptr last;
    std::vector<Promise<Stream>> tries;
    std::vector<Promise<void>> clocks;
    std::vector<Promise<void>> tails;
};

using Held = std::shared_ptr<Race>;

void launched(const Held& race);

// Koniec wyścigu: próby, które jeszcze trwają, i zegary przerywa się już poza zamkiem — przerwanie
// odrzuca, a odrzucenie wraca do `failed`, który bierze ten sam zamek.
void ended(const Held& race, std::unique_lock<std::mutex>& kept) {
    race->over = true;

    std::vector<Promise<Stream>> tries = std::move(race->tries);
    std::vector<Promise<void>> clocks = std::move(race->clocks);

    kept.unlock();

    for (Promise<Stream>& one : tries) one.cancel();
    for (Promise<void>& one : clocks) one.cancel();
}

void won(const Held& race, Stream stream) {
    std::unique_lock<std::mutex> kept{race->guard};

    if (race->over) {
        kept.unlock();

        stream.close();

        return;
    }

    const Promise<Stream>::Resolve resolve = race->resolve;

    ended(race, kept);

    resolve(std::move(stream));
}

void failed(const Held& race, std::exception_ptr why) {
    std::unique_lock<std::mutex> kept{race->guard};

    if (race->over) return;

    race->last = why;
    ++race->lost;

    if (race->lost == race->order.size()) {
        const Reject reject = *race->reject;
        const std::exception_ptr last = race->last;

        ended(race, kept);

        reject(last);

        return;
    }

    // Wszystkie zaczęte odmówiły — następny rusza od razu, bez czekania na zegar.
    const bool idle = race->lost == race->next && race->next < race->order.size();

    kept.unlock();

    if (idle) launched(race);
}

// Ogniwa prób i zegara budujemy poza zamkiem: odmowa od razu — gniazda nie da się otworzyć — rozstrzyga
// próbę, zanim `fail` wróci, a jego ciało bierze ten sam zamek.
void launched(const Held& race) {
    std::size_t at = 0;
    Address where;
    bool more = false;

    {
        const std::lock_guard<std::mutex> kept{race->guard};

        if (race->over || race->next >= race->order.size()) return;

        at = race->next++;
        where = race->order[at];
        more = race->next < race->order.size();
    }

    Promise<Stream> attempt = Stream::connect(where, race->patience);

    Promise<void> tail = attempt
                             .fail([weak = std::weak_ptr<Race>{race}](const std::exception&) {
                                 if (const Held alive = weak.lock()) failed(alive, std::current_exception());
                             })
                             .then([weak = std::weak_ptr<Race>{race}](Stream stream) {
                                 if (const Held alive = weak.lock()) won(alive, std::move(stream));
                                 else stream.close();
                             });

    std::optional<Promise<void>> clock;

    if (more) {
        // Zegar do następnej próby. Gdy w międzyczasie ruszyła już kolejna (odmowa przyspieszyła
        // kolejkę), zegar nie robi nic — `next` jest wtedy dalej niż u tego, kto go nastawiał.
        clock = Time::later(race->gap).then([weak = std::weak_ptr<Race>{race}, after = at + 1] {
            const Held alive = weak.lock();

            if (!alive) return;

            {
                const std::lock_guard<std::mutex> held{alive->guard};

                if (alive->over || alive->next != after) return;
            }

            launched(alive);
        });
    }

    bool late = false;

    {
        const std::lock_guard<std::mutex> kept{race->guard};

        late = race->over;

        if (!late) {
            race->tries.push_back(attempt);
            race->tails.push_back(tail);

            if (clock) race->clocks.push_back(*clock);
        }
    }

    // Wyścig rozstrzygnął się, zanim ta próba stanęła w stanie — nie ma już na co jej puszczać.
    if (late) {
        attempt.cancel();

        return;
    }

    tail.start();

    if (clock) clock->start();
}

}

Promise<Stream> raced(std::vector<Address> found, Patience patience, std::chrono::milliseconds gap) {
    if (found.empty())
        return Promise<Stream>{[](auto, auto reject) { reject(std::make_exception_ptr(Trouble{"There is no address."})); }};

    auto race = std::make_shared<Race>();

    race->order = interleaved(std::move(found));
    race->patience = patience;
    race->gap = gap;

    // Jak każda obietnica sieci — rusza od razu, a nie przy pierwszym czekaniu.
    Promise<Stream> made{[race](auto resolve, auto reject) {
        {
            const std::lock_guard<std::mutex> kept{race->guard};

            race->resolve = std::move(resolve);
            race->reject.emplace(std::move(reject));
        }

        launched(race);
    }};

    made.start();

    return made;
}

}

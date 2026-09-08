#include "talcite/secured.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <utility>

#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

namespace talcite::net {

struct Secured::Held {
    Stream one;
    std::unique_ptr<Secure> how;

    // Zabezpieczenie nie jest bezpieczne wątkowo i nie ma powodu, żeby było — dotykamy go
    // wyłącznie krótkimi, całymi porcjami: „wepchnij i zabierz", „zapisz i zabierz". Zamek
    // trzymamy tylko na czas takiej porcji, nigdy przez krok asynchroniczny.
    std::mutex minding;

    Bytes spare;

    // Wszystko, co wychodzi na gniazdo, ustawia się w jednej kolejce — bo strumień przyjmuje
    // jedno pisanie naraz, a uścisk dłoni potrafi pisać w środku czytania.
    std::mutex sending;
    Promise<void> tail{[](auto resolve, auto) { resolve(); }};
    int queued = 0;

    std::atomic<bool> reading{false};
    std::atomic<bool> writing{false};
};

namespace {

Promise<void> already() {
    return Promise<void>{[](auto resolve, auto) { resolve(); }};
}

Promise<Bytes> already(Bytes what) {
    return Promise<Bytes>{
        [what = std::move(what)](auto resolve, auto) mutable { resolve(std::move(what)); }};
}

// Zabezpieczenie oddaje szyfrogram porcjami, więc zabieramy wszystko, co ma.
Bytes drained(Secure& how) {
    Bytes made;

    for (;;) {
        Bytes one = how.pull();

        if (one.empty()) return made;

        made.insert(made.end(), one.begin(), one.end());
    }
}

void amiss(Secure& how) {
    if (const std::string said = how.amiss(); !said.empty()) throw Trouble{said};
}

// **Kopia strumienia spod zamka.** Sam `Stream` to wartość trzymająca `shared_ptr`, więc kopia
// wskazuje na ten sam byt, a kosztuje jedno podbicie licznika. Po co: `Secured::close()` woła na
// składowej `held->one` metodę, która przestawia jej `shared_ptr` — a to zwykły wskaźnik, nie
// atomowy. Robotnik puli czytający tę samą składową w tej samej chwili to niesynchronizowany
// odczyt obok zapisu, czyli UB: przerwany odczyt wskazania albo dekrementacja licznika bloku,
// który drugi wątek właśnie zwalnia.
//
// Odkąd **każde** dotknięcie składowej idzie pod `minding`, praca asynchroniczna biegnie na
// prywatnej kopii i zamka nie trzymamy przez żaden krok asynchroniczny — reguła spisana nad
// `minding` zostaje nienaruszona. Zamknięcie oryginału i tak dosięga kopii, bo obie dzielą jeden
// `Stream::Held`: uchwyt zostaje wymieniony na pusty, więc kopia dostaje przy następnym użyciu
// czyste odrzucenie, a nie zawieszenie.
Stream borrowed(const std::shared_ptr<Secured::Held>& held) {
    const std::lock_guard<std::mutex> kept{held->minding};

    return held->one;
}

// Kolejka jest łańcuchem: każde nowe pisanie doczepia się do poprzedniego, więc na gniazdo
// nigdy nie idą dwa naraz.
//
// **Ale łańcuch trzyma całą swoją historię.** `then` zapisuje w dziecku kopię rodzica (przez
// upstream_), więc ogon po tysiącu zapisów trzymałby tysiąc obietnic razem z ich buforami —
// wykrył to dopiero ASan, bo testy przechodziły. Dlatego liczymy, ile zapisów jest w drodze, i
// gdy ostatni schodzi, ogon wraca do świeżej, rozstrzygniętej obietnicy. Historia się ucina.
Promise<void> posting(const std::shared_ptr<Secured::Held>& held, Bytes what, Patience patience) {
    const std::lock_guard<std::mutex> kept{held->sending};

    ++held->queued;

    Promise<void> made = held->tail
                             .then([held, what = std::move(what), patience]() mutable {
                                 return borrowed(held).write(std::move(what), patience);
                             })
                             .finally([held] {
                                 const std::lock_guard<std::mutex> kept{held->sending};

                                 if (--held->queued == 0) held->tail = already();
                             });

    held->tail = made;

    made.go();

    return made;
}

Bytes taken(Secured::Held& held, std::size_t most) {
    Bytes made;

    if (held.spare.size() <= most) {
        made = std::move(held.spare);

        held.spare.clear();

        return made;
    }

    made.assign(held.spare.begin(), held.spare.begin() + static_cast<std::ptrdiff_t>(most));

    held.spare.erase(held.spare.begin(), held.spare.begin() + static_cast<std::ptrdiff_t>(most));

    return made;
}

using Clock = std::chrono::steady_clock;

Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// Ile rund uścisku dłoni wolno przeprowadzić. Prawdziwy uścisk mieści się w kilku; setka jest
// z ogromnym zapasem. Granica jest tu dlatego, że **każda runda zatrzymuje poprzednią**: łańcuch
// then trzyma całą swoją historię, więc rozmówca odpowiadający w kółko tuż przed terminem
// dokładał po kilkadziesiąt kilobajtów na rundę, bez końca.
constexpr int rounds = 100;

// Uścisk dłoni. Pytamy zabezpieczenie, co ma do wysłania, wysyłamy, czytamy odpowiedź, wpychamy
// — i od początku, aż powie, że gotowe. Wołanie pull() jest tym, co uścisk **napędza**: to jego
// pierwsze pytanie rusza całą wymianę.
//
// Terminem jest **chwila**, nie cierpliwość. Cierpliwość podawana każdej rundzie z osobna była
// terminem na operację, odnawialnym w nieskończoność — więc uścisk jako całość nie miał końca.
Promise<void> turning(const std::shared_ptr<Secured::Held>& held, Clock::time_point until,
                      int turn) {
    Bytes out;
    bool done = false;

    {
        const std::lock_guard<std::mutex> kept{held->minding};

        out = drained(*held->how);
        done = held->how->ready();

        amiss(*held->how);
    }

    Promise<void> sent = out.empty() ? already() : posting(held, std::move(out), left(until));

    if (done) return sent;

    if (turn >= rounds) throw Trouble{"Uścisk dłoni nie chce się skończyć."};

    return sent.then([held, until, turn] {
        return borrowed(held).read(65536, left(until)).then([held, until, turn](const Bytes& came) {
            if (came.empty()) throw Trouble{"Uścisk dłoni urwał się w połowie."};

            {
                const std::lock_guard<std::mutex> kept{held->minding};

                held->how->push(came);

                amiss(*held->how);
            }

            return turning(held, until, turn + 1);
        });
    });
}

Promise<Bytes> reading(const std::shared_ptr<Secured::Held>& held, std::size_t most,
                       Patience patience) {
    Bytes out;
    Bytes plain;

    {
        const std::lock_guard<std::mutex> kept{held->minding};

        if (!held->spare.empty()) return already(taken(*held, most));

        plain = held->how->read();
        out = drained(*held->how);

        amiss(*held->how);

        if (!plain.empty()) {
            held->spare.insert(held->spare.end(), plain.begin(), plain.end());

            plain = taken(*held, most);
        }
    }

    Promise<void> sent = out.empty() ? already() : posting(held, std::move(out), patience);

    if (!plain.empty()) return sent.then([plain = std::move(plain)]() mutable {
        return std::move(plain);
    });

    return sent.then([held, most, patience] {
        return borrowed(held).read(65536, patience).then([held, most, patience](const Bytes& came) {
            // Pustka znaczy to samo, co na gołym strumieniu: druga strona zamknęła.
            if (came.empty()) return already(Bytes{});

            {
                const std::lock_guard<std::mutex> kept{held->minding};

                held->how->push(came);

                amiss(*held->how);
            }

            return reading(held, most, patience);
        });
    });
}

// Zabiera strumień przez wartość i już go nie oddaje. Po opakowaniu nikt nie ma prawa dotknąć
// gniazda pod spodem — uścisk dłoni wpisałby się w środek cudzych danych.
//
// Pusty strażnik i strażnik oddający pustkę kończą się **odmową z powodem**. Wcześniej puste
// zabezpieczenie szło prosto do uścisku dłoni i program sięgał przez pusty wskaźnik.
Promise<Secured> opened(Stream one, std::unique_ptr<Secure> how, Patience patience) {
    if (!how)
        return Promise<Secured>{[](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"Strażnik nie dał zabezpieczenia."}));
        }};

    auto held = std::make_shared<Secured::Held>();

    held->one = std::move(one);
    held->how = std::move(how);

    Promise<Secured> made = turning(held, Clock::now() + patience, 0).then([held] {
        return Secured{held};
    });

    made.go();

    return made;
}

}

// Strażnik jest **wartością**, jak wszystko inne w talcite, a to, co pisze przejściówka, siedzi za
// nim jako `Keeper`. Stanu prywatnego jest tu dokładnie jeden wskaźnik, więc nie ma czego chować
// za `Held`: schowek dokładałby alokację i skok, nie ukrywając niczego, czego klasa abstrakcyjna
// już nie ukrywa.
Guard::Guard(std::shared_ptr<Keeper> how) : how_(std::move(how)) {}

std::unique_ptr<Secure> Guard::client(std::string host) const {
    return how_ ? how_->client(std::move(host)) : nullptr;
}

std::unique_ptr<Secure> Guard::server() const { return how_ ? how_->server() : nullptr; }

Promise<Secured> Secured::client(Stream one, Guard guard, std::string host, Patience patience) {
    return opened(std::move(one), guard.client(std::move(host)), patience);
}

Promise<Secured> Secured::server(Stream one, Guard guard, Patience patience) {
    return opened(std::move(one), guard.server(), patience);
}

// Pusty uchwyt oddaje pusty adres, a nie naruszenie pamięci — ta sama reguła i ten sam powód co
// w strumieniu: `close()` zeruje uchwyt, a klasa wyraźnie zamierza być używalna po zamknięciu.
namespace {

const Address& nowhere() {
    static const Address one;

    return one;
}

}

// Czytelnicy tej samej składowej, więc też pod zamkiem. Oddawane wskazanie prowadzi w głąb
// `Stream::Held`, który żyje tak długo, jak nasz `Held` — a ten trzyma wołający.
const Address& Secured::peer() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.peer();
}

const Address& Secured::mine() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.mine();
}

Promise<Bytes> Secured::read(std::size_t most, Patience patience) const {
    if (!held_)
        return Promise<Bytes>{
            [](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"Połączenie jest zamknięte."}));
        }};

    bool was = false;

    if (!held_->reading.compare_exchange_strong(was, true))
        return Promise<Bytes>{
            [](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"Już czytam."}));
        }};

    auto held = held_;

    // `reading` potrafi rzucić synchronicznie — robi to zawsze, gdy zabezpieczenie ma już zapisany
    // kłopot. Wyjątek wypuszczony stąd omijał `finally`, zostawiał znacznik ustawiony i połączenie
    // już nigdy nie czytało. Funkcja oddająca obietnicę ma zgłaszać błędy obietnicą, nie rzutem.
    try {
        Promise<Bytes> made = reading(held, most, patience).finally([held] {
            held->reading.store(false);
        });

        made.go();

        return made;
    } catch (...) {
        held->reading.store(false);

        std::exception_ptr why = std::current_exception();

        return Promise<Bytes>{[why](auto, auto reject) { reject(why); }};
    }
}

Promise<void> Secured::write(Bytes what, Patience patience) const {
    if (!held_)
        return Promise<void>{
            [](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"Połączenie jest zamknięte."}));
        }};

    bool was = false;

    if (!held_->writing.compare_exchange_strong(was, true))
        return Promise<void>{[](auto, auto reject) { reject(std::make_exception_ptr(Trouble{"Już piszę."})); }};

    auto held = held_;

    Bytes out;

    try {
        const std::lock_guard<std::mutex> kept{held->minding};

        held->how->write(std::move(what));

        out = drained(*held->how);

        amiss(*held->how);
    } catch (...) {
        held->writing.store(false);

        std::exception_ptr why = std::current_exception();

        return Promise<void>{[why](auto, auto reject) { reject(why); }};
    }

    Promise<void> made =
        (out.empty() ? already() : posting(held, std::move(out), patience)).finally([held] {
            held->writing.store(false);
        });

    made.go();

    return made;
}

// Strumień zamykamy **pod tym samym zamkiem, co zabezpieczenie** — nie dlatego, że zamknięcie
// samo w sobie jest niebezpieczne, tylko dlatego, że przestawia `shared_ptr` w składowej, którą
// w tej samej chwili może czytać robotnik puli. Odkąd czytelnicy biorą kopię spod `minding`,
// wystarczy, że zapis też jest pod nim.
void Secured::close() {
    if (!held_) return;

    {
        const std::lock_guard<std::mutex> kept{held_->minding};

        held_->how->close();
        held_->one.close();
    }

    held_.reset();
}

}

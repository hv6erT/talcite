#include "talcite/tls.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

namespace talcite::net {

TlsTrouble::TlsTrouble(Why why, const std::string& said, std::string host, Address where)
    : Trouble(said, 0, std::move(where)), why_(why), host_(std::move(host)) {}

struct TlsStream::Held {
    Stream one;
    std::unique_ptr<TlsSession> how;

    // Nazwa, pod którą klient pytał serwer — do kłopotu z uściskiem, bo człowiek zna serwer po nazwie,
    // a nie po adresie. Po stronie serwera pusta.
    std::string host;

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
Bytes drained(TlsSession& how) {
    Bytes made;

    for (;;) {
        Bytes one = how.pull();

        if (one.empty()) return made;

        made.insert(made.end(), one.begin(), one.end());
    }
}

// Kłopot zabezpieczenia wychodzi jako `TlsTrouble`: z powodem, który rozpoznała przejściówka
// (szyfry, zaufanie, termin, nazwa), z nazwą serwera i z adresem drugiej strony. Woła się pod
// `minding`, więc i strumień czytamy bezpiecznie.
void amiss(const TlsStream::Held& held) {
    if (const std::string said = held.how->error(); !said.empty())
        throw TlsTrouble{held.how->why(), said, held.host, held.one.peer()};
}

// **Kopia strumienia spod zamka.** Sam `Stream` to wartość trzymająca `shared_ptr`, więc kopia
// wskazuje na ten sam byt, a kosztuje jedno podbicie licznika. Po co: `TlsStream::close()` woła na
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
Stream borrowed(const std::shared_ptr<TlsStream::Held>& held) {
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
Promise<void> posting(const std::shared_ptr<TlsStream::Held>& held, Bytes what, Patience patience) {
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

    made.start();

    return made;
}

Bytes taken(TlsStream::Held& held, std::size_t most) {
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
Promise<void> turning(const std::shared_ptr<TlsStream::Held>& held, Clock::time_point until,
                      int turn) {
    Bytes out;
    bool done = false;

    {
        const std::lock_guard<std::mutex> kept{held->minding};

        out = drained(*held->how);
        done = held->how->ready();

        amiss(*held);
    }

    Promise<void> sent = out.empty() ? already() : posting(held, std::move(out), left(until));

    if (done) return sent;

    if (turn >= rounds) throw Trouble{"The handshake will not finish."};

    return sent.then([held, until, turn] {
        return borrowed(held).read(65536, left(until)).then([held, until, turn](const Bytes& came) {
            if (came.empty()) throw Trouble{"The handshake broke off halfway."};

            {
                const std::lock_guard<std::mutex> kept{held->minding};

                held->how->push(came);

                amiss(*held);
            }

            return turning(held, until, turn + 1);
        });
    });
}

Promise<Bytes> reading(const std::shared_ptr<TlsStream::Held>& held, std::size_t most,
                       Patience patience) {
    Bytes out;
    Bytes plain;

    {
        const std::lock_guard<std::mutex> kept{held->minding};

        if (!held->spare.empty()) return already(taken(*held, most));

        plain = held->how->read();
        out = drained(*held->how);

        amiss(*held);

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

                amiss(*held);
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
Promise<TlsStream> opened(Stream one, std::unique_ptr<TlsSession> how, std::string host, Patience patience) {
    if (!how)
        return Promise<TlsStream>{[](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The provider gave no session."}));
        }};

    auto held = std::make_shared<TlsStream::Held>();

    held->one = std::move(one);
    held->how = std::move(how);
    held->host = std::move(host);

    Promise<TlsStream> made = turning(held, Clock::now() + patience, 0).then([held] {
        return TlsStream{held};
    });

    made.start();

    return made;
}

// Brak dostawcy odrzuca obietnicę, zamiast rzucać wołającemu w twarz — funkcja oddająca obietnicę
// zgłasza błędy tą samą drogą, co reszta modułu. Typ jest ten sam, który rzuca `Seam::provider()`.
Promise<TlsStream> unbound() {
    return Promise<TlsStream>{[](auto, auto reject) {
        reject(std::make_exception_ptr(Unbound{TlsProvider::kind()}));
    }};
}

}

Promise<TlsStream> TlsStream::client(Stream one, Seam<TlsProvider> tls, std::string host, Patience patience) {
    if (!tls.bound()) return unbound();

    std::unique_ptr<TlsSession> how = tls.provider().client(host);

    return opened(std::move(one), std::move(how), std::move(host), patience);
}

Promise<TlsStream> TlsStream::server(Stream one, Seam<TlsProvider> tls, Patience patience) {
    if (!tls.bound()) return unbound();

    return opened(std::move(one), tls.provider().server(), {}, patience);
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
const Address& TlsStream::peer() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.peer();
}

const Address& TlsStream::mine() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.mine();
}

Promise<Bytes> TlsStream::read(std::size_t most, Patience patience) const {
    if (!held_)
        return Promise<Bytes>{
            [](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The connection is closed."}));
        }};

    bool was = false;

    if (!held_->reading.compare_exchange_strong(was, true))
        return Promise<Bytes>{
            [](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"Already reading."}));
        }};

    auto held = held_;

    // `reading` potrafi rzucić synchronicznie — robi to zawsze, gdy zabezpieczenie ma już zapisany
    // kłopot. Wyjątek wypuszczony stąd omijał `finally`, zostawiał znacznik ustawiony i połączenie
    // już nigdy nie czytało. Funkcja oddająca obietnicę ma zgłaszać błędy obietnicą, nie rzutem.
    try {
        Promise<Bytes> made = reading(held, most, patience).finally([held] {
            held->reading.store(false);
        });

        made.start();

        return made;
    } catch (...) {
        held->reading.store(false);

        std::exception_ptr why = std::current_exception();

        return Promise<Bytes>{[why](auto, auto reject) { reject(why); }};
    }
}

Promise<void> TlsStream::write(Bytes what, Patience patience) const {
    if (!held_)
        return Promise<void>{
            [](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The connection is closed."}));
        }};

    bool was = false;

    if (!held_->writing.compare_exchange_strong(was, true))
        return Promise<void>{[](auto, auto reject) { reject(std::make_exception_ptr(Trouble{"Already writing."})); }};

    auto held = held_;

    Bytes out;

    try {
        const std::lock_guard<std::mutex> kept{held->minding};

        held->how->write(std::move(what));

        out = drained(*held->how);

        amiss(*held);
    } catch (...) {
        held->writing.store(false);

        std::exception_ptr why = std::current_exception();

        return Promise<void>{[why](auto, auto reject) { reject(why); }};
    }

    Promise<void> made =
        (out.empty() ? already() : posting(held, std::move(out), patience)).finally([held] {
            held->writing.store(false);
        });

    made.start();

    return made;
}

// Strumień zamykamy **pod tym samym zamkiem, co zabezpieczenie** — nie dlatego, że zamknięcie
// samo w sobie jest niebezpieczne, tylko dlatego, że przestawia `shared_ptr` w składowej, którą
// w tej samej chwili może czytać robotnik puli. Odkąd czytelnicy biorą kopię spod `minding`,
// wystarczy, że zapis też jest pod nim.
void TlsStream::close() {
    if (!held_) return;

    {
        const std::lock_guard<std::mutex> kept{held_->minding};

        held_->how->close();
        held_->one.close();
    }

    held_.reset();
}

}

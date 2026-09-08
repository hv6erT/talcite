#include "talcite/stream.hpp"

#include <atomic>
#include <mutex>
#include <utility>

#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

#include "handing.hpp"
#include "inside.hpp"
#include "starting.hpp"

namespace talcite::net {

struct Stream::Held {
    ~Held() {
        close();

        retiring(numbered);
    }

    // Zamknięcie musi być jednorazowe: uchwyt oddany centrali dwa razy zostałby zamknięty dwa
    // razy, a drugie zamknięcie trafiłoby w cudze, świeżo otwarte gniazdo o tym samym numerze.
    void close() {
        if (const Handle one = which.exchange(nothing); one != nothing) forget(one);
    }

    // **Numer przeżywa zamknięcie i ginie dopiero razem z uchwytem.** Zgłoszenia w locie trzymają
    // uchwyt przez `kept`, więc deskryptor zamyka się dopiero wtedy, gdy nikt nie może już ani go
    // zgłosić, ani na nim czytać. Zamykany wcześniej wracał jako numer świeżo otwartego gniazda —
    // i wtedy ktoś czytał z cudzego połączenia.
    Handle numbered = nothing;

    std::atomic<Handle> which{nothing};
    Address peer;
    Address mine;

    // **Kto** trzyma dany kierunek, a nie „czy ktoś go trzyma" — powód opisany niżej, przy wzorze
    // zdejmowania. Gołe wskaźniki, bo to są wyłącznie znaczniki tożsamości: nigdy nie sięgamy przez
    // nie do obiektu. Zgłoszenia żyją własnym uchwytem w centrali i przez `kept`.
    std::mutex guard;
    const void* reading = nullptr;
    const void* writing = nullptr;
};

namespace {

std::chrono::steady_clock::time_point until(Patience patience) {
    return std::chrono::steady_clock::now() + patience;
}

// Jedno czytanie i jedno pisanie naraz. To nie jest oszczędność, tylko ochrona: dwa czytania na
// jednym strumieniu przepletłyby bajty i rozjechały wszystko, co po nich idzie. Drugie zgłoszenie
// dostaje odmowę, zamiast po cichu psuć dane.
template <typename T>
Promise<T> busy() {
    auto made = std::make_shared<Result<T>>();

    spoiled(*made, std::make_exception_ptr(Trouble{"Strumień jest już zajęty w tę stronę."}));

    return HandPromise<T>{made};
}

template <typename T>
Promise<T> amiss_now(const std::string& said, int code = 0, Address where = {}) {
    auto made = std::make_shared<Result<T>>();

    spoiled(*made, std::make_exception_ptr(Trouble{said, code, std::move(where)}));

    return HandPromise<T>{made};
}

// Zgłoszenie sieciowe jest **jednocześnie biletem obietnicy**, więc anulowanie musi je zdjąć
// z centrali, a nie tylko odrzucić. Wzór jest we wszystkich sześciu ten sam.
//
// Anulowanie zwalnia kierunek **od razu**, bez oglądania się na to, co powiedziało `withdraw` —
// bo to jest jedyna chwila, w której wołający się o tym dowiaduje. Zaraz po anulowaniu ma prawo
// poprosić o następne czytanie i ma je dostać, a nie usłyszeć „zajęty".
//
// Dwa zaplecza kończą przy tym inaczej i **na tym polegał błąd**. Na uniksach `withdraw` wyjmuje
// zgłoszenie z mapy i oddaje prawdę, więc zwolnienie na miejscu było oczywiste. Na Windows
// `CancelIoEx` tylko prosi — zakończenie i tak przyjdzie portem — więc `withdraw` nie mówi prawdy
// **nigdy**, a znacznik zdejmowało dopiero spóźnione `done()` na wątku puli. Między anulowaniem
// a tą chwilą kierunek zostawał zajęty i następna prośba dostawała odmowę bez powodu.
//
// Dlatego znacznik nie jest flagą, tylko **tożsamością**: zwalnia go wyłącznie ten, kto go zajął.
// Spóźnione zakończenie anulowanej operacji zastaje wtedy cudzą tożsamość i nie rusza niczego —
// bez tego zdjęłoby znacznik należący już do następnej operacji.
//
// Zakończenie może też przyjść **przed** naszym odrzuceniem i rozstrzygnąć bilet pierwsze —
// wołający dostałby wtedy kłopot z numerem przerwania zamiast `Cancelled`. Od tego jest `quitted`:
// ustawione przed zdejmowaniem, każe spóźnionemu zakończeniu posprzątać i zamilknąć.
struct JoinPromise : Doing, Result<Stream> {
    std::shared_ptr<Stream::Held> mine;

    // Nawiązanie nie ma znacznika kierunku, a mimo to zdjąć się musi: zgłoszenie leżące
    // w centrali trzyma `kept`, czyli **otwarte gniazdo**. Porzucone łączenie bez terminu
    // zostawiało je otwarte do końca programu.
    void dropped() override { (void)withdraw(shared_from_this()); }

    void done(std::size_t, int code) override {
        if (quitted.load()) return;

        if (code != 0) {
            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Nie mogę się połączyć: " + spoken(code) + ".", code,
                                       mine->peer}));

            return;
        }

        // Adres rozmówcy znamy od początku — to jest ten, pod który się łączyliśmy. Pytamy
        // system, bo przy nazwie domenowej i przy rodzinie podwójnej może się okazać inny, ale
        // **jego milczenie nie ma prawa wymazać tego, co już wiemy**.
        if (const Address said = named(held, true); said.held()) mine->peer = said;

        mine->mine = named(held, false);

        settled(*this, Stream{mine});
    }

    void late() override {
        if (quitted.load()) return;

        spoiled(*this, std::make_exception_ptr(Overdue{}));
    }
};

struct ReadPromise : Doing, Result<Bytes> {
    std::shared_ptr<Stream::Held> mine;

    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->reading == this) mine->reading = nullptr;
    }

    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    // Bajty, które przyszły, są ważniejsze od tego, jak operacja się skończyła. Przy IOCP
    // przerwane czytanie potrafi mieć już przepisaną część treści — oddanie jej zamiast wyjątku
    // sprawia, że **żaden bajt nie ginie** i strumień nigdy nie wchodzi w stan nieznany.
    //
    // Anulowanie jest tu jedynym wyjątkiem i to wyjątkiem świadomym: kto sam przerwał czytanie,
    // ten powiedział, że treść go już nie obchodzi, a odebrane bajty nie mają dokąd pójść.
    void done(std::size_t much, int code) override {
        yielded();

        if (quitted.load()) return;

        if (much > 0) {
            buffer.resize(much);

            settled(*this, std::move(buffer));

            return;
        }

        if (code != 0) {
            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Nie mogę czytać: " + spoken(code) + ".", code,
                                       mine->peer}));

            return;
        }

        // Zero bajtów bez błędu znaczy dokładnie jedno: druga strona zamknęła.
        buffer.clear();

        settled(*this, std::move(buffer));
    }

    // Termin z pustymi rękami odrzuca. Gdyby spełniał pustką, nie dałoby się odróżnić ciszy od
    // zamknięcia przez drugą stronę — a to są dwie zupełnie różne wiadomości.
    void late() override {
        yielded();

        if (quitted.load()) return;

        spoiled(*this, std::make_exception_ptr(Overdue{}));
    }
};

struct WritePromise : Doing, Result<void> {
    std::shared_ptr<Stream::Held> mine;

    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->writing == this) mine->writing = nullptr;
    }

    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    void done(std::size_t much, int code) override {
        yielded();

        if (quitted.load()) return;

        if (code != 0) {
            // Ile bajtów zdążyło zejść przed zerwaniem, mówimy **wprost**. Bufor został
            // przeniesiony i już go nie ma, więc bez tej liczby wołający nie wie, czy wznawiać od
            // zera, czy wcale — a ponowienie całości znaczy dla drugiej strony treść podwojoną.
            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Nie mogę pisać: " + spoken(code) + ". Zeszło " +
                                           std::to_string(much) + " z " +
                                           std::to_string(buffer.size()) + " bajtów.",
                                       code, mine->peer}));

            return;
        }

        settled(*this);
    }

    void late() override {
        yielded();

        if (quitted.load()) return;

        spoiled(*this, std::make_exception_ptr(Overdue{}));
    }
};

}

// Nawiązania **nie zaczynamy tutaj**. Na uniksach jest to nieblokujący connect, na Windows
// ConnectEx z zakończeniem w porcie — a to jest wiedza zaplecza, nie strumienia. Zgłaszamy
// zamiar, a centrala wie, jak go ruszyć.
Promise<Stream> Stream::connect(Address where, Patience patience) {
    if (!where.held()) return amiss_now<Stream>("Pusty adres nie jest celem.");

    auto mine = std::make_shared<Held>();

    mine->which = opened(where.family(), true);
    mine->numbered = mine->which.load();

    if (mine->which.load() == nothing)
        return amiss_now<Stream>("Nie mogę otworzyć gniazda: " + spoken(net::amiss()) + ".",
                                 net::amiss(), where);

    mine->peer = where;

    auto one = std::make_shared<JoinPromise>();

    one->mine = mine;
    one->kept = mine;
    one->held = mine->which.load();
    one->kind = Kind::joining;
    one->where = where;
    one->ever = patience.count() > 0;
    one->until = until(patience);

    submit(one);

    return HandPromise<Stream>{one};
}

// Pusty adres oddawany wtedy, gdy uchwytu już nie ma. Wszystkie pozostałe metody odpowiadają po
// zamknięciu grzecznie, więc te też muszą — sięganie po nieistniejący uchwyt kończyło się
// naruszeniem pamięci, a klasa wyraźnie zamierza być używalna po close().
namespace {

const Address& nowhere() {
    static const Address one;

    return one;
}

}

// Brama przyjęła połączenie i ma goły uchwyt, ale Held zna tylko ten plik — stąd wolna funkcja
// zamiast metody. Publiczny konstruktor Stream nikomu nie szkodzi: Held jest typem niepełnym,
// więc z zewnątrz nie da się go zbudować.
Stream greeting(Handle one, Address from) {
    auto mine = std::make_shared<Stream::Held>();

    mine->which = one;
    mine->numbered = one;
    mine->peer = std::move(from);
    mine->mine = named(one, false);

    return Stream{mine};
}

const Address& Stream::peer() const { return held_ ? held_->peer : nowhere(); }

const Address& Stream::mine() const { return held_ ? held_->mine : nowhere(); }

Promise<Bytes> Stream::read(std::size_t most, Patience patience) const {
    if (!held_) return amiss_now<Bytes>("Strumień jest zamknięty.");

    // Zerowe czytanie **nie jest końcem strumienia**, a tak wyglądało: recv z zerową długością
    // oddaje zero także na zdrowym gnieździe, a zero bez błędu znaczy u nas „druga strona
    // zamknęła". Warstwa ramkująca trafiała na to na granicy ramki.
    if (most == 0) return amiss_now<Bytes>("Zerowe czytanie niczego nie powie.");

    // Przydział bufora idzie **przed** zajęciem znacznika. Rozmiar podaje wołający, więc resize
    // potrafi rzucić — a wyjątek rzucony po zajęciu zostawiał znacznik ustawiony na zawsze i
    // gniazdo już nigdy nie czytało, bo nie powstawało zgłoszenie, które by go zdjęło.
    auto one = std::make_shared<ReadPromise>();

    one->buffer.resize(most);

    // Zajęcie kierunku i zapamiętanie, czyj on jest, pod jednym zamkiem: inaczej dwa czytania
    // zaczęte naraz zapisałyby się jedno po drugim.
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->reading != nullptr) return busy<Bytes>();

        held_->reading = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::getting;
    one->ever = patience.count() > 0;
    one->until = until(patience);

    submit(one);

    return HandPromise<Bytes>{one};
}

Promise<void> Stream::write(Bytes what, Patience patience) const {
    if (!held_) return amiss_now<void>("Strumień jest zamknięty.");

    auto one = std::make_shared<WritePromise>();

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->writing != nullptr) return busy<void>();

        held_->writing = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::putting;
    one->buffer = std::move(what);
    one->ever = patience.count() > 0;
    one->until = until(patience);

    submit(one);

    return HandPromise<void>{one};
}

// Algorytm Nagle'a jest własnością TCP, a gniazdo lokalne nie ma z TCP nic wspólnego — tam to
// wywołanie odmówiłoby po cichu, a cicha odmowa jest gorsza od niezrobienia niczego.
void Stream::nagle(bool given) const {
    if (!held_ || held_->peer.family() == Family::local) return;

    nagling(held_->which.load(), given);
}

void Stream::close() {
    if (!held_) return;

    held_->close();
    held_.reset();
}

}

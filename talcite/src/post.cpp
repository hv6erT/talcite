#include "talcite/post.hpp"

#include <atomic>
#include <mutex>
#include <utility>

#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

#include "handing.hpp"
#include "inside.hpp"
#include "starting.hpp"

namespace talcite::net {

struct Post::Held {
    ~Held() {
        close();

        retiring(numbered);
    }

    void close() {
        if (const Handle one = which.exchange(nothing); one != nothing) forget(one);
    }

    // **Numer przeżywa zamknięcie i ginie dopiero razem z uchwytem.** Zgłoszenia w locie trzymają
    // uchwyt przez `kept`, więc deskryptor zamyka się dopiero wtedy, gdy nikt nie może już ani go
    // zgłosić, ani na nim czytać. Zamykany wcześniej wracał jako numer świeżo otwartego gniazda —
    // i wtedy ktoś czytał z cudzego połączenia.
    Handle numbered = nothing;

    std::atomic<Handle> which{nothing};
    Address mine;

    // **Kto** trzyma dany kierunek, a nie „czy ktoś go trzyma" — powód i cały wzór opisane przy
    // zdejmowaniu w strumieniu. Gołe wskaźniki, bo to są wyłącznie znaczniki tożsamości: nigdy nie
    // sięgamy przez nie do obiektu. Zgłoszenia żyją własnym uchwytem w centrali i przez `kept`.
    std::mutex guard;
    const void* fetching = nullptr;
    const void* sending = nullptr;
};

namespace {

template <typename T>
Promise<T> amiss_now(const std::string& said, int code = 0, Address where = {}) {
    auto made = std::make_shared<Result<T>>();

    spoiled(*made, std::make_exception_ptr(Trouble{said, code, std::move(where)}));

    return HandPromise<T>{made};
}

struct FetchPromise : Doing, Result<Parcel> {
    std::shared_ptr<Post::Held> mine;

    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->fetching == this) mine->fetching = nullptr;
    }

    // Wzór zdejmowania, tożsamość znacznika i zasłona `quitted` — opisane przy nawiązaniu
    // w strumieniu.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    void done(std::size_t much, int code) override {
        yielded();

        if (quitted.load()) return;

        if (code != 0) {
            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Nie mogę odebrać: " + spoken(code) + ".", code,
                                       mine->mine}));

            return;
        }

        // Obcięcie zgłaszamy jako kłopot, zamiast oddać kawałek udający całość.
        if (cut) {
            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Datagram nie zmieścił się w buforze.", 0, where}));

            return;
        }

        Parcel made;

        made.from = where;
        made.bytes = std::move(buffer);
        made.bytes.resize(much);

        settled(*this, std::move(made));
    }

    void late() override {
        yielded();

        if (quitted.load()) return;

        spoiled(*this, std::make_exception_ptr(Overdue{}));
    }
};

struct SendPromise : Doing, Result<void> {
    std::shared_ptr<Post::Held> mine;

    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->sending == this) mine->sending = nullptr;
    }

    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    void done(std::size_t much, int code) override {
        yielded();

        if (quitted.load()) return;

        if (code != 0) {
            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Nie mogę wysłać: " + spoken(code) + ".", code, where}));

            return;
        }

        // Krótszy datagram niż podany nie zdarza się — a gdyby się zdarzył, znaczyłby, że poszła
        // paczka, której nikt nie zamawiał. Lepiej powiedzieć o tym wprost.
        if (much != buffer.size()) {
            spoiled(*this, std::make_exception_ptr(Trouble{"Datagram wyszedł ucięty.", 0, where}));

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

// Poczta, tak jak brama, nie oddaje obietnicy: otwarcie gniazda i przypisanie portu kończą się
// od razu. Adres bez portu znaczy „daj mi jakikolwiek" — tak robi się stronę wysyłającą.
Post::Post(Address where) : held_(std::make_shared<Held>()) {
    if (!where.held()) throw Trouble{"Pusty adres nie jest miejscem dla poczty."};

    // Poczta jest datagramowa, a gniazdo lokalne datagramów nie ma — Windows nie daje ich wcale,
    // więc nie dajemy ich nigdzie. Odmawiamy wprost, zamiast oddawać numer błędu systemu, z
    // którego nie widać, że rzecz jest niemożliwa z założenia, a nie nieudana tym razem.
    if (where.family() == Family::local)
        throw Trouble{"Poczta nie działa na gnieździe lokalnym — ono jest tylko strumieniowe."};

    const Handle one = opened(where.family(), false);

    if (one == nothing)
        throw Trouble{"Nie mogę otworzyć gniazda: " + spoken(amiss()) + ".", amiss(), where};

    held_->which = one;
    held_->numbered = one;

    sharing(one);

    if (!bound(one, where)) {
        const int why = amiss();

        held_->close();
        held_.reset();

        throw Trouble{"Nie mogę zająć adresu: " + spoken(why) + ".", why, where};
    }

    held_->mine = named(one, false);
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

const Address& Post::mine() const { return held_ ? held_->mine : nowhere(); }

Promise<Parcel> Post::receive(std::size_t most, Patience patience) const {
    if (!held_) return amiss_now<Parcel>("Poczta jest zamknięta.");

    // Przydział bufora idzie **przed** zajęciem znacznika. Rozmiar podaje wołający, więc resize
    // potrafi rzucić — a wyjątek rzucony po zajęciu zostawiał znacznik ustawiony na zawsze i
    // gniazdo już nigdy nie czytało, bo nie powstawało zgłoszenie, które by go zdjęło.
    auto one = std::make_shared<FetchPromise>();

    one->buffer.resize(most);

    // Zajęcie kierunku i zapamiętanie, czyj on jest, pod jednym zamkiem: inaczej dwa odbierania
    // zaczęte naraz zapisałyby się jedno po drugim.
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->fetching != nullptr) return amiss_now<Parcel>("Poczta już odbiera.");

        held_->fetching = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::fetching;
    one->ever = patience.count() > 0;
    one->until = std::chrono::steady_clock::now() + patience;

    submit(one);

    return HandPromise<Parcel>{one};
}

Promise<void> Post::send(Address to, Bytes what, Patience patience) const {
    if (!held_) return amiss_now<void>("Poczta jest zamknięta.");

    if (!to.held()) return amiss_now<void>("Pusty adres nie jest celem.");

    auto one = std::make_shared<SendPromise>();

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->sending != nullptr) return amiss_now<void>("Poczta już wysyła.");

        held_->sending = one.get();
    }

    one->mine = held_;
    one->kept = held_;
    one->held = held_->which.load();
    one->kind = Kind::sending;
    one->where = std::move(to);
    one->buffer = std::move(what);
    one->ever = patience.count() > 0;
    one->until = std::chrono::steady_clock::now() + patience;

    submit(one);

    return HandPromise<void>{one};
}

void Post::close() {
    if (!held_) return;

    held_->close();
    held_.reset();
}

}

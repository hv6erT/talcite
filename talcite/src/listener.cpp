#include "talcite/listener.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "handing.hpp"
#include "inside.hpp"
#include "piping.hpp"
#include "starting.hpp"

namespace talcite::net {

struct Listener::Held {
    ~Held() {
        close();

        retiring(numbered);
    }

    // **Brama lokalna sprząta po sobie plik.** Zostawiony myli następne uruchomienie: wygląda jak
    // żywe gniazdo, a nie jest — i choć `listening` umie go rozpoznać po odmowie połączenia,
    // zostawianie śmieci w katalogu użytkownika nie jest naszą rolą. Kasujemy **przy zamknięciu**,
    // a nie z ostatnim uchwytem: ten ginie dopiero, gdy wrócą anulowane przyjęcia, a do tego czasu
    // pod tą ścieżką mogła już stanąć nowa brama — i straciłaby plik. Deskryptorowi kasowanie
    // ścieżki nie szkodzi.
    //
    // Brama rury nie ma gniazda nasłuchującego ani pliku: zamyka się wolna instancja, a czekające
    // przyjęcie dostaje anulowanie na swojej instancji.
    void close() {
        if (const Handle one = which.exchange(nothing); one != nothing) {
            forget(one);

            if (mine.family() == Family::local) (void)std::remove(mine.path().c_str());
        }

        if (!gate) return;

        pipes::shut(*gate);

        const std::lock_guard<std::mutex> kept{guard};

        if (waiting != nothing) forget(waiting);
    }

    // **Numer przeżywa zamknięcie i ginie dopiero razem z uchwytem.** Zgłoszenia w locie trzymają
    // uchwyt przez `kept`, więc deskryptor zamyka się dopiero wtedy, gdy nikt nie może już ani go
    // zgłosić, ani na nim czytać. Zamykany wcześniej wracał jako numer świeżo otwartego gniazda —
    // i wtedy ktoś czytał z cudzego połączenia.
    Handle numbered = nothing;

    std::atomic<Handle> which{nothing};
    Address mine;

    // Centrala trzyma **jedno** zgłoszenie na gniazdo na kierunek, więc drugie przyjmowanie
    // zaczęte w trakcie pierwszego nadpisywało je bez śladu: pierwsze nie dostawało ani
    // zakończenia, ani terminu, znikało z mapy — a razem z nim znikała cierpliwość — i obietnica
    // nie rozstrzygała się nigdy, trzymając przy tym bramę i jej deskryptor do końca programu.
    //
    // Pamiętamy **kto** to miejsce trzyma, a nie „czy ktoś je trzyma". Bez tożsamości nie da się
    // zwolnić go przy anulowaniu, a anulowanie jest jedyną chwilą, w której wołający się o tym
    // dowiaduje: na IOCP anulowana operacja nie znika, tylko wraca portem jeszcze długo potem, więc
    // gołą flagę wolno byłoby skasować dopiero wtedy — i skasowałoby się cudzą, gdyby w międzyczasie
    // ktoś zdążył ruszyć.
    //
    // Goły wskaźnik, bo to jest wyłącznie znacznik tożsamości: nigdy nie sięgamy przez niego do
    // obiektu. Zgłoszenie żyje własnym uchwytem w centrali i przez `kept`.
    std::mutex guard;
    const void* meeting = nullptr;

    // Rura (`piping.hpp`): brama i instancja, na której czeka przyjęcie — pod tym samym zamkiem.
    std::shared_ptr<pipes::Gate> gate;
    Handle waiting = nothing;
};

namespace {

struct MeetPromise : Doing, detail::Result<Stream> {
    std::shared_ptr<Listener::Held> mine;

    // Zwalnia miejsce **tylko wtedy, gdy wciąż jest moje**. Spóźnione zakończenie anulowanej
    // operacji trafia tu, gdy miejsce należy już do następnej — i ma wtedy nie zrobić nic.
    void yielded() {
        const std::lock_guard<std::mutex> kept{mine->guard};

        if (mine->meeting != this) return;

        mine->meeting = nullptr;
        mine->waiting = nothing;
    }

    // Instancja rury, która nie została strumieniem, schodzi przez centralę — jest do niej przypięta.
    void unused() {
        if (instance) retiring(held);
    }

    // Brama rury znowu ma wolną instancję — czymkolwiek skończyło się to przyjęcie.
    void refilled() {
        if (instance) pipes::refill(*mine->gate, held);
    }

    bool instance = false;

    // Anulowanie zwalnia miejsce **od razu**, bez oglądania się na to, co powiedziało `withdraw`.
    // Na POSIX zgłoszenie właśnie wyszło z mapy i prawda tego dowodzi. Na Windowsie `withdraw` nie
    // może powiedzieć prawdy nigdy — anulowana operacja wraca portem — ale tam centrala trzyma
    // zgłoszenia po nakładce, a nie po gnieździe, i drugie `AcceptEx` na tym samym gnieździe
    // nasłuchującym jest zwyczajną rzeczą. W obu razach miejsce jest wolne.
    //
    // Zasłona `quitted` — opisana przy nawiązaniu w strumieniu.
    void dropped() override {
        (void)withdraw(shared_from_this());

        yielded();
    }

    void done(std::size_t, int code) override {
        refilled();
        yielded();

        if (quitted.load()) {
            // **Przyjęte gniazdo nie ma komu trafić.** Anulowane przyjęcie potrafi mimo wszystko
            // dojść do skutku — zostawał wtedy po nim otwarty deskryptor, którego nikt już nigdy
            // nie zamknie. Droga błędu zamykała go od dawna, droga udana nie zamykała go wcale.
            if (instance) {
                unused();
            } else if (made != nothing) {
                ending(made);

                made = nothing;
            }

            return;
        }

        if (code != 0) {
            unused();

            spoiled(*this, std::make_exception_ptr(
                               Trouble{"Cannot accept a connection: " + spoken(code) + ".", code,
                                       mine->mine}));

            return;
        }

        settled(*this, greeting(made, where));
    }

    void late() override {
        refilled();
        yielded();
        unused();

        if (quitted.load()) return;

        spoiled(*this, std::make_exception_ptr(Overdue{}));
    }
};

}

// Brama nie oddaje obietnicy, bo nie ma na co czekać: otwarcie gniazda, przypisanie portu i
// nasłuchiwanie kończą się od razu. Obietnica, która nigdy nie czeka, jest kłamstwem o kształcie.
Listener::Listener(Address where, int waiting) : held_(std::make_shared<Held>()) {
    if (!where.held()) throw Trouble{"An empty address is no place to listen."};

    if (where.family() == Family::local && pipes::chosen()) {
        held_->gate = pipes::gated(where);
        held_->mine = std::move(where);

        return;
    }

    const Handle one = opened(where.family(), true);

    if (one == nothing)
        throw Trouble{"Cannot open a socket: " + spoken(amiss()) + ".", amiss(), where};

    held_->which = one;
    held_->numbered = one;

    // `SO_REUSEADDR` mówi o porcie w stanie TIME_WAIT, a gniazdo lokalne portu nie ma — więc dla
    // niego to wywołanie nie znaczy nic i tylko odmówiłoby po cichu.
    if (where.family() != Family::local) sharing(one);

    if (!listening(one, where, waiting)) {
        const int why = amiss();

        held_->close();
        held_.reset();

        throw Trouble{"Cannot listen: " + spoken(why) + ".", why, where};
    }

    // **Pytanie systemu ma prawo nic nie powiedzieć, a my i tak wiemy.** Przy porcie zero to
    // system nadaje numer i bez `getsockname` nie znamy go wcale — ale przy gnieździe lokalnym
    // adresem jest ścieżka, którą sami podaliśmy. Milczenie systemu nie ma prawa jej wymazać:
    // niszczyciel kasuje po sobie plik właśnie po tej ścieżce.
    held_->mine = named(one, false);

    if (!held_->mine.held()) held_->mine = where;
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

const Address& Listener::mine() const { return held_ ? held_->mine : nowhere(); }

Promise<Stream> Listener::accept(Patience patience) const {
    if (!held_) {
        auto made = std::make_shared<detail::Result<Stream>>();

        detail::spoiled(*made, std::make_exception_ptr(Trouble{"The gate is closed."}));

        return HandPromise<Stream>{made};
    }

    auto one = std::make_shared<MeetPromise>();

    // Zajęcie miejsca i zapamiętanie, czyje ono jest, pod jednym zamkiem: inaczej dwa przyjmowania
    // zaczęte naraz zapisałyby się jedno po drugim i drugie zostałoby z miejscem, którego pierwsze
    // nie oddało.
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        if (held_->meeting != nullptr) {
            auto made = std::make_shared<detail::Result<Stream>>();

            detail::spoiled(*made,
                    std::make_exception_ptr(Trouble{"The gate is already accepting.", 0, held_->mine}));

            return HandPromise<Stream>{made};
        }

        held_->meeting = one.get();

        if (held_->gate) {
            held_->waiting = pipes::spare(*held_->gate);

            if (held_->waiting == nothing) {
                held_->meeting = nullptr;

                auto made = std::make_shared<detail::Result<Stream>>();

                detail::spoiled(*made, std::make_exception_ptr(Trouble{"The gate is closed.", 0, held_->mine}));

                return HandPromise<Stream>{made};
            }

            one->instance = true;
            one->held = held_->waiting;
        }
    }

    one->mine = held_;
    one->kept = held_;

    if (!one->instance) one->held = held_->which.load();

    one->kind = Kind::meeting;
    one->ever = patience.count() > 0;
    one->until = std::chrono::steady_clock::now() + patience;

    submit(one);

    return HandPromise<Stream>{one};
}

void Listener::close() {
    if (!held_) return;

    held_->close();
    held_.reset();
}

}

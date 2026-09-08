#include "talcite/exchange.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"

#include "inside.hpp"
#include "starting.hpp"

#ifndef TALCITE_IOCP

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>

#ifdef TALCITE_EPOLL
#include <sys/epoll.h>
#else
#include <poll.h>
#endif
#endif

namespace talcite::net {

namespace {

// Budzik centrali. Na uniksach wystarczy potok, ale WSAPoll przyjmuje **wyłącznie gniazda**, więc
// na Windows budzikiem jest para gniazd na pętli zwrotnej. To jest ta sama sztuczka, którą robi
// każda biblioteka sieciowa na tym systemie, i jedyna, która tam działa.
class Waking {
public:
    Waking() {
#ifdef _WIN32
        starting();

        const Handle gate = opened(Family::four, true);

        if (gate == nothing) return;

        sharing(gate);

        if (!listening(gate, Address::loopback(Family::four, 0), 1)) {
            ending(gate);

            return;
        }

        const Address where = named(gate, false);

        told_ = opened(Family::four, true);

        (void)joined(told_, where);

        for (int tried = 0; tried < 200 && heard_ == nothing; ++tried) {
            (void)taken(gate, heard_);

            if (heard_ == nothing) std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        ending(gate);
#else
        int both[2] = {-1, -1};

        if (::pipe(both) == 0) {
            // Oba końce nieblokujące, bo opróżnianie budzika czyta aż do pustki — a czytanie
            // blokujące z pustego potoku zatrzymałoby centralę na zawsze.
            for (const int one : both) {
                ::fcntl(one, F_SETFL, ::fcntl(one, F_GETFL, 0) | O_NONBLOCK);

                // Budzik centrali nie ma trafiać do programu potomnego.
                ::fcntl(one, F_SETFD, ::fcntl(one, F_GETFD, 0) | FD_CLOEXEC);
            }

            heard_ = both[0];
            told_ = both[1];
        }
#endif
    }

    ~Waking() {
        ending(heard_);
        ending(told_);
    }

    Waking(const Waking&) = delete;
    Waking& operator=(const Waking&) = delete;

    [[nodiscard]] Handle heard() const { return heard_; }

    void ring() const {
        const char one = 'x';

#ifdef _WIN32
        (void)putting(told_, &one, 1);
#else
        const ssize_t sent = ::write(told_, &one, 1);

        (void)sent;
#endif
    }

    void drain() const {
        char much[64];

        for (;;) {
#ifdef _WIN32
            if (getting(heard_, much, sizeof much) <= 0) return;
#else
            const ssize_t got = ::read(heard_, much, sizeof much);

            if (got > 0) continue;

            // Przerwanie sygnałem nie znaczy „pusto" — inaczej zostaje bajt i budzik dzwoni
            // jałowo w kółko.
            if (got < 0 && errno == EINTR) continue;

            return;
#endif
        }
    }

private:
    Handle heard_ = nothing;
    Handle told_ = nothing;
};

// Zgłoszenie gotowe do oddania puli: co, z jakim numerem i czy gotowość niosła flagę błędu.
struct Given {
    std::shared_ptr<Doing> what;
    int code = 0;
    bool amiss = false;
};

struct Stirred {
    Handle which = nothing;
    bool reading = false;
    bool writing = false;
    bool amiss = false;
};

}

struct Exchange::Held {
    struct Watching {
        std::shared_ptr<Doing> reading;
        std::shared_ptr<Doing> writing;
    };

    mutable std::mutex guard;
    std::unordered_map<Handle, Watching> watching;

    // Gniazda oddane do zamknięcia. Zamyka je wątek centrali, a nie ten, kto oddał — deskryptor
    // zamknięty pod czekającym poll potrafi wrócić jako numer świeżo otwartego gniazda i wtedy
    // centrala pilnuje nie tego, co myśli.
    std::vector<Handle> ending;
    Waking waking;
#ifdef TALCITE_EPOLL
    int rings = -1;
#endif
    std::thread staff;
    bool closing = false;
};

namespace {

std::atomic<Exchange::Held*> standing{nullptr};

#ifdef TALCITE_EPOLL

// Uzbrajamy na **jeden raz** (EPOLLONESHOT): po zgłoszeniu gotowości jądro samo wyłącza gniazdo
// i milczy, dopóki go nie uzbroimy z powrotem. Bez tego poziomowy epoll kręciłby się w kółko —
// zgłasza gotowość tak długo, jak długo nikt danych nie odebrał, a odebrać ma je dopiero
// zgłoszenie, które właśnie wypisaliśmy.
//
// Dzięki temu zdjęcie zgłoszenia **nie kosztuje ani jednego wywołania systemowego**: gniazdo jest
// już wyłączone. Płacimy jedno wywołanie przy zgłaszaniu i nic przy kończeniu.
bool tuned(Exchange::Held& held, Handle one) {
    const auto found = held.watching.find(one);

    std::uint32_t wanted = 0;

    if (found != held.watching.end()) {
        if (found->second.reading) wanted |= EPOLLIN;
        if (found->second.writing) wanted |= EPOLLOUT;
    }

    if (wanted == 0) return true;

    epoll_event what{};

    what.events = wanted | EPOLLONESHOT;
    what.data.fd = static_cast<int>(one);

    // Zmiana najpierw, dopisanie dopiero gdy gniazda jeszcze nie było — tak jest o jedno
    // wywołanie taniej w przypadku częstszym.
    if (::epoll_ctl(held.rings, EPOLL_CTL_MOD, static_cast<int>(one), &what) == 0) return true;

    // **Nieudane uzbrojenie nie ma prawa przepaść.** Przy wyczerpanym limicie obserwacji jądro
    // nigdy nie zgłosi gotowości tego gniazda, a zgłoszenie zostawione w mapie wisiałoby do
    // terminu — albo bez końca, gdy cierpliwość jest zerowa.
    return ::epoll_ctl(held.rings, EPOLL_CTL_ADD, static_cast<int>(one), &what) == 0;
}

void untuned(Exchange::Held& held, Handle one) {
    (void)::epoll_ctl(held.rings, EPOLL_CTL_DEL, static_cast<int>(one), nullptr);
}

std::vector<Stirred> awaited(Exchange::Held& held, int patience) {
    epoll_event seen[256];

    const int many = ::epoll_wait(held.rings, seen, 256, patience);

    std::vector<Stirred> made;

    for (int at = 0; at < many; ++at) {
        const Handle which = static_cast<Handle>(seen[at].data.fd);
        const std::uint32_t what = seen[at].events;

        if (which == held.waking.heard()) {
            held.waking.drain();

            continue;
        }

        // Zerwanie i błąd budzą obie strony, bo obie i tak się już nie doczekają.
        const bool over = (what & (EPOLLERR | EPOLLHUP)) != 0;

        made.push_back(Stirred{which, (what & EPOLLIN) != 0 || over,
                               (what & EPOLLOUT) != 0 || over, (what & EPOLLERR) != 0});
    }

    return made;
}

#else

bool tuned(Exchange::Held&, Handle) { return true; }

void untuned(Exchange::Held&, Handle) {}

std::vector<Stirred> awaited(Exchange::Held& held, int patience) {
    std::vector<pollfd> watched;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        watched.push_back(
            pollfd{static_cast<decltype(pollfd::fd)>(held.waking.heard()), POLLIN, 0});

        for (const auto& [which, pair] : held.watching) {
            short wanted = 0;

            if (pair.reading) wanted |= POLLIN;
            if (pair.writing) wanted |= POLLOUT;

            if (wanted == 0) continue;

            watched.push_back(pollfd{static_cast<decltype(pollfd::fd)>(which), wanted, 0});
        }
    }

#ifdef _WIN32
    const int stirred = ::WSAPoll(watched.data(), static_cast<ULONG>(watched.size()), patience);
#else
    const int stirred = ::poll(watched.data(), static_cast<nfds_t>(watched.size()), patience);
#endif

    std::vector<Stirred> made;

    if (stirred <= 0) return made;

    if ((watched.front().revents & POLLIN) != 0) held.waking.drain();

    for (std::size_t at = 1; at < watched.size(); ++at) {
        const short what = watched[at].revents;

        if (what == 0) continue;

        const bool over = (what & (POLLERR | POLLNVAL | POLLHUP)) != 0;

        made.push_back(Stirred{static_cast<Handle>(watched[at].fd), (what & POLLIN) != 0 || over,
                               (what & POLLOUT) != 0 || over,
                               (what & (POLLERR | POLLNVAL)) != 0});
    }

    return made;
}

#endif

// Odmowa dla zgłoszenia na gnieździe, którego już nie ma. Idzie przez pulę, a nie wprost, bo
// done() budzi czekającego i wywołuje jego ciąg dalszy — a tego nie robimy na wątku wołającego.
void aborted(std::shared_ptr<Doing> one) {
    pooling().take([one = std::move(one)] { one->done(0, ECONNABORTED); });
}

// Krok i zakończenie idą **na wątek z puli**, nie na wątek centrali. Ciąg dalszy obietnicy to
// cudzy kod — gdyby pobiegł na jedynym wątku, który pilnuje wszystkich gniazd, jeden powolny then
// zatrzymałby całą sieć. Wątek centrali nie robi więc nic poza czekaniem.
void handed(std::shared_ptr<Doing> one, int what, bool amiss = false) {
    pooling().take([one = std::move(one), what, amiss]() mutable {
        if (what > 0) {
            one->done(0, what);

            return;
        }

        if (what < 0) {
            one->late();

            return;
        }

        // System powiedział „gotowe", a mimo to nie dał rady — zdarza się, gdy zdarzenie było
        // fałszywe albo ktoś inny zdążył pierwszy. Wtedy po prostu czekamy dalej.
        //
        // Chyba że gotowość przyszła z flagą błędu: wtedy czekanie dalej byłoby kręceniem się
        // w kółko, bo popsute gniazdo zgłasza się natychmiast i bez końca.
        if (!stepped(*one)) {
            if (amiss) one->done(0, EPIPE);
            else submit(std::move(one));
        }
    });
}

}

Exchange::Exchange() : held_(std::make_unique<Held>()) {
    // Pula **musi** powstać przed centralą, choć nic jej tu nie potrzebuje. Obiekty statyczne
    // giną w kolejności odwrotnej do powstawania, a wątek centrali oddaje pracę do puli — więc
    // gdyby pula powstała później, zginęłaby wcześniej i wątek sięgałby do zwolnionej pamięci.
    // Program, który zaczyna od sieci, trafiał na to za każdym razem; testy nie, bo one dotykają
    // puli wcześniej. Znalazł to dopiero pomiar wydajności.
    (void)pooling();

    // Wskazanie ustawiamy w konstruktorze, a nie przy każdym running(): jednorazowe utworzenie
    // obiektu statycznego jest bezpieczne wątkowo, a powtarzany zapis tej samej wartości z kilku
    // wątków byłby wyścigiem — nawet gdy wartość się nie zmienia.
    standing = held_.get();

#ifdef TALCITE_EPOLL
    held_->rings = ::epoll_create1(EPOLL_CLOEXEC);

    epoll_event what{};

    what.events = EPOLLIN;
    what.data.fd = static_cast<int>(held_->waking.heard());

    (void)::epoll_ctl(held_->rings, EPOLL_CTL_ADD, static_cast<int>(held_->waking.heard()), &what);
#endif

    held_->staff = std::thread{[this] {
        Held& held = *held_;

        for (;;) {
            std::chrono::steady_clock::time_point soonest{};
            bool timed = false;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                for (const Handle one : held.ending) net::ending(one);

                held.ending.clear();

                for (const auto& [which, pair] : held.watching)
                    for (const std::shared_ptr<Doing>& one : {pair.reading, pair.writing})
                        if (one && one->ever && (!timed || one->until < soonest)) {
                            soonest = one->until;
                            timed = true;
                        }
            }

            int patience = -1;

            if (timed) {
                // **W górę, nie w dół.** Ucięcie do zera przy terminie bliższym niż milisekunda
                // sprawiało, że poll wracał natychmiast, przegląd nie znajdował nic przeterminowanego
                // i pętla kręciła się w kółko — przy zapleczu poll odbudowując za każdym razem całą
                // tablicę deskryptorów.
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                    soonest - std::chrono::steady_clock::now());

                // I **z ograniczeniem do zakresu**: cierpliwość podaje wołający, a powyżej mniej
                // więcej dwudziestu pięciu dni liczba przekręcała się na ujemną, którą poll czyta
                // jako „czekaj bez końca" — czyli taki termin nie zadziałałby nigdy.
                const std::int64_t much =
                    std::min<std::int64_t>(std::max<std::int64_t>(left.count(), 0),
                                           std::numeric_limits<int>::max());

                patience = static_cast<int>(much);
            }

            const std::vector<Stirred> stirred = awaited(held, patience);

            std::vector<Given> giving;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                const auto now = std::chrono::steady_clock::now();

                std::vector<Handle> touched;

                for (const Stirred& one : stirred) {
                    const auto found = held.watching.find(one.which);

                    if (found == held.watching.end()) continue;

                    Held::Watching& pair = found->second;

                    // Flaga błędu **nie skraca drogi**. Wcześniej zgłoszenie kończyło się wtedy
                    // wymyślonym EPIPE, a krok nie biegł w ogóle — więc bajty odebrane przed
                    // zerwaniem przepadały, choć jądro wciąż pozwalało je przeczytać, a każdy powód
                    // nieudanego nawiązania zwijał się do tego samego numeru. Prawdziwy powód
                    // wyciąga dopiero krok: recv przy czytaniu, SO_ERROR przy łączeniu.
                    if (pair.reading && one.reading) {
                        giving.push_back(Given{std::move(pair.reading), 0, one.amiss});
                        pair.reading.reset();
                    }

                    if (pair.writing && one.writing) {
                        giving.push_back(Given{std::move(pair.writing), 0, one.amiss});
                        pair.writing.reset();
                    }

                    touched.push_back(one.which);
                }

                // Przeterminowane zbieramy osobno, bo termin mija także wtedy, gdy na gnieździe
                // nic się nie dzieje — a wtedy nikt o nim nie powie.
                for (auto& [which, pair] : held.watching)
                    for (std::shared_ptr<Doing>* one : {&pair.reading, &pair.writing})
                        if (*one && (*one)->ever && (*one)->until <= now) {
                            giving.push_back(Given{std::move(*one), -1, false});
                            one->reset();

                            touched.push_back(which);
                        }

                // Poprawiamy **wyłącznie te gniazda, które ruszyliśmy**. Pierwsza wersja
                // przechodziła po całej mapie co obrót — czyli przy tysiącu połączeń tysiąc
                // wywołań systemowych na każde jedno zdarzenie. Pomiar to pokazał od razu.
                for (const Handle which : touched) {
                    const auto found = held.watching.find(which);

                    if (found == held.watching.end()) continue;

                    if (found->second.reading || found->second.writing) {
                        if (tuned(held, which)) continue;

                        for (std::shared_ptr<Doing>* one :
                             {&found->second.reading, &found->second.writing})
                            if (*one) giving.push_back(Given{std::move(*one), ENOSPC, false});

                        held.watching.erase(found);

                        continue;
                    }

                    held.watching.erase(found);
                }
            }

            for (Given& one : giving) handed(std::move(one.what), one.code, one.amiss);
        }
    }};
}

// Rozbiórka ma kolejność i każdy jej krok ma powód.
Exchange::~Exchange() {
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->closing = true;
    }

    held_->waking.ring();

    // 1. Wątek centrali staje — od tej chwili nikt już nie odda pracy do puli.
    if (held_->staff.joinable()) held_->staff.join();

    // 2. Centrala przestaje istnieć dla świata — **przed** opróżnieniem puli. Zadanie kończone
    //    w puli potrafi zgłosić się z powrotem, a zgłoszenie przyjęte po tym, jak wątek centrali
    //    już stanął, nie miałoby kogo obsłużyć i wisiałoby do końca programu.
    standing.store(nullptr);

    // 3. Pula kończy to, co dostała — inaczej zakończenie w locie sięgnęłoby tu za chwilę.
    pooling().drain();

    // 4. Dopiero teraz zgłoszenia, które jeszcze wiszą. Każde z nich trzyma gniazdo i w
    //    destruktorze woła forget() — gdyby ginęły razem z mapą, sięgałyby do centrali w trakcie
    //    jej własnego niszczenia. To był prawdziwy błąd, znaleziony pomiarem, a nie testami.
    //
    //    **Odrzucamy je, zamiast po cichu porzucić.** Zgłoszenie jest zarazem wynikiem obietnicy
    //    wołającego: porzucone zostawało w stanie świeżym, a wątek stojący na `await` nie budził
    //    się nigdy i program nie kończył się wcale.
    for (auto& [which, pair] : held_->watching)
        for (const std::shared_ptr<Doing>& one : {pair.reading, pair.writing})
            if (one) one->done(0, ECONNABORTED);

    held_->watching.clear();

    for (const Handle one : held_->ending) net::ending(one);

    held_->ending.clear();

#ifdef TALCITE_EPOLL
    if (held_->rings >= 0) ::close(held_->rings);
#endif
}

std::size_t Exchange::waiting() const {
    const std::lock_guard<std::mutex> kept{held_->guard};

    return held_->watching.size();
}

// Centrala jest jedna na program i budzi się przy pierwszym gnieździe. Wątku, który pilnuje
// wszystkich połączeń, nie stawia się na zapas.
Exchange& Exchange::running() {
    static Exchange one;

    return one;
}

namespace {

// Rozpoczęcie operacji. Przy zapleczach gotowościowych ruszać trzeba tylko nawiązanie — reszta
// czeka na sygnał od systemu i dopiero wtedy woła stepped(). Fałsz znaczy „już po wszystkim".
bool began(Doing& one) {
    if (one.kind != Kind::joining) return true;

    // Na pętli zwrotnej nawiązanie kończy się od razu i nie ma na co czekać.
    if (joined(one.held, one.where)) {
        one.done(0, 0);

        return false;
    }

    if (!underway()) {
        one.done(0, amiss());

        return false;
    }

    return true;
}

}

void submit(std::shared_ptr<Doing> one) {
    (void)Exchange::running();

    Exchange::Held* held = standing.load();

    if (held == nullptr) {
        one->done(0, ECONNABORTED);

        return;
    }

    const Handle which = one->held;

    // Zgłoszenie na gnieździe, które wołający zdążył zamknąć. Uchwyt jest wtedy zużyty, a bez tej
    // odmowy zgłoszenie **wisiałoby na zawsze**: centrala trzymałaby je pod numerem, którego nikt
    // nigdy nie ruszy, a wątek czekający na obietnicę stałby do końca programu.
    if (which == nothing) {
        aborted(std::move(one));

        return;
    }

    if (!began(*one)) return;

    const bool writing = outward(one->kind);

    std::shared_ptr<Doing> given;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        Exchange::Held::Watching& pair = held->watching[which];

        if (writing) pair.writing = std::move(one);
        else pair.reading = std::move(one);

        if (!tuned(*held, which)) {
            std::shared_ptr<Doing>& slot = writing ? pair.writing : pair.reading;

            given = std::move(slot);
            slot.reset();

            if (!pair.reading && !pair.writing) held->watching.erase(which);
        }
    }

    if (given) {
        aborted(std::move(given));

        return;
    }

    held->waking.ring();
}

// Zdjęcie jednego zgłoszenia — to, czego potrzebuje anulowana obietnica. Bez tego przerwane
// czytanie zostawało w centrali z zajętym kierunkiem: wołający przestawał czekać, ale gniazdo do
// końca programu odpowiadało „zajęty", a `kept` trzymało otwarty deskryptor.
//
// Całość stoi na **sprawdzeniu tożsamości pod zamkiem**. Wątek centrali wyjmuje zgłoszenie z mapy
// pod tym samym zamkiem, zanim odda je puli, więc albo zastaniemy je w gnieździe, albo on je już
// zabrał i zaraz pobiegnie done() — trzeciej możliwości nie ma i nie ma czego ścigać.
bool withdraw(const std::shared_ptr<Doing>& one) {
    Exchange::Held* held = standing.load();

    if (held == nullptr || !one || one->held == nothing) return false;

    const Handle which = one->held;
    const bool writing = outward(one->kind);

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        const auto found = held->watching.find(which);

        if (found == held->watching.end()) return false;

        std::shared_ptr<Doing>& slot = writing ? found->second.writing : found->second.reading;

        if (slot != one) return false;

        slot.reset();

        // Nieudanego przezbrojenia **nie zamieniamy tu na odmowę**. Drugi kierunek jest cudzym
        // zgłoszeniem, a anulowanie jednego czytania nie jest miejscem, w którym wolno zerwać
        // trwający zapis — zostanie w mapie i skończy się terminem albo tak jak zawsze.
        if (found->second.reading || found->second.writing) {
            (void)tuned(*held, which);
        } else {
            untuned(*held, which);

            held->watching.erase(found);
        }
    }

    held->waking.ring();

    return true;
}

// Zamknięcie deskryptora, oddane centrali. **Woła się je dopiero wtedy, gdy ginie ostatni uchwyt
// do gniazda** — nie wtedy, gdy ktoś je zamyka. Powód jest taki, że numer zamkniętego gniazda
// system natychmiast oddaje następnemu: gdyby ktoś jeszcze mógł ten numer zgłosić albo na nim
// czytać, sięgnąłby do cudzego połączenia. Zgłoszenia w locie trzymają uchwyt przez `kept`, więc
// dopóki którekolwiek żyje, tu nie trafiamy.
void retiring(Handle one) {
    if (one == nothing) return;

    Exchange::Held* held = standing.load();

    // Przed pierwszym gniazdem i po rozbiórce centrali nie ma komu oddać numeru — a zamknąć go
    // i tak trzeba.
    if (held == nullptr) {
        ending(one);

        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->ending.push_back(one);
    }

    held->waking.ring();
}

// Zapomnienie zdejmuje gniazdo z pilnowania i odrzuca wszystko, co na nim wisiało. **Deskryptora
// nie zamyka** — od tego jest retiring.
void forget(Handle one) {
    Exchange::Held* held = standing.load();

    // Zanim centrala wstała i po tym, jak zeszła, nie ma z czego zdejmować.
    if (held == nullptr) return;

    std::shared_ptr<Doing> reading;
    std::shared_ptr<Doing> writing;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (const auto found = held->watching.find(one); found != held->watching.end()) {
            reading = std::move(found->second.reading);
            writing = std::move(found->second.writing);

            held->watching.erase(found);
        }

        untuned(*held, one);
    }

    // Zerwanie rozmowy jest bezpieczne od razu — **nie oddaje numeru systemowi**, więc nikt nie
    // przejmie deskryptora spod biegnącego kroku. Zamknięcie przychodzi osobno, przez retiring,
    // i dopiero wtedy, gdy zniknie ostatni uchwyt do gniazda.
    shutting(one);

    held->waking.ring();

    if (reading) handed(std::move(reading), ECONNABORTED);
    if (writing) handed(std::move(writing), ECONNABORTED);
}

}

#endif

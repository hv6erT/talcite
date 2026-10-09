#include "talcite/peer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "stunning.hpp"

namespace talcite::net {

// To, co widzi pętla odbioru. **Nie to samo, co trzyma wołający** — patrz Held niżej.
struct Living {
    Udp post;

    // Przekaźnik jest **cudzą własnością**: rozmówca go używa, ale nigdy nie zamyka. Ten sam
    // egzemplarz dotykają trzy wątki — pętla poczty przy odpowiadaniu, pętla przekaźnika przy
    // odbiorze i wołający przy wysyłce — i wolno im, bo żaden nie woła na nim `close`. To `close`
    // zeruje uchwyt, i to on był wyścigiem w poczcie, a nie samo czytanie.
    Relay relay;

    mutable std::mutex guard;
    std::condition_variable stirring;

    std::vector<Candidate> mine;
    std::vector<Candidate> theirs;
    Address met;

    // Którym kanałem poszła zwycięska droga. **Kanał jest częścią tożsamości rozmowy**: ten sam
    // adres widziany przez przekaźnik i wprost to dwie różne drogi, a dane bierzemy tylko tą,
    // która wygrała.
    bool relayed = false;

    // Adresy, którym już nadaliśmy pozwolenie na własnym przekaźniku — bez portów, bo pozwolenie
    // sięga adresu. Bez tej pamięci każdy obrót przebijania nadawałby je od nowa.
    std::vector<Address> allowed;

    Number number{};
    std::deque<Bytes> spare;

    // Poczta przyjmuje **jedną wysyłkę naraz** i drugą odrzuca. A wysyłają tu trzy rzeczy: próby
    // z reach, odpowiedzi i podtrzymanie z pętli odbioru oraz dane od wołającego. Bez tego zamka
    // deptałyby sobie po piętach i gubiły datagramy.
    std::mutex speaking;

    std::atomic<bool> reaching{false};
    std::atomic<bool> living{false};
    std::atomic<bool> fetching{false};
    std::atomic<bool> shutting{false};
};

// Wołający trzyma **osobny** obiekt, którego pętla nie trzyma nigdy. Dzięki temu wypuszczenie
// ostatniego rozmówcy z rąk zamyka gniazdo **natychmiast**, a nie dopiero wtedy, gdy pętla sama
// zajrzy na słaby uchwyt.
//
// To nie jest ozdoba. Zadanie z puli, które przeżyje swojego właściciela i zawiśnie na centrali,
// **zakleszcza cały program przy wyjściu**: rozbiórka centrali czeka na opróżnienie puli, a
// zadanie w puli czeka na centralę, której już nie ma. Złapało to pierwsze uruchomienie testów.
struct Peer::Held {
    std::shared_ptr<Living> alive;

    // **Własna kopia poczty, wyłącznie do zamykania.** Kopie dzielą gniazdo, ale nie dzielą
    // obiektu — a `Udp`, jak każdy typ wartościowy w talcite, nie znosi dotykania tego samego
    // egzemplarza z dwóch wątków naraz. Zamykanie tej, z której czyta pętla, było wyścigiem
    // na jej własnym uchwycie.
    Udp closing;

    ~Held() {
        {
            // Znacznik stawiamy **pod tym samym zamkiem**, który trzyma czekający przy sprawdzaniu
            // warunku. Postawiony obok, mógł trafić w szparę między sprawdzeniem a zaśnięciem —
            // a wtedy powiadomienie przepadało i czekający przesypiał całą cierpliwość.
            const std::lock_guard<std::mutex> kept{alive->guard};

            alive->shutting.store(true);
        }

        closing.close();
        alive->stirring.notify_all();
    }
};

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t roomy = 65535;

constexpr Patience beating{15000};

constexpr Patience pausing{250};

constexpr Patience listening{1000};

// Kolejka odebranych przesyłek, których nikt jeszcze nie odebrał. Bez granicy rozmówca nadający
// szybciej, niż wołający czyta, doprowadzał proces do wyczerpania pamięci. Po przepełnieniu ginie
// **najstarsza** — przy datagramach starsza treść jest mniej warta niż świeża.
constexpr std::size_t plenty = 256;

Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// Ile czekamy na jedno pozwolenie od własnego przekaźnika.
constexpr Patience granting{1000};

// Wysyłka wybranym kanałem. Próby i podtrzymanie **wolno zgubić** — to jest sieć, a od powtarzania
// jest pętla wyżej. Dlatego kłopot tu nie wychodzi na zewnątrz.
//
// Przez przekaźnik idzie się bez naszego zamka: on pilnuje swojej poczty sam, a tamten zamek broni
// **naszego** gniazda przed drugą wysyłką w tej samej chwili.
void posting(const std::shared_ptr<Living>& held, const Address& to, Bytes what, Patience patience,
             bool through = false) {
    try {
        if (held->shutting.load()) return;

        if (through) {
            held->relay.send(to, std::move(what), patience).await();

            return;
        }

        const std::lock_guard<std::mutex> kept{held->speaking};

        if (held->shutting.load()) return;

        held->post.send(to, std::move(what), patience).await();
    } catch (const std::exception&) {
    }
}

// Ile obcych adresów wolno dopisać z samych prób. NAT zwykle nadaje drugiej stronie jeden inny
// port, czasem kilka — kilkanaście z zapasem starcza, a bez granicy sama lista była miejscem, do
// którego dowolny nadawca mógł wlać ile chciał.
constexpr std::size_t roomier = 16;

bool knowing(const Living& held, const Address& where) {
    for (const Candidate& one : held.theirs)
        if (one.where == where) return true;

    return false;
}

// Droga, o której dowiadujemy się dopiero z cudzej próby. Zdarza się zawsze, gdy NAT nadał drugiej
// stronie port inny niż ten, który zdążyła zgłosić — a to jest reguła, nie wyjątek. **Dopisujemy
// tylko adres, którego gospodarza już znamy z drogi podanej przez sygnalizację**: przed naprawą
// wystarczyło jedno nieproszone zapytanie, żeby obcy trafił na listę i dostał od nas odzew.
void noted(const std::shared_ptr<Living>& held, const Address& where) {
    const std::lock_guard<std::mutex> kept{held->guard};

    if (knowing(*held, where)) return;

    if (held->theirs.size() >= roomier) return;

    for (const Candidate& one : held->theirs)
        if (one.where.family() == where.family() && one.where.bytes() == where.bytes()) {
            held->theirs.push_back(Candidate{where, Candidate::Sort::seen});

            return;
        }
}

// Co zrobić z jednym odebranym datagramem. `through` mówi, którym kanałem przyszedł: naszym
// gniazdem czy naszym przekaźnikiem — i tym samym kanałem idzie odpowiedź, bo tylko on jest
// dowiedziony jako przechodni.
void taking(const std::shared_ptr<Living>& held, Datagram came, bool through) {
    Message made;

    if (understood(came.bytes, made)) {
        // Cudza próba jest dowodem, że dziura z tamtej strony stoi. Odpowiadamy od razu
        // i odpytujemy zwrotnie, bo przejście musi się udać w **obie** strony.
        if (made.sort == Sort::asking) {
            posting(held, came.from, told(made.number, came.from), listening, through);

            noted(held, came.from);

            Number number;

            bool met = false;

            bool known = false;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                number = held->number;
                met = held->met.held();
                known = knowing(*held, came.from);
            }

            // Odpytujemy zwrotnie tylko dopóki nie mamy zwycięskiej drogi — później każda cudza
            // próba mnożyłaby ruch bez powodu — i **wyłącznie drogę, którą znamy**. Nasz numer
            // transakcji jest jedynym dowodem tożsamości odpowiedzi; odesłany komukolwiek, kto
            // się odezwał, pozwalał obcemu odpowiedzieć nim i wygrać przebijanie.
            //
            // To jest zarazem jedyne miejsce, w którym wychodzimy własnym przekaźnikiem: nie
            // dobijamy się nim z siebie, tylko odpowiadamy temu, kto sam pod niego przyszedł.
            if (!met && known) posting(held, came.from, asked(number), listening, through);

            return;
        }

        if (made.sort == Sort::told) {
            bool won = false;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                // Sam numer transakcji nie wystarczy: musi jeszcze przyjść z drogi, którą
                // sami zaproponowaliśmy albo poznaliśmy od gospodarza, którego znamy.
                if (made.number == held->number && !held->met.held() &&
                    knowing(*held, came.from)) {
                    held->met = came.from;
                    held->relayed = through;

                    won = true;
                }
            }

            if (won) held->stirring.notify_all();
        }

        // Wiadomość STUN **nigdy** nie trafia do danych wołającego. To jest cała treść
        // rozdzielania: jedno gniazdo niesie i sprawdzanie łączności, i rozmowę.
        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        // **Dane bierzemy wyłącznie od tego, z kim się przebiliśmy, i wyłącznie tym kanałem.**
        // Wcześniej pętla wrzucała do kolejki każdy datagram, a wołający dostawał gołe bajty bez
        // nadawcy — więc każdy, kto poznał adres i port, wstrzykiwał treść w rozmowę.
        if (!held->met.held() || !(came.from == held->met) || held->relayed != through) return;

        if (held->spare.size() >= plenty) held->spare.pop_front();

        held->spare.push_back(std::move(came.bytes));
    }

    held->stirring.notify_all();
}

// Jeden obrót pętli odbioru. Oddaje fałsz, gdy gniazdo się zamknęło i nie ma po co kręcić dalej.
bool turning(const std::shared_ptr<Living>& held, Clock::time_point& beat) {
    Datagram came;

    try {
        if (held->shutting.load()) return false;

        came = held->post.receive(roomy, listening).await();
    } catch (const Overdue&) {
        Address met;

        bool through = false;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            met = held->met;
            through = held->relayed;
        }

        // Odwzorowanie na NAT-cie żyje zwykle trzydzieści sekund od ostatniego ruchu.
        // Podtrzymanie nie jest ozdobą, tylko warunkiem, żeby cisza nie zerwała połączenia.
        // Idzie **kanałem rozmowy**: przy drodze przez przekaźnik podtrzymywać trzeba tamtą
        // stronę, a nie odwzorowanie, którego rozmowa wcale nie używa.
        if (met.held() && Clock::now() - beat >= beating) {
            Number number;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                number = held->number;
            }

            posting(held, met, asked(number), listening, through);

            beat = Clock::now();
        }

        return true;
    } catch (const std::exception&) {
        return false;
    }

    taking(held, std::move(came), false);

    return true;
}

// Jeden obrót pętli od przekaźnika. Osobna, bo obie czekają blokująco, a jeden wątek nie umie
// czekać na dwa gniazda naraz.
bool drawing(const std::shared_ptr<Living>& held) {
    Datagram came;

    try {
        if (held->shutting.load()) return false;

        came = held->relay.receive(roomy, listening).await();
    } catch (const Overdue&) {
        return true;
    } catch (const std::exception&) {
        return false;
    }

    taking(held, std::move(came), true);

    return true;
}

// Pętla odbioru. **Trzyma wątek z puli przez całe życie połączenia** — świadomie, bo pula dla pracy
// zwykłej rośnie bez ograniczeń, a rozmówca jest jeden. Przy tysiącu rozmówców trzeba by to
// przepisać na łańcuch obietnic.
//
// Trzyma **słaby** uchwyt i bierze mocny tylko na jeden obrót. Dzięki temu porzucony rozmówca —
// taki, którego nikt nie zamknął, tylko wypuścił z rąk — kończy pętlę sam, najdalej po sekundzie,
// zamiast kręcić się bez końca i trzymać przy życiu zarówno wątek, jak i gniazdo.
void minding(std::weak_ptr<Living> weak) {
    Clock::time_point beat = Clock::now();

    for (;;) {
        const std::shared_ptr<Living> held = weak.lock();

        if (!held || held->shutting.load()) break;

        if (!turning(held, beat)) break;
    }

    if (const std::shared_ptr<Living> held = weak.lock()) {
        held->living.store(false);

        held->stirring.notify_all();
    }
}

// Pętla od przekaźnika — ten sam słaby uchwyt i ta sama zasada co wyżej. Rozmówca **nie zamyka
// przekaźnika**, bo go nie posiada; ta pętla kończy się najdalej sekundę po zamknięciu rozmówcy,
// a przekaźnik gaśnie wtedy, gdy puści go ostatni uchwyt — także ten trzymany tutaj.
void watching(std::weak_ptr<Living> weak) {
    for (;;) {
        const std::shared_ptr<Living> held = weak.lock();

        if (!held || held->shutting.load()) break;

        if (!drawing(held)) break;
    }

    if (const std::shared_ptr<Living> held = weak.lock()) {
        held->fetching.store(false);

        held->stirring.notify_all();
    }
}

void waking(const std::shared_ptr<Living>& held) {
    bool was = false;

    if (held->living.compare_exchange_strong(was, true)) {
        Promise<void> made{[weak = std::weak_ptr<Living>{held}](auto resolve, auto) {
            minding(weak);

            resolve();
        }};

        made.start();
    }

    if (!held->relay.held()) return;

    was = false;

    if (held->fetching.compare_exchange_strong(was, true)) {
        Promise<void> made{[weak = std::weak_ptr<Living>{held}](auto resolve, auto) {
            watching(weak);

            resolve();
        }};

        made.start();
    }
}

// Pozwolenia na własnym przekaźniku. Bez nich jego serwer wyrzuci każdy datagram od drugiej strony
// i nasz użyczony adres byłby ogłoszony na darmo. Pozwolenie sięga **adresu, nie portu**, więc
// jedno starczy na wszystkie porty, które NAT tamtej stronie nada — i dlatego pamiętamy je
// z wyzerowanym portem.
//
// Odmowa nie przerywa przebijania: drogi bezpośrednie działają bez przekaźnika i to one mają
// pierwszeństwo.
void letting(const std::shared_ptr<Living>& held) {
    if (!held->relay.held()) return;

    // Pozwolenie bez przydziału jest bez sensu: nie ma czego pilnować i prawdziwy serwer odpowie
    // odmową. Przekaźnik, którego nie udało się otworzyć przy zbieraniu, po prostu nie istnieje.
    if (!held->relay.lent().held()) return;

    std::vector<Address> whom;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        for (const Candidate& one : held->theirs) {
            Address bare = one.where;

            bare.port(0);

            if (std::find(held->allowed.begin(), held->allowed.end(), bare) !=
                held->allowed.end())
                continue;

            if (std::find(whom.begin(), whom.end(), bare) != whom.end()) continue;

            whom.push_back(bare);
        }
    }

    for (const Address& one : whom) {
        if (held->shutting.load()) return;

        try {
            held->relay.allow(one, granting).await();
        } catch (const std::exception&) {
        }

        // Zapisujemy także po odmowie. Powtarzanie nieudanego pozwolenia w każdym obrocie
        // przebijania zjadałoby jego cierpliwość na drodze, która i tak nie działa.
        const std::lock_guard<std::mutex> kept{held->guard};

        held->allowed.push_back(one);
    }
}

template <typename T>
Promise<T> amiss_now(const std::string& said) {
    return Promise<T>{
        [said](auto, auto reject) { reject(std::make_exception_ptr(Trouble{said})); }};
}

}

Peer::Peer(Udp post) : held_(std::make_shared<Held>()) {
    held_->alive = std::make_shared<Living>();
    held_->alive->post = post;
    held_->alive->number = numbered();
    held_->closing = std::move(post);
}

// Przekaźnik podaje się **przy powstaniu albo wcale**. Wpięty później zostawiałby chwilę, w której
// zebrane już drogi milczą o użyczonym adresie, a druga strona nie ma pod co się dobijać.
Peer::Peer(Udp post, Relay relay) : Peer(std::move(post)) {
    held_->alive->relay = std::move(relay);
}

std::vector<Candidate> Peer::candidates() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->mine;
}

std::vector<Candidate> Peer::theirs() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->theirs;
}

Address Peer::met() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->met;
}

bool Peer::relayed() const {
    if (!held_) return false;

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->relayed;
}

// Drogi próbujemy w kolejności, w jakiej leżą, a leżą wedle rodzaju: własna karta przed odbiciem
// od serwera, odbicie przed przekaźnikiem. Dwie maszyny w jednej sieci nie mają po co wychodzić
// na świat, a przekaźnik kosztuje cudze pasmo i jest ostatecznością.
void Peer::add(Candidate one) {
    if (!held_) return;

    if (!one.where.held()) return;

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    if (one.where == held_->alive->post.mine()) return;

    std::vector<Candidate>& theirs = held_->alive->theirs;

    if (std::find(theirs.begin(), theirs.end(), one) != theirs.end()) return;

    const auto at =
        std::upper_bound(theirs.begin(), theirs.end(), one,
                         [](const Candidate& left, const Candidate& right) {
                             return left.sort < right.sort;
                         });

    theirs.insert(at, one);
}

// Zbieranie kandydatów. Adresy własnych kart dostają port **naszego gniazda**, bo to jego szuka
// druga strona. Pętla zwrotna wypada: wysłana drugiej stronie kazałaby jej dobijać się do samej
// siebie, a odpowiedź od własnego gniazda wyglądałaby jak udane przebicie.
Promise<std::vector<Candidate>> Peer::gather(StunClient scout, Patience patience) {
    if (!held_) return amiss_now<std::vector<Candidate>>("The peer is empty.");

    if (held_->alive->living.load())
        return amiss_now<std::vector<Candidate>>("Gathering must finish before punching starts.");

    auto held = held_->alive;

    Promise<std::vector<Candidate>> made{[held, scout, patience](auto resolve, auto reject) {
        try {
            const Clock::time_point until = Clock::now() + patience;

            const std::uint16_t port = held->post.mine().port();

            std::vector<Candidate> found;

            for (Address one : mine(held->post.mine().family()).await()) {
                if (one == Address::loopback(one.family(), 0)) continue;

                one.port(port);

                found.push_back(Candidate{one, Candidate::Sort::here});
            }

            // Odbicia od serwera może nie być — bez zwiadowcy, bez odpowiedzi albo po terminie. To nie
            // jest powód, żeby wywrócić zbieranie: w jednej sieci lokalnej same karty wystarczą.
            // **Termin jest nasz**, nie zwiadowcy: ten ma własną cierpliwość z konstruktora,
            // zwykle o rząd wielkości dłuższą, i bez tego pilnowania zjadał ją w całości.
            if (scout.held() && Clock::now() < until) {
                try {
                    if (const Address seen = scout.seen().timeout(left(until)).await(); seen.held())
                        found.push_back(Candidate{seen, Candidate::Sort::seen});
                } catch (const std::exception&) {
                }
            }

            // Przekaźnik jest **ostatecznością**, więc pytamy o niego na końcu, a jego adres trafia
            // do listy jako droga `lent` — czyli za wszystkimi innymi. Otwieramy go tylko wtedy,
            // gdy nie jest jeszcze otwarty: wołający ma prawo zrobić to sam i podać go gotowego.
            // Nieudany przekaźnik nie wywraca zbierania, tak samo jak nieudany zwiad.
            if (held->relay.held() && Clock::now() < until) {
                try {
                    Address lent = held->relay.lent();

                    if (!lent.held()) lent = held->relay.open(left(until)).await();

                    if (lent.held()) found.push_back(Candidate{lent, Candidate::Sort::lent});
                } catch (const std::exception&) {
                }
            }

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                held->mine = found;
            }

            resolve(found);
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Przebijanie. Obie strony robią to samo i **muszą robić to naraz**: mój datagram ginie na cudzym
// NAT-cie, ale otwiera dziurę w moim, a cudzy ginie na moim, otwierając dziurę w jego. Dopiero
// drugi datagram każdej ze stron trafia na otwarte przejście.
Promise<Address> Peer::reach(Patience patience) {
    if (!held_) return amiss_now<Address>("The peer is empty.");

    bool was = false;

    if (!held_->alive->reaching.compare_exchange_strong(was, true))
        return amiss_now<Address>("The peer is already punching.");

    auto held = held_->alive;

    Promise<Address> made{[held, patience](auto resolve, auto reject) {
        try {
            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->theirs.empty())
                    throw Trouble{"No known way to the other side."};
            }

            waking(held);

            const Clock::time_point until = Clock::now() + patience;

            while (Clock::now() < until && !held->shutting.load()) {
                // Pozwolenia nadajemy w każdym obrocie, bo drogi mogą dojść z sygnalizacji już
                // w trakcie przebijania. Adres raz załatwiony nie kosztuje tu nic.
                letting(held);

                std::vector<Candidate> ways;

                Number number;

                {
                    const std::lock_guard<std::mutex> kept{held->guard};

                    ways = held->theirs;
                    number = held->number;
                }

                for (const Candidate& one : ways) {
                    if (held->shutting.load()) break;

                    // Termin sprawdzamy **przy każdej drodze**, nie raz na obrót. Przy kilkunastu
                    // drogach jeden obrót trwał kilkanaście ćwierci sekundy, więc reach przekraczał
                    // własną cierpliwość o rząd wielkości.
                    if (Clock::now() >= until) break;

                    posting(held, one.where, asked(number), pausing);
                }

                std::unique_lock<std::mutex> kept{held->guard};

                if (held->stirring.wait_for(kept, pausing,
                                            [&held] { return held->met.held(); })) {
                    const Address won = held->met;

                    kept.unlock();

                    held->reaching.store(false);

                    resolve(won);

                    return;
                }
            }

            throw Overdue{};
        } catch (...) {
            held->reaching.store(false);

            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

Promise<void> Peer::send(Bytes what, Patience patience) {
    if (!held_) return amiss_now<void>("The peer is empty.");

    Address met;

    bool through = false;

    {
        const std::lock_guard<std::mutex> kept{held_->alive->guard};

        met = held_->alive->met;
        through = held_->alive->relayed;
    }

    if (!met.held()) return amiss_now<void>("No one to talk to yet.");

    auto held = held_->alive;

    Promise<void> made{[held, met, through, what = std::move(what), patience](auto resolve,
                                                                             auto reject) mutable {
        try {
            if (held->shutting.load()) throw Trouble{"The peer is closed."};

            // Mówimy **tym kanałem, którym się przebiliśmy**. Wysyłka wprost pod adres, który
            // odezwał się przez przekaźnik, trafiłaby w NAT drugiej strony i przepadła.
            if (through) {
                held->relay.send(met, std::move(what), patience).await();

                resolve();

                return;
            }

            const std::lock_guard<std::mutex> kept{held->speaking};

            if (held->shutting.load()) throw Trouble{"The peer is closed."};

            held->post.send(met, std::move(what), patience).await();

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

Promise<Bytes> Peer::receive(std::size_t most, Patience patience) {
    if (!held_) return amiss_now<Bytes>("The peer is empty.");

    auto held = held_->alive;

    Promise<Bytes> made{[held, most, patience](auto resolve, auto reject) {
        try {
            waking(held);

            std::unique_lock<std::mutex> kept{held->guard};

            // Śmierć pętli też budzi. Bez tego warunku powiadomienie o zamkniętym gnieździe nie
            // miało jak stać się prawdą, więc wołający przesypiał całą cierpliwość i dostawał
            // termin zamiast prawdziwego powodu — trzymając przy tym wątek z puli.
            // Budzi też śmierć **każdej** z pętli — a przy przekaźniku są dwie. Dopóki choć jedna
            // żyje, cisza jest tylko ciszą i wołający ma czekać dalej.
            if (!held->stirring.wait_for(kept, patience, [&held] {
                    return !held->spare.empty() || held->shutting.load() ||
                           (!held->living.load() && !held->fetching.load());
                }))
                throw Overdue{};

            if (held->spare.empty()) throw Trouble{"The peer is closed."};

            // Datagram jest całością. Oddanie kawałka udającego całość byłoby gorsze niż odmowa —
            // tak samo rozstrzyga to Udp. Ale **odmowa nie ma prawa go zjeść**: sprawdzamy rozmiar
            // przed zdjęciem z kolejki, więc powtórzenie z większym miejscem jeszcze go zastanie.
            if (held->spare.front().size() > most)
                throw Trouble{"The parcel does not fit in the given space."};

            Bytes one = std::move(held->spare.front());

            held->spare.pop_front();

            kept.unlock();

            resolve(std::move(one));
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Zamknięcie wprost robi dokładnie to, co wypuszczenie ostatniego rozmówcy z rąk — całą robotę
// wykonuje niszczyciel Held. Dwie drogi, jedno miejsce.
void Peer::close() { held_.reset(); }

}

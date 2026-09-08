#include "talcite/relay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

#include "stunning.hpp"

namespace talcite::net {

// To, co widzi pętla odbioru — ten sam podział co w rozmówcy i z tego samego powodu: zadanie z puli,
// które przeżyje swojego właściciela i zawiśnie na centrali, zakleszcza program przy wyjściu.
//
// Nazwa **nie brzmi Living**, choć rola jest ta sama: tamto stoi w peer.cpp, też w przestrzeni
// talcite::net i też na jej poziomie. Dwie różne struktury o jednej nazwie w jednej przestrzeni to
// naruszenie zasady jednej definicji — konsolidator wybiera dowolną z nich, a niszczyciel jednej
// biegnie po polach drugiej. Objawiło się naruszeniem pamięci przy pierwszym uruchomieniu.
// Kanał związany z jednym rozmówcą. `ready` mówi, że serwer go potwierdził; do tej chwili wysyłka
// idzie oznajmieniem, bo kanał nieuznany przez serwer po prostu przepada.
struct Bound {
    Address peer;
    std::uint16_t number = 0;
    std::chrono::steady_clock::time_point until{};
    std::chrono::steady_clock::time_point tried{};
    bool ready = false;
    bool binding = false;
};

struct Lasting {
    Post post;
    Address server;
    std::string user;
    std::string word;

    mutable std::mutex guard;
    std::condition_variable stirring;

    std::string realm;
    std::string nonce;
    Bytes key;

    Address lent;
    Address seen;

    std::deque<Parcel> spare;

    // Trwające zapytanie. Przekaźnik prowadzi **jedną rozmowę naraz** — przydział, odświeżenie,
    // pozwolenie — więc zamiast tablicy wystarczy jedno miejsce na numer i jedno na odpowiedź.
    Number number{};
    Message answer;
    bool waiting = false;
    bool answered = false;

    std::chrono::steady_clock::time_point until{};
    std::chrono::steady_clock::time_point tried{};
    std::chrono::steady_clock::time_point beat{};
    bool renewing = false;
    bool lost = false;

    // Pozwolenia **wygasają osobno od przydziału** — serwer trzyma je pięć minut od ostatniego
    // nadania i ruch ich nie przedłuża. Trzymamy więc listę adresów, którym pozwoliliśmy, żeby
    // odnawiać je razem, jednym zapytaniem.
    std::vector<Address> allowed;
    std::chrono::steady_clock::time_point granted{};
    bool permitting = false;

    // Kanały. Numer jest **naszym** wyborem, nie serwera: prosimy o niego i od tej chwili obie
    // strony rozumieją cztery bajty zamiast trzydziestu sześciu.
    std::vector<Bound> bound;
    std::uint16_t next = 0x4000;

    std::mutex speaking;
    std::mutex talking;

    std::atomic<bool> living{false};
    std::atomic<bool> shutting{false};
};

struct Relay::Held {
    std::shared_ptr<Lasting> alive;

    // Własna kopia poczty, wyłącznie do zamykania — kopie dzielą gniazdo, ale nie dzielą obiektu.
    Post closing;

    ~Held() {
        {
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

constexpr Patience listening{1000};

// Ile życia prosimy dla przydziału i ile przed jego końcem odnawiamy. Serwer może dać mniej, niż
// prosimy, i wtedy liczy się jego słowo — dlatego czas końca bierzemy z odpowiedzi, nie z prośby.
constexpr int living_for = 600;

constexpr int early = 60;

// Ile czekamy przed powtórką nieudanego odświeżenia. Bez tego odstępu każdy obrót pętli — czyli
// **co sekundę** — zaczynał kolejną próbę, bo warunek „pora odnawiać" jest od chwili nieudanej
// próby prawdziwy już zawsze. Zadania piętrzyły się na zamku rozmowy aż do końca przydziału.
constexpr int again = 10;

// Serwer trzyma pozwolenie pięć minut. Odnawiamy z minutą zapasu.
constexpr int allowing = 240;

// Co ile stukamy do serwera, żeby droga do niego nie zarosła.
constexpr int beating = 15;

// Serwer trzyma kanał dziesięć minut. Odnawiamy z minutą zapasu.
constexpr int holding = 600;

constexpr int sooner = 60;

// Ile kanałów naraz. Rozmówca ma jedną drogę, więc jeden kanał starcza; zapas jest na wypadek,
// gdyby jedno gniazdo obsługiwało kilku rozmówców.
constexpr std::size_t roomiest = 16;

constexpr std::size_t plenty = 256;

// Ilu rozmówcom pamiętamy pozwolenie, żeby je odnawiać. Powyżej tej granicy pozwolenie nadaje się
// raz i wygasa po pięciu minutach — bez granicy sama lista byłaby miejscem, do którego wołający
// mógłby wlać ile chce, a każdy wpis rośnie w każdym zapytaniu odnawiającym.
constexpr std::size_t roomier = 64;

// **Próby i podtrzymanie wolno zgubić** — ta sama reguła i ten sam powód co w `Peer`, gdzie stoi
// to od dawna. Bez tego jedno odrzucenie od poczty (choćby „Poczta już wysyła") wywracało całe
// zapytanie zamiast doczekać następnej rundy powtórek — i zostawiało przekaźnik w stanie
// oczekiwania, w którym przyjąłby obcą odpowiedź na stary numer.
void posting(const std::shared_ptr<Lasting>& held, const Address& to, Bytes what,
             Patience patience) {
    try {
        const std::lock_guard<std::mutex> kept{held->speaking};

        if (held->shutting.load()) return;

        held->post.send(to, std::move(what), patience).await();
    } catch (const std::exception&) {
    }
}

Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

void renewed(const std::shared_ptr<Lasting>& held);

void granting(const std::shared_ptr<Lasting>& held);

void keeping(const std::shared_ptr<Lasting>& held);

void binding(const std::shared_ptr<Lasting>& held, const Address& to);

void channeling(const std::shared_ptr<Lasting>& held);

bool turning(const std::shared_ptr<Lasting>& held) {
    Parcel came;

    try {
        if (held->shutting.load()) return false;

        came = held->post.receive(roomy, listening).await();
    } catch (const Overdue&) {
        keeping(held);
        renewed(held);
        granting(held);
        channeling(held);

        return true;
    } catch (const std::exception&) {
        return false;
    }

    // Odpowiedź przychodzi **tylko od serwera**. Cudzy datagram na tym gnieździe nie ma prawa ani
    // udawać odpowiedzi, ani podać się za treść od rozmówcy — od tego jest atrybut adresu w środku
    // wiadomości, a nie to, skąd pakiet przyszedł.
    if (!(came.from == held->server)) return true;

    // Kanał sprawdzamy **przed** wiadomością, bo rozstrzygają o tym te same dwa bity: STUN ma je
    // zerowe, kanał ma tam `01`. Nie ma tu żadnego zgadywania.
    {
        std::uint16_t channel = 0;

        Bytes data;

        if (unwrapped(came.bytes, channel, data)) {
            Address whom;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                // Numer kanału **my** nadaliśmy, więc bierzemy go także wtedy, gdy potwierdzenie
                // jeszcze nie wróciło: serwer ma prawo zacząć nim mówić wcześniej niż odpowiedzieć.
                for (const Bound& one : held->bound)
                    if (one.number == channel) whom = one.peer;

                if (!whom.held()) return true;

                if (held->spare.size() >= plenty) held->spare.pop_front();

                held->spare.push_back(Parcel{whom, std::move(data)});
            }

            held->stirring.notify_all();

            return true;
        }
    }

    Message made;

    if (!understood(came.bytes, made)) return true;

    if (made.sort == Sort::telling && made.task == Task::data) {
        if (!made.peer.held()) return true;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->spare.size() >= plenty) held->spare.pop_front();

            held->spare.push_back(Parcel{made.peer, std::move(made.data)});
        }

        held->stirring.notify_all();

        return true;
    }

    if (made.sort != Sort::told && made.sort != Sort::amiss) return true;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (!held->waiting || made.number != held->number) return true;

        held->answer = std::move(made);
        held->answered = true;
    }

    held->stirring.notify_all();

    return true;
}

void minding(std::weak_ptr<Lasting> weak) {
    for (;;) {
        const std::shared_ptr<Lasting> held = weak.lock();

        if (!held || held->shutting.load()) break;

        if (!turning(held)) break;
    }

    if (const std::shared_ptr<Lasting> held = weak.lock()) {
        held->living.store(false);

        held->stirring.notify_all();
    }
}

void waking(const std::shared_ptr<Lasting>& held) {
    bool was = false;

    if (!held->living.compare_exchange_strong(was, true)) return;

    Promise<void> made{[weak = std::weak_ptr<Lasting>{held}](auto resolve, auto) {
        minding(weak);

        resolve();
    }};

    made.go();
}

// Jedno zapytanie z powtórkami, rozkład jak w STUN: pół sekundy, potem podwojenie. Biegnie na wątku
// z puli i blokuje go — tak samo jak zwiad i z tego samego powodu: zapytań jest garść na cały czas
// życia przydziału, nie jedno na pakiet.
Message asking(const std::shared_ptr<Lasting>& held, const Bytes& what, const Number& number,
               Patience patience) {
    const std::lock_guard<std::mutex> alone{held->talking};

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->number = number;
        held->waiting = true;
        held->answered = false;
    }

    const Clock::time_point until = Clock::now() + patience;

    Patience wait{500};

    while (Clock::now() < until) {
        posting(held, held->server, what, left(until));

        std::unique_lock<std::mutex> kept{held->guard};

        const Patience edge = std::min(wait, left(until));

        if (held->stirring.wait_for(kept, edge, [&held] {
                return held->answered || held->shutting.load();
            })) {
            if (held->shutting.load()) {
                held->waiting = false;

                throw Trouble{"Przekaźnik jest zamknięty."};
            }

            Message made = std::move(held->answer);

            held->waiting = false;
            held->answered = false;

            return made;
        }

        wait *= 2;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->waiting = false;
    }

    throw Overdue{};
}

// Dopisanie tożsamości do zapytania. Bez poświadczeń nie dopisujemy nic — część serwerów, w tym
// wszystkie udawane w testach, przydziela bez uwierzytelniania.
void naming(const std::shared_ptr<Lasting>& held, Bytes& made) {
    const std::lock_guard<std::mutex> kept{held->guard};

    if (held->key.empty()) return;

    texting(made, 0x0006, held->user);
    texting(made, 0x0014, held->realm);
    texting(made, 0x0015, held->nonce);

    stamping(made, held->key);
}

// Przydział wygasa, jeśli nikt go nie odnowi — zwykle po dziesięciu minutach. Odnawiamy z minutą
// zapasu, **na osobnym zadaniu z puli**, a nie tu: odświeżenie jest zapytaniem, a na odpowiedź czeka
// właśnie ta pętla. Odświeżanie z jej wnętrza zakleszczyłoby ją na sobie samej.
void renewed(const std::shared_ptr<Lasting>& held) {
    bool gone = false;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->until == Clock::time_point{}) return;

        // Odświeżenie w biegu **wyklucza drugie**. Wcześniej pilnowało tego przesunięcie czasu
        // końca, ale tamto kłamało o stanie przydziału i po pierwszej nieudanej próbie zostawiało
        // warunek „pora odnawiać" prawdziwy na zawsze.
        if (held->renewing) return;

        if (Clock::now() < held->until - std::chrono::seconds{early}) return;

        // Przydział, którego nie udało się odnowić do jego własnego końca, przepadł. Nadawanie
        // przez niego dalej to nadawanie w próżnię, więc mówimy to wprost zamiast milczeć.
        if (Clock::now() >= held->until) {
            held->until = Clock::time_point{};
            held->lost = true;

            gone = true;
        } else if (held->tried != Clock::time_point{} &&
                   Clock::now() - held->tried < std::chrono::seconds{again}) {
            return;
        } else {
            held->tried = Clock::now();
            held->renewing = true;
        }
    }

    if (gone) {
        held->stirring.notify_all();

        return;
    }

    Promise<void> made{[held](auto resolve, auto) {
        int lasting = 0;

        try {
            const Number number = numbered();

            Bytes what = begun(Task::refresh, Sort::asking, number);

            wording(what, 0x000D, static_cast<std::uint32_t>(living_for));

            naming(held, what);

            const Message told = asking(held, what, number, Patience{5000});

            if (told.sort == Sort::told)
                lasting = told.lifetime > 0 ? told.lifetime : living_for;
        } catch (const std::exception&) {
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            // **Czas końca bierzemy z odpowiedzi, nie z prośby.** Po nieudanej próbie zostaje
            // stary — bo to on mówi prawdę o tym, kiedy serwer nas wyrzuci.
            if (lasting > 0) {
                held->until = Clock::now() + std::chrono::seconds{lasting};
                held->tried = Clock::time_point{};
            }

            held->renewing = false;
        }

        resolve();
    }};

    made.go();
}

// Pozwolenia odnawiamy **wszystkie naraz**: jedno zapytanie niesie tyle adresów rozmówców, ile
// trzeba, a serwer każdemu z nich liczy pięć minut od nowa. Ruch pozwolenia nie przedłuża — cicha
// rozmowa umierała po pięciu minutach i wyglądało to na wadę sieci, nie na wygasły wpis.
void granting(const std::shared_ptr<Lasting>& held) {
    std::vector<Address> whom;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->lost || held->permitting || held->allowed.empty()) return;

        if (Clock::now() - held->granted < std::chrono::seconds{allowing}) return;

        held->granted = Clock::now();
        held->permitting = true;

        whom = held->allowed;
    }

    Promise<void> made{[held, whom](auto resolve, auto) {
        bool done = false;

        try {
            const Number number = numbered();

            Bytes what = begun(Task::permission, Sort::asking, number);

            for (const Address& one : whom) placing(what, 0x0012, one, true);

            naming(held, what);

            done = asking(held, what, number, Patience{5000}).sort == Sort::told;
        } catch (const std::exception&) {
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            // Nieudane odnowienie powtarzamy za chwilę, a nie za cały okres — inaczej jedno zgubione
            // zapytanie kosztowałoby resztę pozwolenia.
            if (!done)
                held->granted = Clock::now() - std::chrono::seconds{allowing - again};

            held->permitting = false;
        }

        resolve();
    }};

    made.go();
}

// Związanie kanału z rozmówcą. Numer wybieramy sami i **trzymamy go od razu**, jeszcze przed
// odpowiedzią: dzięki temu treść, którą serwer zdąży nim przysłać, ma już do kogo należeć.
//
// Kanał wiąże się z adresem **razem z portem**, inaczej niż pozwolenie. Dlatego nie robi się tego
// przy `allow`, tylko przy pierwszej wysyłce — dopiero wtedy port jest znany i prawdziwy.
void binding(const std::shared_ptr<Lasting>& held, const Address& to) {
    std::uint16_t number = 0;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->lost) return;

        bool known = false;

        for (Bound& one : held->bound)
            if (one.peer == to) {
                known = true;

                if (one.binding) return;

                if (one.ready && Clock::now() < one.until - std::chrono::seconds{sooner}) return;

                // Nieudane wiązanie powtarzamy co chwilę, a nie przy każdej wysyłce — inaczej
                // rozmowa z serwerem, który kanałów nie daje, wysyłałaby po zapytaniu na datagram.
                if (Clock::now() - one.tried < std::chrono::seconds{again}) return;

                one.binding = true;
                one.tried = Clock::now();
                number = one.number;

                break;
            }

        if (!known) {
            if (held->bound.size() >= roomiest) return;

            number = held->next++;

            if (held->next > 0x7FFF) held->next = 0x4000;

            held->bound.push_back(Bound{to, number, {}, Clock::now(), false, true});
        }
    }

    Promise<void> made{[held, to, number](auto resolve, auto) {
        bool done = false;

        try {
            const Number one = numbered();

            Bytes what = begun(Task::channel, Sort::asking, one);

            wording(what, 0x000C, static_cast<std::uint32_t>(number) << 16);

            placing(what, 0x0012, to, true);

            naming(held, what);

            done = asking(held, what, one, Patience{5000}).sort == Sort::told;
        } catch (const std::exception&) {
        }

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            for (Bound& one : held->bound)
                if (one.number == number) {
                    one.binding = false;

                    if (done) {
                        one.ready = true;
                        one.until = Clock::now() + std::chrono::seconds{holding};
                    }
                }
        }

        resolve();
    }};

    made.go();
}

// Kanał żyje dziesięć minut i **ruch go nie przedłuża** — tak samo jak pozwolenie. Przeterminowany
// przestaje działać po cichu, więc odnawiamy go zawczasu.
void channeling(const std::shared_ptr<Lasting>& held) {
    std::vector<Address> whom;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->lost) return;

        for (const Bound& one : held->bound)
            if (one.ready && Clock::now() >= one.until - std::chrono::seconds{sooner})
                whom.push_back(one.peer);
    }

    for (const Address& one : whom) binding(held, one);
}

// Odwzorowanie na NAT-cie między nami a serwerem żyje zwykle trzydzieści sekund od ostatniego ruchu,
// a odświeżenia przydziału chodzą raz na dziewięć minut. Bez tego stukania serwer po pół minuty
// ciszy nie miałby już czym do nas wrócić i dane od rozmówcy przepadałyby po drodze — przydział
// byłby żywy, a mimo to głuchy. Oznajmienie wiążące jest do tego stworzone: nikt na nie nie
// odpowiada, więc nie ma na co czekać ani czego dopasowywać.
void keeping(const std::shared_ptr<Lasting>& held) {
    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->until == Clock::time_point{}) return;

        if (Clock::now() - held->beat < std::chrono::seconds{beating}) return;

        held->beat = Clock::now();
    }

    try {
        posting(held, held->server, begun(Task::binding, Sort::telling, numbered()), listening);
    } catch (const std::exception&) {
    }
}

template <typename T>
Promise<T> amiss_now(const std::string& said) {
    return Promise<T>{
        [said](auto, auto reject) { reject(std::make_exception_ptr(Trouble{said})); }};
}

}

Relay::Relay(Post post, Address server, std::string user, std::string word)
    : held_(std::make_shared<Held>()) {
    held_->alive = std::make_shared<Lasting>();
    held_->alive->post = post;
    held_->alive->server = std::move(server);
    held_->alive->user = std::move(user);
    held_->alive->word = std::move(word);
    held_->closing = std::move(post);
}

// Pusty przekaźnik oddaje pusty adres. Reszta metod pyta o uchwyt, te dwie pytać zapomniały —
// a przekaźnik ma konstruktor domyślny i `held()`, więc wołanie na pustym jest przewidziane.
Address Relay::lent() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->lent;
}

Address Relay::seen() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->alive->guard};

    return held_->alive->seen;
}

// Przydział. Serwer, który wymaga uwierzytelnienia, odpowiada najpierw odmową 401 z dziedziną
// i jednorazowym znacznikiem — i **dopiero z nich da się policzyć klucz**, więc pierwsze zapytanie
// zawsze idzie bez podpisu, a drugie z podpisem. Tak każe RFC i tak działa każdy serwer TURN.
Promise<Address> Relay::open(Patience patience) {
    if (!held_) return amiss_now<Address>("Przekaźnik jest pusty.");

    auto held = held_->alive;

    Promise<Address> made{[held, patience](auto resolve, auto reject) {
        try {
            waking(held);

            const Clock::time_point until = Clock::now() + patience;

            Number number = numbered();

            Bytes what = begun(Task::allocate, Sort::asking, number);

            // Przekaźnik ma nam podać adres dla UDP; siedemnastka to numer tego protokołu.
            wording(what, 0x0019, 17u << 24);

            Message told = asking(held, what, number, left(until));

            if (told.sort == Sort::amiss && (told.code == 401 || told.code == 438)) {
                if (held->user.empty())
                    throw Trouble{"Przekaźnik żąda poświadczeń, a nie mam żadnych.", told.code};

                {
                    const std::lock_guard<std::mutex> kept{held->guard};

                    held->realm = told.realm;
                    held->nonce = told.nonce;
                    held->key = keyed(held->user, told.realm, held->word);
                }

                number = numbered();

                what = begun(Task::allocate, Sort::asking, number);

                wording(what, 0x0019, 17u << 24);

                naming(held, what);

                told = asking(held, what, number, left(until));
            }

            if (told.sort == Sort::amiss)
                throw Trouble{"Przekaźnik odmówił przydziału: " +
                                  (told.said.empty() ? std::to_string(told.code) : told.said) + ".",
                              told.code, held->server};

            if (!told.lent.held()) throw Trouble{"Przekaźnik nie podał użyczonego adresu."};

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                held->lent = told.lent;
                held->seen = told.seen;

                // **Czas końca bierzemy z odpowiedzi, nie z prośby.** Serwer ma prawo dać mniej,
                // a odświeżanie liczone od tego, o co prosiliśmy, spóźniłoby się o tę różnicę.
                held->until =
                    Clock::now() +
                    std::chrono::seconds{told.lifetime > 0 ? told.lifetime : living_for};

                held->beat = Clock::now();
            }

            resolve(told.lent);
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.go();

    return made;
}

// Przekaźnik nie przepuści ani jednego bajtu od kogoś, o kim mu nie powiedziano. To nie jest
// grzeczność, tylko obrona samego przekaźnika przed staniem się otwartym wzmacniaczem.
Promise<void> Relay::allow(Address peer, Patience patience) {
    if (!held_) return amiss_now<void>("Przekaźnik jest pusty.");

    if (!peer.held()) return amiss_now<void>("Pusty adres nie jest rozmówcą.");

    auto held = held_->alive;

    Promise<void> made{[held, peer, patience](auto resolve, auto reject) {
        try {
            // Pozwolenie jest zapytaniem, a odpowiedź na nie rozpoznaje **pętla odbioru** — więc
            // musi chodzić. Zwykle chodzi już od przydziału, ale nie wolno na to liczyć: bez tego
            // pozwolenie nadane pierwszym ruchem czekałoby na odpowiedź, której nikt nie czyta.
            waking(held);

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->lost) throw Trouble{"Przydział przepadł."};
            }

            const Number number = numbered();

            Bytes what = begun(Task::permission, Sort::asking, number);

            placing(what, 0x0012, peer, true);

            naming(held, what);

            const Message told = asking(held, what, number, patience);

            if (told.sort == Sort::amiss)
                throw Trouble{"Przekaźnik odmówił pozwolenia: " +
                                  (told.said.empty() ? std::to_string(told.code) : told.said) + ".",
                              told.code, held->server};

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                bool known = false;

                for (const Address& one : held->allowed)
                    if (one.family() == peer.family() && one.bytes() == peer.bytes()) known = true;

                // Pozwolenie sięga **adresu, nie portu** — tak każe RFC. Dlatego pamiętamy je po
                // adresie: drugi port tego samego gospodarza jest już przepuszczony.
                if (!known && held->allowed.size() < roomier) {
                    // Zegar odnawiania rusza dopiero od pierwszego wpisu. Przestawianie go przy
                    // każdym kolejnym odsuwałoby odnowienie tych, które czekają już dawno.
                    if (held->allowed.empty()) held->granted = Clock::now();

                    held->allowed.push_back(peer);
                }
            }

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.go();

    return made;
}

// Wysyłka idzie **oznajmieniem**, nie zapytaniem: nie ma na nią odpowiedzi i nie ma czego czekać.
// Gubi się tak samo jak zwykły datagram, bo to jest zwykły datagram, tylko opakowany.
Promise<void> Relay::send(Address to, Bytes what, Patience patience) {
    if (!held_) return amiss_now<void>("Przekaźnik jest pusty.");

    if (!to.held()) return amiss_now<void>("Pusty adres nie jest celem.");

    auto held = held_->alive;

    Promise<void> made{[held, to, what = std::move(what), patience](auto resolve,
                                                                   auto reject) mutable {
        try {
            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->lost) throw Trouble{"Przydział przepadł."};
            }

            std::uint16_t channel = 0;

            {
                const std::lock_guard<std::mutex> kept{held->guard};

                for (const Bound& one : held->bound)
                    if (one.peer == to && one.ready && Clock::now() < one.until)
                        channel = one.number;
            }

            if (channel != 0) {
                posting(held, held->server, wrapped(channel, what), patience);

                resolve();

                return;
            }

            // Kanału jeszcze nie ma, więc ta przesyłka idzie **oznajmieniem**, a kanał zamawiamy
            // w tle. Czekanie na niego kosztowałoby całą rozmowę tyle, ile jedno zapytanie do
            // serwera — a oznajmienie działa od pierwszego bajtu.
            binding(held, to);

            Bytes one = begun(Task::sending, Sort::telling, numbered());

            placing(one, 0x0012, to, true);

            giving(one, what);

            posting(held, held->server, std::move(one), patience);

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.go();

    return made;
}

Promise<Parcel> Relay::receive(std::size_t most, Patience patience) {
    if (!held_) return amiss_now<Parcel>("Przekaźnik jest pusty.");

    auto held = held_->alive;

    Promise<Parcel> made{[held, most, patience](auto resolve, auto reject) {
        try {
            waking(held);

            std::unique_lock<std::mutex> kept{held->guard};

            if (!held->stirring.wait_for(kept, patience, [&held] {
                    return !held->spare.empty() || held->lost || held->shutting.load() ||
                           !held->living.load();
                }))
                throw Overdue{};

            // Odebrane wcześniej oddajemy nawet wtedy, gdy przydział już przepadł — te bajty
            // naprawdę przyszły i nikt ich nie unieważnił.
            if (held->spare.empty() && held->lost) throw Trouble{"Przydział przepadł."};

            if (held->spare.empty()) throw Trouble{"Przekaźnik jest zamknięty."};

            if (held->spare.front().bytes.size() > most)
                throw Trouble{"Przesyłka nie mieści się w podanym miejscu."};

            Parcel one = std::move(held->spare.front());

            held->spare.pop_front();

            kept.unlock();

            resolve(std::move(one));
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.go();

    return made;
}

void Relay::close() { held_.reset(); }

}

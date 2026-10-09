#include "talcite/stunclient.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/time.hpp"

#include "stunning.hpp"

namespace talcite::net {

// Znacznik zdejmuje **śmierć obietnicy**, a nie jej ciało. Wcześniej zerowało go samo ciało, więc
// pytanie anulowane albo porzucone nieruszone unieruchamiało zwiadowcę na dobre. Teraz znika
// najpóźniej wtedy, gdy wołający wypuści obietnicę z rąk.
struct Asking {
    explicit Asking(std::shared_ptr<StunClient::Held> given) : held(std::move(given)) {}

    ~Asking();

    Asking(const Asking&) = delete;
    Asking& operator=(const Asking&) = delete;
    Asking(Asking&&) = delete;
    Asking& operator=(Asking&&) = delete;

    std::shared_ptr<StunClient::Held> held;
};

struct StunClient::Held {
    Udp post;
    std::vector<Address> servers;
    Patience patience{0};

    mutable std::mutex guard;
    Address last;
    Mapping known = Mapping::unknown;

    std::atomic<bool> asking{false};
};

Asking::~Asking() { held->asking.store(false); }

namespace {

using Clock = std::chrono::steady_clock;

Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// Jedno pytanie do jednego serwera, razem z powtórkami. **Biegnie na wątku z puli i blokuje go**
// — świadomie. Przy TLS taki wybór byłby zły, bo tam czekanie jest na połączenie i rośnie razem
// z ich liczbą; tu pytanie jest jedno na gniazdo, zadawane raz, przed rozmową. A pula dla pracy
// zwykłej rośnie bez ograniczeń, więc zakończenia z centrali nie mają jak się zagłodzić.
Address trying(StunClient::Held& held, const Address& where, Clock::time_point until) {
    const Number number = numbered();
    const Bytes what = asked(number);

    // Rozkład z RFC: pół sekundy, potem podwojenie. Terminem na całość jest Patience, więc
    // wołający nie musi o tym rozkładzie wiedzieć.
    Patience wait{500};

    while (Clock::now() < until) {
        held.post.send(where, what, left(until)).await();

        const Clock::time_point edge = std::min(Clock::now() + wait, until);

        while (Clock::now() < edge) {
            Datagram came;

            try {
                came = held.post.receive(1500, left(edge)).await();
            } catch (const Overdue&) {
                break;
            } catch (const Trouble&) {
                // Za duży datagram albo inny kłopot na gnieździe **nie jest odpowiedzią serwera**
                // i nie ma prawa wywrócić pytania. To gniazdo niesie potem także ruch rozmówcy,
                // więc jeden zwykły duży pakiet kończył wcześniej cały zwiad.
                continue;
            }

            Message made;

            // Nie STUN albo cudza odpowiedź — czekamy dalej. Numer transakcji jest jedynym
            // dowodem tożsamości odpowiedzi i tylko on decyduje.
            if (!understood(came.bytes, made)) continue;

            if (made.number != number) continue;

            if (made.sort == Sort::amiss)
                throw Trouble{"The STUN server refused: " +
                                  (made.said.empty() ? std::to_string(made.code) : made.said) + ".",
                              made.code, where};

            if (made.seen.held()) return made.seen;
        }

        wait *= 2;
    }

    return {};
}

Address hunting(StunClient::Held& held) {
    // Wykaz bierzemy **pod zamkiem i na własność**. Czytany wprost, mógł się przealokować pod
    // pętlą zakresową, gdyby ktoś równolegle wołał add().
    std::vector<Address> servers;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        servers = held.servers;
    }

    if (servers.empty()) throw Trouble{"The scout knows no server."};

    const Clock::time_point until = Clock::now() + held.patience;

    // Odmowa albo kłopot na jednym serwerze **nie kończy obchodu**. Wcześniej wystarczył jeden źle
    // ustawiony albo złośliwy serwer na początku listy, żeby wszystkie następne nie dostały ani
    // jednego pakietu.
    std::exception_ptr first;

    for (const Address& where : servers) {
        if (Clock::now() >= until) break;

        try {
            if (const Address made = trying(held, where, until); made.held()) return made;
        } catch (const Trouble&) {
            if (!first) first = std::current_exception();
        }
    }

    if (first) std::rethrow_exception(first);

    throw Overdue{};
}

}

StunClient::StunClient(Udp post, Patience patience) : held_(std::make_shared<Held>()) {
    held_->post = std::move(post);
    held_->patience = patience;
}

// Pusty zwiad przyjmuje wszystko i nie oddaje nic — zamiast przewracać program. Reszta metod pyta
// o uchwyt, te zapomniały.
void StunClient::add(Address server) {
    if (!held_) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->servers.push_back(std::move(server));
}

const std::vector<Address>& StunClient::servers() const {
    static const std::vector<Address> none;

    return held_ ? held_->servers : none;
}

// Wykaz oddajemy przez referencję, bo tak stoi w nagłówku. Wołający, który dopisuje serwery
// z drugiego wątku w trakcie czytania tej referencji, i tak jej nie utrzyma — to jest ta sama
// umowa co przy każdym typie wartościowym w talcite.

Address StunClient::last() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->guard};

    return held_->last;
}

StunClient::Mapping StunClient::known() const {
    if (!held_) return {};

    const std::lock_guard<std::mutex> kept{held_->guard};

    return held_->known;
}

// Wszystkie pytania idą tym samym gniazdem, a poczta odbiera po jednym naraz — więc drugie
// pytanie zaczęte w trakcie pierwszego zjadłoby cudzą odpowiedź. Odmawiamy, zamiast psuć.
Promise<Address> StunClient::seen() const {
    if (!held_)
        return Promise<Address>{[](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The scout is empty."}));
        }};

    bool was = false;

    if (!held_->asking.compare_exchange_strong(was, true))
        return Promise<Address>{[](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The scout is already asking."}));
        }};

    auto held = held_;

    Promise<Address> made{[held, kept = std::make_shared<Asking>(held)](auto resolve,
                                                                                 auto reject) {
        try {
            const Address one = hunting(*held);

            {
                const std::lock_guard<std::mutex> also{held->guard};

                held->last = one;
            }

            resolve(one);
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Dwa pytania do dwóch **różnych** serwerów, z tego samego gniazda. Ten sam adres i port
// w obu odpowiedziach znaczy, że NAT nadaje odwzorowanie niezależnie od celu — i przebijanie ma
// sens. Różne znaczy NAT symetryczny: adres poznany od serwera jest dla drugiej strony
// bezużyteczny i przebijanie nie zadziała **nigdy**, choćby się próbowało bez końca.
Promise<StunClient::Mapping> StunClient::mapping() const {
    if (!held_)
        return Promise<Mapping>{[](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The scout is empty."}));
        }};

    bool was = false;

    if (!held_->asking.compare_exchange_strong(was, true))
        return Promise<Mapping>{[](auto, auto reject) {
            reject(std::make_exception_ptr(Trouble{"The scout is already asking."}));
        }};

    auto held = held_;

    Promise<Mapping> made{[held, kept = std::make_shared<Asking>(held)](auto resolve,
                                                                                auto reject) {
        try {
            Mapping what = Mapping::unknown;

            std::vector<Address> servers;

            {
                const std::lock_guard<std::mutex> also{held->guard};

                servers = held->servers;
            }

            // Werdykt wymaga **dwóch różnych** serwerów. Ten sam adres dwa razy dawał dwie
            // identyczne odpowiedzi, a z nich werdykt „zgodne" — czyli kłamstwo w stronę
            // niebezpieczną: że przebijanie zadziała, choć NAT może być symetryczny.
            Address first;
            Address other;

            for (const Address& one : servers) {
                if (!first.held()) {
                    first = one;

                    continue;
                }

                if (!(one == first)) {
                    other = one;

                    break;
                }
            }

            // Przy jednym serwerze „nie wiem" jest prawdziwą odpowiedzią, a nie awarią.
            if (first.held() && other.held()) {
                // **Każde pytanie dostaje własną połowę cierpliwości.** Wspólny termin oznaczał,
                // że milczący pierwszy serwer zjadał go w całości, drugi nie dostawał ani jednego
                // pakietu, a mapping kończył się pomyślnym „nie wiem".
                const Patience half{held->patience.count() / 2 + 1};

                const Address one = trying(*held, first, Clock::now() + half);
                const Address two = trying(*held, other, Clock::now() + half);

                if (one.held() && two.held())
                    what = one == two ? Mapping::alike : Mapping::symmetric;

                if (one.held()) {
                    const std::lock_guard<std::mutex> also{held->guard};

                    held->last = one;
                }
            }

            {
                const std::lock_guard<std::mutex> also{held->guard};

                held->known = what;
            }

            resolve(what);
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    made.start();

    return made;
}

}

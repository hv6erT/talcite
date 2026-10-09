#include "talcite/beacon.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "talcite/random.hpp"
#include "talcite/udp.hpp"

// **Majak w sieci lokalnej** (S1). Udostępniający słucha na znanym porcie UDP i odpowiada na
// pytanie o znacznik, który zna; pytający rozgłasza pytanie w swojej sieci i zbiera odpowiedzi.
// Tak biorący znajduje zaproszenie bez adresu — wystarczy mu kod. Znacznik i treść odpowiedzi to
// bajty wołającego: majak nie wie, co znaczą, i nie powinien wiedzieć (twa wkłada tu skrót
// etykiety, a nie ją samą, i numer portu).
//
// **Wiadomość**: `TLCB`, wersja, rodzaj (pytanie, odpowiedź), osiem bajtów losowych z pytania
// (odpowiedź je powtarza — po nich pytający poznaje odpowiedzi na swoje pytanie, a nie cudze),
// długość i znacznik, a w odpowiedzi jeszcze długość i treść. Wszystko, co do tego kształtu nie
// pasuje — inna wersja, ucięte, za długie, nadmiarowe bajty — jest ignorowane bez słowa: port
// stoi w sieci, a obcy datagram nie ma prawa niczego zmienić.
//
// Rozgłaszamy tylko IPv4 (255.255.255.255): sieć domowa ma IPv4 zawsze, a IPv6 bez niego — rzadko.
// Odpowiedź idzie wprost do pytającego, nie rozgłoszeniem.
namespace talcite::net {

namespace {

constexpr std::byte magic[4]{std::byte{'T'}, std::byte{'L'}, std::byte{'C'}, std::byte{'B'}};
constexpr std::byte version{1};
constexpr std::byte asking{1};
constexpr std::byte answering{2};
constexpr std::size_t nonce = 8;
constexpr std::size_t widest_datagram = 4 + 2 + nonce + 1 + Beacon::widest_tag + 2 + Beacon::widest_payload;

// Ile odpowiedzi na sekundę majak daje najwyżej — zapytany zalewem nie staje się wzmacniaczem.
constexpr int answers_per_second = 32;

struct Read {
    std::byte kind{};
    Bytes nonce;
    Bytes tag;
    Bytes payload;
};

Bytes written(std::byte kind, const Bytes& once, const Bytes& tag, const Bytes* payload) {
    Bytes made(magic, magic + 4);

    made.push_back(version);
    made.push_back(kind);
    made.insert(made.end(), once.begin(), once.end());
    made.push_back(static_cast<std::byte>(tag.size()));
    made.insert(made.end(), tag.begin(), tag.end());

    if (payload != nullptr) {
        made.push_back(static_cast<std::byte>(payload->size() >> 8));
        made.push_back(static_cast<std::byte>(payload->size() & 0xff));
        made.insert(made.end(), payload->begin(), payload->end());
    }

    return made;
}

std::optional<Read> read(const Bytes& got) {
    if (got.size() < 4 + 2 + nonce + 1 || std::memcmp(got.data(), magic, 4) != 0 || got[4] != version) return std::nullopt;

    Read made;

    // Rodzaj i długość znacznika sprawdza ten, kto czyta: majak odpowiada tylko na pytanie o swój
    // znacznik, a pytający bierze tylko odpowiedź ze swoim — innego rodzaju ani dłuższego znacznika
    // żaden z nich nie zna.
    made.kind = got[5];

    std::size_t at = 6;

    made.nonce.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.begin() + static_cast<std::ptrdiff_t>(at + nonce));
    at += nonce;

    const auto tagged = static_cast<std::size_t>(got[at]);

    ++at;

    if (at + tagged > got.size()) return std::nullopt;

    made.tag.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.begin() + static_cast<std::ptrdiff_t>(at + tagged));
    at += tagged;

    if (made.kind == asking) return at == got.size() ? std::optional{made} : std::nullopt;

    if (at + 2 > got.size()) return std::nullopt;

    const std::size_t much = (static_cast<std::size_t>(got[at]) << 8) | static_cast<std::size_t>(got[at + 1]);

    at += 2;

    if (much > Beacon::widest_payload || at + much != got.size()) return std::nullopt;

    made.payload.assign(got.begin() + static_cast<std::ptrdiff_t>(at), got.end());

    return made;
}

}

struct Beacon::Held : std::enable_shared_from_this<Beacon::Held> {
    // Odbiór w centrali trzyma pocztę przy życiu, więc sama śmierć majaka by jej nie zamknęła —
    // a odbiór bez terminu czekałby wtedy do końca programu. Zamykamy pocztę wprost: zamknięta
    // odrzuca odbiór i wysyłkę, a łańcuch kończy się sam. Samo anulowanie łańcucha nie dosięgało
    // wysyłki odpowiedzi, która akurat biegła w `all_settled`.
    ~Held() { post.close(); }

    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;

    Udp post;
    Address mine;

    std::mutex guard;
    std::map<Bytes, Bytes> offers;

    std::optional<Promise<void>> running;

    std::chrono::steady_clock::time_point second{};
    int answered = 0;

    // Odpowiedź na pytanie, które pasuje do czegoś, co oferujemy — albo nic.
    std::optional<Bytes> answer(const Bytes& got) {
        const std::optional<Read> asked = read(got);

        if (!asked || asked->kind != asking) return std::nullopt;

        const std::lock_guard<std::mutex> kept{guard};

        const auto found = offers.find(asked->tag);

        if (found == offers.end()) return std::nullopt;

        const auto now = std::chrono::steady_clock::now();

        if (now - second >= std::chrono::seconds{1}) {
            second = now;
            answered = 0;
        }

        if (++answered > answers_per_second) return std::nullopt;

        return written(answering, asked->nonce, asked->tag, &found->second);
    }

    // Jedna runda: datagram, odpowiedź, następna runda. Ciąg trzyma majak słabo — zamknięty
    // majak kończy pętlę, bo zamknięta poczta odmawia odbioru.
    static Promise<void> listened(const std::weak_ptr<Held>& weak) {
        const std::shared_ptr<Held> self = weak.lock();

        if (!self || !self->post.held()) return Promise<void>{[](auto resolve, auto) { resolve(); }};

        return self->post.receive(widest_datagram + 1, Patience{0}).then([weak](Datagram got) {
            const std::shared_ptr<Held> self = weak.lock();

            if (!self) return Promise<void>{[](auto resolve, auto) { resolve(); }};

            std::optional<Bytes> said = self->answer(got.bytes);

            Promise<void> sent = said ? Promise<void>::all_settled(self->post.send(got.from, std::move(*said), Patience{1000}))
                                      : Promise<void>{[](auto resolve, auto) { resolve(); }};

            return sent.then([weak] { return listened(weak); });
        });
    }
};

namespace {

const Address& nowhere() {
    static const Address one;

    return one;
}

}

// Majak słucha na wszystkich adresach IPv4 tego komputera.
Beacon::Beacon(std::uint16_t port) : held_(std::make_shared<Held>()) {
    held_->post = Udp{Address::any(Family::four, port)};
    held_->mine = held_->post.mine();
    held_->running.emplace(Held::listened(held_));
    held_->running->start();
}

const Address& Beacon::mine() const { return held_ ? held_->mine : nowhere(); }

void Beacon::offer(Bytes tag, Bytes payload) const {
    if (tag.empty() || tag.size() > widest_tag) throw Trouble{"A beacon tag takes 1 to 64 bytes."};

    if (payload.size() > widest_payload) throw Trouble{"A beacon answer takes at most 1024 bytes."};

    if (!held_) throw Trouble{"The beacon is closed."};

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->offers[std::move(tag)] = std::move(payload);
}

void Beacon::withdraw(const Bytes& tag) const {
    if (!held_) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    held_->offers.erase(tag);
}

void Beacon::close() {
    if (!held_) return;

    held_->post.close();
    held_.reset();
}

namespace {

struct Seeking {
    Udp post;
    Bytes once;
    Bytes tag;
    std::chrono::steady_clock::time_point until;
    std::vector<Sighting> found;
};

Promise<void> gathered(std::shared_ptr<Seeking> seeking) {
    const auto left = std::chrono::duration_cast<Patience>(seeking->until - std::chrono::steady_clock::now());

    if (left <= Patience{0}) return Promise<void>{[](auto resolve, auto) { resolve(); }};

    return seeking->post.receive(widest_datagram + 1, left)
        .then([seeking](Datagram got) {
            const std::optional<Read> told = read(got.bytes);

            if (told && told->kind == answering && told->nonce == seeking->once && told->tag == seeking->tag) {
                Sighting one{got.from, told->payload};

                if (std::find(seeking->found.begin(), seeking->found.end(), one) == seeking->found.end())
                    seeking->found.push_back(std::move(one));
            }

            return gathered(seeking);
        });
}

// Pytanie wysłane na każdy adres po kolei. Adres, na który wysyłka odmówi (brak sieci, brak trasy
// rozgłoszeniowej), nie przerywa pozostałych.
Promise<void> asked(std::shared_ptr<Seeking> seeking, std::shared_ptr<std::vector<Address>> to, std::size_t at,
                    std::shared_ptr<Bytes> question) {
    if (at >= to->size()) return Promise<void>{[](auto resolve, auto) { resolve(); }};

    return Promise<void>::all_settled(seeking->post.send((*to)[at], *question, Patience{1000}))
        .then([seeking, to, at, question] { return asked(seeking, to, at + 1, question); });
}

}

// **Szukanie**: pytanie na adres rozgłoszeniowy (albo na podane adresy — ten sam komputer, próby),
// potem odpowiedzi zbierane do końca cierpliwości. Te same odpowiedzi od tego samego nadawcy liczą
// się raz. Brak odpowiedzi to pusta lista, nie odmowa — „nikogo tu nie ma" jest wynikiem.
Promise<std::vector<Sighting>> Beacon::sought(std::uint16_t port, Bytes tag, Patience patience,
                                             std::vector<Address> to) {
    // Odmowa obietnicą, nie wyjątkiem z wołania — także wtedy, gdy nie dało się otworzyć gniazda.
    const auto refused = [](std::exception_ptr why) {
        return Promise<std::vector<Sighting>>{[why](auto, auto reject) { reject(why); }};
    };

    if (tag.empty() || tag.size() > widest_tag)
        return refused(std::make_exception_ptr(Trouble{"A beacon tag takes 1 to 64 bytes."}));

    auto seeking = std::make_shared<Seeking>();

    try {
        seeking->post = Udp{Address::any(Family::four, 0)};
    } catch (...) {
        return refused(std::current_exception());
    }

    seeking->post.broadcast(true);
    seeking->once = talcite::random(nonce);
    seeking->tag = std::move(tag);
    seeking->until = std::chrono::steady_clock::now() + patience;

    if (to.empty()) to.push_back(Address{"255.255.255.255", port});

    const auto question = std::make_shared<Bytes>(written(asking, seeking->once, seeking->tag, nullptr));
    const auto targets = std::make_shared<std::vector<Address>>(std::move(to));

    return asked(seeking, targets, 0, question)
        .then([seeking] {
            return Promise<void>::all_settled(gathered(seeking));
        })
        .then([seeking] {
            seeking->post.close();

            return std::move(seeking->found);
        });
}

}

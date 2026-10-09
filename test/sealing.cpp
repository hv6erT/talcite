#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/channel.hpp"
#include "talcite/cipher.hpp"
#include "talcite/digest.hpp"
#include "talcite/handshake.hpp"
#include "talcite/listener.hpp"
#include "talcite/random.hpp"
#include "talcite/seam.hpp"
#include "talcite/stream.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

// **Uzgodnienie i kanał szyfrowany** (S1 w twa). Rdzeń talcite nie linkuje adapterów, więc szyfry
// daje tu atrapa, która **nie jest kryptografią** i nie udaje, że nią jest: Diffie-Hellman w ciele
// modulo 2^61−1 i „pieczęć" z mieszania splitmix i FNV. Atrapa ma tylko te własności, których
// sprawdzają próby — obie strony dochodzą do tego samego sekretu, inny klucz albo zmieniony bajt
// łamie pieczęć — a prawdziwe X25519 i AES-GCM sprawdzają wektory w talcite-adapters.
//
// Kanał sprawdzamy na atrapie uzgodnienia ze stałymi kluczami, więc te próby biegną wszędzie.
// Drogę z długim kluczem liczy HKDF z systemowych skrótów — tam, gdzie system ich nie daje, jej
// próby pomijają się tak samo jak próby skrótów.
namespace {

const net::Patience soon{4000};

constexpr std::uint64_t prime = (std::uint64_t{1} << 61) - 1;

std::uint64_t times(std::uint64_t one, std::uint64_t two) {
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(one) * two) % prime);
}

std::uint64_t power(std::uint64_t base, std::uint64_t exponent) {
    std::uint64_t made = 1;

    for (; exponent != 0; exponent >>= 1) {
        if ((exponent & 1) != 0) made = times(made, base);

        base = times(base, base);
    }

    return made;
}

Bytes spread(std::uint64_t value) {
    Bytes made(32);

    for (std::size_t at = 0; at < 8; ++at) made[at] = static_cast<std::byte>((value >> (8 * at)) & 0xff);

    return made;
}

std::optional<std::uint64_t> gathered(const Bytes& one) {
    if (one.size() != 32) return std::nullopt;

    std::uint64_t made = 0;

    for (std::size_t at = 0; at < 32; ++at) {
        const auto each = std::to_integer<std::uint64_t>(one[at]);

        if (at >= 8 && each != 0) return std::nullopt;

        if (at < 8) made |= each << (8 * at);
    }

    return made;
}

class Agreeing : public Agreement {
public:
    explicit Agreeing(const Bytes& secret) {
        for (std::size_t at = 0; at < 8 && at < secret.size(); ++at)
            secret_ |= std::to_integer<std::uint64_t>(secret[at]) << (8 * at);

        secret_ = secret_ % (prime - 3) + 2;
    }

    [[nodiscard]] Bytes mine() const override { return spread(power(3, secret_)); }

    [[nodiscard]] std::optional<Bytes> shared(const Bytes& theirs) const override {
        const std::optional<std::uint64_t> value = gathered(theirs);

        if (!value || *value < 2 || *value > prime - 2) return std::nullopt;

        return spread(power(*value, secret_));
    }

private:
    std::uint64_t secret_ = 0;
};

std::uint64_t mixed(std::uint64_t state) {
    state += 0x9e3779b97f4a7c15ULL;
    state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
    state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;

    return state ^ (state >> 31);
}

void poured(std::uint64_t& into, const Bytes& what) {
    for (const std::byte one : what) into = (into ^ std::to_integer<std::uint64_t>(one)) * 0x100000001b3ULL;

    into = mixed(into ^ what.size());
}

class Ciphers : public CipherProvider {
public:
    [[nodiscard]] std::string_view name() const override { return "fake ciphers"; }

    [[nodiscard]] std::unique_ptr<Agreement> agreement() override { return std::make_unique<Agreeing>(random(8)); }

    [[nodiscard]] std::unique_ptr<Agreement> agreement(const Bytes& secret) override {
        return std::make_unique<Agreeing>(secret);
    }

    [[nodiscard]] Bytes sealed(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& plain) override {
        Bytes made = streamed(key, nonce, plain);
        const Bytes stamp = stamped(key, nonce, extra, made);

        made.insert(made.end(), stamp.begin(), stamp.end());

        return made;
    }

    [[nodiscard]] std::optional<Bytes> opened(const Bytes& key, const Bytes& nonce, const Bytes& extra,
                                              const Bytes& sealed) override {
        if (sealed.size() < 16) return std::nullopt;

        const Bytes body(sealed.begin(), sealed.end() - 16);

        if (!(stamped(key, nonce, extra, body) == Bytes(sealed.end() - 16, sealed.end()))) return std::nullopt;

        return streamed(key, nonce, body);
    }

private:
    static void sized(const Bytes& key, const Bytes& nonce) {
        if (key.size() != 32 || nonce.size() != 12) throw Trouble{"fake ciphers want a 32-byte key and a 12-byte nonce"};
    }

    static Bytes streamed(const Bytes& key, const Bytes& nonce, const Bytes& what) {
        sized(key, nonce);

        std::uint64_t seed = 0xcbf29ce484222325ULL;

        poured(seed, key);
        poured(seed, nonce);

        Bytes made = what;

        for (std::size_t at = 0; at < made.size(); ++at)
            made[at] ^= static_cast<std::byte>(mixed(seed + at / 8) >> (8 * (at % 8)));

        return made;
    }

    static Bytes stamped(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& body) {
        sized(key, nonce);

        Bytes made;

        for (const std::uint64_t start : {0x84222325cbf29ce4ULL, 0x1b3100000001ULL}) {
            std::uint64_t into = start;

            poured(into, key);
            poured(into, nonce);
            poured(into, extra);
            poured(into, body);

            for (std::size_t at = 0; at < 8; ++at) made.push_back(static_cast<std::byte>(into >> (8 * at)));
        }

        return made;
    }
};

Seam<CipherProvider> ciphers() { return Seam<CipherProvider>{std::make_shared<Ciphers>()}; }

// Atrapa z ustalonym sekretem efemerycznym — żeby próba mogła policzyć wiadomości i klucze sama,
// z opisu drogi, i porównać bajt w bajt.
class Fixed : public Ciphers {
public:
    explicit Fixed(std::byte secret) : secret_(secret) {}

    [[nodiscard]] std::unique_ptr<Agreement> agreement() override { return std::make_unique<Agreeing>(Bytes{secret_}); }

private:
    std::byte secret_;
};

Seam<CipherProvider> fixed(int secret) {
    return Seam<CipherProvider>{std::make_shared<Fixed>(static_cast<std::byte>(secret))};
}

// Atrapa uzgodnienia: dzwoniący pyta „talcite?", odbierający odpowiada „talcite!", klucze stałe.
// Cokolwiek innego to odmowa — tak sprawdzamy, że nieudane uzgodnienie zamyka kanał.
const Bytes calling_key(32, std::byte{'c'});
const Bytes answering_key(32, std::byte{'a'});

class Greeting : public net::Handshake {
public:
    explicit Greeting(net::Side side) : side_(side) {
        keys_ = side == net::Side::calling ? net::Keying{calling_key, answering_key}
                                           : net::Keying{answering_key, calling_key};
    }

    std::optional<Bytes> said() override {
        if (side_ == net::Side::calling && step_ == 0) {
            step_ = 1;

            return bytes("talcite?");
        }

        if (side_ == net::Side::answering && step_ == 1) {
            step_ = 2;
            ready_ = true;

            return bytes("talcite!");
        }

        return std::nullopt;
    }

    void heard(const Bytes& one) override {
        if (side_ == net::Side::answering && step_ == 0 && one == bytes("talcite?")) {
            step_ = 1;

            return;
        }

        if (side_ == net::Side::calling && step_ == 1 && one == bytes("talcite!")) {
            step_ = 2;
            ready_ = true;

            return;
        }

        throw Trouble{"fake handshake heard something else"};
    }

    [[nodiscard]] bool ready() const override { return ready_; }

    [[nodiscard]] const net::Keying& keys() const override { return keys_; }

private:
    net::Side side_;
    net::Keying keys_;
    int step_ = 0;
    bool ready_ = false;
};

// Uzgodnienie, które nigdy się nie kończy: co turę mówi i słucha — kanał ma je przerwać sam.
class Endless : public net::Handshake {
public:
    std::optional<Bytes> said() override {
        if (spoke_) return std::nullopt;

        spoke_ = true;

        return bytes("jeszcze");
    }

    void heard(const Bytes&) override { spoke_ = false; }

    [[nodiscard]] bool ready() const override { return false; }

    [[nodiscard]] const net::Keying& keys() const override { throw Trouble{"never ready"}; }

private:
    bool spoke_ = false;
};

struct Pair {
    net::Stream answering;
    net::Stream calling;
};

Pair joined(const net::Listener& gate) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream answering = meeting.await();
    net::Stream calling = joining.await();

    return Pair{std::move(answering), std::move(calling)};
}

net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

struct Ends {
    net::Channel answering;
    net::Channel calling;
};

Ends greeted(const net::Listener& gate) {
    Pair pair = joined(gate);

    auto answering = net::Channel::opened(std::move(pair.answering), std::make_shared<Greeting>(net::Side::answering),
                                          ciphers(), soon);
    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                        ciphers(), soon);

    return Ends{answering.await(), calling.await()};
}

Bytes header(std::size_t much) {
    Bytes made(4);

    for (std::size_t at = 0; at < 4; ++at) made[at] = static_cast<std::byte>((much >> (24 - 8 * at)) & 0xff);

    return made;
}

Bytes frame(const Bytes& what) {
    Bytes made = header(what.size());

    made.insert(made.end(), what.begin(), what.end());

    return made;
}

Bytes counted(std::uint64_t counter) {
    Bytes made(12);

    for (std::size_t at = 0; at < 8; ++at) made[4 + at] = static_cast<std::byte>((counter >> (56 - 8 * at)) & 0xff);

    return made;
}

// Ramka danych tak, jak ją pieczętuje kanał — tak udaje drugą stronę surowy strumień.
Bytes sealed_frame(const Bytes& key, std::uint64_t counter, const Bytes& plain) {
    Ciphers one;

    const Bytes head = header(plain.size() + 16);
    Bytes made = head;
    const Bytes body = one.sealed(key, counted(counter), head, plain);

    made.insert(made.end(), body.begin(), body.end());

    return made;
}

// Surowy strumień odbiera wszystko, aż do końca — tyle, ile mu wysłano przed zamknięciem.
Bytes drained(const net::Stream& one) {
    Bytes made;

    for (;;) {
        const Bytes came = one.read(65536, soon).await();

        if (came.empty()) return made;

        made.insert(made.end(), came.begin(), came.end());
    }
}

// Surowy odbierający: przyjmuje „talcite?", odpowiada „talcite!" — dalej rozmawia ramkami.
struct Raw {
    net::Stream raw;
    net::Channel calling;
};

Raw half(const net::Listener& gate) {
    Pair pair = joined(gate);

    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                        ciphers(), soon);

    Bytes heard;

    while (heard.size() < 12) {
        const Bytes came = pair.answering.read(12 - heard.size(), soon).await();

        heard.insert(heard.end(), came.begin(), came.end());
    }

    pair.answering.write(frame(bytes("talcite!")), soon).await();

    return Raw{std::move(pair.answering), calling.await()};
}

bool found(const Bytes& in, const Bytes& what) {
    for (std::size_t at = 0; at + what.size() <= in.size(); ++at)
        if (Bytes(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + what.size())) ==
            what)
            return true;

    return false;
}

bool missing() {
    if (summing()) return false;

    std::cout << "        ten system nie daje skrótów — próba pominięta\n";

    return true;
}

Bytes long_key() { return Bytes(32, std::byte{7}); }

// Uzgodnienie w pamięci: wiadomości z jednej strony prosto do drugiej, bez sieci.
void talked(net::Handshake& calling, net::Handshake& answering) {
    for (int turn = 0; turn < 8 && !(calling.ready() && answering.ready()); ++turn) {
        while (const std::optional<Bytes> one = calling.said()) answering.heard(*one);
        while (const std::optional<Bytes> one = answering.said()) calling.heard(*one);
    }
}

}

TRIAL("Kanał: wiadomości idą w obie strony, po kolei i bez zmian") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    const Bytes big = random(1 << 20);

    ends.calling.send(bytes("pierwsza"), soon).await();
    ends.calling.send(Bytes{}, soon).await();
    ends.calling.send(big, soon).await();
    ends.answering.send(bytes("odpowiedź"), soon).await();

    ought(ends.answering.receive(soon).await() == bytes("pierwsza"));
    ought(ends.answering.receive(soon).await().empty());
    ought(ends.answering.receive(soon).await() == big);
    ought(ends.calling.receive(soon).await() == bytes("odpowiedź"));
}

TRIAL("Kanał: wiele wysłań naraz dochodzi w kolejności") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    std::vector<Promise<void>> sending;

    for (int at = 0; at < 50; ++at) sending.push_back(ends.calling.send(bytes(std::to_string(at)), soon));

    for (int at = 0; at < 50; ++at) ought(ends.answering.receive(soon).await() == bytes(std::to_string(at)));

    for (auto& one : sending) one.await();
}

TRIAL("Kanał: na drucie nie ma treści jawnej") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    const Bytes secret = bytes("TAJNE TAJNE TAJNE");

    raw.calling.send(secret, soon).await();
    raw.calling.close();

    const Bytes wire = drained(raw.raw);

    same(wire.size(), static_cast<std::size_t>(4 + 17 + 16));
    ought(!found(wire, secret));
    ought(wire == sealed_frame(calling_key, 0, secret));
}

TRIAL("Kanał: ramka od drugiej strony otwiera się kluczem odbierania") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    raw.raw.write(sealed_frame(answering_key, 0, bytes("raz")), soon).await();
    raw.raw.write(sealed_frame(answering_key, 1, bytes("dwa")), soon).await();

    ought(raw.calling.receive(soon).await() == bytes("raz"));
    ought(raw.calling.receive(soon).await() == bytes("dwa"));
}

TRIAL("Kanał: podrobiona ramka to odmowa i zamknięcie") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    Bytes forged = sealed_frame(answering_key, 0, bytes("prawda"));

    forged[6] ^= std::byte{1};

    raw.raw.write(forged, soon).await();

    threw(raw.calling.receive(soon).await());

    ought(drained(raw.raw).empty());
}

TRIAL("Kanał: powtórzona albo przestawiona ramka to odmowa") {
    {
        const net::Listener gate = standing();
        Raw raw = half(gate);

        const Bytes once = sealed_frame(answering_key, 0, bytes("raz"));

        raw.raw.write(once, soon).await();
        raw.raw.write(once, soon).await();

        ought(raw.calling.receive(soon).await() == bytes("raz"));
        threw(raw.calling.receive(soon).await());
    }
    {
        const net::Listener gate = standing();
        Raw raw = half(gate);

        raw.raw.write(sealed_frame(answering_key, 1, bytes("dwa")), soon).await();

        threw(raw.calling.receive(soon).await());
    }
}

TRIAL("Kanał: ramka zapieczętowana kluczem tej samej strony to odmowa") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    raw.raw.write(sealed_frame(calling_key, 0, bytes("odbite")), soon).await();

    threw(raw.calling.receive(soon).await());
}

TRIAL("Kanał: zapowiedź za długiej ramki to odmowa") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    raw.raw.write(header((4u << 20) + 17), soon).await();

    threw(raw.calling.receive(soon).await());

    ought(drained(raw.raw).empty());
}

TRIAL("Kanał: najdłuższa wiadomość przechodzi, dłuższa odmawia") {
    const net::Listener gate = standing();
    const Ends ends = greeted(gate);

    const Bytes most = random(4u << 20);

    auto sending = ends.calling.send(most, net::Patience{20000});

    ought(ends.answering.receive(net::Patience{20000}).await() == most);

    sending.await();

    threw(ends.calling.send(Bytes((4u << 20) + 1), soon).await());
}

TRIAL("Kanał: koniec strumienia w środku ramki to kłopot") {
    const net::Listener gate = standing();
    Raw raw = half(gate);

    const Bytes whole = sealed_frame(answering_key, 0, bytes("urwana wiadomość"));

    raw.raw.write(Bytes(whole.begin(), whole.begin() + 10), soon).await();
    raw.raw.close();

    threw(raw.calling.receive(soon).await());
}

// Odmowa od razu, nie po upływie cierpliwości — druga strona milczy, więc tylko sprawdzenie na
// wejściu może skończyć sprawę szybko.
TRIAL("Kanał: bez uzgodnienia albo szyfru odmawia od razu") {
    const net::Listener gate = standing();
    const net::Patience long_enough{60000};
    const auto started = std::chrono::steady_clock::now();

    {
        Pair pair = joined(gate);

        threw(net::Channel::opened(std::move(pair.calling), nullptr, ciphers(), long_enough).await());
    }
    {
        Pair pair = joined(gate);

        threw(net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                   Seam<CipherProvider>{}, long_enough)
                  .await());
    }

    ought(std::chrono::steady_clock::now() - started < std::chrono::seconds{20});
}

TRIAL("Kanał: nieudane uzgodnienie zamyka strumień") {
    const net::Listener gate = standing();
    Pair pair = joined(gate);

    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Greeting>(net::Side::calling),
                                        ciphers(), soon);

    Bytes heard;

    while (heard.size() < 12) {
        const Bytes came = pair.answering.read(12 - heard.size(), soon).await();

        heard.insert(heard.end(), came.begin(), came.end());
    }

    ought(heard == frame(bytes("talcite?")));

    pair.answering.write(frame(bytes("obcy!")), soon).await();

    threw(calling.await());

    ought(drained(pair.answering).empty());
}

TRIAL("Kanał: uzgodnienie bez końca przerywa się samo") {
    const net::Listener gate = standing();
    Pair pair = joined(gate);

    // Cierpliwość długa — przerwać ma limit tur, a nie upływ czasu.
    const net::Patience long_enough{60000};
    const auto started = std::chrono::steady_clock::now();

    auto answering =
        net::Channel::opened(std::move(pair.answering), std::make_shared<Endless>(), ciphers(), long_enough);
    auto calling = net::Channel::opened(std::move(pair.calling), std::make_shared<Endless>(), ciphers(), long_enough);

    threw(answering.await());
    threw(calling.await());

    ought(std::chrono::steady_clock::now() - started < std::chrono::seconds{20});
}

TRIAL("Kanał: zamknięty odmawia i nie ma adresu") {
    const net::Listener gate = standing();
    Ends ends = greeted(gate);

    ought(ends.calling.held());
    ought(ends.calling.peer().port() == gate.mine().port());

    ends.calling.close();

    ought(!ends.calling.held());
    same(ends.calling.peer().port(), static_cast<std::uint16_t>(0));
    threw(ends.calling.send(bytes("po"), soon).await());
    threw(ends.calling.receive(soon).await());
    threw(ends.answering.receive(soon).await());
}

TRIAL("Uzgodnienie z długim kluczem: obie strony mają te same klucze, na krzyż") {
    if (missing()) return;

    const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
    const auto answering = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());

    talked(*calling, *answering);

    ought(calling->ready());
    ought(answering->ready());
    same(calling->keys().sending.size(), static_cast<std::size_t>(32));
    ought(calling->keys().sending == answering->keys().receiving);
    ought(calling->keys().receiving == answering->keys().sending);
    ought(!(calling->keys().sending == calling->keys().receiving));
    ought(!(calling->keys().sending == long_key()));
}

// Wiadomości i klucze policzone tu wprost z opisu drogi: HKDF-SHA-512 z solą — długim kluczem,
// sekretem — wynikiem uzgodnienia, treścią — etykietą, kontekstem i obiema wiadomościami e, każde
// z długością przed sobą; potwierdzenia to HMAC-SHA-256 etykiety strony. Zmiana któregokolwiek
// szczegółu to inny protokół — dwie wersje twa przestałyby się dogadywać, więc próba ma to widzieć.
TRIAL("Uzgodnienie z długim kluczem: bajty zgodne z opisem drogi") {
    if (missing()) return;

    const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", fixed(5));
    const auto answering = net::keyed(net::Side::answering, long_key(), "twa 1", fixed(9));

    const Agreeing mine{Bytes{std::byte{5}}};
    const Agreeing theirs{Bytes{std::byte{9}}};

    const Bytes e_c = mine.mine();
    const Bytes e_r = theirs.mine();

    Bytes info = bytes("talcite keyed 1");

    for (const Bytes& one : {bytes("twa 1"), e_c, e_r}) {
        const Bytes head = header(one.size());

        info.insert(info.end(), head.begin(), head.end());
        info.insert(info.end(), one.begin(), one.end());
    }

    const Bytes made = derived(Sum::sha512, long_key(), *mine.shared(e_r), info, 128);

    const auto part = [&made](std::size_t which) {
        return Bytes(made.begin() + static_cast<std::ptrdiff_t>(32 * which),
                     made.begin() + static_cast<std::ptrdiff_t>(32 * which + 32));
    };

    Bytes second = e_r;
    const Bytes confirm_answering = stamped(Sum::sha256, part(2), bytes("answering"));

    second.insert(second.end(), confirm_answering.begin(), confirm_answering.end());

    const Bytes first = *calling->said();

    ought(first == e_c);

    answering->heard(first);

    ought(*answering->said() == second);

    calling->heard(second);

    ought(*calling->said() == stamped(Sum::sha256, part(3), bytes("calling")));
    ought(calling->keys().sending == part(0));
    ought(calling->keys().receiving == part(1));
}

TRIAL("Uzgodnienie z długim kluczem: trzy wiadomości i nic więcej") {
    if (missing()) return;

    const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
    const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

    ought(!answering->said());

    const std::optional<Bytes> first = calling->said();

    ought(first.has_value());
    same(first->size(), static_cast<std::size_t>(32));
    ought(!calling->said());

    answering->heard(*first);

    const std::optional<Bytes> second = answering->said();

    ought(second.has_value());
    same(second->size(), static_cast<std::size_t>(64));
    ought(!answering->ready());

    calling->heard(*second);

    ought(!calling->ready());

    const std::optional<Bytes> third = calling->said();

    ought(third.has_value());
    same(third->size(), static_cast<std::size_t>(32));
    ought(calling->ready());
    ought(!calling->said());

    answering->heard(*third);

    ought(answering->ready());
    ought(!answering->said());
}

TRIAL("Uzgodnienie z długim kluczem: za każdym razem klucze świeże") {
    if (missing()) return;

    const auto one = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
    const auto two = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());
    const auto three = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
    const auto four = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());

    talked(*one, *two);
    talked(*three, *four);

    ought(one->ready() && three->ready());
    ought(!(one->keys().sending == three->keys().sending));
}

TRIAL("Uzgodnienie z długim kluczem: inny klucz albo kontekst to odmowa") {
    if (missing()) return;

    Bytes other = long_key();

    other[31] ^= std::byte{1};

    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
        const auto answering = net::keyed(net::Side::answering, other, "twa 1", ciphers());

        answering->heard(*calling->said());

        threw(calling->heard(*answering->said()));
        ought(!calling->ready());
        threw((void)calling->keys());
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "twa 2", ciphers());

        answering->heard(*calling->said());

        threw(calling->heard(*answering->said()));
    }
}

TRIAL("Uzgodnienie z długim kluczem: zmieniony bajt w każdej wiadomości to odmowa") {
    if (missing()) return;

    for (int which = 0; which < 3; ++which) {
        const auto calling = net::keyed(net::Side::calling, long_key(), "twa 1", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "twa 1", ciphers());

        bool refused = false;

        try {
            Bytes first = *calling->said();

            if (which == 0) first[0] ^= std::byte{1};

            answering->heard(first);

            Bytes second = *answering->said();

            if (which == 1) second[40] ^= std::byte{1};

            calling->heard(second);

            Bytes third = *calling->said();

            if (which == 2) third[5] ^= std::byte{1};

            answering->heard(third);
        } catch (const Trouble&) {
            refused = true;
        }

        ought(refused);
        ought(!answering->ready());
    }
}

TRIAL("Uzgodnienie z długim kluczem: krótki klucz, zła długość, zły punkt i zła kolej to odmowa") {
    if (missing()) return;

    threw((void)net::keyed(net::Side::calling, Bytes(31, std::byte{7}), "", ciphers()));
    threw((void)net::keyed(net::Side::calling, long_key(), "", Seam<CipherProvider>{}));

    {
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        threw(answering->heard(Bytes(31, std::byte{9})));
    }
    {
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        threw(answering->heard(Bytes(32)));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());

        threw(calling->heard(Bytes(64)));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        answering->heard(*calling->said());

        const Bytes second = *answering->said();

        threw(calling->heard(Bytes(second.begin(), second.begin() + 63)));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        answering->heard(*calling->said());

        Bytes second = *answering->said();

        second.push_back(std::byte{0});

        threw(calling->heard(second));
    }
    {
        const auto calling = net::keyed(net::Side::calling, long_key(), "", ciphers());
        const auto answering = net::keyed(net::Side::answering, long_key(), "", ciphers());

        answering->heard(*calling->said());
        calling->heard(*answering->said());

        Bytes third = *calling->said();

        third.push_back(std::byte{0});

        threw(answering->heard(third));
        ought(!answering->ready());
    }
}

TRIAL("Kanał z długim kluczem: przez sieć rozmawia, z innym kluczem nie rusza") {
    if (missing()) return;

    const net::Listener gate = standing();

    {
        Pair pair = joined(gate);

        auto answering = net::Channel::opened(std::move(pair.answering),
                                              net::keyed(net::Side::answering, long_key(), "twa 1", ciphers()),
                                              ciphers(), soon);
        auto calling = net::Channel::opened(std::move(pair.calling),
                                            net::keyed(net::Side::calling, long_key(), "twa 1", ciphers()), ciphers(),
                                            soon);

        const net::Channel one = answering.await();
        const net::Channel two = calling.await();

        two.send(bytes("dzień dobry"), soon).await();
        one.send(bytes("dobry wieczór"), soon).await();

        ought(one.receive(soon).await() == bytes("dzień dobry"));
        ought(two.receive(soon).await() == bytes("dobry wieczór"));
    }
    {
        Pair pair = joined(gate);

        Bytes other = long_key();

        other[0] ^= std::byte{1};

        auto answering = net::Channel::opened(std::move(pair.answering),
                                              net::keyed(net::Side::answering, other, "twa 1", ciphers()), ciphers(),
                                              soon);
        auto calling = net::Channel::opened(std::move(pair.calling),
                                            net::keyed(net::Side::calling, long_key(), "twa 1", ciphers()), ciphers(),
                                            soon);

        threw(calling.await());
        threw(answering.await());
    }
}

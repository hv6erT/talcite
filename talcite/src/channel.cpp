#include "talcite/channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

#include "talcite/trouble.hpp"

namespace talcite::net {

// **Kanał szyfrowany na strumieniu** (S1 w twa): najpierw uzgodnienie klucza (dowolna droga szwu
// `Handshake`), potem wiadomości zapieczętowane AES-256-GCM z szwu szyfrów. Każda wiadomość to
// ramka: cztery bajty długości od najstarszego, potem treść. Ramki uzgodnienia idą jawnie — chroni je
// samo uzgodnienie; ramki danych są zapieczętowane, a nagłówek długości jest treścią dodatkową AEAD,
// więc podmiana długości to odmowa. Nonce to licznik wiadomości w danym kierunku (cztery bajty zer
// i osiem licznika), więc powtórzenie, przestawienie i zgubienie wiadomości łamie pieczęć następnej.
// Kanał mówi wiadomościami, nie bajtami — po drugiej stronie przychodzi dokładnie to, co wysłano.
//
// Zapisy idą po kolei przez ogon obietnic (jak w `TlsStream`), odczyt bierze z zapasu to, co
// przyszło ponad bieżącą ramkę. Strumień czyta się i zamyka pod `minding`, a praca asynchroniczna
// biegnie na kopii — ta sama reguła i ten sam powód co w `TlsStream` (`close()` przestawia
// `shared_ptr` w składowej). Podrobiona ramka albo nieudane uzgodnienie zamykają strumień: liczniki
// już się rozjechały, a druga strona ma się dowiedzieć od razu, a nie po upływie cierpliwości.
// Kanał ma jednego czytelnika naraz — dwa `receive` w locie mogłyby wziąć liczniki nie po kolei.
namespace {

// Najdłuższa ramka: wiadomość cztery megabajty plus znacznik. Dłuższa zapowiedź to zerwanie — obcy
// nie ma prawa kazać nam rezerwować gigabajtów.
constexpr std::uint32_t biggest = (4u << 20) + 16;

using Clock = std::chrono::steady_clock;

Patience left(Clock::time_point until) {
    const auto rest = std::chrono::duration_cast<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

Promise<void> already() {
    return Promise<void>{[](auto resolve, auto) { resolve(); }};
}

template <typename T>
Promise<T> refused(const char* said) {
    return Promise<T>{[said](auto, auto reject) { reject(std::make_exception_ptr(Trouble{said})); }};
}

const Address& nowhere() {
    static const Address one;

    return one;
}

Bytes headed(std::size_t much) {
    Bytes made(4);

    for (int at = 0; at < 4; ++at) made[static_cast<std::size_t>(at)] = static_cast<std::byte>((much >> (24 - 8 * at)) & 0xff);

    return made;
}

Bytes nonce(std::uint64_t counter) {
    Bytes made(12);

    for (int at = 0; at < 8; ++at) made[static_cast<std::size_t>(4 + at)] = static_cast<std::byte>((counter >> (56 - 8 * at)) & 0xff);

    return made;
}

}

struct Channel::Held {
    Stream one;
    Seam<CipherProvider> cipher;
    Keying keys;

    std::mutex minding;
    Bytes spare;
    std::uint64_t sent = 0;
    std::uint64_t taken = 0;

    std::mutex sending;
    Promise<void> tail{[](auto resolve, auto) { resolve(); }};
    int queued = 0;
};

namespace {

Stream borrowed(const std::shared_ptr<Channel::Held>& held) {
    const std::lock_guard<std::mutex> kept{held->minding};

    return held->one;
}

void shut(Channel::Held& held) {
    const std::lock_guard<std::mutex> kept{held.minding};

    held.one.close();
}

// Zapis po kolei: każdy następny czeka na poprzedni, a ogon wraca do spełnionego, gdy kolejka pusta.
Promise<void> posting(const std::shared_ptr<Channel::Held>& held, Bytes what, Patience patience) {
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

// Cała ramka z zapasu, jeśli już jest — inaczej nic.
std::optional<Bytes> whole(Channel::Held& held) {
    const std::lock_guard<std::mutex> kept{held.minding};

    if (held.spare.size() < 4) return std::nullopt;

    std::uint32_t much = 0;

    for (int at = 0; at < 4; ++at) much = (much << 8) | static_cast<std::uint32_t>(held.spare[static_cast<std::size_t>(at)]);

    if (much > biggest) {
        held.one.close();

        throw Trouble{"The other side announced a frame that is too long."};
    }

    if (held.spare.size() < 4 + static_cast<std::size_t>(much)) return std::nullopt;

    Bytes made(held.spare.begin() + 4, held.spare.begin() + 4 + static_cast<std::ptrdiff_t>(much));

    held.spare.erase(held.spare.begin(), held.spare.begin() + 4 + static_cast<std::ptrdiff_t>(much));

    return made;
}

// Następna ramka: z zapasu albo po doczytaniu. Koniec strumienia w środku ramki to zerwanie.
Promise<Bytes> framed(const std::shared_ptr<Channel::Held>& held, Clock::time_point until) {
    if (std::optional<Bytes> ready = whole(*held))
        return Promise<Bytes>{[made = std::move(*ready)](auto resolve, auto) mutable { resolve(std::move(made)); }};

    return borrowed(held).read(65536, left(until)).then([held, until](const Bytes& came) {
        if (came.empty()) throw Trouble{"The other side closed the channel."};

        {
            const std::lock_guard<std::mutex> kept{held->minding};

            held->spare.insert(held->spare.end(), came.begin(), came.end());
        }

        return framed(held, until);
    });
}

Bytes frame(const Bytes& what) {
    Bytes made = headed(what.size());

    made.insert(made.end(), what.begin(), what.end());

    return made;
}

// Uzgodnienie: póki ma co powiedzieć — mówimy; gotowe — koniec; inaczej czekamy na ramkę.
Promise<void> shaking(const std::shared_ptr<Channel::Held>& held, const std::shared_ptr<Handshake>& how,
                      Clock::time_point until, int turn) {
    Bytes out;

    while (std::optional<Bytes> one = how->said()) {
        const Bytes made = frame(*one);

        out.insert(out.end(), made.begin(), made.end());
    }

    Promise<void> sent = out.empty() ? already() : posting(held, std::move(out), left(until));

    if (how->ready()) return sent;

    if (turn >= 16) throw Trouble{"The handshake will not finish."};

    return sent.then([held, how, until, turn] {
        return framed(held, until).then([held, how, until, turn](const Bytes& came) {
            how->heard(came);

            return shaking(held, how, until, turn + 1);
        });
    });
}

}

Promise<Channel> Channel::opened(Stream one, std::shared_ptr<Handshake> how, Seam<CipherProvider> cipher,
                                 Patience patience) {
    if (!how || !cipher.bound()) return refused<Channel>("A channel needs a handshake and a cipher.");

    auto held = std::make_shared<Held>();

    held->one = std::move(one);
    held->cipher = std::move(cipher);

    Promise<Channel> made = shaking(held, how, Clock::now() + patience, 0)
                                .then([held, how] {
                                    held->keys = how->keys();

                                    return Channel{held};
                                })
                                .fail([held](const std::exception&) { shut(*held); });

    made.start();

    return made;
}

const Address& Channel::peer() const {
    if (!held_) return nowhere();

    const std::lock_guard<std::mutex> kept{held_->minding};

    return held_->one.peer();
}

// Pieczęć liczy się od razu, pod zamkiem — licznik idzie w tej samej kolejności, w której ramki
// wchodzą do kolejki zapisu.
Promise<void> Channel::send(Bytes what, Patience patience) const {
    if (!held_) return refused<void>("The channel is closed.");

    if (what.size() + 16 > biggest) return refused<void>("The message is too long for the channel.");

    Bytes made;

    {
        const std::lock_guard<std::mutex> kept{held_->sending};

        const Bytes head = headed(what.size() + 16);

        made = head;

        const Bytes sealed = held_->cipher.provider().sealed(held_->keys.sending, nonce(held_->sent++), head, what);

        made.insert(made.end(), sealed.begin(), sealed.end());
    }

    return posting(held_, std::move(made), patience);
}

Promise<Bytes> Channel::receive(Patience patience) const {
    if (!held_) return refused<Bytes>("The channel is closed.");

    const std::shared_ptr<Held> held = held_;

    return framed(held, Clock::now() + patience).then([held](const Bytes& came) {
        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> kept{held->minding};

            which = held->taken++;
        }

        const std::optional<Bytes> plain =
            held->cipher.provider().opened(held->keys.receiving, nonce(which), headed(came.size()), came);

        if (!plain) {
            shut(*held);

            throw Trouble{"A message in the channel was forged or lost."};
        }

        return *plain;
    });
}

void Channel::close() {
    if (!held_) return;

    shut(*held_);

    held_.reset();
}

}

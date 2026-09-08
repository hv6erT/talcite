#include "inside.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "talcite/trouble.hpp"

#include "starting.hpp"

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>

#include <afunix.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace talcite::net {

// Ile bajtów wolno podać jednym wywołaniem tam, gdzie system bierze długość jako int.
constexpr int brimful = 1 << 30;


namespace {

// Adres talcite ma szesnaście gołych bajtów, a system chce swojej struktury. Zamiana jest w jednym
// miejscu, żeby nikt jej nie przepisywał po swojemu — a przy okazji tylko ten plik wie, jak
// wygląda sockaddr.
int filled(const Address& from, sockaddr_storage& into) {
    std::memset(&into, 0, sizeof into);

    if (from.family() == Family::four) {
        auto* four = reinterpret_cast<sockaddr_in*>(&into);

        four->sin_family = AF_INET;
        four->sin_port = htons(from.port());

        std::memcpy(&four->sin_addr, from.bytes().data(), 4);

        return static_cast<int>(sizeof(sockaddr_in));
    }

    // Gniazdo lokalne nie ma ani adresu, ani portu — ma ścieżkę, a długość struktury liczy się
    // z jej długości, nie z rozmiaru pola. Ścieżka mieści się razem z zerem, bo `Address::local`
    // pilnuje tego już przy budowaniu adresu i nie ma tu czego sprawdzać drugi raz.
    if (from.family() == Family::local) {
        auto* nearby = reinterpret_cast<sockaddr_un*>(&into);

        nearby->sun_family = AF_UNIX;

        const std::string& said = from.path();

        std::memcpy(nearby->sun_path, said.data(), said.size());

        return static_cast<int>(offsetof(sockaddr_un, sun_path) + said.size() + 1);
    }

    // Pusty adres **nie jest szóstką**. Bez tego domyślnie skonstruowany Address budował poprawny
    // sockaddr_in6 dla [::]:0 i wiązanie na nim się udawało, zamiast odmówić.
    if (from.family() != Family::six) return 0;

    auto* six = reinterpret_cast<sockaddr_in6*>(&into);

    six->sin6_family = AF_INET6;
    six->sin6_port = htons(from.port());
    six->sin6_scope_id = from.scope();

    std::memcpy(&six->sin6_addr, from.bytes().data(), 16);

    return static_cast<int>(sizeof(sockaddr_in6));
}

Address seen(const sockaddr_storage& from) {
    std::array<std::byte, 16> raw{};

    if (from.ss_family == AF_INET) {
        const auto* four = reinterpret_cast<const sockaddr_in*>(&from);

        std::memcpy(raw.data(), &four->sin_addr, 4);

        return Address{Family::four, raw, ntohs(four->sin_port)};
    }

    // Nasz koniec połączenia lokalnego **nie ma nazwy** — `getsockname` oddaje wtedy puste
    // `sun_path` i pusty adres jest tu prawdą, a nie awarią.
    if (from.ss_family == AF_UNIX) {
        const auto* nearby = reinterpret_cast<const sockaddr_un*>(&from);

        std::size_t much = 0;

        while (much < sizeof nearby->sun_path && nearby->sun_path[much] != '\0') ++much;

        if (much == 0) return {};

        // Linux dopuszcza ścieżkę o kilka bajtów dłuższą, niż nasz adres umie unieść. Cudzego
        // gniazda o takiej ścieżce nie mamy jak wyrazić, więc oddajemy pustkę zamiast rzucać
        // z pomocnika, który stoi na drodze każdego zakończenia.
        try {
            return Address::local(std::string{nearby->sun_path, much});
        } catch (const Trouble&) {
            return {};
        }
    }

    if (from.ss_family == AF_INET6) {
        const auto* six = reinterpret_cast<const sockaddr_in6*>(&from);

        std::memcpy(raw.data(), &six->sin6_addr, 16);

        Address made{Family::six, raw, ntohs(six->sin6_port)};

        made.scope(six->sin6_scope_id);

        return made;
    }

    return {};
}

}

// Te dwie wystawiamy, bo zaplecze windowsowe składa i rozbiera adresy u siebie — a nie chcemy
// drugiej kopii tej wiedzy.
int filling(const Address& from, void* into) {
    return filled(from, *static_cast<sockaddr_storage*>(into));
}

Address facing(const void* from) { return seen(*static_cast<const sockaddr_storage*>(from)); }

// Która strona gniazda interesuje daną operację. Nawiązanie kończy się gotowością **do pisania**
// — to jedyna rzecz w tej tablicy, która nie jest oczywista.
bool outward(Kind kind) {
    return kind == Kind::joining || kind == Kind::putting || kind == Kind::sending;
}

// Krok operacji. To jedyne miejsce, które wie, jak wygląda praca na tym systemie — zgłoszenie
// dostaje z niej wyłącznie liczbę bajtów i numer błędu. Zwraca prawdę, gdy operacja się
// skończyła; fałsz znaczy „system jeszcze nie mógł, czekaj dalej".
bool stepped(Doing& one) {
    switch (one.kind) {
        case Kind::joining: one.done(0, settling(one.held)); return true;

        case Kind::meeting: {
            Handle made = nothing;

            const Address from = taken(one.held, made);

            if (made == nothing) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.made = made;
            one.where = from;

            one.done(0, 0);

            return true;
        }

        case Kind::getting: {
            const long much = getting(one.held, one.buffer.data(), one.buffer.size());

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }

        // Pisanie kończy się dopiero wtedy, gdy zeszło wszystko — a między jednym a drugim
        // obrotem pamięta, ile już poszło.
        case Kind::putting: {
            while (one.much < one.buffer.size()) {
                const long much =
                    putting(one.held, one.buffer.data() + one.much, one.buffer.size() - one.much);

                if (much < 0) {
                    if (again()) return false;

                    one.done(one.much, amiss());

                    return true;
                }

                one.much += static_cast<std::size_t>(much);
            }

            one.done(one.much, 0);

            return true;
        }

        case Kind::fetching: {
            bool cut = false;

            const long much =
                fetching(one.held, one.buffer.data(), one.buffer.size(), one.where, cut);

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.cut = cut;

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }

        case Kind::sending: {
            const long much = sending(one.held, one.buffer.data(), one.buffer.size(), one.where);

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }
    }

    return true;
}

// Gniazda są od początku nieblokujące, bo centrala nigdy nie chce czekać w wywołaniu systemowym
// — od czekania jest jedno miejsce i to ono pilnuje wszystkich naraz.
Handle opened(Family family, bool stream) {
    starting();

    // **Gniazdo lokalne jest wyłącznie strumieniowe.** Windows nie ma dla niego datagramów wcale,
    // a rodzina, która na jednym systemie umie więcej niż na drugim, jest gorsza od rodziny, która
    // wszędzie umie tyle samo.
    if (family == Family::local && !stream) return nothing;

    const int which = family == Family::local  ? AF_UNIX
                      : family == Family::four ? AF_INET
                                               : AF_INET6;

#ifdef _WIN32
    const Handle made = ::socket(which, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

    if (made == nothing) return nothing;

    u_long yes = 1;

    ::ioctlsocket(static_cast<SOCKET>(made), FIONBIO, &yes);
#else
    // **Zamykane przy uruchomieniu programu potomnego.** Bez tego dziecko dziedziczy wszystkie
    // nasze gniazda i trzyma zajęty port długo po tym, jak my go zamkniemy — dokładnie ten sam
    // powód, dla którego gałąź windowsowa buduje listę uchwytów do przekazania.
    const Handle made = ::socket(which, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

    if (made < 0) return nothing;

    ::fcntl(made, F_SETFL, ::fcntl(made, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);

    // **Zapis w gniazdo, którego druga strona już zamknęła, nie ma prawa zabić naszego procesu.**
    // Domyślną reakcją na SIGPIPE jest śmierć — a biblioteka sieciowa, która ubija program
    // wołającego, dlatego że rozmówca się rozłączył, jest nie do użycia. Tam, gdzie jest
    // `MSG_NOSIGNAL`, wyłączamy to przy każdym wysłaniu; BSD i macOS znaku przy wysyłaniu nie
    // mają, więc dostają znacznik na gnieździe.
    //
    // Znacznika procesowego nie ruszamy: `SIG_IGN` na SIGPIPE byłoby cichą zmianą warunków całej
    // aplikacji, o którą nikt nas nie prosił — ta sama zasada, co przy rurach programu potomnego,
    // gdzie sygnał blokujemy tylko na czas pisania i tylko na swoim wątku.
#ifdef SO_NOSIGPIPE
    {
        int yes = 1;

        ::setsockopt(made, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    }
#endif
#endif

    // Nasłuchiwanie na `::` ma łapać także klientów po IPv4. Windows domyślnie tego nie robi,
    // więc ten sam kod na dwóch systemach słuchałby na dwóch różnych zbiorach adresów.
    if (family == Family::six) {
        int off = 0;

        ::setsockopt(static_cast<int>(made), IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&off), sizeof off);
    }

    return made;
}

void ending(Handle one) {
    if (one == nothing) return;

#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(one));
#else
    ::close(one);
#endif
}

bool joined(Handle one, const Address& where) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    const int made = ::connect(static_cast<int>(one), reinterpret_cast<sockaddr*>(&told), much);

    // Zero znaczy, że zdążyło od razu — na pętli zwrotnej to reguła, a nie wyjątek.
    return made == 0;
}

// Nieblokujące nawiązanie kończy się gotowością do pisania, a **czy się udało**, mówi dopiero
// SO_ERROR. Bez tego pytania nieudane połączenie wygląda jak udane.
int settling(Handle one) {
    int why = 0;

#ifdef _WIN32
    int much = sizeof why;
#else
    socklen_t much = sizeof why;
#endif

    if (::getsockopt(static_cast<int>(one), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&why), &much) != 0)
        return amiss();

    return why;
}

Address named(Handle one, bool peer) {
    sockaddr_storage told{};

#ifdef _WIN32
    int much = sizeof told;
#else
    socklen_t much = sizeof told;
#endif

    const int made = peer ? ::getpeername(static_cast<int>(one),
                                          reinterpret_cast<sockaddr*>(&told), &much)
                          : ::getsockname(static_cast<int>(one),
                                          reinterpret_cast<sockaddr*>(&told), &much);

    if (made != 0) return {};

    return seen(told);
}

void nagling(Handle one, bool given) {
    const int off = given ? 0 : 1;

    ::setsockopt(static_cast<int>(one), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&off), sizeof off);
}

// Bez tego serwer po zamknięciu nie wstaje przez minutę, bo port siedzi w TIME_WAIT. Na Windows
// SO_REUSEADDR znaczy co innego — tam pozwala **odebrać** cudzy port — więc go nie ustawiamy.
void sharing(Handle one) {
#ifndef _WIN32
    const int yes = 1;

    ::setsockopt(one, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#else
    (void)one;
#endif
}

namespace {

// Gniazdo lokalne **zostawia po sobie plik**, a przypisanie do istniejącego kończy się odmową —
// więc po programie, który padł, zostaje plik, przez który nie wstanie następny.
//
// Kasujemy go **wyłącznie wtedy, gdy nikt pod nim nie słucha**, a dowodem na to jest odmowa
// połączenia. Udane połączenie znaczy, że ktoś tam już stoi, i wtedy to my mamy ustąpić, a nie
// kasować cudze gniazdo. Każda inna odpowiedź systemu jest niejednoznaczna, więc przy niej też
// nie ruszamy pliku: przypisanie odmówi za chwilę samo i powie o tym wołającemu.
void reaping(const Address& where) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    const Handle probe = opened(Family::local, true);

    if (probe == nothing) return;

    const int made = ::connect(static_cast<int>(probe), reinterpret_cast<sockaddr*>(&told), much);
    const int why = made == 0 ? 0 : amiss();

    ending(probe);

    if (made == 0) return;

#ifdef _WIN32
    const bool dead = why == WSAECONNREFUSED;
#else
    const bool dead = why == ECONNREFUSED;
#endif

    if (dead) (void)std::remove(where.path().c_str());
}

}

bool listening(Handle one, const Address& where, int waiting) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    if (where.family() == Family::local) reaping(where);

    if (::bind(static_cast<int>(one), reinterpret_cast<sockaddr*>(&told), much) != 0)
        return false;

    // **Zawężenie, nie zapora.** Plik gniazda powstaje z prawami wziętymi z maski procesu, czyli
    // zwykle dostępny dla wszystkich na maszynie — a gniazdo, do którego może się dobić każdy
    // użytkownik, jest dziurą. Zdejmujemy więc prawa obcym zaraz po przypisaniu.
    //
    // Szpary między przypisaniem a tą zmianą nie da się domknąć od środka i nie udajemy, że się
    // da: prawdziwą osłoną jest katalog, w którym gniazdo stoi. Kto trzyma je w katalogu dostępnym
    // tylko dla siebie, ma to zamknięte od pierwszej chwili.
#ifndef _WIN32
    if (where.family() == Family::local) (void)::chmod(where.path().c_str(), S_IRUSR | S_IWUSR);
#endif

    return ::listen(static_cast<int>(one), waiting) == 0;
}

bool bound(Handle one, const Address& where) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    return ::bind(static_cast<int>(one), reinterpret_cast<sockaddr*>(&told), much) == 0;
}

// Datagram większy niż bufor jest **po cichu obcinany** i to jest jedna z najbrzydszych pułapek
// UDP. Da się ją wykryć: Linux z MSG_TRUNC oddaje prawdziwą długość, a Windows odmawia całkiem
// z WSAEMSGSIZE. Obcięcie zgłaszamy jako kłopot, zamiast oddać kawałek udający całość.
long fetching(Handle one, void* into, std::size_t much, Address& from, bool& cut) {
    sockaddr_storage told{};

    cut = false;

#ifdef _WIN32
    int wide = sizeof told;

    const long made = static_cast<long>(::recvfrom(static_cast<SOCKET>(one),
                                                   static_cast<char*>(into),
                                                   static_cast<int>(much), 0,
                                                   reinterpret_cast<sockaddr*>(&told), &wide));

    if (made < 0 && amiss() == WSAEMSGSIZE) {
        cut = true;

        from = seen(told);

        return static_cast<long>(much);
    }
#else
    socklen_t wide = sizeof told;

    int how = 0;

#ifdef MSG_TRUNC
    how = MSG_TRUNC;
#endif

    long made = static_cast<long>(::recvfrom(one, into, much, how,
                                             reinterpret_cast<sockaddr*>(&told), &wide));

    if (made >= 0 && static_cast<std::size_t>(made) > much) {
        cut = true;
        made = static_cast<long>(much);
    }
#endif

    if (made >= 0) from = seen(told);

    return made;
}

// Datagram wychodzi w całości albo wcale — nie ma tu czego dopisywać w pętli, jak przy strumieniu.
long sending(Handle one, const void* from, std::size_t much, const Address& to) {
    sockaddr_storage told{};

    const int wide = filled(to, told);

#ifdef _WIN32
    const int room = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::sendto(static_cast<SOCKET>(one), static_cast<const char*>(from),
                                      room, 0, reinterpret_cast<sockaddr*>(&told), wide));
#else
    return static_cast<long>(
        ::sendto(one, from, much, 0, reinterpret_cast<sockaddr*>(&told), wide));
#endif
}

// Windows bierze długość jako int, uniksy jako size_t. Zwężanie robione wszędzie dawało na
// uniksach zero dla każdej długości będącej wielokrotnością cztero­gigabajtowej granicy — a zero
// przy pisaniu nie posuwało licznika, więc pętla kroku kręciła się w nieskończoność na pełnym
// rdzeniu, a przy czytaniu wyglądało jak koniec strumienia. Tam, gdzie zwężać trzeba, **ucinamy
// zamiast przekręcać**: krótki zapis i tak dokończy się następnym zgłoszeniem.
long getting(Handle one, void* into, std::size_t much) {
#ifdef _WIN32
    const int wide = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::recv(static_cast<SOCKET>(one), static_cast<char*>(into), wide, 0));
#else
    return static_cast<long>(::recv(one, into, much, 0));
#endif
}

long putting(Handle one, const void* from, std::size_t much) {
#ifdef _WIN32
    const int wide = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(
        ::send(static_cast<SOCKET>(one), static_cast<const char*>(from), wide, 0));
#else
    // Patrz `making`: bez tego znaku zapis w gniazdo po rozłączeniu zabija proces wołającego.
#ifdef MSG_NOSIGNAL
    return static_cast<long>(::send(one, from, much, MSG_NOSIGNAL));
#else
    return static_cast<long>(::send(one, from, much, 0));
#endif
#endif
}

// „Jeszcze nie teraz" nie jest błędem, tylko zaproszeniem do ponownego czekania. Na Windows ma
// inny numer niż na uniksach, więc pytanie stoi tu, a nie w pięciu miejscach.
bool again() {
#ifdef _WIN32
    return amiss() == WSAEWOULDBLOCK;
#else
    return amiss() == EAGAIN || amiss() == EWOULDBLOCK || amiss() == EINTR;
#endif
}

// Nieblokujące nawiązanie prawie nigdy nie kończy się od razu — mówi „zacząłem" i to nie jest
// błąd. Numer tej odpowiedzi jest inny na każdym systemie, więc pytanie stoi w jednym miejscu.
bool underway() {
#ifdef _WIN32
    return amiss() == WSAEWOULDBLOCK || amiss() == WSAEINPROGRESS;
#else
    return amiss() == EINPROGRESS || amiss() == EINTR;
#endif
}

void shutting(Handle one) {
    if (one == nothing) return;

#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(one), SD_BOTH);
#else
    ::shutdown(one, SHUT_RDWR);
#endif
}

Address taken(Handle one, Handle& made) {
    sockaddr_storage told{};

#ifdef _WIN32
    int much = sizeof told;

    made = ::accept(static_cast<SOCKET>(one), reinterpret_cast<sockaddr*>(&told), &much);

    if (made == nothing) return {};

    u_long yes = 1;

    ::ioctlsocket(static_cast<SOCKET>(made), FIONBIO, &yes);
#else
    socklen_t much = sizeof told;

    made = ::accept(one, reinterpret_cast<sockaddr*>(&told), &much);

    if (made < 0) {
        made = nothing;

        return {};
    }

    // Przyjęte gniazdo też nie ma trafić do programu potomnego.
    ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);

    ::fcntl(made, F_SETFL, ::fcntl(made, F_GETFL, 0) | O_NONBLOCK);
#endif

    return seen(told);
}

}

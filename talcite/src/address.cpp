#include "talcite/address.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "talcite/trouble.hpp"
#include "starting.hpp"

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace talcite::net {

namespace {

// Ile bajtów mieści ścieżka gniazda lokalnego. Pole `sun_path` ma 108 bajtów na Linuksie i na
// Windows, ale **104 na macOS**, a jeden zjada zero kończące napis — więc granicą jest najkrótszy
// z nich bez tego zera. Odmowa przy budowaniu adresu jest lepsza niż odmowa dopiero przy
// przypisaniu gniazda: tam wychodzi jako goły numer błędu, bez wskazania, co było za długie.
constexpr std::size_t longest = 103;

// Numer łącza po znaku procenta w adresie link-local. Bywa nazwą (`%eth0`) albo liczbą (`%3`) —
// Windows pisze liczby, systemy uniksowe nazwy, a ten sam adres musi dać się wpisać na obu.
std::uint32_t marked(const std::string& said) {
    if (said.empty()) return 0;

    if (said.size() <= 9 && std::all_of(said.begin(), said.end(),
                                        [](char one) { return one >= '0' && one <= '9'; }))
        return static_cast<std::uint32_t>(std::stoul(said));

#ifdef _WIN32
    return 0;
#else
    return if_nametoindex(said.c_str());
#endif
}

// Adres z gniazdowej postaci systemowej. Ta sama zamiana potrzebna jest przy getaddrinfo i przy
// wykazie kart, więc stoi raz.
bool drawn(const sockaddr* from, std::uint16_t port, Address& made) {
    std::array<std::byte, 16> raw{};

    if (from == nullptr) return false;

    if (from->sa_family == AF_INET) {
        const auto* four = reinterpret_cast<const sockaddr_in*>(from);

        std::memcpy(raw.data(), &four->sin_addr, 4);

        made = Address{Family::four, raw, port};

        return true;
    }

    if (from->sa_family == AF_INET6) {
        const auto* six = reinterpret_cast<const sockaddr_in6*>(from);

        std::memcpy(raw.data(), &six->sin6_addr, 16);

        made = Address{Family::six, raw, port};

        made.scope(six->sin6_scope_id);

        return true;
    }

    return false;
}

bool fitting(const Address& one, Family family) {
    return family == Family::none || one.family() == family;
}

// Numery od getaddrinfo nie są ani errno, ani numerami Winsocka — na systemach uniksowych mają
// własną tablicę. Na Windows to te same numery co reszta Winsocka, więc tam wystarczy spoken.
std::string amiss_named(int code) {
#ifdef _WIN32
    return spoken(code);
#else
    return ::gai_strerror(code);
#endif
}

}

Address::Address(std::string_view text, std::uint16_t port) {
    starting();

    std::string said{text};

    // Zero w środku tekstu **nie jest końcem adresu**. inet_pton bierze wskaźnik na znaki
    // zakończone zerem, więc widziało sam przedrostek, a reszty nikt nie sprawdzał — i „1.2.3.4"
    // sklejone z bzdurą budowało się pomyślnie. Kto kroi nazwę z bufora sieciowego, dostawał
    // poprawnie wyglądający adres z popsutego pola.
    if (said.find('\0') != std::string::npos)
        throw Trouble{"To nie jest adres: tekst ma zero w środku."};

    // Nawiasy kwadratowe zdejmujemy **przed** numerem łącza, a nie po nim. Odwrotnie zamykający
    // nawias wpadał do tekstu numeru, a do rozbioru szło `[fe80::1` — czyli postać, którą sami
    // wypisujemy przez spelled(), nie dawała się wczytać z powrotem.
    if (said.size() >= 2 && said.front() == '[' && said.back() == ']')
        said = said.substr(1, said.size() - 2);

    // Numer łącza odcinamy przed rozbiorem, bo inet_pton go nie zna.
    std::uint32_t scope = 0;

    if (const std::size_t mark = said.find('%'); mark != std::string::npos) {
        const std::string tail = said.substr(mark + 1);

        scope = marked(tail);

        // Nieznana nazwa łącza cichła do zera, a adres link-local bez numeru jest bezużyteczny:
        // błąd wychodził dopiero przy połączeniu, daleko od literówki.
        if (scope == 0)
            throw Trouble{"To nie jest numer łącza: " + tail + "."};

        said.resize(mark);
    }

    in_addr four{};

    if (::inet_pton(AF_INET, said.c_str(), &four) == 1) {
        std::memcpy(bytes_.data(), &four, 4);

        family_ = Family::four;
        port_ = port;

        return;
    }

    in6_addr six{};

    if (::inet_pton(AF_INET6, said.c_str(), &six) == 1) {
        std::memcpy(bytes_.data(), &six, 16);

        family_ = Family::six;
        scope_ = scope;
        port_ = port;

        return;
    }

    // Nazwa domenowa **nie jest** adresem i nie udajemy, że jest — od nazw jest resolve, który
    // umie zapytać świata. Cichy pusty adres wyszedłby dopiero przy nieudanym połączeniu.
    throw Trouble{"To nie jest adres: " + std::string{text} + "."};
}

// Adres lokalny nie powstaje z bajtów ani z portu — jest od tego `local()`. Bez tej odmowy dałoby
// się złożyć adres, który mówi o sobie „lokalny", a niesie szesnaście zer i numer portu; taki
// przeszedłby przez `held()` i dopiero gniazdo powiedziałoby, że czegoś w nim brakuje.
Address::Address(Family family, const std::array<std::byte, 16>& bytes, std::uint16_t port)
    : bytes_(bytes), port_(port), family_(family) {
    if (family == Family::local)
        throw Trouble{"Adres lokalny nie powstaje z bajtów — jest od tego local()."};
}

// Ścieżkę bierzemy przez wartość i przenosimy: wołający albo ją buduje na miejscu, albo oddaje
// swoją, a kopiowanie w środku byłoby trzecim egzemplarzem tego samego napisu.
Address Address::local(std::string path) {
    if (path.empty()) throw Trouble{"Ścieżka gniazda lokalnego jest pusta."};

    // Ten sam powód, dla którego zero w środku odrzuca konstruktor tekstowy: ścieżka jedzie do
    // pola zakończonego zerem, więc reszty i tak nikt by nie zobaczył.
    if (path.find('\0') != std::string::npos)
        throw Trouble{"Ścieżka gniazda lokalnego ma zero w środku."};

    if (path.size() > longest)
        throw Trouble{"Ścieżka gniazda lokalnego jest dłuższa niż " + std::to_string(longest) +
                      " bajtów: " + path + "."};

    Address made;

    made.family_ = Family::local;
    made.path_ = std::move(path);

    return made;
}

// Port i numer łącza **odmawiają** na adresie lokalnym, zamiast zapisać wartość, której nikt
// nigdy nie przeczyta. Stan, którego nie da się wyrazić, jest lepszy od stanu, którego nie da się
// zauważyć — ta sama zasada, na której stoi `Key`.
void Address::port(std::uint16_t given) {
    if (family_ == Family::local) throw Trouble{"Adres lokalny nie ma portu."};

    port_ = given;
}

void Address::scope(std::uint32_t given) {
    if (family_ == Family::local) throw Trouble{"Adres lokalny nie ma numeru łącza."};

    scope_ = given;
}

Address Address::any(Family family, std::uint16_t port) {
    if (family == Family::local)
        throw Trouble{"Adres lokalny nie ma postaci dowolnej — trzeba podać ścieżkę."};

    Address made;

    made.family_ = family;
    made.port_ = port;

    return made;
}

Address Address::loopback(Family family, std::uint16_t port) {
    if (family == Family::local)
        throw Trouble{"Gniazdo lokalne samo jest pętlą zwrotną — nie ma osobnego adresu."};

    Address made = any(family, port);

    if (family == Family::four) {
        made.bytes_[0] = std::byte{127};
        made.bytes_[3] = std::byte{1};
    } else if (family == Family::six) {
        made.bytes_[15] = std::byte{1};
    }

    return made;
}

std::string Address::text() const {
    if (!held()) return {};

    if (family_ == Family::local) return path_;

    char made[INET6_ADDRSTRLEN]{};

    const int which = family_ == Family::four ? AF_INET : AF_INET6;

    if (::inet_ntop(which, bytes_.data(), made, sizeof made) == nullptr) return {};

    std::string said{made};

    if (family_ == Family::six && scope_ != 0) said += "%" + std::to_string(scope_);

    return said;
}

// Adres IPv6 z portem zapisuje się w nawiasach kwadratowych, bo inaczej dwukropków nie da się
// rozdzielić. Trzymamy to w jednym miejscu, żeby nie było sklejane w pięciu.
std::string Address::spelled() const {
    if (!held()) return {};

    // Ścieżka bez doklejonego portu: gniazdo lokalne portu nie ma, a „/run/tw.sock:0" byłoby
    // kłamstwem o tym, czym ten adres jest.
    if (family_ == Family::local) return path_;

    if (family_ == Family::four) return text() + ":" + std::to_string(port_);

    return "[" + text() + "]:" + std::to_string(port_);
}

Promise<std::vector<Address>> resolve(std::string host, std::uint16_t port, Family family) {
    return Promise<std::vector<Address>>{[host, port, family](auto giving, auto amiss) {
        starting();

        addrinfo wanted{};

        wanted.ai_family = family == Family::four    ? AF_INET
                           : family == Family::six ? AF_INET6
                                                   : AF_UNSPEC;
        wanted.ai_socktype = SOCK_STREAM;

        addrinfo* found = nullptr;

        const int code = ::getaddrinfo(host.c_str(), nullptr, &wanted, &found);

        if (code != 0) {
            amiss(std::make_exception_ptr(
                Trouble{"Nie znam nazwy " + host + ": " + amiss_named(code) + ".", code}));

            return;
        }

        // Wykaz od systemu zwalnia strażnik, nie linia na końcu: zbieranie potrafi rzucić przy
        // braku pamięci, a wtedy przydział systemowy wyciekał.
        struct Freeing {
            addrinfo* one;

            ~Freeing() { ::freeaddrinfo(one); }
        } const freeing{found};

        std::vector<Address> made;

        for (const addrinfo* one = found; one != nullptr; one = one->ai_next) {
            Address which;

            if (!drawn(one->ai_addr, port, which)) continue;

            // Ten sam adres potrafi przyjść kilka razy, raz na każdy rodzaj gniazda.
            if (std::find(made.begin(), made.end(), which) == made.end()) made.push_back(which);
        }

        if (made.empty()) {
            amiss(std::make_exception_ptr(Trouble{"Nazwa nie ma adresów: " + host + "."}));

            return;
        }

        giving(std::move(made));
    }};
}


// Wykaz własnych adresów. Potrzebny przy zbieraniu kandydatów do przebijania NAT: dwie maszyny
// w jednej sieci lokalnej mają się dogadać przez switcha obok, a nie przez pół internetu.
//
// **Oddajemy wszystko, co system zgłasza na podniesionych kartach, razem z pętlą zwrotną.** To jest
// pytanie „jakie mam adresy", a nie „które z nich się do czegoś nadadzą" — odsiewanie należy do
// wołającego, bo każde zastosowanie odsiewa inaczej. Zbieranie kandydatów wyrzuci pętlę zwrotną,
// żeby druga strona nie próbowała dobić się do samej siebie.
Promise<std::vector<Address>> mine(Family family) {
    return Promise<std::vector<Address>>{[family](auto giving, auto amiss) {
        starting();

        std::vector<Address> made;

#ifdef _WIN32
        // Rozmiar bufora zmienia się między pytaniem a odpowiedzią, bo karty potrafią dojść w
        // międzyczasie. Stąd pętla, a nie jedno wywołanie z odczytanym rozmiarem.
        ULONG much = 16384;

        std::vector<std::byte> room;

        IP_ADAPTER_ADDRESSES* first = nullptr;

        ULONG code = ERROR_BUFFER_OVERFLOW;

        for (int turn = 0; turn < 4 && code == ERROR_BUFFER_OVERFLOW; ++turn) {
            room.assign(much, std::byte{});

            first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(room.data());

            code = ::GetAdaptersAddresses(AF_UNSPEC,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
                                          nullptr, first, &much);
        }

        // ERROR_NO_DATA znaczy „nie ma adresów tej rodziny", nie awarię — POSIX oddaje wtedy
        // pusty wykaz i tak samo ma robić Windows.
        if (code != NO_ERROR && code != ERROR_NO_DATA) {
            amiss(std::make_exception_ptr(
                Trouble{"Nie umiem wypisać własnych adresów: " + spoken(static_cast<int>(code)) + ".",
                        static_cast<int>(code)}));

            return;
        }

        for (const IP_ADAPTER_ADDRESSES* card = first; card != nullptr; card = card->Next) {
            if (card->OperStatus != IfOperStatusUp) continue;

            for (const IP_ADAPTER_UNICAST_ADDRESS* one = card->FirstUnicastAddress; one != nullptr;
                 one = one->Next) {
                Address which;

                if (!drawn(one->Address.lpSockaddr, 0, which)) continue;

                if (!fitting(which, family)) continue;

                if (std::find(made.begin(), made.end(), which) == made.end()) made.push_back(which);
            }
        }
#else
        ifaddrs* first = nullptr;

        if (::getifaddrs(&first) != 0) {
            amiss(std::make_exception_ptr(
                Trouble{"Nie umiem wypisać własnych adresów: " + spoken(errno) + ".", errno}));

            return;
        }

        struct Freeing {
            ifaddrs* one;

            ~Freeing() { ::freeifaddrs(one); }
        } const freeing{first};

        for (const ifaddrs* one = first; one != nullptr; one = one->ifa_next) {
            if ((one->ifa_flags & IFF_UP) == 0) continue;

            Address which;

            if (!drawn(one->ifa_addr, 0, which)) continue;

            if (!fitting(which, family)) continue;

            // Numer łącza **tylko dla link-local** (fe80::/10). Dla reszty zero jest stanem
            // normalnym — glibc wypełnia to pole wyłącznie przy adresach łączowych — a doklejany
            // wszystkim robił z pętli zwrotnej `::1%1`, przez co przestawała równać się `::1`
            // i wychodziła na zewnątrz jako kandydat do przebijania.
            if (which.family() == Family::six && which.scope() == 0 && one->ifa_name != nullptr &&
                std::to_integer<unsigned>(which.bytes()[0]) == 0xFE &&
                (std::to_integer<unsigned>(which.bytes()[1]) & 0xC0) == 0x80)
                which.scope(::if_nametoindex(one->ifa_name));

            if (std::find(made.begin(), made.end(), which) == made.end()) made.push_back(which);
        }

#endif

        giving(std::move(made));
    }};
}

}

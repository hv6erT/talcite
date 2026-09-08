#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/peer.hpp"
#include "talcite/post.hpp"
#include "talcite/scout.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

#include "servers.hpp"
#include "speaking.hpp"

namespace {

using namespace talcite;

Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

std::string spelling(const Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

// Adres drugiej strony podaje się dokładnie tak, jak wypisał go jej program: adres, dwukropek, port.
bool taken(const std::string& said, net::Address& made) {
    const std::size_t colon = said.rfind(':');

    if (colon == std::string::npos || colon + 1 >= said.size()) return false;

    const unsigned long port = std::stoul(said.substr(colon + 1));

    if (port > 65535) return false;

    made = net::Address{said.substr(0, colon), static_cast<std::uint16_t>(port)};

    return true;
}

std::string named(net::Scout::Mapping what) {
    switch (what) {
        case net::Scout::Mapping::alike: return "zgodne — przebijanie ma sens";
        case net::Scout::Mapping::symmetric: return "symetryczne — przebijanie nie zadziała";
        default: return "nieznane";
    }
}

}

// Przebijanie NAT sprawdzone naprawdę. Testy przechodzą protokół po pętli zwrotnej, a to jest coś
// zupełnie innego niż dwie maszyny za dwoma NAT-ami — czyli dokładnie ten przypadek, na którym
// `fork` się przewracał.
//
// **Sygnalizacją jesteś Ty.** Talcite jej nie pisze i nie napisze: nie ma jednego standardu, a wybór
// należy do wdrożenia. Tutaj kanałem sygnalizacji jest kopiuj-wklej.
//
// Uruchamia się **raz, po obu stronach naraz**, bez argumentów. Każda strona wypisze swoje drogi
// i poczeka, aż wklei się cudzą; po wklejeniu obie zaczynają się dobijać.
//
// Uruchomienie dwa razy — najpierw po adres, potem z cudzym adresem — **nie zadziała i nie może**:
// odwzorowanie na NAT-cie należy do **gniazda**, a drugie uruchomienie otwiera nowe gniazdo i
// dostaje nowy port zewnętrzny. Adres podany drugiej stronie wskazywałby wtedy dziurę, której już
// nie ma. Dlatego gniazdo musi przeżyć całą wymianę adresów — i dlatego czytamy z wejścia zamiast
// z wiersza poleceń.
int main(int count, char** given) {
    const showing::Speaking speaking;

    const net::Patience soon{8000};

    try {
        net::Post mine{net::Address::any(net::Family::four, 0)};

        net::Scout scout{mine, soon};

        for (const showing::Server& one : showing::known()) {
            try {
                const std::vector<net::Address> found =
                    net::resolve(one.first, one.second, net::Family::four).await();

                if (!found.empty()) scout.add(found.front());
            } catch (const std::exception&) {
            }
        }

        if (scout.servers().empty()) {
            std::cout << "żadnego serwera STUN nie udało się rozwiązać — bez tego nie poznam"
                         " swojego adresu zewnętrznego.\n";

            return 1;
        }

        std::cout << "odwzorowanie: " << named(scout.mapping().await()) << "\n";

        net::Peer peer{mine};

        const std::vector<net::Way> ways = peer.gather(scout, net::Patience{12000}).await();

        std::cout << "\nmoje drogi — przepisz je drugiej stronie:\n";

        for (const net::Way& one : ways)
            std::cout << "    " << one.where.spelled()
                      << (one.sort == net::Way::Sort::here ? "   (własna karta)" : "   (odbite)")
                      << "\n";

        std::vector<std::string> said;

        for (int at = 1; at < count; ++at) said.emplace_back(given[at]);

        if (said.empty()) {
            std::cout << "\nWklej **odbity** adres drugiej strony i naciśnij enter."
                         " Gniazdo czeka otwarte,\nwięc adres wypisany wyżej jest wciąż ważny.\n> ";

            std::string line;

            if (!std::getline(std::cin, line) || line.empty()) {
                std::cout << "\nnic nie podano — kończę.\n";

                return 0;
            }

            said.push_back(line);
        }

        for (const std::string& one : said) {
            net::Address where;

            if (!taken(one, where)) {
                std::cout << "nie rozumiem adresu: " << one << "\n";

                return 2;
            }

            peer.add(net::Way{where, net::Way::Sort::seen});

            std::cout << "\ndobijam się do " << where.spelled() << "\n";
        }

        const net::Address met = peer.reach(net::Patience{45000}).await();

        std::cout << "**przebite** — rozmawiam z " << met.spelled() << "\n\n";

        const std::string me = ways.empty() ? std::string{"?"} : ways.back().where.spelled();

        // Trzy części, bo dwie rzeczy warto zobaczyć osobno. Najpierw rozmowa: czy treść idzie
        // w obie strony. Potem **cisza dłuższa niż odwzorowanie żyje na NAT-cie** — zwykle
        // trzydzieści sekund — w której gada wyłącznie podtrzymanie z wnętrza rozmówcy. I znów
        // rozmowa: jeśli przeszła, to znaczy, że podtrzymanie utrzymało dziurę otwartą.
        const auto talking = [&](const char* what, std::chrono::seconds much) {
            const auto began = std::chrono::steady_clock::now();

            int turn = 0;

            while (std::chrono::steady_clock::now() - began < much) {
                peer.send(spelt(std::string{what} + " " + std::to_string(turn++) + " od " + me),
                          soon)
                    .await();

                Time::later(std::chrono::milliseconds{1000}).await();

                for (int at = 0; at < 8; ++at) {
                    Bytes came;

                    // Najpierw odbieramy, dopiero potem piszemy. Wypisanie początku zdania przed
                    // odbiorem zostawiało je urwane, gdy kolejka okazała się pusta.
                    try {
                        came = peer.receive(4096, net::Patience{1}).await();
                    } catch (const std::exception&) {
                        break;
                    }

                    std::cout << "  słyszę: " << spelling(came) << "\n";
                }
            }
        };

        std::cout << "rozmowa przez dziesięć sekund:\n";

        talking("rata", std::chrono::seconds{10});

        std::cout << "\ncisza przez czterdzieści pięć sekund — gada tylko podtrzymanie:\n";

        Time::later(std::chrono::milliseconds{45000}).await();

        std::cout << "\nznów rozmowa — jeśli idzie, dziura przetrwała ciszę:\n";

        talking("po ciszy", std::chrono::seconds{10});

        std::cout << "\nkoniec.\n";
    } catch (const Overdue&) {
        std::cout << "nie zdążyliśmy się przebić. Obie strony muszą dobijać się naraz, a przy"
                     " NAT-cie symetrycznym po którejkolwiek stronie nie uda się nigdy.\n";

        return 1;
    } catch (const net::Trouble& why) {
        std::cout << "kłopot: " << why.what() << "\n";

        return 1;
    } catch (const std::exception& why) {
        std::cout << "kłopot: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

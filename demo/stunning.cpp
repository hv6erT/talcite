#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/udp.hpp"
#include "talcite/stunclient.hpp"
#include "talcite/time.hpp"

#include "servers.hpp"
#include "speaking.hpp"

namespace {

using showing::known;
using showing::Server;

std::string named(talcite::net::StunClient::Mapping what) {
    switch (what) {
        case talcite::net::StunClient::Mapping::alike: return "zgodne — przebijanie ma sens";
        case talcite::net::StunClient::Mapping::symmetric:
            return "symetryczne — przebijanie nie zadziała";
        default: return "nieznane";
    }
}

// Adres serwera podaje się jako gospodarz:port albo sam gospodarz — wtedy port 3478, jak każe RFC.
bool taken(const std::string& said, Server& made) {
    const std::size_t colon = said.rfind(':');

    // Dwukropek w środku IPv6 nie jest rozdzielnikiem portu; tam trzeba nawiasów albo nic.
    if (colon == std::string::npos || said.find(':') != colon) {
        made = Server{said, 3478};

        return !said.empty();
    }

    const std::string host = said.substr(0, colon);
    const std::string rest = said.substr(colon + 1);

    if (host.empty() || rest.empty()) return false;

    // Numer portu **nie zawija się po cichu**: 99999 stawało się portem 34463, a wpisany minus
    // jeden — portem 65535.
    const unsigned long which = std::stoul(rest);

    if (which > 65535) return false;

    made = Server{host, static_cast<std::uint16_t>(which)};

    return true;
}

}

// Testy sprawdzają kodek wobec udawanego serwera złożonego bajt po bajcie. Tego, czy prawdziwy
// serwer w internecie odpowiada tak, jak myślimy, żaden test w piaskownicy nie sprawdzi — od tego
// jest ten program. Bez argumentów bierze serwery wbudowane; z argumentami pyta podanych, na
// przykład:
//
//     talcite-stunning stun.l.google.com:19302 stun.cloudflare.com:3478
//
// Dwa serwery albo więcej dają dodatkowo werdykt o odwzorowaniu NAT; jeden daje sam adres.
int main(int count, char** given) {
    using namespace talcite;

    // Konsola Windows domyślnie nie jest w UTF-8, a my piszemy w UTF-8.
    const showing::Speaking speaking;

    const net::Patience soon{8000};

    try {
        std::vector<Server> asking;

        if (count < 2) {
            asking = known();

            std::cout << "biorę serwery wbudowane; własne podaje się w wierszu poleceń.\n";
        } else {
            for (int at = 1; at < count; ++at) {
                Server one;

                if (!taken(given[at], one)) {
                    std::cout << "nie rozumiem adresu: " << given[at] << "\n";

                    return 2;
                }

                asking.push_back(std::move(one));
            }
        }

        net::Udp mine{net::Address::any(net::Family::four, 0)};

        std::cout << "gniazdo stoi na " << mine.mine().spelled() << "\n";

        net::StunClient scout{mine, soon};

        for (const Server& one : asking) {
            std::vector<net::Address> found;

            try {
                found = net::resolve(one.first, one.second, net::Family::four).await();
            } catch (const net::Trouble& why) {
                std::cout << "nie rozwiązałem " << one.first << ": " << why.what() << "\n";

                continue;
            }

            if (found.empty()) {
                std::cout << "nic nie znalazłem pod " << one.first << "\n";

                continue;
            }

            std::cout << "serwer " << one.first << " to " << found.front().spelled() << "\n";

            scout.add(found.front());
        }

        if (scout.servers().empty()) {
            std::cout << "żadnego serwera nie udało się rozwiązać.\n";

            return 1;
        }

        const net::Address seen = scout.seen().await();

        std::cout << "widzą mnie jako " << seen.spelled() << "\n";

        if (scout.servers().size() >= 2)
            std::cout << "odwzorowanie: " << named(scout.mapping().await()) << "\n";
        else
            std::cout << "o odwzorowaniu nic nie powiem — trzeba dwóch serwerów.\n";
    } catch (const Overdue&) {
        std::cout << "żaden serwer nie odpowiedział na czas.\n";

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

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

// What the NAT mapping means for hole punching, in words.
std::string named(talcite::net::StunClient::Mapping what) {
    switch (what) {
        case talcite::net::StunClient::Mapping::alike: return "consistent — punching makes sense";
        case talcite::net::StunClient::Mapping::symmetric:
            return "symmetric — punching will not work";
        default: return "unknown";
    }
}

// A server address is given as host:port or as a bare host, which then gets port 3478, as the RFC
// says. Returns false for an empty host or port, or a port above 65535.
bool taken(const std::string& said, Server& made) {
    const std::size_t colon = said.rfind(':');

    // A colon inside IPv6 is not a port separator; there it takes brackets or nothing.
    if (colon == std::string::npos || said.find(':') != colon) {
        made = Server{said, 3478};

        return !said.empty();
    }

    const std::string host = said.substr(0, colon);
    const std::string rest = said.substr(colon + 1);

    if (host.empty() || rest.empty()) return false;

    // The port number does not wrap silently: without the check, 99999 would become port 34463, and
    // a typed minus one port 65535.
    const unsigned long which = std::stoul(rest);

    if (which > 65535) return false;

    made = Server{host, static_cast<std::uint16_t>(which)};

    return true;
}

}

// The tests check the codec against a fake server assembled byte by byte. Whether a real server on
// the internet answers the way we think, no sandboxed test can check; that is what this program is
// for. Without arguments it takes the built-in servers; with arguments it asks the given ones, for
// example:
//
//     talcite-stunning stun.l.google.com:19302 stun.cloudflare.com:3478
//
// Two or more servers also give a verdict on the NAT mapping; one gives only the address.
int main(int count, char** given) {
    using namespace talcite;

    // The Windows console is not in UTF-8 by default, and we write UTF-8.
    const showing::Speaking speaking;

    const net::Patience soon{8000};

    try {
        std::vector<Server> asking;

        if (count < 2) {
            asking = known();

            std::cout << "using the built-in servers; your own go on the command line.\n";
        } else {
            for (int at = 1; at < count; ++at) {
                Server one;

                if (!taken(given[at], one)) {
                    std::cout << "cannot understand the address: " << given[at] << "\n";

                    return 2;
                }

                asking.push_back(std::move(one));
            }
        }

        net::Udp mine{net::Address::any(net::Family::four, 0)};

        std::cout << "socket up at " << mine.mine().spelled() << "\n";

        net::StunClient scout{mine, soon};

        for (const Server& one : asking) {
            std::vector<net::Address> found;

            try {
                found = net::resolve(one.first, one.second, net::Family::four).await();
            } catch (const net::Trouble& why) {
                std::cout << "could not resolve " << one.first << ": " << why.what() << "\n";

                continue;
            }

            if (found.empty()) {
                std::cout << "found nothing under " << one.first << "\n";

                continue;
            }

            std::cout << "server " << one.first << " is " << found.front().spelled() << "\n";

            scout.add(found.front());
        }

        if (scout.servers().empty()) {
            std::cout << "no server could be resolved.\n";

            return 1;
        }

        const net::Address seen = scout.seen().await();

        std::cout << "they see me as " << seen.spelled() << "\n";

        if (scout.servers().size() >= 2)
            std::cout << "mapping: " << named(scout.mapping().await()) << "\n";
        else
            std::cout << "nothing to say about the mapping — it takes two servers.\n";
    } catch (const Overdue&) {
        std::cout << "no server answered in time.\n";

        return 1;
    } catch (const net::Trouble& why) {
        std::cout << "trouble: " << why.what() << "\n";

        return 1;
    } catch (const std::exception& why) {
        std::cout << "trouble: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

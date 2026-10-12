#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/peer.hpp"
#include "talcite/udp.hpp"
#include "talcite/stunclient.hpp"
#include "talcite/time.hpp"

#include "servers.hpp"
#include "speaking.hpp"

namespace {

using namespace talcite;

// The bytes of a string, one per character.
Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

// The bytes as a string, one character per byte.
std::string spelling(const Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

// The other side's address is given exactly as its program printed it: address, colon, port.
// Returns false when there is no port or it is above 65535.
bool taken(const std::string& said, net::Address& made) {
    const std::size_t colon = said.rfind(':');

    if (colon == std::string::npos || colon + 1 >= said.size()) return false;

    const unsigned long port = std::stoul(said.substr(colon + 1));

    if (port > 65535) return false;

    made = net::Address{said.substr(0, colon), static_cast<std::uint16_t>(port)};

    return true;
}

// What the NAT mapping means for hole punching, in words.
std::string named(net::StunClient::Mapping what) {
    switch (what) {
        case net::StunClient::Mapping::alike: return "consistent — punching makes sense";
        case net::StunClient::Mapping::symmetric:
            return "symmetric — punching will not work";
        default: return "unknown";
    }
}

}

// NAT hole punching, tested for real. The tests run the protocol over loopback, and that is
// something quite different from two machines behind two NATs, which is exactly the case this
// program covers.
//
// You are the signaling. talcite does not write it and will not: there is no single standard, and
// the choice belongs to the deployment. Here the signaling channel is copy and paste.
//
// Run it once, on both sides at the same time, without arguments. Each side prints its ways and
// waits for the other side's to be pasted; once they are pasted, both start knocking.
//
// Running it twice (first for the address, then with the other side's address) does not work and
// cannot: a mapping on the NAT belongs to the socket, and a second run opens a new socket and gets
// a new external port. The address given to the other side would then point at a hole that no
// longer exists. So the socket has to live through the whole exchange of addresses, and that is why
// we read from standard input rather than from the command line.
int main(int count, char** given) {
    const showing::Speaking speaking;

    const net::Patience soon{8000};

    try {
        net::Udp mine{net::Address::any(net::Family::four, 0)};

        net::StunClient scout{mine, soon};

        for (const showing::Server& one : showing::known()) {
            try {
                const std::vector<net::Address> found =
                    net::resolve(one.first, one.second, net::Family::four).get();

                if (!found.empty()) scout.add(found.front());
            } catch (const std::exception&) {
            }
        }

        if (scout.servers().empty()) {
            std::cout << "no STUN server could be resolved — without one I cannot learn"
                         " my external address.\n";

            return 1;
        }

        std::cout << "mapping: " << named(scout.mapping().get()) << "\n";

        net::Peer peer{mine};

        const std::vector<net::Candidate> ways = peer.gather(scout, net::Patience{12000}).get();

        std::cout << "\nmy ways — copy them to the other side:\n";

        for (const net::Candidate& one : ways)
            std::cout << "    " << one.where.spelled()
                      << (one.sort == net::Candidate::Sort::here ? "   (own interface)"
                                                                 : "   (reflected)")
                      << "\n";

        std::vector<std::string> said;

        for (int at = 1; at < count; ++at) said.emplace_back(given[at]);

        if (said.empty()) {
            std::cout << "\nPaste the other side's reflected address and press enter."
                         " The socket stays open,\nso the address printed above is still valid.\n> ";

            std::string line;

            if (!std::getline(std::cin, line) || line.empty()) {
                std::cout << "\nnothing given — quitting.\n";

                return 0;
            }

            said.push_back(line);
        }

        for (const std::string& one : said) {
            net::Address where;

            if (!taken(one, where)) {
                std::cout << "cannot understand the address: " << one << "\n";

                return 2;
            }

            peer.add(net::Candidate{where, net::Candidate::Sort::seen});

            std::cout << "\nknocking at " << where.spelled() << "\n";
        }

        const net::Address met = peer.reach(net::Patience{45000}).get();

        std::cout << "punched through — talking to " << met.spelled() << "\n\n";

        const std::string me = ways.empty() ? std::string{"?"} : ways.back().where.spelled();

        // Three parts, because two things are worth seeing separately. First a conversation: does
        // content go both ways. Then a silence longer than a mapping lives on the NAT (usually
        // thirty seconds), during which only the peer's internal keepalive talks. And a
        // conversation again: if it gets through, the keepalive has kept the hole open.
        const auto talking = [&](const char* what, std::chrono::seconds much) {
            const auto began = std::chrono::steady_clock::now();

            int turn = 0;

            while (std::chrono::steady_clock::now() - began < much) {
                peer.send(spelt(std::string{what} + " " + std::to_string(turn++) + " from " + me),
                          soon)
                    .get();

                later(std::chrono::milliseconds{1000}).get();

                for (int at = 0; at < 8; ++at) {
                    Bytes came;

                    // Receive first and print afterwards, so that a line is never left half-printed
                    // when the queue turns out to be empty.
                    try {
                        came = peer.receive(4096, net::Patience{1}).get();
                    } catch (const std::exception&) {
                        break;
                    }

                    std::cout << "  hearing: " << spelling(came) << "\n";
                }
            }
        };

        std::cout << "talking for ten seconds:\n";

        talking("round", std::chrono::seconds{10});

        std::cout << "\nforty-five seconds of silence — only the keepalive talks:\n";

        later(std::chrono::milliseconds{45000}).get();

        std::cout << "\ntalking again — if it gets through, the hole survived the silence:\n";

        talking("after silence", std::chrono::seconds{10});

        std::cout << "\nthe end.\n";
    } catch (const Overdue&) {
        std::cout << "we did not punch through in time. Both sides have to knock at the same time, and with a"
                     " symmetric NAT on either side it will never work.\n";

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

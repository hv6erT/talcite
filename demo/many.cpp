#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "talcite/listener.hpp"
#include "talcite/stream.hpp"

#include "speaking.hpp"

using namespace talcite;

// The real shape of a server: a read waits on every connection, but only a handful speak up. This
// is the case in which epoll beats poll, and the only one in which the difference shows at all.
//
//     talcite-many [connections] [active] [rounds]
//
// The program exercises what the unit tests do not reach: the order in which static objects are
// destroyed, and calls into a hub that has already been torn down. It is worth running.
int main(int count, char** args) {
    // The Windows console is not in UTF-8 by default, and we write UTF-8.
    const showing::Speaking speaking;

    const int many = count > 1 ? std::atoi(args[1]) : 1000;
    const int busy = count > 2 ? std::atoi(args[2]) : 10;
    const int rounds = count > 3 ? std::atoi(args[3]) : 300;

    const net::Patience soon{30000};

    try {
        net::Listener gate{net::Address::loopback(net::Family::four, 0)};

        std::vector<net::Stream> clients;
        std::vector<net::Stream> servers;

        for (int at = 0; at < many; ++at) {
            auto meeting = gate.accept(soon);
            auto joining = net::Stream::connect(gate.mine(), soon);

            servers.push_back(std::move(meeting).get());
            clients.push_back(std::move(joining).get());
        }

        std::cout << many << " connections up\n";

        std::vector<Promise<Bytes>> reading;

        for (int at = 0; at < many; ++at) reading.push_back(servers[at].read(64, soon));

        const Bytes one(64, std::byte{7});

        const auto began = std::chrono::steady_clock::now();

        int at = 0;

        for (int round = 0; round < rounds; ++round) {
            std::vector<int> which;
            std::vector<Promise<void>> writing;

            for (int done = 0; done < busy; ++done) {
                which.push_back(at);

                at = (at + 1) % many;
            }

            for (const int one_at : which) writing.push_back(clients[one_at].write(one, soon));

            for (std::size_t done = 0; done < which.size(); ++done) {
                std::move(writing[done]).get();

                (void)std::move(reading[which[done]]).get();

                reading[which[done]] = servers[which[done]].read(64, soon);
            }
        }

        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);

        const long long whole = static_cast<long long>(busy) * rounds;

        std::cout << busy << " active, " << whole << " exchanges in " << spent.count() << " ms — "
                  << (spent.count() > 0 ? whole * 1000 / spent.count() : 0) << " per second\n";
    } catch (const std::exception& why) {
        std::cout << "trouble: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

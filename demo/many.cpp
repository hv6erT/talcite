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

// Prawdziwy kształt serwera: na **każdym** połączeniu wisi czytanie, ale odzywa się garstka.
// To jest przypadek, w którym epoll bije poll — i jedyny, w którym w ogóle widać różnicę.
//
//     talcite-many [połączeń] [czynnych] [rund]
//
// Ten program znalazł dwa błędy, których nie złapało pięćset testów: kolejność niszczenia
// obiektów statycznych i sięganie do centrali już rozebranej. Warto go uruchamiać.
int main(int count, char** args) {
    // Konsola Windows domyślnie nie jest w UTF-8, a my piszemy w UTF-8.
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

            servers.push_back(meeting.await());
            clients.push_back(joining.await());
        }

        std::cout << many << " połączeń stoi\n";

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
                writing[done].await();

                (void)reading[which[done]].await();

                reading[which[done]] = servers[which[done]].read(64, soon);
            }
        }

        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);

        const long long whole = static_cast<long long>(busy) * rounds;

        std::cout << busy << " czynnych, " << whole << " wymian w " << spent.count() << " ms — "
                  << (spent.count() > 0 ? whole * 1000 / spent.count() : 0) << " na sekundę\n";
    } catch (const std::exception& why) {
        std::cout << "kłopot: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

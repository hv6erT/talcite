#include <cstddef>
#include <iostream>
#include <string>

#include "talcite/exchange.hpp"
#include "talcite/gate.hpp"
#include "talcite/post.hpp"
#include "talcite/stream.hpp"
#include "talcite/trouble.hpp"

#include "speaking.hpp"

namespace {

talcite::Bytes spelt(const std::string& what) {
    talcite::Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

std::string read(const talcite::Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

}

// Najkrótszy dowód, że sieć w talcite działa na tej maszynie: brama, połączenie do samego siebie,
// odbicie napisu i koniec. Nic tu nie wymaga drugiego programu ani internetu.
int main() {
    using namespace talcite;

    // Konsola Windows domyślnie nie jest w UTF-8, a my piszemy w UTF-8. Bez tego cały wypis
    // wychodzi zniekształcony — polskie znaki rozpadają się na pary krzaków.
    const showing::Speaking speaking;

    const net::Patience soon{5000};

    try {
        const net::Gate gate{net::Address::loopback(net::Family::four, 0)};

        std::cout << "brama stoi na " << gate.mine().spelled() << "\n";

        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(gate.mine(), soon);

        net::Stream server = meeting.await();
        net::Stream client = joining.await();

        std::cout << "klient " << client.mine().spelled() << " rozmawia z "
                  << client.peer().spelled() << "\n";

        client.write(spelt("witaj, świecie"), soon).await();

        const std::string heard = read(server.read(1024, soon).await());

        std::cout << "serwer usłyszał: " << heard << "\n";

        server.write(spelt(heard + " — i wzajemnie"), soon).await();

        std::cout << "klient usłyszał: " << read(client.read(1024, soon).await()) << "\n";

        client.close();

        std::cout << "po zamknięciu serwer czyta: " << server.read(1024, soon).await().size()
                  << " bajtów\n";

        const net::Post here{net::Address::loopback(net::Family::four, 0)};
        const net::Post there{net::Address::loopback(net::Family::four, 0)};

        std::cout << "poczta stoi na " << here.mine().spelled() << "\n";

        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), spelt("kartka bez połączenia"), soon).await();

        const net::Parcel came = waiting.await();

        std::cout << "poczta dostała od " << came.from.spelled() << ": " << read(came.bytes)
                  << "\n";
    } catch (const net::Trouble& why) {
        std::cout << "kłopot: " << why.what() << "\n";

        return 1;
    } catch (const std::exception& why) {
        std::cout << "kłopot: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

#include <cstddef>
#include <iostream>
#include <string>

#include "talcite/address.hpp"
#include "talcite/listener.hpp"
#include "talcite/udp.hpp"
#include "talcite/stream.hpp"

#include "speaking.hpp"

namespace {

// The bytes of a string, one per character.
talcite::Bytes spelt(const std::string& what) {
    talcite::Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

// The bytes as a string, one character per byte.
std::string read(const talcite::Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

}

// The shortest proof that networking in talcite works on this machine: a listener, a connection to
// itself, an echoed text, and the end. Nothing here needs a second program or the internet.
int main() {
    using namespace talcite;

    // The Windows console is not in UTF-8 by default, and we write UTF-8. Without this the whole
    // output comes out garbled, with every non-ASCII letter falling apart into a pair of junk
    // characters.
    const showing::Speaking speaking;

    const net::Patience soon{5000};

    try {
        const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

        std::cout << "listener up at " << gate.mine().spelled() << "\n";

        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(gate.mine(), soon);

        net::Stream server = meeting.await();
        net::Stream client = joining.await();

        std::cout << "client " << client.mine().spelled() << " talks to "
                  << client.peer().spelled() << "\n";

        client.write(spelt("hello, world"), soon).await();

        const std::string heard = read(server.read(1024, soon).await());

        std::cout << "server heard: " << heard << "\n";

        server.write(spelt(heard + " — and to you too"), soon).await();

        std::cout << "client heard: " << read(client.read(1024, soon).await()) << "\n";

        client.close();

        std::cout << "after the close the server reads: " << server.read(1024, soon).await().size()
                  << " bytes\n";

        const net::Udp here{net::Address::loopback(net::Family::four, 0)};
        const net::Udp there{net::Address::loopback(net::Family::four, 0)};

        std::cout << "mailbox up at " << here.mine().spelled() << "\n";

        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), spelt("a note without a connection"), soon).await();

        const net::Datagram came = waiting.await();

        std::cout << "mailbox got from " << came.from.spelled() << ": " << read(came.bytes)
                  << "\n";
    } catch (const net::Trouble& why) {
        std::cout << "trouble: " << why.what() << "\n";

        return 1;
    } catch (const std::exception& why) {
        std::cout << "trouble: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

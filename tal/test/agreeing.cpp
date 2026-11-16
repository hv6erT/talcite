#include "agreeing.hpp"

#include <string>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/net.hpp"
#include "tal/task.hpp"
#include "tal/testing.hpp"
#include "tal/trouble.hpp"

// What every cipher of the port does under the key agreements and the channel of sealed messages,
// whoever brings it: the toy of the trials, CNG built into tal on Windows, OpenSSL in the adapters'
// trials. The agreements run in memory, the channel over the loopback.
namespace tal::testing {

namespace {

// Both ends of one TCP connection on the loopback of `loop`.
std::pair<Stream, Stream> plain(Loop& loop) {
    const net::Listener listener = wait(loop, net::Listener::opened(net::Address::loopback(net::Family::four, 0)));
    auto [near, far] = wait(loop, all(net::connect(listener.mine()), listener.accepted()));

    return {near.stream, far.stream};
}

// Whether the agreement of the pair `calling` and `answering` ended with the keys crossed.
bool crossed(const net::Handshake& calling, const net::Handshake& answering) {
    return calling.ready() && answering.ready() && calling.keys().sending == answering.keys().receiving && calling.keys().receiving == answering.keys().sending &&
           calling.keys().sending != calling.keys().receiving && calling.keys().sending.size() == 32;
}

// Whether `work` fails as `Trouble` of the kind `kind`.
template <typename Work>
bool refused(Work work, Kind kind) {
    try {
        work();
    } catch (const Trouble& why) {
        return why.kind() == kind;
    }

    return false;
}

}

// Carries the messages of `calling` and `answering` from one to the other until neither says more.
void talked(net::Handshake& calling, net::Handshake& answering) {
    for (bool moved = true; moved;) {
        moved = false;

        while (std::optional<Bytes> one = calling.said()) {
            answering.heard(*one);
            moved = true;
        }

        while (std::optional<Bytes> one = answering.said()) {
            calling.heard(*one);
            moved = true;
        }
    }
}

// Runs every trial of the agreements and the channel on `cipher`; a failure stops at the first.
void agreed(const std::shared_ptr<crypto::Cipher>& cipher) {
    const Port<crypto::Cipher> port{cipher};
    const Bytes key = crypto::random(32);

    {
        auto calling = net::keyed(net::Side::calling, key, "trial", port);
        auto answering = net::keyed(net::Side::answering, key, "trial", port);

        talked(*calling, *answering);
        OUGHT(crossed(*calling, *answering));

        auto again = net::keyed(net::Side::calling, key, "trial", port);
        auto other = net::keyed(net::Side::answering, key, "trial", port);

        talked(*again, *other);
        OUGHT(again->keys().sending != calling->keys().sending);
    }

    {
        auto calling = net::keyed(net::Side::calling, key, "trial", port);
        auto answering = net::keyed(net::Side::answering, crypto::random(32), "trial", port);

        OUGHT(refused([&] { talked(*calling, *answering); }, Kind::denied));
    }

    {
        auto calling = net::keyed(net::Side::calling, key, "trial", port);
        auto answering = net::keyed(net::Side::answering, key, "another", port);

        OUGHT(refused([&] { talked(*calling, *answering); }, Kind::denied));
    }

#ifdef TAL_OWN_CURVE
    {
        auto calling = net::cpace(net::Side::calling, bytes("1234-5678"), "trial", port);
        auto answering = net::cpace(net::Side::answering, bytes("1234-5678"), "trial", port);

        talked(*calling, *answering);
        OUGHT(crossed(*calling, *answering));
    }

    {
        auto calling = net::cpace(net::Side::calling, bytes("1234-5678"), "trial", port);
        auto answering = net::cpace(net::Side::answering, bytes("1234-5679"), "trial", port);

        OUGHT(refused([&] { talked(*calling, *answering); }, Kind::denied));
    }
#endif

    Loop loop;
    auto [near, far] = plain(loop);
    auto [one, two] = wait(loop, all(net::Sealed::opened(near, net::keyed(net::Side::calling, key, "trial", port), port),
                                     net::Sealed::opened(far, net::keyed(net::Side::answering, key, "trial", port), port)));
    Bytes large(1 << 20);

    for (std::size_t at = 0; at < large.size(); ++at) large[at] = static_cast<std::byte>(at * 31 % 251);

    wait(loop, one.send(bytes("ping")));
    SAME(text::of(wait(loop, two.received())), std::string{"ping"});

    const auto [sent, got] = wait(loop, all(
                                            [](net::Sealed by, Bytes what) -> Task<std::size_t> {
                                                const std::size_t much = what.size();

                                                co_await by.send(std::move(what));

                                                co_return much;
                                            }(two, large),
                                            one.received()));

    SAME(sent, large.size());
    OUGHT(got == large);

    wait(loop, one.send({}));
    OUGHT(wait(loop, two.received()).empty());

    one.close();
    two.close();
}

}

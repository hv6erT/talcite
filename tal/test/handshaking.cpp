#include "handshaking.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/net.hpp"
#include "tal/testing.hpp"
#include "tal/trouble.hpp"

#include "certificates.hpp"

// What every TLS of the port does with real handshakes, whoever brings it: Schannel built into
// tal on Windows, OpenSSL in the adapters' trials. Most of it runs in memory, two sessions with the
// bytes carried by hand from one to the other, the same every time; one trial goes over the
// loopback through `secured` and `serving`. The certificates are those of `certificates.cpp`.
namespace tal::testing {

namespace {

namespace given = certificates;

// The bytes of the hexadecimal digits `text`.
Bytes unhexed(std::string_view text) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < text.size(); at += 2) made.push_back(static_cast<std::byte>(std::stoi(std::string{text.substr(at, 2)}, nullptr, 16)));

    return made;
}

// The credentials of a server with the bundle `bundle`, opened with `password`.
net::Credentials showing(std::string_view bundle, std::string password = "talcite") { return net::Credentials{unhexed(bundle), std::move(password), {}}; }

// The credentials of a client trusting `pem` alone.
net::Credentials trusting(std::string_view pem = given::authority_pem) { return net::Credentials{{}, {}, std::string{pem}}; }

// A client's session and a server's.
struct Pair {
    std::unique_ptr<net::Session> client;
    std::unique_ptr<net::Session> server;
};

// Moves what `from` has to send into `to`; whether anything moved.
bool carried(net::Session& from, net::Session& to) {
    Bytes out = from.pull();

    if (out.empty()) return false;

    to.push(std::move(out));

    return true;
}

// A client of `host` with `client` and a server with `server`, after their handshake, which ends
// when both are ready, either fails, or nothing moves any more.
Pair shook(const Making& made, net::Credentials client, net::Credentials server, const std::string& host = "talcite.test") {
    Pair one{made(std::move(client))->client(host), made(std::move(server))->server()};

    for (int turn = 0; turn < 50; ++turn) {
        const bool there = carried(*one.client, *one.server);
        const bool back = carried(*one.server, *one.client);

        if (one.client->failed() || one.server->failed()) break;

        if (one.client->ready() && one.server->ready() && !there && !back) break;
    }

    return one;
}

// The kind of trouble of `session`, if any.
std::optional<Kind> kind(const net::Session& session) {
    if (const std::optional<Trouble> why = session.failed()) return why->kind();

    return std::nullopt;
}

// The state of `session` in words: "ready", "waiting", or the kind of its trouble and what it says.
std::string told(const net::Session& session) {
    if (const std::optional<Trouble> why = session.failed()) return std::string{spoken(why->kind())} + ": " + why->what();

    return session.ready() ? "ready" : "waiting";
}

// Nothing when `session` failed with `wanted` and with a sentence that says something; otherwise
// what it did instead.
std::string refused(const net::Session& session, Kind wanted) {
    const std::optional<Trouble> why = session.failed();

    if (why.has_value() && why->kind() == wanted && std::string_view{why->what()}.size() > 3) return {};

    return "not " + std::string{spoken(wanted)} + ", " + told(session);
}

// Nothing when the pair's handshake went well on both sides; otherwise the state of each side.
std::string shaken(const Pair& one) {
    if (one.client->ready() && one.server->ready() && !one.client->failed() && !one.server->failed()) return {};

    return "client " + told(*one.client) + "; server " + told(*one.server);
}

// Sends `what` from `from` to `to` and reads it there.
std::string passed(net::Session& from, net::Session& to, std::string_view what) {
    from.write(bytes(what));

    while (carried(from, to)) {
    }

    return text::of(to.read());
}

}

// Runs every trial of the port on the adapters `made` makes; a failure stops at the first.
void handshaken(const Making& made) {
    {
        Pair one = shook(made, trusting(), showing(given::identity_hex));

        SAME(shaken(one), std::string{});
        SAME(passed(*one.client, *one.server, "ping"), std::string{"ping"});
        SAME(passed(*one.server, *one.client, "pong"), std::string{"pong"});

        Bytes large(100000);

        for (std::size_t at = 0; at < large.size(); ++at) large[at] = static_cast<std::byte>(at % 253);

        one.client->write(large);

        Bytes got;

        while (carried(*one.client, *one.server)) {
            const Bytes piece = one.server->read();

            got.insert(got.end(), piece.begin(), piece.end());
        }

        const Bytes rest = one.server->read();

        got.insert(got.end(), rest.begin(), rest.end());

        OUGHT(got == large);
        OUGHT(!one.server->closed());

        one.client->close();

        while (carried(*one.client, *one.server)) {
        }

        (void)one.server->read();

        OUGHT(one.server->closed());
        OUGHT(!one.server->failed());
    }

    SAME(shaken(shook(made, trusting(), showing(given::chained_hex))), std::string{});
    SAME(shaken(shook(made, trusting(std::string{given::stranger_pem} + std::string{given::authority_pem}), showing(given::identity_hex))), std::string{});

    SAME(refused(*shook(made, trusting(), showing(given::expired_hex)).client, Kind::denied), std::string{});
    SAME(refused(*shook(made, trusting(), showing(given::identity_hex), "other.test").client, Kind::denied), std::string{});
    SAME(refused(*shook(made, trusting(), showing(given::identity_hex), "127.0.0.1").client, Kind::denied), std::string{});
    SAME(refused(*shook(made, trusting(given::stranger_pem), showing(given::identity_hex)).client, Kind::denied), std::string{});
    SAME(refused(*shook(made, trusting(), showing(given::single_hex)).client, Kind::denied), std::string{});
    SAME(refused(*shook(made, trusting(), showing(given::foreign_hex)).client, Kind::denied), std::string{});
    SAME(refused(*shook(made, trusting(), showing(given::client_hex)).client, Kind::denied), std::string{});

    SAME(refused(*made(trusting("garbage"))->client("talcite.test"), Kind::malformed), std::string{});
    SAME(refused(*made(trusting(given::authority_pem.substr(0, 200)))->client("talcite.test"), Kind::malformed), std::string{});
    SAME(refused(*made(trusting())->client(""), Kind::other), std::string{});
    SAME(refused(*made(showing(given::identity_hex, "wrong"))->server(), Kind::denied), std::string{});
    SAME(refused(*made(showing("0102030405"))->server(), Kind::malformed), std::string{});
    SAME(refused(*made(showing(given::stranger_bare_hex, ""))->server(), Kind::malformed), std::string{});
    SAME(refused(*made(trusting())->server(), Kind::missing), std::string{});
    OUGHT(!kind(*made(trusting())->client("talcite.test")).has_value());

    Loop loop;
    const net::Listener listener = wait(loop, net::Listener::opened(net::Address::loopback(net::Family::four, 0)));
    auto [near, far] = wait(loop, all(net::connect(listener.mine()), listener.accepted()));
    auto [client, server] = wait(loop, all(net::secured(near.stream, "talcite.test", Port<net::Tls>{made(trusting())}),
                                           net::serving(far.stream, Port<net::Tls>{made(showing(given::identity_hex))})));

    wait(loop, client.write(bytes("over the loopback")));
    SAME(text::of(wait(loop, server.read(100))), std::string{"over the loopback"});

    server.close();

    SAME(wait(loop, client.read(10)).size(), std::size_t{0});
}

}

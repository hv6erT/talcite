#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/net.hpp"
#include "tal/task.hpp"
#include "tal/tls.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "certificates.hpp"
#include "handshaking.hpp"
#include "quieting.hpp"
#include "turning.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wincrypt.h>

#undef near
#undef far
#endif

// TLS of tal::net over any stream, with the session of `turning.cpp`: a greeting each way for the
// handshake, a goodbye record for the close, and a poisoned record or a refused handshake on
// demand. The adapters of the systems run their own trials with real handshakes.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::net::Address;
using tal::net::Family;
using tal::net::Listener;

using tal::testing::turned;

// Both plain ends of one TCP connection on the loopback: the one that connected, then the one
// accepted.
std::pair<tal::Stream, tal::Stream> plain(tal::Loop& loop) {
    const Listener listener = tal::wait(loop, Listener::opened(Address::loopback(Family::four, 0)));
    auto [near, far] = tal::wait(loop, tal::all(tal::net::connect(listener.mine()), listener.accepted()));

    return {near.stream, far.stream};
}

// Both secure ends over `ends`: the client's, for the server "talcite.test", and the server's.
std::pair<tal::Stream, tal::Stream> secure(tal::Loop& loop, std::pair<tal::Stream, tal::Stream> ends, tal::Port<tal::net::Tls> client = turned()) {
    auto [near, far] = tal::wait(loop, tal::all(tal::net::secured(ends.first, "talcite.test", client), tal::net::serving(ends.second, turned())));

    return {near, far};
}

// Reads from `from` until `much` bytes came or it ended.
tal::Task<Bytes> gathered(tal::Stream from, std::size_t much) {
    Bytes all;

    while (all.size() < much) {
        const Bytes piece = co_await from.read(4096);

        if (piece.empty()) break;

        all.insert(all.end(), piece.begin(), piece.end());
    }

    co_return all;
}

// The trouble a task fails with, or none.
template <typename T>
std::optional<tal::Trouble> troubled(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return why;
    }

    return std::nullopt;
}

// The kind of trouble a task fails with, as spoken, or "none".
template <typename T>
std::string kind_of(tal::Loop& loop, tal::Task<T> work) {
    const std::optional<tal::Trouble> why = troubled(loop, std::move(work));

    return why ? std::string{tal::spoken(why->kind())} : std::string{"none"};
}

// What the trouble a task fails with says, or "none".
template <typename T>
std::string said_by(tal::Loop& loop, tal::Task<T> work) {
    const std::optional<tal::Trouble> why = troubled(loop, std::move(work));

    return why ? std::string{why->what()} : std::string{"none"};
}

// A plain stream that passes everything on until `stuck`; from then on a write never ends, as a
// write to a peer that reads nothing does once every buffer between them is full.
class Sticking final : public tal::Channel {
public:
    // Passes on to `under`.
    explicit Sticking(tal::Stream under) : under_(std::move(under)) {}

    // What `under` gives.
    tal::Task<Bytes> read(std::size_t most) override { return under_.read(most); }

    // `what` to `under`, or a wait that never ends once stuck.
    tal::Task<void> write(Bytes what) override {
        if (!stuck.load()) return under_.write(std::move(what));

        return [](tal::Pending<void> never) -> tal::Task<void> { co_await never.task(); }(tal::Pending<void>{});
    }

    // Closes `under`.
    void close() override { under_.close(); }

    std::atomic<bool> stuck{false};

private:
    tal::Stream under_;
};

// Writes `what` to `into`; gives its length.
tal::Task<std::size_t> written(tal::Stream into, Bytes what) {
    const std::size_t much = what.size();

    co_await into.write(std::move(what));

    co_return much;
}

}

TRIAL("TLS: the handshake ends on both sides, and what one writes the other reads") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop));

    tal::wait(loop, near.write(tal::bytes("ping")));
    SAME(tal::text::of(tal::wait(loop, gathered(far, 4))), std::string{"ping"});

    tal::wait(loop, far.write(tal::bytes("pong")));
    SAME(tal::text::of(tal::wait(loop, gathered(near, 4))), std::string{"pong"});

    tal::wait(loop, near.write({}));
}

TRIAL("TLS: a hundred thousand bytes each way at once, in records, whole and in order") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop));
    Bytes there(100000);
    Bytes back(100000);

    for (std::size_t at = 0; at < there.size(); ++at) {
        there[at] = static_cast<std::byte>(at % 251);
        back[at] = static_cast<std::byte>(at % 241);
    }

    auto [sent, returned, got, gotten] =
        tal::wait(loop, tal::all(written(near, there), written(far, back), gathered(far, there.size()), gathered(near, back.size())));

    SAME(sent, there.size());
    SAME(returned, back.size());
    OUGHT(got == there);
    OUGHT(gotten == back);
}

TRIAL("TLS: a read smaller than a record leaves the rest to the next") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop));

    tal::wait(loop, near.write(tal::bytes("abcdefgh")));
    SAME(tal::text::of(tal::wait(loop, far.read(3))), std::string{"abc"});
    SAME(tal::text::of(tal::wait(loop, gathered(far, 5))), std::string{"defgh"});
    SAME(kind_of(loop, far.read(0)), std::string{tal::spoken(tal::Kind::other)});
}

TRIAL("TLS: our close says goodbye, and the other side reads a clean end") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop));

    tal::wait(loop, far.write(tal::bytes("last")));
    far.close();

    SAME(tal::text::of(tal::wait(loop, gathered(near, 100))), std::string{"last"});
    SAME(tal::wait(loop, near.read(10)).size(), std::size_t{0});
    SAME(said_by(loop, far.read(1)), std::string{"The connection is closed."});
    SAME(said_by(loop, far.write(tal::bytes("x"))), std::string{"The connection is closed."});
}

TRIAL("TLS: the last stream gone says goodbye too") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop));

    far = tal::Stream{};

    SAME(tal::wait(loop, near.read(10)).size(), std::size_t{0});
}

TRIAL("TLS: our close closes the stream under it too, after the goodbye") {
    tal::Loop loop;
    auto ends = plain(loop);
    auto [near, far] = secure(loop, ends);

    far.close();

    SAME(tal::wait(loop, near.read(10)).size(), std::size_t{0});
    UNTIL(loop, troubled(loop, ends.second.read(1)).has_value());
}

TRIAL("TLS: one write at a time") {
    tal::Loop loop;
    auto ends = plain(loop);
    auto sticking = std::make_shared<Sticking>(ends.second);
    auto [near, far] = secure(loop, {ends.first, tal::Stream{sticking}});
    tal::Scope scope{loop};

    sticking->stuck = true;
    scope.run([](tal::Stream into) -> tal::Task<void> { co_await into.write(tal::bytes("first")); }(far));

    QUIETED(loop);

    SAME(kind_of(loop, far.write(tal::bytes("x"))), std::string{tal::spoken(tal::Kind::busy)});
}

TRIAL("TLS: an end without goodbye is broken, not the end") {
    tal::Loop loop;
    auto ends = plain(loop);
    auto [near, far] = secure(loop, ends);

    tal::wait(loop, far.write(tal::bytes("cut")));
    ends.second.close();

    SAME(tal::text::of(tal::wait(loop, near.read(10))), std::string{"cut"});

    const std::optional<tal::Trouble> why = troubled(loop, near.read(10));

    OUGHT(why.has_value());
    SAME(why->kind(), tal::Kind::broken);
    SAME(why->where(), std::string{"talcite.test"});
}

TRIAL("TLS: one read at a time; the next is not refused once the first is cut off") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop));
    tal::Scope scope{loop};

    scope.run([](tal::Stream from) -> tal::Task<void> { (void)co_await from.read(10); }(far));

    QUIETED(loop);

    SAME(kind_of(loop, far.read(10)), std::string{tal::spoken(tal::Kind::busy)});

    scope.cancel();

    UNTIL(loop, !scope.busy());

    SAME(kind_of(loop, tal::within(30ms, far.read(10))), std::string{tal::spoken(tal::Kind::overdue)});

    tal::wait(loop, near.write(tal::bytes("still")));
    SAME(tal::text::of(tal::wait(loop, gathered(far, 5))), std::string{"still"});
}

TRIAL("TLS: a refused handshake carries its kind and the server's name") {
    tal::Loop loop;
    auto ends = plain(loop);
    const std::optional<tal::Trouble> why = troubled(loop, tal::net::secured(ends.first, "talcite.test", turned(false, tal::Kind::denied)));

    OUGHT(why.has_value());
    SAME(why->kind(), tal::Kind::denied);
    SAME(why->where(), std::string{"talcite.test"});
    SAME(std::string{why->what()}, std::string{"The handshake is refused."});
}

TRIAL("TLS: a poisoned record breaks the side that reads it") {
    tal::Loop loop;
    auto [near, far] = secure(loop, plain(loop), turned(true));

    tal::wait(loop, near.write(tal::bytes("anything")));

    const std::optional<tal::Trouble> why = troubled(loop, far.read(10));

    OUGHT(why.has_value());
    SAME(std::string{why->what()}, std::string{"Poison in the record."});
}

TRIAL("TLS: a handshake cut off halfway, or that never ends, fails") {
    tal::Loop loop;
    auto ends = plain(loop);

    ends.second.close();

    const std::optional<tal::Trouble> why = troubled(loop, tal::net::secured(ends.first, "talcite.test", turned()));

    OUGHT(why.has_value());
    SAME(why->kind(), tal::Kind::broken);

    auto silent = plain(loop);

    SAME(kind_of(loop, tal::within(50ms, tal::net::secured(silent.first, "talcite.test", turned()))), std::string{tal::spoken(tal::Kind::overdue)});
}

TRIAL("TLS: the built-in one passes the trials of the port; a build without one says so") {
    if (tal::net::Tls::builtin() == nullptr) {
        THREW(tal::Port<tal::net::Tls>{});
        OUGHT(tal::net::Tls::builtin(tal::net::Credentials{}) == nullptr);

        return;
    }

    tal::testing::handshaken([](tal::net::Credentials given) { return tal::net::Tls::builtin(std::move(given)); });
}

#ifdef _WIN32

namespace {

// How many certificates the user's store of intermediate authorities holds.
int authorities() {
    const HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"CA");
    int many = 0;

    if (store == nullptr) return -1;

    for (PCCERT_CONTEXT one = CertEnumCertificatesInStore(store, nullptr); one != nullptr; one = CertEnumCertificatesInStore(store, one)) ++many;

    (void)CertCloseStore(store, 0);

    return many;
}

// The bytes of the hexadecimal digits `text`.
Bytes unhexed(std::string_view text) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < text.size(); at += 2) made.push_back(static_cast<std::byte>(std::stoi(std::string{text.substr(at, 2)}, nullptr, 16)));

    return made;
}

}

TRIAL("TLS: Schannel lends an identity's intermediate authority to the user's store while it lives") {
    const int before = authorities();
    const tal::net::Credentials chained{unhexed(tal::testing::certificates::chained_hex), "talcite", {}};

    OUGHT(before >= 0);

    {
        const auto one = tal::net::Tls::builtin(chained);
        const int during = authorities();

        OUGHT(during == before || during == before + 1);

        {
            const auto two = tal::net::Tls::builtin(chained);

            SAME(authorities(), during);
        }

        SAME(authorities(), during);
    }

    SAME(authorities(), before);
}

#endif

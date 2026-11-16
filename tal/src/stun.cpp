#include "tal/peer.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#include "stunning.hpp"

// Our address as servers on the Internet see it, by STUN (RFC 8489), ported from talcite's
// stunclient.cpp and stunning.cpp: Binding requests and their answers, nothing of TURN. Each
// question goes from a fresh socket of its own, and a server gets the schedule of the RFC (half a
// second, then doubling) for at most `each` before the next one is asked. Only the transaction
// number tells an answer apart, and a datagram that is not STUN, or not ours, changes nothing.
namespace tal::net {

namespace stunning {

namespace {

// The magic cookie: what tells STUN from its older form, and the mask of an XOR-ed address.
constexpr std::uint32_t cookie = 0x2112A442U;

// The big-endian 16-bit number at `at` of `what`.
std::uint16_t whole(const Bytes& what, std::size_t at) {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(what[at]) << 8) | std::to_integer<unsigned>(what[at + 1]));
}

// The big-endian 32-bit number at `at` of `what`.
std::uint32_t wider(const Bytes& what, std::size_t at) { return (static_cast<std::uint32_t>(whole(what, at)) << 16) | whole(what, at + 2); }

// Appends the big-endian 16-bit `what` to `into`.
void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

// The header of a message of the type `type` with `much` bytes after it and the number `number`.
Bytes begun(std::uint16_t type, std::uint16_t much, const Number& number) {
    Bytes made;

    pushed(made, type);
    pushed(made, much);
    pushed(made, static_cast<std::uint16_t>(cookie >> 16));
    pushed(made, static_cast<std::uint16_t>(cookie & 0xFFFF));
    made.insert(made.end(), number.begin(), number.end());

    return made;
}

// The mask of an XOR-ed address of the message `number`: the cookie, then the number.
std::array<std::byte, 16> masked(const Number& number) {
    std::array<std::byte, 16> made{};

    for (std::size_t one = 0; one < 4; ++one) made[one] = static_cast<std::byte>((cookie >> (24 - 8 * one)) & 0xFF);

    for (std::size_t one = 0; one < 12; ++one) made[4 + one] = number[one];

    return made;
}

// The address of the attribute of `much` bytes at `at`, XOR-ed or plain; empty when it is not one.
Address placed(const Bytes& what, std::size_t at, std::uint16_t much, const Number& number, bool xored) {
    if (much < 4) return {};

    const unsigned family = std::to_integer<unsigned>(what[at + 1]);

    if (family != 0x01 && family != 0x02) return {};

    const std::size_t wide = family == 0x01 ? 4U : 16U;

    if (much < 4 + wide) return {};

    std::uint16_t port = whole(what, at + 2);

    if (xored) port = static_cast<std::uint16_t>(port ^ (cookie >> 16));

    const std::array<std::byte, 16> mask = masked(number);
    std::array<std::byte, 16> raw{};

    for (std::size_t one = 0; one < wide; ++one) raw[one] = xored ? what[at + 4 + one] ^ mask[one] : what[at + 4 + one];

    return Address{family == 0x01 ? Family::four : Family::six, raw, port};
}

// The text of `much` bytes at `at`, at most `most` of them, each control character a question
// mark: a stranger wrote it, and it goes on to a person's screen.
std::string spoken(const Bytes& what, std::size_t at, std::size_t much, std::size_t most) {
    std::string made;

    for (std::size_t one = 0; one < much && made.size() < most; ++one) {
        const unsigned sign = std::to_integer<unsigned>(what[at + one]);

        made += sign < 0x20 || sign == 0x7F ? '?' : static_cast<char>(sign);
    }

    return made;
}

}

// A transaction number from the system's randomness: the only proof an answer is ours, so nobody
// beside the way may guess it.
Number numbered() {
    const Bytes given = crypto::random(12);
    Number made{};

    std::copy(given.begin(), given.end(), made.begin());

    return made;
}

// A Binding request of the number `number`, without attributes.
Bytes asked(const Number& number) { return begun(0x0001, 0, number); }

// The success answer to the request `number`: the address `seen`, XOR-ed or in the old plain form.
Bytes told(const Number& number, const Address& seen, bool xored) {
    const bool four = seen.family() == Family::four;
    const std::size_t wide = four ? 4U : 16U;
    const std::array<std::byte, 16> mask = masked(number);
    Bytes value{std::byte{0}, static_cast<std::byte>(four ? 1 : 2)};

    pushed(value, xored ? static_cast<std::uint16_t>(seen.port() ^ (cookie >> 16)) : seen.port());

    for (std::size_t one = 0; one < wide; ++one) value.push_back(xored ? seen.bytes()[one] ^ mask[one] : seen.bytes()[one]);

    Bytes made = begun(0x0101, static_cast<std::uint16_t>(4 + value.size()), number);

    pushed(made, xored ? 0x0020 : 0x0001);
    pushed(made, static_cast<std::uint16_t>(value.size()));
    made.insert(made.end(), value.begin(), value.end());

    return made;
}

// The error answer to the request `number`: the code `code` and the reason `said`.
Bytes refused(const Number& number, int code, std::string_view said) {
    Bytes value{std::byte{0}, std::byte{0}, static_cast<std::byte>(code / 100), static_cast<std::byte>(code % 100)};

    value.insert(value.end(), reinterpret_cast<const std::byte*>(said.data()), reinterpret_cast<const std::byte*>(said.data()) + said.size());

    const std::size_t padded = (value.size() + 3) / 4 * 4;
    Bytes made = begun(0x0111, static_cast<std::uint16_t>(4 + padded), number);

    pushed(made, 0x0009);
    pushed(made, static_cast<std::uint16_t>(value.size()));
    made.insert(made.end(), value.begin(), value.end());
    made.resize(20 + 4 + padded, std::byte{0});

    return made;
}

// The Binding answer in `what`, or nothing when it is not STUN or not a Binding answer. Attributes
// are read within the declared length only; one broken address does not erase a good one, and the
// old MAPPED-ADDRESS counts only without XOR-MAPPED-ADDRESS.
std::optional<Heard> heard(const Bytes& what) {
    if (what.size() < 20 || (std::to_integer<unsigned>(what[0]) & 0xC0) != 0 || wider(what, 4) != cookie) return std::nullopt;

    const std::uint16_t type = whole(what, 0);
    const std::size_t end = 20U + whole(what, 2);

    if (end > what.size() || (type != 0x0101 && type != 0x0111)) return std::nullopt;

    Heard made;
    bool xored = false;

    std::copy(what.begin() + 8, what.begin() + 20, made.number.begin());
    made.answer = type == 0x0101;
    made.error = type == 0x0111;

    for (std::size_t at = 20; at + 4 <= end;) {
        const std::uint16_t kind = whole(what, at);
        const std::uint16_t wide = whole(what, at + 2);

        at += 4;

        if (at + wide > end) break;

        if (kind == 0x0020) {
            if (const Address one = placed(what, at, wide, made.number, true); one.held()) {
                made.seen = one;
                xored = true;
            }
        } else if (kind == 0x0001 && !xored) {
            if (const Address one = placed(what, at, wide, made.number, false); one.held()) made.seen = one;
        } else if (kind == 0x0009 && wide >= 4) {
            made.code = static_cast<int>((std::to_integer<unsigned>(what[at + 2]) & 0x07) * 100 + std::to_integer<unsigned>(what[at + 3]) % 100);
            made.said = spoken(what, at + 4, wide - 4U, 128);
        }

        at += wide + (4 - wide % 4) % 4;
    }

    return made;
}

}

namespace {

using stunning::Number;

// The address the server `where` sees for `post`, asked by the schedule of the RFC within `each`;
// nothing when it stays silent. Throws `Trouble` (refused) when it refuses.
Task<std::optional<Address>> trying(Udp post, Address where, std::chrono::milliseconds each) {
    const Number number = stunning::numbered();
    const auto until = std::chrono::steady_clock::now() + each;
    std::chrono::milliseconds wait{500};

    while (std::chrono::steady_clock::now() < until) {
        try {
            co_await post.send(where, stunning::asked(number));
        } catch (const Trouble&) {
        }

        const auto edge = std::min(std::chrono::steady_clock::now() + wait, until);

        for (auto now = std::chrono::steady_clock::now(); now < edge; now = std::chrono::steady_clock::now()) {
            Datagram came;

            try {
                came = co_await within(std::chrono::duration_cast<std::chrono::milliseconds>(edge - now) + std::chrono::milliseconds{1}, post.received(1500));
            } catch (const Overdue&) {
                break;
            }

            const std::optional<stunning::Heard> one = came.cut ? std::nullopt : stunning::heard(came.bytes);

            if (!one || one->number != number) continue;

            if (one->error)
                throw Trouble{Kind::refused, "The STUN server refused: " + (one->said.empty() ? std::to_string(one->code) : one->said) + ".", where.spelled(),
                              one->code};

            if (one->seen.held()) co_return one->seen;
        }

        wait *= 2;
    }

    co_return std::nullopt;
}

// A fresh socket for asking servers of the family `family`.
Task<Udp> posted(Family family) { co_return co_await Udp::bound(Address::any(family, 0)); }

}

// A scout of `servers`, each given `each` before the next is asked.
Stun::Stun(std::vector<Address> servers, std::chrono::milliseconds each) : servers_(std::move(servers)), each_(each) {}

// Our address as the first server that answers sees it. The first refusal comes out when every
// server failed so, and overdue when none answered at all.
Task<Address> Stun::seen() const {
    if (servers_.empty()) throw Trouble{Kind::other, "The scout knows no server."};

    const std::vector<Address> servers = servers_;
    const std::chrono::milliseconds each = each_;
    std::exception_ptr first;

    for (const Address& one : servers) {
        try {
            const Udp post = co_await posted(one.family());

            if (const std::optional<Address> made = co_await trying(post, one, each)) co_return *made;
        } catch (const Trouble&) {
            if (!first) first = std::current_exception();
        }
    }

    if (first) std::rethrow_exception(first);

    throw Trouble{Kind::overdue, "No STUN server answered."};
}

// Whether two different servers see one socket at the same address: alike when they do, so that
// the way from outside does not depend on who asks; symmetric when they do not. Unknown without
// two different servers of one family, or when either is silent.
Task<Mapping> Stun::mapping() const {
    if (servers_.empty()) co_return Mapping::unknown;

    const Address first = servers_.front();
    const std::chrono::milliseconds each = each_;
    std::optional<Address> second;

    for (const Address& one : servers_)
        if (one != first && one.family() == first.family()) {
            second = one;

            break;
        }

    if (!second) co_return Mapping::unknown;

    const Udp post = co_await posted(first.family());
    const std::optional<Address> one = co_await trying(post, first, each);
    const std::optional<Address> two = co_await trying(post, *second, each);

    if (!one || !two) co_return Mapping::unknown;

    co_return *one == *two ? Mapping::alike : Mapping::symmetric;
}

}

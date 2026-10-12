#include "proving.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "talcite/address.hpp"

using namespace talcite;

TRIAL("Address: a fresh one holds nothing") {
    const net::Address one;

    ought(!one.held());
    same(one.family(), net::Family::none);
    same(one.port(), static_cast<std::uint16_t>(0));
    same(one.text(), std::string{});
    same(one.spelled(), std::string{});
}

TRIAL("Address: IPv4 parses and formats back") {
    const net::Address one{"192.0.2.17", 8080};

    ought(one.held());
    same(one.family(), net::Family::four);
    same(one.port(), static_cast<std::uint16_t>(8080));
    same(one.text(), std::string{"192.0.2.17"});
    same(one.spelled(), std::string{"192.0.2.17:8080"});
}

TRIAL("Address: IPv6 goes in brackets only with a port") {
    const net::Address one{"2001:db8::1", 443};

    same(one.family(), net::Family::six);
    same(one.text(), std::string{"2001:db8::1"});
    same(one.spelled(), std::string{"[2001:db8::1]:443"});
}

TRIAL("Address: square brackets may be given") {
    const net::Address one{"[::1]", 22};

    same(one.family(), net::Family::six);
    same(one.text(), std::string{"::1"});
    same(one.spelled(), std::string{"[::1]:22"});
}

TRIAL("Address: the scope id survives parsing and formatting") {
    const net::Address one{"fe80::1%7", 100};

    same(one.family(), net::Family::six);
    same(one.scope(), static_cast<std::uint32_t>(7));
    same(one.text(), std::string{"fe80::1%7"});
    same(one.spelled(), std::string{"[fe80::1%7]:100"});
}

TRIAL("Address: a domain name is not an address") {
    bool threw = false;

    try {
        const net::Address one{"przyklad.pl", 80};

        (void)one;
    } catch (const net::Trouble& why) {
        threw = true;

        ought(std::string{why.what()}.find("przyklad.pl") != std::string::npos);
    }

    ought(threw);
}

TRIAL("Address: garbage does not silently become an empty address") {
    bool threw = false;

    try {
        const net::Address one{"999.1.2.3", 80};

        (void)one;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Address: any and loopback") {
    const net::Address four = net::Address::any(net::Family::four, 1234);
    const net::Address six = net::Address::any(net::Family::six, 1234);

    same(four.text(), std::string{"0.0.0.0"});
    same(six.text(), std::string{"::"});

    same(net::Address::loopback(net::Family::four, 80).text(), std::string{"127.0.0.1"});
    same(net::Address::loopback(net::Family::six, 80).text(), std::string{"::1"});
}

TRIAL("Address: comparison looks at the bytes, family and port") {
    const net::Address one{"192.0.2.1", 80};
    const net::Address two{"192.0.2.1", 80};
    const net::Address three{"192.0.2.1", 81};
    const net::Address four{"192.0.2.2", 80};

    ought(one == two);
    ought(!(one == three));
    ought(!(one == four));
}

TRIAL("Address: the port can be changed afterwards") {
    net::Address one{"::1", 0};

    one.port(9000);

    same(one.spelled(), std::string{"[::1]:9000"});
}

TRIAL("Address: can be built from raw bytes") {
    std::array<std::byte, 16> raw{};

    raw[0] = std::byte{10};
    raw[3] = std::byte{7};

    const net::Address one{net::Family::four, raw, 25};

    same(one.text(), std::string{"10.0.0.7"});
    same(one.port(), static_cast<std::uint16_t>(25));
}

TRIAL("Names: loopback has an address") {
    const std::vector<net::Address> made = net::resolve("localhost", 80).get();

    ought(!made.empty());

    for (const net::Address& one : made) {
        ought(one.held());
        same(one.port(), static_cast<std::uint16_t>(80));
    }
}

TRIAL("Names: an address written directly passes too") {
    const std::vector<net::Address> made = net::resolve("127.0.0.1", 443).get();

    same(static_cast<int>(made.size()), 1);
    same(made.front().text(), std::string{"127.0.0.1"});
    same(made.front().port(), static_cast<std::uint16_t>(443));
}

TRIAL("Names: one family can be requested") {
    const std::vector<net::Address> made =
        net::resolve("localhost", 80, net::Family::four).get();

    for (const net::Address& one : made) same(one.family(), net::Family::four);
}

TRIAL("Names: an unknown name rejects the promise") {
    bool threw = false;

    try {
        (void)net::resolve("nie.ma.takiej.nazwy.talcite.test", 80).get();
    } catch (const net::Trouble& why) {
        threw = true;

        ought(why.code() != 0);
    }

    ought(threw);
}

TRIAL("Own: the list is not empty and has loopback") {
    const std::vector<net::Address> made = net::mine().get();

    ought(!made.empty());

    bool loopback = false;

    for (const net::Address& one : made) {
        ought(one.held());

        if (one == net::Address::loopback(one.family(), 0)) loopback = true;
    }

    ought(loopback);
}

TRIAL("Own: the list has no ports") {
    for (const net::Address& one : net::mine().get())
        same(one.port(), static_cast<std::uint16_t>(0));
}

TRIAL("Own: one family can be requested") {
    const std::vector<net::Address> made = net::mine(net::Family::four).get();

    ought(!made.empty());

    for (const net::Address& one : made) same(one.family(), net::Family::four);
}

TRIAL("Own: the same address does not appear twice") {
    const std::vector<net::Address> made = net::mine().get();

    for (std::size_t at = 0; at < made.size(); ++at)
        for (std::size_t which = at + 1; which < made.size(); ++which)
            ought(!(made[at] == made[which]));
}

TRIAL("Own: every address can be written and read back") {
    for (const net::Address& one : net::mine().get()) {
        // A link-local address carries the scope id after the percent sign, and it comes back only
        // on systems that name it, so only the address itself is compared, without the id.
        const net::Address again{one.text(), one.port()};

        same(again.family(), one.family());
        same(again.bytes(), one.bytes());
    }
}

// The form we write ourselves must read back. The brackets have to come off before the split at
// the percent sign, otherwise the closing bracket ends up in the text of the scope id.
TRIAL("Address: brackets together with a scope id") {
    const net::Address made{"[fe80::1%1]", 546};

    same(made.family(), net::Family::six);
    same(made.port(), static_cast<std::uint16_t>(546));
    same(made.scope(), 1u);

    const net::Address again{made.text(), made.port()};

    same(again.bytes(), made.bytes());
}

// A zero in the middle must not silently cut the input, or "1.2.3.4" glued to garbage would be
// built successfully.
TRIAL("Address: a zero inside the text does not cut the input") {
    bool threw = false;

    try {
        const net::Address made{std::string_view{"1.2.3.4\0bzdura", 14}, 80};

        (void)made;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

// An unknown interface name must not quietly become zero: a link-local address without a scope
// id is useless, and the error would show up only when connecting, far from the typo.
TRIAL("Address: an unknown interface name is a trouble, not zero") {
    bool threw = false;

    try {
        const net::Address made{"fe80::1%takiejniema", 80};

        (void)made;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

// A scope id belongs only to link-local addresses. Attached to all of them, it would turn
// loopback into ::1%1, which no longer equals ::1 and would come out as a candidate for hole
// punching.
TRIAL("Own: a scope id only on link-local addresses") {
    for (const net::Address& one : net::mine().get()) {
        if (one.family() != net::Family::six) continue;

        if (one.scope() == 0) continue;

        const unsigned first = std::to_integer<unsigned>(one.bytes()[0]);
        const unsigned second = std::to_integer<unsigned>(one.bytes()[1]);

        ought(first == 0xFE && (second & 0xC0) == 0x80);
    }
}

// --- local socket ------------------------------------------------------------------------------

TRIAL("Local address: a path instead of an address and port") {
    const net::Address one = net::Address::local("/tmp/talcite-proba.sock");

    ought(one.held());
    same(one.family(), net::Family::local);
    same(one.path(), std::string{"/tmp/talcite-proba.sock"});
    same(one.port(), static_cast<std::uint16_t>(0));
    same(one.scope(), 0u);

    // Neither text() nor spelled() appends a port: a local socket has none, and "…sock:0" would
    // be a lie about what this address is.
    same(one.text(), std::string{"/tmp/talcite-proba.sock"});
    same(one.spelled(), std::string{"/tmp/talcite-proba.sock"});
}

TRIAL("Local address: equality goes by path") {
    const net::Address one = net::Address::local("/tmp/a.sock");
    const net::Address two = net::Address::local("/tmp/a.sock");
    const net::Address three = net::Address::local("/tmp/b.sock");

    ought(one == two);
    ought(!(one == three));
}

TRIAL("Local address: an empty path is refused") {
    bool told = false;

    try {
        (void)net::Address::local("");
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

TRIAL("Local address: a zero inside the path is refused") {
    bool told = false;

    try {
        (void)net::Address::local(std::string{"/tmp/a\0b.sock", 13});
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

TRIAL("Local address: a too long path is refused when built, not at the socket") {
    bool told = false;

    try {
        (void)net::Address::local("/tmp/" + std::string(120, 'x'));
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}

TRIAL("Local address: port and scope id are refused") {
    net::Address one = net::Address::local("/tmp/a.sock");

    bool port = false;
    bool scope = false;

    try {
        one.port(80);
    } catch (const net::Trouble&) {
        port = true;
    }

    try {
        one.scope(3);
    } catch (const net::Trouble&) {
        scope = true;
    }

    ought(port);
    ought(scope);

    // A refusal leaves no half-changed state behind.
    same(one.port(), static_cast<std::uint16_t>(0));
    same(one.scope(), 0u);
}

TRIAL("Local address: none of the other ways makes one") {
    const std::array<std::byte, 16> raw{};

    bool bytes = false;
    bool any = false;
    bool loop = false;

    try {
        (void)net::Address{net::Family::local, raw, 0};
    } catch (const net::Trouble&) {
        bytes = true;
    }

    try {
        (void)net::Address::any(net::Family::local, 0);
    } catch (const net::Trouble&) {
        any = true;
    }

    try {
        (void)net::Address::loopback(net::Family::local, 0);
    } catch (const net::Trouble&) {
        loop = true;
    }

    ought(bytes);
    ought(any);
    ought(loop);
}

TRIAL("Address: IPv4 from a dual-stack socket comes back as IPv4, the rest stays") {
    const net::Address mapped = net::Address{"::ffff:192.0.2.7", 4242}.unmapped();

    same(mapped.family(), net::Family::four);
    same(mapped.spelled(), std::string{"192.0.2.7:4242"});

    for (const char* other : {"::fffe:192.0.2.7", "::1:ffff:192.0.2.7", "1::ffff:c000:207", "2001:db8::1", "::"}) {
        const net::Address one{other, 80};

        ought(one.unmapped() == one);
    }

    const net::Address four{"192.0.2.7", 80};

    ought(four.unmapped() == four);
    ought(net::Address{}.unmapped() == net::Address{});
}

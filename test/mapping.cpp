#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/listener.hpp"
#include "talcite/router.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"
#include "talcite/udp.hpp"
#include "talcite/xml.hpp"

#include "routing.hpp"

using namespace talcite;

// **Port otwierany na routerze** — PCP, NAT-PMP i UPnP IGD przeciw routerowi z próby: gniazdo UDP
// na pętli zwrotnej udaje port 5351, drugie — SSDP, a brama TCP — serwer opisu i SOAP. Próba sama
// czyta prośby bajt w bajt i pisze odpowiedzi, więc widać dokładnie, co idzie do routera.
//
// XML czyta atrapa, która **nie parsuje** i nie udaje, że to robi: zna kilka ustalonych treści
// (`OPIS`, `DODANO`…) i oddaje za nie drzewa zbudowane tutaj. Prawdziwy XML z prawdziwego routera
// przechodzi przez pugixml w próbie talcite-adapters.
namespace {

const net::Patience soon{6000};

using Clock = std::chrono::steady_clock;

xml::Node element(std::string name, std::string text = {}) {
    xml::Node made;

    made.name = std::move(name);

    if (!text.empty()) xml::text(made, std::move(text));

    return made;
}

xml::Node with(xml::Node one, std::vector<xml::Node> children) {
    for (xml::Node& each : children) one.children.push_back(std::move(each));

    return one;
}

xml::Document document(xml::Node top) {
    xml::Document made;

    made.nodes.push_back(std::move(top));

    return made;
}

xml::Node service(std::string type, std::string control) {
    return with(element("service"), {element("serviceType", std::move(type)), element("controlURL", std::move(control))});
}

// Opis jak u typowego routera: urządzenie bramy, w nim urządzenie WAN, w nim połączenie WAN —
// a obok usługa, która portów nie otwiera.
xml::Document described(std::string control, std::string type = "urn:schemas-upnp-org:service:WANIPConnection:1") {
    xml::Node connection = with(element("device"), {with(element("serviceList"), {service(std::move(type), std::move(control))})});
    xml::Node wan =
        with(element("device"), {with(element("serviceList"), {service("urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1",
                                                                       "/ctl/CmnIfCfg")}),
                                 with(element("deviceList"), {std::move(connection)})});

    return document(with(element("root"), {with(element("device"), {element("deviceType", "urn:schemas-upnp-org:device:InternetGatewayDevice:1"),
                                                                    with(element("deviceList"), {std::move(wan)})})}));
}

xml::Document fault(std::string code, std::string description) {
    return document(with(element("s:Envelope"),
                         {with(element("s:Body"),
                               {with(element("s:Fault"),
                                     {element("faultcode", "s:Client"),
                                      with(element("detail"), {with(element("UPnPError"), {element("errorCode", std::move(code)),
                                                                                           element("errorDescription", std::move(description))})})})})}));
}

class Xml : public Codec<xml::Document> {
public:
    std::map<std::string, xml::Document> known;

    [[nodiscard]] std::string_view name() const override { return "fake xml"; }

    [[nodiscard]] std::string decode(const Bytes& given, xml::Document& into) override {
        const auto found = known.find(text(given));

        if (found == known.end()) return "the fake does not know this document";

        into = found->second;

        return {};
    }

    [[nodiscard]] std::string encode(const xml::Document&, Bytes&) override { return "the fake does not write"; }
};

std::shared_ptr<Xml> fake_xml(const std::string& control = "/ctl/IPConn") {
    auto made = std::make_shared<Xml>();

    made->known["OPIS"] = described(control);
    made->known["DODANO"] = document(with(element("s:Envelope"), {with(element("s:Body"), {element("u:AddPortMappingResponse")})}));
    made->known["ADRES"] = document(with(element("s:Envelope"),
                                         {with(element("s:Body"), {with(element("u:GetExternalIPAddressResponse"),
                                                                        {element("NewExternalIPAddress", "198.51.100.9")})})}));
    made->known["USUNIETO"] = document(with(element("s:Envelope"), {with(element("s:Body"), {element("u:DeletePortMappingResponse")})}));
    made->known["ZAJETY"] = fault("718", "ConflictInMappingEntry");
    made->known["TRWALE"] = fault("725", "OnlyPermanentLeasesSupported");
    made->known["NIE"] = fault("606", "Action not authorized");

    return made;
}

// Router z próby: port 5351 (PCP i NAT-PMP) i port SSDP — oba na pętli zwrotnej.
struct Fake {
    net::Udp pcp{net::Address::loopback(net::Family::four, 0)};
    net::Udp ssdp{net::Address::loopback(net::Family::four, 0)};

    net::Routing routing(Seam<Codec<xml::Document>> xml = {}) const {
        net::Routing made;

        made.gateway = pcp.mine();
        made.mine = net::Address{"127.0.0.1", 0};
        made.discovery = ssdp.mine();
        made.xml = std::move(xml);
        made.label = "twa <próba>";
        made.patience = net::Patience{12000};

        return made;
    }

    net::Datagram heard(net::Patience patience = soon) const { return pcp.receive(1500, patience).await(); }

    void told(const net::Address& to, const Bytes& what) const { pcp.send(to, what, soon).await(); }
};

std::uint32_t got16(const Bytes& from, std::size_t at) {
    return (std::to_integer<std::uint32_t>(from[at]) << 8) | std::to_integer<std::uint32_t>(from[at + 1]);
}

std::uint32_t got32(const Bytes& from, std::size_t at) { return (got16(from, at) << 16) | got16(from, at + 2); }

void put16(Bytes& into, std::uint32_t value) {
    into.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    into.push_back(static_cast<std::byte>(value & 0xff));
}

void put32(Bytes& into, std::uint32_t value) {
    put16(into, value >> 16);
    put16(into, value & 0xffff);
}

Bytes mapped_four(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    Bytes made(10);

    made.push_back(std::byte{0xff});
    made.push_back(std::byte{0xff});

    for (const std::uint8_t one : {a, b, c, d}) made.push_back(static_cast<std::byte>(one));

    return made;
}

Bytes slice(const Bytes& from, std::size_t at, std::size_t much) {
    return Bytes(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + much));
}

// Odpowiedź PCP na prośbę MAP: nagłówek odpowiedzi i to samo MAP z przydzielonym portem i adresem.
Bytes pcp_answer(const Bytes& asked, std::uint32_t result, std::uint32_t lifetime, std::uint32_t port, const Bytes& address) {
    Bytes made;

    made.push_back(std::byte{2});
    made.push_back(std::byte{0x81});
    made.push_back(std::byte{0});
    made.push_back(static_cast<std::byte>(result));
    put32(made, lifetime);
    put32(made, 1234);

    for (int at = 0; at < 12; ++at) made.push_back(std::byte{0});

    const Bytes map = slice(asked, 24, 16);

    made.insert(made.end(), map.begin(), map.end());
    put16(made, got16(asked, 40));
    put16(made, port);
    made.insert(made.end(), address.begin(), address.end());

    return made;
}

Bytes pmp_answer(std::uint32_t op, std::uint32_t result, std::uint32_t inside, std::uint32_t outside, std::uint32_t lifetime) {
    Bytes made;

    made.push_back(std::byte{0});
    made.push_back(static_cast<std::byte>(128 + op));
    put16(made, result);
    put32(made, 77);
    put16(made, inside);
    put16(made, outside);
    put32(made, lifetime);

    return made;
}

// Strona serwera HTTP z próby: jedno połączenie, głowa i treść żądania, odpowiedź.
struct Desk {
    net::Stream plain;
    std::string kept;

    std::string asked() {
        std::string head;

        for (;;) {
            if (const std::size_t at = kept.find("\r\n\r\n"); at != std::string::npos) {
                head = kept.substr(0, at + 4);
                kept.erase(0, at + 4);

                break;
            }

            const Bytes got = plain.read(4096, soon).await();

            if (got.empty()) return {};

            kept += text(got);
        }

        std::size_t much = 0;

        if (const std::size_t at = head.find("Content-Length: "); at != std::string::npos)
            much = std::stoul(head.substr(at + 16, head.find('\r', at) - at - 16));

        while (kept.size() < much) {
            const Bytes got = plain.read(4096, soon).await();

            if (got.empty()) break;

            kept += text(got);
        }

        std::string made = head + kept.substr(0, much);

        kept.erase(0, much);

        return made;
    }

    void answered(int status, const std::string& body) {
        plain.write(bytes("HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: text/xml\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body),
                    soon)
            .await();
        plain.close();
    }
};

Desk accepted(const net::Listener& gate) { return Desk{gate.accept(soon).await(), {}}; }

bool has(const std::string& in, const std::string& what) { return in.find(what) != std::string::npos; }

std::string port_of(const net::Listener& gate) { return std::to_string(gate.mine().port()); }

// Odpowiedź na pierwsze M-SEARCH: opis leży pod podanym adresem.
std::string searched(const Fake& fake, const net::Listener& gate, const std::string& path = "/rootDesc.xml") {
    const net::Datagram came = fake.ssdp.receive(1500, soon).await();
    const std::string said = text(came.bytes);

    fake.ssdp
        .send(came.from,
              bytes("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                    "location: http://127.0.0.1:" +
                    port_of(gate) + path + "\r\n\r\n"),
              soon)
        .await();

    return said;
}

}

TRIAL("Router: trasa domyślna z tablicy Linuksa") {
    const std::string table =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "eth0\t000200C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
        "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
        "eth0\t00000000\t010200C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
        "tun0\t00000000\t00000000\t0001\t0\t0\t50\t00000000\t0\t0\t0\n";

    const std::optional<net::Address> made = net::routed(table);

    ought(made.has_value());
    same(made->text(), std::string{"192.0.2.1"});

    ought(!net::routed("Iface\tDestination\tGateway\n").has_value());
    ought(!net::routed("").has_value());
    ought(!net::routed("Iface\nwlan0\t00000000\tZZZZ\t0003\t0\t0\t0\t00000000\n").has_value());
    ought(!net::routed("Iface\nwlan0\t00000000\t0101A8C0\t0003\t0\t0\tx\t00000000\n").has_value());
    ought(!net::routed("Iface\nwlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00FFFFFF\n").has_value());
}

// Brama tej maszyny zależy od sieci, na której próba biegnie — sprawdzamy tylko, że odpowiedź to
// adres IPv4 albo czysta odmowa, i mówimy, co wyszło.
TRIAL("Router: brama tego komputera — adres IPv4 albo odmowa") {
    try {
        const net::Address one = net::gateway().await();

        ought(one.family() == net::Family::four);
        same(one.port(), static_cast<std::uint16_t>(0));

        std::cout << "        brama: " << one.text() << "\n";
    } catch (const Trouble& why) {
        std::cout << "        bez bramy: " << why.what() << "\n";
    }
}

TRIAL("Router: adres, którym wychodzimy w stronę celu") {
    same(net::leaving(net::Address::loopback(net::Family::four, 0)).await().text(), std::string{"127.0.0.1"});
    same(net::leaving(net::Address::loopback(net::Family::four, 53)).await().port(), static_cast<std::uint16_t>(0));

    threw(net::leaving(net::Address{}).await());
}

TRIAL("Router: pusty, port zero i brak bramy to odmowa") {
    threw(net::Router{}.open(net::Carrier::tcp, 4000, std::chrono::seconds{60}).await());
    threw(net::Router{}.close(net::Opening{}).await());

    const Fake fake;

    threw(net::Router{fake.routing()}.open(net::Carrier::tcp, 0, std::chrono::seconds{60}).await());

    net::Routing none = fake.routing();

    none.gateway = {};

    threw(net::Router{none}.open(net::Carrier::tcp, 4000, std::chrono::seconds{60}).await());
}

// RFC 6887: nagłówek (wersja 2, MAP, czas życia, nasz adres jako ::ffff:a.b.c.d) i MAP (nonce,
// protokół, port wewnętrzny, proponowany port i adres zewnętrzny — „dowolny" to ::ffff:0.0.0.0).
TRIAL("Router: PCP otwiera port — prośba MAP bajt w bajt, odpowiedź daje adres i czas") {
    const Fake fake;
    const net::Router router{fake.routing()};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    const net::Datagram came = fake.heard();
    const Bytes& asked = came.bytes;

    same(asked.size(), static_cast<std::size_t>(60));
    same(std::to_integer<int>(asked[0]), 2);
    same(std::to_integer<int>(asked[1]), 1);
    same(got16(asked, 2), 0u);
    same(got32(asked, 4), 3600u);
    ought(slice(asked, 8, 16) == mapped_four(127, 0, 0, 1));
    same(std::to_integer<int>(asked[36]), 6);
    ought(slice(asked, 37, 3) == Bytes(3));
    same(got16(asked, 40), 4000u);
    same(got16(asked, 42), 4000u);
    ought(slice(asked, 44, 16) == mapped_four(0, 0, 0, 0));

    fake.told(came.from, pcp_answer(asked, 0, 1800, 4001, mapped_four(203, 0, 113, 7)));

    const net::Opening one = opening.await();

    ought(one.by == net::Opener::pcp);
    ought(one.carrier == net::Carrier::tcp);
    same(one.inside, static_cast<std::uint16_t>(4000));
    same(one.outside.text(), std::string{"203.0.113.7"});
    same(one.outside.port(), static_cast<std::uint16_t>(4001));
    same(static_cast<long long>(one.lasting.count()), static_cast<long long>(1800));
}

// Odnowienie i zamknięcie niosą **ten sam nonce** — po nim router poznaje swoje mapowanie.
// Zamknięcie to czas życia zero; po nim nonce znika, a drugie zamknięcie to odmowa.
TRIAL("Router: PCP odnawia i zamyka tym samym nonce, UDP osobnym") {
    const Fake fake;
    const net::Router router{fake.routing()};

    auto first = router.open(net::Carrier::udp, 5000, std::chrono::seconds{600});
    const net::Datagram one = fake.heard();

    same(std::to_integer<int>(one.bytes[36]), 17);
    fake.told(one.from, pcp_answer(one.bytes, 0, 600, 5000, mapped_four(203, 0, 113, 7)));

    const net::Opening opened = first.await();

    ought(opened.carrier == net::Carrier::udp);

    auto second = router.open(net::Carrier::udp, 5000, std::chrono::seconds{600});
    const net::Datagram two = fake.heard();

    ought(slice(two.bytes, 24, 12) == slice(one.bytes, 24, 12));
    fake.told(two.from, pcp_answer(two.bytes, 0, 600, 5000, mapped_four(203, 0, 113, 7)));
    (void)second.await();

    auto other = router.open(net::Carrier::tcp, 5000, std::chrono::seconds{600});
    const net::Datagram three = fake.heard();

    ought(!(slice(three.bytes, 24, 12) == slice(one.bytes, 24, 12)));
    fake.told(three.from, pcp_answer(three.bytes, 0, 600, 5000, mapped_four(203, 0, 113, 7)));
    (void)other.await();

    auto closing = router.close(opened);
    const net::Datagram four = fake.heard();

    same(got32(four.bytes, 4), 0u);
    same(got16(four.bytes, 42), 0u);
    ought(slice(four.bytes, 24, 12) == slice(one.bytes, 24, 12));
    fake.told(four.from, pcp_answer(four.bytes, 0, 0, 0, mapped_four(0, 0, 0, 0)));
    closing.await();

    // Drugie zamknięcie odmawia od razu — router nie zna już tego mapowania, więc nie ma o co pytać.
    const auto started = Clock::now();

    threw(router.close(opened).await());

    ought(Clock::now() - started < std::chrono::seconds{2});
}

TRIAL("Router: PCP powtarza prośbę i pomija cudze odpowiedzi") {
    const Fake fake;
    const net::Udp stranger{net::Address::loopback(net::Family::four, 0)};
    const net::Router router{fake.routing()};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{60});

    const net::Datagram first = fake.heard();
    const net::Datagram again = fake.heard();

    ought(first.bytes == again.bytes);

    Bytes wrong_nonce = pcp_answer(again.bytes, 0, 60, 1, mapped_four(1, 1, 1, 1));

    wrong_nonce[24] ^= std::byte{1};

    fake.told(again.from, wrong_nonce);
    stranger.send(again.from, pcp_answer(again.bytes, 0, 60, 2, mapped_four(2, 2, 2, 2)), soon).await();
    fake.told(again.from, Bytes{std::byte{2}, std::byte{0x81}});
    // Własna prośba odbita z powrotem (bez bitu odpowiedzi) i odpowiedź o cudzy port — też nie.
    fake.told(again.from, again.bytes);

    Bytes other_port = pcp_answer(again.bytes, 0, 60, 3, mapped_four(3, 3, 3, 3));

    other_port[41] ^= std::byte{1};

    fake.told(again.from, other_port);

    Bytes other_protocol = pcp_answer(again.bytes, 0, 60, 3, mapped_four(3, 3, 3, 3));

    other_protocol[36] = std::byte{17};

    fake.told(again.from, other_protocol);
    fake.told(again.from, pcp_answer(again.bytes, 0, 60, 4000, mapped_four(203, 0, 113, 7)));

    const net::Opening one = opening.await();

    same(one.outside.port(), static_cast<std::uint16_t>(4000));
    same(one.outside.text(), std::string{"203.0.113.7"});
}

TRIAL("Router: odmowa PCP wraca z kodem, gdy innej drogi nie ma") {
    const Fake fake;
    const net::Router router{fake.routing()};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{60});
    const net::Datagram came = fake.heard();

    fake.told(came.from, pcp_answer(came.bytes, 2, 0, 0, mapped_four(0, 0, 0, 0)));

    try {
        (void)opening.await();
        ought(false);
    } catch (const Trouble& why) {
        same(why.code(), 2);
        ought(has(why.what(), "PCP: not authorized"));
    }
}

// RFC 6886: router tylko z NAT-PMP odpowiada na PCP „nieznaną wersją" w swoim formacie — wtedy
// mapowanie (12 bajtów) i pytanie o adres zewnętrzny (2 bajty). Następne otwarcie idzie od razu
// drogą, która zadziałała.
TRIAL("Router: NAT-PMP, gdy router nie zna PCP, i potem już od razu") {
    const Fake fake;
    const net::Router router{fake.routing()};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{7200});

    const net::Datagram pcp = fake.heard();

    same(std::to_integer<int>(pcp.bytes[0]), 2);
    fake.told(pcp.from, Bytes{std::byte{0}, std::byte{0x81}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0},
                              std::byte{0}, std::byte{9}});

    const net::Datagram map = fake.heard();

    same(map.bytes.size(), static_cast<std::size_t>(12));
    same(std::to_integer<int>(map.bytes[0]), 0);
    same(std::to_integer<int>(map.bytes[1]), 2);
    same(got16(map.bytes, 4), 4000u);
    same(got16(map.bytes, 6), 4000u);
    same(got32(map.bytes, 8), 7200u);

    // Odpowiedź na inną prośbę (UDP zamiast TCP) i o inny port wewnętrzny — pomijane.
    fake.told(map.from, pmp_answer(1, 0, 4000, 4009, 3600));
    fake.told(map.from, pmp_answer(2, 0, 4001, 4009, 3600));
    fake.told(map.from, pmp_answer(2, 0, 4000, 4002, 3600));

    const net::Datagram address = fake.heard();

    ought(address.bytes == Bytes(2));

    Bytes told{std::byte{0}, std::byte{128}, std::byte{0}, std::byte{0}};

    put32(told, 77);
    told.push_back(std::byte{198});
    told.push_back(std::byte{51});
    told.push_back(std::byte{100});
    told.push_back(std::byte{4});

    fake.told(address.from, told);

    const net::Opening one = opening.await();

    ought(one.by == net::Opener::pmp);
    same(one.outside.text(), std::string{"198.51.100.4"});
    same(one.outside.port(), static_cast<std::uint16_t>(4002));
    same(static_cast<long long>(one.lasting.count()), static_cast<long long>(3600));

    auto renewal = router.open(net::Carrier::udp, 4000, std::chrono::seconds{7200});
    const net::Datagram straight = fake.heard();

    same(std::to_integer<int>(straight.bytes[0]), 0);
    same(std::to_integer<int>(straight.bytes[1]), 1);
    fake.told(straight.from, pmp_answer(1, 0, 4000, 4002, 3600));

    const net::Datagram again = fake.heard();

    fake.told(again.from, told);
    ought(renewal.await().by == net::Opener::pmp);

    auto closing = router.close(one);
    const net::Datagram end = fake.heard();

    same(got16(end.bytes, 6), 0u);
    same(got32(end.bytes, 8), 0u);
    // Zamknięcie nie pyta o adres zewnętrzny — kończy się zaraz po odpowiedzi.
    const auto answered = Clock::now();

    fake.told(end.from, pmp_answer(2, 0, 4000, 0, 0));
    closing.await();

    ought(Clock::now() - answered < std::chrono::milliseconds{1000});
}

TRIAL("Router: odmowa NAT-PMP wraca z kodem") {
    const Fake fake;
    const net::Router router{fake.routing()};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{60});
    const net::Datagram pcp = fake.heard();

    fake.told(pcp.from, Bytes{std::byte{0}, std::byte{0x81}, std::byte{0}, std::byte{1}});

    const net::Datagram map = fake.heard();

    fake.told(map.from, pmp_answer(2, 4, 4000, 0, 0));

    try {
        (void)opening.await();
        ought(false);
    } catch (const Trouble& why) {
        same(why.code(), 4);
        ought(has(why.what(), "NAT-PMP: out of resources"));
    }
}

// Milczący router: PCP prawie dwie sekundy, NAT-PMP pominięty (kto milczy na PCP, milczy i tu),
// UPnP bez szwu XML nie rusza — odmowa szybko, nie po upływie całej cierpliwości.
TRIAL("Router: nikt nie odpowiada — odmowa po jednej turze") {
    const Fake fake;
    const net::Router router{fake.routing()};
    const auto started = Clock::now();

    threw(router.open(net::Carrier::tcp, 4000, std::chrono::seconds{60}).await());

    ought(Clock::now() - started < std::chrono::seconds{5});

    int heard = 0;

    try {
        for (;;) {
            const net::Datagram one = fake.heard(net::Patience{200});

            ought(std::to_integer<int>(one.bytes[0]) == 2);

            ++heard;
        }
    } catch (const Overdue&) {
    }

    same(heard, 3);
}

// UPnP IGD: M-SEARCH, opis z usługą WANIPConnection głęboko w drzewie, AddPortMapping z naszym
// adresem i etykietą (znaki XML ucieczką), adres zewnętrzny, a na koniec DeletePortMapping.
TRIAL("Router: UPnP — wyszukanie, opis, dodanie, adres i usunięcie") {
    const Fake fake;
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Router router{fake.routing(Seam<Codec<xml::Document>>{fake_xml()})};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    const std::string search = searched(fake, gate);

    ought(search.rfind("M-SEARCH * HTTP/1.1\r\n", 0) == 0);
    ought(has(search, "MAN: \"ssdp:discover\"\r\n"));
    ought(has(search, "ST: urn:schemas-upnp-org:"));

    Desk description = accepted(gate);

    ought(description.asked().rfind("GET /rootDesc.xml HTTP/1.1\r\n", 0) == 0);
    description.answered(200, "OPIS");

    Desk adding = accepted(gate);
    const std::string add = adding.asked();

    ought(add.rfind("POST /ctl/IPConn HTTP/1.1\r\n", 0) == 0);
    ought(has(add, "SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"\r\n"));
    ought(has(add, "<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"><NewRemoteHost></NewRemoteHost>"
                   "<NewExternalPort>4000</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>4000</NewInternalPort>"
                   "<NewInternalClient>127.0.0.1</NewInternalClient><NewEnabled>1</NewEnabled>"
                   "<NewPortMappingDescription>twa &lt;próba&gt;</NewPortMappingDescription>"
                   "<NewLeaseDuration>3600</NewLeaseDuration></u:AddPortMapping>"));
    adding.answered(200, "DODANO");

    Desk asking = accepted(gate);

    ought(has(asking.asked(), "#GetExternalIPAddress\"\r\n"));
    asking.answered(200, "ADRES");

    const net::Opening one = opening.await();

    ought(one.by == net::Opener::upnp);
    same(one.outside.text(), std::string{"198.51.100.9"});
    same(one.outside.port(), static_cast<std::uint16_t>(4000));
    same(static_cast<long long>(one.lasting.count()), static_cast<long long>(3600));

    // Odnowienie: od razu UPnP, bez wyszukiwania — usługa jest zapamiętana.
    auto renewal = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    Desk again = accepted(gate);

    ought(has(again.asked(), "#AddPortMapping\"\r\n"));
    again.answered(200, "DODANO");

    Desk asking_again = accepted(gate);

    (void)asking_again.asked();
    asking_again.answered(200, "ADRES");

    ought(renewal.await().by == net::Opener::upnp);

    auto closing = router.close(one);

    Desk deleting = accepted(gate);
    const std::string removal = deleting.asked();

    ought(has(removal, "#DeletePortMapping\"\r\n"));
    ought(has(removal, "<NewRemoteHost></NewRemoteHost><NewExternalPort>4000</NewExternalPort><NewProtocol>TCP</NewProtocol>"));
    deleting.answered(200, "USUNIETO");

    closing.await();
}

// 718 — port zewnętrzny zajęty: inny port. 725 — tylko trwałe: dzierżawa zero, czyli zamknięcie
// należy do wołającego. Adres zewnętrzny, którego router nie poda, zostaje pusty, port — nie.
TRIAL("Router: UPnP — zajęty port i tylko trwałe dzierżawy") {
    const Fake fake;
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Router router{fake.routing(Seam<Codec<xml::Document>>{fake_xml()})};

    auto opening = router.open(net::Carrier::udp, 4000, std::chrono::seconds{1000000});

    (void)searched(fake, gate);

    Desk description = accepted(gate);

    (void)description.asked();
    description.answered(200, "OPIS");

    Desk first = accepted(gate);

    ought(has(first.asked(), "<NewExternalPort>4000</NewExternalPort><NewProtocol>UDP</NewProtocol>"));
    first.answered(500, "ZAJETY");

    Desk second = accepted(gate);
    const std::string moved = second.asked();
    const std::size_t at = moved.find("<NewExternalPort>") + 17;
    const std::string port = moved.substr(at, moved.find('<', at) - at);

    ought(port != "4000");
    // Dłużej niż tydzień IGD w wersji pierwszej nie przyjmuje.
    ought(has(moved, "<NewLeaseDuration>604800</NewLeaseDuration>"));
    second.answered(500, "TRWALE");

    Desk third = accepted(gate);
    const std::string permanent = third.asked();

    ought(has(permanent, "<NewExternalPort>" + port + "</NewExternalPort>"));
    ought(has(permanent, "<NewLeaseDuration>0</NewLeaseDuration>"));
    third.answered(200, "DODANO");

    Desk asking = accepted(gate);

    (void)asking.asked();
    asking.answered(500, "NIE");

    const net::Opening one = opening.await();

    same(one.outside.port(), static_cast<std::uint16_t>(std::stoi(port)));
    same(one.outside.text(), std::string{"0.0.0.0"});
    same(static_cast<long long>(one.lasting.count()), static_cast<long long>(0));
}

TRIAL("Router: odmowa UPnP wraca z kodem") {
    const Fake fake;
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Router router{fake.routing(Seam<Codec<xml::Document>>{fake_xml()})};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    (void)searched(fake, gate);

    Desk description = accepted(gate);

    (void)description.asked();
    description.answered(200, "OPIS");

    Desk adding = accepted(gate);

    (void)adding.asked();
    adding.answered(500, "NIE");

    try {
        (void)opening.await();
        ought(false);
    } catch (const Trouble& why) {
        same(why.code(), 606);
        ought(has(why.what(), "UPnP: Action not authorized"));
    }
}

// Adres sterowania względem adresu opisu — od korzenia, względny i pełny; usługa PPP też otwiera
// porty; opis bez usługi otwierającej porty to brak UPnP.
TRIAL("Router: UPnP — adres sterowania względem opisu i usługa PPP") {
    for (const std::string control : {std::string{"ctl/PPP"}, std::string{"http://127.0.0.1:PORT/upnp/PPP"}}) {
        const Fake fake;
        const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

        std::string absolute = control;

        if (const std::size_t at = absolute.find("PORT"); at != std::string::npos) absolute.replace(at, 4, port_of(gate));

        auto xml = fake_xml();

        xml->known["OPIS"] = described(absolute, "urn:schemas-upnp-org:service:WANPPPConnection:1");

        const net::Router router{fake.routing(Seam<Codec<xml::Document>>{xml})};

        auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

        (void)searched(fake, gate);

        Desk description = accepted(gate);

        (void)description.asked();
        description.answered(200, "OPIS");

        Desk adding = accepted(gate);
        const std::string add = adding.asked();

        ought(add.rfind(control[0] == 'c' ? "POST /ctl/PPP HTTP/1.1\r\n" : "POST /upnp/PPP HTTP/1.1\r\n", 0) == 0);
        ought(has(add, "urn:schemas-upnp-org:service:WANPPPConnection:1#AddPortMapping"));
        adding.answered(200, "DODANO");

        Desk asking = accepted(gate);

        (void)asking.asked();
        asking.answered(200, "ADRES");

        ought(opening.await().by == net::Opener::upnp);
    }

    {
        const Fake fake;
        const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

        auto xml = fake_xml();

        xml->known["OPIS"] = described("/ctl/Other", "urn:schemas-upnp-org:service:Layer3Forwarding:1");

        const net::Router router{fake.routing(Seam<Codec<xml::Document>>{xml})};

        auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

        (void)searched(fake, gate);

        Desk description = accepted(gate);

        (void)description.asked();
        description.answered(200, "OPIS");

        threw(opening.await());
    }
}

// W sieci bywa kilka urządzeń udających bramę (wzmacniacz, drugi router). Port otwiera tylko ten,
// przez który wychodzimy — jego opis idzie pierwszy, nawet gdy odpowiedział później.
TRIAL("Router: UPnP — opis z samej bramy przed cudzym") {
    const Fake fake;
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Listener foreign{net::Address{"127.0.0.2", 0}};
    const net::Router router{fake.routing(Seam<Codec<xml::Document>>{fake_xml()})};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    const net::Datagram came = fake.ssdp.receive(1500, soon).await();

    fake.ssdp
        .send(came.from,
              bytes("HTTP/1.1 200 OK\r\nLOCATION: http://127.0.0.2:" + std::to_string(foreign.mine().port()) +
                    "/rootDesc.xml\r\n\r\n"),
              soon)
        .await();

    (void)searched(fake, gate);

    Desk description = accepted(gate);

    (void)description.asked();
    description.answered(200, "OPIS");

    Desk adding = accepted(gate);

    (void)adding.asked();
    adding.answered(200, "DODANO");

    Desk asking = accepted(gate);

    (void)asking.asked();
    asking.answered(200, "ADRES");

    ought(opening.await().by == net::Opener::upnp);
}

// Usługa lepsza wygrywa, choć w opisie stoi dalej; adres sterowania liczy się względem URLBase,
// gdy opis go podaje, a nie względem adresu samego opisu.
TRIAL("Router: UPnP — lepsza usługa i URLBase") {
    const Fake fake;
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};

    auto xml = fake_xml();

    xml->known["OPIS"] = document(with(
        element("root"),
        {element("URLBase", "http://127.0.0.1:" + port_of(gate) + "/"),
         with(element("device"),
              {with(element("serviceList"), {service("urn:schemas-upnp-org:service:WANPPPConnection:1", "ctl/PPP"),
                                             service("urn:schemas-upnp-org:service:WANIPConnection:1", "ctl/IP")})})}));

    const net::Router router{fake.routing(Seam<Codec<xml::Document>>{xml})};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    (void)searched(fake, gate, "/sub/rootDesc.xml");

    Desk description = accepted(gate);

    ought(description.asked().rfind("GET /sub/rootDesc.xml HTTP/1.1\r\n", 0) == 0);
    description.answered(200, "OPIS");

    Desk adding = accepted(gate);
    const std::string add = adding.asked();

    ought(add.rfind("POST /ctl/IP HTTP/1.1\r\n", 0) == 0);
    ought(has(add, "WANIPConnection:1#AddPortMapping"));
    adding.answered(200, "DODANO");

    Desk asking = accepted(gate);

    (void)asking.asked();
    asking.answered(200, "ADRES");

    ought(opening.await().by == net::Opener::upnp);
}

// Odpowiedź HTTP inna niż 200 i 500 to nie SOAP — kłopot mówi, co router odpowiedział.
TRIAL("Router: UPnP — odpowiedź spoza SOAP to kłopot z jej statusem") {
    const Fake fake;
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Router router{fake.routing(Seam<Codec<xml::Document>>{fake_xml()})};

    auto opening = router.open(net::Carrier::tcp, 4000, std::chrono::seconds{3600});

    (void)searched(fake, gate);

    Desk description = accepted(gate);

    (void)description.asked();
    description.answered(200, "OPIS");

    Desk adding = accepted(gate);

    (void)adding.asked();
    adding.answered(404, "DODANO");

    try {
        (void)opening.await();
        ought(false);
    } catch (const Trouble& why) {
        ought(has(why.what(), "HTTP 404"));
    }
}

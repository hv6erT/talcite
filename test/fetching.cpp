#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/address.hpp"
#include "talcite/http.hpp"
#include "talcite/listener.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"
#include "talcite/tls.hpp"

#include "raced.hpp"

using namespace talcite;

// Klient HTTP przeciw serwerowi z próby: brama na pętli zwrotnej, a po drugiej stronie próba sama
// czyta żądanie i pisze odpowiedź bajt w bajt — tak widać dokładnie, co klient wysyła i jak czyta
// to, co dostaje, łącznie z odpowiedziami krzywymi, których prawdziwy serwer by nie dał. HTTPS
// idzie przez atrapę szwu TLS z `faking.hpp`.

namespace {

const net::Patience soon{8000};

net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

// Druga strona jednego połączenia: zwykła albo za atrapą TLS.
struct Desk {
    net::Stream plain;
    net::TlsStream secure;
    std::string kept;

    void wrote(const std::string& what) {
        if (secure.held()) secure.write(bytes(what), soon).await();
        else plain.write(bytes(what), soon).await();
    }

    // Głowa żądania do pustego wiersza.
    std::string heard() {
        for (;;) {
            if (const std::size_t at = kept.find("\r\n\r\n"); at != std::string::npos) {
                std::string made = kept.substr(0, at + 4);

                kept.erase(0, at + 4);

                return made;
            }

            const Bytes got = secure.held() ? secure.read(4096, soon).await() : plain.read(4096, soon).await();

            if (got.empty()) return {};

            kept += text(got);
        }
    }

    // Czy klient zamknął swoją stronę — czytanie oddaje pustkę. Cisza dłuższa niż `patience` to
    // „nie zamknął".
    bool ended(net::Patience patience = soon) {
        try {
            const Bytes got = secure.held() ? secure.read(64, patience).await() : plain.read(64, patience).await();

            return got.empty();
        } catch (const Overdue&) {
            return false;
        }
    }

    // Treść żądania o podanej długości — po głowie, z tego, co już przyszło, i z dalszego czytania.
    std::string taken(std::size_t much) {
        while (kept.size() < much) {
            const Bytes got = secure.held() ? secure.read(4096, soon).await() : plain.read(4096, soon).await();

            if (got.empty()) break;

            kept += text(got);
        }

        std::string made = kept.substr(0, much);

        kept.erase(0, much);

        return made;
    }

    void close() {
        if (secure.held()) secure.close();
        if (plain.held()) plain.close();
    }
};

Desk accepted(const net::Listener& gate) {
    Desk made;

    made.plain = gate.accept(soon).await();

    return made;
}

Desk accepted(const net::Listener& gate, const Seam<net::TlsProvider>& tls) {
    Desk made;

    made.secure = net::TlsStream::server(gate.accept(soon).await(), tls, soon).await();

    return made;
}

std::string at(const net::Listener& gate, const std::string& target, const char* scheme = "http") {
    return std::string{scheme} + "://127.0.0.1:" + std::to_string(gate.mine().port()) + target;
}

net::Request asked(const std::string& url, std::vector<net::Header> headers = {}) {
    return net::Request{net::Url::from_string(url).value(), std::move(headers)};
}

std::string body(const net::Response& one) { return text(one.body); }

// Powód odrzucenia klienta HTTP, a gdy odrzucił czymś innym — pustka.
std::optional<net::HttpTrouble::Why> why(Promise<net::Response>& one) {
    try {
        (void)one.await();
    } catch (const net::HttpTrouble& trouble) {
        return trouble.why();
    } catch (const std::exception&) {
        return std::nullopt;
    }

    return std::nullopt;
}

bool refused(Promise<net::Response>& one) {
    try {
        (void)one.await();
    } catch (const std::exception&) {
        return true;
    }

    return false;
}

const Seam<net::TlsProvider> nothing{};

}

// --- Url -----------------------------------------------------------------------------------------

// Adres rozbiera się na schemat, host, port i cel: port domyślny ze schematu, cel co najmniej „/",
// zapytanie bez ścieżki dostaje ukośnik, kotwica odpada, schemat i host małymi literami, IPv6
// w nawiasach traci nawiasy. Zapis wraca do tekstu bez portu domyślnego.
TRIAL("HTTP: adres rozbiera się i składa z powrotem") {
    const auto plain = net::Url::from_string("http://Example.COM");

    ought(plain.has_value());
    ought(!plain->secure);
    same(plain->host, std::string{"example.com"});
    same(static_cast<int>(plain->port), 80);
    same(plain->target, std::string{"/"});
    same(net::to_string(*plain), std::string{"http://example.com/"});

    const auto secure = net::Url::from_string("HTTPS://api.themoviedb.org/3/movie/550?api_key=x#góra");

    ought(secure.has_value());
    ought(secure->secure);
    same(static_cast<int>(secure->port), 443);
    same(secure->target, std::string{"/3/movie/550?api_key=x"});
    same(net::to_string(*secure), std::string{"https://api.themoviedb.org/3/movie/550?api_key=x"});

    const auto ported = net::Url::from_string("http://localhost:8080?q=1");

    ought(ported.has_value());
    same(static_cast<int>(ported->port), 8080);
    same(ported->target, std::string{"/?q=1"});
    same(net::to_string(*ported), std::string{"http://localhost:8080/?q=1"});

    const auto six = net::Url::from_string("https://[::1]:8443/a");

    ought(six.has_value());
    same(six->host, std::string{"::1"});
    same(static_cast<int>(six->port), 8443);
    same(net::to_string(*six), std::string{"https://[::1]:8443/a"});

    same(net::Url::from_string("http://ZOO.Example.pl/").value().host, std::string{"zoo.example.pl"});
    same(net::to_string(*net::Url::from_string("https://[::1]/")), std::string{"https://[::1]/"});
    same(net::to_string(*net::Url::from_string("http://a:443/")), std::string{"http://a:443/"});
    same(net::to_string(*net::Url::from_string("https://a:80/")), std::string{"https://a:80/"});
}

// Czego adres nie przyjmuje: innego schematu, braku schematu, użytkownika w adresie, pustego hosta,
// portu zerowego, za dużego albo nie liczbowego, pustego po dwukropku, IPv6 bez nawiasów, spacji,
// znaków sterujących i bajtów spoza ASCII w celu — te koduje wołający.
TRIAL("HTTP: krzywy adres nie jest adresem") {
    for (const char* bad : {"ftp://a/", "a/b", "//a/b", "http://user@a/", "http:///x", "http://a:0/", "http://a:65536/",
                            "http://a:x/", "http://a:/", "http://[::1/", "http://[::1]x/", "http://[::1]:/", "http://::1/",
                            "http://a/b c", "http://a/\x01", "http://a/\x7f", "http://a/zażółć", "http://a b/",
                            "http://[a_b]/", "http://a:80x/", "http://[::1]x80/"})
        ought(!net::Url::from_string(bad).has_value());

    ought(net::Url::from_string("http://moja_nazwa/").has_value());
    ought(net::Url::from_string("http://a:65535/").has_value());
}

// --- Rozmowa -------------------------------------------------------------------------------------

// Zwykłe GET: wiersz żądania z celem, `Host` z portem, gdy nie jest domyślny, nagłówki wołającego —
// poza `Host`, `Connection` i `Accept-Encoding`, które klient stawia sam — i prośba o treść bez
// kompresji. Odpowiedź niesie status, nagłówki, treść o podanej długości i adres, z którego przyszła;
// nagłówek szuka się bez wielkości liter.
TRIAL("HTTP: GET wysyła żądanie i czyta odpowiedź o podanej długości") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/3/movie/550?language=pl-PL"),
                                {{"Accept", "application/json"}, {"host", "oszust"}, {"Connection", "close"},
                                 {"accept-encoding", "gzip"}}));

    Desk desk = accepted(gate);

    const std::string head = desk.heard();

    same(head, "GET /3/movie/550?language=pl-PL HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(gate.mine().port()) +
                   "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n\r\n");

    desk.wrote("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"id\":550}\n");

    const net::Response one = got.await();

    same(one.status, 200);
    same(body(one), std::string{"{\"id\":550}\n"});
    ought(one.header("content-TYPE") != nullptr);
    same(*one.header("content-type"), std::string{"application/json"});
    ought(one.header("x-nie-ma") == nullptr);
    same(net::to_string(one.url), at(gate, "/3/movie/550?language=pl-PL"));
}

// POST (dla SOAP w UPnP): metoda, długość treści od klienta, nie od wołającego, i sama treść.
// Idzie zawsze nowym połączeniem — żądanie, które coś zmienia, nie może trafić na połączenie
// z zapasu, które padło w pół drogi, bo po cichu poszłoby drugi raz. Po odpowiedzi połączenie
// i tak wraca do zapasu i niesie następny GET.
TRIAL("HTTP: POST wysyła treść, zawsze nowym połączeniem") {
    const net::Listener gate = standing();
    const std::string port = std::to_string(gate.mine().port());

    net::HttpClient client{nothing};

    auto first = client.post(asked(at(gate, "/ctl"), {{"Content-Type", "text/xml"}, {"content-length", "999"}}),
                             bytes("<a>1</a>"));

    Desk desk = accepted(gate);

    same(desk.heard(), "POST /ctl HTTP/1.1\r\nHost: 127.0.0.1:" + port +
                           "\r\nContent-Type: text/xml\r\nContent-Length: 8\r\nAccept-Encoding: identity\r\n\r\n");
    same(desk.taken(8), std::string{"<a>1</a>"});

    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    same(body(first.await()), std::string{"ok"});

    auto second = client.post(asked(at(gate, "/ctl")), Bytes{});

    Desk other = accepted(gate);

    same(other.heard().substr(0, 34), std::string{"POST /ctl HTTP/1.1\r\nHost: 127.0.0."});
    other.wrote("HTTP/1.1 500 Internal\r\nContent-Length: 0\r\n\r\n");

    same(second.await().status, 500);

    // Pole metody i treści w żądaniu nic nie znaczy dla `get` — zawsze GET bez treści.
    net::Request odd = asked(at(gate, "/x"));

    odd.method = "DELETE";
    odd.body = bytes("nie");

    auto third = client.get(odd);

    // Zapas trzyma jedno połączenie na serwer — ostatnie, które wróciło.
    const std::string head = other.heard();

    same(head.substr(0, 11), std::string{"GET /x HTTP"});
    ought(head.find("Content-Length") == std::string::npos);

    other.wrote("HTTP/1.1 204 No Content\r\n\r\n");

    same(third.await().status, 204);
}

// 307 i 308 powtarzają POST razem z treścią; 301, 302 i 303 zamieniają go w GET bez treści.
TRIAL("HTTP: przekierowanie POST — 307 z treścią, 303 już jako GET") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.post(asked(at(gate, "/a")), bytes("treść"));

    Desk desk = accepted(gate);

    (void)desk.heard();
    same(desk.taken(7), std::string{"treść"});
    desk.wrote("HTTP/1.1 307 Temporary\r\nLocation: /b\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");

    Desk second = accepted(gate);

    const std::string again = second.heard();

    same(again.substr(0, 12), std::string{"POST /b HTTP"});
    ought(again.find("Content-Length: 7") != std::string::npos);
    same(second.taken(7), std::string{"treść"});
    second.wrote("HTTP/1.1 303 See Other\r\nLocation: /c\r\nContent-Length: 0\r\n\r\n");

    const std::string last = second.heard();

    same(last.substr(0, 11), std::string{"GET /c HTTP"});
    ought(last.find("Content-Length") == std::string::npos);
    second.wrote("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

    same(got.await().status, 200);
}

// Status nie jest błędem: 404 i 429 wracają jako odpowiedź, a nagłówek `Retry-After` razem z nią.
// Wartość nagłówka traci otaczające spacje i tabulatory; nazwa bywa z cyframi i znakami tokenu;
// linia statusu może nie mieć opisu.
TRIAL("HTTP: 404 i 429 to odpowiedzi, a nie błędy") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/a")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");

    same(first.await().status, 404);

    auto second = client.get(asked(at(gate, "/b")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 429\r\nRetry-After: \t 2 \t\r\nX-Limit-2: 5\r\nX.Dziwna_Nazwa!#: 1\r\nContent-Length: 0\r\n\r\n");

    const net::Response two = second.await();

    same(two.status, 429);
    same(*two.header("retry-after"), std::string{"2"});
    ought(two.header("x-limit-2") != nullptr);
    ought(two.header("x.dziwna_nazwa!#") != nullptr);
}

// Treść kawałkami: długości szesnastkowo, rozszerzenia po średniku, kawałek zerowy i przyczepka.
// Wielkie litery i puste pozycje listy w nazwie kodowania nic nie zmieniają. Po przyczepce
// połączenie jest czyste i niesie następne żądanie.
TRIAL("HTTP: treść kawałkami skleja się w całość") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\nTransfer-Encoding: , Chunked ,\r\n\r\n"
               "5;nazwa=wartość\r\nAla m\r\n"
               "A\r\na kota i p\r\n"
               "3 \r\nsa.\r\n"
               "0\r\nX-Przyczepka: tak\r\nX-Druga: też\r\n\r\n");

    same(body(got.await()), std::string{"Ala ma kota i psa."});

    auto again = client.get(asked(at(gate, "/2")));

    same(desk.heard().substr(0, 10), std::string{"GET /2 HTT"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    same(body(again.await()), std::string{"ok"});
}

// Odpowiedź bez długości i bez kawałków trwa do zamknięcia — i tego połączenia nie da się użyć
// drugi raz, więc następne żądanie idzie nowym. Tak samo HTTP/1.0 i `Connection: close`.
TRIAL("HTTP: treść do zamknięcia, HTTP/1.0 i Connection: close nie wracają do zapasu") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\n\r\nwszystko, co jest");
    one.close();

    same(body(first.await()), std::string{"wszystko, co jest"});

    auto second = client.get(asked(at(gate, "/2")));

    Desk two = accepted(gate);

    (void)two.heard();
    two.wrote("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");

    same(body(second.await()), std::string{"ok"});
    ought(two.ended());

    auto third = client.get(asked(at(gate, "/3")));

    Desk three = accepted(gate);

    (void)three.heard();
    three.wrote("HTTP/1.1 200 OK\r\nConnection: keep-alive, Close\r\nContent-Length: 2\r\n\r\nok");

    same(body(third.await()), std::string{"ok"});
    ought(three.ended());

    auto fourth = client.get(asked(at(gate, "/4")));

    Desk four = accepted(gate);

    same(four.heard().substr(0, 10), std::string{"GET /4 HTT"});
    four.wrote("HTTP/1.1 204 No Content\r\n\r\n");

    same(fourth.await().status, 204);
}

// **Jedno połączenie na serwer**: po odpowiedzi z granicą połączenie czeka w zapasie i następne
// żądanie idzie nim — brama nie przyjmuje drugiego. Odpowiedzi 204 i 304 nie mają treści, choćby
// nie było długości.
TRIAL("HTTP: połączenie wraca do zapasu i niesie następne żądania") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 204 No Content\r\n\r\n");

    same(first.await().status, 204);

    auto second = client.get(asked(at(gate, "/2")));

    same(desk.heard().substr(0, 10), std::string{"GET /2 HTT"});
    desk.wrote("HTTP/1.1 304 Not Modified\r\n\r\n");

    same(second.await().status, 304);

    auto third = client.get(asked(at(gate, "/3")));

    same(desk.heard().substr(0, 10), std::string{"GET /3 HTT"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntak");

    same(body(third.await()), std::string{"tak"});
}

// Bajty ponad zapowiedzianą długość to nie następna odpowiedź — połączenie z nimi nie wraca do
// zapasu, a następne żądanie idzie nowym.
TRIAL("HTTP: nadmiarowe bajty zamykają połączenie") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nab");

    same(body(first.await()), std::string{"a"});

    auto second = client.get(asked(at(gate, "/2")));

    Desk two = accepted(gate);

    same(two.heard().substr(0, 10), std::string{"GET /2 HTT"});
    two.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nc");

    same(body(second.await()), std::string{"c"});
    ought(one.ended());
}

// Połączenie z zapasu, które serwer zamknął w bezczynności, pada przed pierwszym bajtem odpowiedzi —
// żądanie idzie wtedy jeszcze raz, nowym połączeniem, i człowiek niczego nie zauważa.
TRIAL("HTTP: połączenie zamknięte w bezczynności zastępuje nowe") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na");

    same(body(first.await()), std::string{"a"});

    one.close();

    auto second = client.get(asked(at(gate, "/2")));

    Desk two = accepted(gate);

    same(two.heard().substr(0, 10), std::string{"GET /2 HTT"});
    two.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nb");

    same(body(second.await()), std::string{"b"});
}

// Połączenie z zapasu, które padło **w środku** odpowiedzi, nie jest bezczynnym: żądanie się nie
// powtarza, bo serwer mógł je już wykonać.
TRIAL("HTTP: połączenie urwane w środku odpowiedzi nie ponawia żądania") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na");

    (void)first.await();

    auto second = client.get(asked(at(gate, "/2")));

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\npół");
    one.close();

    same(why(second), std::optional{net::HttpTrouble::Why::malformed});
}

// Odpowiedzi 1xx przed właściwą się pomija.
TRIAL("HTTP: odpowiedź tymczasowa 1xx przepuszcza właściwą") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    const net::Response one = got.await();

    same(one.status, 200);
    same(body(one), std::string{"ok"});
}

// --- Granice -------------------------------------------------------------------------------------

// Głowa ponad granicę, treść ponad granicę — zapowiedziana długością (odmowa zanim cokolwiek
// przyjdzie), kawałkami i do zamknięcia.
TRIAL("HTTP: granice głowy i treści") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, net::Limits{64, 10, 5, soon}};

    const auto tried = [&](const std::string& answer) {
        auto got = client.get(asked(at(gate, "/")));

        Desk desk = accepted(gate);

        (void)desk.heard();
        desk.wrote(answer);
        desk.close();

        return why(got);
    };

    same(tried("HTTP/1.1 200 OK\r\nX-Długi: " + std::string(100, 'x') + "\r\n\r\n"),
         std::optional{net::HttpTrouble::Why::oversized});
    same(tried("HTTP/1.1 200 OK\r\nX-Bez-Końca: " + std::string(100, 'x')),
         std::optional{net::HttpTrouble::Why::oversized});
    same(tried("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n"), std::optional{net::HttpTrouble::Why::oversized});
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nabcdef\r\n5\r\nghijk\r\n0\r\n\r\n"),
         std::optional{net::HttpTrouble::Why::oversized});
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\n"),
         std::optional{net::HttpTrouble::Why::oversized});
    same(tried("HTTP/1.1 200 OK\r\n\r\n" + std::string(11, 'x')), std::optional{net::HttpTrouble::Why::oversized});

    auto exact = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");

    same(body(exact.await()), std::string{"0123456789"});
}

// Odpowiedzi, których nie da się czytać: linia statusu nie z HTTP/1.x, kod nie liczbą, nagłówek bez
// dwukropka, zawinięty nagłówek, sprzeczne długości, kodowanie inne niż `chunked`, długość kawałka
// nie liczbą, kawałek dłuższy niż zapowiedziany, za długi wiersz kawałka, połączenie zamknięte
// w głowie i w treści, dwie odpowiedzi tymczasowe pod rząd.
TRIAL("HTTP: krzywa odpowiedź to złamany protokół") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    const auto tried = [&](const std::string& answer) {
        auto got = client.get(asked(at(gate, "/")));

        Desk desk = accepted(gate);

        (void)desk.heard();
        desk.wrote(answer);
        desk.close();

        return why(got);
    };

    const std::optional<net::HttpTrouble::Why> bad{net::HttpTrouble::Why::malformed};

    same(tried("SPDY/3 200 OK\r\n\r\n"), bad);
    same(tried("HTTP/1.2 200 OK\r\n\r\n"), bad);
    same(tried("HTTP/1.1_200 OK\r\n\r\n"), bad);
    same(tried("HTTP/1.1 20\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), bad);
    same(tried("HTTP/1.1 200 OK\r\nZła nazwa: x\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nContent-Length: 2x\r\n\r\nab"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5x\r\nabcde\r\n0\r\n\r\n"), bad);
    same(tried("HTTP/2.0 200 OK\r\n\r\n"), bad);
    same(tried("HTTP/1.1 2x0 OK\r\n\r\n"), bad);
    same(tried("HTTP/1.1 099 Za mało\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200OK\r\n\r\n"), bad);
    same(tried("HTTP/1.1 20\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nBez dwukropka\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\n: pusta nazwa\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nX-A: 1\r\n  zawinięte\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc"), bad);
    same(tried("HTTP/1.1 200 OK\r\nContent-Length: dwa\r\n\r\nab"), bad);
    same(tried("HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabc\r\n0\r\n\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string(5000, '1')), bad);
    same(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n"), bad);
    same(tried("HTTP/1.1 200 OK\r\nContent-Le"), bad);
    same(tried("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab"), bad);
    same(tried("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 102 Processing\r\n\r\nHTTP/1.1 200 OK\r\n\r\n"), bad);
}

// Cisza serwera kończy się po `patience` odmową, a nie wiecznym czekaniem.
TRIAL("HTTP: milczący serwer kończy się terminem") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, net::Limits{1024, 1024, 5, net::Patience{200}}};

    auto got = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();

    ought(refused(got));
    ought(!why(got).has_value());
}

// --- Przekierowania ------------------------------------------------------------------------------

// Przekierowania wszystkich pięciu rodzajów, z adresem od korzenia, względnym (ukośnik w zapytaniu
// nie jest ścieżką), bez schematu i pełnym; odpowiedź mówi, gdzie się skończyło. Poświadczenia idą dalej tylko do tego samego serwera.
TRIAL("HTTP: przekierowania prowadzą do celu, a poświadczenia zostają przy swoim serwerze") {
    const net::Listener gate = standing();

    const std::string port = std::to_string(gate.mine().port());

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/a/b?x=/1"), {{"Authorization", "Bearer sekret"}, {"X-Inny", "1"}}));

    Desk desk = accepted(gate);

    same(desk.heard().substr(0, 17), std::string{"GET /a/b?x=/1 HTT"});
    desk.wrote("HTTP/1.1 301 Moved\r\nLocation: c/d\r\nContent-Length: 0\r\n\r\n");

    const std::string second = desk.heard();

    same(second.substr(0, 14), std::string{"GET /a/c/d HTT"});
    ought(second.find("Authorization: Bearer sekret") != std::string::npos);
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /e#kotwica\r\nContent-Length: 0\r\n\r\n");

    same(desk.heard().substr(0, 10), std::string{"GET /e HTT"});
    desk.wrote("HTTP/1.1 303 See Other\r\nLocation: //127.0.0.1:" + port + "/f\r\nContent-Length: 0\r\n\r\n");

    same(desk.heard().substr(0, 10), std::string{"GET /f HTT"});
    desk.wrote("HTTP/1.1 307 Temporary\r\nLocation: http://localhost:" + port + "/g\r\nContent-Length: 0\r\n\r\n");

    Desk other = accepted(gate);

    const std::string moved = other.heard();

    same(moved.substr(0, 10), std::string{"GET /g HTT"});
    ought(moved.find("Host: localhost:" + port) != std::string::npos);
    ought(moved.find("Authorization") == std::string::npos);
    ought(moved.find("X-Inny: 1") != std::string::npos);
    other.wrote("HTTP/1.1 308 Permanent\r\nLocation: /h\r\nContent-Length: 0\r\n\r\n");

    same(other.heard().substr(0, 10), std::string{"GET /h HTT"});
    other.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    const net::Response one = got.await();

    same(one.status, 200);
    same(net::to_string(one.url), "http://localhost:" + port + "/h");
}

// Przekierowanie bez `Location` to zwykła odpowiedź; `Location`, który nie jest adresem, to złamany
// protokół; więcej przekierowań niż w granicy — odmowa.
TRIAL("HTTP: przekierowanie bez celu, z krzywym celem i ponad granicę") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, net::Limits{64 * 1024, 1024, 1, soon}};

    auto bare = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n");

    same(bare.await().status, 302);

    auto crooked = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: ftp://a/b\r\nContent-Length: 0\r\n\r\n");

    same(why(crooked), std::optional{net::HttpTrouble::Why::malformed});

    auto empty = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: \r\nContent-Length: 0\r\n\r\n");

    same(why(empty), std::optional{net::HttpTrouble::Why::malformed});

    auto spaced = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /a b\r\nContent-Length: 0\r\n\r\n");

    same(why(spaced), std::optional{net::HttpTrouble::Why::malformed});

    auto looped = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /1\r\nContent-Length: 0\r\n\r\n");
    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /2\r\nContent-Length: 0\r\n\r\n");

    same(why(looped), std::optional{net::HttpTrouble::Why::redirected});
}

// --- HTTPS ---------------------------------------------------------------------------------------

// HTTPS idzie przez szew TLS: bez dostawcy odmowa, zanim cokolwiek pójdzie w sieć; z dostawcą —
// ta sama rozmowa po szyfrowanym połączeniu, które też wraca do zapasu, a przekierowanie bez
// schematu zostaje przy HTTPS. Przekierowanie z HTTPS na zwykłe HTTP to odmowa.
TRIAL("HTTP: HTTPS przez szew TLS, bez niego odmowa, a zejście na HTTP zatrzymane") {
    const net::Listener gate = standing();

    net::HttpClient bare{nothing};

    auto refused_now = bare.get(asked(at(gate, "/", "https")));

    same(why(refused_now), std::optional{net::HttpTrouble::Why::insecure});

    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    net::HttpClient client{tls};

    auto got = client.get(asked(at(gate, "/tajne", "https")));

    Desk desk = accepted(gate, tls);

    same(desk.heard().substr(0, 15), std::string{"GET /tajne HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nszyfr!");

    same(body(got.await()), std::string{"szyfr!"});

    auto jump = client.get(asked(at(gate, "/skok", "https")));

    same(desk.heard().substr(0, 14), std::string{"GET /skok HTTP"});
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: //127.0.0.1:" + std::to_string(gate.mine().port()) +
               "/cel\r\nContent-Length: 0\r\n\r\n");

    same(desk.heard().substr(0, 13), std::string{"GET /cel HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    same(net::to_string(jump.await().url), at(gate, "/cel", "https"));

    auto down = client.get(asked(at(gate, "/znowu", "https")));

    same(desk.heard().substr(0, 15), std::string{"GET /znowu HTTP"});
    desk.wrote("HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1/\r\nContent-Length: 0\r\n\r\n");

    same(why(down), std::optional{net::HttpTrouble::Why::downgraded});

    net::HttpClient plain{nothing};

    auto upward = plain.get(asked(at(gate, "/")));

    Desk third = accepted(gate);

    (void)third.heard();
    third.wrote("HTTP/1.1 301 Moved\r\nLocation: https://127.0.0.1/\r\nContent-Length: 0\r\n\r\n");

    same(why(upward), std::optional{net::HttpTrouble::Why::insecure});
}

// Odmowa uścisku z powodem przechodzi przez klienta HTTP nietknięta: wołający dostaje `TlsTrouble`
// z powodem od przejściówki i z nazwą serwera z adresu, a nie sam napis.
TRIAL("HTTP: odmowa uścisku TLS dochodzi do wołającego z powodem i nazwą serwera") {
    const net::Listener gate = standing();

    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>(false, net::TlsTrouble::Why::untrusted)};

    net::HttpClient client{tls};

    auto got = client.get(asked(at(gate, "/", "https")));

    bool refused = false;

    try {
        (void)got.await();
    } catch (const net::TlsTrouble& trouble) {
        refused = true;

        same(trouble.why(), net::TlsTrouble::Why::untrusted);
        same(trouble.host(), std::string{"127.0.0.1"});
    }

    ought(refused);
}

// --- Wołający ------------------------------------------------------------------------------------

// Nagłówek wołającego, który łamie wiersz albo ma nazwę spoza tokenu, nie wychodzi w sieć — to byłoby
// wstrzyknięcie drugiego żądania. Pusty klient odmawia, a zamknięty port to odmowa sieci.
TRIAL("HTTP: nagłówek łamiący wiersz, pusty klient i zamknięty port") {
    net::HttpClient client{nothing};

    for (const net::Header& bad : {net::Header{"X-A", "1\r\nGET /inne HTTP/1.1"}, net::Header{"X-A", "a\nb"}, net::Header{"X-A", "a\rb"},
                                   net::Header{"X A", "1"}, net::Header{"", "1"}, net::Header{"X-A", std::string{"a\0b", 3}}}) {
        auto got = client.get(net::Request{*net::Url::from_string("http://127.0.0.1:9/"), {bad}});

        same(why(got), std::optional{net::HttpTrouble::Why::malformed});
    }

    net::HttpClient empty;

    ought(!empty.held());

    auto none = empty.get(asked("http://127.0.0.1/"));

    ought(refused(none));

    std::uint16_t port = 0;

    {
        const net::Listener gate = standing();

        port = gate.mine().port();
    }

    auto closed = client.get(asked("http://127.0.0.1:" + std::to_string(port) + "/"));

    ought(refused(closed));
    ought(!why(closed).has_value());
}

// Dwa żądania naraz do jednego serwera idą dwoma połączeniami; do zapasu wraca jedno, a drugie się
// zamyka — zapas trzyma jedno połączenie na serwer.
TRIAL("HTTP: dwa żądania naraz, jedno połączenie w zapasie") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));
    auto second = client.get(asked(at(gate, "/2")));

    Desk one = accepted(gate);
    Desk two = accepted(gate);

    // Które połączenie niesie które żądanie, wie tylko system; odpowiedź powtarza ścieżkę.
    for (Desk* desk : {&one, &two}) {
        const std::string path = desk->heard().substr(4, 2);

        desk->wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n" + path);
    }

    same(body(first.await()), std::string{"/1"});
    same(body(second.await()), std::string{"/2"});

    const int closed = static_cast<int>(one.ended(net::Patience{300})) + static_cast<int>(two.ended(net::Patience{300}));

    same(closed, 1);
}

// --- Pośrednik -----------------------------------------------------------------------------------

// Zapis pośrednika jak w zmiennych środowiska: bez schematu to HTTP, użytkownik i hasło przed `@`
// z kodowaniem procentowym, ścieżka nic nie znaczy; SOCKS i zapis krzywy dają brak. Wyjątki:
// po przecinkach i spacjach, małymi literami, bez portu, `*.` jak `.`.
TRIAL("Pośrednik: zapis ze środowiska rozbiera się na adres, podpis i wyjątki") {
    const net::Proxy made = net::Proxy::from_strings("Pośrednik", "ala:ma%40kota@Proxy.Example:3128/cokolwiek",
                                                     " .Wewn.Example, localhost:8080  *.lan,,[::1]:80 ");

    ought(!made.plain.has_value());
    ought(made.secure.has_value());
    same(made.secure->url.host, std::string{"proxy.example"});
    same(static_cast<int>(made.secure->url.port), 3128);
    ought(!made.secure->url.secure);
    same(made.secure->login, std::string{"ala:ma@kota"});
    same(made.bypass, (std::vector<std::string>{".wewn.example", "localhost", ".lan", "::1"}));

    const net::Proxy bare = net::Proxy::from_strings("pośrednik.lan:8080", "socks5://pośrednik:1080", "");

    ought(!bare.plain.has_value());
    ought(!bare.secure.has_value());

    const net::Proxy plain = net::Proxy::from_strings("posrednik.lan:8080", "https://posrednik.lan", "*");

    ought(plain.plain.has_value());
    same(plain.plain->url.host, std::string{"posrednik.lan"});
    same(static_cast<int>(plain.plain->url.port), 8080);
    same(plain.plain->login, std::string{});
    ought(plain.secure.has_value());
    ought(plain.secure->url.secure);
    same(static_cast<int>(plain.secure->url.port), 443);
}

namespace {

// Zmienna środowiska podstawiona na czas próby i przywrócona po niej. Pusty napis to „nie ma jej".
class Planted {
public:
    Planted(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = was;

        put(value);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    ~Planted() { put(was_ ? was_->c_str() : ""); }

private:
    void put(const char* value) const {
#ifdef _WIN32
        (void)::_putenv_s(name_, value);
#else
        if (*value == '\0') ::unsetenv(name_);
        else ::setenv(name_, value, 1);
#endif
    }

    const char* name_;
    std::optional<std::string> was_;
};

}

// Na każdym systemie: pośrednik HTTPS ze swojej zmiennej, `all_proxy` dla HTTP, gdy osobnej nie ma,
// i wyjątki z `no_proxy`.
TRIAL("Pośrednik: środowisko daje pośrednika, zapasowego i wyjątki") {
    const Planted plain{"http_proxy", ""};
    const Planted secure{"https_proxy", "http://male:2"};
    const Planted all{"all_proxy", "http://wszystkie:4"};
    const Planted no{"no_proxy", "lan,.dom"};

    const net::Proxy made = net::Proxy::from_environment();

    ought(made.secure.has_value());
    same(made.secure->url.host, std::string{"male"});
    ought(made.plain.has_value());
    same(made.plain->url.host, std::string{"wszystkie"});
    same(made.bypass, (std::vector<std::string>{"lan", ".dom"}));
}

#ifndef _WIN32

// Zmienne jak u curl: małe litery przed wielkimi, `all_proxy` tam, gdzie osobnej nie ma, i bez
// `HTTP_PROXY` wielkimi literami — pod CGI ustawia ją cudze żądanie. Na Windows wielkość liter
// w nazwach zmiennych nic nie znaczy, więc tam tej próby nie ma.
TRIAL("Pośrednik: środowisko jak u curl") {
    const Planted plain_low{"http_proxy", ""};
    const Planted plain_high{"HTTP_PROXY", "http://wielkie:1"};
    const Planted secure_low{"https_proxy", "http://male:2"};
    const Planted secure_high{"HTTPS_PROXY", "http://wielkie:3"};
    const Planted all_low{"all_proxy", ""};
    const Planted all_high{"ALL_PROXY", "http://wszystkie:4"};
    const Planted no_low{"no_proxy", ""};
    const Planted no_high{"NO_PROXY", "lan"};

    const net::Proxy made = net::Proxy::from_environment();

    ought(made.secure.has_value());
    same(made.secure->url.host, std::string{"male"});
    ought(made.plain.has_value());
    same(made.plain->url.host, std::string{"wszystkie"});
    same(made.bypass, std::vector<std::string>{"lan"});

    const Planted secure_gone{"https_proxy", ""};
    const Planted all_set{"all_proxy", "http://male-wszystkie:5"};
    const Planted no_set{"no_proxy", "*"};

    const net::Proxy second = net::Proxy::from_environment();

    same(second.secure->url.host, std::string{"wielkie"});
    same(second.plain->url.host, std::string{"male-wszystkie"});
    same(second.bypass, std::vector<std::string>{"*"});
}

#endif

// Wyjątek obejmuje nazwę i jej poddomeny, ale nie nazwę, która tylko kończy się tymi samymi
// literami; `*` obejmuje wszystko. HTTP i HTTPS biorą każde swojego pośrednika.
TRIAL("Pośrednik: wyjątki omijają pośrednika, reszta idzie do właściwego") {
    const net::Proxy made = net::Proxy::from_strings("http://jeden:1", "http://dwa:2", "example.com,.lan");

    const auto via = [&made](const char* url) -> std::string {
        const net::Gateway* one = made.chosen(*net::Url::from_string(url));

        return one != nullptr ? one->url.host : std::string{"wprost"};
    };

    same(via("http://inny.pl/"), std::string{"jeden"});
    same(via("https://inny.pl/"), std::string{"dwa"});
    same(via("https://example.com/"), std::string{"wprost"});
    same(via("https://api.example.com/"), std::string{"wprost"});
    same(via("https://notexample.com/"), std::string{"dwa"});
    same(via("http://nas.lan/"), std::string{"wprost"});
    same(via("http://lan/"), std::string{"wprost"});

    const net::Proxy all = net::Proxy::from_strings("http://jeden:1", "", "*");

    ought(all.chosen(*net::Url::from_string("http://inny.pl/")) == nullptr);
    ought(net::Proxy{}.chosen(*net::Url::from_string("http://inny.pl/")) == nullptr);
}

// Zwykłe HTTP przez pośrednika: pełny adres w wierszu żądania, `Host` serwera i podpis `Basic`
// dla pośrednika — a podpis podany przez wołającego nie wychodzi.
TRIAL("Pośrednik: zwykłe HTTP idzie z pełnym adresem i podpisem") {
    const net::Listener gate = standing();

    const std::string proxy = "ala:ma%40kota@127.0.0.1:" + std::to_string(gate.mine().port());

    net::HttpClient client{nothing, {}, net::Proxy::from_strings(proxy, "", "")};

    auto got = client.get(asked("http://kino.example:8080/film?id=1", {{"Proxy-Authorization", "Basic cudze"}}));

    Desk desk = accepted(gate);

    same(desk.heard(), std::string{"GET http://kino.example:8080/film?id=1 HTTP/1.1\r\nHost: kino.example:8080\r\n"
                                   "Proxy-Authorization: Basic YWxhOm1hQGtvdGE=\r\nAccept-Encoding: identity\r\n\r\n"});

    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    same(body(got.await()), std::string{"ok"});
}

// HTTPS przez pośrednika: `CONNECT` z nazwą i portem, odpowiedź 200 i TLS z serwerem w tunelu —
// pośrednik nie widzi żądania. Odmowa tunelu (407) to kłopot pośrednika, a nie odpowiedź.
TRIAL("Pośrednik: HTTPS idzie tunelem CONNECT, a odmowa tunelu jest kłopotem pośrednika") {
    const net::Listener gate = standing();
    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    const std::string proxy = "http://127.0.0.1:" + std::to_string(gate.mine().port());

    net::HttpClient client{tls, {}, net::Proxy::from_strings("", proxy, "")};

    auto got = client.get(asked("https://kino.example:8443/tajne"));

    Desk desk = accepted(gate);

    same(desk.heard(), std::string{"CONNECT kino.example:8443 HTTP/1.1\r\nHost: kino.example:8443\r\n\r\n"});

    desk.wrote("HTTP/1.1 200 Connection established\r\nProxy-Agent: próba\r\n\r\n");

    desk.secure = net::TlsStream::server(desk.plain, tls, soon).await();

    same(desk.heard().substr(0, 15), std::string{"GET /tajne HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nszyfr!");

    same(body(got.await()), std::string{"szyfr!"});

    net::HttpClient fresh{tls, {}, net::Proxy::from_strings("", proxy, "")};

    auto shut = fresh.get(asked("https://inne.example/"));

    Desk second = accepted(gate);

    same(second.heard().substr(0, 32), std::string{"CONNECT inne.example:443 HTTP/1."});
    second.wrote("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n");

    same(why(shut), std::optional{net::HttpTrouble::Why::proxy});

    net::HttpClient over{tls, {}, net::Proxy::from_strings("", "https://127.0.0.1:1", "")};

    auto tls_proxy = over.get(asked("https://inne.example/"));

    same(why(tls_proxy), std::optional{net::HttpTrouble::Why::proxy});
}

// Pośrednik, który po 200 dosyła cokolwiek przed powitaniem TLS, łamie protokół — tunel nie rusza.
TRIAL("Pośrednik: bajty po zgodzie na tunel to złamany protokół") {
    const net::Listener gate = standing();
    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    net::HttpClient client{tls, {}, net::Proxy::from_strings("", "127.0.0.1:" + std::to_string(gate.mine().port()), "")};

    auto got = client.get(asked("https://kino.example/"));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\n\r\nniespodzianka");

    same(why(got), std::optional{net::HttpTrouble::Why::malformed});
}

// Adres w wyjątkach idzie wprost, choćby pośrednik był podany — tu pośrednik to zamknięty port, więc
// żądanie przez niego by odmówiło.
TRIAL("Pośrednik: wyjątek idzie wprost do serwera") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, {}, net::Proxy::from_strings("127.0.0.1:9", "", "127.0.0.1")};

    auto got = client.get(asked(at(gate, "/wprost")));

    Desk desk = accepted(gate);

    same(desk.heard().substr(0, 16), std::string{"GET /wprost HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    same(body(got.await()), std::string{"ok"});
}

// --- Wyścig adresów ------------------------------------------------------------------------------

// Rodziny na przemian, zaczynając od tej, którą system dał pierwszą; kolejność w rodzinie zostaje.
TRIAL("Wyścig: adresy idą rodzinami na przemian") {
    const net::Address six_a{"::1", 1};
    const net::Address six_b{"::2", 2};
    const net::Address six_c{"::3", 3};
    const net::Address four_a{"127.0.0.1", 4};
    const net::Address four_b{"127.0.0.2", 5};

    same(net::interleaved({six_a, six_b, six_c, four_a, four_b}),
         (std::vector<net::Address>{six_a, four_a, six_b, four_b, six_c}));
    same(net::interleaved({four_a, six_a, four_b}), (std::vector<net::Address>{four_a, six_a, four_b}));
    ought(net::interleaved({}).empty());
}

// Port, który odmawia od razu, nie czeka na zegar: następny adres rusza zaraz po odmowie. Odstęp
// jest tu długi, więc gdyby czekał, próba trwałaby dłużej niż jej granica.
TRIAL("Wyścig: odmowa od razu przepuszcza następny adres") {
    std::uint16_t closed = 0;

    {
        const net::Listener gone = standing();

        closed = gone.mine().port();
    }

    const net::Listener gate = standing();

    const Stopwatch watch;

    auto got = net::raced({net::Address{"127.0.0.1", closed}, gate.mine()}, soon, std::chrono::milliseconds{6000});

    Desk desk = accepted(gate);

    net::Stream won = got.await();

    ought(watch.spent() < std::chrono::milliseconds{3000});
    same(static_cast<int>(won.peer().port()), static_cast<int>(gate.mine().port()));

    desk.close();
    won.close();
}

// Adres, który milczy — brama z pełną kolejką połączeń gubi każde następne — nie trzyma wyścigu do
// końca cierpliwości: po odstępie rusza następny i wygrywa, a milczący zostaje przerwany.
TRIAL("Wyścig: milczący adres oddaje pierwszeństwo po odstępie") {
    const net::Listener full{net::Address::loopback(net::Family::four, 0), 0};

    std::vector<Promise<net::Stream>> filled;

    for (int at = 0; at < 4; ++at) {
        filled.push_back(net::Stream::connect(full.mine(), net::Patience{1500}));
        (void)Time::later(std::chrono::milliseconds{30}).await();
    }

    const net::Listener gate = standing();

    const Stopwatch watch;

    auto got = net::raced({full.mine(), gate.mine()}, soon, std::chrono::milliseconds{300});

    Desk desk = accepted(gate);

    net::Stream won = got.await();

    ought(watch.spent() < std::chrono::milliseconds{2500});
    same(static_cast<int>(won.peer().port()), static_cast<int>(gate.mine().port()));

    desk.close();
    won.close();

    for (Promise<net::Stream>& one : filled) {
        try {
            one.await().close();
        } catch (const std::exception&) {
        }
    }
}

// Wszystkie adresy odmawiają: wyścig odrzuca, i to błędem sieci, a nie przerwaniem. Pusty wykaz
// odrzuca od razu.
TRIAL("Wyścig: porażka wszystkich jest porażką całości") {
    std::uint16_t first = 0;
    std::uint16_t second = 0;

    {
        const net::Listener one = standing();
        const net::Listener two = standing();

        first = one.mine().port();
        second = two.mine().port();
    }

    auto got = net::raced({net::Address{"127.0.0.1", first}, net::Address{"127.0.0.1", second}}, soon,
                          std::chrono::milliseconds{5000});

    bool network = false;

    try {
        (void)got.await();
    } catch (const net::Trouble&) {
        network = true;
    } catch (const std::exception&) {
    }

    ought(network);

    auto none = net::raced({}, soon);

    bool empty = false;

    try {
        (void)none.await();
    } catch (const std::exception&) {
        empty = true;
    }

    ought(empty);
}

// Adres, którego nie da się nawet zacząć (pusty — gniazda nie ma jak otworzyć), odmawia od razu,
// jeszcze w chwili budowania ogniw próby. Wyścig nie może przy tym stanąć na własnym zamku, tylko
// idzie dalej, do następnego adresu.
TRIAL("Wyścig: adres odrzucony od razu nie zatrzymuje wyścigu") {
    const net::Listener gate = standing();

    auto got = net::raced({net::Address{}, gate.mine()}, soon, std::chrono::milliseconds{6000});

    Desk desk = accepted(gate);

    net::Stream won = got.await();

    same(static_cast<int>(won.peer().port()), static_cast<int>(gate.mine().port()));

    desk.close();
    won.close();
}

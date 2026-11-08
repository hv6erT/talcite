#include "tal/testing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

// The HTTP client against a server in the trial: a listener on the loopback, and on the other side
// the trial itself reads the request and writes the response byte for byte. That shows exactly what
// the client sends and how it reads what it gets, including malformed responses that a real server
// would not give. HTTPS goes through the fake TLS seam from `faking.hpp`.

namespace {

const net::Patience soon{8000};

// A listener on an IPv4 loopback port chosen by the system.
net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

// The other side of one connection: plain or behind the fake TLS.
struct Desk {
    net::Stream plain;
    net::TlsStream secure;
    std::string kept;

    // Writes the text to the client and waits until it is written.
    void wrote(const std::string& what) {
        if (secure.held()) secure.write(bytes(what), soon).get();
        else plain.write(bytes(what), soon).get();
    }

    // The request head up to the empty line, or nothing when the client closed first.
    std::string heard() {
        for (;;) {
            if (const std::size_t at = kept.find("\r\n\r\n"); at != std::string::npos) {
                std::string made = kept.substr(0, at + 4);

                kept.erase(0, at + 4);

                return made;
            }

            const Bytes got = secure.held() ? secure.read(4096, soon).get() : plain.read(4096, soon).get();

            if (got.empty()) return {};

            kept += text(got);
        }
    }

    // Whether the client closed its side: reading returns nothing. Silence longer than `patience`
    // means "did not close".
    bool ended(net::Patience patience = soon) {
        try {
            const Bytes got = secure.held() ? secure.read(64, patience).get() : plain.read(64, patience).get();

            return got.empty();
        } catch (const Overdue&) {
            return false;
        }
    }

    // The request body of the given length: after the head, from what has already arrived and from
    // further reading.
    std::string taken(std::size_t much) {
        while (kept.size() < much) {
            const Bytes got = secure.held() ? secure.read(4096, soon).get() : plain.read(4096, soon).get();

            if (got.empty()) break;

            kept += text(got);
        }

        std::string made = kept.substr(0, much);

        kept.erase(0, much);

        return made;
    }

    // Closes whichever streams this side holds.
    void close() {
        if (secure.held()) secure.close();
        if (plain.held()) plain.close();
    }
};

// Accepts the next plain connection on the listener.
Desk accepted(const net::Listener& gate) {
    Desk made;

    made.plain = gate.accept(soon).get();

    return made;
}

// Accepts the next connection on the listener and performs the server side of the fake TLS
// handshake.
Desk accepted(const net::Listener& gate, const Seam<net::TlsProvider>& tls) {
    Desk made;

    made.secure = net::TlsStream::server(gate.accept(soon).get(), tls, soon).get();

    return made;
}

// The URL of the target on the listener's loopback port, with the given scheme.
std::string at(const net::Listener& gate, const std::string& target, const char* scheme = "http") {
    return std::string{scheme} + "://127.0.0.1:" + std::to_string(gate.mine().port()) + target;
}

// A request for the URL with the given headers; the URL must parse.
net::Request asked(const std::string& url, std::vector<net::Header> headers = {}) {
    return net::Request{net::Url::from_string(url).value(), std::move(headers)};
}

// The response body as text.
std::string body(const net::Response& one) { return text(one.body); }

// The reason the HTTP client refused, or nothing when it refused with something else (or did not
// refuse).
std::optional<net::HttpTrouble::Why> why(Promise<net::Response>& one) {
    try {
        (void)std::move(one).get();
    } catch (const net::HttpTrouble& trouble) {
        return trouble.why();
    } catch (const std::exception&) {
        return std::nullopt;
    }

    return std::nullopt;
}

// Whether the request ended with any exception.
bool refused(Promise<net::Response>& one) {
    try {
        (void)std::move(one).get();
    } catch (const std::exception&) {
        return true;
    }

    return false;
}

const Seam<net::TlsProvider> nothing{};

}

// --- Url -----------------------------------------------------------------------------------------

// A URL breaks down into the scheme, host, port and target: the default port comes from the scheme,
// the target is at least "/", a query without a path gets a slash, the fragment is dropped, the
// scheme and host are lowercased, and IPv6 in brackets loses the brackets. Written back as text, it
// leaves out a default port.
TRIAL("HTTP: a URL breaks down and is put back together") {
    const auto plain = net::Url::from_string("http://Example.COM");

    OUGHT(plain.has_value());
    OUGHT(!plain->secure);
    SAME(plain->host, std::string{"example.com"});
    SAME(static_cast<int>(plain->port), 80);
    SAME(plain->target, std::string{"/"});
    SAME(net::to_string(*plain), std::string{"http://example.com/"});

    const auto secure = net::Url::from_string("HTTPS://api.themoviedb.org/3/movie/550?api_key=x#góra");

    OUGHT(secure.has_value());
    OUGHT(secure->secure);
    SAME(static_cast<int>(secure->port), 443);
    SAME(secure->target, std::string{"/3/movie/550?api_key=x"});
    SAME(net::to_string(*secure), std::string{"https://api.themoviedb.org/3/movie/550?api_key=x"});

    const auto ported = net::Url::from_string("http://localhost:8080?q=1");

    OUGHT(ported.has_value());
    SAME(static_cast<int>(ported->port), 8080);
    SAME(ported->target, std::string{"/?q=1"});
    SAME(net::to_string(*ported), std::string{"http://localhost:8080/?q=1"});

    const auto six = net::Url::from_string("https://[::1]:8443/a");

    OUGHT(six.has_value());
    SAME(six->host, std::string{"::1"});
    SAME(static_cast<int>(six->port), 8443);
    SAME(net::to_string(*six), std::string{"https://[::1]:8443/a"});

    SAME(net::Url::from_string("http://ZOO.Example.pl/").value().host, std::string{"zoo.example.pl"});
    SAME(net::to_string(*net::Url::from_string("https://[::1]/")), std::string{"https://[::1]/"});
    SAME(net::to_string(*net::Url::from_string("http://a:443/")), std::string{"http://a:443/"});
    SAME(net::to_string(*net::Url::from_string("https://a:80/")), std::string{"https://a:80/"});
}

// What a URL does not accept: another scheme, no scheme, a user in the URL, an empty host, port
// zero, a port too large or not numeric, an empty one after the colon, IPv6 without brackets,
// spaces, control characters and non-ASCII bytes in the target (the caller encodes those).
TRIAL("HTTP: a malformed URL is not a URL") {
    for (const char* bad : {"ftp://a/", "a/b", "//a/b", "http://user@a/", "http:///x", "http://a:0/", "http://a:65536/",
                            "http://a:x/", "http://a:/", "http://[::1/", "http://[::1]x/", "http://[::1]:/", "http://::1/",
                            "http://a/b c", "http://a/\x01", "http://a/\x7f", "http://a/zażółć", "http://a b/",
                            "http://[a_b]/", "http://a:80x/", "http://[::1]x80/"})
        OUGHT(!net::Url::from_string(bad).has_value());

    OUGHT(net::Url::from_string("http://moja_nazwa/").has_value());
    OUGHT(net::Url::from_string("http://a:65535/").has_value());
}

// --- Conversation --------------------------------------------------------------------------------

// A plain GET: the request line with the target, Host with the port when it is not the default, the
// caller's headers (except Host, Connection and Accept-Encoding, which the client sets itself) and
// a request for an uncompressed body. The response carries the status, the headers, a body of the
// stated length and the URL it came from; a header is looked up regardless of case.
TRIAL("HTTP: GET sends the request and reads a response of the stated length") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/3/movie/550?language=pl-PL"),
                                {{"Accept", "application/json"}, {"host", "oszust"}, {"Connection", "close"},
                                 {"accept-encoding", "gzip"}}));

    Desk desk = accepted(gate);

    const std::string head = desk.heard();

    SAME(head, "GET /3/movie/550?language=pl-PL HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(gate.mine().port()) +
                   "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n\r\n");

    desk.wrote("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"id\":550}\n");

    const net::Response one = std::move(got).get();

    SAME(one.status, 200);
    SAME(body(one), std::string{"{\"id\":550}\n"});
    OUGHT(one.header("content-TYPE") != nullptr);
    SAME(*one.header("content-type"), std::string{"application/json"});
    OUGHT(one.header("x-nie-ma") == nullptr);
    SAME(net::to_string(one.url), at(gate, "/3/movie/550?language=pl-PL"));
}

// POST (for SOAP in UPnP): the method, the body length from the client rather than from the caller,
// and the body itself. It always goes over a new connection: a request that changes something must
// not land on a pooled connection that dies halfway, because it would silently be sent a second
// time. After the response the connection still goes back to the pool and carries the next GET.
TRIAL("HTTP: POST sends a body, always over a new connection") {
    const net::Listener gate = standing();
    const std::string port = std::to_string(gate.mine().port());

    net::HttpClient client{nothing};

    auto first = client.post(asked(at(gate, "/ctl"), {{"Content-Type", "text/xml"}, {"content-length", "999"}}),
                             bytes("<a>1</a>"));

    Desk desk = accepted(gate);

    SAME(desk.heard(), "POST /ctl HTTP/1.1\r\nHost: 127.0.0.1:" + port +
                           "\r\nContent-Type: text/xml\r\nContent-Length: 8\r\nAccept-Encoding: identity\r\n\r\n");
    SAME(desk.taken(8), std::string{"<a>1</a>"});

    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    SAME(body(std::move(first).get()), std::string{"ok"});

    auto second = client.post(asked(at(gate, "/ctl")), Bytes{});

    Desk other = accepted(gate);

    SAME(other.heard().substr(0, 34), std::string{"POST /ctl HTTP/1.1\r\nHost: 127.0.0."});
    other.wrote("HTTP/1.1 500 Internal\r\nContent-Length: 0\r\n\r\n");

    SAME(std::move(second).get().status, 500);

    // The method and body fields of the request mean nothing to `get`: it is always a GET without a
    // body.
    net::Request odd = asked(at(gate, "/x"));

    odd.method = "DELETE";
    odd.body = bytes("nie");

    auto third = client.get(odd);

    // The pool keeps one connection per server: the last one that came back.
    const std::string head = other.heard();

    SAME(head.substr(0, 11), std::string{"GET /x HTTP"});
    OUGHT(head.find("Content-Length") == std::string::npos);

    other.wrote("HTTP/1.1 204 No Content\r\n\r\n");

    SAME(std::move(third).get().status, 204);
}

// 307 and 308 repeat a POST together with its body; 301, 302 and 303 turn it into a GET without a
// body.
TRIAL("HTTP: redirecting a POST, 307 with the body, 303 as a GET") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.post(asked(at(gate, "/a")), bytes("treść"));

    Desk desk = accepted(gate);

    (void)desk.heard();
    SAME(desk.taken(7), std::string{"treść"});
    desk.wrote("HTTP/1.1 307 Temporary\r\nLocation: /b\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");

    Desk second = accepted(gate);

    const std::string again = second.heard();

    SAME(again.substr(0, 12), std::string{"POST /b HTTP"});
    OUGHT(again.find("Content-Length: 7") != std::string::npos);
    SAME(second.taken(7), std::string{"treść"});
    second.wrote("HTTP/1.1 303 See Other\r\nLocation: /c\r\nContent-Length: 0\r\n\r\n");

    const std::string last = second.heard();

    SAME(last.substr(0, 11), std::string{"GET /c HTTP"});
    OUGHT(last.find("Content-Length") == std::string::npos);
    second.wrote("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");

    SAME(std::move(got).get().status, 200);
}

// A status is not an error: 404 and 429 come back as a response, with the Retry-After header along
// with it. A header value loses surrounding spaces and tabs; a name may have digits and token
// characters; a status line may have no reason phrase.
TRIAL("HTTP: 404 and 429 are responses, not errors") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/a")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");

    SAME(std::move(first).get().status, 404);

    auto second = client.get(asked(at(gate, "/b")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 429\r\nRetry-After: \t 2 \t\r\nX-Limit-2: 5\r\nX.Dziwna_Nazwa!#: 1\r\nContent-Length: 0\r\n\r\n");

    const net::Response two = std::move(second).get();

    SAME(two.status, 429);
    SAME(*two.header("retry-after"), std::string{"2"});
    OUGHT(two.header("x-limit-2") != nullptr);
    OUGHT(two.header("x.dziwna_nazwa!#") != nullptr);
}

// A chunked body: lengths in hexadecimal, extensions after a semicolon, the zero chunk and the
// trailer. Uppercase letters and empty list entries in the coding name change nothing. After the
// trailer the connection is clean and carries the next request.
TRIAL("HTTP: a chunked body is joined into a whole") {
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

    SAME(body(std::move(got).get()), std::string{"Ala ma kota i psa."});

    auto again = client.get(asked(at(gate, "/2")));

    SAME(desk.heard().substr(0, 10), std::string{"GET /2 HTT"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    SAME(body(std::move(again).get()), std::string{"ok"});
}

// A file of the trials for downloads, in a directory of its own, removed first so each trial
// starts without it.
std::filesystem::path saved_as(const std::string& name) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "talcite-fetching";

    std::error_code why;

    std::filesystem::create_directories(folder, why);
    std::filesystem::remove(folder / name, why);

    return folder / name;
}

// The contents of a file, empty when there is none.
std::string inside(const std::filesystem::path& where) {
    std::ifstream file{where, std::ios::binary};

    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The files beside the targets of downloads, left in the trials' folder: none may stay.
std::size_t leftovers() {
    std::size_t made = 0;

    std::error_code why;

    for (const auto& one : std::filesystem::directory_iterator{std::filesystem::temp_directory_path() / "talcite-fetching", why})
        if (one.path().extension() == ".part") ++made;

    return made;
}

// A download takes the body of a 2xx answer into the file, beyond the body limit, whether it comes
// by length or in chunks, and through a redirect; the answer comes back with an empty body and the
// progress reaches the end. The connection goes back to the pool afterwards.
TRIAL("HTTP: a download takes the body into a file, beyond the body limit, by length, in chunks and through a redirect") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, net::Limits{4096, 10, 5, soon}};

    const std::string whole(300'000, 'z');
    const std::filesystem::path first = saved_as("dlugi.bin");

    auto got = client.save(asked(at(gate, "/plik")), first);

    Desk desk = accepted(gate);

    SAME(desk.heard().substr(0, 14), std::string{"GET /plik HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(whole.size()) + "\r\n\r\n" + whole);

    for (int tries = 0; tries < 5000 && !got.ready(); ++tries) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    SAME(got.progress().fraction(), 1.0);

    const net::Response one = std::move(got).get();

    SAME(one.status, 200);
    OUGHT(one.body.empty());
    SAME(inside(first), whole);

    // The next request on the same connection: in chunks, through a redirect to another target.
    const std::filesystem::path second = saved_as("kawalki.bin");

    auto again = client.save(asked(at(gate, "/stary")), second);

    SAME(desk.heard().substr(0, 15), std::string{"GET /stary HTTP"});
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /nowy\r\nContent-Length: 4\r\n\r\ntu: ");
    SAME(desk.heard().substr(0, 14), std::string{"GET /nowy HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
               "5\r\nAla m\r\nD\r\na kota i psa.\r\n0\r\n\r\n");

    SAME(std::move(again).get().status, 200);
    SAME(inside(second), std::string{"Ala ma kota i psa."});
}

// An answer other than 2xx comes back as usual, with its body in memory and within the limit, and
// leaves no file; a connection cut in the middle of the body is refused and leaves no file either,
// so a file at the place is always a whole body.
TRIAL("HTTP: a download that is not 2xx or is cut off leaves no file") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    const std::filesystem::path missing = saved_as("brak.bin");

    auto got = client.save(asked(at(gate, "/brak")), missing);

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\n\r\nbrak!");

    const net::Response one = std::move(got).get();

    SAME(one.status, 404);
    SAME(body(one), std::string{"brak!"});
    OUGHT(!std::filesystem::exists(missing));

    const std::filesystem::path cut = saved_as("urwany.bin");

    auto broken = client.save(asked(at(gate, "/urwany")), cut);

    SAME(desk.heard().substr(0, 16), std::string{"GET /urwany HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npoczatek");
    desk.close();

    SAME(why(broken), std::optional{net::HttpTrouble::Why::malformed});
    OUGHT(!std::filesystem::exists(cut));
    SAME(leftovers(), std::size_t{0});
}

// A download over a file that already stands leaves it as it was, byte for byte, when the answer is
// not 2xx or the body is cut off: a poster that fails to come again is not lost. A whole 2xx body
// replaces it at once.
TRIAL("HTTP: a failed download leaves the file from before whole, and a whole one replaces it") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    const std::filesystem::path poster = saved_as("plakat.jpg");

    {
        std::ofstream{poster, std::ios::binary} << "stary plakat";
    }

    auto missing = client.save(asked(at(gate, "/brak")), poster);

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\n\r\nbrak!");

    SAME(std::move(missing).get().status, 404);
    SAME(inside(poster), std::string{"stary plakat"});

    auto broken = client.save(asked(at(gate, "/urwany")), poster);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npoczatek");
    desk.close();

    SAME(why(broken), std::optional{net::HttpTrouble::Why::malformed});
    SAME(inside(poster), std::string{"stary plakat"});

    auto whole = client.save(asked(at(gate, "/nowy")), poster);

    Desk again = accepted(gate);

    (void)again.heard();
    again.wrote("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nnowy plakat");

    SAME(std::move(whole).get().status, 200);
    SAME(inside(poster), std::string{"nowy plakat"});
    SAME(leftovers(), std::size_t{0});
}

// A download cancelled while the body comes stops at once, from the pool, and leaves no file.
TRIAL("HTTP: a cancelled download stops and leaves no file") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    const std::filesystem::path place = saved_as("przerwany.bin");

    auto got = client.save(asked(at(gate, "/wolny")), place);

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n" + std::string(1000, 'a'));

    for (int tries = 0; tries < 2000 && got.progress().fraction() == 0.0; ++tries) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    OUGHT(got.progress().fraction() > 0.0 && got.progress().fraction() < 1.0);

    got.cancel();

    for (int tries = 0; tries < 2000 && leftovers() > 0; ++tries) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    OUGHT(!std::filesystem::exists(place));
    SAME(leftovers(), std::size_t{0});

    desk.close();
}

// A response without a length and without chunks lasts until the connection closes, and that
// connection cannot be used a second time, so the next request goes over a new one. The same goes
// for HTTP/1.0 and Connection: close.
TRIAL("HTTP: a body until close, HTTP/1.0 and Connection: close do not return to the pool") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\n\r\nwszystko, co jest");
    one.close();

    SAME(body(std::move(first).get()), std::string{"wszystko, co jest"});

    auto second = client.get(asked(at(gate, "/2")));

    Desk two = accepted(gate);

    (void)two.heard();
    two.wrote("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok");

    SAME(body(std::move(second).get()), std::string{"ok"});
    OUGHT(two.ended());

    auto third = client.get(asked(at(gate, "/3")));

    Desk three = accepted(gate);

    (void)three.heard();
    three.wrote("HTTP/1.1 200 OK\r\nConnection: keep-alive, Close\r\nContent-Length: 2\r\n\r\nok");

    SAME(body(std::move(third).get()), std::string{"ok"});
    OUGHT(three.ended());

    auto fourth = client.get(asked(at(gate, "/4")));

    Desk four = accepted(gate);

    SAME(four.heard().substr(0, 10), std::string{"GET /4 HTT"});
    four.wrote("HTTP/1.1 204 No Content\r\n\r\n");

    SAME(std::move(fourth).get().status, 204);
}

// One connection per server: after a response with a known end the connection waits in the pool and
// the next request goes over it, so the listener does not accept a second one. Responses 204 and
// 304 have no body, even without a length.
TRIAL("HTTP: a connection returns to the pool and carries the next requests") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 204 No Content\r\n\r\n");

    SAME(std::move(first).get().status, 204);

    auto second = client.get(asked(at(gate, "/2")));

    SAME(desk.heard().substr(0, 10), std::string{"GET /2 HTT"});
    desk.wrote("HTTP/1.1 304 Not Modified\r\n\r\n");

    SAME(std::move(second).get().status, 304);

    auto third = client.get(asked(at(gate, "/3")));

    SAME(desk.heard().substr(0, 10), std::string{"GET /3 HTT"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntak");

    SAME(body(std::move(third).get()), std::string{"tak"});
}

// Bytes beyond the announced length are not the next response: a connection with them does not
// return to the pool, and the next request goes over a new one.
TRIAL("HTTP: excess bytes close the connection") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nab");

    SAME(body(std::move(first).get()), std::string{"a"});

    auto second = client.get(asked(at(gate, "/2")));

    Desk two = accepted(gate);

    SAME(two.heard().substr(0, 10), std::string{"GET /2 HTT"});
    two.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nc");

    SAME(body(std::move(second).get()), std::string{"c"});
    OUGHT(one.ended());
}

// A pooled connection that the server closed while idle fails before the first byte of the
// response: the request is then sent once more, over a new connection, and the user notices
// nothing.
TRIAL("HTTP: a connection closed while idle is replaced by a new one") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na");

    SAME(body(std::move(first).get()), std::string{"a"});

    one.close();

    auto second = client.get(asked(at(gate, "/2")));

    Desk two = accepted(gate);

    SAME(two.heard().substr(0, 10), std::string{"GET /2 HTT"});
    two.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nb");

    SAME(body(std::move(second).get()), std::string{"b"});
}

// A pooled connection that died in the middle of a response is not an idle one: the request is not
// repeated, because the server may already have carried it out.
TRIAL("HTTP: a connection cut in the middle of a response does not repeat the request") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));

    Desk one = accepted(gate);

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\na");

    (void)std::move(first).get();

    auto second = client.get(asked(at(gate, "/2")));

    (void)one.heard();
    one.wrote("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\npół");
    one.close();

    SAME(why(second), std::optional{net::HttpTrouble::Why::malformed});
}

// 1xx responses before the real one are skipped.
TRIAL("HTTP: an interim 1xx response lets the real one through") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    const net::Response one = std::move(got).get();

    SAME(one.status, 200);
    SAME(body(one), std::string{"ok"});
}

// --- Limits --------------------------------------------------------------------------------------

// A head over the limit, a body over the limit: announced by the length (refused before anything
// arrives), chunked, and until close.
TRIAL("HTTP: limits of the head and the body") {
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

    SAME(tried("HTTP/1.1 200 OK\r\nX-Długi: " + std::string(100, 'x') + "\r\n\r\n"),
         std::optional{net::HttpTrouble::Why::oversized});
    SAME(tried("HTTP/1.1 200 OK\r\nX-Bez-Końca: " + std::string(100, 'x')),
         std::optional{net::HttpTrouble::Why::oversized});
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n"), std::optional{net::HttpTrouble::Why::oversized});
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nabcdef\r\n5\r\nghijk\r\n0\r\n\r\n"),
         std::optional{net::HttpTrouble::Why::oversized});
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nffffffffffffffff\r\n"),
         std::optional{net::HttpTrouble::Why::oversized});
    SAME(tried("HTTP/1.1 200 OK\r\n\r\n" + std::string(11, 'x')), std::optional{net::HttpTrouble::Why::oversized});

    auto exact = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");

    SAME(body(std::move(exact).get()), std::string{"0123456789"});
}

// Responses that cannot be read: a status line not from HTTP/1.x, a code that is not a number, a
// header without a colon, a folded header, conflicting lengths, a coding other than chunked, a
// chunk length that is not a number, a chunk longer than announced, a chunk line that is too long,
// a connection closed in the head and in the body, two interim responses in a row.
TRIAL("HTTP: a malformed response is a protocol violation") {
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

    SAME(tried("SPDY/3 200 OK\r\n\r\n"), bad);
    SAME(tried("HTTP/1.2 200 OK\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1_200 OK\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 20\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nZła nazwa: x\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Length: 2x\r\n\r\nab"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5x\r\nabcde\r\n0\r\n\r\n"), bad);
    SAME(tried("HTTP/2.0 200 OK\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 2x0 OK\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 099 Za mało\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200OK\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 20\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nBez dwukropka\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\n: pusta nazwa\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nX-A: 1\r\n  zawinięte\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Length: dwa\r\n\r\nab"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabc\r\n0\r\n\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string(5000, '1')), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Le"), bad);
    SAME(tried("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab"), bad);
    SAME(tried("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 102 Processing\r\n\r\nHTTP/1.1 200 OK\r\n\r\n"), bad);
}

// Silence from the server ends after `patience` with a refusal, not with waiting forever.
TRIAL("HTTP: a silent server ends with a timeout") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, net::Limits{1024, 1024, 5, net::Patience{200}}};

    auto got = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();

    OUGHT(refused(got));
    OUGHT(!why(got).has_value());
}

// --- Redirects -----------------------------------------------------------------------------------

// Redirects of all five kinds, with a URL from the root, a relative one (a slash in the query is
// not a path), one without a scheme and a full one; the response says where it ended. Credentials
// go on only to the same server.
TRIAL("HTTP: redirects lead to the target, and credentials stay with their server") {
    const net::Listener gate = standing();

    const std::string port = std::to_string(gate.mine().port());

    net::HttpClient client{nothing};

    auto got = client.get(asked(at(gate, "/a/b?x=/1"), {{"Authorization", "Bearer sekret"}, {"X-Inny", "1"}}));

    Desk desk = accepted(gate);

    SAME(desk.heard().substr(0, 17), std::string{"GET /a/b?x=/1 HTT"});
    desk.wrote("HTTP/1.1 301 Moved\r\nLocation: c/d\r\nContent-Length: 0\r\n\r\n");

    const std::string second = desk.heard();

    SAME(second.substr(0, 14), std::string{"GET /a/c/d HTT"});
    OUGHT(second.find("Authorization: Bearer sekret") != std::string::npos);
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /e#kotwica\r\nContent-Length: 0\r\n\r\n");

    SAME(desk.heard().substr(0, 10), std::string{"GET /e HTT"});
    desk.wrote("HTTP/1.1 303 See Other\r\nLocation: //127.0.0.1:" + port + "/f\r\nContent-Length: 0\r\n\r\n");

    SAME(desk.heard().substr(0, 10), std::string{"GET /f HTT"});
    desk.wrote("HTTP/1.1 307 Temporary\r\nLocation: http://localhost:" + port + "/g\r\nContent-Length: 0\r\n\r\n");

    Desk other = accepted(gate);

    const std::string moved = other.heard();

    SAME(moved.substr(0, 10), std::string{"GET /g HTT"});
    OUGHT(moved.find("Host: localhost:" + port) != std::string::npos);
    OUGHT(moved.find("Authorization") == std::string::npos);
    OUGHT(moved.find("X-Inny: 1") != std::string::npos);
    other.wrote("HTTP/1.1 308 Permanent\r\nLocation: /h\r\nContent-Length: 0\r\n\r\n");

    SAME(other.heard().substr(0, 10), std::string{"GET /h HTT"});
    other.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    const net::Response one = std::move(got).get();

    SAME(one.status, 200);
    SAME(net::to_string(one.url), "http://localhost:" + port + "/h");
}

// A redirect without Location is an ordinary response; a Location that is not a URL is a protocol
// violation; more redirects than the limit allows is a refusal.
TRIAL("HTTP: a redirect without a target, with a malformed target and over the limit") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, net::Limits{64 * 1024, 1024, 1, soon}};

    auto bare = client.get(asked(at(gate, "/")));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n");

    SAME(std::move(bare).get().status, 302);

    auto crooked = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: ftp://a/b\r\nContent-Length: 0\r\n\r\n");

    SAME(why(crooked), std::optional{net::HttpTrouble::Why::malformed});

    auto empty = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: \r\nContent-Length: 0\r\n\r\n");

    SAME(why(empty), std::optional{net::HttpTrouble::Why::malformed});

    auto spaced = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /a b\r\nContent-Length: 0\r\n\r\n");

    SAME(why(spaced), std::optional{net::HttpTrouble::Why::malformed});

    auto looped = client.get(asked(at(gate, "/")));

    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /1\r\nContent-Length: 0\r\n\r\n");
    (void)desk.heard();
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: /2\r\nContent-Length: 0\r\n\r\n");

    SAME(why(looped), std::optional{net::HttpTrouble::Why::redirected});
}

// --- HTTPS ---------------------------------------------------------------------------------------

// HTTPS goes through the TLS seam: without a provider it is refused before anything goes out on the
// network; with a provider it is the same conversation over an encrypted connection, which also
// returns to the pool, and a redirect without a scheme stays on HTTPS. A redirect from HTTPS to
// plain HTTP is refused.
TRIAL("HTTP: HTTPS through the TLS seam, refused without it, and a downgrade to HTTP stopped") {
    const net::Listener gate = standing();

    net::HttpClient bare{nothing};

    auto refused_now = bare.get(asked(at(gate, "/", "https")));

    SAME(why(refused_now), std::optional{net::HttpTrouble::Why::insecure});

    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    net::HttpClient client{tls};

    auto got = client.get(asked(at(gate, "/tajne", "https")));

    Desk desk = accepted(gate, tls);

    SAME(desk.heard().substr(0, 15), std::string{"GET /tajne HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nszyfr!");

    SAME(body(std::move(got).get()), std::string{"szyfr!"});

    auto jump = client.get(asked(at(gate, "/skok", "https")));

    SAME(desk.heard().substr(0, 14), std::string{"GET /skok HTTP"});
    desk.wrote("HTTP/1.1 302 Found\r\nLocation: //127.0.0.1:" + std::to_string(gate.mine().port()) +
               "/cel\r\nContent-Length: 0\r\n\r\n");

    SAME(desk.heard().substr(0, 13), std::string{"GET /cel HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    SAME(net::to_string(std::move(jump).get().url), at(gate, "/cel", "https"));

    auto down = client.get(asked(at(gate, "/znowu", "https")));

    SAME(desk.heard().substr(0, 15), std::string{"GET /znowu HTTP"});
    desk.wrote("HTTP/1.1 301 Moved\r\nLocation: http://127.0.0.1/\r\nContent-Length: 0\r\n\r\n");

    SAME(why(down), std::optional{net::HttpTrouble::Why::downgraded});

    net::HttpClient plain{nothing};

    auto upward = plain.get(asked(at(gate, "/")));

    Desk third = accepted(gate);

    (void)third.heard();
    third.wrote("HTTP/1.1 301 Moved\r\nLocation: https://127.0.0.1/\r\nContent-Length: 0\r\n\r\n");

    SAME(why(upward), std::optional{net::HttpTrouble::Why::insecure});
}

// A body without a length lasts until the connection closes. Over TLS that end is whole only when
// the server said goodbye first (close_notify); a connection that simply breaks off may have lost
// the rest of the body on the way, so it is refused instead of passed off as the whole answer.
TRIAL("HTTP: a body until close over TLS is whole only after the server's close notice") {
    const net::Listener gate = standing();

    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    net::HttpClient client{tls};

    auto cut = client.get(asked(at(gate, "/urwany", "https")));

    {
        Desk desk = accepted(gate, tls);

        (void)desk.heard();
        desk.wrote("HTTP/1.1 200 OK\r\n\r\npołowa");
        desk.close();
    }

    SAME(why(cut), std::optional{net::HttpTrouble::Why::malformed});

    auto whole = client.get(asked(at(gate, "/caly", "https")));

    Desk desk = accepted(gate, tls);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\n\r\ncałość");
    desk.wrote("ZAMKNIJ");
    desk.close();

    SAME(body(std::move(whole).get()), std::string{"całość"});
}

// A handshake refusal with a reason passes through the HTTP client untouched: the caller gets a
// `TlsTrouble` with the reason from the adapter and the server name from the URL, not just a
// message.
TRIAL("HTTP: a TLS handshake refusal reaches the caller with the reason and the server name") {
    const net::Listener gate = standing();

    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>(false, net::TlsTrouble::Why::untrusted)};

    net::HttpClient client{tls};

    auto got = client.get(asked(at(gate, "/", "https")));

    bool refused = false;

    try {
        (void)std::move(got).get();
    } catch (const net::TlsTrouble& trouble) {
        refused = true;

        SAME(trouble.why(), net::TlsTrouble::Why::untrusted);
        SAME(trouble.host(), std::string{"127.0.0.1"});
    }

    OUGHT(refused);
}

// --- The caller ----------------------------------------------------------------------------------

// A caller's header that breaks the line or has a name outside the token characters does not go out
// on the network: that would inject a second request. An empty client refuses, and a closed port is
// a network refusal.
TRIAL("HTTP: a header that breaks the line, an empty client and a closed port") {
    net::HttpClient client{nothing};

    for (const net::Header& bad : {net::Header{"X-A", "1\r\nGET /inne HTTP/1.1"}, net::Header{"X-A", "a\nb"}, net::Header{"X-A", "a\rb"},
                                   net::Header{"X A", "1"}, net::Header{"", "1"}, net::Header{"X-A", std::string{"a\0b", 3}}}) {
        auto got = client.get(net::Request{*net::Url::from_string("http://127.0.0.1:9/"), {bad}});

        SAME(why(got), std::optional{net::HttpTrouble::Why::malformed});
    }

    net::HttpClient empty;

    OUGHT(!empty.held());

    auto none = empty.get(asked("http://127.0.0.1/"));

    OUGHT(refused(none));

    std::uint16_t port = 0;

    {
        const net::Listener gate = standing();

        port = gate.mine().port();
    }

    auto closed = client.get(asked("http://127.0.0.1:" + std::to_string(port) + "/"));

    OUGHT(refused(closed));
    OUGHT(!why(closed).has_value());
}

// Two requests at once to one server go over two connections; one returns to the pool and the other
// is closed, since the pool keeps one connection per server.
TRIAL("HTTP: two requests at once, one connection in the pool") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing};

    auto first = client.get(asked(at(gate, "/1")));
    auto second = client.get(asked(at(gate, "/2")));

    Desk one = accepted(gate);
    Desk two = accepted(gate);

    // Only the system knows which connection carries which request; the response repeats the path.
    for (Desk* desk : {&one, &two}) {
        const std::string path = desk->heard().substr(4, 2);

        desk->wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n" + path);
    }

    SAME(body(std::move(first).get()), std::string{"/1"});
    SAME(body(std::move(second).get()), std::string{"/2"});

    const int closed = static_cast<int>(one.ended(net::Patience{300})) + static_cast<int>(two.ended(net::Patience{300}));

    SAME(closed, 1);
}

// --- Proxy ---------------------------------------------------------------------------------------

// A proxy written as in the environment variables: without a scheme it is HTTP, the user and
// password before `@` with percent-encoding, the path means nothing; SOCKS and a malformed form
// give none. Exceptions: separated by commas and spaces, lowercased, without the port, `*.` the
// same as `.`.
TRIAL("Proxy: the form from the environment breaks down into the address, credentials and exceptions") {
    const net::Proxy made = net::Proxy::from_strings("Pośrednik", "ala:ma%40kota@Proxy.Example:3128/cokolwiek",
                                                     " .Wewn.Example, localhost:8080  *.lan,,[::1]:80 ");

    OUGHT(!made.plain.has_value());
    OUGHT(made.secure.has_value());
    SAME(made.secure->url.host, std::string{"proxy.example"});
    SAME(static_cast<int>(made.secure->url.port), 3128);
    OUGHT(!made.secure->url.secure);
    SAME(made.secure->login, std::string{"ala:ma@kota"});
    SAME(made.bypass, (std::vector<std::string>{".wewn.example", "localhost", ".lan", "::1"}));

    const net::Proxy bare = net::Proxy::from_strings("pośrednik.lan:8080", "socks5://pośrednik:1080", "");

    OUGHT(!bare.plain.has_value());
    OUGHT(!bare.secure.has_value());

    const net::Proxy plain = net::Proxy::from_strings("posrednik.lan:8080", "https://posrednik.lan", "*");

    OUGHT(plain.plain.has_value());
    SAME(plain.plain->url.host, std::string{"posrednik.lan"});
    SAME(static_cast<int>(plain.plain->url.port), 8080);
    SAME(plain.plain->login, std::string{});
    OUGHT(plain.secure.has_value());
    OUGHT(plain.secure->url.secure);
    SAME(static_cast<int>(plain.secure->url.port), 443);
}

namespace {

// An environment variable replaced for the duration of the trial and restored after it. An empty
// string means it is not set.
class Planted {
public:
    // Remembers the variable's current value and sets the new one.
    Planted(const char* name, const char* value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = was;

        put(value);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    // Restores the remembered value, or unsets the variable if it had none.
    ~Planted() { put(was_ ? was_->c_str() : ""); }

private:
    // Sets the variable to the value, or unsets it when the value is empty.
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

// On every system: the HTTPS proxy from its own variable, all_proxy for HTTP when there is no
// separate one, and the exceptions from no_proxy.
TRIAL("Proxy: the environment gives the proxy, the fallback and the exceptions") {
    const Planted plain{"http_proxy", ""};
    const Planted secure{"https_proxy", "http://male:2"};
    const Planted all{"all_proxy", "http://wszystkie:4"};
    const Planted no{"no_proxy", "lan,.dom"};

    const net::Proxy made = net::Proxy::from_environment();

    OUGHT(made.secure.has_value());
    SAME(made.secure->url.host, std::string{"male"});
    OUGHT(made.plain.has_value());
    SAME(made.plain->url.host, std::string{"wszystkie"});
    SAME(made.bypass, (std::vector<std::string>{"lan", ".dom"}));
}

#ifndef _WIN32

// Variables as in curl: lowercase before uppercase, all_proxy where there is no separate one, and
// no uppercase HTTP_PROXY, because under CGI it is set by someone else's request. On Windows the
// case of variable names means nothing, so there is no such trial there.
TRIAL("Proxy: the environment as in curl") {
    const Planted plain_low{"http_proxy", ""};
    const Planted plain_high{"HTTP_PROXY", "http://wielkie:1"};
    const Planted secure_low{"https_proxy", "http://male:2"};
    const Planted secure_high{"HTTPS_PROXY", "http://wielkie:3"};
    const Planted all_low{"all_proxy", ""};
    const Planted all_high{"ALL_PROXY", "http://wszystkie:4"};
    const Planted no_low{"no_proxy", ""};
    const Planted no_high{"NO_PROXY", "lan"};

    const net::Proxy made = net::Proxy::from_environment();

    OUGHT(made.secure.has_value());
    SAME(made.secure->url.host, std::string{"male"});
    OUGHT(made.plain.has_value());
    SAME(made.plain->url.host, std::string{"wszystkie"});
    SAME(made.bypass, std::vector<std::string>{"lan"});

    const Planted secure_gone{"https_proxy", ""};
    const Planted all_set{"all_proxy", "http://male-wszystkie:5"};
    const Planted no_set{"no_proxy", "*"};

    const net::Proxy second = net::Proxy::from_environment();

    SAME(second.secure->url.host, std::string{"wielkie"});
    SAME(second.plain->url.host, std::string{"male-wszystkie"});
    SAME(second.bypass, std::vector<std::string>{"*"});
}

#endif

// An exception covers the name and its subdomains, but not a name that merely ends with the same
// letters; `*` covers everything. HTTP and HTTPS each take their own proxy.
TRIAL("Proxy: exceptions bypass the proxy, the rest goes to the right one") {
    const net::Proxy made = net::Proxy::from_strings("http://jeden:1", "http://dwa:2", "example.com,.lan");

    const auto via = [&made](const char* url) -> std::string {
        const net::Gateway* one = made.chosen(*net::Url::from_string(url));

        return one != nullptr ? one->url.host : std::string{"wprost"};
    };

    SAME(via("http://inny.pl/"), std::string{"jeden"});
    SAME(via("https://inny.pl/"), std::string{"dwa"});
    SAME(via("https://example.com/"), std::string{"wprost"});
    SAME(via("https://api.example.com/"), std::string{"wprost"});
    SAME(via("https://notexample.com/"), std::string{"dwa"});
    SAME(via("http://nas.lan/"), std::string{"wprost"});
    SAME(via("http://lan/"), std::string{"wprost"});

    const net::Proxy all = net::Proxy::from_strings("http://jeden:1", "", "*");

    OUGHT(all.chosen(*net::Url::from_string("http://inny.pl/")) == nullptr);
    OUGHT(net::Proxy{}.chosen(*net::Url::from_string("http://inny.pl/")) == nullptr);
}

// Plain HTTP through a proxy: the full URL in the request line, the server's Host and Basic
// credentials for the proxy, while credentials given by the caller do not go out.
TRIAL("Proxy: plain HTTP goes with the full URL and credentials") {
    const net::Listener gate = standing();

    const std::string proxy = "ala:ma%40kota@127.0.0.1:" + std::to_string(gate.mine().port());

    net::HttpClient client{nothing, {}, net::Proxy::from_strings(proxy, "", "")};

    auto got = client.get(asked("http://kino.example:8080/film?id=1", {{"Proxy-Authorization", "Basic cudze"}}));

    Desk desk = accepted(gate);

    SAME(desk.heard(), std::string{"GET http://kino.example:8080/film?id=1 HTTP/1.1\r\nHost: kino.example:8080\r\n"
                                   "Proxy-Authorization: Basic YWxhOm1hQGtvdGE=\r\nAccept-Encoding: identity\r\n\r\n"});

    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    SAME(body(std::move(got).get()), std::string{"ok"});
}

// HTTPS through a proxy: CONNECT with the name and port, a 200 response and TLS with the server
// inside the tunnel, so the proxy does not see the request. A refusal of the tunnel (407) is a
// proxy trouble, not a response.
TRIAL("Proxy: HTTPS goes through a CONNECT tunnel, and a refused tunnel is a proxy trouble") {
    const net::Listener gate = standing();
    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    const std::string proxy = "http://127.0.0.1:" + std::to_string(gate.mine().port());

    net::HttpClient client{tls, {}, net::Proxy::from_strings("", proxy, "")};

    auto got = client.get(asked("https://kino.example:8443/tajne"));

    Desk desk = accepted(gate);

    SAME(desk.heard(), std::string{"CONNECT kino.example:8443 HTTP/1.1\r\nHost: kino.example:8443\r\n\r\n"});

    desk.wrote("HTTP/1.1 200 Connection established\r\nProxy-Agent: próba\r\n\r\n");

    desk.secure = net::TlsStream::server(desk.plain, tls, soon).get();

    SAME(desk.heard().substr(0, 15), std::string{"GET /tajne HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nszyfr!");

    SAME(body(std::move(got).get()), std::string{"szyfr!"});

    net::HttpClient fresh{tls, {}, net::Proxy::from_strings("", proxy, "")};

    auto shut = fresh.get(asked("https://inne.example/"));

    Desk second = accepted(gate);

    SAME(second.heard().substr(0, 32), std::string{"CONNECT inne.example:443 HTTP/1."});
    second.wrote("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\n\r\n");

    SAME(why(shut), std::optional{net::HttpTrouble::Why::proxy});

    net::HttpClient over{tls, {}, net::Proxy::from_strings("", "https://127.0.0.1:1", "")};

    auto tls_proxy = over.get(asked("https://inne.example/"));

    SAME(why(tls_proxy), std::optional{net::HttpTrouble::Why::proxy});
}

// A proxy that sends anything after the 200 before the TLS greeting violates the protocol: the
// tunnel does not start.
TRIAL("Proxy: bytes after the tunnel is granted are a protocol violation") {
    const net::Listener gate = standing();
    const Seam<net::TlsProvider> tls{std::make_shared<faking::Watching>()};

    net::HttpClient client{tls, {}, net::Proxy::from_strings("", "127.0.0.1:" + std::to_string(gate.mine().port()), "")};

    auto got = client.get(asked("https://kino.example/"));

    Desk desk = accepted(gate);

    (void)desk.heard();
    desk.wrote("HTTP/1.1 200 OK\r\n\r\nniespodzianka");

    SAME(why(got), std::optional{net::HttpTrouble::Why::malformed});
}

// A URL in the exceptions goes directly, even when a proxy is given; here the proxy is a closed
// port, so a request through it would be refused.
TRIAL("Proxy: an exception goes directly to the server") {
    const net::Listener gate = standing();

    net::HttpClient client{nothing, {}, net::Proxy::from_strings("127.0.0.1:9", "", "127.0.0.1")};

    auto got = client.get(asked(at(gate, "/wprost")));

    Desk desk = accepted(gate);

    SAME(desk.heard().substr(0, 16), std::string{"GET /wprost HTTP"});
    desk.wrote("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    SAME(body(std::move(got).get()), std::string{"ok"});
}

// --- Address race --------------------------------------------------------------------------------

// The families alternate, starting with the one the system gave first; the order within a family
// stays.
TRIAL("Race: addresses alternate by family") {
    const net::Address six_a{"::1", 1};
    const net::Address six_b{"::2", 2};
    const net::Address six_c{"::3", 3};
    const net::Address four_a{"127.0.0.1", 4};
    const net::Address four_b{"127.0.0.2", 5};

    SAME(net::interleaved({six_a, six_b, six_c, four_a, four_b}),
         (std::vector<net::Address>{six_a, four_a, six_b, four_b, six_c}));
    SAME(net::interleaved({four_a, six_a, four_b}), (std::vector<net::Address>{four_a, six_a, four_b}));
    OUGHT(net::interleaved({}).empty());
}

// A port that refuses at once does not wait for the timer: the next address starts right after the
// refusal. The delay is long here, so if it waited, the trial would take longer than its limit.
TRIAL("Race: an immediate refusal lets the next address through") {
    std::uint16_t closed = 0;

    {
        const net::Listener gone = standing();

        closed = gone.mine().port();
    }

    const net::Listener gate = standing();

    const Stopwatch watch;

    auto got = net::raced({net::Address{"127.0.0.1", closed}, gate.mine()}, soon, std::chrono::milliseconds{6000});

    Desk desk = accepted(gate);

    net::Stream won = std::move(got).get();

    OUGHT(watch.spent() < std::chrono::milliseconds{3000});
    SAME(static_cast<int>(won.peer().port()), static_cast<int>(gate.mine().port()));

    desk.close();
    won.close();
}

// An address that stays silent (a listener with a full backlog drops every further connection) does
// not hold the race until the patience runs out: after the delay the next one starts and wins, and
// the silent one is cancelled.
TRIAL("Race: a silent address gives way after the delay") {
    const net::Listener full{net::Address::loopback(net::Family::four, 0), 0};

    std::vector<Promise<net::Stream>> filled;

    for (int at = 0; at < 4; ++at) {
        filled.push_back(net::Stream::connect(full.mine(), net::Patience{1500}));
        (void)later(std::chrono::milliseconds{30}).get();
    }

    const net::Listener gate = standing();

    const Stopwatch watch;

    auto got = net::raced({full.mine(), gate.mine()}, soon, std::chrono::milliseconds{300});

    Desk desk = accepted(gate);

    net::Stream won = std::move(got).get();

    OUGHT(watch.spent() < std::chrono::milliseconds{2500});
    SAME(static_cast<int>(won.peer().port()), static_cast<int>(gate.mine().port()));

    desk.close();
    won.close();

    for (Promise<net::Stream>& one : filled) {
        try {
            std::move(one).get().close();
        } catch (const std::exception&) {
        }
    }
}

// All addresses refuse: the race rejects, with a network error rather than a cancellation. An empty
// list rejects at once.
TRIAL("Race: the failure of all is the failure of the whole") {
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
        (void)std::move(got).get();
    } catch (const net::Trouble&) {
        network = true;
    } catch (const std::exception&) {
    }

    OUGHT(network);

    auto none = net::raced({}, soon);

    bool empty = false;

    try {
        (void)std::move(none).get();
    } catch (const std::exception&) {
        empty = true;
    }

    OUGHT(empty);
}

// An address that cannot even be started (an empty one, for which no socket can be opened) refuses
// at once, while the trial's attempts are still being built. The race must not deadlock on its own
// lock then, but goes on to the next address.
TRIAL("Race: an address refused at once does not stop the race") {
    const net::Listener gate = standing();

    auto got = net::raced({net::Address{}, gate.mine()}, soon, std::chrono::milliseconds{6000});

    Desk desk = accepted(gate);

    net::Stream won = std::move(got).get();

    SAME(static_cast<int>(won.peer().port()), static_cast<int>(gate.mine().port()));

    desk.close();
    won.close();
}

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/http.hpp"
#include "tal/net.hpp"
#include "tal/task.hpp"
#include "tal/tls.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "httping.hpp"
#include "quieting.hpp"
#include "turning.hpp"

// The HTTP client of tal::net against servers the trials write on the loopback, plain or over the
// TLS of `turning.cpp`, and a proxy among them. Nothing goes to the Internet.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::Path;
using tal::net::Header;
using tal::net::Http;
using tal::net::Limits;
using tal::net::Proxy;
using tal::net::Request;
using tal::net::Response;
using tal::net::Url;
using tal::testing::turned;

// What a server does with one request: the bytes it answers, written a byte at a time when
// `piecewise`, and then whether it closes the connection, cuts it under its TLS, speaks TLS from
// there on (`tunnel`) or falls silent for good (`silent`).
struct Reply {
    std::string bytes;
    bool close = false;
    bool cut = false;
    bool tunnel = false;
    bool piecewise = false;
    bool silent = false;
};

using Script = std::function<Reply(const std::string& request, std::size_t nth)>;

// The value of the header `name` in the request `request`, as the trials write them, or nothing.
std::optional<std::string> told(const std::string& request, std::string_view name) {
    const std::string wanted = "\r\n" + std::string{name} + ": ";
    const std::size_t at = request.find(wanted);

    if (at == std::string::npos) return std::nullopt;

    const std::size_t from = at + wanted.size();

    return request.substr(from, request.find("\r\n", from) - from);
}

// The value of the header `name` in the request `request`, or "none".
std::string said(const std::string& request, std::string_view name) { return told(request, name).value_or("none"); }

// A server on the loopback that answers each request with what its script gives, and keeps the
// requests it heard and how many connections came; over the TLS of `turning.cpp` when `secure`.
class Server {
public:
    // Listens on the loopback of `loop` and answers by `script`.
    Server(tal::Loop& loop, Script script, bool secure = false)
        : script_(std::move(script)), secure_(secure), listener_(tal::wait(loop, tal::net::Listener::opened(tal::net::Address::loopback(tal::net::Family::four, 0)))), scope_(loop) {
        scope_.run(accepting());
    }

    // Stops listening; the scope ends the conversations.
    ~Server() { listener_.close(); }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Its port.
    [[nodiscard]] std::uint16_t port() const { return listener_.mine().port(); }

    // The address of `target` on it.
    [[nodiscard]] Url url(std::string_view target = "/") const {
        return *Url::parsed(std::string{secure_ ? "https" : "http"} + "://127.0.0.1:" + std::to_string(port()) + std::string{target});
    }

    std::vector<std::string> heard;
    std::size_t connections = 0;

private:
    // Takes the connections as they come.
    tal::Task<void> accepting() {
        try {
            for (;;) {
                tal::net::Connection one = co_await listener_.accepted();

                ++connections;
                scope_.run(talking(one.stream));
            }
        } catch (const tal::Trouble&) {
        }
    }

    // One connection: requests read whole, with a body of their Content-Length, and answered.
    tal::Task<void> talking(tal::Stream plain) {
        try {
            tal::Stream stream = secure_ ? co_await tal::net::serving(plain, turned()) : plain;
            std::string buffer;

            for (;;) {
                std::size_t end = 0;

                while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
                    const Bytes got = co_await stream.read(4096);

                    if (got.empty()) co_return;

                    buffer += tal::text::of(got);
                }

                std::string request = buffer.substr(0, end + 4);
                const std::size_t length = told(request, "Content-Length") ? std::stoul(*told(request, "Content-Length")) : 0;

                buffer.erase(0, end + 4);

                while (buffer.size() < length) {
                    const Bytes got = co_await stream.read(4096);

                    if (got.empty()) co_return;

                    buffer += tal::text::of(got);
                }

                request += buffer.substr(0, length);
                buffer.erase(0, length);
                heard.push_back(request);

                const Reply reply = script_(request, heard.size() - 1);

                if (reply.piecewise) {
                    for (const char one : reply.bytes) co_await stream.write(tal::bytes(std::string_view{&one, 1}));
                } else if (!reply.bytes.empty()) {
                    co_await stream.write(tal::bytes(reply.bytes));
                }

                if (reply.silent) co_await tal::Pending<void>{}.task();

                if (reply.cut) {
                    plain.close();

                    co_return;
                }

                if (reply.close) {
                    stream.close();

                    co_return;
                }

                if (reply.tunnel) stream = co_await tal::net::serving(stream, turned());
            }
        } catch (const tal::Trouble&) {
        }
    }

    Script script_;
    bool secure_;
    tal::net::Listener listener_;
    tal::Scope scope_;
};

// A plain answer with `status`, `headers` (whole lines) and `body` of its length.
std::string answer(int status, const std::string& body = {}, const std::string& headers = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body;
}

// The request of `method` for `url`.
Request asking(Url url, std::string method = "GET") {
    Request made;

    made.method = std::move(method);
    made.url = std::move(url);

    return made;
}

// A client without TLS and without a proxy, within `most`.
Http plain(Limits most = {}) { return Http{tal::Optional<tal::net::Tls>{}, most, Proxy{}}; }

// A client with the TLS of `turning.cpp`, without a proxy, within `most`.
Http turning(Limits most = {}) { return Http{tal::Optional<tal::net::Tls>{turned().shared()}, most, Proxy{}}; }

// The kind of trouble `work` fails with, as spoken, or "none".
template <typename T>
std::string kind_of(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())};
    }

    return "none";
}

// The kind `kind` as spoken.
std::string named(tal::Kind kind) { return std::string{tal::spoken(kind)}; }

// 300 000 bytes that differ from place to place, more than one read takes.
const std::string& large() {
    static const std::string made = [] {
        std::string text(300000, ' ');

        for (std::size_t at = 0; at < text.size(); ++at) text[at] = static_cast<char>('a' + at * 7 % 26);

        return text;
    }();

    return made;
}

// The text of the body of `got`.
std::string body(const Response& got) { return tal::text::of(got.body); }

// Whether `request` begins with `start`.
bool begins(const std::string& request, std::string_view start) { return request.rfind(start, 0) == 0; }

// The program's files in memory while it lives.
struct Remembering {
    // Puts files in memory in place.
    Remembering() { tal::defaults().set<tal::fs::Files>(std::make_shared<tal::fs::Memory>()); }

    // Gives the disk back.
    ~Remembering() { tal::defaults().clear<tal::fs::Files>(); }

    Remembering(const Remembering&) = delete;
    Remembering& operator=(const Remembering&) = delete;
};

// Whether the file `where` is there.
bool there(tal::Loop& loop, const Path& where) { return kind_of(loop, tal::fs::look(where)) == "none"; }

// The host of the proxy `one`, or "none".
std::string host_of(const std::optional<tal::net::Gateway>& one) { return one ? one->url.host : std::string{"none"}; }

// A variable of the environment set for a trial, and put back as it was after.
class Variable {
public:
    // Sets `name` to `value`; empty takes it away.
    Variable(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name); old != nullptr) old_ = old;

        put(value);
    }

    // Puts it back.
    ~Variable() { put(old_.value_or(std::string{})); }

    Variable(const Variable&) = delete;
    Variable& operator=(const Variable&) = delete;

private:
    // Sets the variable to `value`, or takes it away when empty.
    void put(const std::string& value) const {
#ifdef _WIN32
        (void)_putenv_s(name_, value.c_str());
#else
        if (value.empty()) (void)unsetenv(name_);
        else (void)setenv(name_, value.c_str(), 1);
#endif
    }

    const char* name_;
    std::optional<std::string> old_;
};

}

TRIAL("Url: http and https addresses are read and written back; anything else is refused") {
    const std::optional<Url> one = Url::parsed("HTTP://Example.COM/a?b#c");

    OUGHT(one.has_value());
    OUGHT(!one->secure);
    SAME(one->host, std::string{"example.com"});
    SAME(one->port, std::uint16_t{80});
    SAME(one->target, std::string{"/a?b"});
    SAME(one->text(), std::string{"http://example.com/a?b"});

    SAME(Url::parsed("https://h:8443")->text(), std::string{"https://h:8443/"});
    SAME(Url::parsed("https://h:443/x")->text(), std::string{"https://h/x"});
    SAME(Url::parsed("http://[::1]:81/")->host, std::string{"::1"});
    SAME(Url::parsed("http://[::1]:81/")->text(), std::string{"http://[::1]:81/"});
    SAME(Url::parsed("http://h?q")->target, std::string{"/?q"});
    SAME(Url::parsed("http://a_b.test/")->host, std::string{"a_b.test"});

    for (const char* wrong : {"ftp://h/", "http://", "http://h:0/", "http://h:65536/", "http://h:/", "http://[::1/", "http://[::1]x/", "http://[::1]:/",
                              "http://h x/", "http://h/a b", "h/x", "http://h:8a/", "http://H\xc3\xb3st/"})
        OUGHT(!Url::parsed(wrong).has_value());
}

TRIAL("Url: a redirect's place resolves whole, without a scheme, from the root and relative") {
    const Url base = *Url::parsed("http://h/a/b?q");

    SAME(base.resolved("c")->text(), std::string{"http://h/a/c"});
    SAME(base.resolved("c#f")->text(), std::string{"http://h/a/c"});
    SAME(base.resolved("/d")->text(), std::string{"http://h/d"});
    SAME(base.resolved("//o/e")->text(), std::string{"http://o/e"});
    SAME(base.resolved("https://x:9/")->text(), std::string{"https://x:9/"});
    OUGHT(!base.resolved("").has_value());
    OUGHT(!base.resolved("a b").has_value());
}

TRIAL("Proxy: logins, schemes, and the exceptions of no_proxy and of Windows") {
    const Proxy one = Proxy::parsed("user:p%40ss@proxy:3128", "https://secure.test:8443", "localhost, .inner.test,*.star.test, [::1]:8080, host:99, 10.*");

    OUGHT(one.plain.has_value());
    SAME(one.plain->url.host, std::string{"proxy"});
    SAME(one.plain->url.port, std::uint16_t{3128});
    SAME(one.plain->login, std::string{"user:p@ss"});
    OUGHT(one.secure.has_value() && one.secure->url.secure);

    for (const char* passed : {"http://localhost/", "http://a.inner.test/", "http://inner.test/", "http://b.star.test/", "http://[::1]/", "http://host/", "http://10.1.2.3/"})
        OUGHT(one.chosen(*Url::parsed(passed)) == nullptr);

    OUGHT(one.chosen(*Url::parsed("http://xinner.test/")) == &*one.plain);
    OUGHT(one.chosen(*Url::parsed("https://other.test/")) == &*one.secure);
    OUGHT(one.chosen(*Url::parsed("http://110.1.2.3/")) == &*one.plain);

    OUGHT(!Proxy::parsed("socks5://p:1080", "", "").plain.has_value());
    OUGHT(Proxy::parsed("p:1", "", "*").chosen(*Url::parsed("http://any.test/")) == nullptr);

    const Proxy windows = tal::net::httping::windowsy("proxy:80", "<local>;*.corp.test");

    OUGHT(windows.chosen(*Url::parsed("http://intranet/")) == nullptr);
    OUGHT(windows.chosen(*Url::parsed("http://a.corp.test/")) == nullptr);
    SAME(windows.chosen(*Url::parsed("http://www.test/"))->url.host, std::string{"proxy"});
    SAME(windows.chosen(*Url::parsed("https://www.test/"))->url.host, std::string{"proxy"});

    const Proxy schemes = tal::net::httping::windowsy("http=p1:1;https=p2:2;socks=s:3", "");

    SAME(schemes.plain->url.host, std::string{"p1"});
    SAME(schemes.secure->url.host, std::string{"p2"});

    const Proxy mixed = tal::net::httping::windowsy("https=p2:2 p0:9", "");

    SAME(mixed.plain->url.host, std::string{"p0"});
    SAME(mixed.secure->url.host, std::string{"p2"});
}

TRIAL("Proxy: the environment as curl reads it, without the uppercase HTTP_PROXY where names have case") {
    const Variable a{"http_proxy", ""};
    const Variable c{"https_proxy", ""};
    const Variable e{"all_proxy", "http://all:2"};
    const Variable g{"no_proxy", "a.test"};
#ifndef _WIN32
    const Variable b{"HTTP_PROXY", "http://bad:1"};
    const Variable d{"HTTPS_PROXY", ""};
    const Variable f{"ALL_PROXY", ""};
    const Variable h{"NO_PROXY", ""};
#endif

    Proxy found = Proxy::from_environment();

    SAME(host_of(found.plain), std::string{"all"});
    SAME(host_of(found.secure), std::string{"all"});
    OUGHT(found.bypass == std::vector<std::string>{"a.test"});

    {
        const Variable i{"HTTPS_PROXY", "x:3"};

        found = Proxy::from_environment();

        SAME(host_of(found.secure), std::string{"x"});
        SAME(host_of(found.plain), std::string{"all"});
    }

    const Variable j{"all_proxy", ""};

    found = Proxy::from_environment();

    OUGHT(host_of(found.plain) != "bad");
}

TRIAL("HTTP: a GET brings the status, the headers and a body of its length; tal writes Host and the coding itself") {
    tal::Loop loop;
    Server server{loop, [](const std::string&, std::size_t) { return Reply{answer(200, "hello", "X-Thing:  yes \r\n")}; }};
    Request request = asking(server.url("/path?x=1"));

    request.headers = {Header{"X-Ask", "1"}, Header{"Host", "evil"}, Header{"Accept-Encoding", "gzip"}, Header{"Content-Length", "99"}};

    const Response got = tal::wait(loop, plain().fetch(request));

    SAME(got.status, 200);
    SAME(body(got), std::string{"hello"});
    SAME(*got.header("x-thing"), std::string{"yes"});
    OUGHT(got.header("nothing") == nullptr);
    SAME(got.url.text(), server.url("/path?x=1").text());

    const std::string& heard = server.heard.at(0);

    OUGHT(begins(heard, "GET /path?x=1 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(server.port()) + "\r\n"));
    SAME(said(heard, "X-Ask"), std::string{"1"});
    SAME(said(heard, "Accept-Encoding"), std::string{"identity"});
    OUGHT(heard.find("evil") == std::string::npos);
    OUGHT(heard.find("gzip") == std::string::npos);
    OUGHT(!told(heard, "Content-Length").has_value());
}

TRIAL("HTTP: a chunked body with extensions and a trailer, every answer a byte at a time") {
    tal::Loop loop;
    Server server{loop, [](const std::string&, std::size_t) {
                      return Reply{"HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n5;ext=1\r\nhello\r\nA \r\n0123456789\r\n0\r\nX-Trailer: t\r\n\r\n", false, false, false, true};
                  }};
    const Http http = plain();

    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"hello0123456789"});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"hello0123456789"});
    SAME(server.connections, std::size_t{1});
}

TRIAL("HTTP: a body until the end, and none for HEAD, 204 and 304") {
    tal::Loop loop;
    Server server{loop, [](const std::string& request, std::size_t) {
                      if (begins(request, "HEAD")) return Reply{"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n"};
                      if (begins(request, "GET /204")) return Reply{"HTTP/1.1 204 No\r\n\r\n"};
                      if (begins(request, "GET /304")) return Reply{"HTTP/1.1 304 Same\r\nContent-Length: 7\r\n\r\n"};

                      return Reply{"HTTP/1.1 200 OK\r\n\r\nall of it", true};
                  }};
    const Http http = plain();

    SAME(body(tal::wait(loop, http.fetch(asking(server.url("/end"))))), std::string{"all of it"});

    const Response head = tal::wait(loop, http.fetch(asking(server.url(), "HEAD")));

    SAME(head.status, 200);
    OUGHT(head.body.empty());
    SAME(tal::wait(loop, http.fetch(asking(server.url("/204")))).status, 204);
    SAME(tal::wait(loop, http.fetch(asking(server.url("/304")))).status, 304);
    SAME(server.connections, std::size_t{2});
}

TRIAL("HTTP: one connection carries request after request; close, HTTP/1.0 and bytes left over take a new one") {
    tal::Loop loop;
    Server server{loop, [](const std::string& request, std::size_t) {
                      if (begins(request, "GET /close")) return Reply{answer(200, "c", "Connection: keep-alive, Close\r\n")};
                      if (begins(request, "GET /old")) return Reply{"HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\no"};
                      if (begins(request, "GET /extra")) return Reply{answer(200, "e") + "garbage"};

                      return Reply{answer(200, "k")};
                  }};
    const Http http = plain();

    for (int turn = 0; turn < 3; ++turn) SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"k"});

    SAME(server.connections, std::size_t{1});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url("/close"))))), std::string{"c"});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"k"});
    SAME(server.connections, std::size_t{2});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url("/old"))))), std::string{"o"});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"k"});
    SAME(server.connections, std::size_t{3});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url("/extra"))))), std::string{"e"});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"k"});
    SAME(server.connections, std::size_t{4});
}

TRIAL("HTTP: an idle connection the server closed is dialed again once; one that answered in part is not") {
    tal::Loop loop;
    Server server{loop, [](const std::string&, std::size_t) { return Reply{answer(200, "k"), true}; }};
    const Http http = plain();

    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"k"});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"k"});
    SAME(server.connections, std::size_t{2});

    Server partly{loop, [](const std::string&, std::size_t nth) { return nth == 0 ? Reply{answer(200, "p")} : Reply{"HTTP/1.1 200 OK\r\nContent-Le", true}; }};

    SAME(body(tal::wait(loop, http.fetch(asking(partly.url())))), std::string{"p"});
    SAME(kind_of(loop, http.fetch(asking(partly.url()))), named(tal::Kind::malformed));
    SAME(partly.connections, std::size_t{1});
}

TRIAL("HTTP: POST always on a new connection, with its length and body") {
    tal::Loop loop;
    Server server{loop, [](const std::string&, std::size_t) { return Reply{answer(201, "made")}; }};
    const Http http = plain();

    (void)tal::wait(loop, http.fetch(asking(server.url())));

    Request posted = asking(server.url("/p"), "POST");

    posted.body = tal::bytes("data");

    SAME(tal::wait(loop, http.fetch(posted)).status, 201);
    SAME(server.connections, std::size_t{2});
    SAME(said(server.heard.at(1), "Content-Length"), std::string{"4"});
    OUGHT(server.heard.at(1).ends_with("\r\n\r\ndata"));

    (void)tal::wait(loop, http.fetch(asking(server.url("/e"), "POST")));

    SAME(said(server.heard.at(2), "Content-Length"), std::string{"0"});
    SAME(server.connections, std::size_t{3});
}

TRIAL("HTTP: redirects are followed; 307 keeps the method and the body, the rest become GET; too many are full") {
    tal::Loop loop;
    Server server{loop, [](const std::string& request, std::size_t) {
                      const std::string method = request.substr(0, request.find(' '));

                      if (request.find(" /a ") != std::string::npos) return Reply{answer(301, {}, "Location: /b\r\n")};
                      if (request.find(" /p307 ") != std::string::npos) return Reply{answer(307, {}, "Location: q\r\n")};
                      if (request.find(" /p303 ") != std::string::npos) return Reply{answer(303, {}, "Location: /q\r\n")};
                      if (request.find(" /loop ") != std::string::npos) return Reply{answer(302, {}, "Location: /loop\r\n")};

                      return Reply{answer(200, method + " " + request.substr(request.find("\r\n\r\n") + 4))};
                  }};
    Request posted = asking(server.url("/p307"), "POST");

    posted.body = tal::bytes("x");

    const Response moved = tal::wait(loop, plain().fetch(asking(server.url("/a"))));

    SAME(body(moved), std::string{"GET "});
    SAME(moved.url.text(), server.url("/b").text());
    SAME(body(tal::wait(loop, plain().fetch(posted))), std::string{"POST x"});

    posted.url = server.url("/p303");

    SAME(body(tal::wait(loop, plain().fetch(posted))), std::string{"GET "});

    Limits two;

    two.redirects = 2;

    const std::size_t before = server.heard.size();

    SAME(kind_of(loop, plain(two).fetch(asking(server.url("/loop")))), named(tal::Kind::full));
    SAME(server.heard.size() - before, std::size_t{3});
}

TRIAL("HTTP: a login follows a redirect only within its scheme, host and port; HTTPS never goes down") {
    tal::Loop loop;
    Server other{loop, [](const std::string&, std::size_t) { return Reply{answer(200, "there")}; }};
    const std::string away = "Location: " + other.url("/y").text() + "\r\n";
    Server server{loop, [away](const std::string& request, std::size_t) {
                      if (request.find(" /away ") != std::string::npos) return Reply{answer(302, {}, away)};
                      if (request.find(" /home ") != std::string::npos) return Reply{answer(302, {}, "Location: /here\r\n")};

                      return Reply{answer(200, "here")};
                  }};
    Request request = asking(server.url("/away"));

    request.headers = {Header{"Authorization", "Bearer s"}, Header{"Cookie", "c=1"}, Header{"X-Keep", "k"}};

    SAME(body(tal::wait(loop, plain().fetch(request))), std::string{"there"});
    OUGHT(!told(other.heard.at(0), "Authorization").has_value());
    OUGHT(!told(other.heard.at(0), "Cookie").has_value());
    SAME(said(other.heard.at(0), "X-Keep"), std::string{"k"});

    request.url = server.url("/home");

    SAME(body(tal::wait(loop, plain().fetch(request))), std::string{"here"});
    SAME(said(server.heard.back(), "Authorization"), std::string{"Bearer s"});
    SAME(said(server.heard.back(), "Cookie"), std::string{"c=1"});

    Server secure{loop, [away](const std::string&, std::size_t) { return Reply{answer(302, {}, away)}; }, true};

    SAME(kind_of(loop, turning().fetch(asking(secure.url()))), named(tal::Kind::denied));
    SAME(other.heard.size(), std::size_t{1});
}

TRIAL("HTTP: limits of the head, the body and the trailer are full; silence is overdue") {
    tal::Loop loop;
    Server server{loop, [](const std::string& request, std::size_t) {
                      if (request.find(" /head ") != std::string::npos) return Reply{answer(200, {}, "X-Long: " + std::string(200, 'h') + "\r\n"), true};
                      if (request.find(" /sized ") != std::string::npos) return Reply{answer(200, std::string(101, 's')), true};
                      if (request.find(" /chunks ") != std::string::npos)
                          return Reply{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n40\r\n" + std::string(64, 'c') + "\r\n40\r\n" + std::string(64, 'c') + "\r\n0\r\n\r\n", true};
                      if (request.find(" /trailer ") != std::string::npos)
                          return Reply{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nA: " + std::string(90, 'a') + "\r\nB: " + std::string(90, 'b') + "\r\n\r\n", true};
                      if (request.find(" /end ") != std::string::npos) return Reply{"HTTP/1.1 200 OK\r\n\r\n" + std::string(101, 'e'), true};
                      if (request.find(" /fits ") != std::string::npos) return Reply{answer(200, std::string(100, 'f'))};
                      if (request.find(" /endless ") != std::string::npos) return Reply{"HTTP/1.1 200 OK\r\nX-Long: " + std::string(300, 'h'), false, false, false, false, true};

                      return Reply{{}, false, false, false, false, true};
                  }};
    Limits most;

    most.head = 150;
    most.body = 100;
    most.quiet = 200ms;

    const Http http = plain(most);

    SAME(kind_of(loop, http.fetch(asking(server.url("/head")))), named(tal::Kind::full));
    SAME(kind_of(loop, http.fetch(asking(server.url("/endless")))), named(tal::Kind::full));
    SAME(kind_of(loop, http.fetch(asking(server.url("/sized")))), named(tal::Kind::full));
    SAME(kind_of(loop, http.fetch(asking(server.url("/chunks")))), named(tal::Kind::full));
    SAME(kind_of(loop, http.fetch(asking(server.url("/trailer")))), named(tal::Kind::full));
    SAME(kind_of(loop, http.fetch(asking(server.url("/end")))), named(tal::Kind::full));
    SAME(tal::wait(loop, http.fetch(asking(server.url("/fits")))).body.size(), std::size_t{100});
    SAME(kind_of(loop, http.fetch(asking(server.url("/silent")))), named(tal::Kind::overdue));
}

TRIAL("HTTP: what is not HTTP/1.x is malformed; a coding other than chunked is unsupported") {
    tal::Loop loop;
    const std::vector<std::pair<std::string, tal::Kind>> cases = {
        {"HTTP/2 200 OK\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 2x0 OK\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 099 OK\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200OK\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1_200 OK\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nNoColon\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\n: empty\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nX: a\r\n folded\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nX: a\nb\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n", tal::Kind::unsupported},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", tal::Kind::unsupported},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloX\r\n0\r\n\r\n", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", tal::Kind::malformed},
        {"HTTP/1.1 200 OK\r\nContent-Le", tal::Kind::malformed},
    };
    std::size_t at = 0;
    Server server{loop, [&cases, &at](const std::string&, std::size_t) { return Reply{cases[at].first, true}; }};

    for (; at < cases.size(); ++at) SAME(kind_of(loop, plain().fetch(asking(server.url()))) + " " + std::to_string(at), named(cases[at].second) + " " + std::to_string(at));
}

TRIAL("HTTP: one interim answer is passed over; a second is malformed") {
    tal::Loop loop;
    Server server{loop, [](const std::string& request, std::size_t) {
                      if (request.find(" /two ") != std::string::npos) return Reply{"HTTP/1.1 100 Go\r\n\r\nHTTP/1.1 102 Still\r\n\r\n" + answer(200, "x"), true};

                      return Reply{"HTTP/1.1 100 Continue\r\n\r\n" + answer(200, "after")};
                  }};

    SAME(body(tal::wait(loop, plain().fetch(asking(server.url())))), std::string{"after"});
    SAME(kind_of(loop, plain().fetch(asking(server.url("/two")))), named(tal::Kind::malformed));
}

TRIAL("HTTP: a request that cannot be sent as given is the caller's, and nothing goes") {
    tal::Loop loop;
    Server server{loop, [](const std::string&, std::size_t) { return Reply{answer(200)}; }};
    const Http http = plain();
    Request broken = asking(server.url());

    broken.headers = {Header{"X-Bad", "a\r\nInjected: yes"}};

    SAME(kind_of(loop, http.fetch(broken)), named(tal::Kind::other));

    broken.headers = {Header{"X Bad", "a"}};

    SAME(kind_of(loop, http.fetch(broken)), named(tal::Kind::other));

    broken.headers = {Header{"X-Nul", std::string{"a\0b", 3}}};

    SAME(kind_of(loop, http.fetch(broken)), named(tal::Kind::other));
    SAME(kind_of(loop, http.fetch(asking(server.url(), ""))), named(tal::Kind::other));
    SAME(kind_of(loop, http.fetch(asking(server.url(), "GE T"))), named(tal::Kind::other));
    SAME(kind_of(loop, http.fetch(asking(Url{}))), named(tal::Kind::other));

    Url nameless = server.url();

    nameless.host.clear();

    SAME(kind_of(loop, http.fetch(asking(nameless))), named(tal::Kind::other));
    SAME(server.connections, std::size_t{0});
}

TRIAL("HTTP: through a proxy, plain HTTP names the whole address and the proxy's login") {
    tal::Loop loop;
    Server proxy{loop, [](const std::string&, std::size_t) { return Reply{answer(200, "via")}; }};
    const Http http{tal::Optional<tal::net::Tls>{}, Limits{}, Proxy::parsed("http://user:pass@127.0.0.1:" + std::to_string(proxy.port()), "", "")};

    SAME(body(tal::wait(loop, http.fetch(asking(*Url::parsed("http://example.test:8080/p?q"))))), std::string{"via"});
    OUGHT(begins(proxy.heard.at(0), "GET http://example.test:8080/p?q HTTP/1.1\r\nHost: example.test:8080\r\n"));
    SAME(said(proxy.heard.at(0), "Proxy-Authorization"), std::string{"Basic dXNlcjpwYXNz"});
}

TRIAL("HTTP: HTTPS through the proxy's tunnel; a refused tunnel is refused, bytes before the server malformed") {
    tal::Loop loop;
    Server proxy{loop, [](const std::string& request, std::size_t) {
                     if (begins(request, "CONNECT refused.test")) return Reply{"HTTP/1.1 407 Who\r\nContent-Length: 0\r\n\r\n", true};
                     if (begins(request, "CONNECT eager.test")) return Reply{"HTTP/1.1 200 OK\r\n\r\nX", true};
                     if (begins(request, "CONNECT")) return Reply{"HTTP/1.1 200 Connection established\r\n\r\n", false, false, true};

                     return Reply{answer(200, "secret")};
                 }};
    const std::string at = "127.0.0.1:" + std::to_string(proxy.port());
    const Http http{tal::Optional<tal::net::Tls>{turned().shared()}, Limits{}, Proxy::parsed("", "user:pass@" + at, "")};

    SAME(body(tal::wait(loop, http.fetch(asking(*Url::parsed("https://example.test/s"))))), std::string{"secret"});
    OUGHT(begins(proxy.heard.at(0), "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test:443\r\n"));
    SAME(said(proxy.heard.at(0), "Proxy-Authorization"), std::string{"Basic dXNlcjpwYXNz"});
    OUGHT(begins(proxy.heard.at(1), "GET /s HTTP/1.1\r\nHost: example.test\r\n"));
    OUGHT(!told(proxy.heard.at(1), "Proxy-Authorization").has_value());

    SAME(kind_of(loop, http.fetch(asking(*Url::parsed("https://refused.test/")))), named(tal::Kind::refused));
    SAME(kind_of(loop, http.fetch(asking(*Url::parsed("https://eager.test/")))), named(tal::Kind::malformed));

    const Http secured{tal::Optional<tal::net::Tls>{turned().shared()}, Limits{}, Proxy::parsed("", "https://" + at, "")};

    SAME(kind_of(loop, secured.fetch(asking(*Url::parsed("https://example.test/")))), named(tal::Kind::unsupported));
}

TRIAL("HTTPS: its connection is kept; an end without the goodbye is broken; without TLS it is unsupported") {
    tal::Loop loop;
    Server server{loop, [](const std::string& request, std::size_t) {
                      if (request.find(" /cut ") != std::string::npos) return Reply{"HTTP/1.1 200 OK\r\n\r\npart", false, true};
                      if (request.find(" /end ") != std::string::npos) return Reply{"HTTP/1.1 200 OK\r\n\r\nall", true};

                      return Reply{answer(200, "s")};
                  },
                  true};
    const Http http = turning();

    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"s"});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url())))), std::string{"s"});
    SAME(server.connections, std::size_t{1});
    SAME(body(tal::wait(loop, http.fetch(asking(server.url("/end"))))), std::string{"all"});
    SAME(kind_of(loop, http.fetch(asking(server.url("/cut")))), named(tal::Kind::broken));

    if (tal::net::Tls::builtin() == nullptr) {
        const std::size_t before = server.connections;

        SAME(kind_of(loop, plain().fetch(asking(server.url()))), named(tal::Kind::unsupported));
        SAME(server.connections, before);
    }
}

TRIAL("Download: a 2xx body goes to its file through the file beside it, with its progress; others leave it") {
    tal::Loop loop;
    const Remembering memory;
    Server server{loop, [](const std::string& request, std::size_t) {
                      if (request.find(" /missing ") != std::string::npos) return Reply{answer(404, "nope")};
                      if (request.find(" /cut ") != std::string::npos) return Reply{"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", true};
                      if (request.find(" /big ") != std::string::npos) return Reply{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n8\r\nbigger!!\r\n0\r\n\r\n"};
                      if (request.find(" /pieces ") != std::string::npos) return Reply{answer(200, "in pieces"), false, false, false, true};
                      if (request.find(" /large ") != std::string::npos) return Reply{answer(200, large())};

                      return Reply{answer(200, "fresh!")};
                  }};
    Limits small;

    small.body = 4;

    const Http http = plain(small);
    const Path file{"/d/f"};
    const Path part{"/d/f.part"};

    tal::wait(loop, tal::fs::make(Path{"/d"}));
    tal::wait(loop, tal::fs::write(file, tal::bytes("old")));

    tal::Progress progress;
    const Response got = tal::wait(loop, http.fetch(asking(server.url()), file, &progress));

    SAME(got.status, 200);
    OUGHT(got.body.empty());
    SAME(tal::text::of(tal::wait(loop, tal::fs::read(file))), std::string{"fresh!"});
    SAME(progress.done(), std::uint64_t{6});
    SAME(progress.total(), std::uint64_t{6});
    OUGHT(!there(loop, part));

    const Response missing = tal::wait(loop, http.fetch(asking(server.url("/missing")), file));

    SAME(missing.status, 404);
    SAME(body(missing), std::string{"nope"});
    SAME(tal::text::of(tal::wait(loop, tal::fs::read(file))), std::string{"fresh!"});
    OUGHT(!there(loop, part));

    SAME(kind_of(loop, http.fetch(asking(server.url("/cut")), file)), named(tal::Kind::malformed));
    UNTIL(loop, !there(loop, part));
    SAME(tal::text::of(tal::wait(loop, tal::fs::read(file))), std::string{"fresh!"});

    (void)tal::wait(loop, http.fetch(asking(server.url("/big")), file));

    SAME(tal::text::of(tal::wait(loop, tal::fs::read(file))), std::string{"bigger!!"});

    (void)tal::wait(loop, http.fetch(asking(server.url("/pieces")), file));

    SAME(tal::text::of(tal::wait(loop, tal::fs::read(file))), std::string{"in pieces"});

    tal::Progress counted;

    (void)tal::wait(loop, http.fetch(asking(server.url("/large")), file, &counted));

    OUGHT(tal::text::of(tal::wait(loop, tal::fs::read(file))) == large());
    SAME(counted.done(), std::uint64_t{large().size()});
}

TRIAL("Download: one cancelled halfway takes its file beside away and leaves the file as it was") {
    tal::Loop loop;
    const Remembering memory;
    Server server{loop, [](const std::string&, std::size_t) { return Reply{"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789", false, false, false, false, false}; }};
    const Path file{"/f"};
    const Path part{"/f.part"};

    tal::wait(loop, tal::fs::write(file, tal::bytes("old")));

    {
        tal::Scope scope{loop};
        tal::Progress progress;

        scope.run([](Http http, Request request, Path into, tal::Progress* counted) -> tal::Task<void> {
            (void)co_await http.fetch(std::move(request), std::move(into), counted);
        }(plain(), asking(server.url()), file, &progress));

        UNTIL(loop, progress.done() == 10);
        OUGHT(there(loop, part));
    }

    UNTIL(loop, !there(loop, part));
    SAME(tal::text::of(tal::wait(loop, tal::fs::read(file))), std::string{"old"});
}

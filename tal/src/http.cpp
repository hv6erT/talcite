#include "tal/http.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "tal/crypto.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/net.hpp"
#include "tal/stream.hpp"
#include "tal/trouble.hpp"

#include "httping.hpp"

// An HTTP/1.1 client, ported from talcite's http.cpp to tasks: as much as services such as TMDB and
// downloads of images need. A body framed by Content-Length, in chunks or until the connection
// closes (over TLS only with the server's goodbye), redirects, and one idle connection per server.
// Only GET and HEAD take an idle connection: any other request goes on a new one, since a request
// that changes something must not be repeated silently on a connection that failed halfway.
//
// A proxy: plain HTTP goes to it with the whole address in the request line, HTTPS through a
// CONNECT tunnel, in which TLS talks to the server itself, so the proxy sees the name and the port
// alone. The variables of the environment come first, as curl reads them; without them, the
// system's own setting (`httping::systemic`). A proxy behind TLS and SOCKS are not spoken.
//
// A status is not an error: 404 and 429 come back as a `Response`, since the caller knows what to
// do with them. `Trouble` means a broken protocol (malformed), what is not spoken (unsupported), a
// way down from HTTPS (denied), a limit of `Limits` passed (full) or a silence longer than allowed
// (overdue); a request that cannot be sent as given is the caller's (other).
//
// A download takes the body of a 2xx answer piece by piece into a file beside the target, and only
// a whole body takes the target's place, in one move; the file beside goes in every other end.
namespace tal::net {

namespace {

// How many bytes one read asks for.
constexpr std::size_t piece = 64 * 1024;

// The longest line of a chunk's size or of a trailer.
constexpr std::size_t line_most = 4096;

// The letter `one` in lowercase, ASCII only.
char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

// `text` in lowercase, ASCII only.
std::string lowered(std::string_view text) {
    std::string made{text};

    std::transform(made.begin(), made.end(), made.begin(), [](char one) { return lowered(one); });

    return made;
}

// Whether `one` and `two` are equal ignoring ASCII case, as header names are compared.
bool alike(std::string_view one, std::string_view two) {
    return one.size() == two.size() && std::equal(one.begin(), one.end(), two.begin(), [](char a, char b) { return lowered(a) == lowered(b); });
}

// `text` without the spaces and tabs around it.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);

    return text;
}

// Whether `one` may stand in a header's name or a method (a token of RFC 9110).
bool tokenish(char one) {
    if ((one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9')) return true;

    return std::string_view{"!#$%&'*+-.^_`|~"}.find(one) != std::string_view::npos;
}

// Whether `one` may stand in a request's target: visible ASCII; the caller encodes the rest (%20).
bool visible(char one) { return one > ' ' && one < 0x7F; }

// The port of the scheme: 443 for HTTPS, 80 for HTTP.
std::uint16_t fallback(bool secure) { return secure ? 443 : 80; }

// A port of the decimal `digits`, 1 to 65535; nothing for anything else.
std::optional<std::uint16_t> ported(std::string_view digits) {
    unsigned made = 0;
    const auto [end, why] = std::from_chars(digits.data(), digits.data() + digits.size(), made);

    if (digits.empty() || why != std::errc{} || end != digits.data() + digits.size() || made == 0 || made > 65535) return std::nullopt;

    return static_cast<std::uint16_t>(made);
}

// Whether `host` may be the host of an address: a name or IPv4 of lowercase letters, digits, dots,
// hyphens and underscores; IPv6 (`bracketed`) of hexadecimal digits, colons and dots.
bool hosted(std::string_view host, bool bracketed) {
    if (host.empty()) return false;

    return std::all_of(host.begin(), host.end(), [bracketed](char one) {
        const bool plain = (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '.' || one == '-';

        return bracketed ? plain || one == ':' : plain || one == '_';
    });
}

// The host of `one` as the Host header and the text write it: IPv6 in brackets, and the port only
// when it is not the scheme's.
std::string authority(const Url& one) {
    std::string made = one.host.find(':') != std::string::npos ? "[" + one.host + "]" : one.host;

    if (one.port != fallback(one.secure)) made += ":" + std::to_string(one.port);

    return made;
}

// One connection: its stream and the bytes read from it but not used yet. `heard` says whether any
// of the answer came, since an idle connection that failed before that was only closed by the
// server meanwhile; `forwarded` marks plain HTTP through a proxy, with the proxy's `login`.
struct Line {
    Stream stream;
    Bytes kept;
    bool heard = false;
    bool forwarded = false;
    std::string login;
};

using Wire = std::shared_ptr<Line>;

using Place = std::tuple<bool, std::string, std::uint16_t>;

// The key of the idle connections for `one`: its scheme, host and port.
Place place_of(const Url& one) { return Place{one.secure, one.host, one.port}; }

// `work`, failing as overdue after `most` of silence.
template <typename T>
Task<T> silent(std::chrono::milliseconds most, Task<T> work) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await within(most, std::move(work));
        } else {
            co_return co_await within(most, std::move(work));
        }
    } catch (const Overdue&) {
        throw Trouble{Kind::overdue, "The other side was silent for too long."};
    }
}

// Reads more of the connection into what is kept; false when the other side ended. An empty buffer,
// as a body read through leaves it, takes what came whole instead of copying it.
Task<bool> more(Wire wire, std::chrono::milliseconds quiet) {
    Bytes got = co_await silent(quiet, wire->stream.read(piece));

    if (got.empty()) co_return false;

    wire->heard = true;

    if (wire->kept.empty()) wire->kept = std::move(got);
    else wire->kept.insert(wire->kept.end(), got.begin(), got.end());

    co_return true;
}

// Where `what` begins in `in`, or npos.
std::size_t found(const Bytes& in, std::string_view what) {
    const auto at = std::search(in.begin(), in.end(), what.begin(), what.end(), [](std::byte one, char two) { return one == static_cast<std::byte>(two); });

    return at == in.end() ? std::string::npos : static_cast<std::size_t>(at - in.begin());
}

// What is kept up to `mark`, without it, taken out with it. A mark further than `most` is a limit
// passed for the `head`, or a line too long, said before the rest comes.
Task<std::string> until(Wire wire, std::string_view mark, std::size_t most, std::chrono::milliseconds quiet, bool head) {
    for (;;) {
        if (const std::size_t at = found(wire->kept, mark); at != std::string::npos) {
            if (at > most) throw head ? Trouble{Kind::full, "The head of the answer is too large."} : Trouble{Kind::malformed, "A line of the answer is too long."};

            std::string made{reinterpret_cast<const char*>(wire->kept.data()), at};

            wire->kept.erase(wire->kept.begin(), wire->kept.begin() + static_cast<std::ptrdiff_t>(at + mark.size()));

            co_return made;
        }

        if (wire->kept.size() > most + mark.size()) throw head ? Trouble{Kind::full, "The head of the answer is too large."} : Trouble{Kind::malformed, "A line of the answer is too long."};

        if (!co_await more(wire, quiet)) throw Trouble{Kind::malformed, "The connection ended in the middle of the answer."};
    }
}

// Where the body of one answer goes: into memory up to `most`, or, for the 2xx answer of a download
// (`spilling`), into its file from the start, counted in its progress.
struct Sink {
    Bytes memory;
    std::size_t most = 0;
    bool spilling = false;
    fs::File file;
    std::uint64_t written = 0;
    Progress* progress = nullptr;
};

// Takes `part` of the body into `sink`.
Task<void> poured(Sink& sink, Bytes part) {
    if (part.empty()) co_return;

    if (sink.spilling) {
        const std::uint64_t at = sink.written;
        const std::size_t much = part.size();

        sink.written += much;

        co_await sink.file.write(at, std::move(part));

        if (sink.progress != nullptr) sink.progress->add(much);

        co_return;
    }

    if (part.size() > sink.most || sink.memory.size() > sink.most - part.size()) throw Trouble{Kind::full, "The body of the answer is too large."};

    if (sink.memory.empty()) sink.memory = std::move(part);
    else sink.memory.insert(sink.memory.end(), part.begin(), part.end());
}

// At most `much` of the kept bytes, taken out; all of them go whole, without a copy.
Bytes taken_out(Line& line, std::uint64_t much) {
    if (much >= line.kept.size()) return std::exchange(line.kept, Bytes{});

    const auto end = line.kept.begin() + static_cast<std::ptrdiff_t>(much);
    Bytes made(line.kept.begin(), end);

    line.kept.erase(line.kept.begin(), end);

    return made;
}

// Exactly `much` bytes of the body, into `sink` as they come.
Task<void> exact(Wire wire, std::uint64_t much, Sink& sink, std::chrono::milliseconds quiet) {
    while (much > 0) {
        if (wire->kept.empty() && !co_await more(wire, quiet)) throw Trouble{Kind::malformed, "The connection ended in the middle of the body."};

        Bytes part = taken_out(*wire, much);

        much -= part.size();

        co_await poured(sink, std::move(part));
    }
}

// The body until the connection ends: an answer without a length and without chunks. Over TLS an
// end without the server's goodbye fails in the stream as broken, so a part never passes for all.
Task<void> drained(Wire wire, Sink& sink, std::chrono::milliseconds quiet) {
    do {
        co_await poured(sink, std::exchange(wire->kept, Bytes{}));
    } while (co_await more(wire, quiet));
}

// The size of a chunk from the line `line`: hexadecimal digits, extensions after a semicolon
// skipped; anything else is malformed.
std::uint64_t chunk_size(const std::string& line) {
    const std::string_view digits = trimmed(std::string_view{line}.substr(0, line.find(';')));
    std::uint64_t made = 0;
    const auto [end, why] = std::from_chars(digits.data(), digits.data() + digits.size(), made, 16);

    if (digits.empty() || why != std::errc{} || end != digits.data() + digits.size()) throw Trouble{Kind::malformed, "The size of a chunk is not a number."};

    return made;
}

// A chunked body: a line with the size in hexadecimal, the chunk, CRLF; a chunk of zero ends it,
// and the trailer up to an empty line, at most `most` bytes, is read and dropped.
Task<void> chunked(Wire wire, Sink& sink, std::size_t most, std::chrono::milliseconds quiet) {
    for (;;) {
        const std::uint64_t much = chunk_size(co_await until(wire, "\r\n", line_most, quiet, false));

        if (much == 0) break;

        if (!sink.spilling && (much > sink.most || sink.memory.size() > sink.most - much)) throw Trouble{Kind::full, "The body of the answer is too large."};

        co_await exact(wire, much, sink, quiet);

        (void)co_await until(wire, "\r\n", 0, quiet, false);
    }

    for (std::size_t heard = 0;;) {
        const std::string line = co_await until(wire, "\r\n", line_most, quiet, false);

        if (line.empty()) co_return;

        heard += line.size() + 2;

        if (heard > most) throw Trouble{Kind::full, "The trailer of the answer is too large."};
    }
}

// The head of an answer taken apart.
struct Head {
    bool eleven = true;
    int status = 0;
    std::vector<Header> headers;
};

// The status line and the header lines of the head `head` (without the empty line that ends it);
// anything that is not HTTP/1.x, a folded header among them, is malformed.
Head parsed(const std::string& head) {
    Head made;
    std::size_t at = head.find("\r\n");
    const std::string_view first = std::string_view{head}.substr(0, at);

    if (first.size() < 12 || first.substr(0, 7) != "HTTP/1." || (first[7] != '0' && first[7] != '1') || first[8] != ' ' || (first.size() > 12 && first[12] != ' '))
        throw Trouble{Kind::malformed, "The status line is not HTTP/1.x."};

    made.eleven = first[7] == '1';

    const std::string_view code = first.substr(9, 3);

    if (!std::all_of(code.begin(), code.end(), [](char one) { return one >= '0' && one <= '9'; }) || code[0] == '0') throw Trouble{Kind::malformed, "The status is not a number."};

    made.status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');

    while (at != std::string::npos) {
        const std::size_t from = at + 2;

        at = head.find("\r\n", from);

        const std::string_view line = std::string_view{head}.substr(from, at == std::string::npos ? std::string::npos : at - from);
        const std::size_t colon = line.find(':');

        if (colon == std::string_view::npos || colon == 0 || !std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(colon), tokenish))
            throw Trouble{Kind::malformed, "A line of the head is not a header."};

        if (line.find_first_of(std::string_view{"\r\n\0", 3}) != std::string_view::npos) throw Trouble{Kind::malformed, "A header breaks its line."};

        made.headers.push_back(Header{std::string{line.substr(0, colon)}, std::string{trimmed(line.substr(colon + 1))}});
    }

    return made;
}

// The words of the list headers `name` (Connection, Transfer-Encoding) among `headers`, lowercase.
std::vector<std::string> worded(const std::vector<Header>& headers, std::string_view name) {
    std::vector<std::string> made;

    for (const Header& one : headers) {
        if (!alike(one.name, name)) continue;

        std::string_view rest = one.value;

        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            const std::string_view word = trimmed(rest.substr(0, comma));

            if (!word.empty()) made.push_back(lowered(word));

            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
    }

    return made;
}

// How a body is read: none, an exact length, in chunks, or until the end.
enum class Framing : std::uint8_t { none, sized, pieces, closing };

struct Frame {
    Framing how = Framing::closing;
    std::uint64_t size = 0;
};

// How the body of `head` comes, for the request `method`: none for HEAD, 204 and 304; chunked when
// Transfer-Encoding says so, and no other coding; otherwise Content-Length, one number within
// `most`; and until the end without it.
Frame framed(const Head& head, std::string_view method, std::uint64_t most) {
    if (method == "HEAD" || head.status == 204 || head.status == 304) return {Framing::none, 0};

    if (const std::vector<std::string> codings = worded(head.headers, "transfer-encoding"); !codings.empty()) {
        if (codings.size() > 1 || codings.back() != "chunked") throw Trouble{Kind::unsupported, "Only the chunked transfer coding is spoken."};

        return {Framing::pieces, 0};
    }

    std::optional<std::uint64_t> size;

    for (const Header& one : head.headers) {
        if (!alike(one.name, "content-length")) continue;

        std::uint64_t made = 0;
        const auto [end, why] = std::from_chars(one.value.data(), one.value.data() + one.value.size(), made);

        if (why != std::errc{} || end != one.value.data() + one.value.size() || (size && *size != made))
            throw Trouble{Kind::malformed, "The length of the content is not one number."};

        size = made;
    }

    if (!size) return {Framing::closing, 0};

    if (*size > most) throw Trouble{Kind::full, "The body of the answer is too large."};

    return {Framing::sized, *size};
}

// Whether the connection may carry the next request: HTTP/1.1, no "Connection: close", a body
// with an end of its own, and nothing left over.
bool reusable(const Head& head, Framing how, const Line& line) {
    if (!head.eleven || how == Framing::closing || !line.kept.empty()) return false;

    const std::vector<std::string> words = worded(head.headers, "connection");

    return std::find(words.begin(), words.end(), "close") == words.end();
}

// The Proxy-Authorization line of `login`, Basic (RFC 7617), or nothing without one.
std::string authorized(const std::string& login) {
    return login.empty() ? std::string{} : "Proxy-Authorization: Basic " + crypto::base64(std::as_bytes(std::span{login})) + "\r\n";
}

// The bytes of `request` on `line`: through a proxy over plain HTTP, the whole address in the
// request line (RFC 9112, 3.2.2) and the proxy's login. The headers tal writes itself are not
// taken from the caller.
Bytes requested(const Request& request, const Line& line) {
    std::string made = request.method + " " + (line.forwarded ? request.url.text() : request.url.target) + " HTTP/1.1\r\nHost: " + authority(request.url) + "\r\n";

    if (line.forwarded) made += authorized(line.login);

    for (const Header& one : request.headers) {
        if (alike(one.name, "host") || alike(one.name, "connection") || alike(one.name, "accept-encoding") || alike(one.name, "proxy-authorization") ||
            alike(one.name, "content-length") || alike(one.name, "transfer-encoding"))
            continue;

        made += one.name + ": " + one.value + "\r\n";
    }

    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" || request.method == "PATCH")
        made += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";

    made += "Accept-Encoding: identity\r\n\r\n";

    Bytes whole = bytes(made);

    whole.insert(whole.end(), request.body.begin(), request.body.end());

    return whole;
}

// Throws `Trouble` (other) for a request that cannot be sent as given: no address, a method or a
// header's name that is not a token, or a value that would break its line.
void checked(const Request& request) {
    if (request.url.host.empty() || request.url.port == 0 || request.url.target.empty() || request.url.target.front() != '/' ||
        !std::all_of(request.url.target.begin(), request.url.target.end(), visible))
        throw Trouble{Kind::other, "The request has no address."};

    if (request.method.empty() || !std::all_of(request.method.begin(), request.method.end(), tokenish)) throw Trouble{Kind::other, "The method is not a token."};

    for (const Header& one : request.headers) {
        if (one.name.empty() || !std::all_of(one.name.begin(), one.name.end(), tokenish)) throw Trouble{Kind::other, "The name of a header is not a token."};

        if (one.value.find_first_of(std::string_view{"\r\n\0", 3}) != std::string::npos) throw Trouble{Kind::other, "The value of a header breaks its line."};
    }
}

// The address `location` relative to `base`: whole, without a scheme (//host/...), from the root
// (/...) or relative, where it takes the place of the last segment of the path.
std::optional<Url> joined(const Url& base, std::string_view location) {
    if (location.find("://") != std::string_view::npos) return Url::parsed(location);

    if (location.substr(0, 2) == "//") return Url::parsed(std::string{base.secure ? "https:" : "http:"} + std::string{location});

    std::string target{location.substr(0, location.find('#'))};

    if (target.empty() || !std::all_of(target.begin(), target.end(), visible)) return std::nullopt;

    if (target.front() != '/') {
        const std::string path = base.target.substr(0, base.target.find('?'));

        target = path.substr(0, path.rfind('/') + 1) + target;
    }

    Url made = base;

    made.target = std::move(target);

    return made;
}

// The value of the hexadecimal digit `one`, or -1.
int nibble(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// `text` with its %XX escapes decoded; a % without two digits stays as it is.
std::string unescaped(std::string_view text) {
    std::string made;

    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '%' && at + 2 < text.size() && nibble(text[at + 1]) >= 0 && nibble(text[at + 2]) >= 0) {
            made += static_cast<char>(nibble(text[at + 1]) * 16 + nibble(text[at + 2]));
            at += 2;
        } else {
            made += text[at];
        }
    }

    return made;
}

// A proxy of the notation `text`, such as http://user:password@proxy:3128/; without a scheme it is
// HTTP, as in curl and wget. Another scheme (SOCKS) and a notation that is not one give nothing.
std::optional<Gateway> gated(std::string_view text) {
    text = trimmed(text);

    if (text.empty()) return std::nullopt;

    std::string whole = text.find("://") == std::string_view::npos ? "http://" + std::string{text} : std::string{text};
    const std::size_t from = whole.find("://") + 3;
    const std::size_t path = whole.find_first_of("/?#", from);
    const std::size_t at = whole.rfind('@', path == std::string::npos ? std::string::npos : path);
    Gateway made;

    if (at != std::string::npos && at >= from) {
        made.login = unescaped(std::string_view{whole}.substr(from, at - from));
        whole.erase(from, at + 1 - from);
    }

    const std::optional<Url> url = Url::parsed(whole);

    crypto::wiped(whole);

    if (!url) {
        crypto::wiped(made.login);

        return std::nullopt;
    }

    made.url = *url;
    made.url.target = "/";

    return made;
}

// The exceptions of no_proxy in `text`: names apart by commas, semicolons or spaces, lowercase,
// "*." shortened to "."; a port beside a name narrows nothing and goes, and IPv6 loses its
// brackets.
std::vector<std::string> bypassed(std::string_view text) {
    std::vector<std::string> made;

    while (!text.empty()) {
        const std::size_t cut = text.find_first_of(",; \t");
        std::string one = lowered(trimmed(text.substr(0, cut)));

        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 1);

        if (one.empty()) continue;

        if (one.rfind("*.", 0) == 0) one.erase(0, 1);

        if (one.front() == '[') {
            one = one.substr(1, one.find(']') - 1);
        } else if (std::count(one.begin(), one.end(), ':') == 1) {
            one.erase(one.find(':'));
        }

        if (!one.empty()) made.push_back(std::move(one));
    }

    return made;
}

// The first of the variables `one` and `two` that is set and not empty, or nothing.
std::string environment(const char* one, const char* two) {
    for (const char* name : {one, two}) {
        if (name == nullptr) continue;

        if (const char* told = std::getenv(name); told != nullptr && *told != '\0') return told;
    }

    return {};
}

// Whether `text` matches `pattern`, whose stars stand for any run of characters.
bool matched(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t after = 0;

    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            after = t;
        } else if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++after;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;

    return p == pattern.size();
}

}

// The address `location` relative to this one, as a redirect gives it.
std::optional<Url> Url::resolved(std::string_view location) const { return joined(*this, location); }

// The http or https address `text`; nothing for another scheme or a host, port or target that is
// not one. The fragment goes, and an empty target is "/".
std::optional<Url> Url::parsed(std::string_view text) {
    const std::size_t scheme_end = text.find("://");

    if (scheme_end == std::string_view::npos) return std::nullopt;

    const std::string scheme = lowered(text.substr(0, scheme_end));

    if (scheme != "http" && scheme != "https") return std::nullopt;

    Url made;

    made.secure = scheme == "https";

    std::string_view rest = text.substr(scheme_end + 3);

    rest = rest.substr(0, rest.find('#'));

    const std::size_t path_at = rest.find_first_of("/?");
    const std::string_view place = rest.substr(0, path_at);
    std::string target{path_at == std::string_view::npos ? std::string_view{} : rest.substr(path_at)};

    if (target.empty() || target.front() == '?') target.insert(0, "/");

    if (!std::all_of(target.begin(), target.end(), visible)) return std::nullopt;

    std::string_view host;
    std::string_view port;
    bool bracketed = false;

    if (!place.empty() && place.front() == '[') {
        const std::size_t close = place.find(']');

        if (close == std::string_view::npos) return std::nullopt;

        host = place.substr(1, close - 1);
        bracketed = true;

        const std::string_view after = place.substr(close + 1);

        if (!after.empty()) {
            if (after.front() != ':' || after.size() == 1) return std::nullopt;

            port = after.substr(1);
        }
    } else {
        const std::size_t colon = place.find(':');

        host = place.substr(0, colon);

        if (colon != std::string_view::npos) {
            port = place.substr(colon + 1);

            if (port.empty()) return std::nullopt;
        }
    }

    made.host = lowered(host);

    if (!hosted(made.host, bracketed)) return std::nullopt;

    if (port.empty()) {
        made.port = fallback(made.secure);
    } else {
        const std::optional<std::uint16_t> number = ported(port);

        if (!number) return std::nullopt;

        made.port = *number;
    }

    made.target = std::move(target);

    return made;
}

// The address written out, the port only when it is not the scheme's.
std::string Url::text() const { return std::string{secure ? "https://" : "http://"} + authority(*this) + target; }

// The value of the first header `name`, ignoring case, or null.
const std::string* Response::header(std::string_view name) const {
    for (const Header& one : headers)
        if (alike(one.name, name)) return &one.value;

    return nullptr;
}

// A proxy of the three texts: for plain HTTP, for HTTPS, and the exceptions.
Proxy Proxy::parsed(std::string_view plain, std::string_view secure, std::string_view bypass) {
    Proxy made;

    made.plain = gated(plain);
    made.secure = gated(secure);
    made.bypass = bypassed(bypass);

    return made;
}

// The variables as curl reads them, lowercase before uppercase, ALL_PROXY for a scheme without its
// own; uppercase HTTP_PROXY never, since under CGI the Proxy header of someone else's request sets
// it (httpoxy), though on Windows, whose names have no case, it is http_proxy itself. Without any
// of them, the system's setting, or no proxy.
Proxy Proxy::from_environment() {
    std::string plain = environment("http_proxy", nullptr);
    std::string secure = environment("https_proxy", "HTTPS_PROXY");
    const std::string all = environment("all_proxy", "ALL_PROXY");

    if (plain.empty()) plain = all;
    if (secure.empty()) secure = all;

    if (plain.empty() && secure.empty()) return httping::systemic().value_or(Proxy{});

    return parsed(plain, secure, environment("no_proxy", "NO_PROXY"));
}

// The proxy for `target`, or none when an exception takes it: "*" takes all, "<local>" the names
// without a dot, a name itself and its subdomains (".name" the same), and a pattern with stars
// what it matches.
const Gateway* Proxy::chosen(const Url& target) const {
    for (const std::string& one : bypass) {
        if (one == "*") return nullptr;

        if (one == "<local>") {
            if (target.host.find_first_of(".:") == std::string::npos) return nullptr;

            continue;
        }

        if (one.find('*') != std::string::npos) {
            if (matched(one, target.host)) return nullptr;

            continue;
        }

        const std::string_view name = one.front() == '.' ? std::string_view{one}.substr(1) : std::string_view{one};

        if (name.empty()) continue;

        if (target.host == name) return nullptr;

        if (target.host.size() > name.size() && target.host.ends_with(name) && target.host[target.host.size() - name.size() - 1] == '.') return nullptr;
    }

    const std::optional<Gateway>& made = target.secure ? secure : plain;

    return made ? &*made : nullptr;
}

// What a client keeps: its TLS, limits and proxy, and one idle connection per server. A second
// connection given back for the same server closes the first: requests to a service go one after
// another, and an idle connection for every one of them would hold sockets nobody uses.
struct Http::Held {
    Optional<Tls> tls;
    Limits limits;
    Proxy proxy;
    std::mutex guard;
    std::map<Place, Wire> idle;

    // Wipes the logins of the proxy.
    ~Held() {
        if (proxy.plain) crypto::wiped(proxy.plain->login);
        if (proxy.secure) crypto::wiped(proxy.secure->login);
    }

    // The idle connection to `where`, taken, or none.
    [[nodiscard]] Wire taken(const Place& where) {
        const std::lock_guard<std::mutex> kept{guard};
        const auto one = idle.find(where);

        return one == idle.end() ? nullptr : std::exchange(one->second, nullptr);
    }

    // Keeps `wire` as the idle connection to `where`, closing the one it replaces.
    void returned(const Place& where, Wire wire) {
        Wire old;

        {
            const std::lock_guard<std::mutex> kept{guard};
            Wire& slot = idle[where];

            old = std::exchange(slot, std::move(wire));
        }

        if (old) old->stream.close();
    }
};

namespace {

using Kept = std::shared_ptr<Http::Held>;

// The TLS of `held`: its own, or the program's; without either, HTTPS is unsupported.
Port<Tls> securing(const Http::Held& held) {
    if (held.tls) return Port<Tls>{held.tls.shared()};

    std::shared_ptr<Tls> found = defaults().get<Tls>();

    if (found == nullptr) throw Trouble{Kind::unsupported, "HTTPS needs a TLS adapter."};

    return Port<Tls>{std::move(found)};
}

// The tunnel to `url` through the proxy `gate` on `wire`: CONNECT, a 2xx answer, and from then on
// the bytes go to the server. A refusal (407 without a login, 403 for a port) is the proxy's, not
// an answer of the server; bytes before the server speaks are malformed.
Task<void> tunneled(Wire wire, Gateway gate, Url url, std::size_t head, std::chrono::milliseconds quiet) {
    const std::string place = (url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host) + ":" + std::to_string(url.port);
    std::string asked = "CONNECT " + place + " HTTP/1.1\r\nHost: " + place + "\r\n" + authorized(gate.login) + "\r\n";
    Bytes sent = bytes(asked);

    crypto::wiped(asked);
    crypto::wiped(gate.login);

    co_await silent(quiet, wire->stream.write(std::move(sent)));

    const Head told = parsed(co_await until(wire, "\r\n\r\n", head, quiet, true));

    if (told.status < 200 || told.status > 299) {
        wire->stream.close();

        throw Trouble{Kind::refused, "The proxy refused the tunnel with status " + std::to_string(told.status) + "."};
    }

    if (!wire->kept.empty()) {
        wire->stream.close();

        throw Trouble{Kind::malformed, "The proxy sent bytes before the server spoke."};
    }
}

// A new connection for `url`: straight, through a proxy with the whole address for HTTP, or
// through its tunnel for HTTPS. Connecting, the tunnel and the handshake each have the limit of
// silence.
Task<Wire> dialed(Kept held, Url url) {
    const std::chrono::milliseconds quiet = held->limits.quiet;
    const Gateway* via = held->proxy.chosen(url);
    auto wire = std::make_shared<Line>();

    if (via != nullptr && via->url.secure) throw Trouble{Kind::unsupported, "A proxy reached over HTTPS is not spoken."};

    if (via == nullptr) {
        wire->stream = (co_await silent(quiet, connect(url.host, url.port))).stream;
    } else {
        const Gateway gate = *via;

        wire->stream = (co_await silent(quiet, connect(gate.url.host, gate.url.port))).stream;

        if (!url.secure) {
            wire->forwarded = true;
            wire->login = gate.login;

            co_return wire;
        }

        co_await tunneled(wire, gate, url, held->limits.head, quiet);
    }

    if (url.secure) wire->stream = co_await silent(quiet, secured(wire->stream, url.host, securing(*held)));

    co_return wire;
}

// Where a download puts the body of its 2xx answer, and its progress.
struct Spill {
    fs::File file;
    Progress* progress = nullptr;
};

// One exchange of `request` on `wire`: the request, the head (after one interim answer at most),
// the body. The connection is kept for the next request when it can be, closed otherwise. With
// `spill`, the body of a 2xx answer goes to its file; any other comes into memory.
Task<Response> exchanged(Kept held, Request request, Wire wire, Spill* spill) {
    const std::chrono::milliseconds quiet = held->limits.quiet;

    co_await silent(quiet, wire->stream.write(requested(request, *wire)));

    Head head = parsed(co_await until(wire, "\r\n\r\n", held->limits.head, quiet, true));

    if (head.status >= 100 && head.status < 200) {
        head = parsed(co_await until(wire, "\r\n\r\n", held->limits.head, quiet, true));

        if (head.status >= 100 && head.status < 200) throw Trouble{Kind::malformed, "Too many interim answers."};
    }

    Sink sink;

    sink.most = held->limits.body;
    sink.spilling = spill != nullptr && head.status >= 200 && head.status < 300;

    const Frame frame = framed(head, request.method, sink.spilling ? std::numeric_limits<std::uint64_t>::max() : held->limits.body);

    if (sink.spilling) {
        sink.file = spill->file;
        sink.progress = spill->progress;

        if (sink.progress != nullptr && frame.how == Framing::sized) sink.progress->expect(frame.size);
    } else if (frame.how == Framing::sized) {
        sink.memory.reserve(static_cast<std::size_t>(frame.size));
    }

    switch (frame.how) {
        case Framing::sized: co_await exact(wire, frame.size, sink, quiet); break;
        case Framing::pieces: co_await chunked(wire, sink, held->limits.head, quiet); break;
        case Framing::closing: co_await drained(wire, sink, quiet); break;
        case Framing::none: break;
    }

    if (reusable(head, frame.how, *wire)) {
        wire->heard = false;
        held->returned(place_of(request.url), wire);
    } else {
        wire->stream.close();
    }

    co_return Response{head.status, std::move(head.headers), std::move(sink.memory), request.url};
}

// An exchange on the idle connection to the server, or on a new one. An idle connection that
// failed before any of the answer came was only closed by the server meanwhile, and the request
// goes again on a new one.
Task<Response> attempted(Kept held, Request request, Spill* spill) {
    if (request.method == "GET" || request.method == "HEAD") {
        if (Wire idle = held->taken(place_of(request.url))) {
            try {
                co_return co_await exchanged(held, request, idle, spill);
            } catch (const Trouble&) {
                idle->stream.close();

                if (idle->heard) throw;
            }
        }
    }

    Wire fresh = co_await dialed(held, request.url);

    co_return co_await exchanged(held, std::move(request), std::move(fresh), spill);
}

// Whether `status` is a redirect that is followed: 301, 302, 303, 307 or 308.
bool redirecting(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// `request` sent, with its redirects followed up to the limit; none goes from HTTPS to plain HTTP.
// 307 and 308 repeat the method with its body, HEAD stays HEAD, and the rest become GET. A login
// (Authorization, Cookie) follows only within the same scheme, host and port.
Task<Response> fetched(Kept held, Request request, Spill* spill) {
    for (int hops = 0;; ++hops) {
        checked(request);

        if (request.url.secure) (void)securing(*held);

        Response got = co_await attempted(held, request, spill);
        const std::string* location = got.header("location");

        if (!redirecting(got.status) || location == nullptr) co_return got;

        if (hops >= held->limits.redirects) throw Trouble{Kind::full, "Too many redirects."};

        const std::optional<Url> next = joined(request.url, *location);

        if (!next) throw Trouble{Kind::malformed, "The place of a redirect is not an address."};

        if (request.url.secure && !next->secure) throw Trouble{Kind::denied, "A redirect leaves HTTPS for plain HTTP."};

        const bool same = next->secure == request.url.secure && next->host == request.url.host && next->port == request.url.port;
        const bool kept = got.status == 307 || got.status == 308;
        Request again{kept || request.method == "HEAD" ? request.method : std::string{"GET"}, *next, {}, kept ? std::move(request.body) : Bytes{}};

        for (Header& one : request.headers)
            if (same || (!alike(one.name, "authorization") && !alike(one.name, "cookie"))) again.headers.push_back(std::move(one));

        request = std::move(again);
    }
}

// The tasks that take away the files beside downloads cut short, on the io pool.
Scope& clearing() {
    static Scope one{io()};

    return one;
}

// The file beside a download's target while the body comes; unless `done`, it is closed and taken
// away when the download ends any other way, cancelled too.
struct Aside {
    Path where;
    fs::File file;
    bool done = false;

    // Takes the file away, unless the download put it in its place or took it away itself.
    ~Aside() {
        if (done) return;

        file.close();

        clearing().run([](Path gone) -> Task<void> {
            try {
                co_await fs::remove(gone);
            } catch (...) {
            }
        }(where));
    }
};

// `request` fetched with the body of its 2xx answer into `into`, through the file beside it; any
// other answer leaves `into` as it was.
Task<Response> downloading(Kept held, Request request, Path into, Progress* progress) {
    Aside aside;

    aside.where = into.parent() / (std::string{into.name()} + ".part");
    Spill spill;

    spill.progress = progress;
    spill.file = co_await fs::open(aside.where, fs::Mode::truncate);
    aside.file = spill.file;

    Response got = co_await fetched(held, std::move(request), &spill);

    if (got.status < 200 || got.status > 299) {
        spill.file.close();
        aside.file.close();
        aside.done = true;

        co_await fs::remove(aside.where);

        co_return got;
    }

    co_await spill.file.flush();

    spill.file.close();
    aside.file.close();

    co_await fs::move(aside.where, into, fs::Conflict::replace);

    aside.done = true;

    co_return got;
}

}

// A client with `tls`, `most` and `proxy`.
Http::Http(Optional<Tls> tls, Limits most, Proxy proxy) : held_(std::make_shared<Held>()) {
    held_->tls = std::move(tls);
    held_->limits = most;
    held_->proxy = std::move(proxy);
}

// The answer to `one`, its body in memory.
Task<Response> Http::fetch(Request one) const { return fetched(held_, std::move(one), nullptr); }

// The answer to `one`, the body of a 2xx answer in the file `into`, counted in `progress`.
Task<Response> Http::fetch(Request one, Path into, Progress* progress) const { return downloading(held_, std::move(one), std::move(into), progress); }

namespace httping {

// The answer to `request` on the plain stream `line`, as the client reads it, alone: no idle
// connections and no redirects; for the trials and the fuzzer.
Task<Response> answered(Stream line, Request request, Limits most) {
    auto held = std::make_shared<Http::Held>();
    auto wire = std::make_shared<Line>();

    held->limits = most;
    wire->stream = std::move(line);

    checked(request);

    co_return co_await exchanged(held, std::move(request), wire, nullptr);
}

// A proxy of the setting of Windows: `proxy` is "host:port" for every scheme, or entries such as
// "http=host:port;https=host:port" apart by semicolons or spaces, the entry of a scheme before the
// common one; `bypass` lists the exceptions apart by semicolons, "<local>" among them.
Proxy windowsy(std::string_view proxy, std::string_view bypass) {
    std::string common;
    std::string plain;
    std::string secure;

    while (!proxy.empty()) {
        const std::size_t cut = proxy.find_first_of("; \t");
        const std::string_view one = trimmed(proxy.substr(0, cut));

        proxy = cut == std::string_view::npos ? std::string_view{} : proxy.substr(cut + 1);

        if (one.empty()) continue;

        const std::size_t equals = one.find('=');

        if (equals == std::string_view::npos) {
            common = one;
        } else if (const std::string scheme = lowered(one.substr(0, equals)); scheme == "http") {
            plain = one.substr(equals + 1);
        } else if (scheme == "https") {
            secure = one.substr(equals + 1);
        }
    }

    return Proxy::parsed(plain.empty() ? common : plain, secure.empty() ? common : secure, bypass);
}

}

}

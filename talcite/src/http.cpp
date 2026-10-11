#include "talcite/http.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

#include "talcite/fs.hpp"
#include "talcite/stream.hpp"

#include "raced.hpp"

// An HTTP/1.1 client, as much as is needed to talk to services such as TMDB and to download images:
// GET, headers, a body framed by Content-Length, in chunks (chunked) or until close, redirects, and
// one idle connection per server. POST serves services such as UPnP (SOAP); it always goes on a new
// connection, because a request that changes something must not be silently repeated on a pooled
// connection that failed halfway. Everything runs on promises over `Stream` and `TlsStream`;
// cryptography stays behind the `TlsProvider` seam, as everywhere in talcite.
//
// A proxy (`Proxy`): plain HTTP goes to it with the full address in the request line, HTTPS through
// a CONNECT tunnel, in which TLS talks directly to the server, so the proxy sees only the name and
// the port. The proxy itself speaks plain HTTP; a proxy behind TLS and SOCKS are not supported.
//
// A response status is not an error: 404 and 429 come back as a `Response`, because the caller
// knows what to do with them. `HttpTrouble` means only a protocol violation or a limit from
// `Limits` being exceeded.
//
// A download (`save`) takes the body of a 2xx answer straight into a file, piece by piece as it
// arrives, without the body limit and without holding it in memory, so a file of gigabytes costs
// one piece of memory; the promise's progress follows the bytes written against Content-Length.
namespace talcite::net {

namespace {

// How many bytes one read asks the connection for.
constexpr std::size_t piece = 64 * 1024;

// The longest chunk header line (chunked) or trailer line; anything longer is no longer a
// conversation.
constexpr std::size_t line_most = 4096;

// An ASCII letter in lowercase; other bytes stay as they are.
char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

// The text with its ASCII letters in lowercase.
std::string lowered(std::string_view text) {
    std::string made{text};

    for (char& one : made) one = lowered(one);

    return made;
}

// Whether two texts are equal ignoring ASCII case, as header names are compared.
bool alike(std::string_view one, std::string_view two) {
    return one.size() == two.size() &&
           std::equal(one.begin(), one.end(), two.begin(), [](char a, char b) { return lowered(a) == lowered(b); });
}

// The text without leading and trailing spaces and tabs.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);

    return text;
}

// A character that may appear in a header name (token in RFC 9110).
bool tokenish(char one) {
    if ((one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9')) return true;

    return std::string_view{"!#$%&'*+-.^_`|~"}.find(one) != std::string_view::npos;
}

// A character of a request target: visible ASCII. Spaces, control characters and bytes outside
// ASCII are encoded by the caller (%20).
bool visible(char one) { return one > ' ' && one < 0x7F; }

// The default port of the scheme: 443 for HTTPS, 80 for HTTP.
std::uint16_t fallback(bool secure) { return secure ? 443 : 80; }

// A port number from decimal digits, 1 to 65535; nothing for anything else.
std::optional<std::uint16_t> ported(std::string_view digits) {
    unsigned made = 0;

    const auto [end, why] = std::from_chars(digits.data(), digits.data() + digits.size(), made);

    if (digits.empty() || why != std::errc{} || end != digits.data() + digits.size() || made == 0 || made > 65535)
        return std::nullopt;

    return static_cast<std::uint16_t>(made);
}

// A host as written in an address: a name or IPv4 from lowercase letters, digits, dots and hyphens
// (and underscores); IPv6 inside brackets.
bool hosted(std::string_view host, bool bracketed) {
    if (host.empty()) return false;

    return std::all_of(host.begin(), host.end(), [bracketed](char one) {
        const bool plain = (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '.' || one == '-';

        return bracketed ? plain || one == ':' : plain || one == '_';
    });
}

// The host for the Host header and for printing: IPv6 in brackets, the port only when it is not the
// scheme's default.
std::string authority(const Url& one) {
    std::string made = one.host.find(':') != std::string::npos ? "[" + one.host + "]" : one.host;

    if (one.port != fallback(one.secure)) made += ":" + std::to_string(one.port);

    return made;
}

// A promise that is already fulfilled.
Promise<void> ready() {
    return Promise<void>{[](auto resolve, auto) { resolve(); }};
}

// A promise that resolves with the given value.
template <typename T>
Promise<T> given(T value) {
    return Promise<T>{[value = std::move(value)](auto resolve, auto) mutable { resolve(std::move(value)); }};
}

// A promise that rejects with the given exception.
template <typename T>
Promise<T> refused(std::exception_ptr why) {
    return Promise<T>{[why](auto, auto reject) { reject(why); }};
}

// A protocol violation with the given message.
HttpTrouble malformed(const std::string& said) { return HttpTrouble{HttpTrouble::Why::malformed, said}; }

// A limit from `Limits` exceeded, with the given message.
HttpTrouble oversized(const std::string& said) { return HttpTrouble{HttpTrouble::Why::oversized, said}; }

// The result of a promise that no longer rejects: a value or a captured exception. This is how a
// failure is caught in order to do something after it: retry on a fresh connection or try the next
// address.
template <typename T>
struct Outcome {
    std::optional<T> value;
    std::exception_ptr why;
};

// Turns a promise into one that always resolves, carrying either the value or the exception.
template <typename T>
Promise<Outcome<T>> caught(Promise<T> one) {
    auto slot = std::make_shared<Outcome<T>>();

    Promise<void> tried = one.then([slot](T value) { slot->value = std::move(value); })
                              .fail([slot](const std::exception&) { slot->why = std::current_exception(); });

    return Promise<void>::all_settled(std::move(tried)).then([slot] { return std::move(*slot); });
}

// One connection: plain or encrypted, with bytes already read but not yet consumed. `heard` tells
// whether at least one byte of the response has arrived: a pooled connection that failed before the
// first byte was simply closed by the server while idle, and the request is sent again.
struct Line {
    Stream plain;
    TlsStream secure;
    Bytes kept;
    bool heard = false;
    bool forwarded = false;
    std::string login;

    // Reads up to `most` bytes through whichever stream the line holds.
    [[nodiscard]] Promise<Bytes> read(std::size_t most, Patience patience) const {
        return secure.held() ? secure.read(most, patience) : plain.read(most, patience);
    }

    // Writes the bytes through whichever stream the line holds.
    [[nodiscard]] Promise<void> write(Bytes what, Patience patience) const {
        return secure.held() ? secure.write(std::move(what), patience) : plain.write(std::move(what), patience);
    }

    // Closes both the encrypted and the plain stream, whichever are held.
    void close() {
        if (secure.held()) secure.close();
        if (plain.held()) plain.close();
    }
};

using Wire = std::shared_ptr<Line>;

using Place = std::tuple<bool, std::string, std::uint16_t>;

// The key of the idle pool: scheme, host and port.
Place place_of(const Url& one) { return Place{one.secure, one.host, one.port}; }

// Adds more bytes to the buffer; false when the other side has closed.
Promise<bool> more(const Wire& wire, Patience patience) {
    return wire->read(piece, patience).then([wire](Bytes got) {
        if (got.empty()) return false;

        wire->heard = true;
        wire->kept.insert(wire->kept.end(), got.begin(), got.end());

        return true;
    });
}

// The offset of `what` in the bytes, or npos.
std::size_t found(const Bytes& in, std::string_view what) {
    const Bytes wanted = bytes(what);

    const auto at = std::search(in.begin(), in.end(), wanted.begin(), wanted.end());

    return at == in.end() ? std::string::npos : static_cast<std::size_t>(at - in.begin());
}

// The bytes from the start of the buffer up to the mark (without it), removed together with the
// mark. A mark further than `most` from the start is an exceeded limit, reported before the rest
// arrives.
Promise<std::string> until(const Wire& wire, std::string_view mark, std::size_t most, Patience patience,
                           bool head) {
    if (const std::size_t at = found(wire->kept, mark); at != std::string::npos) {
        if (at > most) throw head ? oversized("The response head is too large.") : malformed("A line is too long.");

        std::string made = text(std::span<const std::byte>{wire->kept.data(), at});

        wire->kept.erase(wire->kept.begin(), wire->kept.begin() + static_cast<std::ptrdiff_t>(at + mark.size()));

        return given(std::move(made));
    }

    if (wire->kept.size() > most + mark.size())
        throw head ? oversized("The response head is too large.") : malformed("A line is too long.");

    return more(wire, patience).then([wire, mark, most, patience, head](bool got) -> Promise<std::string> {
        if (!got) throw malformed("The connection closed in the middle of the response.");

        return until(wire, mark, most, patience, head);
    });
}

// Where the body of a download goes (`HttpClient::save`): the file, and the counters of the
// promise's progress, the bytes written and the whole from Content-Length (zero when unknown).
struct Spilling {
    fs::File file;
    std::atomic<std::uint64_t>* done = nullptr;
    std::atomic<std::uint64_t>* whole = nullptr;
};

using Spill = std::shared_ptr<Spilling>;

// Where a body goes: into memory, up to `most` bytes, or into the file of a download, without a
// limit, at the next offset (`written`).
struct Sink {
    Bytes memory;
    std::size_t most = 0;
    Spill spill;
    std::uint64_t written = 0;
};

using Sunk = std::shared_ptr<Sink>;

// Takes a piece of the body: appends it in memory within the limit, or writes it to the file and
// counts it for the progress.
Promise<void> took(const Sunk& sink, Bytes part) {
    if (part.empty()) return ready();

    if (!sink->spill) {
        if (part.size() > sink->most || sink->memory.size() > sink->most - part.size())
            throw oversized("The response body is too large.");

        sink->memory.insert(sink->memory.end(), part.begin(), part.end());

        return ready();
    }

    const std::uint64_t at = sink->written;

    sink->written += part.size();

    return sink->spill->file.write(at, std::move(part)).then([sink](std::size_t) { sink->spill->done->store(sink->written); });
}

// The bytes the buffer holds, at most `much` of them, taken out of it.
Bytes taken_out(const Wire& wire, std::uint64_t much) {
    const auto now = static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(much, wire->kept.size()));

    Bytes made(wire->kept.begin(), wire->kept.begin() + now);

    wire->kept.erase(wire->kept.begin(), wire->kept.begin() + now);

    return made;
}

// Exactly `much` bytes of the body, into the sink as they come.
Promise<void> exact(const Wire& wire, std::uint64_t much, const Sunk& sink, Patience patience) {
    if (much == 0) return ready();

    if (!wire->kept.empty()) {
        Bytes part = taken_out(wire, much);
        const std::uint64_t left = much - part.size();

        return took(sink, std::move(part)).then([wire, left, sink, patience] { return exact(wire, left, sink, patience); });
    }

    return more(wire, patience).then([wire, much, sink, patience](bool got) -> Promise<void> {
        if (!got) throw malformed("The connection closed in the middle of the body.");

        return exact(wire, much, sink, patience);
    });
}

// The body until the connection closes: a response without a length and without chunks.
Promise<void> drained(const Wire& wire, const Sunk& sink, Patience patience) {
    Bytes part = taken_out(wire, wire->kept.size());

    return took(sink, std::move(part)).then([wire, sink, patience] {
        return more(wire, patience).then([wire, sink, patience](bool got) -> Promise<void> {
            if (!got) return ready();

            return drained(wire, sink, patience);
        });
    });
}

// A chunk size from hexadecimal digits; nothing when the text is not entirely such a number.
std::optional<std::uint64_t> hexed(std::string_view text) {
    std::uint64_t made = 0;

    const auto [end, why] = std::from_chars(text.data(), text.data() + text.size(), made, 16);

    if (why != std::errc{} || end != text.data() + text.size()) return std::nullopt;

    return made;
}

Promise<void> trailed(const Wire& wire, Patience patience);

// A chunked body: a line with the size in hexadecimal (extensions after a semicolon are skipped),
// the chunk, CRLF; a zero chunk ends it, followed by a trailer up to an empty line, which is read
// and discarded. The sink keeps the limit of a body in memory.
Promise<void> chunked(const Wire& wire, const Sunk& sink, Patience patience) {
    return until(wire, "\r\n", line_most, patience, false).then([wire, sink, patience](std::string line) -> Promise<void> {
        const std::string_view size = trimmed(std::string_view{line}.substr(0, line.find(';')));
        const std::optional<std::uint64_t> much = hexed(size);

        if (!much) throw malformed("A chunk size is not a number.");

        if (*much == 0) return trailed(wire, patience);

        if (!sink->spill && (*much > sink->most || sink->memory.size() > sink->most - *much))
            throw oversized("The response body is too large.");

        // A chunk is followed at once by CRLF: a limit of zero rejects any byte before it.
        return exact(wire, *much, sink, patience)
            .then([wire, patience] { return until(wire, "\r\n", 0, patience, false); })
            .then([wire, sink, patience](const std::string&) { return chunked(wire, sink, patience); });
    });
}

// Reads and discards the trailer lines up to the empty line that ends the response.
Promise<void> trailed(const Wire& wire, Patience patience) {
    return until(wire, "\r\n", line_most, patience, false).then([wire, patience](const std::string& line) -> Promise<void> {
        if (line.empty()) return ready();

        return trailed(wire, patience);
    });
}

// The response head taken apart: version, status, headers.
struct Head {
    bool eleven = true;
    int status = 0;
    std::vector<Header> headers;
};

// Parses the status line and the header lines of a response head (without the final empty line);
// throws a malformed-response error for anything that is not HTTP/1.x.
Head parsed(const std::string& head) {
    Head made;

    std::size_t at = head.find("\r\n");

    const std::string_view first = std::string_view{head}.substr(0, at);

    if (first.size() < 12 || first.substr(0, 7) != "HTTP/1." || (first[7] != '0' && first[7] != '1') || first[8] != ' ' ||
        (first.size() > 12 && first[12] != ' '))
        throw malformed("The status line is not HTTP/1.x.");

    made.eleven = first[7] == '1';

    const std::string_view code = first.substr(9, 3);

    if (!std::all_of(code.begin(), code.end(), [](char one) { return one >= '0' && one <= '9'; }) || code[0] == '0')
        throw malformed("The status code is not a number.");

    made.status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');

    while (at != std::string::npos) {
        const std::size_t from = at + 2;

        at = head.find("\r\n", from);

        const std::string_view line = std::string_view{head}.substr(from, at == std::string::npos ? std::string::npos : at - from);

        // There is no empty line here: the head ends at the first one. A folded header (obs-fold)
        // must be rejected or joined according to RFC 9112; we reject it.
        const std::size_t colon = line.find(':');

        if (colon == std::string_view::npos || colon == 0 ||
            !std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(colon), tokenish))
            throw malformed("A header line is not a header.");

        made.headers.push_back(Header{std::string{line.substr(0, colon)}, std::string{trimmed(line.substr(colon + 1))}});
    }

    return made;
}

// The words of a list header (Connection, Transfer-Encoding) in lowercase.
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

// How to read the body: none, an exact length, in chunks, or until close.
enum class Framing : std::uint8_t { none, sized, pieces, closing };

struct Frame {
    Framing how = Framing::closing;
    std::uint64_t size = 0;
};

// Decides how the body is framed: none for 204 and 304, chunked when Transfer-Encoding says so (any
// other coding is refused), otherwise Content-Length, which must be one number within `most`, and
// until close without it.
Frame framed(const Head& head, std::uint64_t most) {
    if (head.status == 204 || head.status == 304) return {Framing::none, 0};

    if (const std::vector<std::string> codings = worded(head.headers, "transfer-encoding"); !codings.empty()) {
        if (codings.back() != "chunked" || codings.size() > 1) throw malformed("Only the chunked transfer coding is understood.");

        return {Framing::pieces, 0};
    }

    std::optional<std::uint64_t> size;

    for (const Header& one : head.headers) {
        if (!alike(one.name, "content-length")) continue;

        std::uint64_t made = 0;

        const auto [end, why] = std::from_chars(one.value.data(), one.value.data() + one.value.size(), made);

        if (why != std::errc{} || end != one.value.data() + one.value.size() || (size && *size != made))
            throw malformed("The content length is not one number.");

        size = made;
    }

    if (!size) return {Framing::closing, 0};

    if (*size > most) throw oversized("The response body is too large.");

    return {Framing::sized, *size};
}

// Whether the connection may be taken for the next request: HTTP/1.1, no "Connection: close", a
// body with a boundary, and nothing extra left in the buffer.
bool reusable(const Head& head, Framing how, const Wire& wire) {
    if (!head.eleven || how == Framing::closing || !wire->kept.empty()) return false;

    const std::vector<std::string> words = worded(head.headers, "connection");

    return std::find(words.begin(), words.end(), "close") == words.end();
}

// The proxy credential: Basic with the "user:password" pair in base64 (RFC 7617).
std::string based(std::string_view text) {
    static constexpr std::string_view letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string made;

    for (std::size_t at = 0; at < text.size(); at += 3) {
        const std::size_t much = std::min<std::size_t>(3, text.size() - at);

        std::uint32_t three = 0;

        for (std::size_t one = 0; one < 3; ++one)
            three = (three << 8) | (one < much ? static_cast<unsigned char>(text[at + one]) : 0u);

        for (std::size_t one = 0; one < 4; ++one)
            made += one <= much ? letters[(three >> (18 - 6 * one)) & 0x3F] : '=';
    }

    return made;
}

// The Proxy-Authorization header line for the login, or nothing without one.
std::string authorized(const std::string& login) {
    return login.empty() ? std::string{} : "Proxy-Authorization: Basic " + based(login) + "\r\n";
}

// The request for the server or, through a proxy over plain HTTP, with the full address in the
// request line (RFC 9112, 3.2.2) and the proxy credential.
Bytes requested(const Request& request, const Line& line) {
    const std::string target = line.forwarded ? to_string(request.url) : request.url.target;

    std::string made = request.method + " " + target + " HTTP/1.1\r\nHost: " + authority(request.url) + "\r\n";

    if (line.forwarded) made += authorized(line.login);

    for (const Header& one : request.headers) {
        if (alike(one.name, "host") || alike(one.name, "connection") || alike(one.name, "accept-encoding") ||
            alike(one.name, "proxy-authorization") || alike(one.name, "content-length") ||
            alike(one.name, "transfer-encoding"))
            continue;

        made += one.name + ": " + one.value + "\r\n";
    }

    if (request.method != "GET") made += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";

    made += "Accept-Encoding: identity\r\n\r\n";

    Bytes whole = bytes(made);

    whole.insert(whole.end(), request.body.begin(), request.body.end());

    return whole;
}

// Refuses a request whose header names are not tokens or whose values contain CR, LF or NUL, which
// would let a caller inject lines.
void checked(const Request& request) {
    for (const Header& one : request.headers) {
        if (one.name.empty() || !std::all_of(one.name.begin(), one.name.end(), tokenish))
            throw malformed("A request header name is not a token.");

        if (std::any_of(one.value.begin(), one.value.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; }))
            throw malformed("A request header value breaks the line.");
    }
}

// A redirect address relative to the current one: full, without a scheme (//host/...), from the
// root (/...) or relative, where the last one replaces the last segment of the path.
std::optional<Url> joined(const Url& base, std::string_view location) {
    if (location.find("://") != std::string_view::npos) return Url::from_string(location);

    if (location.substr(0, 2) == "//") return Url::from_string(std::string{base.secure ? "https:" : "http:"} + std::string{location});

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

}

// --- Url -----------------------------------------------------------------------------------------

// An address relative to this one, like a redirect target; addresses from device descriptions are
// resolved the same way.
std::optional<Url> Url::resolved(std::string_view location) const { return joined(*this, location); }

// Parses an http or https address; nothing for any other scheme or a malformed host, port or
// target. The fragment is dropped and an empty target becomes "/".
std::optional<Url> Url::from_string(std::string_view text) {
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
            if (after.front() != ':') return std::nullopt;

            port = after.substr(1);

            if (port.empty()) return std::nullopt;
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

// The address written out again, with the port only when it is not the default.
std::string to_string(const Url& one) { return std::string{one.secure ? "https://" : "http://"} + authority(one) + one.target; }

// --- Proxy ---------------------------------------------------------------------------------------

namespace {

// The value of one hexadecimal digit, or -1.
int nibble(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// %40 and similar in the user and the password: in an environment variable, @ and : in a password
// must be encoded, and the proxy receives them in the clear.
std::string unescaped(std::string_view text) {
    std::string made;

    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '%' && at + 2 < text.size() && nibble(text[at + 1]) >= 0 && nibble(text[at + 2]) >= 0) {
            made += static_cast<char>(nibble(text[at + 1]) * 16 + nibble(text[at + 2]));
            at += 2;
            continue;
        }

        made += text[at];
    }

    return made;
}

// A proxy from a notation such as http://user:password@proxy:3128/; without a scheme it is HTTP, as
// in curl and wget. A scheme other than HTTP(S), such as SOCKS, and a malformed notation give
// nothing.
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

    const std::optional<Url> url = Url::from_string(whole);

    if (!url) return std::nullopt;

    made.url = *url;
    made.url.target = "/";

    return made;
}

// The exception list of no_proxy: names separated by commas or spaces, lowercase, "*." shortened to
// ".".
std::vector<std::string> bypassed(std::string_view text) {
    std::vector<std::string> made;

    while (!text.empty()) {
        const std::size_t cut = text.find_first_of(", ");
        std::string one = lowered(trimmed(text.substr(0, cut)));

        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 1);

        if (one.empty()) continue;

        if (one.rfind("*.", 0) == 0) one.erase(0, 1);

        // A port next to a name does not narrow anything, so it is dropped; an IPv6 address in
        // brackets loses the brackets.
        if (one.front() == '[') {
            one = one.substr(1, one.find(']') - 1);
        } else if (std::count(one.begin(), one.end(), ':') == 1) {
            one.erase(one.find(':'));
        }

        made.push_back(std::move(one));
    }

    return made;
}

// The first of the two environment variables that is set and not empty, or an empty text.
std::string environment(const char* one, const char* two) {
    for (const char* name : {one, two}) {
        if (name == nullptr) continue;

        if (const char* told = std::getenv(name); told != nullptr && *told != '\0') return told;
    }

    return {};
}

}

// A proxy configuration from the three texts: for plain HTTP, for HTTPS and the exception list.
Proxy Proxy::from_strings(std::string_view plain, std::string_view secure, std::string_view bypass) {
    Proxy made;

    made.plain = gated(plain);
    made.secure = gated(secure);
    made.bypass = bypassed(bypass);

    return made;
}

// Variables as in curl: lowercase before uppercase, ALL_PROXY for both when a separate one is
// missing. Uppercase HTTP_PROXY is deliberately skipped: under CGI it is set by the Proxy: header
// of someone else's request (httpoxy), so curl does not read it either.
Proxy Proxy::from_environment() {
    std::string plain = environment("http_proxy", nullptr);
    std::string secure = environment("https_proxy", "HTTPS_PROXY");
    const std::string all = environment("all_proxy", "ALL_PROXY");

    if (plain.empty()) plain = all;
    if (secure.empty()) secure = all;

    return from_strings(plain, secure, environment("no_proxy", "NO_PROXY"));
}

// The proxy for the address, or nothing when the address is in the exceptions: "*" means all, a
// name means itself and its subdomains, and ".name" the same.
const Gateway* Proxy::chosen(const Url& target) const {
    for (const std::string& one : bypass) {
        if (one == "*") return nullptr;

        // A leading dot means the same as the bare name.
        const std::string_view name = one.front() == '.' ? std::string_view{one}.substr(1) : std::string_view{one};

        if (name.empty()) continue;

        if (target.host == name) return nullptr;

        if (target.host.size() > name.size() && target.host.ends_with(name) &&
            target.host[target.host.size() - name.size() - 1] == '.')
            return nullptr;
    }

    const std::optional<Gateway>& made = target.secure ? secure : plain;

    return made ? &*made : nullptr;
}

// --- Response ------------------------------------------------------------------------------------

// The value of the first header with this name, ignoring case, or null.
const std::string* Response::header(std::string_view name) const {
    for (const Header& one : headers)
        if (alike(one.name, name)) return &one.value;

    return nullptr;
}

// --- HttpTrouble ---------------------------------------------------------------------------------

HttpTrouble::HttpTrouble(Why why, const std::string& said, Address where)
    : Trouble(said, 0, std::move(where)), why_(why) {}

// --- HttpClient ----------------------------------------------------------------------------------

// The client's state: the TLS seam, the limits and the pool of idle connections, one per server. A
// second connection returned for the same server closes the previous one: with a service such as
// TMDB, requests go one after another from a queue, and a pool entry for every parallel request
// would hold sockets nobody uses.
struct HttpClient::Held {
    Seam<TlsProvider> tls;
    Limits limits;
    Proxy proxy;

    std::mutex guard;
    std::map<Place, Wire> idle;

    // The slot of a taken connection stays empty and does not disappear: the servers one talks to
    // are a few, not thousands.
    [[nodiscard]] Wire taken(const Place& where) {
        const std::lock_guard<std::mutex> held{guard};

        const auto one = idle.find(where);

        return one == idle.end() ? nullptr : std::exchange(one->second, nullptr);
    }

    // Puts the connection into the pool for its server and closes the one it replaces, outside the
    // lock.
    void returned(const Place& where, Wire wire) {
        Wire old;

        {
            const std::lock_guard<std::mutex> held{guard};

            Wire& slot = idle[where];

            old = std::move(slot);
            slot = std::move(wire);
        }

        if (old) old->close();
    }
};

namespace {

using Kept = std::shared_ptr<HttpClient::Held>;

// A connection to the host and port: the name is resolved and the addresses race (`raced`); the
// first one to connect wins. `resolve` does not return an empty list: a name without addresses
// rejects.
Promise<Stream> reached(const std::string& host, std::uint16_t port, Patience patience) {
    return resolve(host, port).then([patience](std::vector<Address> found) { return raced(std::move(found), patience); });
}

// Wraps the stream into a line: as it is for HTTP, after a TLS client handshake for HTTPS.
Promise<Wire> wrapped(Stream stream, const Url& url, const Seam<TlsProvider>& tls, Patience patience) {
    if (!url.secure) {
        auto made = std::make_shared<Line>();

        made->plain = std::move(stream);

        return given(std::move(made));
    }

    return TlsStream::client(std::move(stream), tls, url.host, patience).then([](TlsStream secure) {
        auto made = std::make_shared<Line>();

        made->secure = std::move(secure);

        return made;
    });
}

// A tunnel through the proxy: CONNECT host:port, a 2xx answer, and from then on the bytes go
// straight to the server. A refusal (407 without a credential, 403 for a forbidden port) is the
// proxy's trouble, not the server's answer, so it does not come back as a `Response`. After 2xx the
// proxy must not send anything before the TLS greeting; bytes in the buffer are a protocol
// violation.
Promise<Stream> tunneled(Stream stream, const Gateway& via, const Url& url, std::size_t head, Patience patience) {
    const std::string place = (url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host) + ":" +
                              std::to_string(url.port);

    auto wire = std::make_shared<Line>();

    wire->plain = std::move(stream);

    const std::string asked = "CONNECT " + place + " HTTP/1.1\r\nHost: " + place + "\r\n" + authorized(via.login) + "\r\n";

    return wire->write(bytes(asked), patience)
        .then([wire, head, patience] { return until(wire, "\r\n\r\n", head, patience, true); })
        .then([wire](const std::string& text) {
            const Head told = parsed(text);

            if (told.status < 200 || told.status > 299) {
                wire->close();

                throw HttpTrouble{HttpTrouble::Why::proxy,
                                  "The proxy refused the tunnel with status " + std::to_string(told.status) + "."};
            }

            if (!wire->kept.empty()) {
                wire->close();

                throw malformed("The proxy sent bytes before the server spoke.");
            }

            return wire->plain;
        });
}

// A new line to the address: directly, through a proxy with the full address for HTTP, or through a
// CONNECT tunnel for HTTPS. A proxy reached over HTTPS is refused.
Promise<Wire> dialed(const Kept& held, const Url& url) {
    const Patience patience = held->limits.patience;
    const Seam<TlsProvider> tls = held->tls;

    const Gateway* via = held->proxy.chosen(url);

    if (via == nullptr)
        return reached(url.host, url.port, patience).then([url, patience, tls](Stream stream) {
            return wrapped(std::move(stream), url, tls, patience);
        });

    if (via->url.secure)
        throw HttpTrouble{HttpTrouble::Why::proxy, "A proxy reached over HTTPS is not supported."};

    const Gateway gate = *via;
    const std::size_t head = held->limits.head;

    return reached(gate.url.host, gate.url.port, patience).then([url, gate, head, patience, tls](Stream stream) -> Promise<Wire> {
        if (!url.secure) {
            auto made = std::make_shared<Line>();

            made->plain = std::move(stream);
            made->forwarded = true;
            made->login = gate.login;

            return given(std::move(made));
        }

        return tunneled(std::move(stream), gate, url, head, patience).then([url, tls, patience](Stream inside) {
            return wrapped(std::move(inside), url, tls, patience);
        });
    });
}

// One exchange on the given connection: the request, the head (skipping 1xx responses), the body.
// At the end the connection goes back to the pool if it can be used again, otherwise it is closed.
// With a spill (a download), the body of a 2xx answer goes into its file without the body limit;
// any other answer (a redirect, an error page) stays in memory as usual.
Promise<Response> exchanged(const Kept& held, Request request, Wire wire, Spill spill) {
    const Patience patience = held->limits.patience;
    const Place where = place_of(request.url);

    return wire->write(requested(request, *wire), patience)
        .then([held, wire, patience] { return until(wire, "\r\n\r\n", held->limits.head, patience, true); })
        .then([held, wire, patience](std::string head) -> Promise<std::string> {
            const Head first = parsed(head);

            if (first.status >= 100 && first.status < 200)
                return until(wire, "\r\n\r\n", held->limits.head, patience, true);

            return given(std::move(head));
        })
        .then([held, wire, patience, where, url = request.url, spill](const std::string& text) -> Promise<Response> {
            const Head head = parsed(text);

            if (head.status >= 100 && head.status < 200) throw malformed("Too many interim responses.");

            const bool spilling = spill != nullptr && head.status >= 200 && head.status < 300;
            const Frame frame = framed(head, spilling ? std::numeric_limits<std::uint64_t>::max() : held->limits.body);

            const auto sink = std::make_shared<Sink>();

            sink->most = held->limits.body;

            if (spilling) {
                sink->spill = spill;
                spill->whole->store(frame.how == Framing::sized ? frame.size : 0);
            }

            Promise<void> read = frame.how == Framing::sized   ? exact(wire, frame.size, sink, patience)
                                 : frame.how == Framing::pieces ? chunked(wire, sink, patience)
                                 : frame.how == Framing::closing ? drained(wire, sink, patience)
                                                                 : ready();

            return read.then([held, wire, where, url, head, frame, sink] {
                if (reusable(head, frame.how, wire)) {
                    wire->heard = false;

                    held->returned(where, wire);
                } else {
                    wire->close();
                }

                return Response{head.status, head.headers, std::move(sink->memory), url};
            });
        });
}

// An exchange on a pooled connection or a new one. A pooled connection that failed before the first
// byte of the response was closed by the server while idle, and the request is then sent again on a
// new one.
Promise<Response> attempted(const Kept& held, const Request& request, const Spill& spill) {
    if (Wire idle = request.method == "GET" ? held->taken(place_of(request.url)) : Wire{}) {
        return caught(exchanged(held, request, idle, spill))
            .then([held, request, idle, spill](Outcome<Response> got) -> Promise<Response> {
                if (got.value) return given(std::move(*got.value));

                idle->close();

                if (idle->heard) return refused<Response>(got.why);

                return dialed(held, request.url).then([held, request, spill](Wire wire) { return exchanged(held, request, wire, spill); });
            });
    }

    return dialed(held, request.url).then([held, request, spill](Wire wire) { return exchanged(held, request, wire, spill); });
}

// Whether the status is a redirect that is followed: 301, 302, 303, 307 or 308.
bool redirecting(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Sends the request and follows redirects up to the limit, refusing a downgrade from HTTPS to HTTP.
// A download keeps its spill through the redirects, so the body of the last answer lands in the file.
Promise<Response> fetched(const Kept& held, Request request, int hops, Spill spill = nullptr) {
    checked(request);

    if (request.url.secure && !held->tls.bound())
        throw HttpTrouble{HttpTrouble::Why::insecure, "HTTPS needs a TLS provider."};

    return attempted(held, request, spill).then([held, request, hops, spill](Response got) -> Promise<Response> {
        const std::string* location = got.header("location");

        if (!redirecting(got.status) || location == nullptr) return given(std::move(got));

        if (hops >= held->limits.redirects) throw HttpTrouble{HttpTrouble::Why::redirected, "Too many redirects."};

        const std::optional<Url> next = joined(request.url, *location);

        if (!next) throw malformed("The redirect location is not an address.");

        if (request.url.secure && !next->secure)
            throw HttpTrouble{HttpTrouble::Why::downgraded, "A redirect leaves HTTPS for plain HTTP."};

        // Credentials do not follow a redirect to another server. 307 and 308 repeat the request
        // together with its body; the other redirects become a GET without a body.
        Request again{*next, {}};

        if (got.status == 307 || got.status == 308) {
            again.method = request.method;
            again.body = request.body;
        }

        for (const Header& one : request.headers)
            if (next->host == request.url.host || (!alike(one.name, "authorization") && !alike(one.name, "cookie")))
                again.headers.push_back(one);

        return fetched(held, std::move(again), hops + 1, spill);
    });
}

}

// A client with the TLS seam, the limits and the proxy configuration.
HttpClient::HttpClient(Seam<TlsProvider> tls, Limits limits, Proxy proxy) : held_(std::make_shared<Held>()) {
    held_->tls = std::move(tls);
    held_->limits = limits;
    held_->proxy = std::move(proxy);
}

namespace {

// Like every network promise, it starts at once, not at the first wait.
Promise<Response> sent(const Kept& held, Request request) {
    if (!held) return refused<Response>(std::make_exception_ptr(Trouble{"The HTTP client is empty."}));

    Promise<Response> made = given(0).then([held, request = std::move(request)](int) { return fetched(held, request, 0); });

    made.start();

    return made;
}

}

// Sends a GET request; any body in the request is dropped.
Promise<Response> HttpClient::get(Request request) const {
    request.method = "GET";
    request.body.clear();

    return sent(held_, std::move(request));
}

// Sends a POST request with the given body, always on a new connection.
Promise<Response> HttpClient::post(Request request, Bytes body) const {
    request.method = "POST";
    request.body = std::move(body);

    return sent(held_, std::move(request));
}

namespace {

// The ticket of a download: the bytes written and the whole, for the progress bar.
struct Saved : detail::Result<Response> {
    std::atomic<std::uint64_t> done{0};
    std::atomic<std::uint64_t> whole{0};

    // The finished share between 0 and 1; a settled download counts as complete, one of unknown
    // length as not begun.
    [[nodiscard]] double progress() const override {
        if (detail::over(*this)) return 1.0;

        const std::uint64_t all = whole.load();

        if (all == 0) return 0.0;

        return std::min(1.0, static_cast<double>(done.load()) / static_cast<double>(all));
    }
};

// A promise whose ticket counts the bytes of a download.
class SavePromise : public Promise<Response> {
public:
    // Builds the promise around its callback and the counting ticket.
    SavePromise(Promise<Response>::Callback given, std::shared_ptr<Saved> made)
        : Promise<Response>(std::move(given), std::move(made)) {}
};

// The work of a download on the pool: the file opened anew, the exchange awaited (which hears a
// cancellation), and the file removed again unless a whole 2xx body is in it.
Response downloaded(const Kept& held, const Request& request, const std::filesystem::path& into, Saved& counted) {
    if (!held) throw Trouble{"The HTTP client is empty."};

    const auto spill = std::make_shared<Spilling>();

    spill->file = fs::open(into, fs::Mode::truncate).await();
    spill->done = &counted.done;
    spill->whole = &counted.whole;

    Response got;
    std::error_code ignored;

    try {
        got = fetched(held, request, 0, spill).await();
    } catch (...) {
        spill->file.close();
        std::filesystem::remove(into, ignored);

        throw;
    }

    spill->file.close();

    if (got.status < 200 || got.status > 299) std::filesystem::remove(into, ignored);

    return got;
}

}

// Downloads with a GET into the file at `into`, created anew: the body of a 2xx answer goes to the
// file piece by piece, and the answer comes back with an empty body. Any other answer comes back as
// usual, with its body in memory, and leaves no file; neither does a failed or cancelled download,
// so a file at `into` is always a whole body. Like every network promise, it starts at once.
Promise<Response> HttpClient::save(Request request, std::filesystem::path into) const {
    request.method = "GET";
    request.body.clear();

    auto counted = std::make_shared<Saved>();

    const auto work = [held = held_, request = std::move(request), into = std::move(into), counted](auto resolve, auto) {
        resolve(downloaded(held, request, into, *counted));
    };

    Promise<Response> made = SavePromise{work, counted};

    made.start();

    return made;
}

}

#include "talcite/http.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <span>
#include <tuple>
#include <utility>

#include "talcite/stream.hpp"

#include "raced.hpp"

// **Klient HTTP/1.1** — tyle, ile potrzeba, żeby rozmawiać z usługami w rodzaju TMDB i pobierać
// obrazy: GET, nagłówki, treść z `Content-Length`, kawałkami (`chunked`) albo do zamknięcia,
// przekierowania i jedno bezczynne połączenie na serwer. POST dla usług w rodzaju UPnP (SOAP) —
// zawsze na nowym połączeniu, bo żądania, które coś zmienia, nie wolno po cichu powtórzyć na
// połączeniu z zapasu, które padło w pół drogi. Wszystko idzie obietnicami na `Stream`
// i `TlsStream`; kryptografia zostaje za szwem `TlsProvider`, jak wszędzie w talcite.
//
// **Pośrednik** (`Proxy`): zwykłe HTTP idzie do niego z pełnym adresem w wierszu żądania, HTTPS
// przez tunel `CONNECT`, w którym TLS rozmawia już wprost z serwerem — pośrednik widzi tylko nazwę
// i port. Sam pośrednik mówi zwykłym HTTP; pośrednik za TLS i SOCKS nie są obsługiwane.
//
// Status odpowiedzi nie jest błędem: 404 i 429 wracają jako `Response`, bo co z nimi zrobić, wie
// wołający. `HttpTrouble` znaczy wyłącznie złamanie protokołu albo przekroczenie granicy z `Limits`.
namespace talcite::net {

namespace {

constexpr std::size_t piece = 64 * 1024;

// Wiersz nagłówka kawałka (`chunked`) i wiersz przyczepki — dłuższy nie jest już rozmową.
constexpr std::size_t line_most = 4096;

char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

std::string lowered(std::string_view text) {
    std::string made{text};

    for (char& one : made) one = lowered(one);

    return made;
}

bool alike(std::string_view one, std::string_view two) {
    return one.size() == two.size() &&
           std::equal(one.begin(), one.end(), two.begin(), [](char a, char b) { return lowered(a) == lowered(b); });
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);

    return text;
}

// Znak, który może stać w nazwie nagłówka (`token` z RFC 9110).
bool tokenish(char one) {
    if ((one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9')) return true;

    return std::string_view{"!#$%&'*+-.^_`|~"}.find(one) != std::string_view::npos;
}

// Znak celu żądania: widoczny ASCII. Spację, sterujące i bajty spoza ASCII koduje wołający (%20).
bool visible(char one) { return one > ' ' && one < 0x7F; }

std::uint16_t fallback(bool secure) { return secure ? 443 : 80; }

std::optional<std::uint16_t> ported(std::string_view digits) {
    unsigned made = 0;

    const auto [end, why] = std::from_chars(digits.data(), digits.data() + digits.size(), made);

    if (digits.empty() || why != std::errc{} || end != digits.data() + digits.size() || made == 0 || made > 65535)
        return std::nullopt;

    return static_cast<std::uint16_t>(made);
}

// Host w zapisie adresu: nazwa albo IPv4 z liter, cyfr, kropek i myślników; IPv6 w nawiasach.
bool hosted(std::string_view host, bool bracketed) {
    if (host.empty()) return false;

    return std::all_of(host.begin(), host.end(), [bracketed](char one) {
        const bool plain = (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '.' || one == '-';

        return bracketed ? plain || one == ':' : plain || one == '_';
    });
}

std::string authority(const Url& one) {
    std::string made = one.host.find(':') != std::string::npos ? "[" + one.host + "]" : one.host;

    if (one.port != fallback(one.secure)) made += ":" + std::to_string(one.port);

    return made;
}

Promise<void> ready() {
    return Promise<void>{[](auto resolve, auto) { resolve(); }};
}

template <typename T>
Promise<T> given(T value) {
    return Promise<T>{[value = std::move(value)](auto resolve, auto) mutable { resolve(std::move(value)); }};
}

template <typename T>
Promise<T> refused(std::exception_ptr why) {
    return Promise<T>{[why](auto, auto reject) { reject(why); }};
}

HttpTrouble malformed(const std::string& said) { return HttpTrouble{HttpTrouble::Why::malformed, said}; }

HttpTrouble oversized(const std::string& said) { return HttpTrouble{HttpTrouble::Why::oversized, said}; }

// Wynik obietnicy, która już nie odrzuca: wartość albo przechwycony wyjątek. Tak łapie się porażkę,
// żeby coś po niej zrobić — ponowić na świeżym połączeniu albo spróbować następnego adresu.
template <typename T>
struct Outcome {
    std::optional<T> value;
    std::exception_ptr why;
};

template <typename T>
Promise<Outcome<T>> caught(Promise<T> one) {
    auto slot = std::make_shared<Outcome<T>>();

    Promise<void> tried = one.then([slot](T value) { slot->value = std::move(value); })
                              .fail([slot](const std::exception&) { slot->why = std::current_exception(); });

    return Promise<void>::all_settled(std::move(tried)).then([slot] { return std::move(*slot); });
}

// **Jedno połączenie**: zwykłe albo szyfrowane, z bajtami przeczytanymi, a jeszcze nie zużytymi.
// `heard` mówi, czy przyszedł już choć bajt odpowiedzi — połączenie wzięte z zapasu, które padło
// przed pierwszym bajtem, serwer po prostu zamknął w bezczynności i żądanie idzie jeszcze raz.
struct Line {
    Stream plain;
    TlsStream secure;
    Bytes kept;
    bool heard = false;
    bool forwarded = false;
    std::string login;

    [[nodiscard]] Promise<Bytes> read(std::size_t most, Patience patience) const {
        return secure.held() ? secure.read(most, patience) : plain.read(most, patience);
    }

    [[nodiscard]] Promise<void> write(Bytes what, Patience patience) const {
        return secure.held() ? secure.write(std::move(what), patience) : plain.write(std::move(what), patience);
    }

    void close() {
        if (secure.held()) secure.close();
        if (plain.held()) plain.close();
    }
};

using Wire = std::shared_ptr<Line>;

using Place = std::tuple<bool, std::string, std::uint16_t>;

Place place_of(const Url& one) { return Place{one.secure, one.host, one.port}; }

// Dołożenie do zapasu bajtów; `false`, gdy druga strona zamknęła.
Promise<bool> more(const Wire& wire, Patience patience) {
    return wire->read(piece, patience).then([wire](Bytes got) {
        if (got.empty()) return false;

        wire->heard = true;
        wire->kept.insert(wire->kept.end(), got.begin(), got.end());

        return true;
    });
}

std::size_t found(const Bytes& in, std::string_view what) {
    const Bytes wanted = bytes(what);

    const auto at = std::search(in.begin(), in.end(), wanted.begin(), wanted.end());

    return at == in.end() ? std::string::npos : static_cast<std::size_t>(at - in.begin());
}

// Bajty od początku zapasu aż do znacznika (bez niego), zdjęte razem ze znacznikiem. Znacznik
// dalej niż `most` od początku to przekroczona granica — zanim przyjdzie reszta.
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

// Dokładnie `much` bajtów treści, dopisanych do `into`.
Promise<void> exact(const Wire& wire, std::size_t much, std::shared_ptr<Bytes> into, Patience patience) {
    if (wire->kept.size() >= much) {
        into->insert(into->end(), wire->kept.begin(), wire->kept.begin() + static_cast<std::ptrdiff_t>(much));
        wire->kept.erase(wire->kept.begin(), wire->kept.begin() + static_cast<std::ptrdiff_t>(much));

        return ready();
    }

    return more(wire, patience).then([wire, much, into, patience](bool got) -> Promise<void> {
        if (!got) throw malformed("The connection closed in the middle of the body.");

        return exact(wire, much, into, patience);
    });
}

// Treść do zamknięcia połączenia — odpowiedź bez długości i bez kawałków.
Promise<void> drained(const Wire& wire, std::shared_ptr<Bytes> into, std::size_t most, Patience patience) {
    into->insert(into->end(), wire->kept.begin(), wire->kept.end());
    wire->kept.clear();

    if (into->size() > most) throw oversized("The response body is too large.");

    return more(wire, patience).then([wire, into, most, patience](bool got) -> Promise<void> {
        if (!got) return ready();

        return drained(wire, into, most, patience);
    });
}

std::optional<std::size_t> hexed(std::string_view text) {
    std::size_t made = 0;

    const auto [end, why] = std::from_chars(text.data(), text.data() + text.size(), made, 16);

    if (why != std::errc{} || end != text.data() + text.size()) return std::nullopt;

    return made;
}

Promise<void> trailed(const Wire& wire, Patience patience);

// Treść kawałkami: wiersz z długością szesnastkowo (rozszerzenia po średniku się pomija), kawałek,
// CRLF; kawałek zerowy kończy, a po nim przyczepka do pustego wiersza — czytana i odrzucana.
Promise<void> chunked(const Wire& wire, std::shared_ptr<Bytes> into, std::size_t most, Patience patience) {
    return until(wire, "\r\n", line_most, patience, false)
        .then([wire, into, most, patience](std::string line) -> Promise<void> {
            const std::string_view size = trimmed(std::string_view{line}.substr(0, line.find(';')));
            const std::optional<std::size_t> much = hexed(size);

            if (!much) throw malformed("A chunk size is not a number.");

            if (*much == 0) return trailed(wire, patience);

            if (*much > most || into->size() > most - *much) throw oversized("The response body is too large.");

            // Po kawałku od razu CRLF: granica zero odrzuca każdy bajt przed nim.
            return exact(wire, *much, into, patience)
                .then([wire, patience] { return until(wire, "\r\n", 0, patience, false); })
                .then([wire, into, most, patience](const std::string&) { return chunked(wire, into, most, patience); });
        });
}

Promise<void> trailed(const Wire& wire, Patience patience) {
    return until(wire, "\r\n", line_most, patience, false).then([wire, patience](const std::string& line) -> Promise<void> {
        if (line.empty()) return ready();

        return trailed(wire, patience);
    });
}

// Głowa odpowiedzi rozebrana: wersja, status, nagłówki.
struct Head {
    bool eleven = true;
    int status = 0;
    std::vector<Header> headers;
};

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

        // Pustego wiersza tu nie ma — głowa kończy się na pierwszym. Zawinięty nagłówek (obs-fold)
        // RFC 9112 każe odrzucić albo skleić; odrzucamy.
        const std::size_t colon = line.find(':');

        if (colon == std::string_view::npos || colon == 0 ||
            !std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(colon), tokenish))
            throw malformed("A header line is not a header.");

        made.headers.push_back(Header{std::string{line.substr(0, colon)}, std::string{trimmed(line.substr(colon + 1))}});
    }

    return made;
}

// Słowa listy nagłówka (`Connection`, `Transfer-Encoding`) małymi literami.
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

// Jak odczytać treść: bez niej, dokładna długość, kawałkami albo do zamknięcia.
enum class Framing : std::uint8_t { none, sized, pieces, closing };

struct Frame {
    Framing how = Framing::closing;
    std::size_t size = 0;
};

Frame framed(const Head& head, std::size_t most) {
    if (head.status == 204 || head.status == 304) return {Framing::none, 0};

    if (const std::vector<std::string> codings = worded(head.headers, "transfer-encoding"); !codings.empty()) {
        if (codings.back() != "chunked" || codings.size() > 1) throw malformed("Only the chunked transfer coding is understood.");

        return {Framing::pieces, 0};
    }

    std::optional<std::size_t> size;

    for (const Header& one : head.headers) {
        if (!alike(one.name, "content-length")) continue;

        std::size_t made = 0;

        const auto [end, why] = std::from_chars(one.value.data(), one.value.data() + one.value.size(), made);

        if (why != std::errc{} || end != one.value.data() + one.value.size() || (size && *size != made))
            throw malformed("The content length is not one number.");

        size = made;
    }

    if (!size) return {Framing::closing, 0};

    if (*size > most) throw oversized("The response body is too large.");

    return {Framing::sized, *size};
}

// Czy połączenie wolno wziąć do następnego żądania: HTTP/1.1, bez `Connection: close`, treść
// z granicą i nic nadmiarowego w zapasie.
bool reusable(const Head& head, Framing how, const Wire& wire) {
    if (!head.eleven || how == Framing::closing || !wire->kept.empty()) return false;

    const std::vector<std::string> words = worded(head.headers, "connection");

    return std::find(words.begin(), words.end(), "close") == words.end();
}

// Podpis dla pośrednika: `Basic` z parą „użytkownik:hasło" w base64 (RFC 7617).
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

std::string authorized(const std::string& login) {
    return login.empty() ? std::string{} : "Proxy-Authorization: Basic " + based(login) + "\r\n";
}

// Żądanie do serwera albo — przez pośrednika, zwykłym HTTP — z pełnym adresem w wierszu żądania
// (RFC 9112, 3.2.2) i podpisem dla pośrednika.
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

void checked(const Request& request) {
    for (const Header& one : request.headers) {
        if (one.name.empty() || !std::all_of(one.name.begin(), one.name.end(), tokenish))
            throw malformed("A request header name is not a token.");

        if (std::any_of(one.value.begin(), one.value.end(), [](char c) { return c == '\r' || c == '\n' || c == '\0'; }))
            throw malformed("A request header value breaks the line.");
    }
}

// Adres przekierowania względem bieżącego: pełny, bez schematu (`//host/...`), od korzenia
// (`/...`) albo względny — ten ostatni zastępuje ostatni odcinek ścieżki.
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

// Adres względem tego — jak cel przekierowania; tak samo rozwiązuje się adresy z opisów urządzeń.
std::optional<Url> Url::resolved(std::string_view location) const { return joined(*this, location); }

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

std::string to_string(const Url& one) { return std::string{one.secure ? "https://" : "http://"} + authority(one) + one.target; }

// --- Proxy ---------------------------------------------------------------------------------------

namespace {

int nibble(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// `%40` i podobne w użytkowniku i haśle — w zmiennej środowiska `@` i `:` w haśle muszą być
// zakodowane, a pośrednik dostaje je jawnie.
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

// Pośrednik z zapisu w rodzaju `http://użytkownik:hasło@pośrednik:3128/`; bez schematu to HTTP,
// jak u curl i wget. Schemat inny niż HTTP(S) — SOCKS — i zapis krzywy dają brak.
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

std::vector<std::string> bypassed(std::string_view text) {
    std::vector<std::string> made;

    while (!text.empty()) {
        const std::size_t cut = text.find_first_of(", ");
        std::string one = lowered(trimmed(text.substr(0, cut)));

        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 1);

        if (one.empty()) continue;

        if (one.rfind("*.", 0) == 0) one.erase(0, 1);

        // Port przy nazwie nie zawęża — pomijamy go; adres IPv6 w nawiasach traci nawiasy.
        if (one.front() == '[') {
            one = one.substr(1, one.find(']') - 1);
        } else if (std::count(one.begin(), one.end(), ':') == 1) {
            one.erase(one.find(':'));
        }

        made.push_back(std::move(one));
    }

    return made;
}

std::string environment(const char* one, const char* two) {
    for (const char* name : {one, two}) {
        if (name == nullptr) continue;

        if (const char* told = std::getenv(name); told != nullptr && *told != '\0') return told;
    }

    return {};
}

}

Proxy Proxy::from_strings(std::string_view plain, std::string_view secure, std::string_view bypass) {
    Proxy made;

    made.plain = gated(plain);
    made.secure = gated(secure);
    made.bypass = bypassed(bypass);

    return made;
}

// Zmienne jak u curl: małe litery przed wielkimi, `ALL_PROXY` dla obu, gdy osobnej nie ma.
// `HTTP_PROXY` wielkimi literami świadomie pomijamy: pod CGI ustawia ją nagłówek `Proxy:` z cudzego
// żądania (httpoxy), więc curl też jej nie czyta.
Proxy Proxy::from_environment() {
    std::string plain = environment("http_proxy", nullptr);
    std::string secure = environment("https_proxy", "HTTPS_PROXY");
    const std::string all = environment("all_proxy", "ALL_PROXY");

    if (plain.empty()) plain = all;
    if (secure.empty()) secure = all;

    return from_strings(plain, secure, environment("no_proxy", "NO_PROXY"));
}

// Pośrednik dla adresu albo brak, gdy adres stoi w wyjątkach: `*` to wszystkie, nazwa to ona sama
// i jej poddomeny, `.nazwa` — tak samo.
const Gateway* Proxy::chosen(const Url& target) const {
    for (const std::string& one : bypass) {
        if (one == "*") return nullptr;

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

const std::string* Response::header(std::string_view name) const {
    for (const Header& one : headers)
        if (alike(one.name, name)) return &one.value;

    return nullptr;
}

// --- HttpTrouble ---------------------------------------------------------------------------------

HttpTrouble::HttpTrouble(Why why, const std::string& said, Address where)
    : Trouble(said, 0, std::move(where)), why_(why) {}

// --- HttpClient ----------------------------------------------------------------------------------

// Stan klienta: szew TLS, granice i zapas bezczynnych połączeń — **jedno na serwer**. Drugie
// połączenie oddane do tego samego serwera zamyka poprzednie: przy usłudze w rodzaju TMDB żądania
// idą po kolei z kolejki, a zapas na każde równoległe żądanie trzymałby gniazda, których nikt nie
// użyje.
struct HttpClient::Held {
    Seam<TlsProvider> tls;
    Limits limits;
    Proxy proxy;

    std::mutex guard;
    std::map<Place, Wire> idle;

    // Miejsce po wziętym połączeniu zostaje puste, nie znika: serwerów, z którymi się rozmawia, jest
    // kilka, a nie tysiące.
    [[nodiscard]] Wire taken(const Place& where) {
        const std::lock_guard<std::mutex> held{guard};

        const auto one = idle.find(where);

        return one == idle.end() ? nullptr : std::exchange(one->second, nullptr);
    }

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

// Połączenie z hostem i portem: nazwa rozwiązana, adresy w wyścigu (`raced`) — pierwszy, który się
// połączy, wygrywa. `resolve` nie oddaje pustego wykazu: nazwa bez adresów odrzuca.
Promise<Stream> reached(const std::string& host, std::uint16_t port, Patience patience) {
    return resolve(host, port).then([patience](std::vector<Address> found) { return raced(std::move(found), patience); });
}

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

// Tunel przez pośrednika: `CONNECT host:port`, odpowiedź 2xx i od tej chwili bajty idą wprost do
// serwera. Odmowa — 407 bez podpisu, 403 dla zakazanego portu — jest kłopotem pośrednika, nie
// odpowiedzią serwera, więc nie wraca jako `Response`. Pośrednik nie może po 2xx dosłać niczego
// przed powitaniem TLS; bajty w zapasie to złamany protokół.
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

// Jedna wymiana na danym połączeniu: żądanie, głowa (z pominięciem odpowiedzi 1xx), treść. Na koniec
// połączenie wraca do zapasu, jeśli można go użyć jeszcze raz, a inaczej się zamyka.
Promise<Response> exchanged(const Kept& held, Request request, Wire wire) {
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
        .then([held, wire, patience, where, url = request.url](const std::string& text) -> Promise<Response> {
            const Head head = parsed(text);

            if (head.status >= 100 && head.status < 200) throw malformed("Too many interim responses.");

            const Frame frame = framed(head, held->limits.body);
            const auto body = std::make_shared<Bytes>();

            Promise<void> read = frame.how == Framing::sized   ? exact(wire, frame.size, body, patience)
                                 : frame.how == Framing::pieces ? chunked(wire, body, held->limits.body, patience)
                                 : frame.how == Framing::closing ? drained(wire, body, held->limits.body, patience)
                                                                 : ready();

            return read.then([held, wire, where, url, head, frame, body] {
                if (reusable(head, frame.how, wire)) {
                    wire->heard = false;

                    held->returned(where, wire);
                } else {
                    wire->close();
                }

                return Response{head.status, head.headers, std::move(*body), url};
            });
        });
}

// Wymiana z połączeniem z zapasu albo nowym. Połączenie z zapasu, które padło przed pierwszym bajtem
// odpowiedzi, serwer zamknął w bezczynności — żądanie idzie wtedy jeszcze raz, na nowym.
Promise<Response> attempted(const Kept& held, const Request& request) {
    if (Wire idle = request.method == "GET" ? held->taken(place_of(request.url)) : Wire{}) {
        return caught(exchanged(held, request, idle)).then([held, request, idle](Outcome<Response> got) -> Promise<Response> {
            if (got.value) return given(std::move(*got.value));

            idle->close();

            if (idle->heard) return refused<Response>(got.why);

            return dialed(held, request.url).then([held, request](Wire wire) { return exchanged(held, request, wire); });
        });
    }

    return dialed(held, request.url).then([held, request](Wire wire) { return exchanged(held, request, wire); });
}

bool redirecting(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

Promise<Response> fetched(const Kept& held, Request request, int hops) {
    checked(request);

    if (request.url.secure && !held->tls.bound())
        throw HttpTrouble{HttpTrouble::Why::insecure, "HTTPS needs a TLS provider."};

    return attempted(held, request).then([held, request, hops](Response got) -> Promise<Response> {
        const std::string* location = got.header("location");

        if (!redirecting(got.status) || location == nullptr) return given(std::move(got));

        if (hops >= held->limits.redirects) throw HttpTrouble{HttpTrouble::Why::redirected, "Too many redirects."};

        const std::optional<Url> next = joined(request.url, *location);

        if (!next) throw malformed("The redirect location is not an address.");

        if (request.url.secure && !next->secure)
            throw HttpTrouble{HttpTrouble::Why::downgraded, "A redirect leaves HTTPS for plain HTTP."};

        // Poświadczenia nie idą za przekierowaniem do innego serwera. 307 i 308 powtarzają żądanie
        // razem z treścią, reszta przekierowań to już GET bez treści.
        Request again{*next, {}};

        if (got.status == 307 || got.status == 308) {
            again.method = request.method;
            again.body = request.body;
        }

        for (const Header& one : request.headers)
            if (next->host == request.url.host || (!alike(one.name, "authorization") && !alike(one.name, "cookie")))
                again.headers.push_back(one);

        return fetched(held, std::move(again), hops + 1);
    });
}

}

HttpClient::HttpClient(Seam<TlsProvider> tls, Limits limits, Proxy proxy) : held_(std::make_shared<Held>()) {
    held_->tls = std::move(tls);
    held_->limits = limits;
    held_->proxy = std::move(proxy);
}

namespace {

// Jak każda obietnica sieci — rusza od razu, a nie przy pierwszym czekaniu.
Promise<Response> sent(const Kept& held, Request request) {
    if (!held) return refused<Response>(std::make_exception_ptr(Trouble{"The HTTP client is empty."}));

    Promise<Response> made = given(0).then([held, request = std::move(request)](int) { return fetched(held, request, 0); });

    made.start();

    return made;
}

}

Promise<Response> HttpClient::get(Request request) const {
    request.method = "GET";
    request.body.clear();

    return sent(held_, std::move(request));
}

Promise<Response> HttpClient::post(Request request, Bytes body) const {
    request.method = "POST";
    request.body = std::move(body);

    return sent(held_, std::move(request));
}

}

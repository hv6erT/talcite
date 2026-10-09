#include "talcite/capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talcite {

namespace {

constexpr std::chrono::milliseconds patience{250};

// Przez SSH odpowiedź wraca po pełnym obiegu sieci, a ten bywa dłuższy niż ćwierć sekundy —
// wtedy odpowiedzi przychodziły po sondzie. Czekamy dłużej; wartownik i tak kończy czekanie, gdy
// tylko terminal odpowie, więc szybki terminal nie traci nic.
constexpr std::chrono::milliseconds afar{1000};

// Zmienna środowiska **z konsoli**, nie z procesu. Pustka i brak znaczą tu to samo, bo tak je
// dotąd czytała cała podłoga.
std::string named_of(const Console& from, std::string_view what) {
    return from.variable(what).value_or(std::string{});
}

bool holds(std::string_view where, std::string_view what) {
    return where.find(what) != std::string_view::npos;
}

// Tryb surowy na czas badania, zdjęty także wtedy, gdy rozbiór odpowiedzi rzuci. Badanie odpala
// się samo, więc nie może zakładać, że ktoś już ten tryb ustawił — a konsola liczy włączenia,
// więc nie zdejmie go też komuś, kto ustawił go wcześniej.
class Rawly {
public:
    explicit Rawly(Console& given) : console_(given) { console_.raw(true); }

    ~Rawly() { console_.raw(false); }

    Rawly(const Rawly&) = delete;
    Rawly& operator=(const Rawly&) = delete;
    Rawly(Rawly&&) = delete;
    Rawly& operator=(Rawly&&) = delete;

private:
    Console& console_;
};

// Sekwencja pytań. Na końcu Primary DA, bo odpowiada na nie każdy terminal od VT100 wzwyż —
// kiedy przyjdzie jego odpowiedź, wiemy, że wszystko wcześniejsze jest już albo odpowiedziane,
// albo zignorowane, i nie ma na co dłużej czekać.
std::string asking() {
    std::string made;

    made += "\x1b[>c";
    made += "\x1b[?1006$p";
    made += "\x1b[?1000$p";
    made += "\x1b[?2004$p";
    made += "\x1b[?1004$p";
    made += "\x1b[?2026$p";
    made += "\x1b[?1049$p";

    // Rozmiar kratki w pikselach. Bez niego obrazek musiałby zgadywać proporcję, a kratka bywa
    // od 1:1,8 do 1:2,4 — i to widać na każdym kole.
    made += "\x1b[16t";

    // Zapytanie protokołu kitty: **prześlij jeden piksel i powiedz, czy się udało**. Terminal,
    // który tego nie zna, ma połknąć całą sekwencję APC w milczeniu; ten, który nie połknie,
    // wypluje śmieci — a te i tak wracają do wołającego jako naciśnięte klawisze.
    made += "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";

    made += "\x1bP+q5463;524742\x1b\\";
    made += "\x1b]11;?\x1b\\";

    // Pomiary szerokości. Zaczynamy od kolumny pierwszej, więc odpowiedź o położeniu kursora
    // jest wprost szerokością, i nie musimy wiedzieć, w którym wierszu jesteśmy.
    made += "\r\xe4\xbd\xa0\x1b[6n\r\x1b[K";
    made += "\r\xe2\x9d\xa4\xef\xb8\x8f\x1b[6n\r\x1b[K";
    made += "\r\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb\x1b[6n\r\x1b[K";

    // Sekstant (U+1FB00) i oktant (U+1CD00) — znaki, którymi rysujemy obrazek w kratkach.
    // **Ten pomiar odpowiada wyłącznie na pytanie, czy znak zajmuje jedną kratkę.** Czy font go
    // w ogóle ma, nie da się zmierzyć: brakujący glif rysuje się jako pusty prostokąt i zajmuje
    // dokładnie tyle samo miejsca. Rozważaliśmy `DECRQCRA`, ale ono liczy sumę **znaków**, nie
    // pikseli, więc na pustym prostokącie daje to samo co na prawdziwym glifie.
    made += "\r\xf0\x9f\xac\x80\x1b[6n\r\x1b[K";
    made += "\r\xf0\x9c\xb4\x80\x1b[6n\r\x1b[K";

    made += "\x1b[c";

    return made;
}

}

struct Capabilities::Held {
    explicit Held(Seam<Console> with) : console(std::move(with)) {}

    // Szew rozmowy z terminalem. Próby podstawiają tu atrapę, więc badanie i podłoga sprawdzają się
    // bez terminala i bez przestawiania środowiska procesu.
    Seam<Console> console;

    Colors colors = Colors::mono;

    // `NO_COLOR` to **pomiar**, a nie życzenie aplikacji: mówi o środowisku, w którym program
    // stoi. Dlatego nie blokuje sondy — sonda podnosi `colors` swoim trybem — tylko przykrywa
    // wynik przy odczycie. Kto chce koloru mimo zmiennej, mówi to przez `Setup`.
    bool colorless = false;

    Mouse mouse = Mouse::none;
    Screen screen = Screen::none;
    Shade shade = Shade::neutral;

    bool bracketed_paste = false;
    bool focus_events = false;
    bool synchronized = false;
    bool unicode = false;
    bool titles = false;

    int wide = 1;
    int varied = 1;
    int joined = 1;

    Graphics graphics = Graphics::none;

    // W tmuxie albo w screenie — patrz `ground`.
    bool muxed = false;

    // Zero znaczy „nie wiadomo", a nie „zero pikseli". Terminale mają prawo nie odpowiedzieć.
    ui::Size cell;

    // Zmierzone szerokości znaków, którymi chcemy rysować. Zero znaczy brak odpowiedzi.
    int sextant = 0;
    int octant = 0;

    std::string named;
    int version = 0;

    std::string rest;
    std::string tail;

    std::vector<int> measured;

    bool asked = false;
    bool done = false;
};

Capabilities::Capabilities(Seam<Console> with) : held_(std::make_unique<Held>(std::move(with))) {}

Capabilities::~Capabilities() = default;

// Podłoga ze środowiska. To jest stan, w którym zostajemy, gdy terminal nie odpowie na nic —
// i dlatego jest zachowawcza. Odpowiedzi, które przyjdą, tylko ją podnoszą.
void Capabilities::ground() const {
    Held& held = *held_;

    const Console& from = held.console.provider();

    const std::string term = named_of(from, "TERM");
    const std::string rich = named_of(from, "COLORTERM");
    const std::string lang = named_of(from, "LANG") + named_of(from, "LC_ALL") + named_of(from, "LC_CTYPE");

    // Pusty TERM to na Windows stan **normalny** — system go nie ustawia. Traktowanie pustki
    // jak terminala bez możliwości było przeniesieniem nawyku z POSIX i kosztowało cały kolor
    // w Windows Terminalu.
#ifdef _WIN32
    const bool console = term != "dumb";
#else
    const bool console = !term.empty() && term != "dumb";
#endif

    held.colorless = !named_of(from, "NO_COLOR").empty();

    // **W tmuxie albo w screenie zmienne zewnętrznego terminala kłamią.** Sesja założona w kitty
    // albo w Windows Terminalu niesie `KITTY_WINDOW_ID`, `WT_SESSION` i jego `TERM_PROGRAM` do
    // każdego okna — także wtedy, gdy klient siedzi już w zupełnie innym terminalu — a sam
    // multiplekser obrazów kitty nie przepuszcza. O tym, co umie, mówi tu tylko `TERM` i sonda.
    const bool muxed = !named_of(from, "TMUX").empty() || !named_of(from, "STY").empty() ||
                       term.rfind("tmux", 0) == 0 || term.rfind("screen", 0) == 0;

    held.muxed = muxed;

    if (rich == "truecolor" || rich == "24bit" || (!muxed && !named_of(from, "WT_SESSION").empty())) {
        held.colors = Colors::rgb;
    } else if (holds(term, "256")) {
        held.colors = Colors::palette;
    } else if (console) {
        held.colors = from.vt() ? Colors::rgb : Colors::ansi;
    }

#ifdef _WIN32
    held.unicode = true;
#else
    held.unicode = holds(lang, "UTF-8") || holds(lang, "utf8") || holds(lang, "UTF8");
#endif

    // Kitty i ghostty mówią o sobie w `TERM`, a kitty dokłada jeszcze własną zmienną. Sonda
    // i tak to potwierdzi, ale zgadnięcie ze środowiska działa również tam, gdzie sondy nie ma —
    // w potoku, w próbie, przy przekierowanym wyjściu.
    if (holds(term, "kitty") || holds(term, "ghostty") ||
        (!muxed && !named_of(from, "KITTY_WINDOW_ID").empty()))
        held.graphics = Graphics::kitty;

    if (console) held.mouse = Mouse::basic;

    held.named = muxed ? std::string{} : named_of(from, "TERM_PROGRAM");
    if (held.named.empty()) held.named = term;

    if (console) {
        held.screen = Screen::older;
        held.titles = true;
    }
}

void Capabilities::ready() const {
    if (held_->asked) return;

    held_->asked = true;

    ground();

    if (!held_->console.provider().lively()) return;

    probe();
}

void Capabilities::probe() const {
    Console& console = held_->console.provider();

    const Rawly rawly{console};

    console.write(asking());

    const auto began = std::chrono::steady_clock::now();

    const bool remote = !named_of(console, "SSH_CONNECTION").empty() || !named_of(console, "SSH_TTY").empty();
    const std::chrono::milliseconds waited = remote ? afar : patience;

    while (!held_->done) {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);

        if (spent >= waited) break;

        const std::string given = console.hear(waited - spent, nullptr);

        // Pustka znaczy, że czas minął — kręcenie się dalej byłoby jałowym obiegiem.
        if (given.empty()) break;

        fed(given);
    }

    // Bajty, których nie rozpoznaliśmy, to najpewniej klawisze naciśnięte w trakcie startu.
    // Muszą zostać oddane wołającemu, nie wyrzucone — inaczej pierwszy klawisz po uruchomieniu
    // czasem by przepadał, w sposób nie do powtórzenia.
    held_->rest += held_->tail;
    held_->tail.clear();

    if (held_->measured.size() > 0) held_->wide = held_->measured[0];
    if (held_->measured.size() > 1) held_->varied = held_->measured[1];
    if (held_->measured.size() > 2) held_->joined = held_->measured[2];
    if (held_->measured.size() > 3) held_->sextant = held_->measured[3];
    if (held_->measured.size() > 4) held_->octant = held_->measured[4];
}

namespace {

// Stan trybu z DECRPM: zero znaczy „nie znam takiego trybu", cokolwiek innego — że terminal
// go zna. To jest właśnie ta różnica, której zgadywanie po nazwie terminala nigdy nie daje.
bool knowing(int state) { return state != 0; }

// Wtórne DA oddaje liczbę rodziny, nie nazwę. Tablica jest krótka i celowo zachowawcza —
// nieznany numer zostawia nazwę ze środowiska, bo zmyślona byłaby gorsza niż żadna.
// Kolor tła przychodzi szesnastkowo i w zmiennej liczbie cyfr: rgb:R/G/B, rgb:RR/GG/BB, aż
// do czterech cyfr na składową. Każdą sprowadzamy więc do ułamka jej własnym zakresem.
double parted(std::string_view said) {
    std::uint32_t made = 0;
    int digits = 0;

    for (const char sign : said) {
        int value = -1;

        if (sign >= '0' && sign <= '9') value = sign - '0';
        else if (sign >= 'a' && sign <= 'f') value = sign - 'a' + 10;
        else if (sign >= 'A' && sign <= 'F') value = sign - 'A' + 10;
        else break;

        made = made * 16 + static_cast<std::uint32_t>(value);
        ++digits;
    }

    if (digits == 0) return -1.0;

    const double most = static_cast<double>((1u << (4 * digits)) - 1);

    return static_cast<double>(made) / most;
}

// Jasność liczymy wagami widzenia, nie sumą — oko widzi zieleń kilkakrotnie mocniej niż błękit,
// a tło o składowych 0,0,255 jest ciemne mimo niemałej sumy.
Shade shading(std::string_view body) {
    const std::size_t at = body.find("rgb");
    if (at == std::string_view::npos) return Shade::neutral;

    const std::size_t from = body.find(':', at);
    if (from == std::string_view::npos) return Shade::neutral;

    std::string_view rest = body.substr(from + 1);

    double among[3] = {-1.0, -1.0, -1.0};

    for (double& one : among) {
        one = parted(rest);
        if (one < 0.0) return Shade::neutral;

        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) break;

        rest = rest.substr(slash + 1);
    }

    if (among[1] < 0.0 || among[2] < 0.0) return Shade::neutral;

    const double light = 0.2126 * among[0] + 0.7152 * among[1] + 0.0722 * among[2];

    return light > 0.5 ? Shade::light : Shade::dark;
}

std::string calling(int family) {
    switch (family) {
        case 0: return "xterm";
        case 1: return "vte";
        case 41: return "vt420";
        case 61:
        case 64:
        case 65: return "vt520";
        case 77: return "mintty";
        case 83: return "screen";
        case 84: return "tmux";
        case 85: return "rxvt";
        default: return {};
    }
}

std::vector<int> numbers(std::string_view body) {
    std::vector<int> made;

    int one = 0;
    bool any = false;

    for (const char sign : body) {
        if (sign >= '0' && sign <= '9') {
            one = one * 10 + (sign - '0');
            any = true;

            continue;
        }

        if (any) made.push_back(one);

        one = 0;
        any = false;
    }

    if (any) made.push_back(one);

    return made;
}

}

// Rozbiór odpowiedzi. Skanujemy od początku: co rozpoznane — zabieramy, co niedokończone —
// zostawiamy na później, co obce — odkładamy jako cudze bajty.
void Capabilities::fed(std::string_view bytes) const {
    Held& held = *held_;

    held.tail += bytes;

    std::size_t at = 0;

    while (at < held.tail.size()) {
        if (held.tail[at] != '\x1b') {
            held.rest += held.tail[at];
            ++at;

            continue;
        }

        if (at + 1 >= held.tail.size()) break;

        const char lead = held.tail[at + 1];

        if (lead == '[') {
            std::size_t end = at + 2;

            while (end < held.tail.size() &&
                   !(held.tail[end] >= 0x40 && held.tail[end] <= 0x7E))
                ++end;

            if (end >= held.tail.size()) break;

            const std::string_view body{held.tail.data() + at + 2, end - at - 2};
            const char last = held.tail[end];

            bool known = true;

            if (last == 'y' && !body.empty() && body.front() == '?') {
                const std::vector<int> among = numbers(body);

                if (among.size() >= 2 && knowing(among[1])) switch (among[0]) {
                        case 1006: held.mouse = Mouse::sgr; break;
                        case 1000:
                            if (held.mouse == Mouse::none) held.mouse = Mouse::basic;
                            break;
                        case 2004: held.bracketed_paste = true; break;
                        case 1004: held.focus_events = true; break;
                        case 2026: held.synchronized = true; break;
                        case 1049: held.screen = Screen::apart; break;
                        default: break;
                    }
            } else if (last == 'R') {
                const std::vector<int> among = numbers(body);

                if (among.size() >= 2) held.measured.push_back(std::max(1, among[1] - 1));
            } else if (last == 'c' && !body.empty() && body.front() == '>') {
                const std::vector<int> among = numbers(body);

                if (among.size() >= 2) held.version = among[1];
                if (!among.empty()) {
                    const std::string mine = calling(among[0]);

                    if (!mine.empty()) held.named = mine;
                }
            } else if (last == 't') {
                // Odpowiedź na `16t` brzmi `6;wysokość;szerokość` — inne raporty okna zaczynają
                // się inną liczbą i nie są nasze.
                const std::vector<int> among = numbers(body);

                if (among.size() >= 3 && among[0] == 6)
                    held.cell = ui::Size{among[2], among[1]};
            } else if (last == 'c') {
                // **Sixel poznajemy z odpowiedzi, którą i tak dostajemy.** Pierwotne DA wysyłamy
                // jako terminator sondy, a czwórka na jego liście znaczy „umiem sixel". Dotąd
                // parametry szły do kosza.
                //
                // **Poza multiplekserem.** tmux od wersji 3.4 przyjmuje sixel i odpowiada czwórką, ale
                // obraz trzyma po swojemu: gubi go przy przełączaniu okien i przewijaniu, a przy
                // podziale ekranu potrafi narysować go w sąsiednim panelu. Piksele w tmuxie to więc
                // jawne życzenie aplikacji (`Setup::graphics`), nie wniosek z sondy.
                const std::vector<int> among = numbers(body);

                if (!held.muxed && std::find(among.begin(), among.end(), 4) != among.end() &&
                    held.graphics < Graphics::sixel)
                    held.graphics = Graphics::sixel;

                held.done = true;
            } else {
                known = false;
            }

            // Nierozpoznana sekwencja jest kompletna, ale to nie znaczy, że jest nasza —
            // strzałka naciśnięta w trakcie startu wygląda tak samo. Oddajemy ją w całości.
            if (!known) held.rest.append(held.tail, at, end + 1 - at);

            at = end + 1;

            continue;
        }

        // Protokół kitty odpowiada w APC. Tylko po to tu jest — żadna inna sekwencja, której
        // oczekujemy, nie zaczyna się od `ESC _`.
        if (lead == '_') {
            std::size_t end = at + 2;

            while (end < held.tail.size()) {
                if (held.tail[end] == '\x1b' && end + 1 < held.tail.size() &&
                    held.tail[end + 1] == '\\')
                    break;

                ++end;
            }

            if (end >= held.tail.size()) break;

            const std::string_view body{held.tail.data() + at + 2, end - at - 2};

            if (holds(body, "OK")) held.graphics = Graphics::kitty;

            at = end + 2;

            continue;
        }

        if (lead == 'P' || lead == ']') {
            std::size_t end = at + 2;

            while (end < held.tail.size()) {
                if (held.tail[end] == '\x07') break;
                if (held.tail[end] == '\x1b' && end + 1 < held.tail.size() &&
                    held.tail[end + 1] == '\\')
                    break;

                ++end;
            }

            if (end >= held.tail.size()) break;

            const std::string_view body{held.tail.data() + at + 2, end - at - 2};

            bool known = false;

            if (lead == 'P' && body.rfind("1+r", 0) == 0) {
                known = true;

                if (holds(body, "5463") || holds(body, "524742")) held.colors = Colors::rgb;
            }

            if (lead == 'P' && body.rfind("0+r", 0) == 0) known = true;

            if (lead == ']' && body.rfind("11;", 0) == 0) {
                known = true;

                const Shade told = shading(body.substr(3));

                if (told != Shade::neutral) held.shade = told;
            }

            const std::size_t after = end + (held.tail[end] == '\x07' ? 1 : 2);

            if (!known) held.rest.append(held.tail, at, after - at);

            at = after;

            continue;
        }

        held.rest += held.tail[at];
        ++at;
    }

    held.tail.erase(0, at);
}

Colors Capabilities::colors() const {
    ready();

    return held_->colorless ? Colors::mono : held_->colors;
}

Mouse Capabilities::mouse() const {
    ready();

    return held_->mouse;
}

Screen Capabilities::screen() const {
    ready();

    return held_->screen;
}

Graphics Capabilities::graphics() const {
    ready();

    return held_->graphics;
}

// **Sufit, nie wybór.** Metoda mówi, jak gęstej siatki terminal nie rozsypie — nie jaką siatką
// mamy rysować. Wyboru dokonuje `Term`, biorąc mniejszą z dwóch: życzenia i tego sufitu.
//
// Sonda odpowiada wyłącznie na pytanie „czy ten znak zajmuje jedną kratkę". Nie odpowiada na
// pytanie „czy font go ma", bo brakujący glif rysuje się jako pusty prostokąt i zajmuje dokładnie
// tyle samo miejsca. Dlatego sufit **niczego nie obiecuje**: podniesiony bez życzenia dałby
// połowie użytkowników obrazek złożony z pustych ramek, a domyślnie i tak zostajemy przy znakach
// blokowych, które są w Unicode od 1.1 i ma je każdy font terminalowy.
//
// Brak pomiaru — zero — nie obniża. Milczący terminal niczego nie obalił, a to co innego niż
// terminal mówiący „ten znak zajmuje dwie kratki".
ui::Blocks Capabilities::blocks() const {
    ready();

    if (held_->sextant != 0 && held_->sextant != 1) return ui::Blocks::basic;

    if (held_->octant != 0 && held_->octant != 1) return ui::Blocks::sextants;

    return ui::Blocks::octants;
}

ui::Size Capabilities::cell() const {
    ready();

    return held_->cell;
}

Shade Capabilities::shade() const {
    ready();

    return held_->shade;
}

bool Capabilities::bracketed_paste() const {
    ready();

    return held_->bracketed_paste;
}

bool Capabilities::focus_events() const {
    ready();

    return held_->focus_events;
}

bool Capabilities::synchronized() const {
    ready();

    return held_->synchronized;
}

bool Capabilities::unicode() const {
    ready();

    return held_->unicode;
}

bool Capabilities::titles() const {
    ready();

    return held_->titles;
}

// Trzy **zmierzone szerokości** znaków próbnych: chińskiego, emoji ze znacznikiem wariantu
// i emoji złożonego spójnikiem. Nazywały się `broad`, `marked` i `joined` — z czego `marked`
// znaczyło w tej bibliotece „zaznaczony", a żadna z trzech nie mówiła, że chodzi o szerokość.
int Capabilities::wide() const {
    ready();

    return held_->wide;
}

int Capabilities::varied() const {
    ready();

    return held_->varied;
}

int Capabilities::joined() const {
    ready();

    return held_->joined;
}

const std::string& Capabilities::named() const {
    ready();

    return held_->named;
}

int Capabilities::version() const {
    ready();

    return held_->version;
}

// Bajty, które przyszły przy starcie i nie były odpowiedzią — czyli **resztki**, a nie zapas.
const std::string& Capabilities::rest() const {
    ready();

    return held_->rest;
}

}

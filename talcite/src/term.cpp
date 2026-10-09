#include "talcite/term.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "talcite/base64.hpp"
#include "talcite/encoding.hpp"
#include "talcite/decoder.hpp"
#include "talcite/pixels.hpp"

namespace talcite {

namespace {

// Samotny Esc rozstrzyga się czasem, bo terminal nie mówi, czy coś jeszcze przyśle. Fork dawał
// dwadzieścia milisekund i przez ssh strzałka rozpadała się na Esc i literę.
constexpr std::chrono::milliseconds hesitating{50};

// Jak długo, oddawszy terminal, czekamy na dzwonek pętli w jednym obrocie.
constexpr std::chrono::milliseconds glancing{100};

}

struct Term::Held {
    std::unique_ptr<Loop> mine;
    Loop* loop = nullptr;

    explicit Held(Seam<Console> with) : console(with), compat(std::move(with)) {}

    // **Wszystko, co sięga do terminala, idzie przez ten szew**: klatki, tytuł, mysz i ekran,
    // a także wejście, tryb surowy, rozmiar i czas dwukliku. `Term` nie ma ani linijki kodu
    // systemowego, więc próby z atrapą sprawdzają go całego.
    Seam<Console> console;

    // Stała, bo po usunięciu setterów nie ma w niej ani jednej metody niestałej: mierzy terminal
    // i o niczym nie decyduje. Pyta przez tę samą konsolę, na którą rysujemy.
    const Capabilities compat;
    Decoder decoder;

    // Życzenie aplikacji. W `Capabilities` nic się nie wpisuje — ona mierzy i tyle — więc
    // wartość skuteczna powstaje przy każdym odczycie jako `życzenie.value_or(pomiar)`.
    Setup wish;

    // Świeża konfiguracja unieważnia obraz ekranu tak samo jak zmiana rozmiaru okna.
    bool shifted = false;

    ui::Root* onto = nullptr;

    std::string spare;
    bool running = true;
    bool entered = false;
    bool fresh = true;

    std::chrono::milliseconds twice{0};

    View standing;

    // Obrazy kitty (G2): co stoi na ekranie po ostatniej klatce i które obrazy terminal ma u siebie.
    // Terminal trzyma obraz tak długo, jak ktoś po naszej stronie trzyma jego piksele.
    std::vector<Placement> placed;
    std::vector<std::pair<std::uint32_t, std::weak_ptr<const Pixels>>> sent;

    Style now;
    ui::Spot at{-1, -1};
    bool showing = false;

    int wide = 80;
    int high = 25;

    // Ile razy zeszliśmy terminalowi z drogi. Licznik, a nie flaga, bo zagnieżdżone zejście jest
    // dozwolone i ma znaczyć jedno: pierwszy wychodzi, ostatni wraca. Zdejmowanie stanu dwa razy
    // wysłałoby drugi komplet sekwencji, a przy myszy zgubiłoby stan pośredni.
    int away = 0;
};

namespace {

int apart(int one, int two) { return (one - two) * (one - two); }

// Najbliższy punkt palety 256: sześcian 6×6×6 albo szarości. Bierzemy bliższy z dwóch, bo
// szarość z sześcianu bywa dalej niż osobna szara drabinka.
int indexed(Color one) {
    // Stopnie sześcianu z palety (`Color::indexed`): czerwony numerów 16, 52, 88, 124, 160, 196.
    static const std::array<int, 6> steps = [] {
        std::array<int, 6> made{};

        for (int which = 0; which < 6; ++which) made[static_cast<std::size_t>(which)] = Color::indexed(16 + 36 * which)->red;

        return made;
    }();

    const auto closest = [](int given) {
        int best = 0;

        for (int which = 1; which < 6; ++which)
            if (apart(given, steps[which]) < apart(given, steps[best])) best = which;

        return best;
    };

    const int red = closest(one.red);
    const int green = closest(one.green);
    const int blue = closest(one.blue);

    const int cube = apart(one.red, steps[red]) + apart(one.green, steps[green]) +
                     apart(one.blue, steps[blue]);

    int grey = (one.red + one.green + one.blue) / 3;
    int level = (grey - 8) / 10;

    level = level < 0 ? 0 : (level > 23 ? 23 : level);
    grey = 8 + 10 * level;

    const int plain =
        apart(one.red, grey) + apart(one.green, grey) + apart(one.blue, grey);

    if (plain < cube) return 232 + level;

    return 16 + 36 * red + 6 * green + blue;
}

// Najbliższy z szesnastu podstawowych palety (`Color::indexed`). Wartości jak w xtermie — inne
// terminale malują je nieco inaczej, ale wybór najbliższego i tak jest przybliżeniem.
int shortened(Color one) {
    int best = 0;
    int least = -1;

    for (int which = 0; which < 16; ++which) {
        const Color basic = *Color::indexed(which);
        const int much = apart(one.red, basic.red) + apart(one.green, basic.green) + apart(one.blue, basic.blue);

        if (least < 0 || much < least) {
            least = much;
            best = which;
        }
    }

    return best;
}

void tinted(Color one, bool foreground, Colors how, std::string& into) {
    if (one.transparent || how == Colors::mono) return;

    if (how == Colors::rgb) {
        into += foreground ? ";38;2;" : ";48;2;";
        into += std::to_string(one.red) + ";" + std::to_string(one.green) + ";" +
                std::to_string(one.blue);

        return;
    }

    if (how == Colors::palette) {
        into += foreground ? ";38;5;" : ";48;5;";
        into += std::to_string(indexed(one));

        return;
    }

    const int which = shortened(one);

    if (which < 8) into += ";" + std::to_string((foreground ? 30 : 40) + which);
    else into += ";" + std::to_string((foreground ? 90 : 100) + which - 8);
}

// Styl wypisujemy zawsze od zera. Wyliczanie różnicy między dwoma stylami oszczędziłoby kilka
// bajtów, a kosztowało pamiętanie, którym atrybutem się co gasi — i tak najdroższe są skoki
// kursora, nie te kilka znaków.
void painted(const Style& what, Colors how, std::string& into) {
    into += "\x1b[0";

    if (what.bold) into += ";1";
    if (what.faint) into += ";2";
    if (what.italic) into += ";3";
    if (what.under) into += ";4";
    if (what.reverse) into += ";7";
    if (what.crossed) into += ";9";

    tinted(what.foreground, true, how, into);
    tinted(what.background, false, how, into);

    into += "m";
}

void placed(ui::Spot where, std::string& into) {
    into += "\x1b[" + std::to_string(where.y + 1) + ";" + std::to_string(where.x + 1) + "H";
}

// **Protokół graficzny kitty** (G2). Obraz idzie do terminala raz, pod numerem — numerem jest znak
// obrazu z kratek — a potem tylko się go stawia i zdejmuje. Piksele surowe (`f=32`, RGBA), w base64,
// porcjami po 4096 znaków; kolejne porcje niosą tylko `m` i `q`. `q=2` ucisza odpowiedzi, które
// inaczej przyszłyby jako klawisze. Postawienie podaje kolumny i wiersze (`c`, `r`), więc terminal
// wpasowuje obraz w prostokąt kratek, nawet gdy wymiar kratki znamy źle, a `C=1` zostawia kursor
// w miejscu — dlatego przy kitty ostatni wiersz ekranu może mieć obraz.
void kitty(const Picture& one, std::string& into) {
    const std::string coded = base64(one.pixels->stills()[one.still].bytes);
    const ui::Size size = one.pixels->size();

    constexpr std::size_t piece = 4096;

    for (std::size_t at = 0;; at += piece) {
        const bool more = at + piece < coded.size();

        into += "\x1b_G";

        if (at == 0)
            into += "a=t,f=32,s=" + std::to_string(size.wide) + ",v=" + std::to_string(size.high) +
                    ",i=" + std::to_string(one.mark) + ",";

        into += more ? "q=2,m=1;" : "q=2,m=0;";
        into.append(coded, at, piece);
        into += "\x1b\\";

        if (!more) return;
    }
}

void stood(const Placement& one, std::string& into) {
    placed(one.at, into);

    into += "\x1b_Ga=p,i=" + std::to_string(one.picture.mark) + ",c=" + std::to_string(one.picture.size.wide) +
            ",r=" + std::to_string(one.picture.size.high) + ",C=1,q=2\x1b\\";
}

// Zdjęcie z ekranu (`d=i`, dane zostają) i zwolnienie (`d=I`, terminal zapomina obraz).
void unstood(std::uint32_t mark, bool forget, std::string& into) {
    into += "\x1b_Ga=d,d=";
    into += forget ? 'I' : 'i';
    into += ",i=" + std::to_string(mark) + ",q=2\x1b\\";
}

bool same_place(const Placement& one, const Placement& two) {
    return one.picture.mark == two.picture.mark && one.at == two.at;
}

// Wszystko precz — przy wyjściu i przy oddaniu terminala, bo cudzy program dostaje ekran czysty,
// a nasze numery mogłyby wejść w jego.
using Sent = std::vector<std::pair<std::uint32_t, std::weak_ptr<const Pixels>>>;

void forgotten(Console& out, std::vector<Placement>& placed, Sent& sent) {
    if (placed.empty() && sent.empty()) return;

    out.write("\x1b_Ga=d,d=A,q=2\x1b\\");

    placed.clear();
    sent.clear();
}

// Skok kursora kosztuje kilka bajtów, znak jeden. Krótką przerwę między zmienionymi komórkami
// taniej jest **przepisać** niż przeskoczyć — pod warunkiem, że przepisane komórki wyjdą
// dokładnie takie, jakie już są: ten sam styl co obowiązujący, żaden znak podwójny i nic, co
// jest tylko jego ciągiem dalszym.
constexpr int nearby = 4;

bool bridged(const View& made, int y, int from, int upto, const Style& now, std::string& into) {
    if (upto - from > nearby) return false;

    for (int x = from; x < upto; ++x) {
        const Cell one = made.at({x, y});

        if (one.letter == 0 || width(one.letter) == 2) return false;

        if (!(one.style == now)) return false;
    }

    for (int x = from; x < upto; ++x) utf8::encode(made.at({x, y}).letter, into);

    return true;
}

// Trzy ustawienia, których terminal nie odgadnie z klatki: trzeba mu je **powiedzieć** sekwencją
// i cofnąć drugą. Dla nich `Capabilities` nie ma settera, więc wartość skuteczna powstaje tutaj.
struct Sending {
    Screen screen = Screen::none;
    Mouse mouse = Mouse::none;
    bool hover = false;
    bool bracketed_paste = false;
};

// Śledzenie ruchu **bez wciśniętego przycisku** nie ma czego mierzyć w terminalu: żadna odpowiedź
// nie mówi, czy `?1003` jest rozumiane. Rozstrzyga więc samo życzenie, a domyślnie jest wyłączone —
// bo włączone każe terminalowi zgłaszać każde drgnięcie kursora, także wtedy, gdy nikogo to nie
// obchodzi, a przez ssh jest to prawdziwy ruch na łączu.
Sending sending(const Capabilities& compat, const Setup& wish) {
    Sending made;

    made.screen = wish.screen.value_or(compat.screen());
    made.mouse = wish.mouse.value_or(compat.mouse());
    made.hover = wish.hover.value_or(false);
    made.bracketed_paste = wish.bracketed_paste.value_or(compat.bracketed_paste());

    return made;
}

// Jedyne pole, w którym życzenie nie wygrywa bez zastrzeżeń: `Capabilities::blocks()` oddaje
// sufit, czyli gęstość, której terminal jeszcze nie rozsypie. Glif szerszy niż kratka psuje `View`,
// trafianie myszą i wypisywanie różnicy, więc bierzemy mniejszą z dwóch.
ui::Blocks blocked(const Capabilities& compat, const Setup& wish) {
    const ui::Blocks want = wish.blocks.value_or(ui::Blocks::basic);
    const ui::Blocks most = compat.blocks();

    return want < most ? want : most;
}

bool synchronized(const Capabilities& compat, const Setup& wish) {
    return wish.synchronized.value_or(compat.synchronized());
}

bool titles(const Capabilities& compat, const Setup& wish) {
    return wish.titles.value_or(compat.titles());
}

// `?1003` dokłada się do śledzenia, które już stoi, i **tylko wtedy, gdy ono stoi**: bez zgłaszania
// myszy w ogóle nie miałby kto tego ruchu przysłać. Zdejmujemy je pierwsze, bo obejmuje sobą to,
// co `?1002` — zdjęte na końcu zostawiłoby terminal na chwilę przy szerszym śledzeniu.
void mousing(Console& out, Mouse given, bool hover) {
    if (given == Mouse::sgr) out.write("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    else if (given == Mouse::basic) out.write("\x1b[?1000h");

    if (given != Mouse::none && hover) out.write("\x1b[?1003h");
}

void unmoused(Console& out, Mouse given, bool hover) {
    if (given != Mouse::none && hover) out.write("\x1b[?1003l");

    if (given == Mouse::sgr) out.write("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    else if (given == Mouse::basic) out.write("\x1b[?1000l");
}

void screened(Console& out, Screen given) {
    if (given == Screen::apart) out.write("\x1b[?1049h");
    else if (given == Screen::older) out.write("\x1b[?47h");
}

void unscreened(Console& out, Screen given) {
    if (given == Screen::apart) out.write("\x1b[?1049l");
    else if (given == Screen::older) out.write("\x1b[?47l");
}

// Ubranie terminala i jego zdjęcie. Dwie połówki jednej rzeczy, więc stoją obok siebie i mają
// **jedno** miejsce: robi to i konstruktor, i destruktor, i zejście z drogi na czas cudzego
// programu. Rozpisane w trzech miejscach rozjechałyby się przy pierwszej zmianie.
void dressed(Console& out, const Sending& now) {
    screened(out, now.screen);

    out.write("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");

    mousing(out, now.mouse, now.hover);

    if (now.bracketed_paste) out.write("\x1b[?2004h");
}

void undressed(Console& out, const Sending& now) {
    if (now.bracketed_paste) out.write("\x1b[?2004l");

    unmoused(out, now.mouse, now.hover);

    out.write("\x1b[?7h\x1b[?25h");

    unscreened(out, now.screen);
}

// Wysyłamy **samą różnicę**, nie całość: bezwarunkowe powtórzenie `\x1b[?1049h` na terminalu, który
// już jest na ekranie zapasowym, kosztowałoby mrugnięcie, a przy myszy zgubiłoby stan pośredni.
void shifting(Console& out, const Sending& was, const Sending& now) {
    if (was.bracketed_paste != now.bracketed_paste) out.write(now.bracketed_paste ? "\x1b[?2004h" : "\x1b[?2004l");

    if (was.mouse != now.mouse || was.hover != now.hover) {
        unmoused(out, was.mouse, was.hover);
        mousing(out, now.mouse, now.hover);
    }

    if (was.screen != now.screen) {
        unscreened(out, was.screen);
        screened(out, now.screen);

        out.write("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");
    }
}

// Rozmiar z konsoli, prawda, gdy się zmienił. Konsola, która rozmiaru nie zna — potok, próba —
// zostawia poprzedni, a na starcie 80×25.
bool measured(const Console& from, int& wide, int& high) {
    const std::optional<ui::Size> now = from.size();

    if (!now.has_value() || now->wide <= 0 || now->high <= 0) return false;

    if (now->wide == wide && now->high == high) return false;

    wide = now->wide;
    high = now->high;

    return true;
}

}

Term::Term(Setup given, Seam<Console> with) : held_(std::make_unique<Held>(std::move(with))) {
    Held& held = *held_;

    held.wish = std::move(given);

    // Pętla należy do wątku i może być tylko jedna. Jeśli już stoi — przyjmujemy ją; jeśli nie —
    // zakładamy własną, bo bez niej Term nie ma na czym czekać.
    if (Loop* standing = Loop::current()) {
        held.loop = standing;
    } else {
        held.mine = std::make_unique<Loop>();
        held.loop = held.mine.get();
    }

    Console& console = held.console.provider();

    console.raw(true);

    held.twice = console.double_click();

    // Badanie musi się odbyć na przygotowanym terminalu, ale **przed** wejściem na ekran
    // zapasowy — inaczej znaki próbne mrugnęłyby na czystym ekranie aplikacji.
    (void)held.compat.colors();

    held.spare = held.compat.rest();

    dressed(console, sending(held.compat, held.wish));

    held.entered = true;

    // Rozmiar dopiero po wejściu na ekran zapasowy: na Windows to inny bufor, a mierzymy ten,
    // na którym rysujemy.
    (void)measured(console, held.wide, held.high);
}

// Sprzątamy w odwrotnej kolejności i bez względu na to, jak wyszliśmy. Terminal zostawiony
// w trybie surowym bez kursora jest nie do użycia, a użytkownik nie ma jak tego cofnąć.
Term::~Term() {
    Held& held = *held_;

    // Cofamy stan **bieżący**, nie startowy: po `reconfigure` mysz albo ekran zapasowy mogą stać
    // inaczej niż w konstruktorze, a wyłączyć trzeba to, co naprawdę jest włączone.
    //
    // Gdy terminal jest właśnie oddany komuś innemu, zdejmować nie ma czego — `Pause` zdjął i ubranie,
    // i tryb surowy już przy wejściu.
    if (held.away > 0) return;

    forgotten(held.console.provider(), held.placed, held.sent);

    if (held.entered) undressed(held.console.provider(), sending(held.compat, held.wish));

    held.console.provider().raw(false);
}

// **Zejście z drogi.** Cofamy dokładnie to, co założył konstruktor, i w odwrotnej kolejności:
// najpierw sekwencje, potem tryb konsoli. Program, który teraz ruszy, zastaje terminal taki, jaki
// zastaliśmy go my — z echem, z Enterem, z Ctrl-C, na tym buforze, na którym startowaliśmy.
//
// Klasa, a nie para metod publicznych, bo para metod jest umową, której kompilator nie pilnuje:
// dałoby się wrócić bez wychodzenia albo wyjść i nie wrócić. Tu wyjście i powrót są jedną rzeczą,
// więc powrót następuje także wtedy, gdy po drodze poleci wyjątek. Klasa zagnieżdżona sięga do
// `Held` bez żadnego `friend`, a publiczna powierzchnia `Term` rośnie o jedno pojęcie, nie o dwa
// czasowniki.
Term::Pause::Pause(Term& given) : term_(given) {
    Held& held = *term_.held_;

    if (held.away++ > 0) return;

    forgotten(held.console.provider(), held.placed, held.sent);

    if (held.entered) undressed(held.console.provider(), sending(held.compat, held.wish));

    held.console.provider().raw(false);
}

Term::Pause::~Pause() {
    Held& held = *term_.held_;

    if (--held.away > 0) return;

    Console& console = held.console.provider();

    console.raw(true);

    if (held.entered) dressed(console, sending(held.compat, held.wish));

    // Po cudzym programie na ekranie nie zostało nic z naszej klatki, a przy `Screen::plain`
    // nawet ekran zapasowy jej nie schował. `shifted` znaczy dla `draw()` dokładnie to samo, co
    // zmiana rozmiaru: porównaj z pustą kartką, czyli odmaluj wszystko.
    held.shifted = true;
    held.fresh = true;

    // Rozmiar mógł się zmienić, gdy nas nie było — zmiany szły wtedy nie do nas.
    (void)measured(console, held.wide, held.high);
}

Loop& Term::loop() const { return *held_->loop; }

// **Zmiana idzie przez pętlę, nie od razu.** Wywołanie może przyjść z dowolnego wątku — z okna
// ustawień, z wczytanego pliku konfiguracji — a przestawienie terminala w połowie składania klatki
// dałoby pół obrazu starą siatką i pół nową. Pętla wykonuje zaległości w `turn()`, tuż przed
// rysowaniem, więc zmiana wchodzi zawsze na granicy klatki i zawsze na właściwym wątku.
//
// Zaległe wywołanie trzyma surowy wskaźnik na stan `Term`, więc zniszczenie go z jeszcze
// niewykonanym `reconfigure` w kolejce jest błędem wołającego, tak samo jak przy każdym innym
// `Loop::post` sięgającym do obiektu.
void Term::reconfigure(Setup given) {
    held_->loop->post([held = held_.get(), given = std::move(given)]() mutable {
        const Sending was = sending(held->compat, held->wish);

        held->wish = std::move(given);

        shifting(held->console.provider(), was, sending(held->compat, held->wish));

        if (held->onto != nullptr) {
            held->onto->blocks(blocked(held->compat, held->wish));
            held->onto->cell(held->wish.cell.value_or(held->compat.cell()));
            held->onto->colors(held->wish.colors.value_or(held->compat.colors()));
            held->onto->unicode(held->wish.unicode.value_or(held->compat.unicode()));
            held->onto->graphics(held->wish.graphics.value_or(held->compat.graphics()));
        }

        held->shifted = true;
    });
}

Colors Term::colors() const { return held_->wish.colors.value_or(held_->compat.colors()); }

Mouse Term::mouse() const { return held_->wish.mouse.value_or(held_->compat.mouse()); }

Shade Term::shade() const { return held_->wish.shade.value_or(held_->compat.shade()); }

Graphics Term::graphics() const {
    return held_->wish.graphics.value_or(held_->compat.graphics());
}

ui::Blocks Term::blocks() const { return blocked(held_->compat, held_->wish); }

ui::Size Term::cell() const { return held_->wish.cell.value_or(held_->compat.cell()); }

bool Term::unicode() const { return held_->wish.unicode.value_or(held_->compat.unicode()); }

// Fakty o kratce zostawiamy na korzeniu od razu, bo widżet obrazu czyta je stamtąd przy malowaniu,
// a pierwsza klatka bywa rysowana zanim cokolwiek innego się wydarzy.
// **Korzeń przestawiany w biegu** — tak przełącza się ekrany (panele, edytor, podgląd), a klawisz,
// który przełącza, przychodzi w środku doręczania. Reszta odczytanego kawałka idzie już do nowego,
// bo `fed` bierze bieżący korzeń przy każdym bajcie. Ognisko i czekanie na drugie kliknięcie każdy
// korzeń trzyma swoje, więc klik na starym ekranie i klik na nowym nie składają się w dwuklik,
// a powrót zastaje ognisko tam, gdzie zostało. Okno dwukliku korzeń dostaje stąd, z ustawienia
// systemu, razem z resztą faktów o terminalu.
void Term::show(ui::Root& given) {
    held_->onto = &given;

    given.double_click(held_->twice);
    given.blocks(blocks());
    given.cell(cell());
    given.colors(colors());
    given.unicode(unicode());
    given.graphics(graphics());
}

ui::Root* Term::shown() const { return held_->onto; }

ui::Size Term::size() const { return {held_->wide, held_->high}; }

void Term::title(std::string_view said) {
    if (!titles(held_->compat, held_->wish)) return;

    std::string made = "\x1b]0;";
    made += said;
    made += "\x1b\\";

    held_->console.provider().write(made);
}

// Klatka powstaje w całości w pamięci, na terminal idzie sama różnica i to jednym zapisem:
// widoczna połowa klatki jest gorsza niż klatka spóźniona o mikrosekundę.
void Term::draw() {
    Held& held = *held_;

    if (held.onto == nullptr)
        throw std::logic_error("talcite::Term: no root set");

    // Ekran należy teraz do cudzego programu. Rysowanie po nim byłoby pisaniem po czyimś obrazie,
    // a nasza pamięć klatki i tak jest już nieaktualna.
    if (held.away > 0) return;

    View made = held.onto->render({{0, 0}, {held.wide, held.high}});

    std::string into;

    // Zmiana rozmiaru unieważnia poprzednią klatkę — po wyczyszczeniu ekranu porównujemy
    // z pustą kartką, więc wszystko wyjdzie od nowa.
    //
    // Świeża konfiguracja robi dokładnie to samo i **tym jest** wymuszenie renderowania wszystkiego
    // od nowa. Drzewo i tak składa się w całości przy każdej klatce; jedyne, co pamiętało poprzedni
    // obraz, to `standing`, i te cztery linijki mu tę pamięć odbierają.
    if (held.shifted || !(held.standing.size() == made.size())) {
        // Obrazy kitty nie należą do tekstu, więc czyszczenie ekranu ich nie zdejmuje — robimy to sami.
        // Dane zostają: odmalowanie postawi je bez ponownego wysyłania.
        if (!held.placed.empty()) into += "\x1b_Ga=d,d=a,q=2\x1b\\";

        held.placed.clear();

        into += "\x1b[0m\x1b[2J";

        held.standing = View{made.size()};
        held.now = Style{};
        held.at = ui::Spot{-1, -1};

        held.shifted = false;
    }

    // **Obrazy w pikselach** (G1, sixel). Klatka już wie, które obrazy widać w całości (`bared`):
    // ich kratki są puste i niosą znak obrazu. Ostatni wiersz ekranu zostaje dla znaków, bo terminal
    // po obrazie przesuwa kursor pod niego, a pod ostatnim wierszem jest już tylko przewinięcie.
    // Obraz idzie na terminal, gdy którakolwiek jego kratka zmieniła się w tej klatce — nowy, przesunięty,
    // inna klatka animacji, odsłonięty, ekran wyczyszczony. Kratki idą wcześniej: przepisanie ich
    // zmazuje piksele pod spodem, a obraz kładzie się na czystym.
    std::vector<Placement> pictured;
    std::vector<Placement> standing;

    const ui::Size grid = cell();
    const Graphics kind = graphics();

    if (kind == Graphics::sixel && grid.wide > 0 && grid.high > 0)
        pictured = made.bared({{0, 0}, {made.size().wide, made.size().high - 1}});

    // **Kitty** (G2) liczy się inaczej: tekst obrazu nie zmazuje, więc nie decyduje o nim zmiana
    // kratek, tylko porównanie z tym, co stoi. Co znikło albo się przesunęło, zdejmujemy przed
    // kratkami; obraz, którego pikseli nikt już nie trzyma, terminal zapomina. Nowe stawiamy po
    // kratkach, wysyłając wcześniej te, których terminal jeszcze nie ma. Zapominanie idzie na końcu,
    // gdy pamięć poprzedniej klatki (`standing`, `placed`) już nie trzyma pikseli — inaczej obraz
    // zszedłby z terminala klatkę później.
    if (kind == Graphics::kitty) {
        standing = made.bared({{0, 0}, made.size()});

        std::erase_if(standing, [](const Placement& one) {
            return !one.picture.pixels || one.picture.still >= one.picture.pixels->stills().size();
        });
    }

    for (const Placement& one : held.placed)
        if (std::none_of(standing.begin(), standing.end(), [&one](const Placement& now) { return same_place(one, now); }))
            unstood(one.picture.mark, false, into);

    std::erase_if(pictured, [&held, &made](const Placement& one) {
        for (int y = one.at.y; y < one.at.y + one.picture.size.high; ++y)
            for (int x = one.at.x; x < one.at.x + one.picture.size.wide; ++x)
                if (!(made.at({x, y}) == held.standing.at({x, y}))) return false;

        return true;
    });

    const Colors palette = colors();

    for (int y = 0; y < made.size().high; ++y)
        for (int x = 0; x < made.size().wide; ++x) {
            const Cell one = made.at({x, y});

            // Ciąg dalszy szerokiego znaku wyszedł razem z jego pierwszą połową.
            if (one.letter == 0) continue;

            if (one == held.standing.at({x, y})) continue;

            if (!(held.at == ui::Spot{x, y})) {
                if (held.at.y != y || held.at.x < 0 || held.at.x > x ||
                    !bridged(made, y, held.at.x, x, held.now, into))
                    placed({x, y}, into);

                held.at = ui::Spot{x, y};
            }

            if (!(one.style == held.now)) {
                painted(one.style, palette, into);

                held.now = one.style;
            }

            utf8::encode(one.letter, into);

            const bool broad = x + 1 < made.size().wide && made.at({x + 1, y}).letter == 0;

            held.at.x += broad ? 2 : 1;

            // Zawijanie mamy wyłączone, więc na ostatniej kolumnie kursor stoi w miejscu
            // nieokreślonym — następny znak dostanie własny skok.
            if (held.at.x >= made.size().wide) held.at = ui::Spot{-1, -1};
        }

    // Po obrazie kursor stoi tam, gdzie postawił go terminal — następny znak dostanie własny skok.
    for (const Placement& one : pictured) {
        if (!one.picture.sixel) continue;

        placed(one.at, into);

        into += *one.picture.sixel;

        held.at = ui::Spot{-1, -1};
    }

    for (const Placement& one : standing) {
        if (std::any_of(held.placed.begin(), held.placed.end(), [&one](const Placement& was) { return same_place(was, one); }))
            continue;

        if (std::none_of(held.sent.begin(), held.sent.end(), [&one](const auto& was) { return was.first == one.picture.mark; })) {
            kitty(one.picture, into);

            held.sent.emplace_back(one.picture.mark, one.picture.pixels);
        }

        stood(one, into);

        // `C=1` zostawia kursor w miejscu, ale terminal, który klucza nie zna, przesunąłby go —
        // następny znak dostaje więc własny skok, jak po sixelu.
        held.at = ui::Spot{-1, -1};
    }

    held.placed = std::move(standing);

    // **Kursor bierzemy z korzenia, nie z kartki.** Klatka mówi, co narysować, a nie gdzie stoi
    // daszek — ten należy do ogniska. Pytamy po złożeniu, bo dopiero wtedy prostokąty są świeże.
    if (const std::optional<ui::Spot> spot = held.onto->caret(); spot.has_value()) {
        placed(*spot, into);

        held.at = *spot;

        if (!held.showing) {
            into += "\x1b[?25h";

            held.showing = true;
        }
    } else if (held.showing) {
        into += "\x1b[?25l";

        held.showing = false;
    }

    held.standing = std::move(made);

    std::erase_if(held.sent, [&into](const auto& one) {
        if (!one.second.expired()) return false;

        unstood(one.first, true, into);

        return true;
    });

    if (!into.empty()) {
        // Wstrzymanie rysowania na czas klatki, jeśli terminal to umie — wtedy nawet duża
        // zmiana pojawia się w całości naraz.
        Console& out = held.console.provider();

        if (synchronized(held.compat, held.wish)) out.write("\x1b[?2026h");

        out.write(into);

        if (synchronized(held.compat, held.wish)) out.write("\x1b[?2026l");
    }
}

namespace {

// Karmienie **po bajcie**, z bieżącym korzeniem przy każdym: klawisz w środku kawałka może
// przestawić korzeń (`Term::show`), a klawisze wpisane z wyprzedzeniem mają pójść już do nowego.
// Dekoder nie przywiązuje znaczenia do granic karmienia — próba „po bajcie to samo, co całością"
// tego pilnuje — więc to jedyna różnica. Koszt: kawałek to najwyżej kilobajt.
void fed(Decoder& decoder, std::string_view given, ui::Root* const& onto) {
    for (std::size_t at = 0; at < given.size(); ++at) decoder.fed(given.substr(at, 1), *onto);
}

}

void Term::stop() {
    held_->running = false;

    held_->loop->bell().ring();
}

void Term::run() {
    while (turn()) {
    }
}

bool Term::turn() {
    Held& held = *held_;

    if (held.onto == nullptr)
        throw std::logic_error("talcite::Term: no root set");

    // Terminal jest oddany: wejście należy do cudzego programu, ekran do tego, co on na nim
    // napisze. Czytanie stdin ukradłoby mu klawisz, a rysowanie zamazało obraz — zostaje sama
    // pętla, żeby zegar, sieć i czytanie plików mogły dokończyć swoje. Czekamy na dzwonku, a nie
    // w pustym obrocie, bo `run()` kręciłby się wtedy na gorąco.
    if (held.away > 0) {
        (void)held.loop->bell().wait(glancing);
        (void)held.loop->turn();

        return held.running;
    }

    // Klawisze naciśnięte w trakcie badania czekają od uruchomienia. Idą pierwsze, przed
    // czymkolwiek świeżym — inaczej przepadłyby mimo całej roboty, żeby ich nie zgubić.
    bool doing = false;

    if (!held.spare.empty()) {
        const std::string given = std::move(held.spare);
        held.spare.clear();

        fed(held.decoder, given, held.onto);

        doing = true;
    }

    // Pierwszą klatkę rysujemy **przed** czekaniem, bo czekanie może nie mieć ogranicznika:
    // inaczej program stałby pusty do pierwszego naciśnięcia.
    if (held.fresh) {
        held.fresh = false;

        draw();
    }

    // Bez ogranicznika, dopóki nie ma czego rozstrzygać. Samotny Esc daje pięćdziesiąt
    // milisekund. Kliknięcie czekające na swoje drugie nie budzi pętli: korzeń rozstrzyga je
    // chwilą drugiego, gdy ono przyjdzie, a gdy nie przyjdzie — nie ma czego rozstrzygać.
    std::optional<std::chrono::milliseconds> patience;

    if (held.decoder.pending()) patience = hesitating;

    // **Drzewo też ma prawo poprosić o obrót.** Animowany obrazek liczy klatkę z zegara, więc mówi
    // tylko, kiedy najpóźniej chce być obudzony; nikt tu nie wie, że chodzi o klatkę.
    const std::optional<std::chrono::milliseconds> wanted = held.onto->due();

    std::optional<std::chrono::steady_clock::time_point> awaited;

    if (wanted.has_value()) {
        const std::chrono::milliseconds soon =
            *wanted > std::chrono::milliseconds{0} ? *wanted : std::chrono::milliseconds{0};

        awaited = std::chrono::steady_clock::now() + soon;

        if (!patience.has_value() || soon < *patience) patience = soon;
    }

    // Czekamy na klawiaturę i na dzwonek pętli naraz. Konsola wraca pustką także po zmianie
    // rozmiaru — jak się o niej dowiaduje, to już jej sprawa — więc w bezczynności nie ma ani
    // jednego jałowego przebudzenia po tej stronie.
    Console& console = held.console.provider();

    const std::string given = console.hear(patience, &held.loop->bell());

    // Wznowiony po zatrzymaniu: terminal był w tym czasie powłoki, która zdjęła z niego nasze
    // ubranie i pisała po ekranie. Ubieramy go na nowo i rysujemy wszystko, jak po cudzym programie.
    if (console.resumed()) {
        if (held.entered) dressed(console, sending(held.compat, held.wish));

        held.shifted = true;
        doing = true;
    }

    const bool spoken = !given.empty();

    if (spoken) fed(held.decoder, given, held.onto);

    // Rozmiar sprawdzamy w każdym obrocie, nie tylko po pustce: obrót zdarza się też po klawiszu
    // i po dzwonku pętli, a okno mogło zmienić się właśnie wtedy.
    const bool sized = measured(console, held.wide, held.high);

    // Nic nie przyszło, a dekoder coś trzyma — to znaczy, że Esc był sam.
    if (!spoken && held.decoder.pending()) {
        held.decoder.flush(*held.onto);

        doing = true;
    }

    if (spoken) doing = true;

    if (sized) {
        ui::SizeEvent what{held.wide, held.high};

        (void)held.onto->dispatch(what);

        doing = true;
    }

    // Czas, o który prosiło drzewo, upłynął — więc któryś węzeł maluje się teraz inaczej niż przed
    // chwilą. Pytanie ponowne nic by tu nie dało: węzeł liczący klatkę z zegara zawsze oddaje resztę
    // **bieżącej** klatki, czyli liczbę dodatnią, i po przekroczeniu granicy wygląda tak samo jak
    // przed nią. Rozstrzyga więc chwila zapamiętana przed czekaniem, a nie ponowne pytanie.
    if (awaited.has_value() && std::chrono::steady_clock::now() >= *awaited) doing = true;

    // **Praca z pętli też jest zdarzeniem.** Wywołanie zwrotne przeniesione przez `here()` albo
    // `Loop::post` po to właśnie sięga do drzewa węzłów, więc bez tego zmiana byłaby widoczna
    // dopiero po najbliższym klawiszu — a pasek postępu nie ruszyłby się wcale.
    if (held.loop->turn() > 0) doing = true;

    // Puste obroty — te, w których tylko upłynął ogranicznik — nie kosztują nic. Gdy coś się
    // wydarzyło, składamy klatkę; jeśli mimo to nic się nie zmieniło, na terminal i tak nie
    // pójdzie ani bajt.
    if (doing) draw();

    return held.running;
}

}

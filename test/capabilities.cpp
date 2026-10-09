#include "proving.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/capabilities.hpp"

using namespace talcite;

namespace {

// Linia do atrapy stoi w klasie bazowej **przed** `Capabilities`, bo ta dostaje konsolę już przy
// budowie. Sama atrapa niczego nie udaje ponad rozmowę: badanie i podłoga idą przez prawdziwy
// kod, więc rozbiór odpowiedzi sprawdza się bez żadnego terminala i na każdym systemie.
struct Wire {
    std::shared_ptr<faking::Terminal> line;
};

// Terminal nagrany.
class Taped : private Wire, public Capabilities {
public:
    explicit Taped(std::string said, std::size_t piece = 0)
        : Taped(faking::terminal(std::move(said), piece)) {}

    explicit Taped(std::shared_ptr<faking::Terminal> given)
        : Wire{std::move(given)}, Capabilities(Seam<Console>{line}) {}

    [[nodiscard]] const std::string& asked() const { return line->written; }

    [[nodiscard]] int rounds() const { return line->rounds; }

    [[nodiscard]] const faking::Terminal& terminal() const { return *line; }
};

// Terminal nieodpowiadający — ani jednego bajtu, także na wartownika. Za konsolę odpowiada sam,
// zamiast pytać tę prawdziwą: inaczej wynik zależałby od tego, z czego uruchomiono program.
std::shared_ptr<faking::Terminal> mute(bool vt = false) {
    auto made = faking::terminal();

    made->sequences = vt;

    return made;
}

// Terminal, z którym nie ma jak rozmawiać — przekierowanie do potoku.
std::shared_ptr<faking::Terminal> apart() {
    auto made = faking::terminal();

    made->alive = false;

    return made;
}

const std::string modern =
    "\x1b[>0;280;0c"
    "\x1b[?1006;2$y"
    "\x1b[?1000;2$y"
    "\x1b[?2004;2$y"
    "\x1b[?1004;2$y"
    "\x1b[?2026;2$y"
    "\x1b[?1049;2$y"
    "\x1bP1+r5463=787465726d2d323536636f6c6f72\x1b\\"
    "\x1b]11;rgb:0000/0000/0000\x1b\\"
    "\x1b[1;3R"
    "\x1b[1;2R"
    "\x1b[1;3R"
    "\x1b[?62;1;2;6;9;15;22;29c";

}

TRIAL("Capabilities: pytania kończą się wartownikiem") {
    const Taped one{modern};

    (void)one.colors();

    const std::string& asked = one.asked();

    ought(!asked.empty());
    ought(asked.rfind("\x1b[c") == asked.size() - 3);
}

TRIAL("Capabilities: nowoczesny terminal oddaje wszystko") {
    const Taped one{modern};

    same(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    same(static_cast<int>(one.screen()), static_cast<int>(Screen::apart));
    ought(one.bracketed_paste());
    ought(one.focus_events());
    ought(one.synchronized());
    same(static_cast<int>(one.shade()), static_cast<int>(Shade::dark));
    same(one.version(), 280);
    same(one.named(), std::string{"xterm"});
}

TRIAL("Capabilities: pomiary szerokości wracają po kolei") {
    const Taped one{modern};

    same(one.wide(), 2);
    same(one.varied(), 1);
    same(one.joined(), 2);
}

TRIAL("Capabilities: XTGETTCAP podnosi kolor do pełnego") {
    const Taped one{modern};

    same(static_cast<int>(one.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Capabilities: nieznany tryb zostaje nieznany") {
    const Taped one{
        "\x1b[?1006;0$y"
        "\x1b[?2004;0$y"
        "\x1b[?2026;0$y"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.mouse()), static_cast<int>(Mouse::basic));
    ought(!one.bracketed_paste());
    ought(!one.synchronized());
}

TRIAL("Capabilities: jasne tło rozpoznane") {
    const Taped one{
        "\x1b]11;rgb:ffff/ffff/ffff\x1b\\"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

TRIAL("Capabilities: wartownik kończy czekanie od razu") {
    const Taped one{"\x1b[?62;1c"};

    (void)one.colors();

    same(one.rounds(), 1);
}

TRIAL("Capabilities: milczący terminal zostaje przy podłodze") {
    const Taped one{mute()};

    same(static_cast<int>(one.screen()), static_cast<int>(Screen::older));
    ought(!one.bracketed_paste());
    ought(!one.synchronized());
    same(one.wide(), 1);
}

TRIAL("Capabilities: bez terminala nie wysyłamy ani bajtu") {
    const Taped one{apart()};

    (void)one.colors();

    ought(one.asked().empty());
}

TRIAL("Capabilities: pytamy raz, choćby pytań było wiele") {
    const Taped one{modern};

    (void)one.colors();
    (void)one.mouse();
    (void)one.bracketed_paste();
    (void)one.wide();

    same(one.rounds(), 1);
}

TRIAL("Capabilities: cudze bajty nie giną") {
    const Taped one{
        "abc"
        "\x1b[?2004;2$y"
        "q"
        "\x1b[?62;1c"};

    ought(one.bracketed_paste());
    same(one.rest(), std::string{"abcq"});
}

TRIAL("Capabilities: odpowiedź poszatkowana składa się w całość") {
    const Taped one{modern, 3};

    same(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    ought(one.bracketed_paste());
    same(one.wide(), 2);
}

TRIAL("Capabilities: krótki zapis koloru tła też działa") {
    const Taped one{
        "\x1b]11;rgb:f/f/f\x1b\\"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

TRIAL("Capabilities: sam błękit to ciemne tło, choć składowa wysoka") {
    const Taped one{
        "\x1b]11;rgb:0000/0000/ffff\x1b\\"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::dark));
}

TRIAL("Capabilities: kolor tła zakończony dzwonkiem też się liczy") {
    const Taped one{
        "\x1b]11;rgb:ffff/ffff/ffff\x07"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

// Ogon, który w chwili końca badania jest niedokończony, może być równie dobrze początkiem
// naciśniętego klawisza. Wyrzucenie go gubiłoby klawisz, więc idzie do cudzych bajtów.
TRIAL("Capabilities: niedokończony ogon nie ginie") {
    const Taped one{
        "\x1b[?2004;2$y"
        "\x1b[?62;1c"
        "\x1b[A"};

    ought(one.bracketed_paste());
    same(one.rest(), std::string{"\x1b[A"});
}

TRIAL("Capabilities: tmux i screen rozpoznają się po rodzinie") {
    const Taped one{"\x1b[>84;0;0c\x1b[?62;1c"};
    const Taped two{"\x1b[>83;40500;0c\x1b[?62;1c"};

    same(one.named(), std::string{"tmux"});
    same(two.named(), std::string{"screen"});
}

TRIAL("Capabilities: klawisze wciśnięte w trakcie startu nie giną") {
    const Taped one{
        "\x1b[?2004;2$y"
        "\x1b[A"
        "\x1b[?62;1c"
        "q"};

    ought(one.bracketed_paste());
    same(one.rest(), std::string{"\x1b[A" "q"});
}

TRIAL("Capabilities: nieznana odpowiedź OSC też wraca w całości") {
    const Taped one{
        "\x1b]10;rgb:1111/1111/1111\x1b\\"
        "\x1b[?62;1c"};

    same(one.rest(), std::string{"\x1b]10;rgb:1111/1111/1111\x1b\\"});
}

// Podłoga czyta środowisko **z konsoli**, więc próba ustawia je atrapie, zamiast przestawiać
// zmienne całego procesu. Dawniej robiła to przez setenv — i przy okazji zależała od tego, co
// w środowisku zostawił uruchamiający.
TRIAL("Capabilities: NO_COLOR gasi kolor mimo odpowiedzi terminala") {
    auto line = faking::terminal(modern);

    line->environment["NO_COLOR"] = "1";
    line->environment["COLORTERM"] = "truecolor";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::mono));
}

TRIAL("Capabilities: COLORTERM podnosi do pełnej palety") {
    auto line = mute();

    line->environment["COLORTERM"] = "truecolor";
    line->environment["TERM"] = "xterm";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Capabilities: TERM z 256 daje paletę indeksowaną") {
    auto line = mute();

    line->environment["TERM"] = "xterm-256color";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
}

TRIAL("Capabilities: TERM dumb znaczy nic") {
    auto line = mute();

    line->environment["TERM"] = "dumb";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::mono));
    same(static_cast<int>(told.screen()), static_cast<int>(Screen::none));
    same(static_cast<int>(told.mouse()), static_cast<int>(Mouse::none));
    ought(!told.titles());
}

// Na tej drodze **decyduje konsola, nie TERM** — i tylko tutaj, bo wszystkie wcześniejsze gałęzie
// (NO_COLOR, COLORTERM, 256, dumb) kończą się przed pytaniem konsoli. Odpowiedź konsoli podstawia
// atrapa, więc obie strony tej decyzji sprawdzają się na każdym systemie, a wynik nie zależy od
// tego, z czego uruchomiono program.
TRIAL("Capabilities: zwykły TERM przy konsoli bez sekwencji daje szesnaście kolorów") {
    auto line = mute(false);

    line->environment["TERM"] = "vt220";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::ansi));
}

TRIAL("Capabilities: zwykły TERM przy konsoli z sekwencjami daje pełną paletę") {
    auto line = mute(true);

    line->environment["TERM"] = "vt220";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Capabilities: COLORTERM 24bit też podnosi do pełnej palety") {
    auto line = mute();

    line->environment["COLORTERM"] = "24bit";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

// Windows Terminal nie ustawia ani COLORTERM, ani TERM, ale ma pełną paletę i mówi o sobie własną
// zmienną.
TRIAL("Capabilities: WT_SESSION daje pełną paletę bez TERM") {
    auto line = mute();

    line->environment.erase("TERM");
    line->environment["WT_SESSION"] = "1";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

// Umowa NO_COLOR mówi „obecna i niepusta". Pusta zmienna niczego nie gasi.
TRIAL("Capabilities: pusty NO_COLOR nie gasi koloru") {
    auto line = mute();

    line->environment["NO_COLOR"] = "";

    const Taped told{line};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
}

TRIAL("Capabilities: KITTY_WINDOW_ID zgaduje obrazy kitty bez sondy") {
    auto line = apart();

    line->environment["KITTY_WINDOW_ID"] = "1";

    const Taped told{line};

    same(static_cast<int>(told.graphics()), static_cast<int>(Graphics::kitty));
}

TRIAL("Capabilities: nazwę bierzemy z TERM_PROGRAM, a bez niej z TERM") {
    auto one = apart();
    auto two = apart();

    one->environment["TERM_PROGRAM"] = "WezTerm";

    const Taped first{one};
    const Taped second{two};

    same(first.named(), std::string{"WezTerm"});
    same(second.named(), std::string{"xterm-256color"});
}

// Na Windows podłoga zakłada unicode zawsze, więc zmienne rozstrzygają tylko na POSIX-ie.
TRIAL("Capabilities: unicode rozstrzyga LANG, LC_ALL albo LC_CTYPE") {
    auto plain = apart();
    auto fine = apart();

    plain->environment["LANG"] = "C";
    fine->environment["LANG"] = "C";
    fine->environment["LC_CTYPE"] = "pl_PL.utf8";

    const Taped one{plain};
    const Taped two{fine};

#ifdef _WIN32

    ought(one.unicode());

#else

    ought(!one.unicode());

#endif

    ought(two.unicode());
}

TRIAL("Capabilities: sonda chodzi w trybie surowym i zdejmuje go po sobie") {
    const Taped one{modern};

    (void)one.colors();

    same(one.terminal().entered, 1);
    same(one.terminal().depth, 0);
}

TRIAL("Capabilities: bez terminala nie ma trybu surowego") {
    const Taped one{apart()};

    (void)one.colors();

    same(one.terminal().entered, 0);
}

// --- Obrazy -------------------------------------------------------------------------------------
//
// Sixela nie musimy o nic pytać: pierwotne DA wysyłamy jako wartownika sondy, a czwórka na jego
// liście znaczy „umiem sixel". Kitty odpowiada w APC. Rozmiar kratki w pikselach przychodzi z `16t`.

namespace {

// Pięć pomiarów szerokości po kolei: szeroki znak, znacznik, złączenie, sekstant, oktant.
std::string taped(const char* wides, const char* da, const char* more = "") {
    std::string made;

    made += "\x1b[>0;280;0c";
    made += more;

    for (const char* at = wides; *at != '\0'; ++at) {
        made += "\x1b[1;";
        made += static_cast<char>('0' + (*at - '0') + 1);
        made += "R";
    }

    made += da;

    return made;
}

const char* const plain = "\x1b[?62;1;2;6;22c";
const char* const drawing = "\x1b[?62;1;2;4;6;22c";

}

TRIAL("Capabilities: sixel poznaje się z pierwotnego DA") {
    const Taped one{taped("21211", drawing)};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::sixel));
}

TRIAL("Capabilities: bez czwórki w DA nie ma sixela") {
    const Taped one{taped("21211", plain)};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::none));
}

// tmux od 3.4 odpowiada czwórką, ale obraz gubi i rysuje w cudzych panelach — piksele w nim to
// jawne życzenie aplikacji, nie wniosek z sondy. Ten sam terminal bez tmuxa sixel ma.
TRIAL("Capabilities: w tmuxie i w screenie czwórka z DA nie daje sixela") {
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"TMUX", "/tmp/tmux-1000/default,1234,0"}, {"STY", "1234.pts-0.host"}}) {
        auto line = faking::terminal(taped("21211", drawing));

        line->environment[name] = value;

        const Taped one{line};

        same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::none));
    }

    auto line = faking::terminal(taped("21211", drawing));

    line->environment["TERM"] = "tmux-256color";

    same(static_cast<int>(Taped{line}.graphics()), static_cast<int>(Graphics::none));
}

// Kitty jest lepsze od sixela — pełna barwa zamiast palety — więc gdy terminal umie oba, wygrywa.
TRIAL("Capabilities: kitty wygrywa z sixelem") {
    const Taped one{taped("21211", drawing, "\x1b_Gi=31;OK\x1b\\")};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::kitty));
}

TRIAL("Capabilities: odpowiedź kitty nie wraca jako naciśnięte klawisze") {
    const Taped one{taped("21211", plain, "\x1b_Gi=31;OK\x1b\\")};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::kitty));
    same(one.rest(), std::string{});
}

TRIAL("Capabilities: rozmiar kratki przychodzi z 16t") {
    const Taped one{taped("21211", plain, "\x1b[6;20;10t")};

    same(one.cell().wide, 10);
    same(one.cell().high, 20);
}

// Zero znaczy „nie wiadomo", a nie „zero pikseli" — i wołający ma prawo to odróżnić, bo od tego
// zależy, czy zgadywać proporcję kratki.
TRIAL("Capabilities: nieznany rozmiar kratki to zero") {
    const Taped one{taped("21211", plain)};

    same(one.cell().wide, 0);
    same(one.cell().high, 0);
}

TRIAL("Capabilities: cudzy raport okna nie jest rozmiarem kratki") {
    const Taped one{taped("21211", plain, "\x1b[8;24;80t")};

    same(one.cell().wide, 0);
}

// --- Znaki blokowe ------------------------------------------------------------------------------
//
// Pomiar odpowiada wyłącznie na pytanie „czy ten znak zajmuje jedną kratkę". Czy font go ma, nie
// da się zmierzyć — brakujący glif rysuje się jako pusty prostokąt i zajmuje tyle samo miejsca.
// Dlatego `blocks()` oddaje **sufit**, a nie wybór: mówi, czego terminal jeszcze nie rozsypie.
// Wyboru dokonuje `Term`, biorąc mniejszą z dwóch — życzenia i sufitu.

TRIAL("Blocks: oba znaki równe kratce podnoszą sufit do oktantów") {
    const Taped one{taped("21211", plain)};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::octants));
}

TRIAL("Blocks: rozjechany sekstant zbija sufit do znaków blokowych") {
    const Taped one{taped("21221", plain)};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::basic));
}

TRIAL("Blocks: rozjechany oktant zbija sufit do sekstantów") {
    const Taped one{taped("21212", plain)};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::sextants));
}

// Milczący terminal niczego nie obalił, więc sufit zostaje wysoko. Brak pomiaru to nie to samo,
// co pomiar mówiący „nie".
TRIAL("Blocks: bez pomiaru sufit zostaje najwyższy") {
    const Taped one{mute()};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::octants));
}

TRIAL("Capabilities: sonda pyta o kratkę i o kitty, a wartownik dalej jest ostatni") {
    const Taped one{taped("21211", plain)};

    (void)one.graphics();

    const std::string& asked = one.asked();

    ought(asked.find("\x1b[16t") != std::string::npos);
    ought(asked.find("\x1b_Gi=31") != std::string::npos);
    ought(asked.rfind("\x1b[c") == asked.size() - 3);
}


// W tmuxie zmienne zewnętrznego terminala nie zgadują niczego: sesja założona w kitty niesie
// `KITTY_WINDOW_ID` do każdego okna, a tmux obrazów kitty nie przepuszcza. Zostaje `TERM`.
TRIAL("Capabilities: w tmuxie zmienne zewnętrznego terminala nie zgadują") {
    auto line = apart();

    line->environment["TMUX"] = "/tmp/tmux-1000/default,1234,0";
    line->environment["TERM"] = "tmux-256color";
    line->environment["KITTY_WINDOW_ID"] = "1";
    line->environment["WT_SESSION"] = "x";
    line->environment["TERM_PROGRAM"] = "WezTerm";

    const Taped told{line};

    same(static_cast<int>(told.graphics()), static_cast<int>(Graphics::none));
    same(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
    same(told.named(), std::string{"tmux-256color"});

    // screen i tmux rozpoznane innymi drogami: zmienną screena, samym TERM.
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"TMUX", "/tmp/tmux-1000/default,1234,0"},
             {"STY", "1234.pts-0.host"},
             {"TERM", "screen-256color"},
             {"TERM", "tmux"}}) {
        auto other = apart();

        other->environment[name] = value;
        other->environment["KITTY_WINDOW_ID"] = "1";
        other->environment["TERM_PROGRAM"] = "WezTerm";

        const Taped muxed{other};

        same(static_cast<int>(muxed.graphics()), static_cast<int>(Graphics::none));
        ought(muxed.named() != "WezTerm");
    }

    // Ten sam zestaw bez tmuxa — zmienne znów mówią.
    auto plain = apart();

    plain->environment["KITTY_WINDOW_ID"] = "1";
    plain->environment["TERM_PROGRAM"] = "WezTerm";

    const Taped bare{plain};

    same(static_cast<int>(bare.graphics()), static_cast<int>(Graphics::kitty));
    same(bare.named(), std::string{"WezTerm"});
}

// Przez SSH sonda czeka dłużej, bo odpowiedź wraca po pełnym obiegu sieci.
TRIAL("Capabilities: przez SSH sonda czeka dłużej") {
    auto near = mute();
    auto far = mute();

    far->environment["SSH_CONNECTION"] = "10.0.0.2 50000 10.0.0.1 22";

    const Taped here{near};
    const Taped there{far};

    (void)here.colors();
    (void)there.colors();

    ought(!near->patience.empty() && near->patience.front().has_value());
    ought(!far->patience.empty() && far->patience.front().has_value());

    ought(*near->patience.front() <= std::chrono::milliseconds{250});
    ought(*far->patience.front() > std::chrono::milliseconds{900});

    auto tty = mute();

    tty->environment["SSH_TTY"] = "/dev/pts/3";

    const Taped through{tty};

    (void)through.colors();

    ought(!tty->patience.empty() && tty->patience.front().has_value() &&
          *tty->patience.front() > std::chrono::milliseconds{900});
}

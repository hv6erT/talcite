#include "talcite/term.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/decoder.hpp"

#include "spout.hpp"

#ifdef _WIN32

#include <windows.h>

#else

#include <csignal>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#endif

namespace talcite {

namespace {

// Samotny Esc rozstrzyga się czasem, bo terminal nie mówi, czy coś jeszcze przyśle. Fork dawał
// dwadzieścia milisekund i przez ssh strzałka rozpadała się na Esc i literę.
constexpr std::chrono::milliseconds hesitating{50};

// Cisza, po której pierwsze kliknięcie przestaje czekać na drugie. Żadna specyfikacja
// przeglądarkowa tego nie ustala — implementacje biorą wartość z systemu, a GTK ma domyślnie
// tyle. Na Windows pytamy wprost o ustawienie użytkownika; pod POSIX nie ma o to kogo zapytać,
// a przy pracy zdalnej ustawienie i tak leżałoby na niewłaściwej maszynie.
constexpr std::chrono::milliseconds deciding{400};

// Na POSIX SIGWINCH przerywa czekanie i o zmianie rozmiaru wiemy natychmiast. Windows wkłada ją
// do kolejki wejścia jako rekord, ale rekordu nie widać przez tryb wirtualny, więc tam zostaje
// jeszcze zaglądanie co jakiś czas.
constexpr std::chrono::milliseconds glancing{100};

std::chrono::milliseconds twice() {
#ifdef _WIN32

    const UINT told = GetDoubleClickTime();

    if (told > 0) return std::chrono::milliseconds{static_cast<int>(told)};

#endif

    return deciding;
}

#ifndef _WIN32

volatile std::sig_atomic_t stirred = 0;

void widened(int) { stirred = 1; }

#endif

}

struct Term::Held {
    std::unique_ptr<Loop> mine;
    Loop* loop = nullptr;

    // Stała, bo po usunięciu setterów nie ma w niej ani jednej metody niestałej: mierzy terminal
    // i o niczym nie decyduje.
    const Compatibilities compat;
    Decoder decoder;

    // Życzenie aplikacji. W `Compatibilities` nic się nie wpisuje — ona mierzy i tyle — więc
    // wartość skuteczna powstaje przy każdym odczycie jako `życzenie.value_or(pomiar)`.
    Setup wish;

    // Świeża konfiguracja unieważnia obraz ekranu tak samo jak zmiana rozmiaru okna.
    bool shifted = false;

    ui::Root* onto = nullptr;

    std::string spare;
    bool running = true;
    bool entered = false;
    bool fresh = true;

    std::chrono::milliseconds twice = deciding;
    std::chrono::steady_clock::time_point since;
    bool counting = false;

    View standing;
    Style now;
    ui::Spot at{-1, -1};
    bool showing = false;

    int wide = 80;
    int high = 25;

    // Ile razy zeszliśmy terminalowi z drogi. Licznik, a nie flaga, bo zagnieżdżone zejście jest
    // dozwolone i ma znaczyć jedno: pierwszy wychodzi, ostatni wraca. Zdejmowanie stanu dwa razy
    // wysłałoby drugi komplet sekwencji, a przy myszy zgubiłoby stan pośredni.
    int away = 0;

#ifdef _WIN32
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE probe = nullptr;
    DWORD was_in = 0;
    DWORD was_out = 0;
    UINT was_read = 0;
    UINT was_said = 0;
#else
    termios was{};
    bool raw = false;
    void (*was_winch)(int) = nullptr;
#endif
};

namespace {

#ifdef _WIN32

// Same modyfikatory i blokady nie zamieniają się na żaden bajt, więc czekanie na nie skończyłoby
// się staniem w ReadFile do następnego prawdziwego klawisza.
bool aside(const KEY_EVENT_RECORD& key) {
    switch (key.wVirtualKeyCode) {
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_CAPITAL:
        case VK_NUMLOCK:
        case VK_SCROLL: return true;
        default: return false;
    }
}

// Kolejka konsoli budzi czekanie także rekordami, których tryb wirtualny nie zamienia na bajty:
// zmianą rozmiaru, ogniskiem, puszczeniem klawisza. ReadFile zjadłby je i **stanął** do
// następnego klawisza — a wtedy żaden obrót by się nie odbył i zmiana rozmiaru nie wyszłaby na
// wierzch. Więc najpierw zaglądamy: jeśli nie ma nic mówiącego, zdejmujemy ciszę sami.
bool speaking(HANDLE input) {
    for (int again = 0; again < 16; ++again) {
        DWORD many = 0;

        // Nie konsola — wejście przekierowane. Wtedy czytamy wprost, jak dotąd.
        if (GetNumberOfConsoleInputEvents(input, &many) == 0) return true;

        if (many == 0) return false;

        INPUT_RECORD seen[64];
        DWORD given = 0;
        const DWORD asked = many < 64 ? many : 64;

        if (PeekConsoleInputW(input, seen, asked, &given) == 0 || given == 0) return true;

        // Rekordy myszy zostawiamy czytaniu, nawet gdy same nie muszą nic dać: to ReadFile
        // zamienia je na sekwencję, gdy śledzenie myszy jest włączone.
        for (DWORD at = 0; at < given; ++at)
            if (seen[at].EventType == MOUSE_EVENT ||
                (seen[at].EventType == KEY_EVENT && seen[at].Event.KeyEvent.bKeyDown != 0 &&
                 !aside(seen[at].Event.KeyEvent)))
                return true;

        DWORD taken = 0;

        if (ReadConsoleInputW(input, seen, given, &taken) == 0 || taken == 0) return false;
    }

    return false;
}

// Prawda, gdy rozmiar się zmienił. Mierzy okno, nie bufor — bufor bywa wyższy o przewijanie.
bool measured(HANDLE output, int& wide, int& high) {
    CONSOLE_SCREEN_BUFFER_INFO told{};

    if (GetConsoleScreenBufferInfo(output, &told) == 0) return false;

    const int now_wide = told.srWindow.Right - told.srWindow.Left + 1;
    const int now_high = told.srWindow.Bottom - told.srWindow.Top + 1;

    if (now_wide <= 0 || now_high <= 0) return false;

    if (now_wide == wide && now_high == high) return false;

    wide = now_wide;
    high = now_high;

    return true;
}

#endif

int apart(int one, int two) { return (one - two) * (one - two); }

// Najbliższy punkt palety 256: sześcian 6×6×6 albo szarości. Bierzemy bliższy z dwóch, bo
// szarość z sześcianu bywa dalej niż osobna szara drabinka.
int indexed(Color one) {
    static const int steps[6] = {0, 95, 135, 175, 215, 255};

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

// Najbliższy z szesnastu podstawowych. Wartości jak w xtermie — inne terminale malują je
// nieco inaczej, ale wybór najbliższego i tak jest przybliżeniem.
int shortened(Color one) {
    static const int basics[16][3] = {
        {0, 0, 0},       {128, 0, 0},   {0, 128, 0},   {128, 128, 0},
        {0, 0, 128},     {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
        {128, 128, 128}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
        {0, 0, 255},     {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};

    int best = 0;
    int least = -1;

    for (int which = 0; which < 16; ++which) {
        const int much = apart(one.red, basics[which][0]) + apart(one.green, basics[which][1]) +
                         apart(one.blue, basics[which][2]);

        if (least < 0 || much < least) {
            least = much;
            best = which;
        }
    }

    return best;
}

void tinted(Color one, bool ink, Colors how, std::string& into) {
    if (one.transparent || how == Colors::mono) return;

    if (how == Colors::rgb) {
        into += ink ? ";38;2;" : ";48;2;";
        into += std::to_string(one.red) + ";" + std::to_string(one.green) + ";" +
                std::to_string(one.blue);

        return;
    }

    if (how == Colors::palette) {
        into += ink ? ";38;5;" : ";48;5;";
        into += std::to_string(indexed(one));

        return;
    }

    const int which = shortened(one);

    if (which < 8) into += ";" + std::to_string((ink ? 30 : 40) + which);
    else into += ";" + std::to_string((ink ? 90 : 100) + which - 8);
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

    tinted(what.ink, true, how, into);
    tinted(what.paper, false, how, into);

    into += "m";
}

void placed(ui::Spot where, std::string& into) {
    into += "\x1b[" + std::to_string(where.y + 1) + ";" + std::to_string(where.x + 1) + "H";
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

        if (one.letter == 0 || widely(one.letter) == 2) return false;

        if (!(one.style == now)) return false;
    }

    for (int x = from; x < upto; ++x) spelled(made.at({x, y}).letter, into);

    return true;
}

// Trzy ustawienia, których terminal nie odgadnie z klatki: trzeba mu je **powiedzieć** sekwencją
// i cofnąć drugą. Dla nich `Compatibilities` nie ma settera, więc wartość skuteczna powstaje tutaj.
struct Sending {
    Screen screen = Screen::none;
    Mouse mouse = Mouse::none;
    bool hovering = false;
    bool pasting = false;
};

// Śledzenie ruchu **bez wciśniętego przycisku** nie ma czego mierzyć w terminalu: żadna odpowiedź
// nie mówi, czy `?1003` jest rozumiane. Rozstrzyga więc samo życzenie, a domyślnie jest wyłączone —
// bo włączone każe terminalowi zgłaszać każde drgnięcie kursora, także wtedy, gdy nikogo to nie
// obchodzi, a przez ssh jest to prawdziwy ruch na łączu.
Sending sending(const Compatibilities& compat, const Setup& wish) {
    Sending made;

    made.screen = wish.screen.value_or(compat.screen());
    made.mouse = wish.mouse.value_or(compat.mouse());
    made.hovering = wish.hovering.value_or(false);
    made.pasting = wish.pasting.value_or(compat.pasting());

    return made;
}

// Jedyne pole, w którym życzenie nie wygrywa bez zastrzeżeń: `Compatibilities::blocks()` oddaje
// sufit, czyli gęstość, której terminal jeszcze nie rozsypie. Glif szerszy niż kratka psuje `View`,
// trafianie myszą i wypisywanie różnicy, więc bierzemy mniejszą z dwóch.
ui::Blocks blocked(const Compatibilities& compat, const Setup& wish) {
    const ui::Blocks want = wish.blocks.value_or(ui::Blocks::basic);
    const ui::Blocks most = compat.blocks();

    return want < most ? want : most;
}

bool syncing(const Compatibilities& compat, const Setup& wish) {
    return wish.syncing.value_or(compat.syncing());
}

bool titling(const Compatibilities& compat, const Setup& wish) {
    return wish.titling.value_or(compat.titling());
}

// `?1003` dokłada się do śledzenia, które już stoi, i **tylko wtedy, gdy ono stoi**: bez zgłaszania
// myszy w ogóle nie miałby kto tego ruchu przysłać. Zdejmujemy je pierwsze, bo obejmuje sobą to,
// co `?1002` — zdjęte na końcu zostawiłoby terminal na chwilę przy szerszym śledzeniu.
void mousing(Mouse given, bool hovering) {
    if (given == Mouse::sgr) writing("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    else if (given == Mouse::basic) writing("\x1b[?1000h");

    if (given != Mouse::none && hovering) writing("\x1b[?1003h");
}

void unmoused(Mouse given, bool hovering) {
    if (given != Mouse::none && hovering) writing("\x1b[?1003l");

    if (given == Mouse::sgr) writing("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    else if (given == Mouse::basic) writing("\x1b[?1000l");
}

void screened(Screen given) {
    if (given == Screen::apart) writing("\x1b[?1049h");
    else if (given == Screen::older) writing("\x1b[?47h");
}

void unscreened(Screen given) {
    if (given == Screen::apart) writing("\x1b[?1049l");
    else if (given == Screen::older) writing("\x1b[?47l");
}

// Ubranie terminala i jego zdjęcie. Dwie połówki jednej rzeczy, więc stoją obok siebie i mają
// **jedno** miejsce: robi to i konstruktor, i destruktor, i zejście z drogi na czas cudzego
// programu. Rozpisane w trzech miejscach rozjechałyby się przy pierwszej zmianie.
void dressed(const Sending& now) {
    screened(now.screen);

    writing("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");

    mousing(now.mouse, now.hovering);

    if (now.pasting) writing("\x1b[?2004h");
}

void undressed(const Sending& now) {
    if (now.pasting) writing("\x1b[?2004l");

    unmoused(now.mouse, now.hovering);

    writing("\x1b[?7h\x1b[?25h");

    unscreened(now.screen);
}

#ifdef _WIN32

void rawly(HANDLE input, HANDLE output, DWORD was_in, DWORD was_out) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    DWORD raw = was_in;
    raw &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                               ENABLE_MOUSE_INPUT);
    raw |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;

    SetConsoleMode(input, raw);

    DWORD painting = was_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    painting &= ~static_cast<DWORD>(ENABLE_WRAP_AT_EOL_OUTPUT);

    SetConsoleMode(output, painting);
}

#else

bool rawly(const termios& was) {
    termios raw = was;

    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    return tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

#endif

// Wysyłamy **samą różnicę**, nie całość: bezwarunkowe powtórzenie `\x1b[?1049h` na terminalu, który
// już jest na ekranie zapasowym, kosztowałoby mrugnięcie, a przy myszy zgubiłoby stan pośredni.
void shifting(const Sending& was, const Sending& now) {
    if (was.pasting != now.pasting) writing(now.pasting ? "\x1b[?2004h" : "\x1b[?2004l");

    if (was.mouse != now.mouse || was.hovering != now.hovering) {
        unmoused(was.mouse, was.hovering);
        mousing(now.mouse, now.hovering);
    }

    if (was.screen != now.screen) {
        unscreened(was.screen);
        screened(now.screen);

        writing("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");
    }
}

}

Term::Term(Setup given) : held_(std::make_unique<Held>()) {
    Held& held = *held_;

    held.wish = std::move(given);
    held.twice = twice();

    // Pętla należy do wątku i może być tylko jedna. Jeśli już stoi — przyjmujemy ją; jeśli nie —
    // zakładamy własną, bo bez niej Term nie ma na czym czekać.
    if (Loop* standing = Loop::current()) {
        held.loop = standing;
    } else {
        held.mine = std::make_unique<Loop>();
        held.loop = held.mine.get();
    }

#ifdef _WIN32

    held.input = GetStdHandle(STD_INPUT_HANDLE);
    held.output = GetStdHandle(STD_OUTPUT_HANDLE);

    GetConsoleMode(held.input, &held.was_in);
    GetConsoleMode(held.output, &held.was_out);

    held.was_read = GetConsoleCP();
    held.was_said = GetConsoleOutputCP();

    rawly(held.input, held.output, held.was_in, held.was_out);

#else

    if (tcgetattr(STDIN_FILENO, &held.was) == 0) held.raw = rawly(held.was);

    held.was_winch = std::signal(SIGWINCH, widened);

#endif

    // Badanie musi się odbyć na przygotowanym terminalu, ale **przed** wejściem na ekran
    // zapasowy — inaczej znaki próbne mrugnęłyby na czystym ekranie aplikacji.
    (void)held.compat.colors();

    held.spare = held.compat.rest();

    dressed(sending(held.compat, held.wish));

    held.entered = true;

#ifdef _WIN32

    // Standardowe wyjście wskazuje bufor, z którego wystartowaliśmy, a ekran zapasowy jest już
    // innym buforem. CONOUT$ otwiera ten czynny, więc rozmiar mierzymy tam, gdzie rysujemy.
    held.probe = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                             nullptr);

    if (held.probe == INVALID_HANDLE_VALUE) held.probe = nullptr;

    (void)measured(held.probe != nullptr ? held.probe : held.output, held.wide, held.high);

#else

    winsize told{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) == 0 && told.ws_col > 0) {
        held.wide = told.ws_col;
        held.high = told.ws_row;
    }

#endif
}

// Sprzątamy w odwrotnej kolejności i bez względu na to, jak wyszliśmy. Terminal zostawiony
// w trybie surowym bez kursora jest nie do użycia, a użytkownik nie ma jak tego cofnąć.
Term::~Term() {
    Held& held = *held_;

    // Cofamy stan **bieżący**, nie startowy: po `reconfigure` mysz albo ekran zapasowy mogą stać
    // inaczej niż w konstruktorze, a wyłączyć trzeba to, co naprawdę jest włączone.
    //
    // Gdy terminal jest właśnie oddany komuś innemu, zdejmować nie ma czego — `Aside` zdjął to
    // już przy wejściu. Tryby konsoli i tak przywracamy niżej, bo tam cofnięcie w kółko nic
    // nie kosztuje.
    if (held.entered && held.away == 0) undressed(sending(held.compat, held.wish));

#ifdef _WIN32

    if (held.probe != nullptr) CloseHandle(held.probe);

    SetConsoleMode(held.input, held.was_in);
    SetConsoleMode(held.output, held.was_out);

    if (held.was_read != 0) SetConsoleCP(held.was_read);
    if (held.was_said != 0) SetConsoleOutputCP(held.was_said);

#else

    if (held.raw) tcsetattr(STDIN_FILENO, TCSANOW, &held.was);

    std::signal(SIGWINCH, held.was_winch == nullptr ? SIG_DFL : held.was_winch);

#endif
}

// **Zejście z drogi.** Cofamy dokładnie to, co założył konstruktor, i w odwrotnej kolejności:
// najpierw sekwencje, potem tryby konsoli. Program, który teraz ruszy, zastaje terminal taki, jaki
// zastaliśmy go my — z echem, z Enterem, z Ctrl-C, na tym buforze, na którym startowaliśmy.
//
// Klasa, a nie para metod publicznych, bo para metod jest umową, której kompilator nie pilnuje:
// dałoby się wrócić bez wychodzenia albo wyjść i nie wrócić. Tu wyjście i powrót są jedną rzeczą,
// więc powrót następuje także wtedy, gdy po drodze poleci wyjątek. Klasa zagnieżdżona sięga do
// `Held` bez żadnego `friend`, a publiczna powierzchnia `Term` rośnie o jedno pojęcie, nie o dwa
// czasowniki.
Term::Aside::Aside(Term& given) : term_(given) {
    Held& held = *term_.held_;

    if (held.away++ > 0) return;

    if (held.entered) undressed(sending(held.compat, held.wish));

#ifdef _WIN32

    SetConsoleMode(held.input, held.was_in);
    SetConsoleMode(held.output, held.was_out);

    if (held.was_read != 0) SetConsoleCP(held.was_read);
    if (held.was_said != 0) SetConsoleOutputCP(held.was_said);

#else

    if (held.raw) tcsetattr(STDIN_FILENO, TCSANOW, &held.was);

    // Uchwyt na zmianę rozmiaru też oddajemy: dopóki nie rysujemy, zmiana nas nie obchodzi,
    // a nasza obsługa i tak tylko podnosi flagę, której nikt nie czyta.
    std::signal(SIGWINCH, held.was_winch == nullptr ? SIG_DFL : held.was_winch);

#endif
}

Term::Aside::~Aside() {
    Held& held = *term_.held_;

    if (--held.away > 0) return;

#ifdef _WIN32

    rawly(held.input, held.output, held.was_in, held.was_out);

#else

    if (held.raw) held.raw = rawly(held.was);

    std::signal(SIGWINCH, widened);

#endif

    if (held.entered) dressed(sending(held.compat, held.wish));

    // Po cudzym programie na ekranie nie zostało nic z naszej klatki, a przy `Screen::plain`
    // nawet ekran zapasowy jej nie schował. `shifted` znaczy dla `draw()` dokładnie to samo, co
    // zmiana rozmiaru: porównaj z pustą kartką, czyli odmaluj wszystko.
    held.shifted = true;
    held.fresh = true;

    // Rozmiar mógł się zmienić, gdy nas nie było — SIGWINCH szedł wtedy nie do nas.
#ifdef _WIN32

    (void)measured(held.probe != nullptr ? held.probe : held.output, held.wide, held.high);

#else

    winsize told{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) == 0 && told.ws_col > 0) {
        held.wide = told.ws_col;
        held.high = told.ws_row;
    }

#endif
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

        shifting(was, sending(held->compat, held->wish));

        if (held->onto != nullptr) {
            held->onto->blocks(blocked(held->compat, held->wish));
            held->onto->cell(held->wish.cell.value_or(held->compat.cell()));
            held->onto->colors(held->wish.colors.value_or(held->compat.colors()));
            held->onto->unicode(held->wish.unicode.value_or(held->compat.unicode()));
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
void Term::onto(ui::Root& given) {
    held_->onto = &given;

    given.blocks(blocks());
    given.cell(cell());
    given.colors(colors());
    given.unicode(unicode());
}

ui::Root* Term::onto() const { return held_->onto; }

int Term::wide() const { return held_->wide; }

int Term::high() const { return held_->high; }

void Term::title(std::string_view said) {
    if (!titling(held_->compat, held_->wish)) return;

    std::string made = "\x1b]0;";
    made += said;
    made += "\x1b\\";

    writing(made);
}

// Klatka powstaje w całości w pamięci, na terminal idzie sama różnica i to jednym zapisem:
// widoczna połowa klatki jest gorsza niż klatka spóźniona o mikrosekundę.
void Term::draw() {
    Held& held = *held_;

    if (held.onto == nullptr)
        throw std::logic_error("talcite::Term: nie ustawiono korzenia");

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
        into += "\x1b[0m\x1b[2J";

        held.standing = View{made.size()};
        held.now = Style{};
        held.at = ui::Spot{-1, -1};

        held.shifted = false;
    }

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

            spelled(one.letter, into);

            const bool broad = x + 1 < made.size().wide && made.at({x + 1, y}).letter == 0;

            held.at.x += broad ? 2 : 1;

            // Zawijanie mamy wyłączone, więc na ostatniej kolumnie kursor stoi w miejscu
            // nieokreślonym — następny znak dostanie własny skok.
            if (held.at.x >= made.size().wide) held.at = ui::Spot{-1, -1};
        }

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

    if (!into.empty()) {
        // Wstrzymanie rysowania na czas klatki, jeśli terminal to umie — wtedy nawet duża
        // zmiana pojawia się w całości naraz.
        if (syncing(held.compat, held.wish)) writing("\x1b[?2026h");

        writing(into);

        if (syncing(held.compat, held.wish)) writing("\x1b[?2026l");
    }

    held.standing = std::move(made);
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
        throw std::logic_error("talcite::Term: nie ustawiono korzenia");

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

        held.decoder.fed(given, *held.onto);

        doing = true;
    }

    // Pierwszą klatkę rysujemy **przed** czekaniem, bo czekanie na POSIX nie ma ogranicznika:
    // inaczej program stałby pusty do pierwszego naciśnięcia.
    if (held.fresh) {
        held.fresh = false;

        draw();
    }

    bool sized = false;

#ifndef _WIN32

    if (stirred != 0) {
        stirred = 0;
        sized = true;
    }

#endif

    // Bez ogranicznika, dopóki nie ma czego rozstrzygać. Samotny Esc daje pięćdziesiąt
    // milisekund, kliknięcie czekające na swoje drugie — tyle, ile mu jeszcze zostało.
    std::chrono::milliseconds patience{-1};

    if (held.decoder.pending()) patience = hesitating;

    if (held.onto->clicking()) {
        if (!held.counting) {
            held.counting = true;
            held.since = std::chrono::steady_clock::now();
        }

        const std::chrono::milliseconds gone =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - held.since);

        const std::chrono::milliseconds left =
            held.twice > gone ? held.twice - gone : std::chrono::milliseconds{0};

        if (patience.count() < 0 || left < patience) patience = left;
    } else {
        held.counting = false;
    }

    // **Drzewo też ma prawo poprosić o obrót.** Animowany obrazek liczy klatkę z zegara, więc mówi
    // tylko, kiedy najpóźniej chce być obudzony; nikt tu nie wie, że chodzi o klatkę.
    const std::optional<std::chrono::milliseconds> wanted = held.onto->due();

    std::optional<std::chrono::steady_clock::time_point> awaited;

    if (wanted.has_value()) {
        const std::chrono::milliseconds soon =
            *wanted > std::chrono::milliseconds{0} ? *wanted : std::chrono::milliseconds{0};

        awaited = std::chrono::steady_clock::now() + soon;

        if (patience.count() < 0 || soon < patience) patience = soon;
    }

#ifdef _WIN32

    // Na Windows zaglądamy i tak, bo zmiany rozmiaru nie widać inaczej.
    if (patience.count() < 0 || patience > glancing) patience = glancing;

#endif

#ifdef _WIN32

    const DWORD waiting = static_cast<DWORD>(patience.count());

    HANDLE watched[2] = {held.input, held.loop->bell().waited()};

    const DWORD ready = WaitForMultipleObjects(2, watched, FALSE, waiting);

    bool spoken = ready == WAIT_OBJECT_0 && speaking(held.input);

    if (spoken) {
        char room[1024];
        DWORD given = 0;

        if (ReadFile(held.input, room, sizeof(room), &given, nullptr) != 0 && given > 0)
            held.decoder.fed(std::string_view{room, given}, *held.onto);
        else spoken = false;
    }

    // Rozmiar sprawdzamy w każdym obrocie, nie tylko po upłynięciu ogranicznika: obrót zdarza się
    // też po klawiszu i po dzwonku pętli, a okno mogło zmienić się właśnie wtedy.
    if (measured(held.probe != nullptr ? held.probe : held.output, held.wide, held.high))
        sized = true;

#else

    pollfd watched[2] = {{STDIN_FILENO, POLLIN, 0}, {held.loop->bell().waited(), POLLIN, 0}};

    // Na POSIX czekamy bez końca, gdy nie ma czego rozstrzygać: SIGWINCH przerwie poll, a
    // dzwonek pętli obudzi. Zero jałowych przebudzeń w bezczynności.
    const int ready = ::poll(watched, 2, static_cast<int>(patience.count()));

    bool spoken = false;

    if (ready > 0 && (watched[0].revents & POLLIN) != 0) {
        char room[1024];

        const ssize_t given = ::read(STDIN_FILENO, room, sizeof(room));

        if (given > 0) {
            spoken = true;

            held.decoder.fed(std::string_view{room, static_cast<std::size_t>(given)},
                             *held.onto);
        }
    }

    if (stirred != 0) {
        stirred = 0;
        sized = true;
    }

    if (sized) {
        winsize told{};

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) == 0 && told.ws_col > 0) {
            held.wide = told.ws_col;
            held.high = told.ws_row;
        }
    }

#endif

    // Cisza dłuższa niż okno znaczy, że drugie kliknięcie nie przyjdzie — więc pierwsze było
    // pojedyncze. Ta sama myśl co przy samotnym Esc, tylko z dłuższą ciszą.
    if (held.counting && held.onto->clicking()) {
        const std::chrono::milliseconds gone =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - held.since);

        if (gone >= held.twice) {
            held.counting = false;

            held.onto->settle();

            doing = true;
        }
    }

    // Nic nie przyszło, a dekoder coś trzyma — to znaczy, że Esc był sam.
    if (!spoken && held.decoder.pending()) {
        held.decoder.flush(*held.onto);

        doing = true;
    }

    if (spoken) doing = true;

    if (sized) {
        ui::SizeEvent what{held.wide, held.high};

        (void)held.onto->deal(what);

        doing = true;
    }

    // Czas, o który prosiło drzewo, upłynął — więc któryś węzeł maluje się teraz inaczej niż przed
    // chwilą. Pytanie ponowne nic by tu nie dało: węzeł liczący klatkę z zegara zawsze oddaje resztę
    // **bieżącej** klatki, czyli liczbę dodatnią, i po przekroczeniu granicy wygląda tak samo jak
    // przed nią. Rozstrzyga więc chwila zapamiętana przed czekaniem, a nie ponowne pytanie.
    if (awaited.has_value() && std::chrono::steady_clock::now() >= *awaited) doing = true;

    // **Praca z pętli też jest zdarzeniem.** Wywołanie zwrotne przeniesione przez `main()` albo
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

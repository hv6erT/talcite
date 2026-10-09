#include "talcite/console.hpp"

#include "talcite/encoding.hpp"

#include "conhost.hpp"
#include "sealed.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#else

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#endif

namespace talcite {

namespace {

// Najwięcej, ile oddaje jedno słuchanie. Odpowiedzi terminala mieszczą się w kilobajcie; więcej
// to wklejka albo śmieci i nie ma powodu trzymać ich w całości.
constexpr std::size_t brim = 64 * 1024;

// Cisza, po której pierwsze kliknięcie przestaje czekać na drugie. Żadna specyfikacja
// przeglądarkowa tego nie ustala — implementacje biorą wartość z systemu, a GTK ma domyślnie
// tyle. Na Windows pytamy wprost o ustawienie użytkownika; pod POSIX nie ma o to kogo zapytać,
// a przy pracy zdalnej ustawienie i tak leżałoby na niewłaściwej maszynie.
constexpr std::chrono::milliseconds deciding{400};

#ifdef _WIN32

// Windows wkłada zmianę rozmiaru do kolejki wejścia jako rekord, ale tylko wtedy, gdy zmienia się
// bufor — w conhoście z przewijaniem okno rośnie, a bufor stoi. Dlatego słuchanie zagląda do
// rozmiaru co tyle, niezależnie od rekordów.
constexpr std::chrono::milliseconds glancing{100};

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
// następnego klawisza. Więc najpierw zaglądamy: jeśli nie ma nic mówiącego, zdejmujemy ciszę sami.
bool speaking(HANDLE input) {
    for (int again = 0; again < 16; ++again) {
        DWORD many = 0;

        // Nie konsola — wejście przekierowane. Wtedy czytamy wprost.
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

// Rozmiar okna, nie bufora — bufor bywa wyższy o przewijanie. **Mierzymy bufor czynny w tej
// chwili**: standardowe wyjście wskazuje ten, z którego wystartowaliśmy, a ekran zapasowy jest
// innym buforem, i to po każdym `?1049h` nowym. Dlatego CONOUT$ otwieramy przy każdym pomiarze,
// zamiast trzymać uchwyt z chwili startu, który po powrocie z cudzego programu wskazywałby bufor
// już nieczynny.
std::optional<ui::Size> measured() {
    const HANDLE probe = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                     nullptr);

    const HANDLE output = probe != INVALID_HANDLE_VALUE ? probe : GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO told{};

    const bool read = GetConsoleScreenBufferInfo(output, &told) != 0;

    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);

    if (!read) return std::nullopt;

    const int wide = told.srWindow.Right - told.srWindow.Left + 1;
    const int high = told.srWindow.Bottom - told.srWindow.Top + 1;

    if (wide <= 0 || high <= 0) return std::nullopt;

    return ui::Size{wide, high};
}

// **Konsola, która VT nie przyjmuje** — Windows 7 i 8.1, pierwsze wydania 10 i „starsza konsola"
// z ustawień. Do tej pory dostawała te same sekwencje, co reszta, i wypisywała je jako tekst.
// Kompilacja z TALCITE_LEGACY_CONSOLE wymusza tę drogę także na nowej konsoli — żeby dało się ją
// sprawdzić bez starego systemu.
#ifdef TALCITE_LEGACY_CONSOLE
constexpr bool forced = true;
#else
constexpr bool forced = false;
#endif

// Klej między `conhost::Screen` a buforem konsoli. Zapis karmi siatkę i przepisuje zmienione
// wiersze przez WriteConsoleOutputW; ekran zapasowy to **osobny bufor**, wielkości okna — wyjście
// z niego oddaje bufor główny taki, jaki był, razem z przewijaniem, bez przepisywania ani jednej
// komórki. Wejście czytamy rekordami i zamieniamy na bajty, które dałaby konsola z VT.
class Legacy {
public:
    explicit Legacy(HANDLE output) : original_(output), screen_(opened(output)) {
        CONSOLE_SCREEN_BUFFER_INFO told{};

        if (GetConsoleScreenBufferInfo(output, &told) != 0) {
            // Kursor stoi tam, gdzie zostawiła go powłoka — mówimy to siatce tak, jak powiedziałby
            // to terminal: skokiem.
            const int x = told.dwCursorPosition.X - told.srWindow.Left;
            const int y = told.dwCursorPosition.Y - told.srWindow.Top;

            screen_.fed("\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H");
        }

        (void)screen_.dirty();
    }

    ~Legacy() {
        parted();
        unfonted();
    }

    Legacy(const Legacy&) = delete;
    Legacy& operator=(const Legacy&) = delete;
    Legacy(Legacy&&) = delete;
    Legacy& operator=(Legacy&&) = delete;

    void written(std::string_view bytes) {
        screen_.fed(bytes);

        flushed();
    }

    [[nodiscard]] std::string answered() { return screen_.answers(); }

    // Rekordy, które czekają, na bajty. `stirred` mówi, że zmienił się bufor — wołający i tak
    // zmierzy okno od nowa.
    [[nodiscard]] std::string heard(HANDLE input, bool& stirred) {
        std::string made;

        DWORD many = 0;

        if (GetNumberOfConsoleInputEvents(input, &many) == 0 || many == 0) return made;

        INPUT_RECORD seen[64];
        DWORD given = 0;

        if (ReadConsoleInputW(input, seen, many < 64 ? many : 64, &given) == 0) return made;

        for (DWORD at = 0; at < given; ++at) {
            const INPUT_RECORD& one = seen[at];

            if (one.EventType == KEY_EVENT) {
                const KEY_EVENT_RECORD& key = one.Event.KeyEvent;

                // Znak wpisany z Altem na klawiaturze numerycznej przychodzi z puszczeniem Alta.
                if (key.bKeyDown != 0)
                    made += conhost::typed({key.wVirtualKeyCode, static_cast<char16_t>(key.uChar.UnicodeChar),
                                            key.dwControlKeyState, key.wRepeatCount},
                                           pending_);
                else if (key.wVirtualKeyCode == VK_MENU && key.uChar.UnicodeChar != 0)
                    made += conhost::typed({0, static_cast<char16_t>(key.uChar.UnicodeChar), 0, 1}, pending_);
            } else if (one.EventType == MOUSE_EVENT) {
                const MOUSE_EVENT_RECORD& mouse = one.Event.MouseEvent;
                const ui::Spot origin = corner();

                made += pointing_.pointed({{mouse.dwMousePosition.X - origin.x, mouse.dwMousePosition.Y - origin.y},
                                           mouse.dwButtonState,
                                           mouse.dwEventFlags,
                                           mouse.dwControlKeyState},
                                          screen_.tracking());
            } else if (one.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                stirred = true;
            }
        }

        return made;
    }

    // Okno ma nowy rozmiar. Własny bufor idzie za nim, żeby nie wyrosły paski przewijania.
    void resized(ui::Size size) {
        if (size == screen_.size()) return;

        if (own_ != nullptr)
            SetConsoleScreenBufferSize(own_, COORD{static_cast<SHORT>(size.wide), static_cast<SHORT>(size.high)});

        screen_.resize(size);
    }

    // **Czcionka rastrowa** zna tylko stronę kodową OEM: ramki, strzałki i litery spoza niej konsola
    // pokazałaby jako znaki zapytania. Na czas programu bierzemy TrueType tej samej wysokości —
    // Consolas, a gdzie jej nie ma, Lucida Console — i przy wyjściu oddajemy dawną. Gdy żadna się nie
    // przyjmie, zostaje rastrowa; co wtedy wybrać we właściwościach okna, mówi pomoc programu.
    void fonted() {
        CONSOLE_FONT_INFOEX now{};
        now.cbSize = sizeof now;

        if (!was_.has_value() && GetCurrentConsoleFontEx(original_, FALSE, &now) != 0 && (now.FontFamily & TMPF_TRUETYPE) == 0) {
            was_ = now;

            for (const std::wstring_view face : {std::wstring_view{L"Consolas"}, std::wstring_view{L"Lucida Console"}}) {
                CONSOLE_FONT_INFOEX wanted = now;

                wanted.nFont = 0;
                wanted.dwFontSize.X = 0;
                wanted.FontFamily = FF_DONTCARE;
                wanted.FontWeight = FW_NORMAL;

                std::fill(std::begin(wanted.FaceName), std::end(wanted.FaceName), L'\0');
                std::copy(face.begin(), face.end(), std::begin(wanted.FaceName));

                if (SetCurrentConsoleFontEx(original_, FALSE, &wanted) != 0 && truetyped(original_)) break;
            }

            // Żadna nie weszła, a nieudana próba mogła zostawić inną rastrową — wraca ta sprzed nas.
            if (!truetyped(original_)) unfonted();
        }

        CONSOLE_FONT_INFO font{};

        if (GetCurrentConsoleFont(original_, FALSE, &font) != 0 && font.dwFontSize.X > 0 && font.dwFontSize.Y > 0)
            screen_.pixels({font.dwFontSize.X, font.dwFontSize.Y});
    }

    // Dawna czcionka wraca — przy wyjściu z programu i przed oddaniem konsoli programowi potomnemu.
    void unfonted() {
        if (!was_.has_value()) return;

        SetCurrentConsoleFontEx(original_, FALSE, &*was_);

        was_.reset();
    }

    // Powrót do bufora głównego, gdyby aplikacja nie wyszła z ekranu zapasowego sama.
    void parted() {
        if (own_ == nullptr) return;

        SetConsoleActiveScreenBuffer(original_);
        CloseHandle(own_);

        own_ = nullptr;
    }

private:
    static conhost::Screen opened(HANDLE output) {
        CONSOLE_SCREEN_BUFFER_INFOEX told{};
        told.cbSize = sizeof told;

        if (GetConsoleScreenBufferInfoEx(output, &told) == 0) {
            const std::optional<ui::Size> size = measured();

            return conhost::Screen{size.value_or(ui::Size{80, 25}), 7, 0};
        }

        conhost::Palette palette{};

        for (std::size_t at = 0; at < palette.size(); ++at) {
            const COLORREF one = told.ColorTable[at];

            palette[at] = conhost::Rgb{GetRValue(one), GetGValue(one), GetBValue(one)};
        }

        const ui::Size size{told.srWindow.Right - told.srWindow.Left + 1, told.srWindow.Bottom - told.srWindow.Top + 1};

        return conhost::Screen{size, static_cast<std::uint8_t>(told.wAttributes & 0x0F),
                               static_cast<std::uint8_t>((told.wAttributes >> 4) & 0x0F), palette};
    }

    [[nodiscard]] HANDLE active() const { return own_ != nullptr ? own_ : original_; }

    [[nodiscard]] static bool truetyped(HANDLE buffer) {
        CONSOLE_FONT_INFOEX now{};
        now.cbSize = sizeof now;

        return GetCurrentConsoleFontEx(buffer, FALSE, &now) != 0 && (now.FontFamily & TMPF_TRUETYPE) != 0;
    }

    // Lewy górny róg okna w buforze. Własny bufor ma go w zerze; główny tam, dokąd go przewinięto.
    [[nodiscard]] ui::Spot corner() const {
        CONSOLE_SCREEN_BUFFER_INFO told{};

        if (GetConsoleScreenBufferInfo(active(), &told) == 0) return {};

        return {told.srWindow.Left, told.srWindow.Top};
    }

    void flushed() {
        if (screen_.apart() && own_ == nullptr) {
            own_ = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);

            if (own_ == INVALID_HANDLE_VALUE) own_ = nullptr;

            if (own_ != nullptr) {
                const ui::Size size = screen_.size();

                // Bufor zapasowy dostaje czcionkę, którą ustawiliśmy głównemu: SetCurrentConsoleFontEx
                // bierze uchwyt bufora, nie okna, więc nie zakładamy, że nowy bufor ją odziedziczy.
                if (was_.has_value()) {
                    CONSOLE_FONT_INFOEX now{};
                    now.cbSize = sizeof now;

                    if (GetCurrentConsoleFontEx(original_, FALSE, &now) != 0) SetCurrentConsoleFontEx(own_, FALSE, &now);
                }

                SetConsoleScreenBufferSize(own_, COORD{static_cast<SHORT>(size.wide), static_cast<SHORT>(size.high)});
                SetConsoleActiveScreenBuffer(own_);
            }
        } else if (!screen_.apart()) {
            parted();
        }

        const HANDLE target = active();
        const ui::Spot origin = corner();
        const ui::Size size = screen_.size();

        const std::vector<int> rows = screen_.dirty();

        std::vector<CHAR_INFO> line(static_cast<std::size_t>(size.wide));

        for (const int y : rows) {
            for (int x = 0; x < size.wide; ++x) {
                const conhost::Cell& one = screen_.at(x, y);

                CHAR_INFO& into = line[static_cast<std::size_t>(x)];

                into.Char.UnicodeChar = static_cast<WCHAR>(one.glyph);
                into.Attributes = static_cast<WORD>(one.ink | (one.paper << 4) | (one.under ? COMMON_LVB_UNDERSCORE : 0));
            }

            SMALL_RECT where{static_cast<SHORT>(origin.x), static_cast<SHORT>(origin.y + y),
                             static_cast<SHORT>(origin.x + size.wide - 1), static_cast<SHORT>(origin.y + y)};

            WriteConsoleOutputW(target, line.data(), COORD{static_cast<SHORT>(size.wide), 1}, COORD{0, 0}, &where);
        }

        const ui::Spot cursor = screen_.cursor();

        SetConsoleCursorPosition(target, COORD{static_cast<SHORT>(origin.x + cursor.x), static_cast<SHORT>(origin.y + cursor.y)});

        CONSOLE_CURSOR_INFO shape{};

        if (GetConsoleCursorInfo(target, &shape) != 0 && (shape.bVisible != 0) != screen_.shown()) {
            shape.bVisible = screen_.shown() ? TRUE : FALSE;

            SetConsoleCursorInfo(target, &shape);
        }

        if (const std::optional<std::string> title = screen_.title(); title.has_value())
            SetConsoleTitleW(to_wstring(*title).c_str());
    }

    HANDLE original_;
    HANDLE own_ = nullptr;
    std::optional<CONSOLE_FONT_INFOEX> was_;
    conhost::Screen screen_;
    conhost::Pointing pointing_;
    char16_t pending_ = 0;
};

#else

// Rurka sygnału zmiany rozmiaru. Obsługa sygnału wolno jej tylko pisać do deskryptora, a słuchanie
// czeka na nią w tym samym poll(), co na klawiaturę — więc zmiana rozmiaru budzi czekanie
// niezawodnie, także wtedy, gdy przyszła tuż przed nim.
volatile std::sig_atomic_t stirred = -1;

void widened(int) {
    if (stirred < 0) return;

    const int saved = errno;
    const unsigned char one = 1;
    const ssize_t wrote = ::write(stirred, &one, 1);
    (void)wrote;
    errno = saved;
}

// **Sygnały, po których terminal ma wrócić do człowieka.** Zabity przez SIGTERM albo SIGHUP
// program zostawiał terminal w trybie surowym, na ekranie zapasowym, z myszą i bez kursora —
// powłoka pod spodem była nie do użycia do `reset`. Zatrzymany z zewnątrz (SIGTSTP) zostawiał to
// samo powłoce, a wznowiony (SIGCONT) rysował po tym, co narysowała ona.
//
// Obsługa sygnału wolno jej tylko to, co bezpieczne w obsłudze sygnału: `tcsetattr`, `write`,
// `sigaction`, `raise`. Stąd stan w zmiennych z góry — tryb sprzed i tryb surowy — oraz pożegnanie
// stałym napisem, który zdejmuje wszystko, co `Term` umie włączyć; zdjęcie czegoś, czego nie
// włączono, nic nie robi.
termios cooked_kept{};
termios raw_kept{};
volatile std::sig_atomic_t rawly = 0;
volatile std::sig_atomic_t returned = 0;

constexpr char farewell[] = "\x1b[?2004l\x1b[?1003l\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l";

void restored() {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &cooked_kept);

    const ssize_t wrote = ::write(STDOUT_FILENO, farewell, sizeof farewell - 1);
    (void)wrote;
}

void unblocked(int which) {
    sigset_t one;

    sigemptyset(&one);
    sigaddset(&one, which);

    (void)::pthread_sigmask(SIG_UNBLOCK, &one, nullptr);
}

void handled(int which, void (*how)(int), struct sigaction* was) {
    struct sigaction made {};

    made.sa_handler = how;
    made.sa_flags = SA_RESTART;
    sigemptyset(&made.sa_mask);

    (void)::sigaction(which, &made, was);
}

// SIGTERM, SIGHUP, SIGQUIT: terminal wraca, a sygnał przychodzi jeszcze raz, już z domyślną
// reakcją — kod wyjścia i powód końca są takie, jakich spodziewa się ten, kto go wysłał.
void parted(int which) {
    restored();

    handled(which, SIG_DFL, nullptr);
    unblocked(which);

    (void)::raise(which);
}

// SIGTSTP: terminal wraca do powłoki, a my zatrzymujemy się naprawdę — domyślną reakcją, wywołaną
// stąd. Wznowienie wraca w to samo miejsce: tryb surowy na nowo i znak dla słuchania, że trzeba
// ubrać terminal i narysować wszystko jeszcze raz.
void paused(int which) {
    const int saved = errno;

    restored();

    handled(which, SIG_DFL, nullptr);
    unblocked(which);

    (void)::raise(which);

    handled(which, paused, nullptr);

    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &raw_kept);

    returned = 1;
    widened(0);

    errno = saved;
}

// SIGCONT po zatrzymaniu, którego nie dało się złapać (SIGSTOP): powłoka mogła przez ten czas
// przestawić terminal i pisać po ekranie.
void continued(int) {
    if (rawly == 0) return;

    const int saved = errno;

    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &raw_kept);

    returned = 1;
    widened(0);

    errno = saved;
}

void drained(int from) {
    unsigned char room[64];

    while (::read(from, room, sizeof(room)) > 0) {
    }
}

std::optional<ui::Size> measured() {
    winsize told{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) != 0 || told.ws_col == 0 || told.ws_row == 0)
        return std::nullopt;

    return ui::Size{told.ws_col, told.ws_row};
}

#endif

// **Prawdziwa konsola procesu — jedna.** Terminal jest jeden, więc i właściciel jego trybu jest
// jeden: dwa obiekty liczyłyby włączenia trybu surowego osobno, każdy z własnym zapamiętanym
// stanem, i drugi przywracałby to, co pierwszy dopiero założył. Wszystko, co biblioteka mówi
// terminalowi i od niego słyszy, idzie tędy — dlatego próby podstawiają w tym miejscu atrapę
// i sprawdzają rozmowę co do bajtu, bez terminala i bez przestawiania środowiska procesu.
class Real final : public Console {
public:
    Real() = default;

    // Konsola porzucona w trybie surowym zostawiłaby terminal bez echa i bez Entera.
    ~Real() override {
        if (depth_ > 0) {
            depth_ = 1;

            raw(false);
        }

#ifndef _WIN32

        stirred = -1;

        if (heard_ >= 0) ::close(heard_);
        if (rung_ >= 0) ::close(rung_);

#endif
    }

    Real(const Real&) = delete;
    Real& operator=(const Real&) = delete;
    Real(Real&&) = delete;
    Real& operator=(Real&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "console"; }

    // Pusta wartość to co innego niż brak zmiennej, ale rozstrzyga o tym czytający: `NO_COLOR=`
    // według swojej umowy nie gasi niczego, a `TERM=` znaczy tyle, co brak.
    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override {
        const std::string key{name};

        const char* told = std::getenv(key.c_str());

        if (told == nullptr) return std::nullopt;

        return std::string{told};
    }

    [[nodiscard]] bool lively() const override {
#ifdef _WIN32

        DWORD mode = 0;

        return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0 &&
               GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;

#else

        return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1;

#endif
    }

    // Na Windows nie ma czego zgadywać ze środowiska: albo konsola przyjmuje sekwencje ucieczki,
    // albo nie. Ustawiamy bit, **odczytujemy z powrotem** — bo starsze conhosty potrafią przyjąć
    // wywołanie i nic nie zrobić — i przywracamy stan. Konsola, która przyjęła VT, ma pełną paletę;
    // ta, która nie przyjęła, ma szesnaście kolorów z czasów przed nią.
    [[nodiscard]] bool vt() const override {
#ifdef _WIN32

        if (forced) return false;

        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);

        DWORD was = 0;
        if (GetConsoleMode(output, &was) == 0) return false;

        if ((was & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) return true;

        if (SetConsoleMode(output, was | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) return false;

        DWORD now = 0;
        const bool stuck =
            GetConsoleMode(output, &now) != 0 && (now & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;

        SetConsoleMode(output, was);

        return stuck;

#else

        return false;

#endif
    }

    // Zapamiętujemy, co oddaliśmy, bo słuchanie na Windows porównuje z tym bieżący pomiar
    // i wraca, gdy się różni.
    [[nodiscard]] std::optional<ui::Size> size() const override {
        const std::optional<ui::Size> made = measured();

        told_ = made;

#ifdef _WIN32

        if (legacy_ != nullptr && made.has_value()) legacy_->resized(*made);

#endif

        return made;
    }

    [[nodiscard]] std::chrono::milliseconds double_click() const override {
#ifdef _WIN32

        const UINT told = GetDoubleClickTime();

        if (told > 0) return std::chrono::milliseconds{static_cast<int>(told)};

#endif

        return deciding;
    }

    // Tryb surowy — jeden, ten, w którym rysujemy. Bez echa i bez trybu wierszowego, bo inaczej
    // klawisze i odpowiedzi terminala czekałyby na Enter; bez sygnałów i bez XON, bo Ctrl+C
    // i Ctrl+S są dla programu zwykłymi klawiszami; bez zamiany CR na LF, bo Enter ma przyjść
    // jako Enter. Na Windows dochodzi strona kodowa: konsola z ustawieniem innym niż UTF-8 liczy
    // każdy bajt jako osobną kolumnę — znak chiński mierzy się wtedy na trzy — a pomiar musi się
    // odbyć w tym samym kodowaniu, w którym potem rysujemy. Zawijanie gasimy, bo klatka stawia
    // znak także w ostatniej kolumnie.
    //
    // **Włączenia się zliczają**: przywraca dopiero wyłączenie ostatnie, więc badanie w środku
    // `Term` nie zdejmuje trybu, który `Term` założył wcześniej. Razem z trybem zakładamy
    // i zdejmujemy obsługę SIGWINCH — dopóki nie rysujemy, zmiana rozmiaru nas nie obchodzi.
    void raw(bool on) override {
        if (on) {
            if (depth_++ == 0) entered();

            return;
        }

        if (depth_ == 0) return;

        if (--depth_ == 0) left();
    }

    // Czy od ostatniego pytania program był zatrzymany i wznowiony — wtedy terminal trzeba ubrać
    // i narysować od nowa. Pytanie zdejmuje znak.
    [[nodiscard]] bool resumed() override {
#ifdef _WIN32
        return false;
#else
        if (returned == 0) return false;

        returned = 0;

        return true;
#endif
    }

    // Bajty prosto na wyjście. **Nie ma tu buforowania i nie ma obsługi błędu.** Krótki zapis
    // dobieramy w pętli, a zapis, który się nie udał, milknie: nie ma komu tego zgłosić, bo na
    // drugim końcu jest terminal, a nie plik.
    void write(std::string_view bytes) override {
#ifdef _WIN32

        if (legacy_ != nullptr) {
            legacy_->written(bytes);

            return;
        }

        DWORD given = 0;

        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()),
                  &given, nullptr);

#else

        std::size_t put = 0;

        while (put < bytes.size()) {
            const ssize_t given = ::write(STDOUT_FILENO, bytes.data() + put, bytes.size() - put);

            if (given <= 0) return;

            put += static_cast<std::size_t>(given);
        }

#endif
    }

    // Pustka znaczy „nie ma czego czytać, rozejrzyj się": minął czas, zadzwonił dzwonek albo
    // zmienił się rozmiar. Wołający i tak sprawdza wszystkie trzy, więc powodu nie oddajemy.
    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds> patience,
                                   const Bell* also) override {
        std::string made;

#ifdef _WIN32

        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

        // Odpowiedzi na sondę są już gotowe — przyszły z zapisu pytań.
        if (legacy_ != nullptr) {
            made = legacy_->answered();

            if (!made.empty()) return made;
        }

        const auto began = std::chrono::steady_clock::now();

        for (;;) {
            std::chrono::milliseconds slice = glancing;

            if (patience.has_value()) {
                const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - began);

                const std::chrono::milliseconds left = *patience - spent;

                if (left.count() <= 0) return made;

                slice = std::min(slice, left);
            }

            HANDLE watched[2] = {input, also != nullptr ? static_cast<HANDLE>(also->waited()) : nullptr};

            const DWORD many = also != nullptr ? 2 : 1;

            const DWORD ready =
                WaitForMultipleObjects(many, watched, FALSE, static_cast<DWORD>(slice.count()));

            if (ready == WAIT_OBJECT_0 + 1) return made;

            if (ready == WAIT_OBJECT_0 && legacy_ != nullptr) {
                bool stirred = false;

                made = legacy_->heard(input, stirred);

                if (!made.empty() || stirred) break;
            } else if (ready == WAIT_OBJECT_0 && speaking(input)) {
                DWORD mode = 0;

                // **Konsola czytana szeroko.** `ReadFile` przy stronie kodowej 65001 oddawał w części
                // conhostów zero zamiast każdego znaku spoza ASCII — „ą" z klawiatury przychodziło
                // jako pusty bajt. `ReadConsoleW` daje UTF-16, które przekładamy sami; połówkę pary
                // zastępczej z końca odczytu trzymamy do następnego.
                if (GetConsoleMode(input, &mode) != 0) {
                    wchar_t room[512];
                    DWORD given = 0;

                    if (ReadConsoleW(input, room, sizeof(room) / sizeof(room[0]), &given, nullptr) == 0 || given == 0)
                        return made;

                    std::wstring wide = half_;

                    half_.clear();

                    wide.append(room, given);

                    if (!wide.empty() && wide.back() >= 0xD800 && wide.back() <= 0xDBFF) {
                        half_ = wide.back();
                        wide.pop_back();
                    }

                    made = to_string(wide);

                    break;
                }

                char room[1024];
                DWORD given = 0;

                // Wejście przekierowane i skończone budziłoby czekanie bez przerwy, więc pusty
                // odczyt kończy słuchanie, a wołający się rozgląda.
                if (ReadFile(input, room, sizeof(room), &given, nullptr) == 0 || given == 0)
                    return made;

                made.assign(room, static_cast<std::size_t>(given));

                break;
            }

            if (ready == WAIT_FAILED) return made;

            if (told_.has_value() && measured() != told_) return made;
        }

#else

        prepared();

        pollfd watched[3] = {{STDIN_FILENO, POLLIN, 0},
                             {heard_, POLLIN, 0},
                             {also != nullptr ? also->waited() : -1, POLLIN, 0}};

        const int ready = ::poll(watched, 3, patience.has_value() ? static_cast<int>(patience->count()) : -1);

        if (ready <= 0) return made;

        if ((watched[1].revents & POLLIN) != 0) {
            drained(heard_);

            return made;
        }

        if ((watched[0].revents & POLLIN) == 0) return made;

        char room[1024];

        const ssize_t given = ::read(STDIN_FILENO, room, sizeof(room));

        if (given > 0) made.assign(room, static_cast<std::size_t>(given));

#endif

        if (made.size() > brim) made.resize(brim);

        return made;
    }

private:
    void entered() {
#ifdef _WIN32

        input_ = GetStdHandle(STD_INPUT_HANDLE);
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);

        if (GetConsoleMode(input_, &was_in_) == 0 || GetConsoleMode(output_, &was_out_) == 0)
            return;

        was_read_ = GetConsoleCP();
        was_said_ = GetConsoleOutputCP();

        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);

        // **Bez szybkiej edycji.** W konsoli Windows 10 (conhost) kliknięcie zaczyna wtedy zaznaczanie
        // zamiast trafić do programu, a dopóki zaznaczenie trwa, każdy zapis do konsoli stoi — cały
        // interfejs zamarza. Bitu szybkiej edycji nie da się ruszyć bez `ENABLE_EXTENDED_FLAGS`.
        // Windows Terminal tego trybu nie ma, więc tam nic się nie zmienia.
        DWORD raw = was_in_;
        raw &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                                   ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE);
        raw |= ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;

        DWORD vt = was_out_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        vt &= ~static_cast<DWORD>(ENABLE_WRAP_AT_EOL_OUTPUT);

        // **Tryb VT albo tłumaczenie — rozstrzyga pierwsze wejście i zostaje do końca.** Konsola,
        // która bitu VT nie zna, odrzuca całe wywołanie z nim, także na wejściu — dlatego dla niej
        // tryb wejścia idzie bez VT, za to z myszą: rekordy myszy zamieniamy sami.
        if (legacy_ == nullptr && !modern_) {
            DWORD now = 0;

            modern_ = !forced && SetConsoleMode(output_, vt) != 0 && GetConsoleMode(output_, &now) != 0 &&
                      (now & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;

            if (!modern_) legacy_ = std::make_unique<Legacy>(output_);
        }

        if (legacy_ != nullptr) {
            SetConsoleMode(input_, raw | ENABLE_MOUSE_INPUT);

            legacy_->fonted();
        } else {
            SetConsoleMode(input_, raw | ENABLE_VIRTUAL_TERMINAL_INPUT);
            SetConsoleMode(output_, vt);
        }

        held_ = true;

#else

        prepared();

        was_winch_ = std::signal(SIGWINCH, widened);

        if (tcgetattr(STDIN_FILENO, &was_) != 0) return;

        termios raw = was_;

        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;

        cooked_kept = was_;
        raw_kept = raw;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;

        rawly = 1;

        // Tylko tam, gdzie sygnał miał reakcję domyślną. Program, który ustawił własną obsługę albo
        // sygnał zignorował, bierze odpowiedzialność na siebie — nie przykrywamy jej.
        for (std::size_t at = 0; at < parting.size(); ++at) taken_[at] = defaulted(parting[at], before_[at]);

        held_ = true;

#endif
    }

    void left() {
#ifndef _WIN32

        std::signal(SIGWINCH, was_winch_ == SIG_ERR || was_winch_ == nullptr ? SIG_DFL : was_winch_);

#endif

        if (!held_) return;

        held_ = false;

#ifndef _WIN32

        rawly = 0;

        for (std::size_t at = 0; at < parting.size(); ++at)
            if (taken_[at]) (void)::sigaction(parting[at], &before_[at], nullptr);

#endif

#ifdef _WIN32

        if (legacy_ != nullptr) {
            legacy_->parted();
            legacy_->unfonted();
        }

        SetConsoleMode(input_, was_in_ | ENABLE_EXTENDED_FLAGS);
        SetConsoleMode(output_, was_out_);

        if (was_read_ != 0) SetConsoleCP(was_read_);
        if (was_said_ != 0) SetConsoleOutputCP(was_said_);

#else

        tcsetattr(STDIN_FILENO, TCSANOW, &was_);

#endif
    }

#ifndef _WIN32

    // Rurka sygnału powstaje przy pierwszej potrzebie i żyje tyle, co konsola, czyli do końca
    // procesu. Nie trafia do programów potomnych.
    void prepared() {
        if (heard_ >= 0) return;

        int both[2] = {-1, -1};

        if (!sealed::piped(both, true)) return;

        heard_ = both[0];
        rung_ = both[1];

        stirred = rung_;
    }

#endif

    int depth_ = 0;
    bool held_ = false;

    mutable std::optional<ui::Size> told_;

#ifdef _WIN32
    std::unique_ptr<Legacy> legacy_;
    bool modern_ = false;
    std::wstring half_;
    HANDLE input_ = nullptr;
    HANDLE output_ = nullptr;
    DWORD was_in_ = 0;
    DWORD was_out_ = 0;
    UINT was_read_ = 0;
    UINT was_said_ = 0;
#else
    static constexpr std::array<int, 6> parting{SIGTERM, SIGHUP, SIGQUIT, SIGINT, SIGTSTP, SIGCONT};

    // Obsługa pod sygnał z `parting`: kończące — `parted`, zatrzymanie — `paused`, wznowienie —
    // `continued`.
    static bool defaulted(int which, struct sigaction& was) {
        if (::sigaction(which, nullptr, &was) != 0 || was.sa_handler != SIG_DFL) return false;

        handled(which, which == SIGTSTP ? paused : which == SIGCONT ? continued : parted, nullptr);

        return true;
    }

    std::array<struct sigaction, 6> before_{};
    std::array<bool, 6> taken_{};
    termios was_{};
    void (*was_winch_)(int) = nullptr;
    int heard_ = -1;
    int rung_ = -1;
#endif
};

}

Seam<Console> console() {
    static const Seam<Console> one{std::make_shared<Real>()};

    return one;
}

}

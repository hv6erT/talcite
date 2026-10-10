#include "talcite/console.hpp"

#include "talcite/encoding.hpp"

#include "conhost.hpp"
#include "sealed.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
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

// The most that one listen returns. Terminal replies fit in a kilobyte; more is a paste or garbage,
// and there is no reason to keep it whole.
constexpr std::size_t brim = 64 * 1024;

// The silence after which a first click stops waiting for a second one. No browser specification
// fixes it: implementations take the value from the system, and GTK has this much by default. On
// Windows the user's setting is asked for directly; under POSIX there is no one to ask, and in
// remote work the setting would sit on the wrong machine anyway.
constexpr std::chrono::milliseconds deciding{400};

#ifdef _WIN32

// Windows puts a size change into the input queue as a record, but only when the buffer changes:
// in a conhost with scrollback the window grows while the buffer stays. So listening looks at the
// size this often, regardless of records.
constexpr std::chrono::milliseconds glancing{100};

// The trace of listening, for finding out on a real Windows console why the program waits. It is
// written only when the environment variable TALCITE_TRACE names a file; the file is opened once,
// appended to, and every line is flushed at once, so the trace survives the program being killed.
// Each line starts with the milliseconds since the first line. Without the variable this costs one
// look at the environment.
FILE* traced() {
    static FILE* const kept = [] {
        wchar_t named[MAX_PATH];

        const DWORD many = GetEnvironmentVariableW(L"TALCITE_TRACE", named, MAX_PATH);

        if (many == 0 || many >= MAX_PATH) return static_cast<FILE*>(nullptr);

        return _wfopen(named, L"a");
    }();

    return kept;
}

// Writes one line of the trace, if there is one.
void noted(const std::string& line) {
    FILE* const into = traced();

    if (into == nullptr) return;

    static const auto began = std::chrono::steady_clock::now();

    const auto spent =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();

    std::fprintf(into, "%10lld %s\n", static_cast<long long>(spent), line.c_str());
    std::fflush(into);
}

// One input record in a few characters for the trace: a key with its state, virtual key and
// character, a mouse record with its flags and buttons, or the kind of a silent record.
std::string recorded(const INPUT_RECORD& one) {
    char room[96];

    switch (one.EventType) {
        case KEY_EVENT:
            std::snprintf(room, sizeof(room), "key(%s vk=%u ch=%u rep=%u)",
                          one.Event.KeyEvent.bKeyDown != 0 ? "down" : "up",
                          static_cast<unsigned>(one.Event.KeyEvent.wVirtualKeyCode),
                          static_cast<unsigned>(one.Event.KeyEvent.uChar.UnicodeChar),
                          static_cast<unsigned>(one.Event.KeyEvent.wRepeatCount));
            break;
        case MOUSE_EVENT:
            std::snprintf(room, sizeof(room), "mouse(flags=%lu buttons=%lu)",
                          static_cast<unsigned long>(one.Event.MouseEvent.dwEventFlags),
                          static_cast<unsigned long>(one.Event.MouseEvent.dwButtonState));
            break;
        case WINDOW_BUFFER_SIZE_EVENT: std::snprintf(room, sizeof(room), "size"); break;
        case FOCUS_EVENT: std::snprintf(room, sizeof(room), "focus"); break;
        case MENU_EVENT: std::snprintf(room, sizeof(room), "menu"); break;
        default: std::snprintf(room, sizeof(room), "type=%u", static_cast<unsigned>(one.EventType)); break;
    }

    return room;
}

// Modifiers and locks alone turn into no byte, so waiting for them would end with standing in
// ReadFile until the next real key.
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

// The console queue also wakes the wait with records that virtual terminal mode does not turn into
// bytes: a size change, focus, a key release. ReadFile would eat them and block until the next
// key. So the queue is looked at first: if there is nothing that speaks, the silent records are
// removed here.
bool speaking(HANDLE input) {
    for (int again = 0; again < 16; ++again) {
        DWORD many = 0;

        // Not a console: the input is redirected. Then it is read directly.
        if (GetNumberOfConsoleInputEvents(input, &many) == 0) return true;

        if (many == 0) return false;

        INPUT_RECORD seen[64];
        DWORD given = 0;
        const DWORD asked = many < 64 ? many : 64;

        if (PeekConsoleInputW(input, seen, asked, &given) == 0 || given == 0) {
            noted("speaking: " + std::to_string(many) + " waiting, peek gave none");

            return true;
        }

        if (traced() != nullptr) {
            std::string line = "speaking: peeked";

            for (DWORD at = 0; at < given; ++at) line += ' ' + recorded(seen[at]);

            noted(line);
        }

        // Mouse records are left for reading, even when they need not produce anything: ReadFile
        // turns them into a sequence when mouse tracking is on.
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

// The size of the window, not of the buffer; the buffer is often taller by the scrollback. The
// buffer active right now is measured: standard output points to the one we started from, and the
// alternate screen is another buffer, a new one after every `?1049h`. That is why CONOUT$ is
// opened at every measurement instead of keeping a handle from the start, which after returning
// from another program would point to a buffer no longer active.
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

// A console that does not accept VT: Windows 7 and 8.1, the first releases of 10 and the "legacy
// console" from the settings. Building with TALCITE_LEGACY_CONSOLE forces this path on a new
// console too, so that it can be checked without an old system.
#ifdef TALCITE_LEGACY_CONSOLE
constexpr bool forced = true;
#else
constexpr bool forced = false;
#endif

// The glue between `conhost::Screen` and the console buffer. Writing feeds the grid and copies the
// changed rows through WriteConsoleOutputW; the alternate screen is a separate buffer of the
// window's size, so leaving it gives back the main buffer as it was, scrollback included, without
// rewriting a single cell. Input is read as records and turned into the bytes a console with VT
// would give.
class Legacy {
public:
    // Builds the grid from the console's buffer: its size, colors and palette, and the cursor
    // where the shell left it.
    explicit Legacy(HANDLE output) : original_(output), screen_(opened(output)) {
        CONSOLE_SCREEN_BUFFER_INFO told{};

        if (GetConsoleScreenBufferInfo(output, &told) != 0) {
            // The cursor stands where the shell left it; the grid is told so the way a terminal
            // would be told: with a jump.
            const int x = told.dwCursorPosition.X - told.srWindow.Left;
            const int y = told.dwCursorPosition.Y - told.srWindow.Top;

            screen_.fed("\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H");
        }

        (void)screen_.dirty();
    }

    // Returns to the main buffer and gives back the original font.
    ~Legacy() {
        parted();
        unfonted();
    }

    Legacy(const Legacy&) = delete;
    Legacy& operator=(const Legacy&) = delete;
    Legacy(Legacy&&) = delete;
    Legacy& operator=(Legacy&&) = delete;

    // Feeds what `Term` writes into the grid and copies the changes to the console.
    void written(std::string_view bytes) {
        screen_.fed(bytes);

        flushed();
    }

    // The grid's replies to the probe's questions, waiting to be read as input.
    [[nodiscard]] std::string answered() { return screen_.answers(); }

    // The waiting records, as bytes. `stirred` says that the buffer changed; the caller measures
    // the window anew anyway.
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

                // A character typed with Alt on the numeric keypad comes with the release of Alt.
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

    // The window has a new size. The own buffer follows it, so that no scroll bars appear.
    void resized(ui::Size size) {
        if (size == screen_.size()) return;

        if (own_ != nullptr)
            SetConsoleScreenBufferSize(own_, COORD{static_cast<SHORT>(size.wide), static_cast<SHORT>(size.high)});

        screen_.resize(size);
    }

    // A raster font knows only the OEM code page: the console would show frames, arrows and
    // letters outside it as question marks. For the time of the program a TrueType font of the same
    // height is taken (Consolas, or Lucida Console where it is missing), and the old one is given
    // back at exit. When neither is accepted, the raster font stays; the program's help says what
    // to choose in the window properties then.
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

            // Neither one took, and a failed attempt may have left another raster font: the one
            // from before comes back.
            if (!truetyped(original_)) unfonted();
        }

        CONSOLE_FONT_INFO font{};

        if (GetCurrentConsoleFont(original_, FALSE, &font) != 0 && font.dwFontSize.X > 0 && font.dwFontSize.Y > 0)
            screen_.pixels({font.dwFontSize.X, font.dwFontSize.Y});
    }

    // The old font comes back, at program exit and before the console is handed to a child
    // program.
    void unfonted() {
        if (!was_.has_value()) return;

        SetCurrentConsoleFontEx(original_, FALSE, &*was_);

        was_.reset();
    }

    // Back to the main buffer, in case the application did not leave the alternate screen itself.
    void parted() {
        if (own_ == nullptr) return;

        SetConsoleActiveScreenBuffer(original_);
        CloseHandle(own_);

        own_ = nullptr;
    }

private:
    // A grid in the console's colors and palette, of the window's size; when the console says
    // nothing, 80 by 25 in light gray on black.
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

    // The buffer being drawn on: our own alternate one, or the original.
    [[nodiscard]] HANDLE active() const { return own_ != nullptr ? own_ : original_; }

    // Whether the buffer's current font is a TrueType one.
    [[nodiscard]] static bool truetyped(HANDLE buffer) {
        CONSOLE_FONT_INFOEX now{};
        now.cbSize = sizeof now;

        return GetCurrentConsoleFontEx(buffer, FALSE, &now) != 0 && (now.FontFamily & TMPF_TRUETYPE) != 0;
    }

    // The top left corner of the window in the buffer. Our own buffer has it at zero; the main one
    // wherever it was scrolled to.
    [[nodiscard]] ui::Spot corner() const {
        CONSOLE_SCREEN_BUFFER_INFO told{};

        if (GetConsoleScreenBufferInfo(active(), &told) == 0) return {};

        return {told.srWindow.Left, told.srWindow.Top};
    }

    // Switches buffers when the grid entered or left the alternate screen, copies the changed rows,
    // and carries over the cursor, its visibility and the window title.
    void flushed() {
        if (screen_.apart() && own_ == nullptr) {
            own_ = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);

            if (own_ == INVALID_HANDLE_VALUE) own_ = nullptr;

            if (own_ != nullptr) {
                const ui::Size size = screen_.size();

                // The alternate buffer gets the font set for the main one: SetCurrentConsoleFontEx
                // takes a buffer handle, not a window, so the new buffer is not assumed to inherit
                // it.
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

// The pipe for the size change signal. A signal handler may only write to a descriptor, and
// listening waits for it in the same poll() as for the keyboard, so a size change wakes the wait
// reliably, even when it came just before it.
volatile std::sig_atomic_t stirred = -1;

// The SIGWINCH handler: one byte into the pipe, with errno preserved. Also called directly to wake
// listening after a resume.
void widened(int) {
    if (stirred < 0) return;

    const int saved = errno;
    const unsigned char one = 1;
    const ssize_t wrote = ::write(stirred, &one, 1);
    (void)wrote;
    errno = saved;
}

// Signals after which the terminal has to go back to the person. Without this, a program killed
// by SIGTERM or SIGHUP would leave the terminal in raw mode, on the alternate screen, with the
// mouse on and without a cursor, and the shell underneath would be unusable until reset. A program
// stopped from outside (SIGTSTP) would leave the same to the shell, and once resumed (SIGCONT)
// would draw over what the shell drew.
//
// A signal handler may only do what is safe in a signal handler: `tcsetattr`, `write`,
// `sigaction`, `raise`. Hence the state in variables prepared in advance (the mode from before and
// the raw mode) and a farewell with a fixed string that turns off everything `Term` can turn on;
// turning off something that was not turned on does nothing.
termios cooked_kept{};
termios raw_kept{};
volatile std::sig_atomic_t rawly = 0;
volatile std::sig_atomic_t returned = 0;

constexpr char farewell[] = "\x1b[?2004l\x1b[?1003l\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l";

// Gives the terminal back: the mode from before and the farewell string. Safe in a signal handler.
void restored() {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &cooked_kept);

    const ssize_t wrote = ::write(STDOUT_FILENO, farewell, sizeof farewell - 1);
    (void)wrote;
}

// Unblocks the signal in this thread, so that raising it from its own handler takes effect.
void unblocked(int which) {
    sigset_t one;

    sigemptyset(&one);
    sigaddset(&one, which);

    (void)::pthread_sigmask(SIG_UNBLOCK, &one, nullptr);
}

// Installs `how` for the signal, with SA_RESTART; `was` receives the previous action if given.
void handled(int which, void (*how)(int), struct sigaction* was) {
    struct sigaction made {};

    made.sa_handler = how;
    made.sa_flags = SA_RESTART;
    sigemptyset(&made.sa_mask);

    (void)::sigaction(which, &made, was);
}

// SIGTERM, SIGHUP, SIGQUIT: the terminal comes back, and the signal arrives once more, now with
// the default action, so the exit code and the reason for the end are what the sender expects.
void parted(int which) {
    restored();

    handled(which, SIG_DFL, nullptr);
    unblocked(which);

    (void)::raise(which);
}

// SIGTSTP: the terminal goes back to the shell, and the program really stops, through the default
// action raised from here. Resuming returns to the same place: raw mode again and a sign for
// listening that the terminal has to be set up and everything drawn once more.
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

// SIGCONT after a stop that could not be caught (SIGSTOP): in the meantime the shell may have
// changed the terminal settings and written over the screen.
void continued(int) {
    if (rawly == 0) return;

    const int saved = errno;

    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &raw_kept);

    returned = 1;
    widened(0);

    errno = saved;
}

// Reads the pipe empty, so that the next poll() waits again.
void drained(int from) {
    unsigned char room[64];

    while (::read(from, room, sizeof(room)) > 0) {
    }
}

// The terminal size from TIOCGWINSZ, or nothing when the output is not a terminal.
std::optional<ui::Size> measured() {
    winsize told{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) != 0 || told.ws_col == 0 || told.ws_row == 0)
        return std::nullopt;

    return ui::Size{told.ws_col, told.ws_row};
}

#endif

// The real console of the process; there is one. The terminal is one, so the owner of its mode is
// one too: two objects would count raw mode activations separately, each with its own remembered
// state, and the second would restore what the first had only just set. Everything the library
// says to the terminal and hears from it goes through here, which is why trials substitute a fake
// at this point and check the conversation byte by byte, without a terminal and without changing
// the process environment.
class Real final : public Console {
public:
    Real() = default;

    // A console abandoned in raw mode would leave the terminal without echo and without Enter, so
    // the destructor leaves raw mode whatever the depth; it also closes the signal pipe.
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

    // The name of this console, for diagnostics.
    [[nodiscard]] std::string_view name() const override { return "console"; }

    // An environment variable, or nothing when it is not set. An empty value is not the same as a
    // missing variable, but the reader decides: `NO_COLOR=` by its own convention turns nothing
    // off, and `TERM=` means as much as a missing one.
    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override {
        const std::string key{name};

        const char* told = std::getenv(key.c_str());

        if (told == nullptr) return std::nullopt;

        return std::string{told};
    }

    // Whether both input and output are a console or a terminal, not a file or a pipe.
    [[nodiscard]] bool lively() const override {
#ifdef _WIN32

        DWORD mode = 0;

        return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0 &&
               GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;

#else

        return isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1;

#endif
    }

    // On Windows there is nothing to guess from the environment: either the console accepts escape
    // sequences or it does not. The bit is set, read back (because older conhosts can accept the
    // call and do nothing), and the state is restored. A console that accepted VT has the full
    // palette; one that did not has the sixteen colors from the time before it. Elsewhere this is
    // always false, and the colors are judged from the environment.
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

    // The terminal size in cells. What was returned is remembered, because listening on Windows
    // compares the current measurement with it and returns when they differ.
    [[nodiscard]] std::optional<ui::Size> size() const override {
        const std::optional<ui::Size> made = measured();

        told_ = made;

#ifdef _WIN32

        if (legacy_ != nullptr && made.has_value()) legacy_->resized(*made);

#endif

        return made;
    }

    // The double-click time: the user's setting on Windows, otherwise the GTK default.
    [[nodiscard]] std::chrono::milliseconds double_click() const override {
#ifdef _WIN32

        const UINT told = GetDoubleClickTime();

        if (told > 0) return std::chrono::milliseconds{static_cast<int>(told)};

#endif

        return deciding;
    }

    // Raw mode, the single one in which drawing happens. No echo and no line mode, because
    // otherwise keys and terminal replies would wait for Enter; no signals and no XON, because
    // Ctrl+C and Ctrl+S are ordinary keys for the program; no CR to LF translation, because Enter
    // has to come as Enter. On Windows the code page joins in: a console set to anything other than
    // UTF-8 counts every byte as a separate column (a Chinese character then measures three), and
    // the measurement must happen in the same encoding that drawing uses later. Wrapping is turned
    // off, because the frame puts a character in the last column too.
    //
    // Activations are counted: only the last deactivation restores, so a probe inside `Term` does
    // not remove the mode `Term` set earlier. The SIGWINCH handler is installed and removed
    // together with the mode; while nothing is drawn, a size change does not matter.
    void raw(bool on) override {
        if (on) {
            if (depth_++ == 0) entered();

            return;
        }

        if (depth_ == 0) return;

        if (--depth_ == 0) left();
    }

    // Whether the program was stopped and resumed since the last question; then the terminal has
    // to be set up and drawn anew. The question clears the sign.
    [[nodiscard]] bool resumed() override {
#ifdef _WIN32
        return false;
#else
        if (returned == 0) return false;

        returned = 0;

        return true;
#endif
    }

    // Bytes straight to the output. There is no buffering here and no error handling. A short
    // write is completed in a loop, and a write that failed falls silent: there is no one to
    // report it to, because a terminal is at the other end, not a file.
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

    // Empty means "nothing to read, look around": the time passed, the bell rang or the size
    // changed. The caller checks all three anyway, so the reason is not returned.
    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds> patience,
                                   const Bell* also) override {
        std::string made;

#ifdef _WIN32

        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);

        // The replies to the probe are ready already; they came from writing the questions.
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

            if (ready == WAIT_OBJECT_0 + 1) {
                noted("hear: the bell rang");

                return made;
            }

            if (ready == WAIT_OBJECT_0) noted("hear: input is waiting");

            if (ready == WAIT_OBJECT_0 && legacy_ != nullptr) {
                bool stirred = false;

                made = legacy_->heard(input, stirred);

                if (!made.empty() || stirred) break;
            } else if (ready == WAIT_OBJECT_0 && speaking(input)) {
                DWORD mode = 0;

                // A console read as wide text. In some conhosts `ReadFile` with code page 65001
                // returns a zero in place of every non-ASCII character, so "ą" from the keyboard
                // would arrive as an empty byte. `ReadConsoleW` gives UTF-16, which is converted
                // here; the half of a surrogate pair at the end of a read is kept for the next one.
                if (GetConsoleMode(input, &mode) != 0) {
                    wchar_t room[512];
                    DWORD given = 0;

                    noted("hear: ReadConsoleW begins");

                    const auto asked_at = std::chrono::steady_clock::now();

                    const BOOL read = ReadConsoleW(input, room, sizeof(room) / sizeof(room[0]), &given, nullptr);

                    noted("hear: ReadConsoleW gave " + std::to_string(read != 0 ? given : 0) + " after " +
                          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - asked_at)
                                             .count()) +
                          " ms");

                    if (read == 0 || given == 0) return made;

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

                // Redirected input that has ended would wake the wait without pause, so an empty
                // read ends listening, and the caller looks around.
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
    // Enters raw mode: on Windows the UTF-8 code pages and console modes, choosing VT or the
    // translation; elsewhere termios, SIGWINCH and the signals that give the terminal back.
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

        // Without quick edit. In the Windows 10 console (conhost) a click would otherwise start a
        // selection instead of reaching the program, and while the selection lasts every write to
        // the console blocks, so the whole interface freezes. The quick edit bit cannot be changed
        // without `ENABLE_EXTENDED_FLAGS`. Windows Terminal does not have this mode, so nothing
        // changes there.
        DWORD raw = was_in_;
        raw &= ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                                   ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE);
        raw |= ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;

        DWORD vt = was_out_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        vt &= ~static_cast<DWORD>(ENABLE_WRAP_AT_EOL_OUTPUT);

        // VT mode or the translation: the first entry decides, and it stays until the end. A
        // console that does not know the VT bit rejects the whole call with it, on input too, so
        // for such a console the input mode goes without VT but with the mouse: mouse records are
        // translated here.
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

        // IEXTEN goes too, as in cfmakeraw: on the BSDs and macOS it keeps Ctrl+V (literal next)
        // and Ctrl+O (discard) for the terminal driver even without ICANON, so the program would
        // never see them.
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;

        cooked_kept = was_;
        raw_kept = raw;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;

        rawly = 1;

        // Only where the signal had its default action. A program that installed its own handler or
        // ignored the signal takes the responsibility itself, and that is not covered over.
        for (std::size_t at = 0; at < parting.size(); ++at) taken_[at] = defaulted(parting[at], before_[at]);

        held_ = true;

#endif
    }

    // Leaves raw mode: restores SIGWINCH, the signal handlers, the console modes and code pages or
    // the termios state from before.
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

    // The signal pipe is created at first need and lives as long as the console, that is, until
    // the process ends. It does not leak into child programs.
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

    // Installs the handler for a signal from `parting`, but only over the default action: ending
    // signals get `parted`, the stop gets `paused`, the resume gets `continued`. False when the
    // signal had a handler of its own; `was` keeps the previous action.
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

// The process console, shared by everyone who asks.
Seam<Console> console() {
    static const Seam<Console> one{std::make_shared<Real>()};

    return one;
}

}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/area.hpp"

// **Konsola bez sekwencji VT** (Windows 7, 8, 8.1 i pierwsze wydania 10 — conhost sprzed 1511 oraz
// „starsza konsola" z ustawień). Biblioteka mówi z terminalem sekwencjami VT i słucha sekwencji VT;
// tam, gdzie konsola ich nie zna, tłumaczymy na brzegu, w `Console`: `Term`, sonda możliwości
// i dekoder są te same i nie wiedzą, z czym rozmawiają.
//
// Ten plik to część przenośna tłumaczenia, bez Win32, więc sprawdza się na każdym systemie:
//
// - `Screen` — mały terminal: czyta to, co pisze `Term` (tekst w UTF-8, CUP, SGR, EL, ED, tryby DEC,
//   tytuł w OSC), trzyma siatkę komórek w szesnastu kolorach konsoli i odpowiada na pytania sondy
//   (DECRQM, DSR, DA, OSC 11, 16t) tak, jak odpowiedziałby terminal, który umie dokładnie to, co
//   umie tłumaczenie. Klej w `console.cpp` przepisuje zmienione wiersze do konsoli.
// - `typed` i `Pointing` — w drugą stronę: rekordy klawiatury i myszy konsoli na te same bajty, które
//   conhost z trybem VT dałby dekoderowi.
namespace talcite::conhost {

// Komórka w kolorach konsoli: barwa i tło jako indeksy palety konsoli (bity BGR i jasność, jak
// FOREGROUND_BLUE…), znak, podkreślenie i to, czy komórka jest prawą połową znaku na dwie kratki.
struct Cell {
    char32_t glyph = U' ';
    std::uint8_t ink = 7;
    std::uint8_t paper = 0;
    bool under = false;
    bool trailing = false;

    friend bool operator==(const Cell&, const Cell&) = default;
};

// Śledzenie myszy włączone przez aplikację (DECSET 1000, 1002, 1003, 1006).
struct Tracking {
    bool clicks = false;
    bool drags = false;
    bool moves = false;
    bool sgr = false;

    friend bool operator==(const Tracking&, const Tracking&) = default;
};

struct Rgb {
    int red = 0;
    int green = 0;
    int blue = 0;

    friend bool operator==(const Rgb&, const Rgb&) = default;
};

// Szesnaście kolorów konsoli w jej kolejności indeksów.
using Palette = std::array<Rgb, 16>;

// Paleta, z którą conhost przychodził do Windows 10 1709 — gdy konsola nie powie własnej.
[[nodiscard]] Palette classic();

// Indeks palety najbliższy kolorowi RGB — dla SGR 38;2 i 38;5.
[[nodiscard]] std::uint8_t nearest(const Palette& among, int red, int green, int blue);

class Screen {
public:
    Screen(ui::Size size, std::uint8_t ink, std::uint8_t paper, Palette palette = classic());

    void fed(std::string_view bytes);

    void resize(ui::Size size);

    // Rozmiar kratki w pikselach, jeśli konsola go zna — odpowiedź na 16t.
    void pixels(ui::Size cell) { pixels_ = cell; }

    [[nodiscard]] ui::Size size() const { return size_; }

    [[nodiscard]] const Cell& at(int x, int y) const;

    [[nodiscard]] ui::Spot cursor() const { return cursor_; }

    [[nodiscard]] bool shown() const { return shown_; }

    [[nodiscard]] bool apart() const { return apart_; }

    [[nodiscard]] const Tracking& tracking() const { return tracking_; }

    // Wiersze zmienione od ostatniego pytania, rosnąco; pytanie je zdejmuje.
    [[nodiscard]] std::vector<int> dirty();

    // Odpowiedzi dla sondy — bajty, które terminal odesłałby na wejście; pytanie je zdejmuje.
    [[nodiscard]] std::string answers();

    // Nowy tytuł okna, jeśli przyszedł; pytanie go zdejmuje.
    [[nodiscard]] std::optional<std::string> title();

private:
    enum class Mode : std::uint8_t { ground, escape, csi, text, swallowed, ending };

    struct Pen {
        std::uint8_t ink = 7;
        std::uint8_t paper = 0;
        bool bold = false;
        bool under = false;
        bool reverse = false;
    };

    void taken(unsigned char byte);
    void escaped(unsigned char byte);
    void gathered(unsigned char byte);
    void stringed(unsigned char byte);
    void put(char32_t one);
    void control(unsigned char one);
    void dispatched(char last);
    void moded(bool on);
    void asked_mode();
    void styled();
    void colored(std::size_t& at, bool foreground);
    void told();
    void lined();
    void erased_line(int how);
    void erased_screen(int how);
    void cleared(int x, int y, int much);
    void written(int x, int y, const Cell& one);
    void touched(int y);
    void switched(bool on, bool cursor);
    [[nodiscard]] Cell blank() const;
    [[nodiscard]] Cell inked(char32_t glyph) const;
    [[nodiscard]] int parameter(std::size_t which, int fallback) const;
    [[nodiscard]] Cell& cell(int x, int y);

    ui::Size size_;
    std::vector<Cell> cells_;
    std::vector<Cell> kept_;
    ui::Spot kept_cursor_{};
    std::vector<bool> dirty_;
    ui::Spot cursor_{};
    bool pending_ = false;
    bool shown_ = true;
    bool wrapping_ = true;
    bool apart_ = false;
    Tracking tracking_;
    Palette palette_;
    std::optional<ui::Size> pixels_;

    std::uint8_t ink_;
    std::uint8_t paper_;
    Pen pen_;

    Mode mode_ = Mode::ground;
    bool lead_osc_ = false;
    std::string string_;
    std::vector<int> parameters_;
    char marker_ = 0;
    char intermediate_ = 0;
    std::size_t length_ = 0;
    char32_t code_ = 0;
    std::size_t left_ = 0;

    std::string answers_;
    std::optional<std::string> title_;
};

// Rekord klawiatury konsoli w postaci przenośnej: kod wirtualnego klawisza, znak UTF-16 (jedna
// jednostka — pary zastępcze przychodzą dwoma rekordami), stan klawiszy sterujących jak w
// `dwControlKeyState`, liczba powtórzeń. Klej podaje tylko naciśnięcia.
struct Pressed {
    std::uint16_t key = 0;
    char16_t unit = 0;
    std::uint32_t state = 0;
    int repeat = 1;
};

// Klawisz na bajty dekodera. `pending` trzyma połówkę pary zastępczej do następnego rekordu.
[[nodiscard]] std::string typed(const Pressed& one, char16_t& pending);

// Rekord myszy w postaci przenośnej: miejsce w oknie (nie w buforze), przyciski jak w `dwButtonState`
// (z kółkiem w starszym słowie), znaczniki jak w `dwEventFlags`, stan klawiszy sterujących.
struct Pointed {
    ui::Spot at{};
    std::uint32_t buttons = 0;
    std::uint32_t flags = 0;
    std::uint32_t state = 0;
};

// Mysz na sekwencje SGR (1006) — z pamięcią przycisków, bo konsola mówi, co jest wciśnięte, a nie co
// się zmieniło.
class Pointing {
public:
    [[nodiscard]] std::string pointed(const Pointed& one, const Tracking& wanted);

private:
    std::uint32_t held_ = 0;
};

}

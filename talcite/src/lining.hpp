#pragma once

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "talcite/encoding.hpp"
#include "talcite/line.hpp"
#include "talcite/style.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Nazwa kompletu znaków w skórce. Grubość dalej wybiera widżet — to jest wybór kształtu, a nie
// wyglądu; skórka mówi tylko, **jakimi znakami** ta grubość jest rysowana.
[[nodiscard]] inline std::string_view named(LineStyle how) {
    switch (how) {
        case LineStyle::twin: return "talcite.line.twin";
        case LineStyle::thick: return "talcite.line.thick";
        default: return "talcite.line.thin";
    }
}

// Trzy komplety znaków ramki w jednym miejscu, bo dwie kopie tej samej tablicy prędzej czy później
// zaczęłyby się różnić. **To już nie jest nagłówek publiczny**: od chwili, gdy znaki przychodzą ze
// skórki, jedynym czytelnikiem tej tablicy jest ona sama — to z niej bierze wartości wbudowane.
// `LineStyle::none` nie ma tu wpisu: kto nie rysuje linii, nie pyta o znaki.
[[nodiscard]] inline Lines drawn(LineStyle how) {
    switch (how) {
        case LineStyle::twin: return {U'╔', U'╗', U'╚', U'╝', U'═', U'║', U'╠', U'╣'};
        case LineStyle::thick: return {U'┏', U'┓', U'┗', U'┛', U'━', U'┃', U'┣', U'┫'};
        default: return {U'┌', U'┐', U'└', U'┘', U'─', U'│', U'├', U'┤'};
    }
}

// Ile kratek zajmuje najszerszy ze znaków — tyle musi mieć bok, na którym któryś z nich stanie.
[[nodiscard]] inline int widest(std::initializer_list<char32_t> ones) {
    int made = 1;

    for (const char32_t one : ones) made = std::max(made, widely(one));

    return made;
}

// Szerokości lewego i prawego boku ramki w kratkach. **Znak dwukolumnowy zabiera dwie**: skórka
// może rysować ramkę znakami CJK, a wtedy bok nie jest już jedną kratką i wnętrze zaczyna się
// dalej. Bok liczy się od najszerszego znaku, który na nim stanie — pionu, narożników i złączki —
// bo węższy znak w szerszym boku po prostu zostawia obok siebie tło, a szerszy w węższym wyszedłby
// poza ramkę.
struct Sides {
    int left = 1;
    int right = 1;
};

[[nodiscard]] inline Sides sided(const Lines& one) {
    return {widest({one.upright, one.upper_left, one.lower_left, one.left_joint}),
            widest({one.upright, one.upper_right, one.lower_right, one.right_joint})};
}

// Pozioma kreska od `from` na `much` kratek. Idzie **krokiem szerokości znaku**, bo znak
// dwukolumnowy postawiony w każdej kolumnie nadpisywałby własnego sąsiada. Reszta, w którą cały
// znak już się nie mieści, zostaje tym, co tam było — tłem, nie połówką znaku.
inline void across(View& into, Spot from, int much, char32_t glyph, const Style& look) {
    const int step = widely(glyph);

    for (int x = 0; x + step <= much; x += step) into.letter({from.x + x, from.y}, glyph, look);
}

// Czy kreska pozioma dochodzi do prawej kratki `last`. Dla znaku jednokolumnowego to pytanie
// o samą tę kratkę. Dla dwukolumnowego kreska mogła skończyć się o kratkę wcześniej, bo w nieparzystą
// szerokość ostatni znak się nie mieścił — i to też jest „dochodzi", bo dalej dojść nie mogła.
[[nodiscard]] inline bool reaching(const View& seen, Spot last, char32_t glyph) {
    const int step = widely(glyph);

    for (int x = step == 1 ? last.x : last.x - step; x <= last.x; ++x)
        if (seen.at({x, last.y}).letter == glyph) return true;

    return false;
}

}

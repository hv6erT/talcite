#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/style.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Przycinanie napisów do liczby kratek. Stoi **w jednym miejscu**, bo potrzebuje go i napis,
// i legenda, a wcześniej podobny szablon powtórzony w trzech plikach okazał się naruszeniem
// zasady jednej definicji, którego konsolidator nie zgłasza.

// Tabulator w licencjach i stronach podręcznika jest wszędzie. Rozwijamy go **przy rysowaniu**,
// nie w danych: gdyby wszedł do treści wiersza, pozycja bajtowa przestałaby się zgadzać z plikiem.
constexpr int tabbing = 8;

// Ile kratek zajmuje początek wiersza aż do podanego bajtu. Tabulator dobija do najbliższej ósemki,
// znak podwójny bierze dwie kratki, znak łączący zero — ta sama reguła, którą kartka tnie wiersz
// na kawałki. Stoi tu, żeby stała w jednym miejscu: gdyby cięcie i pasek stanu liczyły osobno,
// prędzej czy później pokazałyby dwie różne kolumny dla tego samego kursora.
//
// Bez szerokości okna, więc bez ucinania tabulatora na krawędzi — kartka to robi, bo musi
// zmieścić kawałek, a licząc kolumnę pytamy o wiersz, nie o ekran.
[[nodiscard]] inline int columns(std::string_view what, std::size_t upto) {
    int much = 0;
    std::size_t at = 0;

    while (at < what.size() && at < upto) {
        const std::size_t was = at;
        const char32_t sign = letter(what, at);

        if (sign == U'\t')
            much = ((much / tabbing) + 1) * tabbing;
        else
            much += std::max(widely(sign), 0);

        if (was == at) break;
    }

    return much;
}

// Kawałek napisu, który mieści się w podanej liczbie kratek. Liczymy znakami, nie bajtami, i po
// szerokościach — inaczej pierwszy chiński znak albo pierwszy ogonek rozjechałby łamanie.
inline std::string cut(std::string_view what, int wide) {
    std::size_t at = 0;
    std::size_t upto = 0;
    int much = 0;

    while (at < what.size()) {
        const std::size_t began = at;
        const int one = widely(letter(what, at));

        if (much + one > wide) break;

        much += one;
        upto = at;

        if (began == at) break;
    }

    return std::string{what.substr(0, upto)};
}

// To samo od końca: ogon o zadanej szerokości. Idziemy znak po znaku od początku, bo UTF-8 czyta
// się tylko w tę stronę, a dopiero potem odliczamy od tyłu.
inline std::string tail(std::string_view what, int wide) {
    std::vector<std::size_t> spots;
    std::vector<int> much;

    std::size_t at = 0;

    while (at < what.size()) {
        const std::size_t began = at;

        much.push_back(widely(letter(what, at)));
        spots.push_back(began);
    }

    int room = 0;
    std::size_t which = spots.size();

    while (which > 0 && room + much[which - 1] <= wide) {
        room += much[which - 1];
        --which;
    }

    if (which >= spots.size()) return {};

    return std::string{what.substr(spots[which])};
}

// Wielokropek zajmuje jedną kratkę, więc na tekst zostaje o tyle mniej. Przy szerokości jednej
// kratki zostaje sam wielokropek — to jest jedyna uczciwa odpowiedź, bo każda inna udawałaby, że
// widać coś z treści.
//
// **Znak podaje wołający**, a nie ta funkcja: bierze go ze skórki, jak każdy inny znak. Argument
// jest wymagany właśnie po to, żeby żaden zaszyty wielokropek nie przetrwał niezauważony.
inline std::string elided(const std::string& one, int wide, ElideStyle how, char32_t mark) {
    if (how == ElideStyle::none) return one;

    if (measured(one).wide <= wide) return one;

    if (wide <= 0) return {};

    const std::string dots = spelled(mark);

    if (wide == 1) return dots;

    if (how == ElideStyle::right) return cut(one, wide - 1) + dots;

    if (how == ElideStyle::left) return dots + tail(one, wide - 1);

    const int half = (wide - 1) / 2;

    return cut(one, half) + dots + tail(one, wide - 1 - half);
}

}

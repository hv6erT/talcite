#pragma once

namespace talcite::ui {

// Litery porównujemy bez różnicy wielkości, ale **tylko po alfabecie łacińskim**: zmiana wielkości
// poza nim wymaga tablic Unicode, których talcite nie ma i mieć nie zamierza.
//
// Stało to przepisane osobno w przycisku i w ptaszku, słowo w słowo. Teraz jest raz i pytają o to
// dwa miejsca: przynależność klawisza do zbioru i szukanie litery do podkreślenia w napisie.
inline char32_t folded(char32_t one) {
    return one >= U'A' && one <= U'Z' ? one + (U'a' - U'A') : one;
}

}

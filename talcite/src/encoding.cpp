#include "talcite/encoding.hpp"

namespace talcite {

// Ile bajtów ciągu dalszego zapowiada ten bajt wiodący. Zero znaczy „albo zwykły znak ASCII,
// albo bajt, który nie jest początkiem niczego" — wołający rozstrzyga to sam, bo dekoder wejścia
// odrzuca śmieci inaczej niż pisanie na ekran.
std::size_t following(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 1;
    if ((lead & 0xF0) == 0xE0) return 2;
    if ((lead & 0xF8) == 0xF0) return 3;

    return 0;
}

// Kolejny znak z ciągu. Bajt, który nie trzyma się reguł, i ogon urwany w połowie oddajemy jako
// znak zastępczy i idziemy dalej o jeden bajt — napis z popsutym środkiem ma się wyświetlić,
// a nie zniknąć. Tam, gdzie urwany ogon ma **czekać** na resztę, a nie zniknąć — czyli
// w dekoderze wejścia — tej funkcji się nie używa.
char32_t letter(std::string_view from, std::size_t& at) {
    if (at >= from.size()) return 0;

    const unsigned char one = static_cast<unsigned char>(from[at]);

    if (one < 0x80) {
        ++at;

        return one;
    }

    const std::size_t much = following(one);

    if (much == 0 || at + much >= from.size()) {
        ++at;

        return 0xFFFD;
    }

    char32_t made = one & (0x3F >> much);

    for (std::size_t which = 1; which <= much; ++which) {
        const unsigned char next = static_cast<unsigned char>(from[at + which]);

        if ((next & 0xC0) != 0x80) {
            ++at;

            return 0xFFFD;
        }

        made = (made << 6) | (next & 0x3F);
    }

    at += much + 1;

    return made;
}

void spelled(char32_t one, std::string& into) {
    if (one < 0x80) {
        into += static_cast<char>(one);
    } else if (one < 0x800) {
        into += static_cast<char>(0xC0 | (one >> 6));
        into += static_cast<char>(0x80 | (one & 0x3F));
    } else if (one < 0x10000) {
        into += static_cast<char>(0xE0 | (one >> 12));
        into += static_cast<char>(0x80 | ((one >> 6) & 0x3F));
        into += static_cast<char>(0x80 | (one & 0x3F));
    } else {
        into += static_cast<char>(0xF0 | (one >> 18));
        into += static_cast<char>(0x80 | ((one >> 12) & 0x3F));
        into += static_cast<char>(0x80 | ((one >> 6) & 0x3F));
        into += static_cast<char>(0x80 | (one & 0x3F));
    }
}

std::string spelled(char32_t one) {
    std::string made;

    spelled(one, made);

    return made;
}

// Szerokość znaku w komórkach. Pełnej tablicy Unicode tu nie ma i nie musi być: liczą się
// zakresy, które naprawdę zajmują dwie kratki — pismo wschodnie i emoji. Znaki łączące
// traktujemy jak zwykłe, bo scalanie ich z poprzednią komórką to osobna sprawa, której jeszcze
// nie potrzebujemy, a udawanie zerowej szerokości rozjechałoby wiersz.
int widely(char32_t one) {
    if (one < 0x1100) return 1;

    if ((one >= 0x1100 && one <= 0x115F) || (one >= 0x2E80 && one <= 0x303E) ||
        (one >= 0x3041 && one <= 0x33FF) || (one >= 0x3400 && one <= 0x4DBF) ||
        (one >= 0x4E00 && one <= 0x9FFF) || (one >= 0xA000 && one <= 0xA4CF) ||
        (one >= 0xAC00 && one <= 0xD7A3) || (one >= 0xF900 && one <= 0xFAFF) ||
        (one >= 0xFE10 && one <= 0xFE19) || (one >= 0xFE30 && one <= 0xFE6F) ||
        (one >= 0xFF00 && one <= 0xFF60) || (one >= 0xFFE0 && one <= 0xFFE6) ||
        (one >= 0x1F300 && one <= 0x1F64F) || (one >= 0x1F900 && one <= 0x1F9FF) ||
        (one >= 0x20000 && one <= 0x3FFFD))
        return 2;

    return 1;
}

// Czy znak należy do wyrazu. **To jest przybliżenie i ma nim zostać**: wszystko poza ASCII liczymy
// jako literę, bo prawdziwe rozstrzygnięcie wymagałoby tablic Unicode, a tych w talcite nie ma.
// Skutek jest taki, że „ą" jest literą, ale chiński nawias i cyrylicki znak przestankowy też —
// i to jest cena, którą płacimy świadomie.
//
// Reguła jest przy tym napisana tak, że **poszerzony bajt ciągu UTF-8 daje tę samą odpowiedź co
// znak, z którego pochodzi**: oba wpadają w „nie ASCII, więc litera". Dzięki temu wolno jej używać
// i tam, gdzie idzie się po znakach, i tam, gdzie po bajtach.
//
// Stoi tu, a nie w grzebieniu, bo szukanie po całych wyrazach, zaznaczanie wyrazu podwójnym
// kliknięciem i przyszły skok po wyrazach muszą znaczyć **to samo**.
bool wordly(char32_t one) {
    if (one >= 0x80) return true;

    const char sign = static_cast<char>(one);

    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') ||
           (sign >= 'A' && sign <= 'Z') || sign == '_';
}

}

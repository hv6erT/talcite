#include "talcite/encoding.hpp"

#include <cstdint>

namespace talcite {

namespace utf8 {

// Ile bajtów ciągu dalszego zapowiada ten bajt wiodący. Zero znaczy „albo zwykły znak ASCII,
// albo bajt, który nie jest początkiem niczego" — wołający rozstrzyga to sam, bo dekoder wejścia
// odrzuca śmieci inaczej niż pisanie na ekran.
std::size_t trailing(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 1;
    if ((lead & 0xF0) == 0xE0) return 2;
    if ((lead & 0xF8) == 0xF0) return 3;

    return 0;
}

// Kolejny znak z ciągu. Bajt, który nie trzyma się reguł, i ogon urwany w połowie oddajemy jako
// znak zastępczy i idziemy dalej o jeden bajt — napis z popsutym środkiem ma się wyświetlić,
// a nie zniknąć. Tam, gdzie urwany ogon ma **czekać** na resztę, a nie zniknąć — czyli
// w dekoderze wejścia — tej funkcji się nie używa.
char32_t decode(std::string_view from, std::size_t& at) {
    if (at >= from.size()) return 0;

    const unsigned char one = static_cast<unsigned char>(from[at]);

    if (one < 0x80) {
        ++at;

        return one;
    }

    const std::size_t much = utf8::trailing(one);

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

void encode(char32_t one, std::string& into) {
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

std::string encode(char32_t one) {
    std::string made;

    utf8::encode(one, made);

    return made;
}

}

// **Znak do pokazania, nigdy do wykonania.** Tekst, który trafia na ekran, bywa obcy — nazwa pliku,
// tytuł z sieci, opis z NFO — a terminal wykonuje znaki sterujące: ESC zaczyna sekwencję, która
// potrafi przestawić ekran, zmienić tytuł okna, a w niektórych terminalach wpisać coś do schowka
// albo odpowiedzieć na wejściu, jakby to pisał człowiek. Plik nazwany sekwencją ucieczki byłby więc
// rozkazem dla terminala. Dlatego każda komórka dostaje znak przepuszczony tędy:
//
// - C0 i DEL — ich obrazki z bloku Control Pictures (␛, ␊, ␡), żeby było widać, że coś tam jest;
// - C1 (U+0080–U+009F, m.in. U+009B, jednoznakowy CSI) — znak zastępczy;
// - znaki kierunku pisma (U+200E, U+200F, U+061C, U+202A–U+202E, U+2066–U+2069) i separatory
//   wiersza i akapitu — znak zastępczy. Odwrócenie kierunku robi z nazwy „film”, U+202E, „4pm.exe”
//   na ekranie „filmexe.mp4”: plik wykonywalny udaje film.
//
// Szerokość zostaje ta sama (jedna kratka), więc nic, co liczy kolumny, się nie rozjeżdża.
char32_t shown(char32_t one) {
    if (one < 0x20) return 0x2400 + one;

    if (one == 0x7F) return 0x2421;

    if ((one >= 0x80 && one <= 0x9F) || one == 0x061C || one == 0x200E || one == 0x200F ||
        (one >= 0x2028 && one <= 0x202E) || (one >= 0x2066 && one <= 0x2069))
        return 0xFFFD;

    return one;
}

// Szerokość znaku w komórkach. Pełnej tablicy Unicode tu nie ma i nie musi być: liczą się
// zakresy, które naprawdę zajmują dwie kratki — pismo wschodnie i emoji. Znaki łączące
// traktujemy jak zwykłe, bo scalanie ich z poprzednią komórką to osobna sprawa, której jeszcze
// nie potrzebujemy, a udawanie zerowej szerokości rozjechałoby wiersz.
int width(char32_t one) {
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
bool in_word(char32_t one) {
    if (one >= 0x80) return true;

    const char sign = static_cast<char>(one);

    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') ||
           (sign >= 'A' && sign <= 'Z') || sign == '_';
}


// UTF-8 a natywny napis szeroki systemu. Potrzebne wszędzie tam, gdzie z biblioteką rozmawia
// Windows: on mówi UTF-16, a cała reszta tego kodu mówi UTF-8. Przedtem stały tu **trzy ręcznie
// przepisane kopie** tego samego — widely w `child.cpp`, jej bliźniaczka w `clipboard.cpp`
// i `narrowed` tamże — każda lokalna, każda pod `#ifdef _WIN32`.
//
// Piszemy to **własnym dekoderem, nie przez Win32**, i to jest rozstrzygnięcie, nie wygoda:
// `MultiByteToWideChar` istnieje tylko na jednym systemie, więc kod przez nie pisany nie da się
// uruchomić w próbach nigdzie indziej. Tu ta sama droga chodzi na obu — różni się wyłącznie tym,
// czy `wchar_t` ma dwa bajty (Windows, UTF-16 z parami zastępczymi), czy cztery (POSIX, UTF-32).
//
// **Znana dziura**: gałąź szesnastobitowa wykonuje się wyłącznie na Windows, więc próby chodzące
// na POSIX-ie jej nie dotykają. Para zastępcza jest sprawdzana rachunkiem w drugą stronę — znak
// spoza podstawowej płaszczyzny przechodzi tam i z powrotem — ale samego składania par
// nie widzi tu nikt.
std::string to_string(std::wstring_view from) {
    std::string made;

    made.reserve(from.size());

    for (std::size_t at = 0; at < from.size(); ++at) {
        char32_t one = static_cast<char32_t>(static_cast<std::uint32_t>(from[at]));

        if constexpr (sizeof(wchar_t) == 2) {
            // Wysoka połowa pary zastępczej bez niskiej to napis uszkodzony — oddajemy znak
            // zastępczy, tak samo jak `utf8::decode` przy zepsutym UTF-8, zamiast zgadywać.
            if (one >= 0xD800 && one <= 0xDBFF) {
                const char32_t high = one;

                if (at + 1 < from.size()) {
                    const char32_t low =
                        static_cast<char32_t>(static_cast<std::uint32_t>(from[at + 1]));

                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        one = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);

                        ++at;
                    } else {
                        one = 0xFFFD;
                    }
                } else {
                    one = 0xFFFD;
                }
            } else if (one >= 0xDC00 && one <= 0xDFFF) {
                one = 0xFFFD;
            }
        }

        utf8::encode(one, made);
    }

    return made;
}

std::wstring to_wstring(std::string_view from) {
    std::wstring made;

    made.reserve(from.size());

    std::size_t at = 0;

    while (at < from.size()) {
        // `utf8::decode` oddaje zero tylko wtedy, gdy `at` wyszło poza napis — a tego pilnuje już
        // warunek pętli — albo gdy bajtem jest **prawdziwe zero**. Zero w środku napisu przepisuje
        // się więc jak każdy inny znak: `string_view` zna swoją długość i nie kończy się na `\0`,
        // a ucinanie w tym miejscu gubiłoby resztę po cichu.
        const char32_t one = utf8::decode(from, at);

        if constexpr (sizeof(wchar_t) == 2) {
            if (one >= 0x10000) {
                const char32_t rest = one - 0x10000;

                made.push_back(static_cast<wchar_t>(0xD800 + (rest >> 10)));
                made.push_back(static_cast<wchar_t>(0xDC00 + (rest & 0x3FF)));

                continue;
            }
        }

        made.push_back(static_cast<wchar_t>(one));
    }

    return made;
}

}

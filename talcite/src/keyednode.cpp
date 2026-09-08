#include "talcite/keyednode.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "talcite/encoding.hpp"

#include "folding.hpp"

namespace talcite::ui {

namespace {

bool alike(Key mine, Key one) {
    const Mods here = mine.mods();
    const Mods there = one.mods();

    if (here.ctrl != there.ctrl || here.alt != there.alt) return false;

    if (mine.code() != Code::none || one.code() != Code::none) return mine.code() == one.code();

    const char32_t first = mine.letter();
    const char32_t second = one.letter();

    return first != 0 && second != 0 && folded(first) == folded(second);
}

}

// **Pusty klawisz nie wchodzi**, tak samo jak pusty węzeł nie wchodzi do dzieci: zbiór, w którym
// leży „nic", odpowiadałby na nic.
//
// Dokładanie tego samego dwa razy **nie jest błędem, tylko brakiem skutku** — drugi egzemplarz
// i tak nigdy by nie wygrał, bo pierwszy kończy szukanie. Dlatego cicho pomijamy, zamiast rzucać.
void Keys::add(Key one) {
    if (!one.held() || contains(one)) return;

    inside_.push_back(one);
}

void Keys::add(std::vector<Key> many) {
    for (const Key one : many) add(one);
}

void Keys::remove(Key one) {
    const auto at = std::find_if(inside_.begin(), inside_.end(),
                                 [one](Key mine) { return alike(mine, one); });

    if (at != inside_.end()) inside_.erase(at);
}

void Keys::clear() { inside_.clear(); }

// **Cała reguła dopasowania skrótu stoi tutaj**, i nigdzie indziej. Składa się z trzech części,
// z których każda ma osobny powód.
//
// **Wielkość liter składamy.** `Key::operator==` zostaje dokładne, bo pole tekstowe stoi na
// różnicy między „T" a „t" — ale skrót zgłoszony jako `U'T'` ma odpowiadać na jedno i drugie.
// Klawisze nazwane porównujemy na równość, bo F10 wielkości nie ma.
//
// **Ctrl i Alt porównujemy dokładnie.** To jest cała zdobycz z przeniesienia modyfikatorów do
// klawisza: zbiór trzymający zwykłe `F5` sam z siebie nie odpowie na Ctrl+F5, więc widżet nie musi
// tego odsiewać ręcznie, a zbiór trzymający `Alt+H` odpowie wyłącznie na `Alt+H`.
//
// **Shift przymykamy.** Przy literze jest już zakodowany w samej literze — „H" to Shift+h i pytanie
// o jedno i drugie naraz byłoby pytaniem o to samo dwa razy. Przy klawiszach funkcyjnych terminale
// meldują go niezgodnie i odsianie zabierałoby czasem zwykłe naciśnięcie; legenda robiła ten
// wyjątek u siebie od początku, a teraz stoi on w jednym miejscu dla wszystkich.
bool Keys::contains(Key one) const {
    if (!one.held()) return false;

    return std::any_of(inside_.begin(), inside_.end(),
                       [one](Key mine) { return alike(mine, one); });
}

// Sięgnięcie poza koniec jest błędem wołającego, a nie stanem do przemilczenia — tak samo jak
// w zbiorze opcji. Klawisz oddajemy **przez wartość**, bo to jedna liczba: referencja na niego
// niosłaby tyle samo, a wiązałaby wołającego z układem zbioru w środku.
Key Keys::operator[](std::size_t which) const {
    if (which >= inside_.size()) throw std::out_of_range("talcite::ui::Keys: nie ma takiego klawisza");

    return inside_[which];
}

Key Keys::front() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Keys: nie ma żadnego klawisza");

    return inside_.front();
}

Key Keys::back() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Keys: nie ma żadnego klawisza");

    return inside_.back();
}

// Kolumna, na której stoi litera do podkreślenia — albo −1, gdy żaden z klawiszy w napisie nie
// występuje. **To nie jest rozbieranie napisu**: napis jest czysty i rysuje się w całości, a my
// tylko szukamy w nim litery, którą i tak trzeba by znaleźć, żeby ją podkreślić.
//
// Rozstrzyga **pierwszy klawisz, który w napisie występuje**, i jego pierwsze wystąpienie. Przy
// jednym klawiszu — a tak jest zwykle — nie ma czego rozstrzygać.
//
// Liczymy w **kratkach**, nie w bajtach, i po napisie **już przyciętym**: litera, której po
// przycięciu nie widać, nie ma czego podkreślać, a policzona po całym napisie trafiłaby
// w wielokropek albo w klamrę.
//
// Stoi tu jako wolna funkcja, a nie w ciele szablonu, żeby składanie wielkości liter zostało
// w `src/`: `KeyedNode` jest szablonem, więc jego składowe muszą siedzieć w nagłówku, a ten
// nagłówek jest publiczny.
int marking(const Keys& these, const std::string& shown) {
    for (const Key one : these) {
        const char32_t which = one.letter();

        if (which == 0) continue;

        std::size_t at = 0;
        int wide = 0;

        while (at < shown.size()) {
            const std::size_t began = at;
            const char32_t sign = letter(shown, at);

            if (began == at) break;

            if (folded(sign) == folded(which)) return wide;

            wide += widely(sign);
        }
    }

    return -1;
}

}

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/bytes.hpp"

namespace talcite {

// **Kołdra zszyta z kawałków: dokument, który da się zmieniać bez przepisywania.**
//
// Dokument to łańcuch odcinków, z których każdy mówi „tyle bajtów z oryginału od miejsca X" albo
// „tyle bajtów z dopisków od miejsca Y". Wpisanie znaku rozcina jeden odcinek na trzy, skasowanie
// rozcina i wyrzuca. **Oryginału nikt nigdy nie rusza**: leży w pamięci taki, jaki przyszedł,
// a zmiany żyją obok. Pamięć na zmiany rośnie z ich liczbą, nie z rozmiarem pliku, a cofanie jest
// tanie, bo odcinek wraca na miejsce.
//
// Tak robi Word i tak robi VS Code, i to nie przypadek: to jedyna struktura, która daje jedno
// i drugie naraz.
//
// Oryginał jest w pamięci **z wyboru**: edytor czyta bajty przy każdym ruchu kursora i nie ma jak
// czekać na okno z dysku. Dawne wejście ze źródłem synchronicznym wypadło — źródło poza pamięcią
// jest w talcite obietnicą, a kołdra nad obietnicą (z oknem i stanem „zajęty" w edytorze) to
// osobny krok, który przyjdzie z dokumentem większym niż pamięć.
//
// Cała rzecz jest przy tym **zwykłym źródłem bajtów** — oddaje bajty spod podanego przesunięcia,
// czyli dokładnie to, czego chce `Spool`. Dzięki temu wiersze, zawijanie, znaczniki i pasek nie
// wiedzą, że dokument da się zmieniać.
class Quilt {
public:
    explicit Quilt(Bytes given);

    [[nodiscard]] std::size_t whole() const { return whole_; }

    [[nodiscard]] Bytes bytes(std::size_t at, std::size_t most);

    // Odcinek: tyle bajtów z oryginału albo z dopisków od miejsca `at`.
    struct Piece {
        bool added = false;
        std::size_t at = 0;
        std::size_t much = 0;
    };

    // **Skrawek: kawałek dokumentu podany odcinkami, bez bajtów.** Oryginał i dopiski nigdy się nie
    // zmieniają, więc odwołanie do nich jest ważne zawsze — skrawek wyjęty dziś da się wstawić za
    // tydzień i będzie tym samym tekstem. Na tym stoi dziennik edytora: skasowany gigabajt to
    // jeden odcinek, a nie gigabajt kopii.
    struct Scrap {
        std::vector<Piece> pieces;
        std::size_t much = 0;
    };

    void insert(std::size_t at, std::string_view what);

    void erase(std::size_t at, std::size_t much);

    [[nodiscard]] Scrap taken(std::size_t at, std::size_t much);

    [[nodiscard]] Scrap stored(std::string_view what);

    void put(std::size_t at, const Scrap& what);

    [[nodiscard]] std::size_t lines(const Scrap& what) const;

    [[nodiscard]] std::string text(const Scrap& what) const;

    [[nodiscard]] char last(const Scrap& what) const;

    static void joined(Scrap& into, const Scrap& more, bool front);

    // Ile bajtów leży w dopiskach. Tylko dla prób: pilnują, że cofanie i przenoszenie nie kopiują
    // tekstu, a tego inaczej nie widać.
    [[nodiscard]] std::size_t appended() const { return added_.size(); }

    // Jedno cięcie: `much` bajtów od `at` zastąpionych przez `text`.
    struct Cut {
        std::size_t at = 0;
        std::size_t much = 0;
        std::string_view text;
    };

    // **Wiele cięć naraz**, w jednym przejściu po odcinkach — dla „zamień wszystkie" i jego
    // cofnięcia. Cięcia rosnąco, rozłączne, w miejscach dokumentu sprzed całej zmiany. Ten sam tekst
    // kilku cięć z rzędu trafia do dopisków raz, a odcinki go dzielą.
    void spliced(const std::vector<Cut>& cuts);

private:
    [[nodiscard]] std::size_t split(std::size_t at);

    [[nodiscard]] std::size_t located(std::size_t at);

    Bytes held_;

    // **Dopiski tylko rosną i nigdy się nie kurczą.** Skasowany tekst zostaje w nich jako śmieć
    // i to nie jest przeoczenie: gdyby się kurczyły, każdy odcinek wskazujący dalej trzeba by
    // poprawiać, a wtedy koszt kasowania zależałby od liczby zmian, a nie od jednej.
    std::string added_;

    std::vector<Piece> pieces_;
    std::size_t whole_ = 0;

    // **Początki odcinków**, liczone leniwie po zmianie: czytanie szuka swojego odcinka binarnie,
    // zamiast iść od pierwszego. Zmiana i tak przesuwa wektor odcinków, więc przeliczenie przy
    // pierwszym czytaniu po niej nie zmienia jej kosztu, a czytań jest wiele na jedną zmianę.
    std::vector<std::size_t> starts_;
    bool stale_ = true;
};

}

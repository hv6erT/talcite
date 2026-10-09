#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "talcite/area.hpp"
#include "talcite/event.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

class Node;
class Skin;

// Położenie i liczby jednego paska — to, co trzeba wiedzieć, żeby go narysować i żeby trafić
// w niego myszą. Bez skórki i roli, więc da się to przechować do następnej klatki.
struct Track {
    Spot at;
    bool upright = true;
    int much = 0;
    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;
};

// Część paska pod kratką: strzałki (wiersz), tor przed i za suwakiem (strona), suwak (ciągnięcie).
enum class Part : std::uint8_t { none, back, forth, before, after, thumb };

// Co zrobić po zdarzeniu myszy na pasku: część, w którą trafiło wciśnięcie, a przy ciągnięciu
// suwaka — nowe przewinięcie w jednostkach paska.
struct Asked {
    Part part = Part::none;
    std::optional<std::size_t> to;
};

// **Mysz na pasku, wspólna dla wszystkich przewijających** (U6 w twa): tabela, strona, lista
// i przewijak rysują pasek każdy po swojemu, ale trafianie, chwyt i ciągnięcie liczy się wszędzie
// tak samo — więc raz, tutaj. Widżet podaje po każdym malowaniu, gdzie pasek stanął (we własnych
// współrzędnych), i pyta przy zdarzeniu, co zrobić; samo przewijanie robi sam, swoimi jednostkami.
class Grip {
public:
    void laid(std::optional<Track> given);

    [[nodiscard]] Part part(Spot here) const;

    [[nodiscard]] bool over(Spot here) const { return part(here) != Part::none; }

    [[nodiscard]] bool held() const { return grip_.has_value(); }

    [[nodiscard]] Asked mouse(const MouseEvent& what, Spot here, Node& owner);

private:
    std::optional<Track> track_;
    std::optional<int> grip_;
};

// **Pasek przewijania jako jeden przedmiot.** Dawniej były to dwie publiczne wolne funkcje
// z dziewięcioma i dziesięcioma parametrami pozycyjnymi — barred i edged — a każdy z sześciu
// widżetów wołał jedną z nich, licząc sam, gdzie stoi krawędź (przegląd, krok 5e). Teraz wołający
// wypełnia pola po nazwie i woła `draw`:
//
//     Scrollbar{.at = Scrollbar::edge(given, seen, true), .much = given.high, .shift = shift_,
//               .whole = rows, .window = given.high, .skin = skin(), .role = role}.draw(made);
//
// Nagłówek wewnętrzny: API biblioteki pasków nie rysuje, rysują je widżety. Nazwa roli paska
// zostaje publiczna w widżetach (`scrollbar_role`), bo to ona trafia do skórki.
//
// Obie referencje żyją krócej niż wyrażenie, w którym pasek powstaje — obiekt nie jest do
// przechowywania, tylko do jednego narysowania.
struct Scrollbar {
    // Pierwsza kratka paska w układzie malowanego widoku: górna przy pionowym, lewa przy poziomym.
    Spot at;

    bool upright = true;

    // Długość paska w kratkach, razem ze strzałkami.
    int much = 0;

    // Przewinięcie, długość całości i długość okna — w dowolnych, byle tych samych jednostkach:
    // wierszach, kolumnach albo bajtach.
    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;

    const Skin& skin;
    const std::string& role;

    void draw(View& into) const;

    [[nodiscard]] Track track() const { return Track{at, upright, much, shift, whole, window}; }

    // Pierwsza kratka paska przy krawędzi: pionowego z prawej, poziomego u dołu. Samo liczenie
    // miejsca było dotąd przepisane w każdym przewijaku z osobna, a to dokładnie ten rodzaj
    // arytmetyki, który w kolejnej kopii wychodzi o jedną kratkę obok.
    [[nodiscard]] static Spot edge(Size given, Area seen, bool upright);
};

}

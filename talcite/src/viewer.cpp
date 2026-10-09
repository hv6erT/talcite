#include "talcite/viewer.hpp"

#include <memory>
#include <utility>

#include "spool.hpp"

namespace talcite::ui {

// **Podgląd to strona plus trzy wejścia dokumentu.** Całe rysowanie, przewijanie, znaczniki,
// pytania o bajty i zamiary czekające na okno mieszkają w `Page`; tutaj zostaje wyłącznie to, jak
// dokument przychodzi: bajty w pamięci, źródło bajtowe czytane oknami albo zwój wierszy.
//
// Szpula na bajty nie czeka, więc i podgląd nie czeka: dwie rzeczy, które wymagają przejścia po
// dokumencie — `End` (liczba wierszy) i skok po bajcie — zostawiają po sobie zamiar, a szpula
// budzi stronę po każdym oknie i zamiar dokańcza się sam.
Viewer::Viewer() = default;

Viewer::Viewer(ScrollShape shape) : Page(shape) {}

Viewer::~Viewer() = default;

// Dokument w pamięci: szpula liczy wiersze od razu, więc wszystkie wartości są znane już przy
// podaniu. Tak wchodzi licencja wbudowana w program.
void Viewer::hold(Bytes given) { Page::hold(std::make_shared<Spool>(std::move(given))); }

// Źródło bajtowe: szpula nie liczy niczego z góry. Liczba wierszy jest nieznana, dopóki nie
// dojdziemy do końca — i to jest ta chwila, w której w Midnight Commanderze znika `+`. `head` to
// bajty od początku, które wołający już przeczytał (okno nagłówka): z nimi pierwsza klatka ma
// wiersze od razu, bez pustki do pierwszego okna z puli — plik sieciowy ładuje się chwilę.
void Viewer::hold(Feed given, std::size_t whole, Bytes head) {
    Page::hold(std::make_shared<Spool>(std::move(given), whole, std::move(head)));
}

// Zwój wierszy: bajtów nie ma, więc `at()` i `whole()` są puste, a liczba wierszy jest ta podana.
void Viewer::hold(Roll given, std::size_t lines) { Page::hold(std::move(given), lines); }

}

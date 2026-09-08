#include "talcite/viewer.hpp"

#include <cstdint>
#include <memory>
#include <utility>

#include "spool.hpp"

namespace talcite::ui {

// **Podgląd to strona plus szpula.** Całe rysowanie, przewijanie i znaczniki mieszkają w `Page`;
// tutaj zostaje wyłącznie to, czego strona nie zna: bajty. Szpula zamienia źródło bajtowe na
// wierszowe, a stąd wychodzą odpowiedzi na szwy, o które strona pyta.
struct Viewer::Kept {
    std::shared_ptr<Spool> spool;
};

Viewer::Viewer() : kept_(std::make_unique<Kept>()) {}

Viewer::Viewer(ScrollStyle style) : Page(style), kept_(std::make_unique<Kept>()) {}

Viewer::~Viewer() = default;

namespace {

Roll rolled(const std::shared_ptr<Spool>& spool) {
    return [spool](std::size_t which, std::size_t most) { return spool->lines(which, most); };
}

}

// Dokument w pamięci: szpula liczy wiersze od razu, więc wszystkie cztery wartości są znane już
// przy podaniu. Tak wchodzi licencja wbudowana w program.
void Viewer::hold(Bytes given) {
    kept_->spool = std::make_shared<Spool>(std::move(given));

    // Liczba podana stronie jest tu bez znaczenia — `lines()` i tak pyta szpulę, bo tylko ona wie,
    // czy koniec dokumentu został już osiągnięty.
    hold(rolled(kept_->spool), 0);
}

// Źródło bajtowe: szpula nie liczy niczego z góry. Liczba wierszy jest nieznana, dopóki nie
// dojdziemy do końca — i to jest ta chwila, w której w Midnight Commanderze znika `+`.
void Viewer::hold(Feed given, std::size_t whole) {
    kept_->spool = std::make_shared<Spool>(std::move(given), whole);

    hold(rolled(kept_->spool), 0);
}

std::optional<std::size_t> Viewer::at() const {
    if (!kept_->spool) return {};

    const std::optional<std::size_t> which = first();

    if (!which.has_value()) return {};

    return kept_->spool->at(*which);
}

// Bajt, od którego zaczyna się wiersz **za** ostatnim widocznym. Z niego strona liczy okno paska
// w bajtach; pusty znaczy koniec dokumentu i wtedy oknem jest reszta.
std::optional<std::size_t> Viewer::below() const {
    if (!kept_->spool) return {};

    const std::optional<std::size_t> which = first();

    if (!which.has_value()) return {};

    return kept_->spool->at(*which + shown());
}

std::optional<std::size_t> Viewer::whole() const {
    if (!kept_->spool) return {};

    return kept_->spool->whole();
}

std::optional<std::size_t> Viewer::lines() const {
    if (!kept_->spool) return Page::lines();

    return kept_->spool->counted();
}

// Bez indeksu koniec znajduje się tylko jedną drogą: przechodząc dokument. Szpula policzy wiersze
// przy okazji i od tej chwili `lines()` przestaje być puste.
void Viewer::counting() {
    if (!kept_->spool || kept_->spool->counted().has_value()) return;

    (void)kept_->spool->at(SIZE_MAX);
}

// Odstęp kotwic bierze się z wysokości okna — szpula sama tego nie wie, bo o widoku nic nie wie.
void Viewer::screen(int high) {
    if (kept_->spool) kept_->spool->screen(high);
}

// Skok po bajcie. Bez szpuli nie ma o czym mówić, bo źródło wierszowe bajtów nie zna.
void Viewer::at(std::size_t given) {
    if (!kept_->spool) return;

    first(kept_->spool->found(given));
}

}

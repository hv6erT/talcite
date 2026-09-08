#include "talcite/chosennode.hpp"

#include <algorithm>

namespace talcite::ui {

// **Rośnie na żądanie, a przycina ją tabela.** Zbiór nie zna liczby wierszy i nie ma jak jej
// poznać, więc zaznaczenie wiersza, którego jeszcze nie ma, jest zapamiętywane — a `keep()`
// wołane przy malowaniu ścina wszystko powyżej. Skutek uboczny jest tu pożądany: zaznaczenie
// przeżywa do chwili, w której tabela urośnie na tyle, żeby ten wiersz istniał.
void Chosen::add(std::size_t which) {
    if (inside_.size() <= which) inside_.resize(which + 1, false);

    inside_[which] = true;
}

void Chosen::remove(std::size_t which) {
    if (which < inside_.size()) inside_[which] = false;
}

void Chosen::clear() { inside_.clear(); }

bool Chosen::contains(std::size_t which) const {
    return which < inside_.size() && inside_[which];
}

std::size_t Chosen::many() const {
    return static_cast<std::size_t>(std::count(inside_.begin(), inside_.end(), true));
}

// Składane przy każdym wołaniu, bo w środku leży wektor bitów — wołający robi z tego listę plików
// do skopiowania, więc jedno przejście na czynność, a nie na klatkę.
std::vector<std::size_t> Chosen::all() const {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < inside_.size(); ++at)
        if (inside_[at]) made.push_back(at);

    return made;
}

void Chosen::keep(std::size_t rows) {
    if (inside_.size() > rows) inside_.resize(rows);
}


}

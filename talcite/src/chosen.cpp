#include "talcite/chosen.hpp"

#include <algorithm>

namespace talcite::ui {

// **Rośnie na żądanie, a przycina ją tabela.** Zbiór nie zna liczby wierszy i nie ma jak jej
// poznać, więc zaznaczenie wiersza, którego jeszcze nie ma, jest zapamiętywane — a `keep()`
// wołane przy malowaniu ścina wszystko powyżej. Skutek uboczny jest tu pożądany: zaznaczenie
// przeżywa do chwili, w której tabela urośnie na tyle, żeby ten wiersz istniał.
//
// **Liczba zaznaczonych idzie obok bitów** (`many_`): dawniej `many()` i `empty()` liczyły cały wektor
// przy każdym pytaniu, a panel pyta przy każdym Insert — przytrzymany na stu tysiącach wierszy
// kosztował kwadrat.
void Chosen::add(std::size_t which) {
    if (inside_.size() <= which) inside_.resize(which + 1, false);

    if (!inside_[which]) ++many_;

    inside_[which] = true;
}

void Chosen::remove(std::size_t which) {
    if (which >= inside_.size() || !inside_[which]) return;

    inside_[which] = false;
    --many_;
}

void Chosen::clear() {
    inside_.clear();
    many_ = 0;
}

bool Chosen::contains(std::size_t which) const {
    return which < inside_.size() && inside_[which];
}

std::size_t Chosen::many() const { return many_; }

// Składane przy każdym wołaniu, bo w środku leży wektor bitów — wołający robi z tego listę plików
// do skopiowania, więc jedno przejście na czynność, a nie na klatkę.
std::vector<std::size_t> Chosen::all() const {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < inside_.size(); ++at)
        if (inside_[at]) made.push_back(at);

    return made;
}

// Przycięcie zdejmuje z liczby tylko to, co odcina.
void Chosen::keep(std::size_t rows) {
    if (inside_.size() <= rows) return;

    many_ -= static_cast<std::size_t>(std::count(inside_.begin() + static_cast<std::ptrdiff_t>(rows), inside_.end(), true));

    inside_.resize(rows);
}

}

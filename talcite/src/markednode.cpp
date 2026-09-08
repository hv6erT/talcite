#include "talcite/markednode.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::ui {

namespace {

// Zbiór trzymamy uporządkowany po wierszu, a przy równych po początku — inaczej „następny"
// nie miałby jak powiedzieć, który jest następny.
bool ahead(const Mark& one, const Mark& two) {
    return one.line != two.line ? one.line < two.line : one.from < two.from;
}

}

// **Wstawia z zachowaniem kolejności**, a nie na koniec. To jest cała różnica wobec zbioru opcji:
// znacznik ma miejsce w dokumencie, więc porządek nie należy do wołającego i nie da się go zepsuć.
void Marks::add(Mark given) {
    const auto at = std::upper_bound(inside_.begin(), inside_.end(), given, ahead);

    inside_.insert(at, std::move(given));
}

// Podmiana całości sortuje **stabilnie**: wołający oddaje gotową listę, a kolejność znaczników
// stojących w tym samym miejscu jest jego, nie nasza.
void Marks::all(std::vector<Mark> given) {
    std::stable_sort(given.begin(), given.end(), ahead);

    inside_ = std::move(given);
}

void Marks::clear() { inside_.clear(); }

// Sięgnięcie poza koniec jest błędem wołającego, a nie stanem do przemilczenia — tak samo jak
// w dzieciach węzła, w zbiorze klawiszy i w zbiorze opcji.
const Mark& Marks::operator[](std::size_t which) const {
    if (which >= inside_.size()) throw std::out_of_range("talcite::ui::Marks: nie ma takiego znacznika");

    return inside_[which];
}


}

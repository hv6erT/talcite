#include "talcite/optionsnode.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::ui {

namespace {

// **Po dodaniu napis jest w zbiorze dokładnie raz i stoi tam, gdzie kazano.** Powtórka nie mnoży
// wpisu, tylko go przenosi — ta sama reguła, co w zbiorze klawiszy, i z tego samego powodu: dwa
// identyczne wpisy są nie do odróżnienia, więc drugi nie miałby jak wygrać. W wyborze i w grupie
// opcji dwie takie same pozycje czynią `which()` dwuznacznym, a w historii przeniesienie na
// początek **jest** całą regułą „najświeższe u góry".
void without(std::vector<std::string>& these, const std::string& one) {
    const auto at = std::find(these.begin(), these.end(), one);

    if (at != these.end()) these.erase(at);
}

}

// **Pusty napis nie wchodzi**, tak samo jak pusty klawisz do zbioru klawiszy i pusty węzeł do
// dzieci: pusta pozycja w wyborze jest nie do odróżnienia od tła, a w historii nie ma czego
// przywoływać.
void Options::add(std::string one) {
    if (one.empty()) return;

    without(inside_, one);

    inside_.push_back(std::move(one));
}

void Options::insert(std::size_t at, std::string one) {
    if (one.empty()) return;

    without(inside_, one);

    inside_.insert(inside_.begin() + static_cast<std::ptrdiff_t>(std::min(at, inside_.size())),
                   std::move(one));
}

void Options::remove(std::size_t which) {
    if (which >= inside_.size()) return;

    inside_.erase(inside_.begin() + static_cast<std::ptrdiff_t>(which));
}

void Options::clear() { inside_.clear(); }

// Podmiana całości nie odsiewa powtórek: wołający oddaje gotową listę i to jego lista. Odsiewamy
// tam, gdzie dokładamy po jednym, bo tylko tam wiadomo, co miałoby ustąpić czemu.
void Options::all(std::vector<std::string> given) { inside_ = std::move(given); }

bool Options::contains(const std::string& one) const {
    return std::find(inside_.begin(), inside_.end(), one) != inside_.end();
}

// −1, a nie pustka: numer służy do liczenia w środku widżetu, a nie do oddawania na zewnątrz.
int Options::which(const std::string& one) const {
    const auto at = std::find(inside_.begin(), inside_.end(), one);

    if (at == inside_.end()) return -1;

    return static_cast<int>(at - inside_.begin());
}

// Sięgnięcie poza koniec jest błędem wołającego, a nie stanem do przemilczenia — tak samo jak
// w dzieciach węzła.
const std::string& Options::operator[](std::size_t which) const {
    if (which >= inside_.size()) throw std::out_of_range("talcite::ui::Options: nie ma takiej opcji");

    return inside_[which];
}

const std::string& Options::front() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Options: nie ma żadnej opcji");

    return inside_.front();
}

const std::string& Options::back() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Options: nie ma żadnej opcji");

    return inside_.back();
}

}

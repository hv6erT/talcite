#include "rolling.hpp"

#include <algorithm>
#include <optional>

#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/veilwindow.hpp"

namespace talcite::ui {

namespace {

// Miejsce węzła na ekranie, złożone przejściem w górę po prostokątach — tym samym, którym `Root`
// składa położenie kursora w zagnieżdżonym polu i którym mysz schodzi w dół.
//
// **Liczymy to przy każdej klatce**, a nie raz przy rozwinięciu. Zapamiętana współrzędna starzeje
// się przy pierwszej zmianie rozmiaru okna, przy przewinięciu i przy urośnięciu czegokolwiek
// powyżej — a wtedy pudełko zostaje tam, gdzie widżet *był*.
std::optional<Area> standing(const Node& one) {
    Area made = one.area();

    if (made.size.wide <= 0 || made.size.high <= 0) return {};

    for (const Node* at = one.parent(); at != nullptr; at = at->parent()) {
        const Area& where = at->area();

        made.at.x += where.at.x;
        made.at.y += where.at.y;
    }

    return made;
}

}

Rolled::Rolled(std::weak_ptr<Node> under) : under_(std::move(under)) {}

// Rozwinięta lista zamyka się Escape'em — i to jest jedyne miejsce, gdzie ten klawisz zamyka
// zasłonę. Reszta zachowania idzie po `Veil`, łącznie z połykaniem wszystkiego innego.
bool Rolled::react(KeyEvent& what) {
    if (what.key().code() == Code::escape) {
        close();

        return true;
    }

    return Veil::react(what);
}

// Pudełko staje **pod** zaczepieniem. Gdy pod spodem nie starcza miejsca — nad nim; gdy wystaje
// w prawo — dosuwa się w lewo. Tak samo jak ostatnie menu w pasku, i z tego samego powodu:
// o krawędziach ekranu wie tylko ten, kto ma cały ekran, a zasłona go ma.
//
// Węzła, którego już nie ma, nie da się trzymać — wtedy oddajemy środek, czyli zachowanie `Veil`.
// To nie jest awaria: widżet wolno zdjąć z drzewa przy rozwiniętej liście.
Area Rolled::box(Size given) const {
    if (children().empty()) return {};

    const std::shared_ptr<Node> under = under_.lock();

    if (!under) return Veil::box(given);

    const std::optional<Area> where = standing(*under);

    if (!where.has_value()) return Veil::box(given);

    // Miejsce na cień odejmujemy przed liczeniem, tak samo jak robi to `Veil` — inaczej cień
    // wychodziłby poza ekran, a przy dolnej krawędzi zabrakłoby na niego wiersza.
    const int aside = shade() ? 2 : 0;
    const int apart = shade() ? 1 : 0;

    const Size room{std::max(given.wide - aside, 0), std::max(given.high - apart, 0)};

    Size wants = children().front()->wants(room);

    wants.wide = std::min(wants.wide, room.wide);
    wants.high = std::min(wants.high, room.high);

    if (wants.wide <= 0 || wants.high <= 0) return {};

    const int below = where->at.y + where->size.high;

    // Pod spodem, jeśli się mieści; nad, jeśli nie; a gdy nie mieści się i tam — zostajemy pod
    // spodem i pozwalamy przyciąć. Wybór „mniej złego" należy tu do nas, bo tylko my znamy oba
    // wymiary naraz.
    int y = below;

    if (below + wants.high > room.high) {
        const int above = where->at.y - wants.high;

        if (above >= 0) y = above;
    }

    y = std::clamp(y, 0, std::max(room.high - wants.high, 0));

    const int x = std::clamp(where->at.x, 0, std::max(room.wide - wants.wide, 0));

    return {{x, y}, wants};
}

VeilWindow* housing(Node* from) {
    for (Node* at = from; at != nullptr; at = at->parent())
        if (VeilWindow* one = dynamic_cast<VeilWindow*>(at)) return one;

    return nullptr;
}

Rolls rolling(Node& under, const std::vector<std::string>& among, std::size_t which, int most) {
    if (among.empty()) return {};

    VeilWindow* window = housing(under.parent());

    if (window == nullptr) return {};

    // Szerokość bierzemy z **najdłuższej pozycji**, a nie z widżetu: rozwinięta lista ma pokazać
    // treść, a nie ułożyć się pod szerokość wiersza, z którego wyszła. O krawędzie ekranu i tak
    // dotnie ją zasłona.
    int wide = 0;

    for (const std::string& one : among) wide = std::max(wide, measured(one).wide);

    const int high = std::max(std::min(static_cast<int>(among.size()), most), 1);
    const bool bar = static_cast<int>(among.size()) > high;

    const auto list = std::make_shared<List>(wide + (bar ? 1 : 0), high, ListStyle{bar});

    for (const std::string& one : among) list->children().add(std::make_shared<ListItem>(one));

    if (which < among.size()) list->which(static_cast<int>(which));

    const auto frame = std::make_shared<Frame>();

    frame->children().add(list);

    const auto veil = std::make_shared<Rolled>(under.weak_from_this());

    veil->children().add(frame);

    window->open(veil);

    return {veil, list};
}

}

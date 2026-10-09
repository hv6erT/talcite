#include "popup.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/modals.hpp"

namespace talcite::ui {

Rolled::Rolled(std::weak_ptr<Node> under) : under_(std::move(under)) {}

// Rozwinięta lista zamyka się Escape'em — i to jest jedyne miejsce, gdzie ten klawisz zamyka
// zasłonę. Reszta zachowania idzie po `Modal`, łącznie z połykaniem wszystkiego innego.
bool Rolled::handle(KeyEvent& what) {
    if (what.key().code() == Code::escape) {
        close();

        return true;
    }

    return Modal::handle(what);
}

// Pudełko staje **pod** zaczepieniem. Gdy pod spodem nie starcza miejsca — nad nim; gdy wystaje
// w prawo — dosuwa się w lewo. Tak samo jak ostatnie menu w pasku, i z tego samego powodu:
// o krawędziach ekranu wie tylko ten, kto ma cały ekran, a zasłona go ma.
//
// Węzła, którego już nie ma, nie da się trzymać — wtedy oddajemy środek, czyli zachowanie `Modal`.
// To nie jest awaria: widżet wolno zdjąć z drzewa przy rozwiniętej liście.
Area Rolled::seat(Size given) const {
    if (children().empty()) return {};

    const std::shared_ptr<Node> under = under_.lock();

    if (!under) return Modal::seat(given);

    const std::optional<Area> where = under->screen_area();

    if (!where.has_value()) return Modal::seat(given);

    // Miejsce na cień odejmujemy przed liczeniem, tak samo jak robi to `Modal` — inaczej cień
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

Modals* housing(Node* from) {
    for (Node* at = from; at != nullptr; at = at->parent())
        if (Modals* one = dynamic_cast<Modals*>(at)) return one;

    return nullptr;
}

void Popup::close() {
    if (const std::shared_ptr<Modal> one = rolled_.lock()) one->close();

    rolled_.reset();
}

// Złożenie i otwarcie rozwiniętej listy: ramka, lista z pozycjami, zasłona pod widżetem, wpis do
// okna. Brak okna zasłon w górę drzewa znaczy „nie ma gdzie tego otworzyć" i nie robi nic.
void Popup::open(const std::vector<std::string>& among, std::size_t which,
                 std::function<void(std::size_t)> took) {
    if (among.empty() || opened()) return;

    Modals* window = housing(under_->parent());

    if (window == nullptr) return;

    // Szerokość bierzemy z **najdłuższej pozycji**, a nie z widżetu: rozwinięta lista ma pokazać
    // treść, a nie ułożyć się pod szerokość wiersza, z którego wyszła. O krawędzie ekranu i tak
    // dotnie ją zasłona.
    int wide = 0;

    for (const std::string& one : among) wide = std::max(wide, measured(one).wide);

    const int high = std::max(std::min(static_cast<int>(among.size()), rows_), 1);
    const bool bar = static_cast<int>(among.size()) > high;

    const auto list = std::make_shared<List>(wide + (bar ? 1 : 0), high, ScrollShape{bar});

    for (const std::string& one : among) list->children().add(std::make_shared<ListItem>(one));

    if (which < among.size()) list->which(which);

    // **Wybór wraca zwykłym zdarzeniem, nie wywołaniem zwrotnym.** `ListItem::pick()` ogłasza
    // `PickEvent`, a my słuchamy go na liście, którą sami złożyliśmy. `Rolled` nie bierze w tym
    // udziału i nie musi wiedzieć, po co ją niesie.
    //
    // Uchwyt trzyma **słabe** wskazanie na widżet: rozwinięta lista może przeżyć widżet, jeśli ktoś
    // zdejmie go z drzewa przy otwartej liście. Dopóki widżet żyje, żyje i ten obiekt, więc `this`
    // jest pewne dokładnie wtedy, gdy wskazanie da się zamknąć. Najpierw zamknięcie, potem wybór —
    // widżet, który dostaje wiersz, ma już listę zwiniętą.
    const std::weak_ptr<Node> mine = under_->weak_from_this();
    const std::weak_ptr<List> theirs = list;

    (void)list->on<PickEvent>([this, mine, theirs, took = std::move(took)](PickEvent&) {
        const std::shared_ptr<Node> kept = mine.lock();
        const std::shared_ptr<List> from = theirs.lock();

        if (!kept || !from) return true;

        const std::optional<std::size_t> at = from->which();

        close();

        if (at) took(*at);

        return true;
    });

    const auto frame = std::make_shared<Frame>();

    frame->children().add(list);

    const auto veil = std::make_shared<Rolled>(under_->weak_from_this());

    veil->children().add(frame);

    window->open(veil);

    rolled_ = veil;
}

}

#include "talcite/veilwindow.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "talcite/veil.hpp"
#include "talcite/root.hpp"

namespace talcite::ui {

// Okno zasłon zabiera cały ekran i stawia w sobie wiersz treści — jak pasek menu i legenda.
//
// **Stoi najbardziej na zewnątrz ze wszystkich trzech ram.** W Midnight Commanderze okno zakrywa
// i pasek klawiszy, i pasek menu, więc musi być ich przodkiem. Kolejność zagnieżdżenia to nie jest
// upodobanie: wynika z tego, co ma leżeć na czym.
VeilWindow::VeilWindow() : body_(std::make_shared<Row>()) { children().add(body_); }

Size VeilWindow::wants(Size room) const { return room; }

// Z zewnątrz przyjmuje dokładnie jedno dziecko — treść — a każda otwarta zasłona otwiera dokładnie
// jedno miejsce, które sama zajmuje.
int VeilWindow::takes() const { return 1 + static_cast<int>(veils_.size()); }

Veil* VeilWindow::opened() const { return veils_.empty() ? nullptr : veils_.back().get(); }

void VeilWindow::open(std::shared_ptr<Veil> given) {
    if (!given) throw std::logic_error("talcite::ui::VeilWindow: nie ma czego otworzyć");

    // Do wektora wpisujemy przed dopięciem, bo to on otwiera miejsce w takes().
    veils_.push_back(given);

    Veil* one = given.get();

    try {
        children().add(std::move(given));
    } catch (...) {
        veils_.pop_back();

        throw;
    }

    // **Ognisko sprzed otwarcia pamięta sama zasłona**, nie okno. Przy stosie zasłon każda pamięta
    // swoje, więc zamknięcie wierzchniej oddaje ognisko tej pod nią, a zamknięcie ostatniej —
    // aplikacji. Jedno pole w oknie musiałoby wybrać, które z nich zapamiętać.
    if (Root* top = root()) {
        Node* was = top->focus();

        one->was_ = was != nullptr ? was->weak_from_this() : std::weak_ptr<Node>{};

        // Ognisko idzie **w głąb zasłony**, na przycisk domyślny albo pierwszy chętny. Gdy w środku
        // nie ma nic chętnego, zostaje na samej zasłonie — i wtedy to ona połyka klawisze.
        one->focus();
        one->step(1);
    }
}

void VeilWindow::close() {
    if (Veil* one = opened()) close(one);
}

void VeilWindow::close(Veil* one) {
    const auto at = std::find_if(veils_.begin(), veils_.end(),
                                 [one](const std::shared_ptr<Veil>& mine) {
                                     return mine.get() == one;
                                 });

    if (at == veils_.end()) return;

    // Trzymamy własny uchwyt przez cały ten czas: zdjęcie z drzewa i z wektora może być ostatnim,
    // a wywołanie zwrotne ma prawo jeszcze dotknąć zasłony.
    const std::shared_ptr<Veil> kept = *at;

    // **Ogłaszamy zamknięcie, póki zasłona jeszcze stoi w drzewie.** Po zdjęciu nie miałaby rodzica
    // ani ogniska, a uchwyt wołającego wisi właśnie na niej.
    kept->closing();

    const std::weak_ptr<Node> was = kept->was_;

    veils_.erase(at);

    (void)children().remove(kept.get());

    // Ognisko oddajemy dopiero po zdjęciu. Wcześniej `Root::losing` i tak by je zgasiło, bo
    // gaśnie na każdym zdejmowanym węźle i wszystkim pod nim.
    if (const std::shared_ptr<Node> back = was.lock(); back && back->root() != nullptr)
        back->focus();
}

View VeilWindow::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    const Area whole{{0, 0}, given};
    const Area part = Area::crossed(seen, whole);

    made.within(body_->render(whole, part - whole.at), part.at - seen.at);

    // Zasłony malujemy w kolejności otwierania, więc ostatni leży na wierzchu — tak samo, jak
    // ostatni wygrywa przy trafianiu myszą, bo `Root::under` przy równej warstwie bierze
    // późniejszego.
    for (const std::shared_ptr<Veil>& one : veils_) {
        const Area where{{0, 0}, given};
        const Area cut = Area::crossed(seen, where);

        made.within(one->render(where, cut - where.at), cut.at - seen.at);
    }

    return made;
}

}

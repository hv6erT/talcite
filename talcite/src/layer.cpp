#include "talcite/layer.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/root.hpp"

namespace talcite::ui {

// **Warstwa to węzeł, który leży w pojemniku warstw, da się zamknąć i pamięta, komu oddać
// ognisko.** Tyle mają wspólnego zasłona modalna (`Modal`) i okno niemodalne (`Window`), i tyle
// stało dotąd w dwóch kopiach: własne `was_`, własne `closing()`, własny `friend` pojemnika. Czym
// się różnią — modalnością, a więc tym, dokąd ognisko wraca — rozstrzyga pojemnik w `restore`.

// Zamknięcie idzie przez pojemnik, bo to on zdejmuje i oddaje ognisko. Warstwa poza pojemnikiem
// nie ma czego zamykać.
void Layer::close() {
    if (auto* up = dynamic_cast<Layers*>(parent())) up->close(*this);
}

bool Layer::current() const {
    const auto* up = dynamic_cast<const Layers*>(parent());

    return up != nullptr && up->top() == this;
}

// **Zamknięcie ogłasza się na warstwie i zanim zejdzie ona z drzewa.** Po zdjęciu nie miałaby
// rodzica, więc zdarzenie nie miałoby dokąd bąbelkować, a uchwyt wołającego wisi właśnie na niej.
// Esc nie ma przycisku, więc bez tego ogłoszenia „zamknięto bez odpowiedzi" nie dałoby się
// w ogóle usłyszeć.
void Layer::closing() {
    (void)trigger(CloseEvent{});
}

// Do listy wpisujemy **przed** dopięciem: pojemnik zasłon liczy z niej, ile dzieci przyjmie.
void Layers::adopt(std::shared_ptr<Layer> given) {
    if (!given) throw std::logic_error("talcite::ui::Layers: nothing to open");

    opened_.push_back(given);

    try {
        children().add(std::move(given));
    } catch (...) {
        opened_.pop_back();

        throw;
    }
}

// Wierzchnia to ostatnia warstwa **w kolejności dzieci**, czyli ułożenia — nie otwarcia. W oknach
// to nie to samo, bo podniesienie przestawia dzieci, a listy otwarcia nie rusza. Treść pojemnika
// zasłon warstwą nie jest i leży pod spodem.
Layer* Layers::top() const {
    for (std::size_t at = children().many(); at > 0; --at)
        if (auto* one = dynamic_cast<Layer*>(children()[at - 1].get())) return one;

    return nullptr;
}

void Layers::remember(Layer& one, Node* focus) {
    one.was_ = focus != nullptr ? focus->weak_from_this() : std::weak_ptr<Node>{};
}

std::shared_ptr<Node> Layers::remembered(const Layer& one) { return one.was_.lock(); }

void Layers::close() {
    if (Layer* one = top()) close(*one);
}

// **Ogłoszenie, zdjęcie, ognisko — w tej kolejności.** Własny uchwyt trzymamy przez cały czas, bo
// zdjęcie z drzewa i z listy może być ostatnim, a uchwyt wołającego ma prawo jeszcze dotknąć
// warstwy. Uchwyt zamknięcia ma też prawo otworzyć nową warstwę (komunikat o błędzie po OK) albo
// zamknąć inną — lista mogła się przesunąć, więc miejsca szukamy od nowa, a warstwy, której już
// nie ma, nie zdejmujemy drugi raz.
void Layers::close(Layer& one) {
    const auto at = std::find_if(opened_.begin(), opened_.end(),
                                 [&one](const std::shared_ptr<Layer>& mine) { return mine.get() == &one; });

    if (at == opened_.end()) return;

    const std::shared_ptr<Layer> kept = *at;

    kept->closing();

    const auto again = std::find(opened_.begin(), opened_.end(), kept);

    if (again == opened_.end()) return;

    const std::weak_ptr<Node> was = kept->was_;

    // Warstwa otwarta z uchwytu zamknięcia zapamiętała ognisko, które zaraz zejdzie z drzewa razem
    // z nami — dostaje więc naszą pamięć, żeby po jej zamknięciu ognisko miało dokąd wrócić.
    const auto inside = [&kept](const Node* node) {
        for (; node != nullptr; node = node->parent())
            if (node == kept.get()) return true;

        return false;
    };

    for (auto above = std::next(again); above != opened_.end(); ++above) {
        const std::shared_ptr<Node> mine = (*above)->was_.lock();

        if (!mine || mine->root() == nullptr || inside(mine.get())) (*above)->was_ = was;
    }

    opened_.erase(again);

    (void)children().remove(kept.get());

    restore(was);
}

}

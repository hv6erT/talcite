#include "talcite/modals.hpp"

#include <memory>
#include <utility>
#include <vector>

#include "talcite/modal.hpp"
#include "talcite/root.hpp"

#include "slot.hpp"
#include "stops.hpp"

namespace talcite::ui {

// Pojemnik zasłon to `Layers` z treścią pod spodem i z **modalnym ogniskiem**. Treść jest pierwszym
// dzieckiem, zasłony kolejnymi — na wierzchu ostatnia — i to `Stack` je maluje i z jego kolejności
// trafia mysz. Otwieranie, zamykanie i pamięć ogniska są wspólne z oknami w `Layers`; tu zostaje
// to, czym zasłona różni się od okna: ognisko wchodzi w nią przy otwarciu, prośba o ognisko spod
// niej czeka na jej zamknięcie, a po zamknięciu ognisko wraca tam, skąd przyszło.
//
// **Stoi najbardziej na zewnątrz ze wszystkich trzech ram.** W Midnight Commanderze okno zakrywa
// i pasek klawiszy, i pasek menu, więc musi być ich przodkiem. Kolejność zagnieżdżenia to nie jest
// upodobanie: wynika z tego, co ma leżeć na czym — i z tego, którędy idzie klawisz, bo zasłona
// połyka go, zanim dojdzie do legendy.
Modals::Modals() : body_(std::make_shared<Row>()) { children().add(body_); }

// Treść pod spodem. Gniazdo opisuje `slot.hpp`.
Node* Modals::content() const { return held(*body_); }

void Modals::content(std::shared_ptr<Node> given) { hold(*body_, std::move(given)); }

// Z zewnątrz przyjmuje dokładnie jedno dziecko — treść — a każda otwarta zasłona otwiera dokładnie
// jedno miejsce, które sama zajmuje.
int Modals::capacity() const { return 1 + static_cast<int>(many()); }

// Wierzchnia zasłona. Wszystko, co pojemnik przyjmuje poza treścią, jest zasłoną, więc rzutowanie
// jest pewne.
Modal* Modals::current() const { return static_cast<Modal*>(top()); }

void Modals::open(std::shared_ptr<Modal> given) {
    Modal* one = given.get();

    adopt(std::move(given));

    // **Ognisko sprzed otwarcia pamięta sama zasłona**, nie pojemnik. Przy stosie zasłon każda
    // pamięta swoje, więc zamknięcie wierzchniej oddaje ognisko tej pod nią, a zamknięcie ostatniej —
    // aplikacji. Jedno pole w pojemniku musiałoby wybrać, które z nich zapamiętać.
    if (Root* top = root()) {
        remember(*one, top->focused_node());

        // Ognisko idzie **w głąb zasłony**, na pierwszy chętny przystanek. Gdy w środku nie ma nic
        // chętnego, zostaje na samej zasłonie — i wtedy to ona połyka klawisze. Tylko przy
        // otwarciu: ognisko postawione na zasłonie później (choćby przez oddanie go najbliższemu
        // chętnemu przodkowi) zostaje na niej, żeby Enter nie nacisnął czegoś, czego nikt nie wybrał.
        one->focus();

        if (const std::vector<Node*> these = stops(*one); !these.empty()) these.front()->focus();
    }
}

// **Zasłona jest modalna także dla ogniska.** Węzeł pod wierzchnią zasłoną — w treści albo
// w niższej zasłonie — może poprosić o ognisko w każdej chwili: robi to kartka podglądu, gdy plik
// dojdzie z puli, choć nad nią wisi już komunikat o błędzie. Gdyby prośbę spełnić, klawisze szłyby
// pod okno, którego nie da się wtedy zamknąć ani Enterem, ani Escapem, bo do niego nie docierają.
//
// Dlatego prośba nie ginie, tylko **staje się „tam wrócić po zamknięciu"**: zapisujemy ją jako
// zapamiętane ognisko wierzchniej zasłony, a `Root` ogniska nie przestawia. Przykryty węzeł nie
// dostaje przy tym żadnego zdarzenia — mignięcie `FocusEvent` i `UnfocusEvent` przestawiłoby
// panelom czynny panel, bo one śledzą go właśnie tak.
//
// Pytamy o wierzchnią zasłonę tego okna, nie o zasłonę-przodka proszącego: to ona zejdzie
// pierwsza i to ona odda ognisko.
bool Modals::withheld(Node& one) {
    Modal* top = current();

    if (top == nullptr) return false;

    for (const Node* each = &one; each != nullptr; each = each->parent())
        if (each == top) return false;

    remember(*top, &one);

    return true;
}

// Ognisko oddajemy dopiero po zdjęciu. Wcześniej `Root::losing` i tak by je zgasiło, bo gaśnie na
// każdym zdejmowanym węźle i wszystkim pod nim. Gdy ognisko dalej gdzieś stoi — w zasłonie otwartej
// z uchwytu albo w wierzchniej, gdy zdjęto niższą — zostaje tam.
void Modals::restore(std::weak_ptr<Node> was) {
    if (Root* top = root(); top != nullptr && top->focused_node() != nullptr) return;

    const std::shared_ptr<Node> back = was.lock();

    if (!back || back->root() == nullptr) return;

    // Zapamiętany węzeł mógł w międzyczasie przestać brać ognisko — najprościej tak, że wyłączono
    // wybór, którego lista właśnie się zamyka. Wtedy ognisko idzie do **najbliższego chętnego
    // przodka**, tą samą regułą co przy wyłączeniu. Bez tego w dialogu nie zostawało nigdzie,
    // a dialog połyka klawisze i rozdaje klawisze aktywne tylko z ogniskiem w sobie.
    for (Node* at = back.get(); at != nullptr; at = at->parent())
        if (at->focusable()) {
            at->focus();

            return;
        }
}

}

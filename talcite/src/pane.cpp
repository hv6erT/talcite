#include "talcite/pane.hpp"

#include "talcite/root.hpp"

namespace talcite::ui {

// **Pojemnik nieogniskowalny już istnieje** — to zwykły `Node`: przepuszcza jedyne dziecko, oddaje
// jego życzenie i nie maluje nic od siebie. Jedyne, co `Pane` do tego dokłada, to **ognisko**
// i **wypełnianie miejsca**, i dlatego oba są domyślnie włączone. Gdyby były domyślnie wyłączone,
// element nie różniłby się od tego, co już mamy.
//
// Powód, dla którego on w ogóle musi istnieć, jest jeden i mało oczywisty: **klawisz idzie bąbelkiem
// od ogniska w górę**. Legenda i pasek menu są przodkami treści, więc dopóki ogniska nie ma nigdzie,
// `Root::deal` podaje klawisz korzeniowi i tam wędrówka się kończy — F-klawisze milczą. Okno bez
// żadnego przycisku i bez pola tekstowego musi więc mieć **coś**, co to ognisko potrzyma. Cztery
// dema miały na to własną atrapę; teraz mają jedną wspólną.
bool Pane::focusable() const { return able_; }

// **Wyłączenie oddaje ognisko najbliższemu chętnemu przodkowi** — ta sama reguła i ten sam powód, co
// w przycisku, polu, wierszu listy, pozycji menu, polu wyboru, grupie opcji i wyborze. Pojemnik był
// ósmy i jedyny, który zostawał z ogniskiem na węźle mówiącym o sobie `focusable() == false`.
//
// W dialogu Tab wygrzebałby się z tego sam, bo `Veil::step` przy ognisku spoza swojej listy skacze
// na pierwszy chętny — ale poza zasłoną nie ma kto tego naprawić i ognisko zostaje tam na stałe.
void Pane::able(bool given) {
    if (able_ == given) return;

    able_ = given;

    if (able_) return;

    unfocus();
}

// Prawda oddaje cały pokój, fałsz schodzi do życzenia dziecka. Pierwsze jest tym, czego chce panel
// i podgląd — mają zająć swoją część okna niezależnie od tego, jak duża jest treść.
Size Pane::wants(Size room) const { return full_ ? room : Node::wants(room); }

// **Pojemnik nie zabiera kliknięć swoim dzieciom.** Mysz wędruje od najgłębszego węzła pod kursorem
// w górę, więc dziecko jest pierwsze i kto je weźmie, ten kończy wędrówkę; tutaj dochodzi tylko to,
// czego nikt niżej nie chciał. Wtedy pochłonięcie jest częścią zabrania ogniska: skoro to my je
// bierzemy, to my jesteśmy odpowiedzią na ten klik.
//
// Pojemnik nieogniskowalny oddaje fałsz i **jest przezroczysty także dla myszy** — kliknięcie idzie
// dalej w górę, tak jakby go tu nie było.
//
// Straż na korzeniu jest ta sama co w `Field`: `Node::focus()` rzuca na węźle poza drzewem, a widżet
// bywa renderowany w próbie bez korzenia.
bool Pane::react(ClickEvent&) {
    if (!able_) return false;

    if (root() != nullptr) focus();

    return true;
}

}

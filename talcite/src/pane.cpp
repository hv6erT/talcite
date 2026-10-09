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
// `Root::dispatch` podaje klawisz korzeniowi i tam wędrówka się kończy — F-klawisze milczą. Okno bez
// żadnego przycisku i bez pola tekstowego musi więc mieć **coś**, co to ognisko potrzyma. Cztery
// dema miały na to własną atrapę; teraz mają jedną wspólną.

// **Bierze cały pokój i tylko tyle.** Panel i podgląd mają zająć swoją część okna niezależnie od
// tego, jak duża jest treść, i po to `Pane` to nadpisuje — bez tego `Node::wants` zszedłby do
// życzenia dziecka i cztery dema zmieniłyby wygląd.
//
// Stała, nie ustawienie. Stało tu pole `full_` z parą ustawiaczy, które **nie miały ani jednego
// wołającego** poza trzema próbami sprawdzającymi je same; rozmiar wszedł tu przy okazji i zastygł.
// Teraz od zadawania rozmiaru jest `Box`, a `Pane` został przy tym, po co powstał — przy ognisku.
Size Pane::wants(Size room) const { return room; }

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
bool Pane::handle(ClickEvent&) {
    if (!enabled()) return false;

    if (root() != nullptr) focus();

    return true;
}

}

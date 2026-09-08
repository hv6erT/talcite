#include "talcite/legendwindow.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "talcite/legenditem.hpp"

namespace talcite::ui {

// Legenda zabiera cały ekran i sama stawia nad sobą wiersz treści — tak samo jak pasek menu i z tej
// samej przyczyny: dziecko może wejść na rodzeństwo, ale nie może wyjść poza rodzica.
//
// **Legenda jest oknem zewnętrznym, a pasek menu siedzi w niej**, nie odwrotnie. Wtedy menu dostaje
// wiersze od zera do przedostatniego i rozwinięta lista **nie ma jak zamalować paska klawiszy** —
// dokładnie jak w Midnight Commanderze. Przy odwrotnym zagnieżdżeniu wysokie menu wjechałoby na
// legendę.
LegendWindow::LegendWindow() : body_(std::make_shared<Row>()) {
    children().add(body_);
}

Size LegendWindow::wants(Size room) const { return room; }

// Z zewnątrz przyjmuje dokładnie jedno dziecko — treść — a każde zgłoszone pole otwiera dokładnie
// jedno miejsce, które zaraz samo zajmuje.
int LegendWindow::takes() const { return 1 + static_cast<int>(items_.size()); }

// **Pole robi pasek, a nie wołający.** Wchodzi **wprost pod pasek**, a nie do wiersza treści:
// legenda maluje je sama, w ostatnim wierszu, na szerokościach, które sama dzieli. Do wektora
// wpisujemy przed dopięciem, bo to on otwiera miejsce w `takes()`.
std::shared_ptr<LegendItem> LegendWindow::add(Key key, std::string text) {
    auto one = std::make_shared<LegendItem>(key, std::move(text));

    items_.push_back(one);

    try {
        children().add(one);
    } catch (...) {
        items_.pop_back();

        throw;
    }

    return one;
}

// Przełączenie kontekstu — panel, podgląd, edytor — to opróżnienie paska i nowe pola. Zdejmujemy
// i z wektora, i z drzewa, bo inaczej takes() liczyłby miejsca po polach, których już nie ma.
void LegendWindow::clear() {
    const std::vector<std::shared_ptr<LegendItem>> these = items_;

    items_.clear();

    for (const std::shared_ptr<LegendItem>& one : these) (void)children().remove(one.get());
}

// Podział wiersza. Reszta z dzielenia idzie do pól od lewej, więc pasek **dokładnie** wypełnia
// wiersz i nie zostaje ogryzek przy prawej krawędzi.
int LegendWindow::width(int which, int wide) const {
    const int count = static_cast<int>(items_.size());

    if (count <= 0 || wide <= 0) return 0;

    return wide / count + (which < wide % count ? 1 : 0);
}

// Liczone z tych samych szerokości, którymi maluje paint — inaczej trafianie myszą rozjechałoby
// się z obrazem o resztę z dzielenia.
int LegendWindow::along(int which, int wide) const {
    int made = 0;

    for (int at = 0; at < which; ++at) made += width(at, wide);

    return made;
}

View LegendWindow::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    // Pusty pasek nie zabiera wiersza. Legenda bez pól to legenda, której nie ma, a nie pusta
    // krecha na dole ekranu.
    const int high = items_.empty() ? given.high : std::max(given.high - 1, 0);

    const Area above{{0, 0}, {given.wide, high}};
    const Area part = Area::crossed(seen, above);

    made.within(body_->render(above, part - above.at), part.at - seen.at);

    if (items_.empty() || given.high <= 0) return made;

    const int y = given.high - 1;

    for (int at = 0; at < static_cast<int>(items_.size()); ++at) {
        const Area where{{along(at, given.wide), y}, {width(at, given.wide), 1}};
        const Area cut = Area::crossed(seen, where);

        made.within(items_[static_cast<std::size_t>(at)]->render(where, cut - where.at),
                    cut.at - seen.at);
    }

    return made;
}

// Klawisz dochodzi tu **bąbelkiem od ogniska**, bo legenda jest przodkiem całej treści. To jest ten
// sam chwyt, którym pasek menu dostaje F9, i dlatego nie potrzeba do tego niczego w rdzeniu.
//
// **Niczego nie odsiewamy.** Stał tu ręczny odsiew Ctrl i Alt, bo zbiór klawiszy pola nie umiał
// ich wyrazić i wpadłyby jako zwykłe naciśnięcie. Odkąd modyfikatory są częścią klawisza, robi to
// `Keys::contains`: pole zgłoszone na `F5` nie odpowie na Ctrl+F5, a pole zgłoszone na `Ctrl+F5`
// odpowie wyłącznie na nie. Shift przymyka ta sama reguła i z tego samego powodu, co dotąd tutaj —
// terminale meldują go przy klawiszach funkcyjnych niezgodnie.
//
// **Pasek rozdaje klawisz aktywny, a nie rozstrzyga za pola.** Stała tu pętla z własnym składaniem
// wielkości liter i własnym pytaniem o dostępność — czyli druga kopia tego, co robi już węzeł
// z klawiszami. Dziś pasek tylko podaje zdarzenie po kolei swoim polom, a dopasowanie, odsianie
// wyłączonych i sama czynność należą do `KeyedNode` i do `LegendItem::act()`.
//
// Rozdajemy **wyłącznie po `items_`**, a nie przez `offer(what)`, bo tamto weszłoby najpierw
// w wiersz treści. Pasek klawiszy jest z boku układu i ma dostać klawisz **zamiast** treści,
// a nie przed nią.
//
// Po **migawce**, nie po żywym wykazie: uchwyt pola ma prawo przebudować pasek w reakcji, a wtedy
// wektor się przenosi pod nogami pętli.
bool LegendWindow::react(KeyEvent& what) {
    if (!what.key().held()) return false;

    const std::vector<std::shared_ptr<LegendItem>> these = items_;

    for (const std::shared_ptr<LegendItem>& one : these)
        if (offer(what, *one)) return true;

    return false;
}

}

#include "talcite/form.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/row.hpp"

#include "amid.hpp"

namespace talcite::ui {

namespace {

// **Przyciski najpierw.** Rząd przycisków i odstęp nad nim odkładamy z pokoju, zanim zapytamy
// treść — tak samo przy pytaniu o rozmiar i przy malowaniu. Kolumna pytała treść pierwszą: przy
// pytaniu o rozmiar dawała jej pokój razem z miejscem na przyciski, a przy malowaniu już tylko
// tyle, ile treść wcześniej chciała. Treść, która ma wypełnić wysokość — przewijana strona
// w pudełku z życzeniem „ile chcę, ale zostaw miejsce na to, co pode mną" — dostawała wtedy dwa
// różne pokoje, dwie różne odpowiedzi, i pod nią zostawały puste wiersze.
//
// Dzieci: treść, odstęp, przyciski — w tej kolejności, jak w kolumnie; tylko pokój liczy się od dołu.
class Footed : public Node {
public:
    [[nodiscard]] int capacity() const override { return 3; }

    [[nodiscard]] Size wants(Size room) const override {
        const Size foot = footing(room);
        const Size body = children()[0]->wants({room.wide, std::max(room.high - foot.high, 0)});

        return {std::min(std::max(body.wide, foot.wide), room.wide), std::min(body.high + foot.high, room.high)};
    }

protected:
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.clear({{0, 0}, seen.size});

        const Size foot = footing(given);
        const int left = std::max(given.high - foot.high, 0);
        const int body = std::min(children()[0]->wants({given.wide, left}).high, left);
        const int gap = children()[1]->wants({given.wide, given.high}).high;

        int y = 0;

        for (const auto& [at, high] : {std::pair{0, body}, std::pair{1, std::min(gap, given.high - body)},
                                       std::pair{2, std::max(given.high - body - gap, 0)}}) {
            const Area where{{0, y}, {given.wide, high}};
            const Area part = Area::crossed(seen, where);

            made.draw(children()[static_cast<std::size_t>(at)]->render(where, part - where.at), part.at - seen.at);

            y += high;
        }

        return made;
    }

private:
    // Odstęp i przyciski razem: wysokość obu, szerokość rzędu.
    [[nodiscard]] Size footing(Size room) const {
        const int gap = children()[1]->wants(room).high;
        const Size row = children()[2]->wants({room.wide, std::max(room.high - gap, 0)});

        return {row.wide, std::min(gap + row.high, room.high)};
    }
};

}

// **Formularz to rodzeństwo komunikatu**: tytuł, treść, rząd przycisków — tylko treść nie jest
// napisem, a gniazdem, w które wołający wkłada pola, przełączniki i listy. Tak wyglądają okna
// porządku, filtru, wyglądu czy nazwy katalogu w Midnight Commanderze i tak je składało twa —
// pięć razy tym samym kodem, zanim ten węzeł powstał.
//
// Układ:
//
//     Frame(tytuł, ramka)
//     └── Titled                ← nie węższy niż tytuł
//         └── Footed            ← pokój liczony od dołu: najpierw przyciski, reszta dla treści
//             ├── Column(body)  ← gniazdo wołającego
//             ├── Gap           ← pusty wiersz; znika, gdy nie ma treści albo przycisków
//             └── Amid → Row(1) ← przyciski pośrodku, jak w komunikacie
//
// **Bez własnych ról.** Ramka rysuje się jako ramka, przycisk jako przycisk — skórka, która zna
// `talcite.frame` i `talcite.button`, zna już cały formularz. Komunikat ma rodzinę ról, bo ma
// odmiany (błąd, pytanie); formularz odmian nie ma i nie udaje, że ma.
//
// Escape **nie zamyka** — talcite świadomie oddał to wołającemu razem z domyślnym przyciskiem
// (opisane przy `Modal`), a formularz tej decyzji nie odwraca.
Form::Form(std::string title, FormShape shape) : Dialog(ModalShape{shape.shade}), shape_(shape) {
    frame_ = std::make_shared<Frame>(std::move(title), FrameShape{shape_.border});
    body_ = std::make_shared<Column>();
    const auto column = std::make_shared<Footed>();
    const auto amid = std::make_shared<Amid>();

    amid->children().add(row_);

    column->children().add(body_);
    column->children().add(std::make_shared<Gap>(*body_, *row_));
    column->children().add(amid);

    const auto titled = std::make_shared<Titled>(*frame_);

    titled->children().add(column);
    frame_->children().add(titled);

    children().add(frame_);
}

const std::string& Form::title() const { return frame_->title(); }

void Form::title(std::string given) { frame_->title(std::move(given)); }

}

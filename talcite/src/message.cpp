#include "talcite/message.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"

#include "amid.hpp"

namespace talcite::ui {

namespace {

// Ile kratek zostawiamy wokół pudełka, żeby nie kleiło się do krawędzi ekranu. Cztery, bo dwie
// z każdej strony czytają się jak margines, a jedna wygląda jak niedoróbka.
constexpr int loose = 4;

// **Pudełko, które znika razem ze swoją treścią.** Sama ramka tego nie potrafi: `Frame::wants`
// dolicza krawędzie także wokół dziecka, które nic nie chce, więc pusty napis w pudełku to nie
// pustka, tylko obwódka dwa na dwa. Ten węzeł pyta więc **treść**, nie ramkę, i gdy treść chce
// zera wierszy, sam też chce zera — a kolumna nie daje mu ani jednej kratki.
//
// Dzięki temu kształt kolumny jest **stały**: tytuł, pudełko treści, odstęp, pudełko odpowiedzi
// stoją w niej od początku i na zawsze. `text()` i `buttons().add()` są zwykłymi ustawiaczami —
// niczego nie dopinają, nie wstawiają i nie przebudowują — a to, czy pudełko widać, wychodzi
// z rozmiarów przy najbliższej klatce.
//
// Zgaszone `boxed` znaczy brak ramki, a nie brak pudełka: węzeł zostaje i dalej znika razem
// z treścią, więc gołe komunikaty zachowują się dokładnie tak samo.
class Boxed : public Node {
public:
    Boxed(std::shared_ptr<Node> inside, const std::shared_ptr<Frame>& box)
        : inside_(std::move(inside)) {
        if (box == nullptr) {
            children().add(inside_);

            return;
        }

        box->children().add(inside_);

        children().add(box);
    }

    [[nodiscard]] Size wants(Size room) const override {
        if (inside_->wants(room).high == 0) return {};

        return Node::wants(room);
    }

private:
    std::shared_ptr<Node> inside_;
};

// **Szerokość pudełka rozstrzyga się tutaj.** Dialog pyta swoje jedyne dziecko, ile chce, i tyle mu
// daje — więc gdyby pytał ramkę wprost, komunikat z jednym długim zdaniem rozciągnąłby się na cały
// ekran. Ten węzeł zwęża pytanie, a nie odpowiedź: napis dostaje mniej miejsca, więc **zawija się**,
// zamiast zostać uciętym. Krótki komunikat nadal chce dokładnie tyle, ile ma znaków.
class Capped : public Node {
public:
    [[nodiscard]] Size wants(Size room) const override {
        if (children().empty()) return {};

        // Na ekranie węższym niż dwa marginesy oddechu nie ma — lepiej pokazać treść ciasno niż
        // ładny odstęp wokół jednej kolumny.
        const int wide = room.wide > loose * 2 ? room.wide - loose : room.wide;

        return children().front()->wants({std::max(wide, 1), room.high});
    }
};

// **Kratka oddechu po obu stronach treści** — w komunikacie bez pudełek. Pudełko dawało ją swoją
// krawędzią; bez niego napis stykałby się z ramką okna. Pyta treść o pokój węższy o dwie kratki i chce
// tyle, ile ona, plus te dwie — wysokość treści, więc pusta treść to zero wierszy i `Gap` widzi pustkę.
class Aired : public Node {
public:
    explicit Aired(std::shared_ptr<Node> inside) { children().add(std::move(inside)); }

    [[nodiscard]] Size wants(Size room) const override {
        const Size inside = children().front()->wants({std::max(room.wide - 2, 0), room.high});

        // Ponad pokój wyjdzie tylko na ekranie węższym niż sam oddech, a tam przycina ramka okna.
        return {inside.wide + 2, inside.high};
    }

protected:
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.clear({{0, 0}, seen.size});

        const Area where{{1, 0}, {std::max(given.wide - 2, 0), given.high}};
        const Area part = Area::crossed(seen, where);

        made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

        return made;
    }
};

}

// Komunikat **nie maluje nic własnego**. Jest złożeniem części, które już stoją: ramka daje tło
// i oddech, kolumna układa, napis mówi, rząd przycisków pyta. Cały jego kod to budowa drzewa,
// rozdanie ról i przekazanie odpowiedzi dalej.
//
// Układ, od zewnątrz:
//
//     Capped → Frame(rdzeń)          ← tło i obwódka; jej krawędź **jest** marginesem
//              └── Column
//                  ├── Span(tytuł)   ← wyśrodkowany, rola `<rdzeń>.title`
//                  ├── Boxed         ← pudełko treści; znika, gdy treści nie ma
//                  │   └── Frame(.box) → Span(treść)
//                  ├── Gap           ← pusty wiersz; znika, gdy któregoś pudełka nie ma
//                  └── Boxed         ← pudełko odpowiedzi; znika, gdy nie ma przycisków
//                      └── Frame(.box) → Amid → Row(odstęp 1) → przyciski
//
// **Kolumna jest złożona raz i nie zmienia się nigdy.** Wszystko, co widać albo czego nie widać,
// wychodzi z `wants` — więc `text()` i `buttons().add()` wolno wołać kiedykolwiek, także na oknie
// już otwartym, i nic nie trzeba przy tym przestawiać.
//
// **Bez pudełek** (`boxed` zgaszone) komunikat wygląda jak formularz: tytuł stoi na górnej krawędzi
// ramki, treść ma kratkę oddechu z obu stron, a przyciski stoją pośrodku pod pustym wierszem:
//
//     Capped → Frame(tytuł, rdzeń)
//              └── Titled          ← nie węższy niż tytuł, jak w formularzu
//                  └── Column
//                      ├── Aired → Span(treść)
//                      ├── Gap
//                      └── Amid → Row(odstęp 1) → przyciski
//
// Krawędź zewnętrznej ramki pełni podwójną służbę: rysuje się rolą `<rdzeń>`, więc skórka decyduje,
// czy widać z niej kreskę, czy sam kolor tła. W Midnight Commanderze pudełko błędu ma ją niewidoczną
// i wygląda to jak kolumna oddechu — a to jest dokładnie ta sama kratka.
Message::Message(std::string title, std::string text, MessageShape shape)
    : Dialog(ModalShape{shape.shade}), shape_(shape) {
    const auto capped = std::make_shared<Capped>();

    frame_ = std::make_shared<Frame>(std::string{}, FrameShape{shape_.border});

    const auto column = std::make_shared<Column>();

    title_ = std::make_shared<Span>(std::move(title),
                                    SpanShape{false, Justify::center, Elide::right});

    text_ = std::make_shared<Span>(std::move(text),
                                   SpanShape{true, Justify::center, Elide::none});

    // Odstęp między przyciskami należy do wiersza, nie do treści — przycisk zgaszony
    // do zera nie zostawia po sobie dziury.
    const auto amid = std::make_shared<Amid>();

    amid->children().add(row_);

    // Pudełko wokół treści i wokół przycisków — tak wygląda komunikat w Midnight Commanderze.
    // Kto woli gołą treść, gasi `boxed` i dostaje samą kolumnę w ramce.
    const auto boxing = [this]() -> std::shared_ptr<Frame> {
        if (!shape_.boxed) return nullptr;

        auto made = std::make_shared<Frame>(std::string{}, FrameShape{Border::thin});

        boxes_.push_back(made);

        return made;
    };

    if (!shape_.boxed) {
        frame_->title(title_->text());

        const auto said = std::make_shared<Aired>(text_);
        const auto titled = std::make_shared<Titled>(*frame_);

        column->children().add(said);
        column->children().add(std::make_shared<Gap>(*said, *amid));
        column->children().add(amid);

        titled->children().add(column);
        frame_->children().add(titled);
        capped->children().add(frame_);

        children().add(capped);

        role("talcite.message");

        return;
    }

    const auto said = std::make_shared<Boxed>(text_, boxing());
    const auto asks = std::make_shared<Boxed>(amid, boxing());

    column->children().add(title_);
    column->children().add(said);
    column->children().add(std::make_shared<Gap>(*said, *asks));
    column->children().add(asks);

    frame_->children().add(column);
    capped->children().add(frame_);

    children().add(capped);

    // Rdzeń wchodzi na końcu, gdy części już stoją — ustawiacz rozdaje go im wszystkim.
    role("talcite.message");
}

const std::string& Message::title() const { return title_->text(); }

void Message::title(std::string given) {
    title_->text(std::move(given));

    if (!shape_.boxed) frame_->title(title_->text());
}

const std::string& Message::text() const { return text_->text(); }

void Message::text(std::string given) { text_->text(std::move(given)); }

// Rdzeń rozdaje się **po zbudowaniu i po każdej zmianie**, bo dzieci trzymają go u siebie. Rodzina
// jest pięcioczłonowa: samo okno, tytuł, pudełka w środku, treść i przyciski. Skórka opisuje więc
// jeden komunikat pięcioma wpisami i nie musi niczego zgadywać.
void Message::role(std::string given) {
    Node::role(std::move(given));

    frame_->role(role());
    title_->role(role() + ".title");
    text_->role(role() + ".text");

    for (const std::shared_ptr<Frame>& one : boxes_) one->role(role() + ".box");

    buttons_.role(role() + ".button");
}

}

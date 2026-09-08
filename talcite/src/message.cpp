#include "talcite/message.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"

namespace talcite::ui {

namespace {

// Ile kratek zostawiamy wokół pudełka, żeby nie kleiło się do krawędzi ekranu. Cztery, bo dwie
// z każdej strony czytają się jak margines, a jedna wygląda jak niedoróbka.
constexpr int loose = 4;

// **Odstęp, który znika razem z sąsiadem.** Pusty wiersz między pudełkami ma sens tylko wtedy, gdy
// są oba: komunikat bez treści albo bez odpowiedzi nie ma czego rozdzielać. Pytamy o to przy każdej
// klatce, a nie raz przy budowie, bo i treść, i przyciski wolno dołożyć później — i wtedy wiersz
// ma się pojawić sam, bez przebudowywania kolumny.
class Gap : public Node {
public:
    Gap(const Node& above, const Node& below) : above_(&above), below_(&below) {}

    [[nodiscard]] int takes() const override { return 0; }

    [[nodiscard]] Size wants(Size room) const override {
        if (above_->wants(room).high == 0 || below_->wants(room).high == 0) return {};

        return {0, std::min(1, room.high)};
    }

private:
    const Node* above_;
    const Node* below_;
};

// **Pudełko, które znika razem ze swoją treścią.** Sama ramka tego nie potrafi: `Frame::wants`
// dolicza krawędzie także wokół dziecka, które nic nie chce, więc pusty napis w pudełku to nie
// pustka, tylko obwódka dwa na dwa. Ten węzeł pyta więc **treść**, nie ramkę, i gdy treść chce
// zera wierszy, sam też chce zera — a kolumna nie daje mu ani jednej kratki.
//
// Dzięki temu kształt kolumny jest **stały**: tytuł, pudełko treści, odstęp, pudełko odpowiedzi
// stoją w niej od początku i na zawsze. `text()` i `add()` są zwykłymi ustawiaczami —
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

// Dziecko na środku w poziomie. Wiersz układa przyciski od lewej, a kolumna daje mu całą swoją
// szerokość — bez tego rząd przycisków klejłby się do lewej krawędzi, a w Midnight Commanderze
// stoi pośrodku.
class Amid : public Node {
protected:
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.bare({{0, 0}, seen.size});

        if (children().empty()) return made;

        const Size wants = children().front()->wants(given);
        const int wide = std::clamp(wants.wide, 0, given.wide);

        const Area where{{std::max((given.wide - wide) / 2, 0), 0}, {wide, given.high}};
        const Area part = Area::crossed(seen, where);

        made.within(children().front()->render(where, part - where.at), part.at - seen.at);

        return made;
    }
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
// wychodzi z `wants` — więc `text()` i `add()` wolno wołać kiedykolwiek, także na oknie już
// otwartym, i nic nie trzeba przy tym przestawiać.
//
// Krawędź zewnętrznej ramki pełni podwójną służbę: rysuje się rolą `<rdzeń>`, więc skórka decyduje,
// czy widać z niej kreskę, czy sam kolor tła. W Midnight Commanderze pudełko błędu ma ją niewidoczną
// i wygląda to jak kolumna oddechu — a to jest dokładnie ta sama kratka.
Message::Message(std::string title, std::string text, MessageStyle style)
    : Dialog(VeilStyle{style.shade}), style_(style) {
    const auto capped = std::make_shared<Capped>();

    frame_ = std::make_shared<Frame>(std::string{}, FrameStyle{style_.border});

    const auto column = std::make_shared<Column>();

    title_ = std::make_shared<Span>(std::move(title),
                                    SpanStyle{false, JustifyStyle::center, ElideStyle::right});

    text_ = std::make_shared<Span>(std::move(text),
                                   SpanStyle{true, JustifyStyle::center, ElideStyle::none});

    // Odstęp między przyciskami należy do wiersza, nie do treści — przycisk zgaszony
    // do zera nie zostawia po sobie dziury.
    row_ = std::make_shared<Row>(1);

    const auto amid = std::make_shared<Amid>();

    amid->children().add(row_);

    // Pudełko wokół treści i wokół przycisków — tak wygląda komunikat w Midnight Commanderze.
    // Kto woli gołą treść, gasi `boxed` i dostaje samą kolumnę w ramce.
    const auto boxing = [this]() -> std::shared_ptr<Frame> {
        if (!style_.boxed) return nullptr;

        auto made = std::make_shared<Frame>(std::string{}, FrameStyle{LineStyle::thin});

        boxes_.push_back(made);

        return made;
    };

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

void Message::title(std::string given) { title_->text(std::move(given)); }

const std::string& Message::text() const { return text_->text(); }

void Message::text(std::string given) { text_->text(std::move(given)); }

// **Odpowiedź robi okno, a oddaje ją wołającemu.** Gdyby przyjmowało gotowy przycisk, musiałoby mu
// nadpisać rolę i wcisnąć się przed uchwyty, które wołający już na nim powiesił. Tak jest jedna
// kolejność i nie ma jej jak popsuć, a wołający dostaje pełny przycisk: `primary()`, `keys()`,
// `role()` i własne `on<PickEvent>`, w którym mieszka czynność tej odpowiedzi.
//
// Komunikat **nie wiesza na przycisku niczego swojego** i sam się po naciśnięciu nie zamyka.
// Zamknięcie jest częścią czynności odpowiedzi, więc należy do wołającego: jego uchwyt woła
// `close()`, kiedy uzna, że okno ma zejść. Nie każda odpowiedź kończy okno — przycisk „Pomoc"
// w formularzu nie ma go zamykać — a biblioteka nie zna się na tym lepiej niż aplikacja.
std::shared_ptr<Button> Message::add(std::string text) {
    auto one = std::make_shared<Button>(std::move(text));

    one->role(role() + ".button");

    row_->children().add(one);

    buttons_.push_back(one);

    return one;
}

// Czyszczenie zabiera przyciski razem z odstępami — cały wiersz jest ich i niczyj więcej.
void Message::clear() {
    row_->children().clear();

    buttons_.clear();
}

// Rdzeń rozdaje się **po zbudowaniu i po każdej zmianie**, bo dzieci trzymają go u siebie. Rodzina
// jest pięcioczłonowa: samo okno, tytuł, pudełka w środku, treść i przyciski. Skórka opisuje więc
// jeden komunikat pięcioma wpisami i nie musi niczego zgadywać.
void Message::role(std::string given) {
    Node::role(std::move(given));

    frame_->role(role());
    title_->role(role() + ".title");
    text_->role(role() + ".text");

    for (const std::shared_ptr<Frame>& one : boxes_) one->role(role() + ".box");

    for (const std::shared_ptr<Button>& one : buttons_) one->role(role() + ".button");
}

}

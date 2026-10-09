#pragma once

#include <algorithm>
#include <memory>

#include "talcite/frame.hpp"
#include "talcite/node.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// **Odstęp, który znika razem z sąsiadem.** Pusty wiersz między dwoma częściami ma sens tylko
// wtedy, gdy są obie: komunikat bez treści albo bez odpowiedzi nie ma czego rozdzielać. Pytamy
// o to przy każdej klatce, a nie raz przy budowie, bo i treść, i przyciski wolno dołożyć później —
// i wtedy wiersz ma się pojawić sam, bez przebudowywania kolumny.
class Gap : public Node {
public:
    Gap(const Node& above, const Node& below) : above_(&above), below_(&below) {}

    [[nodiscard]] int capacity() const override { return 0; }

    [[nodiscard]] Size wants(Size room) const override {
        if (above_->wants(room).high == 0 || below_->wants(room).high == 0) return {};

        return {0, std::min(1, room.high)};
    }

private:
    const Node* above_;
    const Node* below_;
};

// **Tytuł też zajmuje miejsce.** Ramka liczy szerokość z treści, a tytuł rysuje w tym, co
// zostało — krótkie okno z długim tytułem obcinałoby go. Ten węzeł pyta treść i tytuł i chce
// szerszego z nich: tytuł z kratką odstępu po obu stronach i dwiema krawędziami.
class Titled : public Node {
public:
    explicit Titled(const Frame& frame) : frame_(&frame) {}

    [[nodiscard]] Size wants(Size room) const override {
        const Size inside = Node::wants(room);
        const int title = frame_->title().empty() ? 0 : measured(frame_->title()).wide + 4;

        return {std::min(std::max(inside.wide, title), room.wide), inside.high};
    }

private:
    const Frame* frame_;
};

// Dziecko na środku w poziomie. Wiersz układa przyciski od lewej, a kolumna daje mu całą swoją
// szerokość — bez tego rząd przycisków klejłby się do lewej krawędzi, a w Midnight Commanderze
// stoi pośrodku. Wspólne dla komunikatu i formularza, dlatego w nagłówku wewnętrznym.
class Amid : public Node {
protected:
    View paint(Size given, Area seen) override {
        View made{seen.size};

        made.clear({{0, 0}, seen.size});

        if (children().empty()) return made;

        const Size wants = children().front()->wants(given);
        const int wide = std::clamp(wants.wide, 0, given.wide);

        const Area where{{std::max((given.wide - wide) / 2, 0), 0}, {wide, given.high}};
        const Area part = Area::crossed(seen, where);

        made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

        return made;
    }
};

}

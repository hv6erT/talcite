#include "talcite/frame.hpp"

#include <algorithm>

#include "talcite/encoding.hpp"
#include "talcite/line.hpp"
#include "talcite/skin.hpp"
#include "lining.hpp"
#include "titling.hpp"

namespace talcite::ui {

Frame::Frame(std::string title, FrameStyle style) : title_(std::move(title)), style_(style) {
    role("talcite.frame");
}

// Która krawędź jest rysowana i ile zabiera. Bez linii nie ma żadnej, a wyłączona krawędź nie
// zabiera treści ani jednej kratki. Góra i dół to zawsze jeden wiersz, ale **boki są tak szerokie,
// jak znaki, którymi je rysuje skórka** — dla znaków dwukolumnowych to dwie kratki, i stąd
// wnętrze zaczyna się o kratkę dalej. Dla zwykłych znaków ramki wszystko wychodzi na jeden.
Frame::Edges Frame::edges() const {
    if (style_.border == LineStyle::none) return {};

    const Sides side = sided(skin().get<Lines>(named(style_.border)));

    return {style_.top ? 1 : 0, style_.bottom ? 1 : 0, style_.left ? side.left : 0,
            style_.right ? side.right : 0};
}

// Ramka zabiera po kratce z każdej strony, którą rysuje — tak samo jak pasek w przewijaku
// i z tego samego powodu: jest częścią ramki, a nie czymś domalowanym na wierzchu treści.
// Krawędź wyłączona nie zabiera nic.
Size Frame::wants(Size room) const {
    const Edges edge = edges();

    const int aside = edge.left + edge.right;
    const int apart = edge.top + edge.bottom;

    if (children().empty()) return {std::min(aside, room.wide), std::min(apart, room.high)};

    const Size wants = children().front()->wants(
        {std::max(room.wide - aside, 0), std::max(room.high - apart, 0)});

    return {std::min(wants.wide + aside, room.wide), std::min(wants.high + apart, room.high)};
}

View Frame::paint(Size given, Area seen) {
    View made{seen.size};

    const Skin& how = skin();

    // Rdzeń nazwy roli zadaje się z zewnątrz, a **tytuł idzie za rdzeniem**: `talcite.frame.title`,
    // a dla panelu z rolą `tw.panel.focus` — `tw.panel.focus.title`. Wspólna rola tytułu nie
    // wystarczała, bo Midnight Commander ma trzy różne barwy tytułu: aktywny panel (`reverse`),
    // okno dialogowe (`dtitle`), okno błędu (`errdtitle`). Rozdzielnik ma dalej swoją.
    //
    // **Dolny napis idzie w stylu ramki, nie tytułu.** MC maluje wiersz stanu pod listą plików
    // zwykłą barwą panelu (`display_mini_info` woła `set_colors`), a tytuł czynnego panelu
    // odwrotnie — gdyby stopka szła za tytułem, byłaby odwrotna razem z nim.
    const Style look = how.get<Style>(role());
    const Style told = how.get<Style>(role() + ".title");

    const char32_t mark = how.get<char32_t>("talcite.elide");

    // **Ramka maluje swoje tło.** Wnętrze zostawało dotąd spacjami w stylu domyślnym, więc pudełko
    // w barwnej skórce było przezroczyste w każdym miejscu, którego dziecko nie zakryło — a tak nie
    // wygląda żadne okno w Midnight Commanderze. Odstęp między treścią a krawędzią, prześwit obok
    // krótszego napisu i wiersz pod ostatnim dzieckiem mają być w kolorze okna, a nie w kolorze
    // tego, co akurat leżało pod spodem.
    //
    // Pod skórką wbudowaną `talcite.frame` jest pustym stylem, a świeża kartka i tak jest spacjami
    // w pustym stylu — więc wypełnienie zapisuje dokładnie to, co już tam stało, i klatka nie
    // zmienia się o ani jedną kratkę. Sprawdzone porównaniem pełnych odcisków, nie na słowo.
    made.fill({{-seen.at.x, -seen.at.y}, given}, look);

    const Edges edge = edges();

    if (given.wide > 0 && given.high > 0) {
        const Lines lines = how.get<Lines>(named(style_.border));

        // Prawy bok zaczyna się tam, gdzie kończy się wnętrze — nie na ostatniej kolumnie, bo bok
        // ze znaku dwukolumnowego zaczyna się o kratkę wcześniej.
        const int right_x = given.wide - edge.right;
        const int last_y = given.high - 1;

        // Napisu nie ma bez krawędzi, w której miałby stać.
        const int room = given.wide - edge.left - edge.right;

        // Linia biegnie przez całą szerokość, gdy boku nie ma — narożnik jest tylko tam, gdzie
        // spotykają się dwie rysowane krawędzie.
        if (edge.top != 0)
            across(made, {edge.left - seen.at.x, -seen.at.y}, room, lines.across, look);

        if (edge.bottom != 0)
            across(made, {edge.left - seen.at.x, last_y - seen.at.y}, room, lines.across, look);

        if (edge.left != 0)
            for (int y = edge.top; y <= last_y - edge.bottom; ++y)
                made.letter({-seen.at.x, y - seen.at.y}, lines.upright, look);

        if (edge.right != 0)
            for (int y = edge.top; y <= last_y - edge.bottom; ++y)
                made.letter({right_x - seen.at.x, y - seen.at.y}, lines.upright, look);

        if (edge.top != 0 && edge.left != 0)
            made.letter({-seen.at.x, -seen.at.y}, lines.upper_left, look);

        if (edge.top != 0 && edge.right != 0)
            made.letter({right_x - seen.at.x, -seen.at.y}, lines.upper_right, look);

        if (edge.bottom != 0 && edge.left != 0)
            made.letter({-seen.at.x, last_y - seen.at.y}, lines.lower_left, look);

        if (edge.bottom != 0 && edge.right != 0)
            made.letter({right_x - seen.at.x, last_y - seen.at.y}, lines.lower_right, look);

        if (edge.top != 0)
            along(made, {edge.left - seen.at.x, -seen.at.y}, room, title_, style_.justify, told,
                  mark);

        if (edge.bottom != 0 && last_y > 0)
            along(made, {edge.left - seen.at.x, last_y - seen.at.y}, room, footer_,
                  style_.justify, look, mark);
    }

    if (children().empty()) return made;

    const Area where{{edge.left, edge.top},
                     {std::max(given.wide - edge.left - edge.right, 0),
                      std::max(given.high - edge.top - edge.bottom, 0)}};

    const Area part = Area::crossed(seen, where);

    made.within(children().front()->render(where, part - where.at), part.at - seen.at);

    // **Rozdzielnik dochodzi do ramki.** Dziecko nigdy nie maluje poza swoim prostokątem, więc
    // złączkę stawia ramka — i rozpoznaje ją **po złożonym obrazie**, a nie po tym, jaki węzeł
    // stoi w środku. Tam, gdzie treść dotyka obu boków tym samym znakiem poziomym, którym ramka
    // rysuje siebie, krawędź zamienia się w złączkę.
    //
    // Poznawanie po obrazie zamiast po typie działa przez dowolne zagnieżdżenie i nie wymaga, żeby
    // ramka znała rozdzielnik. Ceną jest to, że napis złożony z samych takich kresek na całą
    // szerokość też dostanie złączki — rzadkie, a wygląda tak, jak wyglądać powinno.
    if (style_.border != LineStyle::none && (edge.left != 0 || edge.right != 0)) {
        const Lines lines = how.get<Lines>(named(style_.border));

        const int right_x = given.wide - edge.right;
        const int first = edge.left;
        const int last = right_x - 1;

        for (int y = edge.top; y <= given.high - 1 - edge.bottom && first <= last; ++y) {
            if (made.at({first - seen.at.x, y - seen.at.y}).letter != lines.across) continue;

            if (!reaching(made, {last - seen.at.x, y - seen.at.y}, lines.across)) continue;

            if (edge.left != 0) made.letter({-seen.at.x, y - seen.at.y}, lines.left_joint, look);

            if (edge.right != 0)
                made.letter({right_x - seen.at.x, y - seen.at.y}, lines.right_joint, look);
        }
    }

    return made;
}

}

#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/base64.hpp"
#include "talcite/image.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/term.hpp"

using namespace talcite;

// Obraz w pikselach (G1): kratki obrazu niosą jego znak, złożenie przenosi znak w górę, a `Term`
// dostaje piksele tylko dla obrazu widocznego w całości. Atrapa terminala pokazuje każdy bajt,
// więc widać, kiedy sixel idzie, a kiedy nie.

namespace {

std::shared_ptr<const std::string> encoded(std::string said) { return std::make_shared<const std::string>(std::move(said)); }

Style paper(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Style made;

    made.background = Color{false, red, green, blue};

    return made;
}

// Kartka 6×4 na niebieskim tle, w niej obraz 2×2 w (1, 1): dwie kratki z literą, jedna
// przezroczysta (pierwszy plan) i jedna pusta.
View sheet(std::uint32_t mark = 7) {
    View made{{6, 4}};

    made.fill({{0, 0}, {6, 4}}, paper(0, 0, 200));

    View image{{2, 2}};

    image.clear({{0, 0}, {2, 2}});
    image.put({0, 0}, U'▀', paper(250, 0, 0));
    image.put({1, 0}, U'▄', paper(0, 250, 0));
    image.foreground({0, 1}, U'▖', Style{});
    image.picture({{0, 0}, {2, 2}}, Picture{mark, {2, 2}, encoded("SIX")});

    made.draw(image, {1, 1});

    return made;
}

bool holds(const std::string& where, const std::string& what) { return where.find(what) != std::string::npos; }

std::size_t counted(const std::string& where, const std::string& what) {
    std::size_t made = 0;

    for (std::size_t at = where.find(what); at != std::string::npos; at = where.find(what, at + 1)) ++made;

    return made;
}

// Piksele obrazu: każda klatka ma inne bajty, żeby było widać, którą wysłano.
std::shared_ptr<const Pixels> dotted(ui::Size size, int stills = 1) {
    std::vector<Still> made;

    for (int still = 0; still < stills; ++still) {
        Bytes one;

        for (int at = 0; at < size.wide * size.high * 4; ++at) one.push_back(static_cast<std::byte>(at + 40 * still));

        made.push_back(Still{one, std::chrono::milliseconds{100}});
    }

    return std::make_shared<const Pixels>(size, std::move(made));
}

// Korzeń, który rysuje kartkę z obrazem; obraz można przesunąć, zasłonić albo podmienić.
class Framed : public ui::Root {
public:
    ui::Spot at{2, 1};
    ui::Size size{3, 2};
    std::uint32_t mark = 5;
    bool covered = false;
    bool bare = false;
    char32_t elsewhere = U' ';
    ui::Spot other{0, 0};
    std::shared_ptr<const Pixels> pixels = dotted({3, 2}, 2);
    std::size_t still = 0;

protected:
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.fill({{0, 0}, given}, paper(0, 0, 90));
        made.put(other, elsewhere, paper(0, 0, 90));

        for (int y = 0; y < size.high; ++y)
            for (int x = 0; x < size.wide; ++x) made.put({at.x + x, at.y + y}, U'▒', paper(200, 100, 0));

        made.picture({at, size}, Picture{mark, size, bare ? nullptr : encoded("\x1bP0;1;0q\"1;1;30;40#0;2;100;0;0\x1b\\"),
                                         bare ? nullptr : pixels, still});

        if (covered) made.put({at.x + 1, at.y}, U'x', paper(0, 0, 90));

        return made;
    }
};

Setup pixeled(ui::Size cell = {10, 20}, Graphics kind = Graphics::sixel) {
    Setup made;

    made.graphics = kind;
    made.cell = cell;

    return made;
}

Setup kittied() { return pixeled({10, 20}, Graphics::kitty); }

struct Rig {
    std::shared_ptr<faking::Terminal> line = faking::terminal();
    Framed root;
    std::unique_ptr<Term> term;

    explicit Rig(Setup given = pixeled(), ui::Size window = {12, 6}) {
        line->window = window;
        term = std::make_unique<Term>(std::move(given), Seam<Console>{line});
        term->show(root);
    }

    std::string drawn() {
        line->written.clear();
        term->draw();

        return line->written;
    }
};

const std::string sixel_head = "\x1bP0;1;0q";

}

// --- Kartka --------------------------------------------------------------------------------------

TRIAL("View: kratki obrazu niosą jego znak, przezroczysta staje się pustym pierwszym planem") {
    View made{{3, 1}};

    made.clear({{0, 0}, {3, 1}});
    made.put({0, 0}, U'a', paper(1, 2, 3));
    made.picture({{0, 0}, {2, 1}}, Picture{9, {2, 1}, encoded("x")});

    same(made.at({0, 0}).picture, std::uint32_t{9});
    same(made.at({0, 0}).letter, char32_t{U'a'});
    same(made.at({1, 0}).picture, std::uint32_t{9});
    same(static_cast<int>(made.at({1, 0}).shown), static_cast<int>(Shown::foreground));
    same(made.at({1, 0}).letter, char32_t{U' '});
    same(made.at({2, 0}).picture, std::uint32_t{0});
    same(static_cast<int>(made.at({2, 0}).shown), static_cast<int>(Shown::none));
    same(made.pictures().size(), std::size_t{1});
}

TRIAL("View: ten sam znak drugi raz zastępuje wpis, a nie dokłada drugiego") {
    View made{{2, 1}};

    made.picture({{0, 0}, {2, 1}}, Picture{3, {2, 1}, encoded("a")});
    made.picture({{0, 0}, {2, 1}}, Picture{3, {2, 1}, encoded("b")});

    same(made.pictures().size(), std::size_t{1});
    same(*made.pictures().front().sixel, std::string{"b"});
}

TRIAL("View: obraz jedzie w górę razem z kratkami i staje tam, gdzie go wklejono") {
    View made = sheet();

    same(made.pictures().size(), std::size_t{1});
    same(made.at({1, 1}).picture, std::uint32_t{7});
    same(made.at({2, 2}).picture, std::uint32_t{7});
    same(made.at({0, 0}).picture, std::uint32_t{0});

    // Przezroczysta kratka wzięła tło spodu przy wklejeniu.
    ought(made.at({1, 2}).style.background == paper(0, 0, 200).background);

    const std::vector<Placement> found = made.bared({{0, 0}, {6, 4}});

    same(found.size(), std::size_t{1});
    ought(found.front().at == (ui::Spot{1, 1}));
    same(*found.front().picture.sixel, std::string{"SIX"});
}

TRIAL("View: obraz widoczny w całości ma puste kratki z ich tłem i ze swoim znakiem") {
    View made = sheet();

    (void)made.bared({{0, 0}, {6, 4}});

    same(made.at({1, 1}), (Cell{U' ', paper(250, 0, 0), Shown::own, 7}));
    same(made.at({2, 1}), (Cell{U' ', paper(0, 250, 0), Shown::own, 7}));
    same(made.at({1, 2}), (Cell{U' ', paper(0, 0, 200), Shown::own, 7}));
    same(made.at({2, 2}), (Cell{U' ', paper(0, 0, 200), Shown::own, 7}));

    // Pusta kratka obrazu i zwykła pusta kratka z tym samym tłem nie są sobie równe.
    ought(!(made.at({2, 2}) == made.at({0, 0})));
}

TRIAL("View: obraz zasłonięty choćby w jednej kratce zostaje w znakach i traci znak") {
    View made = sheet();
    View dialog{{1, 1}};

    dialog.put({0, 0}, U'x', paper(9, 9, 9));
    made.draw(dialog, {2, 2});

    same(made.bared({{0, 0}, {6, 4}}).size(), std::size_t{0});
    same(made.at({1, 1}).letter, char32_t{U'▀'});
    same(made.at({1, 1}).picture, std::uint32_t{0});
    same(made.at({2, 1}).picture, std::uint32_t{0});
}

TRIAL("View: cień nad obrazem zasłania go — pikseli nie da się przygasić") {
    View made = sheet();
    View shadow{{1, 1}};

    shadow.shade({{0, 0}, {1, 1}}, paper(10, 10, 10));
    made.draw(shadow, {1, 1});

    same(made.at({1, 1}).picture, std::uint32_t{0});
    same(made.bared({{0, 0}, {6, 4}}).size(), std::size_t{0});
}

TRIAL("View: obraz przycięty brzegiem wklejenia albo wystający poza pole nie dostaje pikseli") {
    // Przycięty: tylko lewa kolumna obrazu weszła do kartki.
    View narrow{{2, 4}};

    narrow.draw(sheet(), {0, 0});

    same(narrow.pictures().size(), std::size_t{1});
    same(narrow.bared({{0, 0}, {2, 4}}).size(), std::size_t{0});
    same(narrow.at({1, 1}).picture, std::uint32_t{0});

    // Cały, ale pole kończy się wierszem wcześniej.
    View whole = sheet();

    same(whole.bared({{0, 0}, {6, 2}}).size(), std::size_t{0});

    View fits = sheet();

    same(fits.bared({{0, 0}, {6, 3}}).size(), std::size_t{1});

    // Pole kończy się kolumnę wcześniej, i pole zaczyna się kolumnę później.
    View right = sheet();

    same(right.bared({{0, 0}, {2, 4}}).size(), std::size_t{0});

    View left = sheet();

    same(left.bared({{2, 0}, {4, 4}}).size(), std::size_t{0});
}

TRIAL("View: obraz bez widocznej kratki i obraz bez znaku nie dają nic") {
    View away{{4, 4}};

    away.picture({{10, 10}, {2, 2}}, Picture{4, {2, 2}, encoded("a")});

    same(away.bared({{0, 0}, {4, 4}}).size(), std::size_t{0});

    // Zero to „żaden obraz" — niesie je każda zwykła kratka, więc kartka akurat w rozmiarze obrazu
    // wyglądałaby na obraz w całości.
    View plain{{2, 2}};

    plain.picture({{0, 0}, {2, 2}}, Picture{0, {2, 2}, encoded("b")});

    same(plain.bared({{0, 0}, {2, 2}}).size(), std::size_t{0});
}

TRIAL("View: ten sam obraz wklejony dwa razy jest w spisie raz") {
    View made{{8, 4}};

    made.draw(sheet(), {0, 0});
    made.draw(sheet(), {0, 0});

    same(made.pictures().size(), std::size_t{1});
    same(made.bared({{0, 0}, {8, 4}}).size(), std::size_t{1});
}

TRIAL("View: kratki w innym kształcie niż obraz to nie obraz w całości") {
    View made{{3, 3}};

    made.picture({{0, 0}, {3, 1}}, Picture{8, {1, 3}, encoded("a")});

    same(made.bared({{0, 0}, {3, 3}}).size(), std::size_t{0});
}

TRIAL("View: dwa obrazy obok siebie — oba w całości, każdy na swoim miejscu") {
    View made{{8, 3}};

    made.draw(sheet(1), {0, 0});

    View second{{2, 1}};

    second.put({0, 0}, U'a', paper(1, 1, 1));
    second.put({1, 0}, U'b', paper(1, 1, 1));
    second.picture({{0, 0}, {2, 1}}, Picture{2, {2, 1}, encoded("two")});
    made.draw(second, {5, 0});

    const std::vector<Placement> found = made.bared({{0, 0}, {8, 3}});

    same(found.size(), std::size_t{2});
    same(found[0].picture.mark, std::uint32_t{1});
    ought(found[0].at == (ui::Spot{1, 1}));
    same(found[1].picture.mark, std::uint32_t{2});
    ought(found[1].at == (ui::Spot{5, 0}));
}

// --- Terminal ------------------------------------------------------------------------------------

TRIAL("Term: obraz widoczny w całości idzie sixelem, po kratkach, w swoim miejscu") {
    Rig rig;

    const std::string said = rig.drawn();
    const std::size_t at = said.find(sixel_head);

    ought(at != std::string::npos);
    ought(said.compare(at - 6, 6, "\x1b[2;3H") == 0);

    // Kratki obrazu wyszły puste, z tłem znaku, i przed obrazem.
    ought(said.find("\x1b[0;48;5;") < at);
    same(said.find(std::string{"▒"}), std::string::npos);
    same(counted(said, sixel_head), std::size_t{1});
}

TRIAL("Term: obraz bez zmian nie idzie drugi raz, zmiana obok też go nie wysyła") {
    Rig rig;

    (void)rig.drawn();

    same(rig.drawn(), std::string{});

    rig.root.elsewhere = U'k';

    const std::string said = rig.drawn();

    ought(holds(said, "k"));
    ought(!holds(said, sixel_head));
}

TRIAL("Term: zasłonięty obraz wraca do znaków, odsłonięty dostaje piksele od nowa") {
    Rig rig;

    (void)rig.drawn();

    rig.root.covered = true;

    const std::string hidden = rig.drawn();

    ought(!holds(hidden, sixel_head));
    ought(holds(hidden, "x"));

    // Wszystkie kratki obrazu przepisane znakami — inaczej piksele zostałyby pod nimi.
    same(counted(hidden, std::string{"▒"}), std::size_t{5});

    rig.root.covered = false;

    const std::string back = rig.drawn();

    same(counted(back, sixel_head), std::size_t{1});
    ought(!holds(back, std::string{"▒"}));
}

TRIAL("Term: inny znak w tym samym miejscu — inna klatka animacji — idzie od nowa") {
    Rig rig;

    (void)rig.drawn();

    rig.root.mark = 6;

    same(counted(rig.drawn(), sixel_head), std::size_t{1});
}

TRIAL("Term: przesunięty obraz idzie w nowe miejsce, stare kratki wracają do tła") {
    Rig rig;

    (void)rig.drawn();

    rig.root.at = {5, 2};

    const std::string said = rig.drawn();

    ought(holds(said, "\x1b[3;6H" + sixel_head));
    ought(holds(said, "\x1b[2;3H"));
}

// Po obrazie kursor stoi tam, gdzie postawił go terminal: kratka tuż za obrazem dostaje własny skok,
// choć przed obrazem kursor stał właśnie przy niej.
TRIAL("Term: po obrazie następny znak idzie z własnym skokiem") {
    Rig rig;

    (void)rig.drawn();

    rig.root.mark = 9;

    const std::string again = rig.drawn();

    ought(holds(again, sixel_head));

    rig.root.elsewhere = U'k';
    rig.root.other = {5, 2};

    ought(holds(rig.drawn(), "\x1b[3;6H"));
}

TRIAL("Term: obraz bez bajtów ma puste kratki i nic nie wysyła") {
    Rig rig;

    rig.root.bare = true;

    const std::string said = rig.drawn();

    ought(!holds(said, sixel_head));
    ought(!holds(said, std::string{"▒"}));
}

// Obraz bez bajtów — przed nim w spisie — nie zatrzymuje następnych.
TRIAL("Term: obraz bez bajtów nie zatrzymuje obrazu za nim") {
    class Two : public ui::Root {
    protected:
        View paint(ui::Size given, ui::Area seen) override {
            (void)seen;

            View made{given};

            made.fill({{0, 0}, given}, paper(0, 0, 90));
            made.picture({{0, 0}, {2, 1}}, Picture{1, {2, 1}, nullptr});
            made.picture({{4, 1}, {2, 1}}, Picture{2, {2, 1}, encoded("\x1bP0;1;0qSECOND\x1b\\")});

            return made;
        }
    };

    auto line = faking::terminal();

    line->window = ui::Size{10, 4};

    Two root;
    Term term{pixeled(), Seam<Console>{line}};

    term.show(root);
    line->written.clear();
    term.draw();

    ought(holds(line->written, "\x1b[2;5H\x1bP0;1;0qSECOND"));
}

TRIAL("Term: obraz sięgający ostatniego wiersza zostaje w znakach") {
    Rig rig{pixeled(), {12, 3}};

    const std::string said = rig.drawn();

    ought(!holds(said, sixel_head));
    ought(holds(said, std::string{"▒"}));
}

TRIAL("Term: bez sixela, bez wymiaru kratki albo z życzeniem bez grafiki — same znaki") {
    Setup plain;

    plain.cell = ui::Size{10, 20};

    Rig none{plain};

    ought(!holds(none.drawn(), sixel_head));

    Setup blind;

    blind.graphics = Graphics::sixel;

    Rig unknown{blind};

    ought(!holds(unknown.drawn(), sixel_head));

    Setup kitty = pixeled();

    kitty.graphics = Graphics::kitty;

    Rig other{kitty};

    ought(!holds(other.drawn(), sixel_head));
}

TRIAL("Term: korzeń dostaje grafikę z terminala, także po zmianie konfiguracji") {
    Rig rig;

    same(static_cast<int>(rig.root.graphics()), static_cast<int>(Graphics::sixel));

    Setup none = pixeled();

    none.graphics = Graphics::none;

    rig.term->reconfigure(none);
    (void)rig.term->loop().turn();

    same(static_cast<int>(rig.root.graphics()), static_cast<int>(Graphics::none));

    // Po zmianie konfiguracji ekran rysuje się od nowa — już bez obrazu.
    const std::string said = rig.drawn();

    ought(!holds(said, sixel_head));
    ought(holds(said, std::string{"▒"}));
}

TRIAL("Term: odświeżenie całego ekranu wysyła obraz jeszcze raz") {
    Rig rig;

    (void)rig.drawn();

    rig.term->reconfigure(pixeled());
    (void)rig.term->loop().turn();

    same(counted(rig.drawn(), sixel_head), std::size_t{1});
}

// --- Widżet --------------------------------------------------------------------------------------

namespace {

Pixels square(ui::Size size, std::uint8_t red) {
    Bytes made;

    for (int at = 0; at < size.wide * size.high; ++at)
        for (const std::uint8_t one : {red, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{255}}) made.push_back(std::byte{one});

    return Pixels{size, {Still{made, std::chrono::milliseconds{0}}}};
}

// Korzeń jak terminal z sixelem: kratka 10×20, sekstanty. Obraz dostaje pole 20×5, więc pod nim
// zostaje wiersz — ostatni wiersz ekranu i tak nie bierze pikseli.
class Sheet : public ui::Root {
public:
    std::shared_ptr<ui::Image> image = std::make_shared<ui::Image>();

    explicit Sheet(Graphics given = Graphics::sixel, ui::Size grid = {10, 20}) {
        cell(grid);
        blocks(ui::Blocks::sextants);
        colors(Colors::rgb);
        unicode(true);
        graphics(given);
        children().add(image);
    }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.fill({{0, 0}, given}, paper(0, 0, 0));
        made.draw(image->render({{0, 0}, {20, 5}}), {0, 0});

        return made;
    }
};

bool settled(Loop& loop, ui::Root& top, const ui::Image& one) {
    for (int tries = 0; tries < 500; ++tries) {
        (void)top.render({{0, 0}, {20, 6}});
        (void)loop.turn();

        if (!one.busy()) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return false;
}

std::string raster(const std::string& said) {
    const std::size_t at = said.find('"');

    if (at == std::string::npos) return {};

    return said.substr(at, said.find('#', at) - at);
}

}

TRIAL("Image: przy sixelu zamawia piksele pola, a kratki znaczy swoim obrazem") {
    Loop loop;
    Sheet top;

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{1});

    const Picture& one = made.pictures().front();

    // Pole 20×5 kratek po 10×20 pikseli: kwadrat ma pięć wierszy i dziesięć kolumn.
    ought(one.size == (ui::Size{10, 5}));
    same(raster(*one.sixel), std::string{"\"1;1;100;96"});
    same(made.at({5, 0}).picture, one.mark);
    same(made.at({14, 4}).picture, one.mark);
    same(made.at({4, 0}).picture, std::uint32_t{0});
    same(made.at({15, 0}).picture, std::uint32_t{0});

    // Znaki obrazu policzone z pikseli: czerwień, a nie pustka.
    ought(made.at({5, 0}).style.foreground.red > 150 || made.at({5, 0}).style.background.red > 150);
}

TRIAL("Image: bez sixela obraz jest w znakach i nie zostawia żadnego znaku obrazu") {
    Loop loop;
    Sheet top{Graphics::none};

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{0});
    same(made.at({5, 0}).picture, std::uint32_t{0});
    ought(made.at({5, 0}).shown != Shown::none);
}

TRIAL("Image: nieznany wymiar kratki to znaki, przy sixelu i przy kitty") {
    for (const auto& [given, grid] : std::vector<std::pair<Graphics, ui::Size>>{
             {Graphics::kitty, {0, 20}}, {Graphics::sixel, {0, 20}}, {Graphics::sixel, {10, 0}}}) {
        Loop loop;
        Sheet top{given, grid};

        top.image->hold(square({40, 40}, 200));

        ought(settled(loop, top, *top.image));

        const View made = top.render({{0, 0}, {20, 6}});

        same(made.pictures().size(), std::size_t{0});

        // Obraz jest — w znakach.
        bool red = false;

        for (int x = 0; x < 20; ++x) red = red || made.at({x, 2}).style.foreground.red > 150;

        ought(red);
    }
}

// Znaki obrazu przy pikselach liczą się z pikseli pola, zmniejszonych do siatki — nie z ich rogu.
TRIAL("Image: znaki zastępcze to cały obraz, zmniejszony") {
    Loop loop;
    Sheet top;

    Bytes halves;

    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            for (const std::uint8_t one : {static_cast<std::uint8_t>(x < 20 ? 250 : 0), std::uint8_t{0}, static_cast<std::uint8_t>(x < 20 ? 0 : 250), std::uint8_t{255}})
                halves.push_back(std::byte{one});

    top.image->hold(Pixels{{40, 40}, {Still{halves, std::chrono::milliseconds{0}}}});

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});
    const Cell left = made.at({5, 2});
    const Cell right = made.at({14, 2});

    ought(left.style.foreground.red > 200 && left.style.foreground.blue < 50);
    ought(right.style.foreground.blue > 200 && right.style.foreground.red < 50);
}

// Wynik kodowania dla obrazu, którego już nie ma, nie wchodzi — pozna się go po numerze kolejnym.
TRIAL("Image: spóźniony sixel poprzedniego obrazu przepada") {
    Loop loop;
    Sheet top;

    top.image->hold(square({40, 40}, 255));
    (void)top.render({{0, 0}, {20, 6}});

    top.image->hold(square({40, 40}, 51));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{1});
    ought(holds(*made.pictures().front().sixel, "#0;2;20;0;0"));
}

TRIAL("Image: przejście na sixel w biegu przelicza obraz, powrót go zdejmuje") {
    Loop loop;
    Sheet top{Graphics::none};

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    top.graphics(Graphics::sixel);

    ought(settled(loop, top, *top.image));
    same(top.render({{0, 0}, {20, 6}}).pictures().size(), std::size_t{1});

    top.graphics(Graphics::none);

    ought(settled(loop, top, *top.image));
    same(top.render({{0, 0}, {20, 6}}).pictures().size(), std::size_t{0});
}

TRIAL("Image: nowy obraz dostaje nowy znak") {
    Loop loop;
    Sheet top;

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    const std::uint32_t first = top.render({{0, 0}, {20, 6}}).pictures().front().mark;

    top.image->hold(square({40, 40}, 90));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{1});
    ought(made.pictures().front().mark != first);
}

TRIAL("Image: obraz z pliku przy sixelu też zamawia piksele pola") {
    Loop loop;
    Sheet top;

    std::vector<ui::Size> asked;

    Figure figure;

    figure.size = {40, 40};
    figure.fetch = [&asked](ui::Size want) {
        asked.push_back(want);

        return Promise<Pixels>{[want](auto resolve, auto) { resolve(square(want, 200)); }};
    };

    top.image->hold(std::move(figure));

    ought(settled(loop, top, *top.image));

    ought(!asked.empty());
    ought(asked.back() == (ui::Size{100, 100}));
    same(top.render({{0, 0}, {20, 6}}).pictures().size(), std::size_t{1});
}

TRIAL("Image: przez terminal — znaki, a gdy sixel gotów, obraz") {
    auto line = faking::terminal();

    line->window = ui::Size{20, 6};

    Sheet top{Graphics::none};
    Term term{pixeled(), Seam<Console>{line}};

    term.show(top);
    top.image->hold(square({40, 40}, 200));

    line->written.clear();
    term.draw();

    ought(!holds(line->written, sixel_head));

    for (int tries = 0; tries < 500 && top.image->busy(); ++tries) {
        (void)term.loop().turn();

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    line->written.clear();
    term.draw();

    ought(holds(line->written, "\x1b[1;6H" + sixel_head + "\"1;1;100;96"));
}

// --- Kitty (G2) ----------------------------------------------------------------------------------

namespace {

const std::string kitty_send = "\x1b_Ga=t,";

std::string sent_whole(std::uint32_t mark, const Pixels& pixels, std::size_t still) {
    return "\x1b_Ga=t,f=32,s=" + std::to_string(pixels.size().wide) + ",v=" + std::to_string(pixels.size().high) +
           ",i=" + std::to_string(mark) + ",q=2,m=0;" + base64(pixels.stills()[still].bytes) + "\x1b\\";
}

std::string stood_at(std::string spot, std::uint32_t mark) {
    return "\x1b[" + spot + "H\x1b_Ga=p,i=" + std::to_string(mark) + ",c=3,r=2,C=1,q=2\x1b\\";
}

std::string taken(std::uint32_t mark, char how = 'i') {
    return "\x1b_Ga=d,d=" + std::string(1, how) + ",i=" + std::to_string(mark) + ",q=2\x1b\\";
}

}

TRIAL("Term: kitty wysyła obraz raz, a stawia go w jego kratkach") {
    Rig rig{kittied()};

    const std::string said = rig.drawn();
    const std::size_t sending = said.find(sent_whole(5, *rig.root.pixels, 0));
    const std::size_t standing = said.find(stood_at("2;3", 5));

    ought(sending != std::string::npos);
    ought(standing != std::string::npos);
    ought(sending < standing);
    ought(!holds(said, sixel_head));
    ought(!holds(said, std::string{"▒"}));

    same(rig.drawn(), std::string{});
}

TRIAL("Term: zasłonięty obraz kitty schodzi przed znakami, odsłonięty wraca bez wysyłania") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.covered = true;

    const std::string hidden = rig.drawn();

    ought(holds(hidden, taken(5)));
    ought(hidden.find(taken(5)) < hidden.find("▒"));
    same(counted(hidden, std::string{"▒"}), std::size_t{5});
    ought(!holds(hidden, kitty_send));
    ought(!holds(hidden, "a=p"));

    rig.root.covered = false;

    const std::string back = rig.drawn();

    ought(holds(back, stood_at("2;3", 5)));
    ought(!holds(back, kitty_send));
    ought(!holds(back, "a=d"));
}

TRIAL("Term: przesunięty obraz kitty schodzi i staje w nowym miejscu, bez wysyłania") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.at = {5, 2};

    const std::string said = rig.drawn();

    ought(holds(said, taken(5)));
    ought(holds(said, stood_at("3;6", 5)));
    ought(!holds(said, kitty_send));
}

TRIAL("Term: klatki animacji kitty wysyłają się raz, potem tylko się przestawiają") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.mark = 6;
    rig.root.still = 1;

    const std::string second = rig.drawn();

    ought(holds(second, taken(5)));
    ought(holds(second, sent_whole(6, *rig.root.pixels, 1)));
    ought(holds(second, stood_at("2;3", 6)));
    ought(!holds(second, taken(5, 'I')));

    rig.root.mark = 5;
    rig.root.still = 0;

    const std::string again = rig.drawn();

    ought(holds(again, taken(6)));
    ought(holds(again, stood_at("2;3", 5)));
    ought(!holds(again, kitty_send));
}

TRIAL("Term: obraz kitty, którego pikseli nikt nie trzyma, terminal zapomina od razu") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.pixels = dotted({3, 2});
    rig.root.mark = 8;

    const std::string said = rig.drawn();

    ought(holds(said, taken(5)));
    ought(holds(said, taken(5, 'I')));
    ought(holds(said, sent_whole(8, *rig.root.pixels, 0)));

    // Zasłonięty nie jest zapominany: jego piksele wciąż ktoś trzyma.
    rig.root.covered = true;

    ought(!holds(rig.drawn(), taken(8, 'I')));
}

TRIAL("Term: duży obraz kitty idzie porcjami po 4096 znaków, m=0 tylko na końcu") {
    Rig rig{kittied()};

    rig.root.pixels = dotted({40, 40});

    const std::string said = rig.drawn();
    const std::string coded = base64(rig.root.pixels->stills()[0].bytes);

    same(coded.size(), std::size_t{8536});

    const std::string wanted = "\x1b_Ga=t,f=32,s=40,v=40,i=5,q=2,m=1;" + coded.substr(0, 4096) + "\x1b\\" +
                               "\x1b_Gq=2,m=1;" + coded.substr(4096, 4096) + "\x1b\\" + "\x1b_Gq=2,m=0;" +
                               coded.substr(8192) + "\x1b\\";

    ought(holds(said, wanted));
    same(counted(said, "m=0;"), std::size_t{1});
}

TRIAL("Term: base64 dzielony równo na porcje nie dostaje pustej porcji na końcu") {
    Rig rig{kittied()};

    rig.root.pixels = dotted({48, 32});

    const std::string said = rig.drawn();
    const std::string coded = base64(rig.root.pixels->stills()[0].bytes);

    same(coded.size(), std::size_t{8192});
    ought(holds(said, "\x1b_Gq=2,m=0;" + coded.substr(4096) + "\x1b\\"));
    same(counted(said, "\x1b_G"), std::size_t{3});
}

TRIAL("Term: klatka spoza pikseli obrazu nie idzie do kitty") {
    Rig rig{kittied()};

    rig.root.still = 5;

    const std::string said = rig.drawn();

    ought(!holds(said, "\x1b_G"));
    ought(!holds(said, std::string{"▒"}));
}

TRIAL("Term: kitty stawia obraz także w ostatnim wierszu") {
    Rig rig{kittied(), {12, 3}};

    ought(holds(rig.drawn(), stood_at("2;3", 5)));
}

TRIAL("Term: odświeżenie ekranu zdejmuje obrazy kitty i stawia je bez wysyłania") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.term->reconfigure(kittied());
    (void)rig.term->loop().turn();

    const std::string said = rig.drawn();

    ought(holds(said, "\x1b_Ga=d,d=a,q=2\x1b\\"));
    ought(said.find("\x1b_Ga=d,d=a") < said.find("\x1b[2J"));
    ought(holds(said, stood_at("2;3", 5)));
    ought(!holds(said, kitty_send));
}

TRIAL("Term: przejście z kitty na znaki zdejmuje obraz, a terminal zapomina go, gdy piksele odejdą") {
    Rig rig{kittied()};

    (void)rig.drawn();

    Setup none = kittied();

    none.graphics = Graphics::none;

    rig.term->reconfigure(none);
    (void)rig.term->loop().turn();

    const std::string said = rig.drawn();

    ought(holds(said, "\x1b_Ga=d,d=a"));
    ought(holds(said, std::string{"▒"}));
    ought(!holds(said, "a=p"));

    rig.root.pixels = dotted({3, 2});

    ought(holds(rig.drawn(), taken(5, 'I')));
}

TRIAL("Term: wyjście i oddanie terminala zwalniają wszystkie obrazy kitty") {
    auto line = faking::terminal();

    line->window = ui::Size{12, 6};

    Framed root;

    {
        Term term{kittied(), Seam<Console>{line}};

        term.show(root);
        term.draw();

        {
            line->written.clear();

            const Term::Pause away{term};

            ought(holds(line->written, "\x1b_Ga=d,d=A,q=2\x1b\\"));
        }

        line->written.clear();
        term.draw();

        // Po powrocie obraz idzie od nowa — terminal go zapomniał.
        ought(holds(line->written, sent_whole(5, *root.pixels, 0)));

        line->written.clear();
    }

    ought(holds(line->written, "\x1b_Ga=d,d=A,q=2\x1b\\"));
}

TRIAL("Term: bez obrazów kitty wyjście nie wysyła nic o grafice") {
    auto line = faking::terminal();

    {
        Term term{Setup{}, Seam<Console>{line}};

        line->written.clear();
    }

    ought(!holds(line->written, "\x1b_G"));
}

TRIAL("Image: przy kitty obrazy są gotowe od razu, z pikselami pola, po jednym na klatkę") {
    Loop loop;
    Sheet top{Graphics::kitty};

    Bytes red;

    for (int at = 0; at < 40 * 40; ++at)
        for (const std::uint8_t one : {std::uint8_t{200}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{255}})
            red.push_back(std::byte{one});

    top.image->hold(Pixels{{40, 40}, {Still{red, std::chrono::milliseconds{100}}, Still{red, std::chrono::milliseconds{100}}}});

    const View made = top.render({{0, 0}, {20, 6}});

    ought(!top.image->busy());
    same(made.pictures().size(), std::size_t{1});

    const Picture& one = made.pictures().front();

    ought(one.pixels != nullptr);
    ought(one.sixel == nullptr);
    ought(one.pixels->size() == (ui::Size{100, 100}));
    same(one.pixels->stills().size(), std::size_t{2});
    ought(one.size == (ui::Size{10, 5}));

    // Druga klatka to inny znak na tych samych pikselach.
    top.image->play(false);
    top.image->still(1);

    const View later = top.render({{0, 0}, {20, 6}});
    const Picture& two = later.pictures().front();

    ought(two.mark != one.mark);
    ought(two.pixels == one.pixels);
    same(two.still, std::size_t{1});

    (void)loop;
}

TRIAL("Image: przejście sixel ↔ kitty w biegu przelicza obraz") {
    Loop loop;
    Sheet top{Graphics::sixel};

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));
    ought(top.render({{0, 0}, {20, 6}}).pictures().front().sixel != nullptr);

    top.graphics(Graphics::kitty);

    const View kitty = top.render({{0, 0}, {20, 6}});

    same(kitty.pictures().size(), std::size_t{1});
    ought(kitty.pictures().front().pixels != nullptr);

    top.graphics(Graphics::sixel);

    ought(settled(loop, top, *top.image));
    ought(top.render({{0, 0}, {20, 6}}).pictures().front().sixel != nullptr);
}

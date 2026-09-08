#include "proving.hpp"

#include <string>

#include "talcite/view.hpp"

using namespace talcite;

namespace {

// Styl inny niż domyślny, żeby dało się sprawdzić, że jedzie razem ze znakiem.
Style marked() {
    Style made;
    made.bold = true;
    made.ink = Color{false, 200, 100, 50};

    return made;
}

// Kartka wypełniona jedną literą — najkrótszy sposób na dziecko do wklejania.
View painted(ui::Size how, char32_t what) {
    View made{how};

    for (int y = 0; y < how.high; ++y)
        for (int x = 0; x < how.wide; ++x) made.letter({x, y}, what, Style{});

    return made;
}

}

TRIAL("View: świeża kartka ma rozmiar i same spacje") {
    const View one{{4, 2}};

    same(one.size().wide, 4);
    same(one.size().high, 2);
    same(one.at({0, 0}).letter, U' ');
    same(one.at({3, 1}).letter, U' ');
}

TRIAL("View: ujemny wymiar schodzi do zera, a nie do wyjątku") {
    const View one{{-5, 3}};

    same(one.size().wide, 0);
    same(one.size().high, 3);
    same(one.at({0, 0}).letter, U' ');
}

TRIAL("View: litera staje tam, gdzie kazano") {
    View one{{4, 2}};

    one.letter({2, 1}, U'A', marked());

    same(one.at({2, 1}).letter, U'A');
    ought(one.at({2, 1}).style.bold);
    same(one.at({1, 1}).letter, U' ');
}

TRIAL("View: pisanie poza kartką nic nie robi") {
    View one{{2, 2}};

    one.letter({5, 0}, U'A', Style{});
    one.letter({-1, 0}, U'B', Style{});
    one.letter({0, 9}, U'C', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 1}).letter, U' ');
}

TRIAL("View: napis idzie w prawo") {
    View one{{6, 1}};

    one.string({1, 0}, "abc", Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.at({3, 0}).letter, U'c');
}

TRIAL("View: napis z ogonkami liczy znaki, nie bajty") {
    View one{{4, 1}};

    one.string({0, 0}, "żółw", Style{});

    same(one.at({0, 0}).letter, U'ż');
    same(one.at({1, 0}).letter, U'ó');
    same(one.at({2, 0}).letter, U'ł');
    same(one.at({3, 0}).letter, U'w');
}

TRIAL("View: napis urywa się na krawędzi") {
    View one{{3, 1}};

    one.string({1, 0}, "abcdef", Style{});

    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.size().wide, 3);
}

TRIAL("View: popsuty UTF-8 daje znak zastępczy zamiast zniknąć") {
    View one{{4, 1}};

    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    one.string({0, 0}, said, Style{});

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, U'�');
    same(one.at({2, 0}).letter, U'b');
}

TRIAL("View: znak podwójnej szerokości zajmuje dwie kratki") {
    View one{{4, 1}};

    one.letter({1, 0}, U'漢', Style{});

    same(one.at({1, 0}).letter, U'漢');
    same(one.at({2, 0}).letter, static_cast<char32_t>(0));
}

TRIAL("View: podwójny znak na ostatniej kolumnie zostaje spacją") {
    View one{{3, 1}};

    one.letter({2, 0}, U'漢', Style{});

    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: nadpisanie połowy podwójnego znaku sprząta drugą połowę") {
    View one{{4, 1}};

    one.letter({0, 0}, U'漢', Style{});
    one.letter({1, 0}, U'x', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'x');
}

TRIAL("View: nadpisanie pierwszej połowy sprząta ciąg dalszy") {
    View one{{4, 1}};

    one.letter({1, 0}, U'漢', Style{});
    one.letter({1, 0}, U'x', Style{});

    same(one.at({1, 0}).letter, U'x');
    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: fill zamalowuje prostokąt i nie wychodzi poza niego") {
    View one{{4, 3}};

    one.fill({{1, 1}, {2, 1}}, marked());

    ought(one.at({1, 1}).style.bold);
    ought(one.at({2, 1}).style.bold);
    ought(!one.at({0, 1}).style.bold);
    ought(!one.at({1, 0}).style.bold);
}

TRIAL("View: cień zmienia styl, ale zostawia literę spod spodu") {
    View one = painted({4, 1}, U'x');

    View two{{4, 1}};
    two.bare({{0, 0}, {4, 1}});

    Style dark;
    dark.faint = true;

    two.shade({{1, 0}, {2, 1}}, dark);

    one.within(two, {0, 0});

    same(one.at({0, 0}).letter, U'x');
    ought(!one.at({0, 0}).style.faint);
    same(one.at({1, 0}).letter, U'x');
    ought(one.at({1, 0}).style.faint);
    ought(one.at({2, 0}).style.faint);
    ought(!one.at({3, 0}).style.faint);
}

TRIAL("View: cień nad pustką rysuje się jak zwykła kratka") {
    View one{{2, 1}};

    View two{{2, 1}};

    Style dark;
    dark.faint = true;

    two.shade({{0, 0}, {2, 1}}, dark);

    one.within(two, {0, 0});

    same(one.at({0, 0}).letter, U' ');
    ought(one.at({0, 0}).style.faint);
}

// **Cień nad pustką musi zostać cieniem**, a nie zniknąć. Kratka, na której nikt jeszcze nic nie
// narysował, mówi „mnie tu nie ma", a wklejenie takie pomija — więc przyciemnienie przepadało
// piętro wyżej i cień pod dialogiem urywał się w połowie. Wyszło to dopiero, gdy pojemniki
// przestały zamalowywać swoje prostokąty; wcześniej pustki pod cieniem po prostu nie było.
TRIAL("View: cień przez pustkę dochodzi piętro wyżej") {
    View under = painted({3, 1}, U'x');

    // Piętro środkowe: samo nic.
    View middle{{3, 1}};
    middle.bare({{0, 0}, {3, 1}});

    // Piętro górne: sam cień.
    View above{{3, 1}};
    above.bare({{0, 0}, {3, 1}});

    Style dark;
    dark.faint = true;

    above.shade({{1, 0}, {1, 1}}, dark);

    middle.within(above, {0, 0});
    under.within(middle, {0, 0});

    same(under.at({1, 0}).letter, U'x');
    ought(under.at({1, 0}).style.faint);
    ought(!under.at({0, 0}).style.faint);
    ought(!under.at({2, 0}).style.faint);
}

TRIAL("View: kratka nierysowana nie zmienia niczego") {
    View one = painted({3, 1}, U'x');

    View two{{3, 1}};
    two.letter({0, 0}, U'a', marked());
    two.bare({{1, 0}, {2, 1}});

    one.within(two, {0, 0});

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, U'x');
    same(one.at({2, 0}).letter, U'x');
    ought(!one.at({1, 0}).style.bold);
}

TRIAL("View: zamalowanie po pustce znów zasłania") {
    View one = painted({2, 1}, U'x');

    View two{{2, 1}};
    two.bare({{0, 0}, {2, 1}});
    two.letter({1, 0}, U'b', Style{});

    one.within(two, {0, 0});

    same(one.at({0, 0}).letter, U'x');
    same(one.at({1, 0}).letter, U'b');
}

TRIAL("View: wklejenie stawia cudzą kartkę pod przesunięciem") {
    View one{{6, 4}};

    one.within(painted({2, 2}, U'x'), {3, 1});

    same(one.at({3, 1}).letter, U'x');
    same(one.at({4, 2}).letter, U'x');
    same(one.at({2, 1}).letter, U' ');
    same(one.at({3, 3}).letter, U' ');
}

TRIAL("View: wklejenie przycina to, co wystaje") {
    View one{{3, 2}};

    one.within(painted({4, 4}, U'x'), {2, 1});

    same(one.at({2, 1}).letter, U'x');
    same(one.size().wide, 3);
    same(one.size().high, 2);
}

TRIAL("View: wklejenie z ujemnym przesunięciem bierze tylko widoczną część") {
    View one{{3, 2}};

    View two{{3, 2}};
    two.letter({0, 0}, U'a', Style{});
    two.letter({2, 1}, U'b', Style{});

    one.within(two, {-1, -1});

    same(one.at({1, 0}).letter, U'b');
    same(one.at({0, 0}).letter, U' ');
}

TRIAL("View: wklejenie zupełnie obok nie rusza kartki") {
    View one{{3, 2}};

    one.within(painted({2, 2}, U'x'), {9, 9});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({2, 1}).letter, U' ');
}

TRIAL("View: brzeg wklejenia nie zostawia połowy podwójnego znaku") {
    View one{{3, 1}};

    View two{{4, 1}};
    two.letter({2, 0}, U'漢', Style{});

    one.within(two, {0, 0});

    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: pytanie o kratkę spoza kartki oddaje pustą") {
    const View one{{2, 2}};

    same(one.at({9, 9}).letter, U' ');
    same(one.at({-1, 0}).letter, U' ');
}

#include "proving.hpp"

#include <string>
#include <string_view>

#include "talcite/encoding.hpp"
#include "talcite/view.hpp"

using namespace talcite;

namespace {

// Styl inny niż domyślny, żeby dało się sprawdzić, że jedzie razem ze znakiem.
Style marked() {
    Style made;
    made.bold = true;
    made.foreground = Color{false, 200, 100, 50};

    return made;
}

// Kartka wypełniona jedną literą — najkrótszy sposób na dziecko do wklejania.
View painted(ui::Size how, char32_t what) {
    View made{how};

    for (int y = 0; y < how.high; ++y)
        for (int x = 0; x < how.wide; ++x) made.put({x, y}, what, Style{});

    return made;
}

}

// `put` ma dwa przeciążenia: znak i napis. Każdy zapis, jaki pisze się odruchowo — literał znaku,
// literał napisu, `std::string` — musi trafić tam, gdzie znaczy, bez rzutowania.
TRIAL("View: put rozróżnia znak od napisu") {
    View one{{6, 1}};

    one.put({0, 0}, U'ż', marked());
    one.put({1, 0}, 'a', Style{});
    one.put({2, 0}, "bc", Style{});
    one.put({4, 0}, std::string{"d"}, Style{});
    one.put({5, 0}, std::string_view{"e"}, Style{});

    same(one.at({0, 0}).letter, U'ż');
    ought(one.at({0, 0}).style == marked());
    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.at({3, 0}).letter, U'c');
    same(one.at({4, 0}).letter, U'd');
    same(one.at({5, 0}).letter, U'e');
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

    one.put({2, 1}, U'A', marked());

    same(one.at({2, 1}).letter, U'A');
    ought(one.at({2, 1}).style.bold);
    same(one.at({1, 1}).letter, U' ');
}

TRIAL("View: pisanie poza kartką nic nie robi") {
    View one{{2, 2}};

    one.put({5, 0}, U'A', Style{});
    one.put({-1, 0}, U'B', Style{});
    one.put({0, 9}, U'C', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 1}).letter, U' ');
}

TRIAL("View: napis idzie w prawo") {
    View one{{6, 1}};

    one.put({1, 0}, "abc", Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.at({3, 0}).letter, U'c');
}

TRIAL("View: napis z ogonkami liczy znaki, nie bajty") {
    View one{{4, 1}};

    one.put({0, 0}, "żółw", Style{});

    same(one.at({0, 0}).letter, U'ż');
    same(one.at({1, 0}).letter, U'ó');
    same(one.at({2, 0}).letter, U'ł');
    same(one.at({3, 0}).letter, U'w');
}

TRIAL("View: napis urywa się na krawędzi") {
    View one{{3, 1}};

    one.put({1, 0}, "abcdef", Style{});

    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.size().wide, 3);
}

TRIAL("View: popsuty UTF-8 daje znak zastępczy zamiast zniknąć") {
    View one{{4, 1}};

    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    one.put({0, 0}, said, Style{});

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, U'�');
    same(one.at({2, 0}).letter, U'b');
}

TRIAL("View: znak podwójnej szerokości zajmuje dwie kratki") {
    View one{{4, 1}};

    one.put({1, 0}, U'漢', Style{});

    same(one.at({1, 0}).letter, U'漢');
    same(one.at({2, 0}).letter, static_cast<char32_t>(0));
}

TRIAL("View: podwójny znak na ostatniej kolumnie zostaje spacją") {
    View one{{3, 1}};

    one.put({2, 0}, U'漢', Style{});

    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: nadpisanie połowy podwójnego znaku sprząta drugą połowę") {
    View one{{4, 1}};

    one.put({0, 0}, U'漢', Style{});
    one.put({1, 0}, U'x', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'x');
}

TRIAL("View: nadpisanie pierwszej połowy sprząta ciąg dalszy") {
    View one{{4, 1}};

    one.put({1, 0}, U'漢', Style{});
    one.put({1, 0}, U'x', Style{});

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
    two.clear({{0, 0}, {4, 1}});

    Style dark;
    dark.faint = true;

    two.shade({{1, 0}, {2, 1}}, dark);

    one.draw(two, {0, 0});

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

    one.draw(two, {0, 0});

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
    middle.clear({{0, 0}, {3, 1}});

    // Piętro górne: sam cień.
    View above{{3, 1}};
    above.clear({{0, 0}, {3, 1}});

    Style dark;
    dark.faint = true;

    above.shade({{1, 0}, {1, 1}}, dark);

    middle.draw(above, {0, 0});
    under.draw(middle, {0, 0});

    same(under.at({1, 0}).letter, U'x');
    ought(under.at({1, 0}).style.faint);
    ought(!under.at({0, 0}).style.faint);
    ought(!under.at({2, 0}).style.faint);
}

// **Sam pierwszy plan** (kratka brzegu obrazu): znak i jego barwa własne, tło spod spodu. Napis tak nie
// robi — jego rola podaje własne tło — więc to osobny rodzaj kratki, a nie nowa reguła wklejania.
TRIAL("View: pierwszy plan bierze tło spodu, a znak i barwę ma swoje") {
    View one{{2, 1}};

    Style blue;

    blue.background = Color{false, 0, 0, 160};

    one.fill({{0, 0}, {2, 1}}, blue);

    View two{{2, 1}};
    two.clear({{0, 0}, {2, 1}});

    Style red;

    red.foreground = Color{false, 200, 0, 0};

    two.foreground({0, 0}, U'▌', red);

    // Znak zerowy to brak znaku — bez niego pierwszy plan niczego nie kładzie.
    two.foreground({1, 0}, 0, red);

    same(static_cast<int>(two.at({1, 0}).shown), static_cast<int>(Shown::none));

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'▌');
    same(one.at({0, 0}).style.foreground, (Color{false, 200, 0, 0}));
    same(one.at({0, 0}).style.background, (Color{false, 0, 0, 160}));
    same(static_cast<int>(one.at({0, 0}).shown), static_cast<int>(Shown::own));
    same(one.at({1, 0}).style.background, (Color{false, 0, 0, 160}));

    // Zwykła kratka z tym samym stylem zasłania tło spodu przezroczystym — tak jak napis.
    View three{{2, 1}};
    three.clear({{0, 0}, {2, 1}});
    three.put({0, 0}, U'▌', red);

    one.draw(three, {0, 0});

    ought(one.at({0, 0}).style.background.transparent);
}

TRIAL("View: pierwszy plan przez pustkę dochodzi piętro wyżej, a z podanym tłem jest zwykłą kratką") {
    View under{{2, 1}};

    Style blue;

    blue.background = Color{false, 0, 0, 160};

    under.fill({{0, 0}, {2, 1}}, blue);

    View middle{{2, 1}};
    middle.clear({{0, 0}, {2, 1}});

    View above{{2, 1}};
    above.clear({{0, 0}, {2, 1}});

    Style red;

    red.foreground = Color{false, 200, 0, 0};

    Style papered = red;

    papered.background = Color{false, 0, 120, 0};

    above.foreground({0, 0}, U'▐', red);
    above.foreground({1, 0}, U'▐', papered);

    same(static_cast<int>(above.at({0, 0}).shown), static_cast<int>(Shown::foreground));
    same(static_cast<int>(above.at({1, 0}).shown), static_cast<int>(Shown::own));

    middle.draw(above, {0, 0});

    same(static_cast<int>(middle.at({0, 0}).shown), static_cast<int>(Shown::foreground));

    under.draw(middle, {0, 0});

    same(under.at({0, 0}).letter, U'▐');
    same(under.at({0, 0}).style.background, (Color{false, 0, 0, 160}));
    same(under.at({1, 0}).style.background, (Color{false, 0, 120, 0}));

    // Poza kartką pierwszy plan nie robi nic.
    above.foreground({5, 0}, U'x', red);

    same(above.size().wide, 2);
}

TRIAL("View: kratka nierysowana nie zmienia niczego") {
    View one = painted({3, 1}, U'x');

    View two{{3, 1}};
    two.put({0, 0}, U'a', marked());
    two.clear({{1, 0}, {2, 1}});

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, U'x');
    same(one.at({2, 0}).letter, U'x');
    ought(!one.at({1, 0}).style.bold);
}

TRIAL("View: zamalowanie po pustce znów zasłania") {
    View one = painted({2, 1}, U'x');

    View two{{2, 1}};
    two.clear({{0, 0}, {2, 1}});
    two.put({1, 0}, U'b', Style{});

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'x');
    same(one.at({1, 0}).letter, U'b');
}

TRIAL("View: wklejenie stawia cudzą kartkę pod przesunięciem") {
    View one{{6, 4}};

    one.draw(painted({2, 2}, U'x'), {3, 1});

    same(one.at({3, 1}).letter, U'x');
    same(one.at({4, 2}).letter, U'x');
    same(one.at({2, 1}).letter, U' ');
    same(one.at({3, 3}).letter, U' ');
}

TRIAL("View: wklejenie przycina to, co wystaje") {
    View one{{3, 2}};

    one.draw(painted({4, 4}, U'x'), {2, 1});

    same(one.at({2, 1}).letter, U'x');
    same(one.size().wide, 3);
    same(one.size().high, 2);
}

TRIAL("View: wklejenie z ujemnym przesunięciem bierze tylko widoczną część") {
    View one{{3, 2}};

    View two{{3, 2}};
    two.put({0, 0}, U'a', Style{});
    two.put({2, 1}, U'b', Style{});

    one.draw(two, {-1, -1});

    same(one.at({1, 0}).letter, U'b');
    same(one.at({0, 0}).letter, U' ');
}

TRIAL("View: wklejenie zupełnie obok nie rusza kartki") {
    View one{{3, 2}};

    one.draw(painted({2, 2}, U'x'), {9, 9});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({2, 1}).letter, U' ');
}

TRIAL("View: brzeg wklejenia nie zostawia połowy podwójnego znaku") {
    View one{{3, 1}};

    View two{{4, 1}};
    two.put({2, 0}, U'漢', Style{});

    one.draw(two, {0, 0});

    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: pytanie o kratkę spoza kartki oddaje pustą") {
    const View one{{2, 2}};

    same(one.at({9, 9}).letter, U' ');
    same(one.at({-1, 0}).letter, U' ');
}

// Nazwa pliku, tytuł z sieci, opis z NFO — tekst obcy idzie na ekran, a nie do terminala jako
// rozkaz. C0 i DEL pokazują się swoimi obrazkami, C1 i znaki kierunku pisma znakiem zastępczym,
// a szerokość zostaje jedna kratka, więc kolumny się nie rozjeżdżają.
TRIAL("View: znaki sterujące stają się widoczne, a nie wykonywane") {
    View one{{9, 1}};

    one.put({0, 0}, std::string{"a\x1b]2;\x07"} + "\xc2\x9b" + "\xe2\x80\xae" + "\x7f" + "z", Style{});

    // Liczbami, nie literałami — kompilator słusznie ostrzega przed znakami kierunku w źródle.
    constexpr char32_t substitute = 0xFFFD;

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, char32_t{0x241B});
    same(one.at({2, 0}).letter, U']');
    same(one.at({4, 0}).letter, U';');
    same(one.at({5, 0}).letter, char32_t{0x2407});
    same(one.at({6, 0}).letter, substitute);
    same(one.at({7, 0}).letter, substitute);
    same(one.at({8, 0}).letter, char32_t{0x2421});

    for (const char32_t each : {0x200Eu, 0x200Fu, 0x061Cu, 0x2028u, 0x2029u, 0x202Au, 0x202Eu, 0x2066u, 0x2069u, 0x80u, 0x9Fu})
        same(shown(each), substitute);

    for (const char32_t each : {0x20u, 0x7Eu, 0xA0u, 0x17Cu, 0x2400u, 0x206Au, 0x2027u, 0x6F22u}) same(shown(each), each);

    same(shown(0), char32_t{0x2400});
    same(shown(0x1F), char32_t{0x241F});
}

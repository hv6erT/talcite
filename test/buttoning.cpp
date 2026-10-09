#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

class Box : public ui::Node {
public:
    explicit Box(ui::Size wants) : wants_(wants) {}

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, U'x', Style{});

        return made;
    }

private:
    ui::Size wants_;
};

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

// **Napis jest czysty i rysuje się w całości.** Nie ma żadnej umowy o znakach w środku, więc
// ampersand, nawias czy cokolwiek innego są zwykłymi literami i nic nie znaczą.
TRIAL("Przycisk: napis rysuje się w całości, bez umów o znakach") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak & nie");

    top.children().add(one);

    same(one->text(), std::string{"Tak & nie"});
    same(one->wants({40, 4}), (ui::Size{13, 1}));

    const View made = top.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{"[ Tak & nie ]"});
}

namespace {

// Korzeń, który rozsyła klawisz aktywny sam — w programie robi to dialog, legenda albo menu, ale
// rozsyłanie jest chronione, więc próba potrzebuje własnego wejścia.
class Top : public ui::Root {
public:
    using ui::Root::broadcast;
};

// Czy przycisk odpowie na ten klawisz aktywny. Pytamy **drogą, którą chodzi okno** — podaniem
// zdarzenia — a nie osobną metodą do pytania, bo takiej już nie ma i nie ma jej celowo.
bool hotly(Top& top, const std::shared_ptr<ui::Button>& one, ui::Key what) {
    int done = 0;

    ui::Hook held = one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent key{what};

    (void)top.broadcast(key);

    one->off(held);

    return done != 0;
}

}

TRIAL("Przycisk: klawisze aktywne podaje wołający") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    // Bez klawiszy przycisk nie odpowiada na nic.
    ought(!hotly(top, one, ui::Key{U'T'}));

    one->keys().add(U'T');

    ought(hotly(top, one, ui::Key{U'T'}));

    // Litery bez różnicy wielkości.
    ought(hotly(top, one, ui::Key{U't'}));

    ought(!hotly(top, one, ui::Key{U'n'}));

    // Klawiszy może być kilka i nie muszą być literami.
    one->keys().add(std::vector<ui::Key>{U'T', ui::Code::f2});

    ought(hotly(top, one, ui::Key{U't'}));
    ought(hotly(top, one, ui::Key{ui::Code::f2}));
    ought(!hotly(top, one, ui::Key{ui::Code::f3}));

    // Zestaw się czyści.
    one->keys().clear();

    ought(one->keys().empty());
    ought(!hotly(top, one, ui::Key{U't'}));

    // Pusty klawisz nie jest klawiszem.
    one->keys().add(U'T');

    ought(!hotly(top, one, ui::Key{}));
}

// Zbiór klawiszy odpowiada na te same cztery słowa, co dzieci węzła — i zwija wielkość liter
// wszędzie, nie tylko przy trafianiu.
TRIAL("Przycisk: zbiór klawiszy dokłada, zdejmuje i czyści") {
    const auto one = std::make_shared<ui::Button>("Tak");

    ought(one->keys().empty());
    same(one->keys().many(), std::size_t{0});

    one->keys().add(U'T');
    one->keys().add(ui::Code::f2);

    same(one->keys().many(), std::size_t{2});
    ought(one->keys().contains(ui::Key{U'T'}));
    ought(one->keys().contains(ui::Key{U't'}));
    ought(one->keys().contains(ui::Key{ui::Code::f2}));
    ought(!one->keys().contains(ui::Key{U'n'}));
    ought(!one->keys().contains(ui::Key{}));

    // Litera nie patrzy na Shift (wielkość składa się), klawisz kodowany patrzy: Shift+F2 to inny
    // klawisz niż F2 — jak w MC, gdzie Shift+F7 powtarza szukanie, a F7 je zaczyna.
    ought(one->keys().contains(ui::Key{U't', ui::Mods{.shift = true}}));
    ought(!one->keys().contains(ui::Key{ui::Code::f2, ui::Mods{.shift = true}}));
    ought(!one->keys().contains(ui::Key{ui::Code::f2, ui::Mods{.ctrl = true}}));

    one->keys().add(ui::Key{ui::Code::f2, ui::Mods{.shift = true}});

    ought(one->keys().contains(ui::Key{ui::Code::f2, ui::Mods{.shift = true}}));
    same(one->keys().many(), std::size_t{3});

    one->keys().remove(ui::Key{ui::Code::f2, ui::Mods{.shift = true}});

    // Pusty klawisz nie wchodzi, a ten sam drugi raz nie dokłada się bez skutku.
    one->keys().add(ui::Key{});
    one->keys().add(U't');

    same(one->keys().many(), std::size_t{2});

    // Zdejmowanie też nie patrzy na wielkość litery.
    one->keys().remove(ui::Key{U't'});

    same(one->keys().many(), std::size_t{1});
    ought(!one->keys().contains(ui::Key{U'T'}));

    // Zdjęcie czegoś, czego nie ma, jest bez skutku.
    one->keys().remove(ui::Key{U'z'});

    same(one->keys().many(), std::size_t{1});

    // Po zbiorze da się chodzić.
    int much = 0;

    for (const ui::Key which : one->keys()) {
        (void)which;

        ++much;
    }

    same(much, 1);
    same(static_cast<int>(one->keys().many()), 1);
    same(one->keys().front(), one->keys().back());
    same(one->keys()[0], one->keys().front());

    // Sięgnięcie poza koniec jest błędem wołającego — tak samo jak w zbiorze opcji.
    threw((void)one->keys()[1]);

    one->keys().clear();

    threw((void)one->keys().front());

    ought(one->keys().empty());
}

TRIAL("Przycisk: wyłączony nie odpowiada na swoje klawisze") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    one->keys().add(U'T');
    one->enabled(false);

    ought(!hotly(top, one, ui::Key{U'T'}));
}

// Znakowanie jest **skutkiem**, nie osobnym ustawieniem: jeśli litera klawisza w napisie
// występuje, zostaje podkreślona; jeśli nie — przycisk i tak na ten klawisz odpowiada.
TRIAL("Przycisk: litera klawisza podkreśla się w napisie") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    one->keys().add(U'w');

    top.children().add(one);

    const View made = top.render({{0, 0}, {12, 1}});

    same(read(made, 0), std::string{"[ Przerwij ]"});

    // „[ " to dwie kratki, a „w" stoi na piątej literze napisu.
    ought(made.at({7, 0}).style.under);
    same(made.at({7, 0}).letter, U'w');

    ought(!made.at({6, 0}).style.under);
    ought(!made.at({8, 0}).style.under);
}

TRIAL("Przycisk: podkreśla się pierwsze wystąpienie, bez różnicy wielkości") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    // Wielka litera klawisza, mała w napisie — i tak się znajdzie, na pierwszym „r".
    one->keys().add(U'R');

    top.children().add(one);

    const View made = top.render({{0, 0}, {12, 1}});

    ought(made.at({3, 0}).style.under);
    same(made.at({3, 0}).letter, U'r');

    // Drugie „r" zostaje zwykłe.
    ought(!made.at({6, 0}).style.under);
}

// Wielkość składa się także poza łaciną podstawową — rosyjskie i ukraińskie menu twa podkreślają
// litery cyrylicy, a skrót z wielką literą ma znaleźć małą w napisie i odpowiadać na obie.
TRIAL("Przycisk: wielkość składa się w cyrylicy i w łacinie z ogonkami") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Карта");
    const auto two = std::make_shared<ui::Button>("Mżawka");

    one->keys().add(U'А');
    two->keys().add(U'Ż');

    top.children().add(one);

    const View made = top.render({{0, 0}, {9, 1}});

    same(made.at({3, 0}).letter, U'а');
    ought(made.at({3, 0}).style.under);
    ought(!made.at({2, 0}).style.under);

    ought(hotly(top, one, ui::Key{U'а'}));
    ought(hotly(top, one, ui::Key{U'А'}));
    ought(!hotly(top, one, ui::Key{U'a'}));

    top.children().clear();
    top.children().add(two);

    const View other = top.render({{0, 0}, {10, 1}});

    same(other.at({3, 0}).letter, U'ż');
    ought(other.at({3, 0}).style.under);

    ought(hotly(top, two, ui::Key{U'ż'}));
}

TRIAL("Przycisk: klawisz spoza napisu nie znakuje niczego") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(std::vector<ui::Key>{ui::Code::f2, U'z'});

    top.children().add(one);

    const View made = top.render({{0, 0}, {7, 1}});

    for (int x = 0; x < 7; ++x) ought(!made.at({x, 0}).style.under);

    // Ale odpowiada na oba.
    ought(hotly(top, one, ui::Key{ui::Code::f2}));
    ought(hotly(top, one, ui::Key{U'z'}));
}

TRIAL("Przycisk: rozstrzyga pierwszy klawisz, który w napisie występuje") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    // „q" w napisie nie ma, więc znakuje dopiero „i".
    one->keys().add(std::vector<ui::Key>{U'q', U'i'});

    top.children().add(one);

    const View made = top.render({{0, 0}, {12, 1}});

    same(made.at({8, 0}).letter, U'i');
    ought(made.at({8, 0}).style.under);
}

TRIAL("Przycisk: podstawowy ma szersze klamry") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    same(one->wants({40, 4}), (ui::Size{7, 1}));
    same(read(top.render({{0, 0}, {7, 1}}), 0), std::string{"[ Tak ]"});

    one->primary(true);

    same(one->wants({40, 4}), (ui::Size{9, 1}));
    same(read(top.render({{0, 0}, {9, 1}}), 0), std::string{"[< Tak >]"});
}

TRIAL("Przycisk: pod ogniskiem cały w negatywie") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(U'T');

    top.children().add(one);

    ought(!top.render({{0, 0}, {7, 1}}).at({2, 0}).style.reverse);

    one->focus();

    const View made = top.render({{0, 0}, {7, 1}});

    ought(made.at({0, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);

    // Podkreślenie zostaje także pod ogniskiem.
    ought(made.at({2, 0}).style.under);
}

// Przycisk zgłasza wysokość jeden, a wiersz daje dzieciom **swoją** wysokość — więc malowanie
// musi trzymać się jednego wiersza samo, inaczej negatyw rozlewa się na cały wiersz układu.
TRIAL("Przycisk: maluje jeden wiersz, nie tyle, ile mu dano") {
    ui::Root top;

    const auto row = std::make_shared<ui::Row>();
    const auto one = std::make_shared<ui::Button>("Tak");
    const auto tall = std::make_shared<Box>(ui::Size{4, 3});

    row->children().add(one);
    row->children().add(tall);
    top.children().add(row);

    one->focus();

    const View made = top.render({{0, 0}, {11, 3}});

    ought(made.at({0, 0}).style.reverse);

    // Drugi i trzeci wiersz nad przyciskiem nie należą do niego.
    ought(!made.at({0, 1}).style.reverse);
    ought(!made.at({0, 2}).style.reverse);
}

// Klamra jest tym, co czyni przycisk przyciskiem. Za wąski ucina **napis**, a nie klamrę —
// urwana zostawiała po sobie coś, co wyglądało jak zwykły tekst.
TRIAL("Przycisk: za wąski ucina napis, a nie klamrę") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    top.children().add(one);

    same(read(top.render({{0, 0}, {8, 1}}), 0), std::string{"[ Prz… ]"});
    same(read(top.render({{0, 0}, {5, 1}}), 0), std::string{"[ … ]"});
}

TRIAL("Przycisk: Enter naciska ten z ogniskiem, spacja nie") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    top.children().add(one);

    one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(done, 1);

    // **Spacja nie jest sprawą przycisku.** Była nią kiedyś — odruch z innego świata niż Midnight
    // Commander — i zabierała aplikacji klawisz, który w oknie z polem tekstowym należy do pisania.
    ui::KeyEvent space{U' '};

    ought(!top.dispatch(space));
    same(done, 1);

    // Zwykła litera nie jest sprawą przycisku — tym zajmuje się dialog.
    ui::KeyEvent other{U'z'};

    ought(!top.dispatch(other));
    same(done, 1);
}

// Ctrl+Enter i Alt+spacja to z założenia inne skróty. Tak samo odsiewa je legenda.
TRIAL("Przycisk: ze sterującym nie łapie") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    top.children().add(one);

    one->focus();

    ui::KeyEvent held{ui::Key{ui::Code::enter, ui::Mods{true, false, false}}};

    ought(!top.dispatch(held));

    ui::KeyEvent aside{ui::Key{U' ', ui::Mods{false, true, false}}};

    ought(!top.dispatch(aside));

    same(done, 0);
}

TRIAL("Przycisk: niedostępny nie bierze ogniska i nie odpowiada") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    one->enabled(false);

    top.children().add(one);

    ought(!one->focusable());

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    ought(top.render({{0, 0}, {7, 1}}).at({2, 0}).style.faint);

    one->pick();

    same(done, 0);
}

// Wyłączenie **oddaje ognisko**. Bez tego zostawałoby na węźle, który już nie jest chętny —
// czyli łamałoby niezmiennik, dla którego ten znacznik powstał.
TRIAL("Przycisk: wyłączenie oddaje ognisko") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->enabled(false);

    // Poza dialogiem nie ma chętnego przodka — korzeń ogniska nie bierze — więc gaśnie zupełnie.
    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    // Wyłączenie przycisku bez ogniska niczego nie rusza.
    const auto other = std::make_shared<ui::Button>("Nie");

    top.children().remove(one.get());
    top.children().add(other);

    other->focus();
    one->enabled(true);

    same(top.focused_node(), static_cast<ui::Node*>(other.get()));
}

TRIAL("Przycisk: klik bierze ognisko i naciska") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    top.children().add(one);

    (void)top.render({{0, 0}, {7, 1}});

    ui::ClickEvent what{{3, 0}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

namespace {

struct Both {
    std::shared_ptr<ui::Modals> window;
    std::shared_ptr<ui::Dialog> dialog;
    std::shared_ptr<ui::Button> first;
    std::shared_ptr<ui::Button> second;
};

Both asking(ui::Root& top, int* done) {
    Both made;

    made.window = std::make_shared<ui::Modals>();
    made.dialog = std::make_shared<ui::Dialog>();
    made.first = proving::doing(std::make_shared<ui::Button>("Tak"), [done] { ++*done; });
    made.second = std::make_shared<ui::Button>("Nie");

    made.first->keys().add(U'T');
    made.second->keys().add(U'N');

    const auto column = std::make_shared<ui::Column>();

    column->children().add(made.first);
    column->children().add(made.second);
    made.dialog->children().add(column);

    top.children().add(made.window);
    made.window->open(made.dialog);

    (void)top.render({{0, 0}, {20, 10}});

    return made;
}

}

// **Granica wędrówki wychodzi z modalności.** Tab bąbelkuje jak każdy klawisz, a obsługuje go
// dialog — więc nigdy z niego nie ucieka.
TRIAL("Dialog: Tab chodzi po chętnych i zawija") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));
}

TRIAL("Dialog: Tab z przesuwnikiem chodzi wstecz") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    ui::KeyEvent back{ui::Key{ui::Code::tab, ui::Mods{false, false, true}}};

    ought(top.dispatch(back));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ought(top.dispatch(back));
    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));
}

TRIAL("Dialog: Tab nie wychodzi poza dialog") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto outside = std::make_shared<Box>(ui::Size{10, 3});
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto one = std::make_shared<ui::Button>("Tak");

    dialog->children().add(one);
    window->content(outside);
    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {20, 10}});

    ui::KeyEvent tab{ui::Code::tab};

    for (int turn = 0; turn < 4; ++turn) {
        ought(top.dispatch(tab));

        // Jedyny chętny w dialogu, więc ognisko z niego nie schodzi — a już na pewno nie na to,
        // co leży pod spodem.
        same(top.focused_node(), static_cast<ui::Node*>(one.get()));
    }
}

// **Oznaczenie podstawowego jest tylko rysunkiem.** Okno nie szuka nikogo po drzewie i nie naciska
// niczego za wołającego: Enter należy do tego, kto ma ognisko, a jak ognisko stoi poza przyciskami,
// to Enter nie naciska nic. Kto chce „Enter zatwierdza formularz", wiesza uchwyt tam, gdzie ognisko
// naprawdę stoi — na polu — i sam wie, co ma się wtedy stać.
TRIAL("Dialog: Enter nie naciska podstawowego, gdy ognisko stoi poza przyciskiem") {
    ui::Root top;

    int done = 0;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto aside = std::make_shared<Box>(ui::Size{6, 1});
    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    one->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(aside);
    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    // Ognisko stawiamy poza przyciskami — tak jak stoi w polu tekstowym.
    aside->focus();

    ui::KeyEvent enter{ui::Code::enter};

    // Zasłona i tak połyka wszystko, czego nikt nie wziął — modalność zostaje.
    ought(top.dispatch(enter));
    same(done, 0);
}

// A gdy ognisko stoi **na przycisku**, Enter naciska ten przycisk. Przycisk bierze klawisz
// pierwszy, bo leży bliżej ogniska — i to jest jedyna droga, którą Enter kogokolwiek naciska.
TRIAL("Dialog: Enter na przycisku naciska jego, nie podstawowy") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    int other = 0;

    (void)both.second->on<ui::PickEvent>([&other](ui::PickEvent&) {
        ++other;

        return true;
    });
    both.first->primary(true);

    both.second->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));

    same(other, 1);
    same(done, 0);
}

// Otwarcie stawia ognisko na **pierwszym chętnym**, a nie na przycisku podstawowym. W oknie
// zaczynającym się polem tekstowym pierwsza wpisana litera bywała inaczej klawiszem aktywnym
// przycisku i zamykała okno z pustym polem. Enter dalej naciska podstawowy — to osobna sprawa
// i pilnuje jej próba obok.
TRIAL("Dialog: otwarcie staje na pierwszym chętnym") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto first = std::make_shared<ui::Button>("Tak");
    const auto second = std::make_shared<ui::Button>("Nie");

    second->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(first);
    column->children().add(second);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// Po to ta reguła jest: okno z polem tekstowym ma zaczynać z ogniskiem w polu, choćby przycisk
// podstawowy stał tuż obok.
TRIAL("Dialog: otwarcie z polem tekstowym staje na polu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto field = std::make_shared<ui::Field>(20);
    const auto yes = std::make_shared<ui::Button>("Szukaj");

    yes->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(field);
    column->children().add(yes);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(field.get()));

    // Enter zostaje przy ognisku: przycisk oznaczony jako podstawowy nie jest przez to naciskany.
    int done = 0;

    (void)yes->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(done, 0);
}

TRIAL("Dialog: klawisz aktywny naciska z dowolnego miejsca okna") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    // Ognisko stoi na „Tak", a naciskamy jego klawisz małą literą.
    ui::KeyEvent small{U't'};

    ought(top.dispatch(small));
    same(done, 1);

    ui::KeyEvent large{U'T'};

    ought(top.dispatch(large));
    same(done, 2);

    // Klawisz, którego żaden przycisk nie ma, nic nie robi — ale dialog i tak go połyka.
    ui::KeyEvent other{U'z'};

    ought(top.dispatch(other));
    same(done, 2);
}

TRIAL("Dialog: niedostępny przycisk wypada z wędrówki i z klawiszy") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    both.first->enabled(false);

    // Ognisko zeszło z wyłączonego przycisku na **najbliższego chętnego przodka**, czyli na sam
    // dialog. Gdyby zgasło zupełnie, klawisz szedłby prosto do korzenia i zamknięte okno
    // przestałoby być zamknięte.
    same(top.focused_node(), static_cast<ui::Node*>(both.dialog.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ui::KeyEvent small{U't'};

    ought(top.dispatch(small));
    same(done, 0);
}

// Gdy w dialogu nie ma nic, co bierze ognisko, bierze je sam dialog — i wtedy to on połyka
// klawisze, zamiast zostawiać je aplikacji pod spodem.
TRIAL("Dialog: bez chętnych w środku ognisko zostaje na dialogu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();

    dialog->children().add(std::make_shared<ui::Column>());

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(dialog.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(dialog.get()));
}

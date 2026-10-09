#include "proving.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// Korzeń, który liczy klawisze — po to, żeby dało się sprawdzić, co pozycja oddaje wyżej, a co
// zatrzymuje u siebie.
class Sheet : public ui::Root {
public:
    int keys = 0;

protected:
    bool handle(ui::KeyEvent&) override {
        ++keys;

        return true;
    }
};

// Pojemnik układający dzieci w wiersze, każdemu cała szerokość. Tyle robi z pozycjami lista,
// a tu wystarczy sam ten kawałek — listy jeszcze nie ma.
class Nest : public ui::Node {
public:
    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] int capacity() const override { return ui::plenty; }

    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        int y = 0;

        for (const std::shared_ptr<ui::Node>& one : children()) {
            const ui::Area where{{0, y}, {given.wide, 1}};
            const ui::Area part = ui::Area::crossed(seen, where);

            made.draw(one->render(where, part - where.at), part.at - seen.at);

            ++y;
        }

        return made;
    }
};

// Chętny liść do sprawdzania, dokąd ognisko wychodzi i skąd wraca.
class Perch : public ui::Node {
public:
    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return {1, 1}; }
};

std::shared_ptr<ui::List> listed(int much, int wide = ui::plenty, int high = ui::plenty) {
    const auto made = std::make_shared<ui::List>(wide, high);

    for (int which = 0; which < much; ++which)
        made->children().add(std::make_shared<ui::ListItem>("wiersz " + std::to_string(which)));

    return made;
}

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

TRIAL("ListItem: chce tyle, ile ma napisu, i zawsze jeden wiersz") {
    const auto one = std::make_shared<ui::ListItem>("katalog");

    same(one->wants({20, 3}), (ui::Size{7, 1}));

    // Ciasno: przycina się do tego, co dostało.
    same(one->wants({4, 3}), (ui::Size{4, 1}));

    // Wiersz listy, który się zawija, to nie wiersz listy.
    same(one->wants({20, 0}), (ui::Size{7, 0}));
}

TRIAL("ListItem: napis stoi od pierwszej kratki, bez odstępu") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    const View made = top.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"plik"});
}

TRIAL("ListItem: napis dłuższy niż wiersz przycina się wielokropkiem z prawej") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("dokumenty");

    nest->children().add(one);
    top.children().add(nest);

    const View made = top.render({{0, 0}, {5, 1}});

    same(read(made, 0), std::string{"doku…"});
}

// Podświetlenie idzie przez **całą szerokość wiersza**, a nie tylko pod napisem — inaczej pasek
// wyboru urywałby się w połowie i wyglądałby jak błąd rysowania.
TRIAL("ListItem: pod ogniskiem odwraca się cały wiersz") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    View made = top.render({{0, 0}, {8, 1}});

    ought(!made.at({0, 0}).style.reverse);
    ought(!made.at({7, 0}).style.reverse);

    one->focus();

    made = top.render({{0, 0}, {8, 1}});

    ought(made.at({0, 0}).style.reverse);
    ought(made.at({7, 0}).style.reverse);
}

TRIAL("ListItem: wyłączona gaśnie i ogniska nie bierze") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    ought(one->focusable());

    one->enabled(false);

    ought(!one->focusable());

    // Wołanie na węźle, który ogniska nie bierze, nic nie robi — po cichu, jak w korzeniu.
    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    const View made = top.render({{0, 0}, {8, 1}});

    ought(made.at({0, 0}).style.faint);
}

// Ta sama reguła co w przycisku i w polu: ognisko idzie do najbliższego chętnego przodka, a nie
// na pustkę — bo na pustce dialog przestaje połykać klawisze.
TRIAL("ListItem: wyłączenie z ogniskiem oddaje je przodkowi") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->enabled(false);

    same(top.focused_node(), static_cast<ui::Node*>(nest.get()));
}

TRIAL("ListItem: Enter woła zadanie") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    ought(top.dispatch(what));

    same(done, 1);
    same(top.keys, 0);
}

// Enter ze sterującym nie należy do pozycji — tak samo jak w polu i w legendzie.
TRIAL("ListItem: Enter ze sterującym idzie wyżej") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent what{ui::Key{ui::Code::enter, ui::Mods{true, false, false}}};

    ought(top.dispatch(what));

    same(done, 0);
    same(top.keys, 1);
}

// Strzałki należą do listy, a nie do pozycji, więc pozycja ich nie zatrzymuje.
TRIAL("ListItem: strzałka idzie wyżej") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent what{ui::Code::down};

    ought(top.dispatch(what));

    same(top.keys, 1);
}

// **To jest cały powód, dla którego pozycja bierze ognisko.** Klawisz zaczyna u niej, więc dopięty
// uchwyt widzi go przed kimkolwiek wyżej — i przed wędrówką po wierszach, gdy dojdzie lista.
//
// „Przed" znaczy tu **wcześniej**, a nie „zamiast": prawda z uchwytu zamyka drogę uchwytów, ale
// `handle` przodka biegnie swoją. Widać to na liczniku korzenia: w chwili, gdy uchwyt się odzywa,
// jest jeszcze zerowy, a po wszystkim wynosi jeden.
TRIAL("ListItem: dopięty uchwyt widzi klawisz przed przodkiem") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    int seen = 0;
    int meanwhile = -1;

    one->on<ui::KeyEvent>([&seen, &meanwhile, &top](ui::KeyEvent&) {
        ++seen;
        meanwhile = top.keys;

        return true;
    });

    one->focus();

    ui::KeyEvent what{ui::Code::down};

    ought(top.dispatch(what));

    same(seen, 1);
    same(meanwhile, 0);
    same(top.keys, 1);
}

TRIAL("ListItem: klik bierze ognisko") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto first = std::make_shared<ui::ListItem>("jeden");
    const auto other = std::make_shared<ui::ListItem>("dwa");

    nest->children().add(first);
    nest->children().add(other);
    top.children().add(nest);

    (void)top.render({{0, 0}, {8, 2}});

    ui::ClickEvent what{{2, 1}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(other.get()));
}

TRIAL("ListItem: dwuklik bierze ognisko i woła zadanie") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto first = std::make_shared<ui::ListItem>("jeden");
    const auto other = proving::doing(std::make_shared<ui::ListItem>("dwa"), [&done] { ++done; });

    nest->children().add(first);
    nest->children().add(other);
    top.children().add(nest);

    first->focus();

    (void)top.render({{0, 0}, {8, 2}});

    ui::DoubleClickEvent what{{2, 1}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(other.get()));
    same(done, 1);
}

// Pozycja niedostępna **przepuszcza klik wyżej**: skoro nie odpowiada na swój klawisz, to nie ma
// powodu, żeby odpowiadała na mysz — a lista pod spodem ma prawo zrobić z tym kliknięciem, co chce.
TRIAL("ListItem: wyłączona przepuszcza klik i ogniska nie bierze") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    one->enabled(false);

    nest->children().add(one);
    top.children().add(nest);

    (void)top.render({{0, 0}, {8, 1}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(!top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    ui::DoubleClickEvent twice{{2, 0}, ui::Mouse::left};

    ought(!top.dispatch(twice));

    same(done, 0);
}

TRIAL("ListItem: pick na wyłączonej nic nie robi") {
    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    one->pick();

    same(done, 1);

    one->enabled(false);
    one->pick();

    same(done, 1);
}

TRIAL("ListItem: napis i zadanie da się podmienić po drodze") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("stary");

    nest->children().add(one);
    top.children().add(nest);

    one->text("nowy");

    same(one->text(), std::string{"nowy"});

    const View made = top.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"nowy"});

    int done = 0;

    (void)one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
    one->pick();

    same(done, 1);
}

TRIAL("ListItem: nie przyjmuje dzieci") {
    const auto one = std::make_shared<ui::ListItem>("plik");

    same(one->capacity(), 0);

    threw(one->children().add(std::make_shared<ui::ListItem>("inne")));
}

TRIAL("List: ognisko idzie na pozycję, nie na listę") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one->children().front().get()));
    same(one->which(), std::optional<std::size_t>{0});
}

TRIAL("List: pusta bierze ognisko sama") {
    Sheet top;

    const auto one = std::make_shared<ui::List>();

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
    ought(one->current() == nullptr);
}

TRIAL("List: strzałki chodzą po wierszach i nie zawijają") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{1});

    ui::KeyEvent again{ui::Code::down};

    ought(top.dispatch(again));
    same(one->which(), std::optional<std::size_t>{2});

    // Na ostatnim wierszu w dół nic nie robi — i **nie wraca na pierwszy**.
    ui::KeyEvent past{ui::Code::down};

    ought(top.dispatch(past));
    same(one->which(), std::optional<std::size_t>{2});

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{1});
}

TRIAL("List: Home i End stają na pierwszym i ostatnim") {
    Sheet top;

    const auto one = listed(5);

    top.children().add(one);

    one->focus();

    ui::KeyEvent last{ui::Code::end};

    ought(top.dispatch(last));
    same(one->which(), std::optional<std::size_t>{4});

    ui::KeyEvent first{ui::Code::home};

    ought(top.dispatch(first));
    same(one->which(), std::optional<std::size_t>{0});
}

// Strona liczy się z **prawdziwej wysokości**, którą lista dostała w poprzedniej klatce.
TRIAL("List: strona to tyle wierszy, ile widać") {
    Sheet top;

    const auto one = listed(20);

    top.children().add(one);

    one->focus();

    (void)top.render({{0, 0}, {12, 5}});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{5});

    ui::KeyEvent up{ui::Code::pageup};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{0});

    // Strona przy końcu zatrzymuje się na ostatnim wierszu, zamiast wyjść poza.
    ui::KeyEvent last{ui::Code::end};

    (void)top.dispatch(last);

    ui::KeyEvent over{ui::Code::pagedown};

    ought(top.dispatch(over));
    same(one->which(), std::optional<std::size_t>{19});
}

TRIAL("List: wędrówka omija pozycje niedostępne") {
    Sheet top;

    const auto one = listed(4);

    top.children().add(one);

    dynamic_cast<ui::ListItem*>(one->children()[1].get())->enabled(false);
    dynamic_cast<ui::ListItem*>(one->children()[2].get())->enabled(false);

    one->focus();

    same(one->which(), std::optional<std::size_t>{0});

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{3});

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{0});
}

TRIAL("List: same niedostępne pozycje — ognisko zostaje na liście") {
    Sheet top;

    const auto one = listed(2);

    top.children().add(one);

    dynamic_cast<ui::ListItem*>(one->children()[0].get())->enabled(false);
    dynamic_cast<ui::ListItem*>(one->children()[1].get())->enabled(false);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

// **Przewinięcie liczy malowanie**, bo dopiero tam znana jest wysokość — ta sama reguła co w polu.
TRIAL("List: widok dogania bieżący wiersz") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    one->focus();

    (void)top.render({{0, 0}, {12, 3}});

    same(one->first(), 0);

    ui::KeyEvent last{ui::Code::end};

    (void)top.dispatch(last);
    (void)top.render({{0, 0}, {12, 3}});

    // Dziesięć wierszy, widać trzy, stoimy na dziewiątym — więc widok zaczyna się od siódmego.
    same(one->which(), std::optional<std::size_t>{9});
    same(one->first(), 7);

    ui::KeyEvent first{ui::Code::home};

    (void)top.dispatch(first);
    (void)top.render({{0, 0}, {12, 3}});

    same(one->first(), 0);
}

// Kółko przesuwa **sam widok**: bieżący wiersz zostaje, a `moved` się nie odzywa.
TRIAL("List: kółko przewija widok, a wiersza nie rusza") {
    Sheet top;

    const auto one = listed(10);

    int moves = 0;

    (void)one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    top.children().add(one);

    one->focus();

    same(moves, 1);

    (void)top.render({{0, 0}, {12, 3}});

    ui::WheelEvent what{{2, 1}, -1};

    ought(top.dispatch(what));

    (void)top.render({{0, 0}, {12, 3}});

    // Widok się przesunął, więc zdarzenie idzie — ale bieżący wiersz został tam, gdzie stał.
    same(one->first(), 3);
    same(one->which(), std::optional<std::size_t>{0});
    same(moves, 2);
}

TRIAL("List: kółko na końcu przewijania oddaje zdarzenie wyżej") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    // W górę z zera nie ma dokąd — zdarzenie idzie dalej, żeby dostał je przewijak nad listą.
    ui::WheelEvent up{{2, 1}, 1};

    ought(!top.dispatch(up));

    ui::WheelEvent down{{2, 1}, -1};

    ought(top.dispatch(down));
}

TRIAL("List: zdarzenie odzywa się raz na zmianę wiersza") {
    Sheet top;

    const auto one = listed(4);

    int moves = 0;
    std::optional<std::size_t> last;

    // Zdarzenie nie niesie numeru wiersza — słuchacz czyta go z `which()`. Jedna droga
    // zawiadamiania zamiast dwóch: `on` i `off` jak wszędzie indziej.
    const std::weak_ptr<ui::List> weakly = one;

    (void)one->on<ui::ScrollEvent>([&moves, &last, weakly](ui::ScrollEvent&) {
        ++moves;

        if (const auto got = weakly.lock()) last = got->which();

        return false;
    });

    top.children().add(one);

    one->focus();

    same(moves, 1);
    same(last, std::optional<std::size_t>{0});

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));

    same(moves, 2);
    same(last, std::optional<std::size_t>{1});

    // Ruch w dół z ostatniego wiersza nie jest zmianą.
    ui::KeyEvent end{ui::Code::end};

    (void)top.dispatch(end);

    same(moves, 3);

    ui::KeyEvent past{ui::Code::down};

    ought(top.dispatch(past));

    same(moves, 3);
}

TRIAL("List: wyjście z listy i powrót wracają na ten sam wiersz") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = listed(5);
    const auto perch = std::make_shared<Perch>();

    nest->children().add(one);
    nest->children().add(perch);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);
    (void)top.dispatch(down);

    same(one->which(), std::optional<std::size_t>{2});

    perch->focus();

    same(top.focused_node(), static_cast<ui::Node*>(perch.get()));
    same(one->which(), std::optional<std::size_t>{2});

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one->children()[2].get()));
    same(one->which(), std::optional<std::size_t>{2});
}

TRIAL("List: litera, Tab i Esc idą wyżej") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    ui::KeyEvent letter{U'a'};

    ought(top.dispatch(letter));
    same(top.keys, 1);

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.keys, 2);

    ui::KeyEvent away{ui::Code::escape};

    ought(top.dispatch(away));
    same(top.keys, 3);

    // Strzałka ze sterującym też nie należy do listy.
    ui::KeyEvent held{ui::Key{ui::Code::down, ui::Mods{true, false, false}}};

    ought(top.dispatch(held));
    same(top.keys, 4);
    same(one->which(), std::optional<std::size_t>{0});
}

TRIAL("List: Enter woła zadanie bieżącego wiersza") {
    Sheet top;

    const auto one = listed(3);

    int done = 0;

    (void)one->children()[1]->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    top.children().add(one);

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    ui::KeyEvent what{ui::Code::enter};

    ought(top.dispatch(what));

    same(done, 1);
    same(top.keys, 0);
}

TRIAL("List: which ustawia wiersz z programu i przestawia ognisko") {
    Sheet top;

    const auto one = listed(6);

    top.children().add(one);

    // Bez ogniska numer jest tylko zapamiętany.
    one->which(4);

    same(one->which(), std::optional<std::size_t>{4});
    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one->children()[4].get()));

    // Z ogniskiem przestawia je od razu, a numer poza zakresem przycina.
    one->which(99);

    same(one->which(), std::optional<std::size_t>{5});
    same(top.focused_node(), static_cast<ui::Node*>(one->children()[5].get()));

    one->current(*dynamic_cast<ui::ListItem*>(one->children()[1].get()));

    same(one->which(), std::optional<std::size_t>{1});
    same(top.focused_node(), static_cast<ui::Node*>(one->children()[1].get()));
}

// **`which()` to numer, `current()` to węzeł** — ten sam wiersz pod dwiema nazwami. Kto pyta
// o węzeł, dostaje wiersz do naciśnięcia albo przeczytania; kto o numer — liczbę do własnej tablicy
// obok. Pusta lista oddaje pustkę w obu; obcy wiersz w ustawiaczu nic nie robi.
TRIAL("List: which to numer, current to węzeł, a obcy wiersz nie ustawia niczego") {
    Sheet top;

    const auto one = listed(3);
    const auto other = listed(2);

    top.children().add(one);

    ought(one->current() == nullptr);

    ought(!one->which().has_value());

    one->which(2);

    same(one->which(), std::optional<std::size_t>{2});
    same(static_cast<ui::Node*>(one->current()), one->children()[2].get());
    same(one->current()->text(), std::string{"wiersz 2"});

    one->current(*dynamic_cast<ui::ListItem*>(other->children()[0].get()));

    same(one->which(), std::optional<std::size_t>{2});
    same(static_cast<ui::Node*>(one->current()), one->children()[2].get());

    one->current(*dynamic_cast<ui::ListItem*>(one->children()[0].get()));

    same(one->which(), std::optional<std::size_t>{0});

    const auto empty = std::make_shared<ui::List>();

    ought(empty->current() == nullptr);
    ought(!empty->which().has_value());

    empty->which(0);

    ought(empty->current() == nullptr);
    ought(!empty->which().has_value());

    // Wiersz zdjęty spod numeru: numer nie wskazuje na nic, więc go nie ma — zgodnie z węzłem.
    one->which(2);
    (void)one->children().remove(one->children()[2].get());

    same(one->which().has_value(), one->current() != nullptr);
}

TRIAL("List: pasek przewijania zabiera kolumnę z prawej") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {12, 1}}));

    one->scrollbar(true);

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {11, 1}}));
}

// Pasek „gdy trzeba": lista krótsza niż okno oddaje jego kolumnę wierszom.
TRIAL("List: pasek gdy trzeba tylko przy liście dłuższej niż okno") {
    Sheet top;

    const auto one = std::make_shared<ui::List>(12, 3, ui::ScrollShape{true, true});

    for (int at = 0; at < 3; ++at) one->children().add(std::make_shared<ui::ListItem>("pozycja " + std::to_string(at)));

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {12, 1}}));

    one->children().add(std::make_shared<ui::ListItem>("pozycja 3"));

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {11, 1}}));
}

// Wiersz, który zjechał poza widok, **nie może łapać myszą** w miejscu, gdzie stał poprzednio.
TRIAL("List: wiersz poza widokiem nie łapie myszy") {
    Sheet top;

    const auto one = listed(5);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 2}});

    same(one->children()[0]->area(), (ui::Area{{0, 0}, {12, 1}}));

    one->first(3);

    (void)top.render({{0, 0}, {12, 2}});

    same(one->children()[0]->area().size.high, 0);

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(one->children()[3].get()));
    same(one->which(), std::optional<std::size_t>{3});
}

TRIAL("List: klik w wiersz czyni go bieżącym") {
    Sheet top;

    const auto one = listed(4);

    int moves = 0;

    (void)one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 4}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(one->which(), std::optional<std::size_t>{2});
    same(moves, 1);
}

TRIAL("List: chce tyle, ile jej kazano") {
    const auto one = listed(3, 20, 6);

    same(one->wants({40, 10}), (ui::Size{20, 6}));
    same(one->wants({10, 4}), (ui::Size{10, 4}));

    const auto other = listed(3);

    same(other->wants({40, 10}), (ui::Size{40, 10}));
}

// --- Co lista przyjmuje --------------------------------------------------------------------------

// Numer bieżącego wiersza liczy się **wśród dzieci**, więc obce dziecko rozjechałoby numerację
// z tym, co widać na ekranie. Odmawiamy raz, przy dopięciu.
TRIAL("Lista: przyjmuje wiersze, obcego dziecka nie") {
    const auto one = std::make_shared<ui::List>();

    one->children().add(std::make_shared<ui::ListItem>("plik"));

    same(one->children().many(), std::size_t{1});

    bool threw = false;

    try {
        one->children().add(std::make_shared<ui::Span>("obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(one->children().many(), std::size_t{1});
}

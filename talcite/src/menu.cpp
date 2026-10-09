#include "talcite/menu.hpp"

#include <algorithm>
#include <climits>

#include "talcite/line.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "lining.hpp"

namespace talcite::ui {

namespace {

// Szerokości boków pudełka. Bez ramki nie ma boków, a z ramką są tak szerokie, jak znaki, którymi
// skórka ją rysuje — opisane przy `Frame::edges`.
Sides boxed(const Skin& how, Border border) {
    if (border == Border::none) return {0, 0};

    return sided(how.lines(border));
}

}

// **Menu to lista pozycji** — pudełko, które pasek kładzie na treści, gdy jest rozwinięte. Tytuł
// w pasku jest osobnym węzłem (`MenuTitle`), który to menu zna i otwiera. Przedtem jedno `Menu`
// zajmowało dwa nieprzyległe prostokąty — pas tytułu i pudełko pod nim — z polem `aside_`
// wpisywanym przez pasek tuż przed malowaniem: jedyny nie-prostokąt w bibliotece, utrzymywany
// ręcznie. Teraz każdy z dwóch prostokątów stoi w swoim miejscu drzewa i wie, gdzie jest.
//
// Zwinięte menu **nie jest w drzewie**: pasek dopina je przy rozwinięciu i odpina przy zwinięciu.
// Dlatego „rozwinięte" to jedno pytanie — czy mam rodzica — a nie rachunek na ogniskach.
Menu::Menu(std::string title, MenuShape shape) : title_(std::move(title)), shape_(shape) {
    role("talcite.menubar");
}

int Menu::capacity() const { return plenty; }

// Menu składa się z pozycji i rozdzielników — i z niczego więcej. Cokolwiek innego byłoby dla
// `item()` pustką, po której wędrówka strzałkami przeskakuje, a malowanie rysuje nie wiadomo co.
bool Menu::accepts(const Node& one) const {
    return dynamic_cast<const MenuItem*>(&one) != nullptr ||
           dynamic_cast<const MenuSeparator*>(&one) != nullptr;
}

// **Rozwinięte znaczy dopięte do paska.** Jedno źródło prawdy, bez pytania o ognisko: pasek dopina
// przy `open`, odpina przy zwinięciu, a menu bez pozycji nie jest dopinane nigdy — tytuł bez
// niczego pod spodem to przycisk w pasku, nie pusta lista.
bool Menu::opened() const { return parent() != nullptr; }

// Ognisko na menu znaczy „rozwiń": zwinięte jest poza drzewem i nie ma na czym stanąć.
void Menu::focus() { open(); }

// Rozwinięcie: pasek dopina menu i ognisko schodzi na pozycję — na zapamiętaną, a gdy tej nie ma
// albo jest wyłączona, na pierwszą dostępną. Menu bez pozycji rozwinąć się nie da: ognisko staje
// na tytule, czyli pasek jest czynny i zwinięty.
void Menu::open() {
    if (bar_ == nullptr) return;

    if (children().empty()) {
        if (head_ != nullptr && head_->root() != nullptr) head_->focus();

        return;
    }

    bar_->unfold(*this);

    MenuItem* one = item(which_);

    if (one == nullptr || !one->enabled()) one = item(nearest(-1, 1));

    if (one != nullptr) {
        one->focus();

        return;
    }

    // Same wyłączone pozycje: pudełko stoi, ale nie ma na czym postawić ogniska — zostaje na menu.
    Node::focus();
}

// **Zwinięcie odpina menu z paska**, para do `open`. Zamknięte menu zaczyna następnym razem od
// góry; odpięcie nie posyła `UnfocusEvent`, więc numer gasimy tu wprost. **Ognisko siedzące w menu
// przechodzi na tytuł, a nie gaśnie**: zwinięcie nie jest wyjściem z paska, a ognisko zgaszone bez
// zdarzenia w `Root::losing` wracałoby potem „znikąd" — i pasek brałby to za nowe wejście, gubiąc,
// komu ma oddać ognisko przy wyjściu. Gdy ognisko już wyszło z menu (klik w treść), nie ma czego
// przenosić. Dawniej robił to pasek, sięgając do numeru pozycji menu; teraz menu robi to samo.
//
// Menu zwinięte nie ma czego zwijać i **zachowuje zapamiętaną pozycję** — `which(n)` na zwiniętym
// menu mówi, od czego zacząć przy rozwinięciu. Zgaszenie numeru u rozwiniętego jest strażą: ognisko
// schodzi z menu przez tytuł, więc `UnfocusEvent` gasi numer i tak, a mutant bez tej linii przeżywa.
void Menu::close() {
    Node* up = parent();

    if (up == nullptr) return;

    which_ = -1;

    if (Root* top = root(); top != nullptr && head_ != nullptr)
        for (const Node* at = top->focused_node(); at != nullptr; at = at->parent())
            if (at == this) {
                head_->focus();

                break;
            }

    (void)up->children().remove(this);
}

bool Menu::focusable() const { return true; }

MenuItem* Menu::item(int which) const {
    if (which < 0 || which >= static_cast<int>(children().many())) return nullptr;

    return dynamic_cast<MenuItem*>(children()[static_cast<std::size_t>(which)].get());
}

// Wnętrze pudełka: najszersza pozycja i tyle wierszy, ile pozycji. Pytamy o plenty, bo szerokość
// menu bierze się z jego zawartości, a nie z tego, co akurat zostało na ekranie.
Size Menu::fold() const {
    int wide = 0;

    for (const std::shared_ptr<Node>& one : children())
        wide = std::max(wide, one->wants({plenty, 1}).wide);

    return {wide, static_cast<int>(children().many())};
}

// Pudełko: wnętrze, boki ramki, cień — i co najmniej tak szerokie jak tytuł, żeby nie było węższe
// od tego, spod czego wychodzi.
Size Menu::wants(Size room) const {
    const Size inner = fold();
    const Sides side = boxed(skin(), shape_.border);

    const int aside = shape_.shade ? 2 : 0;
    const int apart = shape_.shade ? 1 : 0;

    const int wide =
        std::max(inner.wide + side.left + side.right + aside, 2 + measured(title_).wide);

    return {std::min(wide, room.wide), std::min(inner.high + 2 + apart, room.high)};
}

// **Pod swoim tytułem** — to jest całe pozycjonowanie menu. Miejsce tytułu bierzemy
// ze `screen_area()`, czyli z prostokątów w górę drzewa, liczonych przy każdej klatce; przesuwamy
// je do układu paska, bo stos pyta o miejsce we własnych współrzędnych. Gdy pudełko nie mieści się
// przy prawej krawędzi, dosuwa się w lewo — tytuł stoi, gdzie stał, bo jest osobnym węzłem w swoim
// rzędzie.
Area Menu::placement(Size room) const {
    const Size wanted = wants(room);

    int x = 0;

    if (head_ != nullptr && bar_ != nullptr) {
        const std::optional<Area> title = head_->screen_area();
        const std::optional<Area> bar = bar_->screen_area();

        if (title.has_value() && bar.has_value()) x = title->at.x - bar->at.x;
    }

    x = std::clamp(x, 0, std::max(room.wide - wanted.wide, 0));

    return {{x, 1}, wanted};
}

// **Podświetlona pozycja ma dwie twarze** — jak w liście: `which()` to numer, `current()` to ona
// sama, którą się naciska, czyta albo porównuje. Pustka znaczy „żadna". W środku numer siedzi
// jako `int`, bo `nearest` liczy na nim kroki z zawinięciem, a zwinięcie gasi go przez `-1`.
std::optional<std::size_t> Menu::which() const {
    if (item(which_) == nullptr) return std::nullopt;

    return static_cast<std::size_t>(which_);
}

MenuItem* Menu::current() const { return item(which_); }

// Numer podświetlonej pozycji trzyma menu, ale samo podświetlenie **jest ogniskiem** — więc
// przestawienie numeru w otwartym menu przestawia ognisko, a gasnięcie poprzedniej dzieje się samo.
// W menu zamkniętym numer jest tylko zapamiętany.
void Menu::aim(int given) {
    which_ = given;

    if (!opened()) return;

    if (MenuItem* one = item(which_); one != nullptr && one->enabled()) one->focus();
}

void Menu::which(std::size_t given) {
    aim(static_cast<int>(std::min<std::size_t>(given, static_cast<std::size_t>(INT_MAX))));
}

// Pozycja spoza menu nic nie zmienia.
void Menu::current(MenuItem& one) {
    if (const std::optional<std::size_t> at = children().find(one)) which(*at);
}

// Najbliższa dostępna pozycja w podanym kierunku, z zawinięciem. Kreski i wyszarzone pozycje
// omijamy tym samym warunkiem, bo kreska to po prostu pozycja niedostępna.
int Menu::nearest(int from, int step) const {
    const int much = static_cast<int>(children().many());

    if (much == 0) return -1;

    int at = from >= 0 ? from : (step > 0 ? -1 : much);

    for (int i = 0; i < much; ++i) {
        at = (at + step + much) % much;

        const MenuItem* one = item(at);

        if (one != nullptr && one->enabled()) return at;
    }

    return -1;
}

// Kolejność jest tu wszystkim: najpierw zamykamy, dopiero potem wołamy powierzone zadanie.
// Odwrotnie menu wisiałoby jeszcze na wierzchu, gdyby zadanie otworzyło okno. Menu bez pozycji
// woła własne zdarzenie — tytuł jest wtedy czynnością.
void Menu::pick() {
    if (children().empty()) {
        if (bar_ != nullptr) bar_->leave();

        (void)trigger(PickEvent{});

        return;
    }

    MenuItem* one = item(which_);

    if (one == nullptr || !one->enabled()) return;

    if (bar_ != nullptr) bar_->leave();

    one->pick();
}

// **Czynność menu to rozwinięcie** — dla klawisza, który trafi tu wprost, a nie przez tytuł.
void Menu::activate() { open(); }

// Samo pudełko: ramka, pozycje, cień. Tytułu tu nie ma — maluje go `MenuTitle` w swoim rzędzie.
// Prostokąt zaczyna się od ramki, więc pozycje stoją od wiersza pierwszego.
View Menu::paint(Size, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    const Style edge = how.style("talcite.menu.frame");

    const Size inner = fold();
    const Sides side = boxed(how, shape_.border);

    const int right_x = side.left + inner.wide;
    const int last_x = right_x + side.right - 1;
    const int last_y = inner.high + 1;

    if (shape_.border != Border::none) {
        const Lines lines = how.lines(shape_.border);

        across(made, {side.left - seen.at.x, -seen.at.y}, inner.wide, lines.across, edge);
        across(made, {side.left - seen.at.x, last_y - seen.at.y}, inner.wide, lines.across, edge);

        for (int y = 1; y < last_y; ++y) {
            made.put({-seen.at.x, y - seen.at.y}, lines.upright, edge);
            made.put({right_x - seen.at.x, y - seen.at.y}, lines.upright, edge);
        }

        made.put({-seen.at.x, -seen.at.y}, lines.upper_left, edge);
        made.put({right_x - seen.at.x, -seen.at.y}, lines.upper_right, edge);
        made.put({-seen.at.x, last_y - seen.at.y}, lines.lower_left, edge);
        made.put({right_x - seen.at.x, last_y - seen.at.y}, lines.lower_right, edge);
    }

    int y = 1;

    for (const std::shared_ptr<Node>& one : children()) {
        const Area where{{side.left, y}, {inner.wide, 1}};
        const Area part = Area::crossed(seen, where);

        made.draw(one->render(where, part - where.at), part.at - seen.at);

        // Kreska dochodzi do ramki, tak jak w Midnight Commanderze. Narożników nie zna sama
        // kreska, bo jej prostokąt kończy się na wnętrzu — rysuje je więc menu.
        if (shape_.border != Border::none &&
            dynamic_cast<const MenuSeparator*>(one.get()) != nullptr) {
            const Lines lines = how.lines(shape_.border);

            made.put({-seen.at.x, y - seen.at.y}, lines.left_joint, edge);
            made.put({right_x - seen.at.x, y - seen.at.y}, lines.right_joint, edge);
        }

        ++y;
    }

    // Cień pada w prawo i w dół, przesunięty o wiersz i o dwie kolumny — inaczej wyglądałby jak
    // druga ramka. Litery spod spodu zostają, tylko przygasają.
    if (shape_.shade) {
        const Style dark = how.style("talcite.shade");

        made.shade({{last_x + 1 - seen.at.x, 1 - seen.at.y}, {2, last_y}}, dark);
        made.shade({{2 - seen.at.x, last_y + 1 - seen.at.y}, {last_x + 1, 1}}, dark);
    }

    return made;
}

// Trzy klawisze są **menu**, reszta należy do pozycji albo do kogoś wyżej. Menu dochodzi tu tylko
// rozwinięte — zwinięte jest poza drzewem, a jego klawiszami zajmuje się tytuł.
//
// **Klawisz aktywny to rozdanie, a nie pytanie o typ** — ta sama reguła i ten sam mechanizm, co
// w dialogu. Menu nie zgaduje, która pozycja na co odpowiada: podaje klawisz swoim dzieciom
// i czeka, aż któraś go weźmie. Rozdajemy **od siebie**, więc trafia to wyłącznie w pozycje tego
// menu, a nie w treść pod paskiem.
//
// Gdy nikt nie weźmie, oddajemy fałsz i klawisz idzie w górę do paska — bo menu jest jego dzieckiem.
bool Menu::handle(KeyEvent& what) {
    // Ze sterującym nie łapiemy **niczego** — także strzałek i Entera. Ctrl+Enter i Alt+strzałka
    // to z założenia inne skróty, a odsianie ich dopiero po rozpoznaniu klawisza zabierało je
    // aplikacji, ilekroć menu było otwarte. Tak samo odsiewa je przycisk, pole i legenda.
    const Mods mods = what.mods();

    if (mods.ctrl || mods.alt) return false;

    switch (what.key().code()) {
        case Code::up: aim(nearest(which_, -1)); return true;
        case Code::down: aim(nearest(which_, 1)); return true;
        case Code::enter: pick(); return true;
        default: break;
    }

    if (what.key().empty()) return false;

    return broadcast(what);
}

// Numer podświetlonej pozycji dostajemy **bąbelkowaniem**: ognisko postawione na pozycji — strzałką,
// klikiem, najechaniem myszą czy z programu — przechodzi tędy w drodze w górę, więc wszystkie te
// drogi schodzą się w jednym miejscu, zamiast każda ustawiać numer po swojemu.
bool Menu::handle(FocusEvent& what) {
    const Node* one = dynamic_cast<const Node*>(what.target());

    if (one == nullptr) return false;

    // Ognisko na samym menu znaczy, że nie ma na czym stanąć — same wyłączone pozycje. Numer musi
    // wtedy zgasnąć, bo inaczej mówiłby o podświetleniu, którego nie widać.
    if (one == this) {
        which_ = -1;

        return false;
    }

    for (int at = 0; at < static_cast<int>(children().many()); ++at)
        if (children()[static_cast<std::size_t>(at)].get() == one) {
            which_ = at;

            break;
        }

    return false;
}

// Zamknięte menu zaczyna następnym razem od góry, a nie od tego, na czym stanęło. Przejście
// z pozycji na pozycję **nie jest zamknięciem**: ognisko dalej siedzi w menu, więc numeru nie
// gasimy — inaczej każda strzałka kasowałaby to, co przed chwilą ustawiła.
bool Menu::handle(UnfocusEvent& what) {
    for (const Node* at = what.other(); at != nullptr; at = at->parent())
        if (at == this) return false;

    which_ = -1;

    return false;
}

}

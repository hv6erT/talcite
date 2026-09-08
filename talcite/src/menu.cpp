#include "talcite/menu.hpp"

#include <algorithm>

#include "talcite/line.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuline.hpp"
#include "talcite/menuwindow.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "lining.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// Szerokości boków pudełka. Bez ramki nie ma boków, a z ramką są tak szerokie, jak znaki, którymi
// skórka ją rysuje — opisane przy `Frame::edges`.
Sides boxed(const Skin& how, LineStyle border) {
    if (border == LineStyle::none) return {0, 0};

    return sided(how.get<Lines>(named(border)));
}

}

Menu::Menu(std::string title, MenuStyle style) : title_(std::move(title)), style_(style) {
    role("talcite.menubar");
}

int Menu::takes() const { return plenty; }

// Menu składa się z pozycji i rozdzielników — i z niczego więcej. Cokolwiek innego byłoby dla
// `item()` pustką, po której wędrówka strzałkami przeskakuje, a malowanie rysuje nie wiadomo co.
bool Menu::accepts(const Node& one) const {
    return dynamic_cast<const MenuItem*>(&one) != nullptr ||
           dynamic_cast<const MenuLine*>(&one) != nullptr;
}

// Otwarte menu to menu z ogniskiem. Nie ma osobnego pola „otwarte", bo dwa źródła tej samej
// prawdy prędzej czy później się rozjeżdżają — a tak otwarcie jest jedną czynnością: focus().
//
// Ognisko siedzi **na podświetlonej pozycji**, a nie na samym menu, więc pytamy o nie w dół całej
// gałęzi. Na samym menu zostaje tylko wtedy, gdy nie ma na czym stanąć — menu puste albo same
// niedostępne pozycje.
bool Menu::opened() const {
    const Root* top = root();

    if (top == nullptr) return false;

    for (const Node* at = top->focus(); at != nullptr; at = at->parent())
        if (at == this) return true;

    return false;
}

// **Otwarcie stawia ognisko na pozycji.** Wraca na tę, na której menu stanęło ostatnio — a że
// zamknięcie gasi numer, w praktyce znaczy to: na pierwszą dostępną, jak w Midnight Commanderze.
void Menu::focus() {
    MenuItem* one = item(which_);

    if (one == nullptr || !one->able()) one = item(nearest(-1, 1));

    if (one != nullptr) {
        one->focus();

        return;
    }

    Node::focus();
}

// Zamknięte leży w pasku i niczego nie zakrywa. Otwarte kładzie się na treści aplikacji, czyli
// na swoim rodzeństwie — i tylko na nim, bo poza pasek nie wychodzi.
int Menu::layer() const { return opened() ? 1 : 0; }

// Otwarcie menu **jest** wzięciem ogniska, więc menu musi się do brania ogniska przyznać.
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

Size Menu::wants(Size room) const {
    const int wide = 2 + measured(title_).wide;

    if (!opened()) return {std::min(wide, room.wide), std::min(1, room.high)};

    const Size inner = fold();
    const Sides side = boxed(skin(), style_.border);

    const int aside = style_.shade ? 2 : 0;
    const int apart = style_.shade ? 1 : 0;

    return {std::min(std::max(inner.wide + side.left + side.right + aside, wide), room.wide),
            std::min(1 + inner.high + 2 + apart, room.high)};
}


// Numer podświetlonej pozycji trzyma menu, ale samo podświetlenie **jest ogniskiem** — więc
// przestawienie numeru w otwartym menu przestawia ognisko, a gasnięcie poprzedniej dzieje się samo.
// W menu zamkniętym numer jest tylko zapamiętany.
void Menu::which(int given) {
    which_ = given;

    if (!opened()) return;

    if (MenuItem* one = item(which_); one != nullptr && one->able()) one->focus();
}

void Menu::which(MenuItem* one) {
    for (int at = 0; at < static_cast<int>(children().many()); ++at)
        if (children()[static_cast<std::size_t>(at)].get() == one) {
            which(at);

            return;
        }
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

        if (one != nullptr && one->able()) return at;
    }

    return -1;
}

// Kolejność jest tu wszystkim: najpierw zamykamy, dopiero potem wołamy powierzone zadanie.
// Odwrotnie menu wisiałoby jeszcze na wierzchu, gdyby zadanie otworzyło okno.
void Menu::pick() {
    MenuItem* one = item(which_);

    if (one == nullptr || !one->able()) return;

    if (MenuWindow* window = dynamic_cast<MenuWindow*>(parent())) window->close();

    one->pick();
}

// **Czynność tytułu to rozwinięcie menu**, bo `focus()` menu otwiera. Litera tytułu robi więc
// dokładnie to, co kliknięcie w tytuł — i nic ponadto.
void Menu::act() { focus(); }

View Menu::paint(Size given, Area seen) {
    View made{seen.size};

    // Menu nie jest prostokątem: zajmuje pas tytułu i pudełko pod nim, a między nimi zostają
    // rogi, które należą do sąsiadów — do tytułu obok albo do treści pod cieniem. Zaczynamy
    // więc od pustki i zamalowujemy tylko to, co naprawdę rysujemy.
    made.bare({{0, 0}, seen.size});

    const Skin& how = skin();

    const Style look = how.get<Style>(opened() ? role() + ".focus" : role());
    const Style edge = how.get<Style>("talcite.menu.frame");

    const int wide = 2 + measured(title_).wide;

    made.fill({{aside_ - seen.at.x, -seen.at.y}, {wide, 1}}, look);
    made.string({aside_ + 1 - seen.at.x, -seen.at.y}, title_, look);

    // Litera aktywna w tytule — ta sama maszyneria, co w pozycji menu i w przycisku. Tytuł stoi
    // o jedną kratkę od lewej krawędzi swojego pasa, więc i litera jest o tę kratkę przesunięta.
    talcite::ui::marking(made, {aside_ + 1 - seen.at.x, -seen.at.y}, title_, marking(title_),
                      Style::merge(look, how.get<Style>(role() + ".key")));

    if (!opened()) return made;

    const Size inner = fold();
    const Sides side = boxed(skin(), style_.border);

    // Pudełko stoi pod tytułem, od własnej kolumny zero. Tytuł mógł zjechać w prawo względem
    // pudełka, gdy ostatnie menu nie mieściło się przy krawędzi ekranu — stąd aside_. Prawy bok
    // zaczyna się za wnętrzem, a ostatnia kolumna pudełka jest o jego szerokość dalej.
    const int right_x = side.left + inner.wide;
    const int last_x = right_x + side.right - 1;
    const int last_y = inner.high + 2;

    if (style_.border != LineStyle::none) {
        const Lines lines = how.get<Lines>(named(style_.border));

        across(made, {side.left - seen.at.x, 1 - seen.at.y}, inner.wide, lines.across, edge);
        across(made, {side.left - seen.at.x, last_y - seen.at.y}, inner.wide, lines.across, edge);

        for (int y = 2; y < last_y; ++y) {
            made.letter({-seen.at.x, y - seen.at.y}, lines.upright, edge);
            made.letter({right_x - seen.at.x, y - seen.at.y}, lines.upright, edge);
        }

        made.letter({-seen.at.x, 1 - seen.at.y}, lines.upper_left, edge);
        made.letter({right_x - seen.at.x, 1 - seen.at.y}, lines.upper_right, edge);
        made.letter({-seen.at.x, last_y - seen.at.y}, lines.lower_left, edge);
        made.letter({right_x - seen.at.x, last_y - seen.at.y}, lines.lower_right, edge);
    }

    int y = 2;

    for (const std::shared_ptr<Node>& one : children()) {
        const Area where{{side.left, y}, {inner.wide, 1}};
        const Area part = Area::crossed(seen, where);

        made.within(one->render(where, part - where.at), part.at - seen.at);

        // Kreska dochodzi do ramki, tak jak w Midnight Commanderze. Narożników nie zna sama
        // kreska, bo jej prostokąt kończy się na wnętrzu — rysuje je więc menu.
        if (style_.border != LineStyle::none &&
            dynamic_cast<const MenuLine*>(one.get()) != nullptr) {
            const Lines lines = how.get<Lines>(named(style_.border));

            made.letter({-seen.at.x, y - seen.at.y}, lines.left_joint, edge);
            made.letter({right_x - seen.at.x, y - seen.at.y}, lines.right_joint, edge);
        }

        ++y;
    }

    // Cień pada w prawo i w dół, przesunięty o wiersz i o dwie kolumny — inaczej wyglądałby jak
    // druga ramka. Litery spod spodu zostają, tylko przygasają.
    if (style_.shade) {
        const Style dark = how.get<Style>("talcite.shade");

        made.shade({{last_x + 1 - seen.at.x, 2 - seen.at.y}, {2, last_y - 1}}, dark);
        made.shade({{2 - seen.at.x, last_y + 1 - seen.at.y}, {last_x + 1, 1}}, dark);
    }

    return made;
}

// Trzy klawisze są **menu**, reszta należy do pozycji albo do kogoś wyżej.
//
// **Klawisz aktywny to rozdanie, a nie pytanie o typ** — ta sama reguła i ten sam mechanizm, co
// w dialogu. Menu nie zgaduje, która pozycja na co odpowiada: podaje klawisz swoim dzieciom
// i czeka, aż któraś go weźmie. Rozdajemy **od siebie**, więc trafia to wyłącznie w pozycje tego
// menu, a nie w treść pod paskiem.
//
// Gdy nikt nie weźmie, oddajemy fałsz i klawisz idzie w górę tak jak dotąd — inaczej otwarte menu
// połknęłoby F10 i pasek klawiszy przestałby działać.
bool Menu::react(KeyEvent& what) {
    if (!opened()) return false;

    // Ze sterującym nie łapiemy **niczego** — także strzałek i Entera. Ctrl+Enter i Alt+strzałka
    // to z założenia inne skróty, a odsianie ich dopiero po rozpoznaniu klawisza zabierało je
    // aplikacji, ilekroć menu było otwarte. Tak samo odsiewa je przycisk, pole i legenda.
    const Mods mods = what.mods();

    if (mods.ctrl || mods.alt) return false;

    switch (what.key().code()) {
        case Code::up: which(nearest(which_, -1)); return true;
        case Code::down: which(nearest(which_, 1)); return true;
        case Code::enter: pick(); return true;
        default: break;
    }

    if (!what.key().held()) return false;

    return offer(what);
}

// Klik w tytuł przełącza, jak w Midnight Commanderze. Otwarcie to wzięcie ogniska, zamknięcie
// to oddanie go temu, kto je miał — o tym drugim wie tylko pasek.
bool Menu::react(ClickEvent&) {
    if (opened()) {
        if (MenuWindow* window = dynamic_cast<MenuWindow*>(parent())) window->close();

        return true;
    }

    if (root() != nullptr) focus();

    return true;
}

// Numer podświetlonej pozycji dostajemy **bąbelkowaniem**: ognisko postawione na pozycji — strzałką,
// klikiem, najechaniem myszą czy z programu — przechodzi tędy w drodze w górę, więc wszystkie te
// drogi schodzą się w jednym miejscu, zamiast każda ustawiać numer po swojemu.
bool Menu::react(FocusEvent& what) {
    const Node* one = dynamic_cast<const Node*>(what.at());

    if (one == nullptr) return false;

    // Ognisko na samym menu znaczy, że nie ma na czym stanąć — menu puste albo pozycja właśnie
    // wyszarzona. Numer musi wtedy zgasnąć, bo inaczej mówiłby o podświetleniu, którego nie widać.
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
bool Menu::react(UnfocusEvent& what) {
    for (const Node* at = what.other(); at != nullptr; at = at->parent())
        if (at == this) return false;

    which_ = -1;

    return false;
}

}

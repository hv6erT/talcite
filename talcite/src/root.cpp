#include "talcite/root.hpp"

#include <stdexcept>
#include <utility>

namespace talcite::ui {

Root::Root() = default;

Root::~Root() = default;

// **Podmiana całej palety jest całą zmianą wyglądu w locie.** Skórki nie da się zmienić po
// zbudowaniu, więc nie ma stanu, w którym połowa ról jest z nowej, a połowa ze starej; następna
// klatka po prostu czyta co innego. Nie ma powiadamiania ani obchodzenia drzewa.
// Skórka przychodzi z wariantami na wszystkie poziomy i **od razu staje na faktach korzenia**:
// poziom, który korzeń już zna, przechodzi na nową skórkę, a nie odwrotnie. Dzięki temu podmiana
// skórki — także na próbę, w oknie ustawień — nie wymaga powtarzania faktów o terminalu.
void Root::skin(Skin given) {
    given.colors(skin_.colors());
    given.unicode(skin_.unicode());

    skin_ = std::move(given);
}

// Barwność i unicode to fakty o terminalu, jak `blocks` i `cell` niżej — kładzie je ten, kto
// pogania drzewo. Korzeń nie trzyma ich osobno: **mieszkają w skórce**, bo to ona z nich korzysta
// przy każdym odczycie, a drugie miejsce prędzej czy później by się rozjechało.
void Root::colors(Colors given) { skin_.colors(given); }

void Root::unicode(bool given) { skin_.unicode(given); }

// **Fakty o kratce kładzie tu ten, kto pogania drzewo** — zwykle `Term`, w `onto()` i przy każdym
// `reconfigure`. Drzewo nie wie, skąd się wzięły, i dlatego równie dobrze może je postawić próba
// albo druga warstwa renderowania. Powiadamiania znów nie ma: widżet czyta je przy malowaniu, więc
// najbliższa klatka po prostu wychodzi inna.
void Root::blocks(Blocks given) { blocks_ = given; }

void Root::cell(Size given) { cell_ = given; }

// Pole przestawiamy przed wysłaniem obu zdarzeń, nie pomiędzy. Uchwyt, który w reakcji na
// utratę zapyta focused(), ma dostać odpowiedź już nową, a nie z połowy operacji.
void Root::focus(Node* one) {
    if (one != nullptr && one->root() != this)
        throw std::logic_error("talcite::ui::Root: węzeł nie należy do tego drzewa");

    // Wołanie na węźle, który ogniska nie bierze, **nic nie robi** — i to nie jest przeoczenie.
    // Ognisko oddaje się także temu, kto je miał wcześniej: tak wracają do aplikacji i pasek menu,
    // i dialog. Ten ktoś mógł w międzyczasie zostać wyłączony albo zniknąć z drzewa, a wyjątek
    // w tej ścieżce wywracałby program za cudzy stan.
    if (one != nullptr && !one->focusable()) return;

    if (one == focus_) return;

    Node* was = focus_;

    focus_ = one;

    if (was != nullptr) {
        UnfocusEvent what{one};

        (void)sent(*was, what);
    }

    if (one != nullptr) {
        FocusEvent what{was};

        (void)sent(*one, what);
    }
}

// Klawisz i wklejenie idą do ogniska, a gdy ogniska nie ma — do korzenia. Inaczej skrót
// obsługiwany przez aplikację milczałby, dopóki użytkownik czegoś nie wskaże.
bool Root::deal(KeyEvent& what) { return sent(focus_ != nullptr ? *focus_ : *this, what); }

bool Root::deal(PasteEvent& what) { return sent(focus_ != nullptr ? *focus_ : *this, what); }

bool Root::deal(SizeEvent& what) { return sent(*this, what); }

// Mysz celuje w najgłębszy węzeł pod kursorem, a nie w ognisko: klika się to, co się widzi.
// Surowe zdarzenie idzie pierwsze i zawsze — kto zaznacza ciągnięciem, nie może czekać na
// werdykt w sprawie kliknięcia.
bool Root::deal(MouseEvent& what) {
    const bool made = sent(*under(what.where()), what);

    minded(what);

    return made;
}

bool Root::deal(WheelEvent& what) { return sent(*under(what.where()), what); }

bool Root::deal(ClickEvent& what) { return sent(*under(what.where()), what); }

bool Root::deal(DoubleClickEvent& what) { return sent(*under(what.where()), what); }

// **Kliknięcie idzie od razu, dwuklik jest zdarzeniem dodatkowym** — tak robi Windows
// (`WM_LBUTTONDOWN` natychmiast, `WM_LBUTTONDBLCLK` zamiast drugiego wciśnięcia), X11
// i przeglądarki. Poprzednio pojedyncze czekało na ciszę po oknie dwukliku, żeby wykluczyć się
// z podwójnym — i każdy przycisk, tytuł menu i pole paska klawiszy działały z opóźnieniem 400 ms
// na POSIX, a na Windows tyle, ile ktoś ustawił w systemie, do 900. Wykluczanie nie było do
// niczego potrzebne: odbiorcy dwukliku znoszą kliknięcie przed nim (lista stawia ognisko, potem
// wybiera; edytor stawia kursor, potem zaznacza wyraz).
//
// Drugie puszczenie w oknie, w tej samej kratce i tym samym przyciskiem, daje sam
// `DoubleClickEvent` — bez drugiego `ClickEvent`, tak jak w Windows. Okno mierzy `Term`, bo tu
// nie ma zegara; jego `settle()` znaczy teraz tylko „drugie już nie przyjdzie, zapomnij".
void Root::minded(const MouseEvent& what) {
    if (what.press() == Press::down) {
        began_ = what.where();
        pressing_ = what.button();

        return;
    }

    if (what.press() != Press::up) return;

    const Mouse which = pressing_;

    pressing_ = Mouse::none;

    // Puszczenie w innej kratce niż wciśnięcie to ciągnięcie, a nie kliknięcie. Kliknięcie,
    // które czeka na swoje drugie, zostawiamy w spokoju — zamknie je czas.
    if (which == Mouse::none || which != what.button() || !(began_ == what.where())) return;

    if (clicking_ && clicked_ == what.button() && waiting_ == what.where()) {
        clicking_ = false;

        DoubleClickEvent one{what.where(), what.button(), what.mods()};

        (void)deal(one);

        return;
    }

    clicking_ = true;
    waiting_ = what.where();
    clicked_ = what.button();
    marked_ = what.mods();

    ClickEvent one{what.where(), what.button(), what.mods()};

    (void)deal(one);
}

void Root::settle() { clicking_ = false; }

// Schodzimy piętro po piętrze, odejmując przesunięcie każdego wejścia, bo prostokąt węzła jest
// wyrażony względem rodzica — tak zapisuje go render. Z rodzeństwa wygrywa **ostatnie** pasujące,
// bo składanie idzie po kolei i ostatnie leży na wierzchu. Węzeł, pod którym nic nie ma, jest
// celem; dopóki nikt nic nie rozstawił, wszystkie prostokąty są zerowe i celem zostaje korzeń.
Node* Root::under(Spot where) {
    Node* at = this;

    for (;;) {
        Node* into = nullptr;

        for (const std::shared_ptr<Node>& one : at->children()) {
            const Area& box = one->area();

            if (where.x < box.at.x || where.x >= box.at.x + box.size.wide || where.y < box.at.y ||
                where.y >= box.at.y + box.size.high)
                continue;

            // Wyżej leżące wygrywa z niżej leżącym, a przy równej warstwie — późniejsze, bo
            // rysuje się na wierzchu.
            if (into == nullptr || one->layer() >= into->layer()) into = one.get();
        }

        if (into == nullptr) return at;

        where.x -= into->area().at.x;
        where.y -= into->area().at.y;

        at = into;
    }
}

void Root::released() {
    if (--busy_ > 0) return;

    // Zwolnienie może pociągnąć kolejne zdejmowania w destruktorach, więc lista musi być
    // najpierw odczepiona, a dopiero potem opróżniona.
    const std::vector<std::shared_ptr<Node>> taken = std::move(dying_);

    dying_.clear();
}

void Root::linger(std::shared_ptr<Node> one) {
    if (busy_ > 0) dying_.push_back(std::move(one));
}

// Kursor składamy przejściem w górę po `area_` — tym samym, którym trafia mysz, więc przewinięcie
// i zagnieżdżenie liczą się same z siebie i nie ma czego pamiętać osobno.
//
// Po drodze **sprawdzamy przynależność na każdym piętrze**, zanim przeliczymy współrzędne wyżej.
// To zastępuje to, co dotąd wychodziło samo z siebie: węzeł, którego nie widać, w ogóle nie
// malował, więc nie zgłaszał kursora. Teraz odpowiada zawsze, więc pytamy, czy jego odpowiedź
// mieści się w tym, co dostał — a że każdy przodek sprawdza się osobno, pole zjechane pod krawędź
// przewijanego okna wypada tam, gdzie powinno, a nie dopiero na krawędzi ekranu.
//
// Wołać **po** złożeniu klatki: `area_` i przewinięcie w polu ustawiają się dopiero przy malowaniu.
namespace {

// Najbliższa prośba w poddrzewie. Chodzimy po całym drzewie, bo węzeł zasłonięty albo o zerowym
// polu też ma prawo się animować — a drzewo interfejsu liczy dziesiątki węzłów, nie tysiące.
[[nodiscard]] std::optional<std::chrono::milliseconds> soonest(const Node& one) {
    std::optional<std::chrono::milliseconds> made = one.due();

    for (const std::shared_ptr<Node>& child : one.children()) {
        const std::optional<std::chrono::milliseconds> said = soonest(*child);

        if (!said.has_value()) continue;

        if (!made.has_value() || *said < *made) made = said;
    }

    return made;
}

}

// Korzeń **nie ma własnej prośby** — zbiera cudze. Dzięki temu `Term` pyta jedno miejsce i nie musi
// wiedzieć, że w środku siedzi obrazek.
std::optional<std::chrono::milliseconds> Root::due() const {
    std::optional<std::chrono::milliseconds> made;

    for (const std::shared_ptr<Node>& one : children()) {
        const std::optional<std::chrono::milliseconds> said = soonest(*one);

        if (!said.has_value()) continue;

        if (!made.has_value() || *said < *made) made = said;
    }

    return made;
}

std::optional<Spot> Root::caret() const {
    if (focus_ == nullptr) return {};

    std::optional<Spot> at = focus_->caret();

    if (!at.has_value()) return {};

    for (const Node* one = focus_; one != nullptr; one = one->parent()) {
        const Area& where = one->area();

        if (at->x < 0 || at->y < 0 || at->x >= where.size.wide || at->y >= where.size.high)
            return {};

        at->x += where.at.x;
        at->y += where.at.y;
    }

    return at;
}

// Ognisko na zdejmowanym węźle albo gdziekolwiek pod nim gaśnie. Bez tego wskazywałoby na
// zmarłego, a wyszłoby to dopiero przy najbliższym klawiszu.
void Root::losing(const Node* one) {
    for (const Node* at = focus_; at != nullptr; at = at->parent())
        if (at == one) {
            focus_ = nullptr;

            return;
        }
}

}

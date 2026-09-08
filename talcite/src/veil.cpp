#include "talcite/veil.hpp"

#include <algorithm>
#include <vector>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/veilwindow.hpp"

namespace talcite::ui {

namespace {

// Wszyscy chętni na ognisko w głąb, w kolejności ułożenia. Wędrówka Tabem chodzi dokładnie po
// tych węzłach, które ognisko wziąć mogą — jedno pojęcie, nie dwa.
//
// **Widżet chętny na ognisko jest dla wędrówki liściem**: co ma w środku, rozstrzyga sam. Bez tej
// reguły Tab w oknie z listą chodziłby po każdym jej wierszu z osobna, bo wiersz też jest chętny.
// Zasłony ta reguła nie dotyczy, bo zbieranie zaczyna się **od jej dzieci** i o jej własną
// chętność nikt tu nie pyta.
void gathering(const Node& from, std::vector<Node*>& into) {
    for (const std::shared_ptr<Node>& one : from.children()) {
        if (one->focusable()) {
            into.push_back(one.get());

            continue;
        }

        gathering(*one, into);
    }
}

// Ognisko mogło zejść głębiej, niż sięga wędrówka — lista oddaje je swojej bieżącej pozycji.
// Przystankiem jest wtedy ten przodek, który na liście przystanków stoi. Węzeł spoza tej listy
// daje pustkę, czyli to samo co brak ogniska: krok naprzód zaczyna wtedy od początku.
Node* landed(const std::vector<Node*>& these, Node* one) {
    for (Node* at = one; at != nullptr; at = at->parent())
        if (std::find(these.begin(), these.end(), at) != these.end()) return at;

    return nullptr;
}

}

Veil::Veil(VeilStyle style) : style_(style) {}

// Zasłona **jest ekranem**, choć rysuje na nim samo pudełko. Stąd bierze się cała modalność:
// `Root::under` wybiera po prostokącie i warstwie, więc kliknięcie obok pudełka trafia w zasłonę,
// bo nie ma tam głębszego dziecka. Nie trzeba do tego żadnej blokady w korzeniu.
Size Veil::wants(Size room) const { return room; }

// Warstwa wyżej niż rodzeństwo — i to wystarczy, żeby leżała na treści i łapała przed nią mysz.
int Veil::layer() const { return 1; }

// Jedno dziecko: pudełko. Ramkę i tytuł rysuje `Frame`, którego wkłada aplikacja — powielanie
// rysowania ramki w drugim miejscu drzewa byłoby drugą prawdą o tym, jak wygląda pudełko.
int Veil::takes() const { return 1; }

// Zasłona bierze ognisko sama, gdy w środku nie ma nic, co by je wzięło — i wtedy połyka klawisze
// jako ona sama, zamiast zostawiać je aplikacji pod spodem.
bool Veil::focusable() const { return true; }

// **Gdzie stanie pudełko — i to jest jedyne, czym zasłony różnią się między sobą.** Domyślnie na
// środku, bo tak stoi okno dialogowe.
//
// Miejsce na cień odejmujemy **przed** ustaleniem położenia, żeby cień nie wychodził poza ekran
// i żeby pudełko stało na środku tego, co naprawdę zostaje. Ta część jest wspólna dla wszystkich
// zasłon, bo dotyczy cienia, a nie tego, kto gdzie stoi — dlatego siedzi tutaj, a nie u tych,
// które położenie liczą inaczej.
Area Veil::box(Size given) const {
    if (children().empty()) return {};

    const int aside = style_.shade ? 2 : 0;
    const int apart = style_.shade ? 1 : 0;

    const Size room{std::max(given.wide - aside, 0), std::max(given.high - apart, 0)};

    Size wants = children().front()->wants(room);

    wants.wide = std::min(wants.wide, room.wide);
    wants.high = std::min(wants.high, room.high);

    return {{std::max((room.wide - wants.wide) / 2, 0), std::max((room.high - wants.high) / 2, 0)},
            wants};
}

View Veil::paint(Size given, Area seen) {
    View made{seen.size};

    // **Zaczynamy od pustki.** Świeża kartka jest wypełniona spacjami, które należą do rysującego,
    // więc zasłona wielkości ekranu zamalowałaby wszystko pod sobą — a ona ma być prostokątem
    // z pudełkiem, przez który resztę widać. Ta sama sztuczka co w menu i z tego samego powodu.
    made.bare({{0, 0}, seen.size});

    if (children().empty()) return made;

    const Area where = box(given);

    if (where.size.wide <= 0 || where.size.high <= 0) return made;

    const Area part = Area::crossed(seen, where);

    made.within(children().front()->render(where, part - where.at), part.at - seen.at);

    // Cień pada w prawo i w dół, przesunięty o wiersz i o dwie kolumny — ta sama reguła co pod
    // menu, bo w Midnight Commanderze jest to ten sam cień. Litery spod spodu zostają, tylko
    // przygasają.
    if (!style_.shade) return made;

    const int last_x = where.at.x + where.size.wide - 1;
    const int last_y = where.at.y + where.size.high - 1;

    const Style dark = skin().get<Style>("talcite.shade");

    made.shade({{last_x + 1 - seen.at.x, where.at.y + 1 - seen.at.y}, {2, where.size.high - 1}},
               dark);

    made.shade({{where.at.x + 2 - seen.at.x, last_y + 1 - seen.at.y}, {where.size.wide, 1}}, dark);

    return made;
}

// **Granica wędrówki wychodzi z modalności, a nie z osobnego pojęcia.** Tab bąbelkuje jak każdy
// klawisz, a obsługuje go ten, kto przestawia ognisko wśród swoich chętnych potomków. Skoro zasłona
// i tak połyka wszystko, Tab nigdy z niej nie ucieka i nie trzeba tego osobno pilnować.
//
// Bez ogniska w środku krok naprzód staje na **pierwszym chętnym**, krok wstecz na ostatnim.
// Nie na przycisku podstawowym: w oknie, które zaczyna się polem tekstowym, pierwsza wpisana litera
// bywa wtedy klawiszem aktywnym przycisku i okno zamyka się z pustym polem. Zasłona nie musi przy
// tym wiedzieć nic ani o polach, ani o przyciskach — kolejność ustala ten, kto składa okno.
void Veil::step(int stops) {
    std::vector<Node*> these;

    gathering(*this, these);

    if (these.empty()) return;

    Root* top = root();

    if (top == nullptr) return;

    const auto at = std::find(these.begin(), these.end(), landed(these, top->focus()));

    if (at == these.end()) {
        if (stops >= 0)
            these.front()->focus();
        else
            these.back()->focus();

        return;
    }

    const int much = static_cast<int>(these.size());
    const int which = static_cast<int>(at - these.begin());

    these[static_cast<std::size_t>(((which + stops) % much + much) % much)]->focus();
}

// **Tu jest modalność klawiatury.** Ognisko siedzi w zasłonie, więc klawisz, którego nikt po drodze
// nie wziął, dochodzi tutaj i dalej nie idzie. Nie ma w tym nic nowego: dokładnie tak zachowuje się
// otwarte menu.
//
// Zamknięcie zdejmuje zasłonę z drzewa w środku doręczania, ale węzeł przeżywa je dzięki `linger`
// w korzeniu — dlatego wolno tu wrócić prawdą po `close()`.
// **Escape'a zasłona nie bierze.** Zamykał okno — czyli wykonywał czynność, której wołający nie
// miał jak odwołać, a to jest dokładnie ta sama decyzja, którą podjęliśmy, usuwając z dialogu
// domyślne naciskanie przycisku podstawowego. Kto chce „Esc zamyka", wiesza `on<KeyEvent>` na
// swojej zasłonie i woła `close()` sam.
//
// Połykanie reszty zostaje, bo połykanie **niczego nie robi** — jest całym zadaniem zasłony
// i jedynym sposobem, w jaki modalność da się w ogóle wyrazić.
bool Veil::react(KeyEvent& what) {
    const Code code = what.key().code();

    if (code == Code::tab) {
        step(what.mods().shift ? -1 : 1);

        return true;
    }

    return true;
}

// Kliknięcie obok pudełka **połykamy i nic więcej**. Midnight Commander nie zamyka okna klikiem
// w tło; zamyka je Esc albo przycisk. Klik w samo pudełko tu nie dochodzi — bierze go dziecko.
bool Veil::react(ClickEvent&) { return true; }

// Surowe zdarzenie myszy i kółko też, żeby zaznaczanie ciągnięciem i przewijanie panelu pod spodem
// nie działały przez zamknięte okno.
bool Veil::react(MouseEvent&) { return true; }

bool Veil::react(WheelEvent&) { return true; }

void Veil::close() {
    if (VeilWindow* window = dynamic_cast<VeilWindow*>(parent())) window->close(this);
}

// **Zamknięcie ogłasza się na zasłonie i zanim zejdzie ona z drzewa.** Po zdjęciu zasłona nie ma
// rodzica, więc zdarzenie nie miałoby dokąd bąbelkować, a uchwyt wołającego — który wisi właśnie
// na niej — nie zobaczyłby niczego. Ogłasza to okno, bo przez nie przechodzi każde zamknięcie:
// i to klawiszem, i to wywołane z programu.
//
// To jest jedyna wiadomość o tym, że okno się skończyło. Esc nie ma przycisku, więc bez niej
// „zamknięto bez odpowiedzi" nie dałoby się w ogóle usłyszeć.
void Veil::closing() {
    CloseEvent one;

    (void)trigger(one);
}

}

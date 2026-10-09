#include "talcite/modal.hpp"

#include <algorithm>
#include <vector>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "stops.hpp"

namespace talcite::ui {

namespace {

// Ognisko mogło zejść głębiej, niż sięga wędrówka — lista oddaje je swojej bieżącej pozycji.
// Przystankiem jest wtedy ten przodek, który na liście przystanków stoi. Węzeł spoza tej listy
// daje pustkę, czyli to samo co brak ogniska: krok naprzód zaczyna wtedy od początku.
Node* landed(const std::vector<Node*>& these, Node* one) {
    for (Node* at = one; at != nullptr; at = at->parent())
        if (std::find(these.begin(), these.end(), at) != these.end()) return at;

    return nullptr;
}

}

Modal::Modal(ModalShape shape) : shape_(shape) {}

// Zasłona **jest ekranem**, choć rysuje na nim samo pudełko. Stąd bierze się cała modalność:
// `Root::under` wybiera po prostokącie i warstwie, więc kliknięcie obok pudełka trafia w zasłonę,
// bo nie ma tam głębszego dziecka. Nie trzeba do tego żadnej blokady w korzeniu.
Size Modal::wants(Size room) const { return room; }

// Jedno dziecko: pudełko. Ramkę i tytuł rysuje `Frame`, którego wkłada aplikacja — powielanie
// rysowania ramki w drugim miejscu drzewa byłoby drugą prawdą o tym, jak wygląda pudełko.
int Modal::capacity() const { return 1; }

// Zasłona bierze ognisko sama, gdy w środku nie ma nic, co by je wzięło — i wtedy połyka klawisze
// jako ona sama, zamiast zostawiać je aplikacji pod spodem.
bool Modal::focusable() const { return true; }

// **Gdzie stanie pudełko — i to jest jedyne, czym zasłony różnią się między sobą.** Domyślnie na
// środku, bo tak stoi okno dialogowe.
//
// Miejsce na cień odejmujemy **przed** ustaleniem położenia, żeby cień nie wychodził poza ekran
// i żeby pudełko stało na środku tego, co naprawdę zostaje. Ta część jest wspólna dla wszystkich
// zasłon, bo dotyczy cienia, a nie tego, kto gdzie stoi — dlatego siedzi tutaj, a nie u tych,
// które położenie liczą inaczej.
Area Modal::seat(Size given) const {
    if (children().empty()) return {};

    const int aside = shape_.shade ? 2 : 0;
    const int apart = shape_.shade ? 1 : 0;

    const Size room{std::max(given.wide - aside, 0), std::max(given.high - apart, 0)};

    Size wants = children().front()->wants(room);

    wants.wide = std::min(wants.wide, room.wide);
    wants.high = std::min(wants.high, room.high);

    return {{std::max((room.wide - wants.wide) / 2, 0), std::max((room.high - wants.high) / 2, 0)},
            wants};
}

View Modal::paint(Size given, Area seen) {
    View made{seen.size};

    // **Zaczynamy od pustki.** Świeża kartka jest wypełniona spacjami, które należą do rysującego,
    // więc zasłona wielkości ekranu zamalowałaby wszystko pod sobą — a ona ma być prostokątem
    // z pudełkiem, przez który resztę widać. Ta sama sztuczka co w menu i z tego samego powodu.
    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    const Area where = seat(given);

    if (where.size.wide <= 0 || where.size.high <= 0) return made;

    const Area part = Area::crossed(seen, where);

    made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

    // Cień pada w prawo i w dół, przesunięty o wiersz i o dwie kolumny — ta sama reguła co pod
    // menu, bo w Midnight Commanderze jest to ten sam cień. Litery spod spodu zostają, tylko
    // przygasają.
    if (!shape_.shade) return made;

    const int last_x = where.at.x + where.size.wide - 1;
    const int last_y = where.at.y + where.size.high - 1;

    const Style dark = skin().style("talcite.shade");

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
void Modal::step(int by) {
    const std::vector<Node*> these = stops(*this);

    if (these.empty()) return;

    Root* top = root();

    if (top == nullptr) return;

    const auto at = std::find(these.begin(), these.end(), landed(these, top->focused_node()));

    if (at == these.end()) {
        if (by >= 0)
            these.front()->focus();
        else
            these.back()->focus();

        return;
    }

    const int much = static_cast<int>(these.size());
    const int which = static_cast<int>(at - these.begin());

    these[static_cast<std::size_t>(((which + by) % much + much) % much)]->focus();
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
bool Modal::handle(KeyEvent& what) {
    const Code code = what.key().code();

    if (code == Code::tab) {
        step(what.mods().shift ? -1 : 1);

        return true;
    }

    return true;
}

// Kliknięcie obok pudełka **połykamy i nic więcej**. Midnight Commander nie zamyka okna klikiem
// w tło; zamyka je Esc albo przycisk. Klik w samo pudełko tu nie dochodzi — bierze go dziecko.
bool Modal::handle(ClickEvent&) { return true; }

// Surowe zdarzenie myszy i kółko też, żeby zaznaczanie ciągnięciem i przewijanie panelu pod spodem
// nie działały przez zamknięte okno.
bool Modal::handle(MouseEvent&) { return true; }

bool Modal::handle(WheelEvent&) { return true; }

}

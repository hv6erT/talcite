#include "talcite/window.hpp"

#include <algorithm>
#include <optional>

#include "talcite/event.hpp"
#include "talcite/root.hpp"
#include "talcite/windows.hpp"

namespace talcite::ui {

// **Okno niemodalne** — to, w czym mcedit trzyma otwarte pliki. Jest warstwą, jak zasłona
// (`Layer`: zamknięcie, bieżące, pamięć ogniska), ale w tym, co ważne, jej odwrotnością: zasłona
// jest jedna, połyka klawisze i mysz i zasłania wszystko pod sobą; okien jest kilka naraz, żadne
// niczego nie połyka, a to na wierzchu jest tylko **bieżące**, czyli to, w którym siedzi ognisko.
// Dlatego dwie pary klas na wspólnej bazie, a nie jedna z przełącznikiem: cała zasłona to
// modalność, a modalności tu nie ma.
//
// Okno **nie rysuje ramki ani tytułu**, tak samo jak zasłona: od ramki jest `Frame`, którego
// wkłada wołający. Pełne okno (`full`) to po prostu takie, któremu wołający ramkę wyłącza — `Frame`
// ma na to krawędzie z osobna. Drugie rysowanie ramki w bibliotece byłoby drugą prawdą o tym,
// jak wygląda pudełko.

Window::Window() = default;

// Jedno dziecko: treść. Tyle samo, co zasłona.
int Window::capacity() const { return 1; }

// Pełne bierze cały pokój; postawione bierze tyle, ile mu dano — a nie tyle, ile chce treść, bo
// rozmiar okna jest **stanem okna**, którym rusza człowiek (`Resize` w menu), a nie życzeniem
// treści przy każdej klatce.
Size Window::wants(Size room) const { return full_ ? room : seat_.size; }

Area Window::placement(Size room) const {
    if (full_) return {{0, 0}, room};

    return seat_;
}

void Window::full(bool given) { full_ = given; }

void Window::seat(Spot where, Size size) {
    seat_ = {where, {std::max(size.wide, 0), std::max(size.high, 0)}};
    full_ = false;
}

namespace {

// Pokój okna to obszar pojemnika z ostatniej klatki. Okno bez rodzica albo jeszcze nie rysowane
// pokoju nie ma — i wtedy nie ma względem czego liczyć granic.
[[nodiscard]] std::optional<Size> roomed(const Window& one) {
    const Node* above = one.parent();

    if (above == nullptr) return std::nullopt;

    const Size room = above->area().size;

    if (room.wide <= 0 || room.high <= 0) return std::nullopt;

    return room;
}

// Jeden krok wzdłuż jednej osi. Granicy przekroczyć nie wolno, ale **okno, które już za nią
// stoi, nie skacze do niej** — może tylko wracać. Tak liczy mcedit: warunek pyta o miejsce, w którym
// okno stoi teraz, a nie przycina wyniku. Po zwężeniu terminala okno za brzegiem zostaje więc
// tam, gdzie było, i daje się z powrotem wyprowadzić krok po kroku.
[[nodiscard]] int stepped(int from, int by, int low, int high) {
    const int to = from + by;

    if (by < 0) return std::max(to, std::min(from, low));

    return std::min(to, std::max(from, high));
}

}

// **Przesunięcie postawionego okna**, jak `WindowMove` w mcedicie: górny wiersz zostaje w pokoju,
// a z prawej zostaje choć jedna kolumna. W mcedicie warunek w lewo porównuje położenie
// z szerokością pokoju zamiast okna, przez co wąskie okno potrafi zniknąć za lewym brzegiem —
// bierzemy oczywisty zamiar: jedną kolumnę widać zawsze, z której strony by okno nie wychodziło.
// Pełne okno nie ma czego przesuwać.
void Window::move(int across, int down) {
    const std::optional<Size> room = roomed(*this);

    if (full_ || !room) return;

    seat_.at.x = stepped(seat_.at.x, across, 1 - seat_.size.wide, room->wide - 1);
    seat_.at.y = stepped(seat_.at.y, down, 0, room->high - 1);
}

// **Zmiana rozmiaru**, jak `WindowResize`: nie mniej niż `least`, a rośnie tylko do prawego
// i dolnego brzegu pokoju. Lewy górny róg stoi.
void Window::resize(int across, int down) {
    const std::optional<Size> room = roomed(*this);

    if (full_ || !room) return;

    seat_.size.wide = stepped(seat_.size.wide, across, least_.wide, room->wide - seat_.at.x);
    seat_.size.high = stepped(seat_.size.high, down, least_.high, room->high - seat_.at.y);
}

// Najmniejsze okno ma choć jedną kratkę — z mniejszego nie byłoby czego złapać.
void Window::least(Size given) { least_ = {std::max(given.wide, 1), std::max(given.high, 1)}; }

// Na wierzch — razem z ogniskiem, które okno pamięta u siebie. Poza oknem-pojemnikiem nie znaczy
// nic, bo „na wierzchu" jest własnością listy, nie węzła.
void Window::raise() {
    if (auto* window = dynamic_cast<Windows*>(parent())) window->raise(*this);
}

View Window::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    made.draw(children().front()->render({{0, 0}, given}, seen), {0, 0});

    return made;
}

// **Kliknięcie w okno podnosi je**, ale go nie połyka: niemodalne okno nie ma prawa zjadać myszy,
// a człowiek, który kliknął w tekst pod spodem, chce i podnieść okno, i postawić tam kursor.
// Dochodzi tu tylko to, czego treść nie wzięła — po kliknięcie w samą treść trzeba będzie wrócić,
// gdy dojdą okna nachodzące na siebie.
bool Window::handle(ClickEvent&) {
    raise();

    return false;
}

// **Ciągnięcie myszą**, jak w mcedicie: wciśnięcie na górnym wierszu postawionego okna chwyta
// położenie, w prawym dolnym rogu — rozmiar. Wciśnięcie podnosi okno, także poza tymi miejscami,
// bo w mcedicie okno wybiera już wciśnięcie. Dochodzi tu tylko to, czego nie wzięła treść — przy
// ramce górny wiersz i róg należą do ramki, a ta myszy nie bierze.
//
// Po chwycie okno prosi korzeń o przytrzymanie, żeby ruch dochodził do niego, gdziekolwiek jest
// kursor. Ruch liczymy **od stanu okna**, nie od ostatniego ruchu: położenie to kursor minus
// miejsce chwytu, rozmiar — kursor minus lewy górny róg, plus jeden. Przez `move` i `resize`
// idą więc te same granice co przy strzałkach, a gdy kursor wraca zza granicy, okno wraca za nim
// dokładnie tam, gdzie kursor go trzyma. Puszczenie kończy.
bool Window::handle(MouseEvent& what) {
    if (what.press() == Press::up) {
        const bool was = drag_ != Drag::none;

        drag_ = Drag::none;

        return was;
    }

    if (what.press() == Press::move) {
        if (drag_ == Drag::none) return false;

        // Chwycone okno stoi w narysowanym pojemniku: bez tego nie dostałoby wciśnięcia, a gdy
        // z niego zejdzie, przytrzymanie je omija i ruch tu nie dochodzi.
        const Area room = parent()->screen_area().value_or(Area{});
        const Spot at{what.where().x - room.at.x, what.where().y - room.at.y};

        if (drag_ == Drag::move)
            move(at.x - grip_.x - seat_.at.x, at.y - grip_.y - seat_.at.y);
        else
            resize(at.x - seat_.at.x + 1 - seat_.size.wide, at.y - seat_.at.y + 1 - seat_.size.high);

        return true;
    }

    drag_ = Drag::none;

    if (what.button() != Mouse::left) return false;

    raise();

    const std::optional<Area> here = screen_area();

    if (full_ || !here) return false;

    const Spot at{what.where().x - here->at.x, what.where().y - here->at.y};

    if (at.y == 0)
        drag_ = Drag::move;
    else if (at.x == here->size.wide - 1 && at.y == here->size.high - 1)
        drag_ = Drag::resize;
    else
        return false;

    grip_ = at;

    if (Root* top = root(); top != nullptr) top->capture(*this);

    return true;
}

}

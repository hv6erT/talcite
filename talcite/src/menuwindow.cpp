#include "talcite/menuwindow.hpp"

#include <algorithm>
#include <utility>

#include "talcite/menu.hpp"
#include "talcite/root.hpp"

namespace talcite::ui {

// Wiersz pod paskiem stawiamy sami, bo tak wygląda okno z menu: na górze tytuły, pod nimi
// treść. Aplikacja wkłada swoje panele do body(), a menu zgłasza przez menu() — i nie ma
// trzeciej drogi, którą dałoby się to złożyć źle.
MenuWindow::MenuWindow() : body_(std::make_shared<Row>()) { children().add(body_); }

// Miejsce jest dokładnie na wiersz treści i na tyle menu, ile zgłoszono — a oba zajmujemy sami,
// więc add z zewnątrz nigdy nie przejdzie.
int MenuWindow::takes() const { return 1 + static_cast<int>(menus_.size()); }

// Pasek zajmuje cały ekran, bo rozwinięte menu musi mieć się gdzie zmieścić — a dziecko nigdy
// nie wychodzi poza rodzica. Treść dostaje wszystko poniżej wiersza z tytułami.
Size MenuWindow::wants(Size room) const { return room; }

// **Menu robi pasek, a nie wołający.** Wchodzi **wprost pod pasek**, a nie do wiersza treści:
// pasek maluje tytuły sam, w wierszu zerowym, a rozwiniętą listę na końcu, żeby leżała na treści.
// Do wektora wpisujemy przed dopięciem, bo to on otwiera miejsce w `takes()`.
std::shared_ptr<Menu> MenuWindow::add(std::string title, MenuStyle style) {
    auto one = std::make_shared<Menu>(std::move(title), style);

    menus_.push_back(one);

    try {
        children().add(one);
    } catch (...) {
        menus_.pop_back();

        throw;
    }

    return one;
}

// Opróżnienie paska: zdejmujemy i z wektora, i z drzewa, bo inaczej takes() liczyłby miejsca
// po menu, których już nie ma. Ta sama reguła i ten sam powód co w legendzie.
void MenuWindow::clear() {
    const std::vector<std::shared_ptr<Menu>> these = menus_;

    menus_.clear();

    for (const std::shared_ptr<Menu>& one : these) (void)children().remove(one.get());
}

int MenuWindow::which(const Menu* one) const {
    for (int at = 0; at < static_cast<int>(menus_.size()); ++at)
        if (menus_[static_cast<std::size_t>(at)].get() == one) return at;

    return -1;
}

// Pytamy **o całą gałąź w górę**, bo ognisko siedzi na pozycji, a nie na samym menu. Bez tego
// pasek nie rozpoznałby własnego menu w zdarzeniu i zapamiętałby jako „ognisko sprzed otwarcia"
// pozycję, którą sam przed chwilą podświetlił.
bool MenuWindow::ours(const Node* one) const {
    for (const Node* at = one; at != nullptr; at = at->parent())
        for (const std::shared_ptr<Menu>& mine : menus_)
            if (mine.get() == at) return true;

    return false;
}

Menu* MenuWindow::opened() const {
    for (const std::shared_ptr<Menu>& one : menus_)
        if (one->opened()) return one.get();

    return nullptr;
}

int MenuWindow::width(int one) const {
    return 2 + measured(menus_[static_cast<std::size_t>(one)]->title()).wide;
}

int MenuWindow::along(int one) const {
    int x = 0;

    for (int at = 0; at < one; ++at) x += width(at);

    return x;
}

// Zamknięcie to oddanie ogniska temu, kto je miał przed otwarciem. Menu gaśnie samo, bo otwarte
// znaczy „mam ognisko" — nie ma tu żadnego drugiego stanu do przestawienia.
void MenuWindow::close() {
    Root* top = root();

    if (top == nullptr) return;

    std::shared_ptr<Node> was = was_.lock();

    was_.reset();

    if (was && was->root() != top) was.reset();

    top->focus(was.get());
}

View MenuWindow::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    const Area below{{0, 1}, {given.wide, std::max(given.high - 1, 0)}};
    const Area under = Area::crossed(seen, below);

    made.within(body_->render(below, under - below.at), under.at - seen.at);

    // Zamknięte menu to sam tytuł w wierszu zerowym. Otwarte maluje się na końcu, bo leży na
    // treści — czyli na swoim rodzeństwie.
    for (int at = 0; at < static_cast<int>(menus_.size()); ++at) {
        Menu& one = *menus_[static_cast<std::size_t>(at)];

        if (one.opened()) continue;

        one.aside_ = 0;

        const Area where{{along(at), 0}, {width(at), 1}};
        const Area part = Area::crossed(seen, where);

        made.within(one.render(where, part - where.at), part.at - seen.at);
    }

    if (Menu* one = opened()) {
        const int at = which(one);
        const int x = along(at);

        const Size wants = one->wants(given);

        // Ostatnie menu nie mieści się rozwinięte przy prawej krawędzi, więc wysuwa się w lewo.
        // O ile — wie tylko pasek, bo tylko on zna szerokość ekranu.
        const int from = x + wants.wide > given.wide ? std::max(given.wide - wants.wide, 0) : x;

        one->aside_ = x - from;

        const int wide = std::max(wants.wide, one->aside_ + width(at));

        const Area where{{from, 0},
                         {std::min(wide, std::max(given.wide - from, 0)),
                          std::min(wants.high, given.high)}};
        const Area part = Area::crossed(seen, where);

        made.within(one->render(where, part - where.at), part.at - seen.at);
    }

    return made;
}

// **Klawisz otwierający podaje wołający**, a nie biblioteka. Stało tu wpisane na sztywno F9 —
// pasek zajmował sobie kombinację, o której nikt mu nie powiedział, i nie dało się jej ani zmienić,
// ani wyłączyć. Dziś pasek jest węzłem z klawiszami, jak pozycja menu, tytuł menu i pole legendy:
// `keys().add(Code::f9)` przywraca dawne zachowanie, a pusty zbiór znaczy „z klawiatury nie".
//
// Czynność jest przełącznikiem, bo taka była od początku: zamknięty pasek otwiera się na pierwszym
// menu, otwarty zamyka.
void MenuWindow::act() {
    if (menus_.empty()) return;

    if (opened() == nullptr)
        menus_.front()->focus();
    else
        close();
}

bool MenuWindow::react(KeyEvent& what) {
    if (menus_.empty()) return false;

    Menu* one = opened();

    const int much = static_cast<int>(menus_.size());

    if (keys().contains(what.key())) {
        act();

        return true;
    }

    switch (what.key().code()) {
        case Code::escape:
            if (one == nullptr) return false;

            close();

            return true;

        case Code::left:
        case Code::right: {
            if (one == nullptr) return false;

            const int step = what.key().code() == Code::left ? -1 : 1;

            menus_[static_cast<std::size_t>((which(one) + step + much) % much)]
                ->focus();

            return true;
        }

        // **Litera tytułu działa dopiero przy czynnym pasku**, czyli po F9 — tak jak w Midnight
        // Commanderze. Bez tego warunku zwykła litera naciśnięta nad panelem rozwijałaby menu,
        // a to jest ostatnia rzecz, jakiej się po niej spodziewać.
        //
        // Pozycje otwartego menu dostają literę **wcześniej**, bo `Menu::react` leży bliżej
        // ogniska i rozdaje ją swoim dzieciom. Tutaj trafia więc tylko to, czego nie wziął nikt
        // w środku — i wtedy pyta tytułów. Rozdajemy po migawce, bo uchwyt ma prawo przebudować
        // pasek w reakcji.
        //
        // **Otwarte menu połyka wszystko, czego samo nie obsłużyło.** W Midnight Commanderze
        // rozwinięte menu jest modalne: F5 przy nim nie kopiuje. Dopóki nad paskiem menu nic nie
        // stało, taki klawisz i tak nie miał dokąd bąbelkować i nikt tego nie zauważył — legenda
        // stojąca wyżej złapałaby go i zrobiła czynność, o którą nikt nie prosił.
        default: {
            if (one == nullptr) return false;

            const std::vector<std::shared_ptr<Menu>> these = menus_;

            for (const std::shared_ptr<Menu>& each : these)
                if (offer(what, *each)) return true;

            return true;
        }
    }
}

// Ognisko sprzed otwarcia zapamiętujemy tu, bo tylko tędy przechodzi każde otwarcie — klawiszem,
// klikiem czy strzałką w bok. Przejście z menu na menu nie liczy się: gdybyśmy zapamiętali menu,
// Esc otwierałby poprzednie zamiast wracać do aplikacji.
bool MenuWindow::react(FocusEvent& what) {
    if (!ours(dynamic_cast<const Node*>(what.at()))) return false;

    Node* other = what.other();

    if (ours(other)) return false;

    was_ = other != nullptr ? other->weak_from_this() : std::weak_ptr<Node>{};

    return false;
}

}

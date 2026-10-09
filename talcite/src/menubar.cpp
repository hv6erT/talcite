#include "talcite/menubar.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "talcite/menu.hpp"
#include "talcite/root.hpp"

#include "menutitle.hpp"
#include "slot.hpp"

namespace talcite::ui {

// **Pasek menu to stos z dwoma piętrami.** Na dole kolumna: rząd tytułów nad wierszem treści.
// Na górze — tylko gdy menu jest rozwinięte — samo menu, które mówi stosowi, gdzie stanąć:
// pod swoim tytułem. Malowanie i trafianie myszą robi `Stack`, układanie tytułów robi `Row`;
// pasek nie liczy już ani szerokości, ani położeń, ani nie maluje niczego na końcu.
//
//     Menubar (Stack)
//     ├── Column
//     │   ├── Row  ← MenuTitle × N
//     │   └── Row  ← gniazdo na treść aplikacji (content)
//     └── [Menu]  ← dopięte tylko gdy rozwinięte; placed() pod swoim tytułem
//
// Rozwinięte menu jest **dzieckiem paska**, więc klawisz od pozycji bąbelkuje pozycja → menu →
// pasek: strzałki w bok i Esc dochodzą tą samą drogą, co przedtem. Menu nie jest zasłoną, więc
// klik w sąsiedni tytuł trafia w tytuł, a pasek stoi pod legendą, więc lista nie zakryje paska
// klawiszy. Trzy koszty, które miałoby menu-zasłona, tu nie powstają wcale.
Menubar::Menubar()
    : frame_(std::make_shared<Column>()),
      bar_(std::make_shared<Row>()),
      body_(std::make_shared<Row>()) {
    frame_->children().add(bar_);
    frame_->children().add(body_);

    children().add(frame_);
}

// Treść pod spodem. Gniazdo opisuje `slot.hpp`.
Node* Menubar::content() const { return held(*body_); }

void Menubar::content(std::shared_ptr<Node> given) { hold(*body_, std::move(given)); }

// Dwa miejsca: kolumna i co najwyżej jedno rozwinięte menu.
int Menubar::capacity() const { return 2; }

// Przyjmujemy wyłącznie swoje: kolumnę i własne menu. Obce dziecko nie ma jak tu wejść, bo stos
// dałby mu miejsce, a pasek nie wiedziałby, co z nim zrobić.
bool Menubar::accepts(const Node& one) const {
    if (&one == frame_.get()) return true;

    for (const std::shared_ptr<Menu>& mine : menus_)
        if (mine.get() == &one) return true;

    return false;
}

// **Menu robi pasek, a nie wołający.** Tytuł idzie do rzędu tytułów od razu; menu zostaje poza
// drzewem, dopóki ktoś go nie rozwinie. Oba znają siebie nawzajem przez pasek: tytuł trzyma menu,
// menu trzyma tytuł i pasek — surowymi wskazaniami, bo wszystkie trzy żyją i umierają razem tutaj.
std::shared_ptr<Menu> Menubar::add(std::string title, MenuShape shape) {
    auto one = std::make_shared<Menu>(std::move(title), shape);
    auto head = std::make_shared<MenuTitle>(*one);

    one->bar_ = this;
    one->head_ = head.get();

    menus_.push_back(one);

    try {
        bar_->children().add(head);
    } catch (...) {
        menus_.pop_back();

        throw;
    }

    return one;
}

void Menubar::clear() {
    fold();

    expanded_ = false;

    bar_->children().clear();
    menus_.clear();
}

// Menu, na którym stoi pasek: rozwinięte, albo to, którego tytuł ma ognisko — czynne, zwinięte.
// Publiczne, bo tytuł jest węzłem wewnętrznym i z zewnątrz nie ma go jak zapytać o ognisko —
// a „które menu jest bieżące" to pytanie, które zadaje i aplikacja, i próba.
Menu* Menubar::current() const {
    for (const std::shared_ptr<Menu>& one : menus_)
        if (one->opened()) return one.get();

    for (const std::shared_ptr<Menu>& one : menus_)
        if (one->head_ != nullptr && one->head_->focused()) return one.get();

    return nullptr;
}

int Menubar::find(const Menu* one) const {
    for (int at = 0; at < static_cast<int>(menus_.size()); ++at)
        if (menus_[static_cast<std::size_t>(at)].get() == one) return at;

    return -1;
}

// Nasze jest wszystko, co siedzi w rzędzie tytułów albo w rozwiniętym menu — pytamy o całą gałąź
// w górę, bo ognisko siedzi na pozycji albo na tytule, nie na pasku.
bool Menubar::ours(const Node* one) const {
    for (const Node* at = one; at != nullptr; at = at->parent()) {
        if (at == bar_.get()) return true;

        for (const std::shared_ptr<Menu>& mine : menus_)
            if (mine.get() == at) return true;
    }

    return false;
}

// **Rozwinięcie to dopięcie.** Co najwyżej jedno menu naraz — więc najpierw zwijamy to, co stało,
// a potem dopinamy nowe jako ostatnie dziecko stosu, czyli na wierzch.
void Menubar::unfold(Menu& one) {
    if (one.parent() == this) return;

    fold();

    for (const std::shared_ptr<Menu>& mine : menus_)
        if (mine.get() == &one) {
            children().add(mine);

            expanded_ = true;

            return;
        }
}

// Zwinięcie rozwiniętego menu robi samo menu (`Menu::close`) — pasek nie sięga już do jego
// numeru pozycji. Bieżące menu, które jest tylko podświetlonym tytułem, zamknięcie zostawia w spokoju.
void Menubar::fold() {
    if (Menu* one = current()) one->close();
}

// **Wejście do paska to ognisko na pierwszym tytule, nic nie rozwinięte** — jak w Midnight
// Commanderze przy `drop_menus` wyłączonym; rozwija dopiero strzałka w dół, Enter, litera albo
// klik. Z `dropdown()` wejście od razu rozwija pierwsze menu — jak MC z „Drop down menus" — i stawia
// bit rozwinięcia, więc strzałki w bok idą dalej rozwinięte; to ta sama droga, co strzałka w dół
// po F9, tylko bez czekania na nią. Pierwsze menu bez pozycji zostaje zwinięte, jak przy strzałce.
// Publiczne do pary z `leave()`, bo pole paska klawiszy musi wchodzić tą samą drogą, co
// klawisz — inaczej klik w pole robił co innego niż F9. Pasek już czynny zostaje, jak stoi.
void Menubar::enter() {
    if (menus_.empty() || current() != nullptr) return;

    expanded_ = false;

    if (dropdown_) {
        menus_.front()->open();
        expanded_ = true;

        return;
    }

    if (Node* head = menus_.front()->head_) head->focus();
}

// Wyjście z paska: zwinięcie i oddanie ogniska temu, kto je miał przed wejściem.
void Menubar::leave() {
    fold();

    expanded_ = false;

    Root* top = root();

    if (top == nullptr) return;

    std::shared_ptr<Node> was = was_.lock();

    was_.reset();

    if (was && was->root() != top) was.reset();

    top->move_focus(was.get());
}

// **Klawisz otwierający podaje wołający**, a nie biblioteka: `keys().add(Code::f9)` przywraca dawne
// zachowanie, a pusty zbiór znaczy „z klawiatury nie". Czynność jest przełącznikiem.
void Menubar::activate() {
    if (current() == nullptr) enter();
    else leave();
}

bool Menubar::handle(KeyEvent& what) {
    if (menus_.empty()) return false;

    Menu* one = current();

    const int much = static_cast<int>(menus_.size());

    if (keys().contains(what.key())) {
        activate();

        return true;
    }

    switch (what.key().code()) {
        case Code::escape:
            if (one == nullptr) return false;

            leave();

            return true;

        case Code::left:
        case Code::right: {
            if (one == nullptr) return false;

            const int step = what.key().code() == Code::left ? -1 : 1;

            // **Sąsiad przejmuje stan bieżącego**: przy rozwiniętym menu strzałka rozwija następne,
            // przy zwiniętym tylko przesuwa podświetlenie. Tak chodzi pasek Midnight Commandera.
            // Pytamy **paska**, a nie bieżącego menu: menu bez pozycji nigdy nie jest rozwinięte,
            // więc wędrówka przez taki tytuł gubiłaby stan i sąsiad wychodziłby zwinięty.
            const std::shared_ptr<Menu>& next =
                menus_[static_cast<std::size_t>((find(one) + step + much) % much)];

            if (expanded_) {
                // Zwijamy wprost, bo menu bez pozycji nie rozwija się i samo by nie zwinęło
                // poprzednika — a stan paska ma przejść dalej mimo to.
                fold();
                next->open();
                expanded_ = true;
            } else if (next->head_ != nullptr) {
                next->head_->focus();
            }

            return true;
        }

        // **Litera tytułu działa dopiero przy czynnym pasku**, czyli po F9 — tak jak w Midnight
        // Commanderze. Bez tego warunku zwykła litera naciśnięta nad panelem rozwijałaby menu.
        //
        // Pozycje rozwiniętego menu dostają literę **wcześniej**, bo `Menu::handle` leży bliżej
        // ogniska. Tutaj trafia więc tylko to, czego nie wziął nikt w środku — i wtedy pyta
        // tytułów. Rozdajemy po migawce, bo uchwyt ma prawo przebudować pasek w reakcji.
        //
        // **Czynny pasek połyka wszystko, czego sam nie obsłużył.** W Midnight Commanderze
        // rozwinięte menu jest modalne: F5 przy nim nie kopiuje. Legenda stoi wyżej i złapałaby
        // klawisz, robiąc czynność, o którą nikt nie prosił.
        default: {
            if (one == nullptr) return false;

            const std::vector<std::shared_ptr<Menu>> these = menus_;

            for (const std::shared_ptr<Menu>& each : these)
                if (each->head_ != nullptr && offer(what, *each->head_)) return true;

            return true;
        }
    }
}

// Ognisko sprzed wejścia zapamiętujemy tu, bo tylko tędy przechodzi każde wejście — klawiszem,
// klikiem czy strzałką w bok. Przejście z tytułu na tytuł i z tytułu na pozycję nie liczy się:
// gdybyśmy zapamiętali tytuł, Esc otwierałby poprzednie zamiast wracać do aplikacji.
bool Menubar::handle(FocusEvent& what) {
    if (!ours(dynamic_cast<const Node*>(what.target()))) return false;

    Node* other = what.other();

    if (ours(other)) return false;

    was_ = other != nullptr ? other->weak_from_this() : std::weak_ptr<Node>{};

    return false;
}

// **Ognisko wychodzi z paska — pasek się zwija.** Przedtem działo się to samo z siebie, bo
// „rozwinięte" wynikało z ogniska; teraz rozwinięcie jest dopięciem, więc trzeba je zdjąć wprost.
// Zdarzenie idzie od starego ogniska w górę, więc dociera tu zawsze, obojętne dokąd ognisko
// poszło — także do zasłony, która stoi poza paskiem.
bool Menubar::handle(UnfocusEvent& what) {
    if (ours(what.other())) return false;

    fold();

    expanded_ = false;

    return false;
}

}

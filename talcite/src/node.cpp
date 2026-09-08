#include "talcite/node.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

Node::Node() : inside_(*this) {}

// Dziecko może żyć dłużej niż rodzic, jeśli ktoś trzyma na nie wskaźnik dzielony. Wskazanie
// na rodzica trzeba wtedy zerwać, inaczej zostałby wskaźnik na zmarłego — a po nim sięga
// wędrówka zdarzeń.
Node::~Node() = default;

// Rola to rdzeń nazwy w skórce, z którego węzeł składa swoje warianty: `.focus`, `.key`, `.off`,
// `.title`. Mieszka w `Node`, a nie w każdym widżecie z osobna, bo jest cechą **węzła, który
// maluje** — a które węzły malują, rozstrzyga `paint`, nie typ. Pojemniki niosą pusty napis
// i nic z tego nie mają; koszt to 32 bajty na węzeł. Ustawiacz jest wirtualny, bo węzeł złożony
// z innych — okno komunikatu — musi przekazać rdzeń swoim częściom.
void Node::role(std::string given) { role_ = std::move(given); }

// Zrywanie więzi należy do dzieci, bo to one je trzymają. `Node` ginie razem ze swoim polem, więc
// wykonuje się to dokładnie raz i dokładnie wtedy, kiedy trzeba.
Children::~Children() {
    for (const std::shared_ptr<Node>& one : inside_) one->up_ = nullptr;
}

// Ile dzieci węzeł przyjmuje, wie tylko on sam. Domyślnie jedno: zwykły węzeł jest opakowaniem,
// a nie układem — układanie kilkorga obok siebie należy do Row i Column, i tylko one oddają tu
// plenty. Napis oddaje zero, bo nie ma czego trzymać w środku.
int Node::takes() const { return 1; }

// **`takes()` mówi ile, `accepts()` mówi czego.** Zwykły węzeł nie zna się na rodzajach i bierze,
// co dadzą — układ, ramka i zasłona układają wszystko tak samo. Przesłania to ten, kto ma wśród
// dzieci **rolę do obsadzenia**: menu ma pozycje, lista ma wiersze, i obie potem po tych dzieciach
// chodzą, zakładając, czym są.
//
// Bez tego założenie było tylko wypowiedziane. `Menu::item()` sięga po `dynamic_cast<MenuItem*>`
// przy każdym dostępie — z wędrówki strzałkami, z malowania, z naciśnięcia — i oddaje pustkę dla
// obcego dziecka, którego nikt nie zabronił dopiąć. Sprawdzenie **raz, przy dopięciu**, jest
// tańsze i uczciwsze niż pustka oddawana w kółko.
bool Node::accepts(const Node&) const { return true; }

// **Ognisko oddajemy najbliższemu chętnemu przodkowi**, a dopiero gdy takiego nie ma — nikomu.
// Gaszenie na amen wyglądało niewinnie i psuło modalność: dialog połyka klawisze **jako ognisko**,
// więc bez ogniska klawisz szedł prosto do korzenia, a zamknięte okno przestawało być zamknięte.
//
// Woła to każdy widżet, który gaśnie pod ogniskiem — przycisk, pole, wiersz listy, pozycja menu,
// pole wyboru, grupa opcji i wybór. Stało to w każdym z nich **słowo w słowo**, razem z tym
// komentarzem; siedem kopii jednej reguły to siedem miejsc, w których może się rozjechać.
void Node::unfocus() {
    if (!focused()) return;

    Root* top = root();

    if (top == nullptr) return;

    for (Node* at = up_; at != nullptr; at = at->up_)
        if (at->focusable()) {
            at->focus();

            return;
        }

    top->focus(nullptr);
}

// Numer warstwy mówi sam węzeł. Wyżej niż zero znaczy „leżę na rodzeństwie", ale nigdy
// „wychodzę poza rodzica": tej granicy nie przekracza nic. Kolejność malowania rozstrzyga
// pojemnik, który i tak decyduje, gdzie dzieci stoją — maluje najpierw niższe warstwy, potem
// wyższe. Kolumna i wiersz kładą dzieci obok siebie, więc nie mają czego porządkować; robią to
// pojemniki, które kładą dzieci **na sobie**.
int Node::layer() const { return 0; }

void Children::add(std::shared_ptr<Node> given) {
    if (!given) throw std::logic_error("talcite::ui::Node: nie ma czego dopiąć");

    if (given->up_ != nullptr) throw std::logic_error("talcite::ui::Node: węzeł ma już rodzica");

    if (static_cast<int>(inside_.size()) >= whose_->takes())
        throw std::logic_error("talcite::ui::Node: węzeł nie przyjmuje więcej dzieci");

    if (!whose_->accepts(*given))
        throw std::logic_error("talcite::ui::Node: węzeł nie tego rodzaju");

    // Cykl w tym drzewie znaczy, że wędrówka zdarzenia kręci się w kółko bez końca. Sprawdzenie
    // kosztuje przejście o głębokość drzewa, czyli nic, a łapie też dopięcie węzła do siebie.
    for (const Node* at = whose_; at != nullptr; at = at->up_)
        if (at == given.get())
            throw std::logic_error("talcite::ui::Node: węzeł nie może być własnym przodkiem");

    given->up_ = whose_;

    inside_.push_back(std::move(given));
}

// **Albo wszystkie, albo żadne.** Przy błędzie w połowie zdejmujemy to, co już weszło, i rzucamy
// dalej — inaczej wołający zostawałby z drzewem złożonym do połowy i bez wiedzy, do której.
void Children::add(std::vector<std::shared_ptr<Node>> given) {
    const std::size_t was = inside_.size();

    try {
        for (std::shared_ptr<Node>& one : given) add(std::move(one));
    } catch (...) {
        while (inside_.size() > was) (void)remove(inside_.back().get());

        throw;
    }
}

// Oddaje to, co zdjęło, więc przeniesienie węzła pod innego rodzica jest jednym wyrażeniem.
// Cudzy węzeł daje pustkę, a nie wyjątek — kto tego nie sprawdzi, dostanie i tak wyraźny błąd
// przy najbliższym add, bo dopięcie pustki jest zabronione.
std::shared_ptr<Node> Children::remove(Node* one) {
    const auto at = std::find_if(inside_.begin(), inside_.end(),
                                 [one](const std::shared_ptr<Node>& mine) {
                                     return mine.get() == one;
                                 });

    if (at == inside_.end()) return nullptr;

    std::shared_ptr<Node> made = std::move(*at);

    inside_.erase(at);

    made->up_ = nullptr;

    // Korzeń zdejmuje ognisko i najechanie, a potem trzyma węzeł przy życiu do końca doręczania —
    // bo zdejmowanie bywa wołane ze środka zdarzenia, które właśnie przez ten węzeł idzie.
    if (Root* top = whose_->root()) {
        top->losing(made.get());
        top->linger(made);
    }

    return made;
}

// Zdejmujemy od końca, bo wtedy wektor nie przesuwa nikogo, a korzeń dostaje węzły w kolejności
// odwrotnej do dopinania — czyli tak, jak przy niszczeniu drzewa.
void Children::clear() {
    Root* top = whose_->root();

    while (!inside_.empty()) {
        std::shared_ptr<Node> made = std::move(inside_.back());

        inside_.pop_back();

        made->up_ = nullptr;

        if (top != nullptr) {
            top->losing(made.get());
            top->linger(made);
        }
    }
}

bool Children::contains(const Node* one) const {
    return std::any_of(inside_.begin(), inside_.end(),
                       [one](const std::shared_ptr<Node>& mine) { return mine.get() == one; });
}

// Korzeń liczymy przejściem w górę, a nie zapamiętujemy. Głębokość drzewa jest rzędu pięciu,
// a zapamiętane pole trzeba by pilnować przy każdym dopięciu i odpięciu — i wisiałoby po
// śmierci korzenia. Przejście jest przy okazji bardziej prawdomówne: węzeł odpięty od drzewa
// nie ma korzenia i to właśnie oddaje.
Root* Node::root() const {
    const Node* at = this;

    while (at->up_ != nullptr) at = at->up_;

    return const_cast<Root*>(dynamic_cast<const Root*>(at));
}

// **Jeden znacznik, jedno znaczenie.** Ognisko bierze tylko ten, kto się do tego przyznaje — i po
// tych samych węzłach chodzi Tab. Gdyby `focus()` działało na czymkolwiek, a znacznik rządził samą
// wędrówką, ognisko mogłoby wylądować tam, gdzie Tab nigdy nie dojdzie: dwa pojęcia pod jedną
// nazwą. Domyślnie fałsz, bo węzeł układający albo malujący nie ma czego robić z klawiaturą.
bool Node::focusable() const { return false; }

// Skórkę trzyma korzeń, a węzeł dochodzi do niej tym samym przejściem w górę, którym dochodzi do
// wszystkiego innego. Bez korzenia — węzeł jeszcze niedopięty albo poddrzewo odpięte — zostaje
// wbudowana, więc **pytanie o wygląd nigdy nie zawodzi**.
//
// Wołać raz na `paint`, do zmiennej lokalnej: każde wywołanie to przejście w górę i `dynamic_cast`
// na szczycie.
const Skin& Node::skin() const {
    const Root* top = root();

    return top != nullptr ? top->skin() : Skin::plain();
}

bool Node::focused() const {
    const Root* top = root();

    return top != nullptr && top->focus() == this;
}

// Wirtualne, bo są widżety, które ogniska nie biorą **same**, tylko oddają je swojemu wnętrzu:
// lista stawia je na bieżącej pozycji, a nie na sobie. Wędrówka Tabem trzyma zwykłe `Node*`
// i woła `focus()` na przystanku, więc bez wirtualności ognisko lądowałoby na widżecie zamiast
// tam, gdzie ma być. `Root::focus(Node*)` zostaje niewirtualne: ono ustawia, a to tylko wskazuje.
void Node::focus() {
    Root* top = root();

    if (top == nullptr)
        throw std::logic_error("talcite::ui::Node: węzeł poza drzewem nie może wziąć ogniska");

    top->focus(this);
}

// Niewirtualne z tego samego powodu, dla którego niewirtualny jest trigger: zapisanie
// przydzielonego prostokąta musi się zdarzyć zawsze, bo od niego zależy trafianie myszą, a nie
// widać po widżecie, że autor o tym zapomniał. Widżety nadpisują paint.
//
// Widoczność podaje się we współrzędnych **własnych** węzła, więc „widać wszystko" to
// {{0, 0}, where.size}, a nie where — ten jest wyrażony w układzie rodzica.
View Node::render(const Area& where) { return render(where, {{0, 0}, where.size}); }

// Węzeł, z którego nic nie widać, nie rysuje nic — i to jest cała oszczędność przy przewijaniu.
// Prostokąt zapisujemy mimo to, bo od niego zależy trafianie myszą, a węzeł niewidoczny w tej
// klatce nie może pamiętać, gdzie stał w poprzedniej.
View Node::render(const Area& where, const Area& seen) {
    area_ = where;

    if (seen.size.wide <= 0 || seen.size.high <= 0) return View{{0, 0}};

    return paint(where.size, seen);
}

// **Kursor nie jest własnością kartki.** Jest jeden na całą klatkę i należy do korzenia, tak samo
// jak skórka — a w `View` siedział tylko dlatego, że dziecko robi własny odklejony bufor i kursor
// musiał przyjechać w górę razem z wklejeniem. To kosztowało przenoszenie go w każdym `within`,
// pole w każdej kartce i przeliczanie współrzędnych w miejscu, które o współrzędnych nie wie nic.
//
// Widżet nie stawia więc kursora nigdzie — **mówi, gdzie go chce**, we własnych współrzędnych.
// Pytanie zadaje korzeń i tylko ognisku, bo migający daszek jest dokładnie jeden. Domyślnie brak:
// przytłaczająca większość widżetów nie ma czego pokazywać.
std::optional<Spot> Node::caret() const { return {}; }

// „Obudź mnie najpóźniej za tyle." Pusto znaczy **nigdy** — i to jest odpowiedź przytłaczającej
// większości węzłów, bo obraz zmienia się zwykle od zdarzenia, nie od zegara.
//
// Umowa jest jednostronna i trzeba jej pilnować po stronie węzła: **czas ma być dodatni**. Węzeł
// oddający wciąż zero każe pętli kręcić się bez końca, a pętla nie ma jak odróżnić tego od
// prawdziwej potrzeby. Węzeł liczący klatkę z zegara robi to sam z siebie — oddaje resztę bieżącej
// klatki, która zerem nie bywa.
std::optional<std::chrono::milliseconds> Node::due() const { return {}; }

// Zwykły węzeł chce tyle, co jego jedyne dziecko, i oddaje mu cały swój prostokąt. Układanie
// kilkorga jest sprawą Row i Column.
Size Node::wants(Size room) const {
    if (inside_.empty()) return {};

    return inside_.front()->wants(room);
}

View Node::paint(Size given, Area seen) {
    View made{seen.size};

    // **Przezroczysty tam, gdzie nic nie rysuje.** Świeża kartka to własne spacje, więc każdy węzeł
    // zamalowywał cały przydzielony prostokąt — także prześwit obok krótszego napisu i wiersz pod
    // ostatnim dzieckiem — i tło rodzica ginęło pod stylem domyślnym. Regułą jest: **maluj to,
    // czym jesteś, a nie prostokąt, który dostałeś.** `Dialog` i `Menu` robiły tak od zawsze; tu
    // jest reszta biblioteki doprowadzona do tej samej zasady.

    made.bare({{0, 0}, seen.size});

    if (inside_.empty()) return made;

    const Area where{{0, 0}, given};
    const Area part = Area::crossed(seen, where);

    made.within(inside_.front()->render(where, part - where.at), part.at - seen.at);

    return made;
}

}

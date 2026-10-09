#include "talcite/page.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"
#include "spool.hpp"

namespace talcite::ui {

namespace {

// Ile wierszy dokumentu bierzemy ze zwoju za jednym razem. Przy zawijaniu jeden wiersz dokumentu
// daje kilka ekranowych, więc pytanie o dokładnie tyle, ile ma wysokości okno, prawie zawsze
// pytałoby o za dużo.
constexpr std::size_t batch = 32;

// Ile wierszy przeskakuje kółko. Ta sama liczba co w przewijakach.
constexpr int stepping = 3;

// Kawałek wiersza dokumentu zajmujący jeden wiersz ekranu: gotowy napis i mapa **kolumna na
// bajt**, po której nakładają się zaznaczenia.
struct Cut {
    std::string text;
    std::vector<std::size_t> bytes;
    std::size_t line = 0;

    // Bajt **tuż za** ostatnim znakiem kawałka. Mapa kolumn mówi, gdzie który znak się zaczyna,
    // ale nie gdzie się kończy — a kliknięcie za końcem wiersza musi trafić właśnie tam.
    std::size_t upto = 0;
};

// **Cięcie wiersza dokumentu na wiersze ekranu, bez tego, co z kawałków powstaje.** Chodzik mówi
// odbiorcy tylko trzy rzeczy: znak zajmuje tyle kolumn, tabulator rozwija się w tyle spacji,
// kawałek kończy się przed tym bajtem. Z tych samych wywołań `parted` buduje kawałki do rysowania,
// a `counted` tylko je liczy. Dawniej liczenie budowało pełne kawałki — napis i mapę kolumn — żeby
// z nich wziąć samą długość wektora, a przewijanie liczy kawałki przy każdym kroku.
//
// Bez zawijania wychodzi jeden kawałek — to, co się mieści — a reszta po prostu nie istnieje dla
// widoku.
//
// Tniemy po **kolumnach**, nie po bajtach ani znakach: znak chiński zajmuje dwie kratki, a znak
// łączący zero. Inaczej wiersz z jednym takim znakiem wyjeżdżałby poza kartkę albo urywał się
// o kratkę za wcześnie.
template <typename Taker>
void walked(const std::string& what, int wide, bool wrap, Taker& taker) {
    int much = 0;

    // Czy w bieżącym kawałku coś stoi — także znak zerowej szerokości, który kolumny nie zajmuje.
    bool any = false;
    bool cut = false;

    // Dokąd doszło cięcie. Bez zawijania wiersz urywa się na krawędzi i reszty bajtów po prostu
    // nie widać, więc ogon nie może się przyznawać do całej długości wiersza.
    std::size_t ended = what.size();

    const auto flushed = [&taker, &much, &any, &cut](std::size_t upto) {
        taker.flushed(upto);

        much = 0;
        any = false;
        cut = true;
    };

    for (std::size_t at = 0; at < what.size();) {
        const std::size_t was = at;
        const char32_t sign = utf8::decode(what, at);

        if (sign == U'\t') {
            const int upto = std::min(((much / tabbing) + 1) * tabbing, wide);

            taker.spaced(was, upto - much);

            much = upto;
            any = true;
        } else {
            const int room = std::max(width(sign), 0);

            if (much + room > wide) {
                if (!wrap) {
                    ended = was;

                    break;
                }

                // Kawałek kończy się **przed** tym znakiem, bo znak się w nim nie zmieścił.
                flushed(was);
            }

            taker.lettered(sign, was, room);

            much += room;
            any = true;
        }

        if (much < wide) continue;

        if (!wrap) {
            ended = at;

            break;
        }

        flushed(at);
    }

    // Ogon dopisujemy tylko wtedy, gdy coś w nim jest. Wiersz o długości dokładnie równej
    // wielokrotności szerokości kończy się na granicy kawałka i pusty ogon po nim byłby pustym
    // wierszem na ekranie, którego w dokumencie nie ma. Pusty wiersz dokumentu to co innego —
    // on musi dać jeden kawałek, i stąd drugi warunek.
    if (any || !cut) taker.flushed(ended);
}

struct Builder {
    std::vector<Cut> made;
    Cut one;

    void spaced(std::size_t was, int much) {
        for (int step = 0; step < much; ++step) {
            one.text.push_back(' ');
            one.bytes.push_back(was);
        }
    }

    // Każda zajęta kolumna wskazuje **bajt znaku, który ją zajął** — także druga kolumna znaku
    // podwójnego i każda kolumna rozwiniętego tabulatora. Bez tego nie dałoby się powiedzieć, czy
    // kratka mieści się w zakresie zaznaczenia.
    void lettered(char32_t sign, std::size_t was, int room) {
        utf8::encode(sign, one.text);

        for (int step = 0; step < room; ++step) one.bytes.push_back(was);
    }

    void flushed(std::size_t upto) {
        one.upto = upto;

        made.push_back(std::move(one));
        one = Cut{};
    }
};

struct Counter {
    int made = 0;

    void spaced(std::size_t, int) {}

    void lettered(char32_t, std::size_t, int) {}

    void flushed(std::size_t) { ++made; }
};

std::vector<Cut> parted(const std::string& what, int wide, bool wrap) {
    Builder builder;

    walked(what, wide, wrap, builder);

    return std::move(builder.made);
}

int counted(const std::string& what, int wide) {
    Counter counter;

    walked(what, wide, true, counter);

    return counter.made;
}

}

struct Page::Kept {
    Roll roll;
    std::optional<std::size_t> told;

    // Dokument bajtowy. Pusty przy źródle wierszowym — wtedy bajtów nie znamy wcale.
    std::shared_ptr<Spool> spool;

    // Zamiary czekające na okno szpuli: „dojdź do końca" i „stań na wierszu z tym bajtem". Żyją do
    // skutku albo do nowego dokumentu.
    bool ending = false;
    std::optional<std::size_t> sought;

    std::size_t top = 0;
    int part = 0;

    std::vector<Cut> shown;
    std::size_t walked = 0;

    // Czy za ostatnim pokazanym kawałkiem jest jeszcze treść — reszta wiersza albo następny wiersz;
    // wiersz, którego źródło jeszcze nie ma, liczy się jako treść.
    bool more = false;

    std::optional<std::size_t> which;

    // Wiersz, który ma być widoczny, a jeszcze nie wiadomo gdzie — bo strona nie zna wysokości.
    std::optional<std::size_t> wish;

    Size sized;
    bool wrapped = true;
    bool fresh = true;

    ScrollShape shape;

    std::string scrollbar_role = "talcite.scroll";
    std::string chosen = "talcite.viewer.mark.focus";

    // Pasek z ostatniej klatki i jego jednostka: wiersze albo bajty (`gauged`).
    Grip grip;
    bool bytes = false;
};

Page::Page() : kept_(std::make_unique<Kept>()) { role("talcite.viewer"); }

Page::Page(ScrollShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.viewer");

    kept_->shape = shape;
}

Page::~Page() = default;

// **Dokument trzyma strona, nie podklasa.** Są dwa rodzaje: bajtowy — szpula, z której strona zna
// wiersze, bajty i długość — i czysto wierszowy, który bajtów nie zna wcale i wtedy `at()`
// i `whole()` są puste, bo „nie ma o czym mówić", a nie „zepsute". Podklasa tylko podaje dokument:
// `Viewer` bajty albo źródło bajtowe, `Editor` szpulę nad kołdrą.
//
// Dawniej strona pytała o dokument przez pięć haków wirtualnych, a każda podklasa trzymała własną
// szpulę. Wyszły z tego dwa błędy: `Editor` przez `using Page::hold` przyjmował zwój, którego nie
// umiał edytować — ekran pokazywał nowy dokument, a pisanie zmieniało stary — a `Viewer` po
// przejściu z bajtów na zwój odpowiadał liczbą wierszy starej szpuli. Dziś nowy dokument zastępuje
// poprzedni w całości, bo jest tylko jedno miejsce, w którym dokument stoi.
//
// Szpula budzi stronę po każdym oknie: klatka się unieważnia, bo strona pamięta, co pokazała,
// a zamiary czekające na okno — koniec i skok po bajcie — dokańczają się same. Uchwyt trzyma stronę
// gołym wskaźnikiem, bo szpulę trzyma strona; dla szpuli edytora nie woła się nigdy, bo ta czyta
// z pamięci i na nic nie czeka.
void Page::hold(std::shared_ptr<Spool> given) {
    Kept& kept = *kept_;

    hold([given](std::size_t which, std::size_t most) { return given->lines(which, most); }, 0);

    kept.spool = std::move(given);

    kept.spool->awoken([this] {
        Kept& now = *kept_;

        invalidate();

        if (now.sought.has_value()) {
            const std::size_t want = *now.sought;

            now.sought.reset();

            at(want);
        }

        if (now.ending && now.spool->counted().has_value()) {
            now.ending = false;

            end();
        }
    });
}

// **Zwój to wszystko, czego strona potrzebuje do rysowania.** Skąd się bierze, jej nie obchodzi.
// Podany wprost jest dokumentem czysto wierszowym, więc szpulę poprzedniego dokumentu i jej
// zamiary zdejmujemy; `hold` ze szpulą woła to samo i dopiero potem kładzie swoją.
void Page::hold(Roll given, std::size_t lines) {
    Kept& kept = *kept_;

    kept.spool.reset();
    kept.ending = false;
    kept.sought.reset();

    kept.roll = std::move(given);
    kept.told = lines;

    kept.top = 0;
    kept.part = 0;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

const std::shared_ptr<Spool>& Page::spool() const { return kept_->spool; }

bool Page::held() const { return static_cast<bool>(kept_->roll); }

std::optional<std::size_t> Page::first() const {
    if (!held()) return {};

    return kept_->top;
}

// Liczba wierszy: szpula wie ją dopiero po dojściu do końca, zwój mówi ją od razu.
std::optional<std::size_t> Page::lines() const {
    const Kept& kept = *kept_;

    if (kept.spool) return kept.spool->counted();

    return kept.told;
}

// Bajt, od którego zaczyna się pierwszy widoczny wiersz — to, co Midnight Commander pokazuje
// w podglądzie jako położenie w pliku.
std::optional<std::size_t> Page::at() const {
    const Kept& kept = *kept_;

    const std::optional<std::size_t> which = first();

    if (!kept.spool || !which.has_value()) return {};

    return kept.spool->at(*which);
}

std::optional<std::size_t> Page::whole() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return {};

    return kept.spool->whole();
}

// Skok po bajcie. Bez szpuli nie ma o czym mówić, bo zwój bajtów nie zna. Gdy szpula nie doszła do
// bajtu, bo czeka na okno, skok zostaje zamiarem i dokańcza się, kiedy okno dojdzie.
void Page::at(std::size_t given) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    const std::size_t which = kept.spool->found(given);

    if (kept.spool->stalled()) {
        kept.sought = given;

        return;
    }

    first(which);
}

void Page::first(std::size_t given) {
    Kept& kept = *kept_;

    if (!held()) return;

    const std::optional<std::size_t> many = lines();
    const std::size_t want = many.has_value() && *many > 0 && given >= *many ? *many - 1 : given;

    if (want == kept.top && kept.part == 0) return;

    kept.top = want;
    kept.part = 0;
    kept.fresh = true;

    (void)move_by(0);

    (void)trigger(ScrollEvent{});
}

// Bez indeksu koniec znajduje się tylko jedną drogą: przechodząc dokument. Szpula policzy wiersze
// przy okazji i od tej chwili `lines()` przestaje być puste. Przy źródle asynchronicznym przejście
// idzie okno po oknie i koniec dokańcza się sam, gdy liczba jest już znana.
void Page::end() {
    Kept& kept = *kept_;

    if (!held()) return;

    if (kept.spool && !kept.spool->counted().has_value()) {
        kept.spool->counting();

        if (!kept.spool->counted().has_value()) kept.ending = true;
    }

    const std::optional<std::size_t> many = lines();

    if (!many.has_value() || *many == 0) return;

    kept.top = *many - 1;
    kept.part = std::max(pieces(kept.top) - 1, 0);
    kept.fresh = true;

    slide(-std::max(kept.sized.high - 1, 0));

    (void)trigger(ScrollEvent{});
}

// Ile wierszy ekranu zajmuje ten wiersz dokumentu. Bez zawijania zawsze jeden; przy zawijaniu tyle,
// ile wyjdzie z pocięcia po kolumnach.
int Page::pieces(std::size_t which) const {
    const Kept& kept = *kept_;

    if (!kept.wrapped || kept.sized.wide <= 0 || !kept.roll) return 1;

    const std::optional<std::vector<std::string>> got = kept.roll(which, 1);

    if (!got.has_value() || got->empty()) return 1;

    return counted(got->front(), kept.sized.wide);
}

// Krok liczy się w wierszach **ekranu**, nie dokumentu — bo tyle przesuwa się obraz. Przy zawijaniu
// jeden wiersz dokumentu to kilka kroków.
// Przesunięcie **bez zawiadamiania**. Skok do zaznaczenia przewija i zmienia bieżący znacznik
// jednym ruchem, więc musi wysłać jedno zdarzenie, a nie dwa.
//
// **Ekran nie wychodzi poza koniec.** Krok w dół zatrzymuje się, gdy ostatni wiersz dokumentu
// stoi na dole ekranu — a nie, jak przedtem, gdy stoi na górze, z pustką pod nim. Tamto pozwalało
// przewinąć pomoc pod F1 „grubo ponad jej rozmiar": trzydzieści kilka wierszy w oknie na
// czterdzieści dało się zsunąć do jednego. Przycięcie idzie po wierszach **ekranu** (z zawijaniem
// wiersz dokumentu to kilka), liczy do przodu tylko tyle, ile ekran ma wysokości, i działa też dla
// zera kroków — tak wraca do porządku skok `first()` w ostatnią stronę. Przed układem wysokości
// nie ma i nie ma czego przycinać.
//
// **Zwój, który jeszcze nie ma wiersza, nie jest końcem.** Pustka zamiast wektora znaczy „czekam
// na okno": krok w dół zatrzymuje się na tym, co jest, a przycięcie do końca nie rusza — bo nie
// wiadomo, gdzie koniec jest. Gdy okno dojdzie, strona maluje się od nowa i następny krok idzie.
//
// **Każdy wiersz dokumentu oglądamy raz na ruch.** `looked` pamięta, co o wierszu wiadomo: `-1`
// — czeka na okno, `0` — za końcem, więcej — tyle kawałków. Dawniej krok w obrębie jednego wiersza
// ciął go od nowa, a przycięcie do końca przechodziło cały ekran od nowa po każdym cofnięciu
// o wiersz ekranu: strona w dół przy końcu dokumentu to było wysokość do kwadratu wierszy
// pobranych i pociętych. Przycięcie liczy teraz ekran raz: cofnięcie o wiersz ekranu, gdy
// przejście doszło do końca dokumentu, dokłada dokładnie jeden wiersz.
bool Page::move_by(int lines) {
    Kept& kept = *kept_;

    if (!held()) return false;

    const std::size_t was = kept.top;
    const int part = kept.part;

    std::unordered_map<std::size_t, int> seen;

    const auto looked = [&kept, &seen](std::size_t which) {
        if (const auto known = seen.find(which); known != seen.end()) return known->second;

        const std::optional<std::vector<std::string>> got = kept.roll(which, 1);

        int made = 1;

        if (!got.has_value())
            made = -1;
        else if (got->empty())
            made = 0;
        else if (kept.wrapped && kept.sized.wide > 0)
            made = counted(got->front(), kept.sized.wide);

        seen.emplace(which, made);

        return made;
    };

    const auto pieced = [&looked](std::size_t which) { return std::max(looked(which), 1); };

    for (int one = 0; one < lines; ++one) {
        if (kept.part + 1 < pieced(kept.top)) {
            ++kept.part;

            continue;
        }

        if (looked(kept.top + 1) <= 0) break;

        ++kept.top;
        kept.part = 0;
    }

    for (int one = 0; one > lines; --one) {
        if (kept.part > 0) {
            --kept.part;

            continue;
        }

        if (kept.top == 0) break;

        --kept.top;
        kept.part = pieced(kept.top) - 1;
    }

    if (const int high = kept.sized.high; high > 0) {
        int rows = -kept.part;

        for (std::size_t which = kept.top; rows <= high; ++which) {
            const int got = looked(which);

            if (got < 0) rows = high;

            if (got <= 0) break;

            rows += got;
        }

        while ((kept.top > 0 || kept.part > 0) && rows < high) {
            if (kept.part > 0) {
                --kept.part;
            } else {
                --kept.top;

                if (looked(kept.top) < 0) break;

                kept.part = pieced(kept.top) - 1;
            }

            ++rows;
        }
    }

    if (kept.top == was && kept.part == part) return false;

    kept.fresh = true;

    return true;
}

// Zawiadomienie (`ScrollEvent`) idzie zwykłą drogą zdarzeń, więc bąbelkuje w górę do przodków.
// Górny pasek jest **rodzeństwem**, nie przodkiem, więc nasłuch trzeba zawiesić wyżej — na
// pojemniku albo korzeniu.
bool Page::slide(int lines) {
    if (!move_by(lines)) return false;

    (void)trigger(ScrollEvent{});

    return true;
}

void Page::page(int pages) { slide(pages * std::max(kept_->sized.high, 1)); }

bool Page::wrapped() const { return kept_->wrapped; }

// Ten sam bajt znaczy co innego w obu trybach, więc wracamy na początek wiersza, w którym stoimy.
// Bez tego przełączenie przerzucałoby ekran w miejsce, którego nikt nie prosił.
void Page::wrapped(bool given) {
    Kept& kept = *kept_;

    if (kept.wrapped == given) return;

    kept.wrapped = given;
    kept.part = 0;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

bool Page::scrollbar() const { return kept_->shape.scrollbar; }

void Page::scrollbar(bool given) {
    if (kept_->shape.scrollbar == given) return;

    kept_->shape.scrollbar = given;
    kept_->fresh = true;
}

// **Znacznik to zakres i rola, nic więcej** — dokładnie jak `Decoration.mark` w CodeMirror.
// Nie jest zakładką, zaznaczeniem ani wynikiem szukania; każde z nich może go użyć, ale żadne
// z nich nim nie jest. Zakres liczy się w bajtach **wiersza**, bo bajt dokumentu istnieje tylko
// przy źródle bajtowym.
//
// Zbiór trzymamy uporządkowany po wierszu, a przy równych po początku — inaczej `next` nie miałby
// jak powiedzieć, który jest następny.
namespace {

bool ahead(const Mark& one, const Mark& two) {
    return one.line != two.line ? one.line < two.line : one.from < two.from;
}

}


std::size_t Page::shown() const { return kept_->walked; }

// Przewija **najmniej, jak się da**: znacznik już widoczny nie rusza obrazu, wyżej ląduje na górze,
// niżej na dole. Zdarzenie idzie jedno, choćby ruch był złożony z dwóch kroków.
bool Page::reach(std::size_t which) {
    MarkedNode::which(which);

    (void)reveal(marks()[which].line);

    (void)trigger(ScrollEvent{});

    return true;
}

// **Przewija najmniej, jak się da**: wiersz już widoczny nie rusza obrazu, wyżej ląduje na górze,
// niżej na dole. Nie zawiadamia — robi to wołający, żeby złożony ruch dał jedno zdarzenie.
bool Page::reveal(std::size_t line) {
    Kept& kept = *kept_;

    // Zanim strona dostanie przydział, **nie ma po czym poznać**, czy wiersz i tak widać: wysokość
    // jest zerowa, więc każdy wiersz poza pierwszym wyglądałby na schowany i obraz skakałby bez
    // powodu. Odkładamy pytanie do złożenia obrazu, kiedy wysokość jest już znana.
    if (kept.sized.high <= 0) {
        kept.wish = line;
        kept.fresh = true;

        return true;
    }

    kept.wish.reset();

    const std::size_t last = kept.walked > 0 ? kept.top + kept.walked - 1 : kept.top;

    if (line >= kept.top && line <= last) return false;

    const bool above = line < kept.top;

    kept.top = line;
    kept.part = 0;
    kept.fresh = true;

    if (!above) (void)move_by(-std::max(kept.sized.high - 1, 0));

    return true;
}

void Page::invalidate() { kept_->fresh = true; }

std::size_t Page::topmost() const { return kept_->top; }

// Kratka, na której stoi ten bajt tego wiersza — albo pusto, gdy wiersz jest poza ekranem albo
// bajt wypadł poza to, co się zmieściło. Stąd bierze się miejsce kursora w edytorze.
std::optional<Spot> Page::spot_of(Place where) const {
    const Kept& kept = *kept_;
    const std::size_t line = where.line;
    const std::size_t byte = where.byte;

    for (std::size_t one = 0; one < kept.shown.size(); ++one) {
        const Cut& piece = kept.shown[one];

        if (piece.line != line) continue;

        for (std::size_t col = 0; col < piece.bytes.size(); ++col)
            if (piece.bytes[col] == byte)
                return Spot{static_cast<int>(col), static_cast<int>(one)};

        // Koniec wiersza: kursor stoi **za** ostatnim znakiem, więc żadna kolumna go nie niesie.
        if (!piece.bytes.empty() && byte > piece.bytes.back())
            return Spot{static_cast<int>(piece.bytes.size()), static_cast<int>(one)};

        if (piece.bytes.empty()) return Spot{0, static_cast<int>(one)};
    }

    return {};
}

// Odwrotność `spot_of`: z kratki na wiersz dokumentu i bajt w nim. Rząd ekranu **nie jest** numerem
// wiersza — przy zawijaniu jeden wiersz zajmuje ich kilka — a kolumna nie jest bajtem, bo tabulator
// zajmuje osiem kratek, a znak chiński dwie. Obie mapy policzyło już cięcie na kawałki, więc tutaj
// zostaje samo odczytanie.
std::optional<Place> Page::place_of(Spot where) const {
    const Kept& kept = *kept_;

    if (where.y < 0 || static_cast<std::size_t>(where.y) >= kept.shown.size()) return {};

    const Cut& piece = kept.shown[static_cast<std::size_t>(where.y)];

    const std::size_t col = static_cast<std::size_t>(std::max(where.x, 0));

    // Za ostatnim znakiem kawałka: kursor staje **tuż za** nim, czyli tam, gdzie zaczyna się
    // następny kawałek albo gdzie kończy się wiersz.
    if (col >= piece.bytes.size()) return Place{piece.line, piece.upto};

    return Place{piece.line, piece.bytes[col]};
}

// **Bieżący znacznik maluje się inną rolą niż pozostałe.** Podgląd wie, na którym stoi, więc to on
// tę rolę podstawia — aplikacja nie musi podawać znaczników od nowa tylko po to, żeby jeden z nich
// wyglądał inaczej. Nazwę roli można podmienić, tak jak własną rolę widżetu.
const std::string& Page::chosen() const { return kept_->chosen; }

void Page::chosen(std::string given) { kept_->chosen = std::move(given); }

const std::string& Page::scrollbar_role() const { return kept_->scrollbar_role; }

void Page::scrollbar_role(std::string given) { kept_->scrollbar_role = std::move(given); }

bool Page::focusable() const { return true; }

// Podgląd nie bierze dzieci — sam jest treścią.
int Page::capacity() const { return 0; }

Size Page::wants(Size room) const { return room; }

// Przeliczamy **tylko wtedy, gdy zmienił się przydział albo położenie**. Bez tego każde odświeżenie
// klatki przechodziłoby dokument od nowa, a przy animowanym sąsiedztwie dzieje się to dziesięć razy
// na sekundę.
void Page::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();
    kept.more = false;

    if (!kept.roll || given.wide <= 0 || given.high <= 0) return;

    if (kept.wish.has_value()) {
        (void)reveal(*kept.wish);

        kept.fresh = false;
    }

    // Odstęp kotwic szpuli bierze się z wysokości okna — szpula o widoku nic nie wie.
    if (kept.spool) kept.spool->screen(given.high);

    const std::size_t want = static_cast<std::size_t>(given.high);

    std::size_t which = kept.top;
    int skip = kept.part;

    kept.walked = 0;

    while (kept.shown.size() < want) {
        const std::optional<std::vector<std::string>> got = kept.roll(which, batch);

        if (!got.has_value() || got->empty()) break;

        for (const std::string& one : *got) {
            for (Cut& piece : parted(one, given.wide, kept.wrapped)) {
                if (skip > 0) {
                    --skip;

                    continue;
                }

                if (kept.shown.size() >= want) {
                    kept.more = true;

                    break;
                }

                piece.line = which;

                kept.shown.push_back(std::move(piece));
            }

            ++which;

            if (kept.shown.size() >= want) break;
        }
    }

    kept.walked = which - kept.top;

    if (!kept.more && kept.shown.size() >= want) {
        const std::optional<std::vector<std::string>> next = kept.roll(which, 1);

        kept.more = !next.has_value() || !next->empty();
    }
}

// Czym mierzyć pasek: **wierszami, gdy znamy ich liczbę, a bajtami, gdy nie**. Przy dokumencie
// w pamięci i przy źródle wierszowym wiersze są znane od razu i to one wychodzą naturalniej.
// Przy strumieniu liczba wierszy jest nieznana do pierwszego `End`, więc pasek chodzi wtedy po
// bajtach — dokładnych od otwarcia. Okno w bajtach to rozpiętość tego, co właśnie stoi na ekranie,
// a `spread` i tak przez te wiersze przeszedł.
bool Page::gauged(std::size_t& shift, std::size_t& whole, std::size_t& window) const {
    const Kept& kept = *kept_;

    const std::optional<std::size_t> many = lines();

    if (many.has_value()) {
        if (*many == 0) return false;

        shift = kept.top;
        whole = *many;
        window = static_cast<std::size_t>(std::max(kept.sized.high, 1));

        return true;
    }

    const std::optional<std::size_t> from = at();
    const std::optional<std::size_t> much = this->whole();

    if (!from.has_value() || !much.has_value() || *much == 0) return false;

    // Okno w bajtach to rozpiętość tego, co właśnie stoi na ekranie: od początku pierwszego
    // widocznego wiersza do początku wiersza **za** ostatnim. Gdy tego drugiego szpula jeszcze nie
    // zna, bierzemy resztę dokumentu.
    const std::optional<std::size_t> upto = kept.spool->at(kept.top + shown());

    shift = *from;
    whole = *much;
    window = upto.has_value() ? *upto - *from : *much - *from;

    return true;
}

namespace {

// Zaznaczenia nakładamy **po** narysowaniu treści, kratka po kratce: literę zostawiamy tę, która
// tam stoi, a styl scalamy przez `Style::merge`. Dzięki temu rola ustawiająca samo tło zostawia
// barwę liter, a rola ustawiająca same litery zostawia tło.
//
// Kratka z literą zerową to dalszy ciąg znaku podwójnego — pomijamy ją, bo pierwsza połowa
// przemalowała już obie.
void painted(View& into, const Cut& piece, Spot at, const Style& look,
             const std::vector<Mark>& marks, std::optional<std::size_t> which,
             const std::string& chosen, const Skin& how) {
    for (std::size_t nth = 0; nth < marks.size(); ++nth) {
        const Mark& one = marks[nth];

        if (one.line != piece.line) continue;

        if (one.upto <= one.from) continue;

        const bool here = which.has_value() && *which == nth;

        const Style over = Style::merge(look, how.style(here ? chosen : one.role));

        for (std::size_t col = 0; col < piece.bytes.size(); ++col) {
            const std::size_t byte = piece.bytes[col];

            if (byte < one.from || byte >= one.upto) continue;

            const Spot spot{at.x + static_cast<int>(col), at.y};
            const Cell said = into.at(spot);

            if (said.letter == 0) continue;

            into.put(spot, said.letter, over);
        }
    }
}

}

View Page::paint(Size given, Area seen) {
    settle();

    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    // Pasek zabiera jedną kolumnę z prawej, więc treść dostaje o tyle mniej miejsca — tak samo,
    // jak w przewijakach.
    int aside = kept_->shape.scrollbar && given.wide > 0 ? 1 : 0;

    spread({given.wide - aside, given.high});

    // Pasek „gdy trzeba" (`needed`): strona, która mieści się cała od pierwszego wiersza, oddaje
    // jego kolumnę treści i układa się jeszcze raz — szerzej zawinięty tekst zajmie wtedy tyle samo
    // wierszy albo mniej, więc dalej się zmieści.
    if (aside != 0 && kept_->shape.needed && kept_->top == 0 && kept_->part == 0 && !kept_->more) {
        aside = 0;

        spread({given.wide, given.high});
    }

    if (kept_->shown.empty() && aside == 0) return made;

    const Style look = skin().style(role());

    made.fill({{0, 0}, seen.size}, look);

    for (std::size_t one = 0; one < kept_->shown.size(); ++one) {
        const Cut& piece = kept_->shown[one];
        const int y = static_cast<int>(one) - seen.at.y;

        made.put({-seen.at.x, y}, piece.text, look);

        if (!marks().empty())
            painted(made, piece, {-seen.at.x, y}, look, marks().all(), which(),
                    kept_->chosen, skin());
    }

    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;

    if (aside != 0 && gauged(shift, whole, window)) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = shift,
                            .whole = whole,
                            .window = window,
                            .skin = skin(),
                            .role = kept_->scrollbar_role};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        kept_->grip.laid(mine);
        kept_->bytes = !lines().has_value();
    } else {
        kept_->grip.laid(std::nullopt);
    }

    return made;
}

// **Oddajemy fałsz dla wszystkiego, czego nie zużyliśmy.** F-klawisze mają dojść do legendy, która
// jest przodkiem, a wędrówka kończy się na pierwszej prawdzie.
bool Page::handle(KeyEvent& what) {
    // Ze sterującym nie bierzemy niczego — Ctrl+Home i Alt+PageDown to z założenia inne skróty niż
    // Home i PageDown. Strona była jedynym widżetem bez tego odsiewu, więc przewijała dokument
    // zamiast przepuścić kombinację do aplikacji.
    if (what.mods().ctrl || what.mods().alt) return false;

    switch (what.key().code()) {
        case Code::up: slide(-1); return true;
        case Code::down: slide(1); return true;
        case Code::pageup: page(-1); return true;
        case Code::pagedown: page(1); return true;
        case Code::home: first(0); return true;
        case Code::end: end(); return true;
        default: return false;
    }
}

bool Page::handle(WheelEvent& what) {
    const std::size_t was = kept_->top;
    const int part = kept_->part;

    slide(-what.steps() * stepping);

    return kept_->top != was || kept_->part != part;
}

bool Page::handle(ClickEvent&) {
    if (root() != nullptr) focus();

    return true;
}

// **Mysz na pasku** (U6 w twa): strzałka — wiersz, tor — strona, suwak — skok tam, gdzie go
// postawiono: na wiersz, gdy pasek liczy wiersze, na bajt, gdy liczy bajty.
bool Page::handle(MouseEvent& what) {
    const Asked asked = kept_->grip.mouse(what, local(what.where()), *this);

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: slide(-1); break;
        case Part::forth: slide(1); break;
        case Part::before: page(-1); break;
        case Part::after: page(1); break;
        case Part::thumb:
            if (asked.to.has_value()) {
                if (kept_->bytes) at(*asked.to);
                else first(*asked.to);
            }
            break;
    }

    return true;
}

}

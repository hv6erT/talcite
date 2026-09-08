#include "talcite/page.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "talcite/bar.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

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

// Wiersz dokumentu pocięty na wiersze ekranu. Bez zawijania oddaje jeden kawałek — to, co się
// mieści — a reszta po prostu nie istnieje dla widoku.
//
// Tniemy po **kolumnach**, nie po bajtach ani znakach: znak chiński zajmuje dwie kratki, a znak
// łączący zero. Inaczej wiersz z jednym takim znakiem wyjeżdżałby poza kartkę albo urywał się
// o kratkę za wcześnie.
std::vector<Cut> parted(const std::string& what, int wide, bool wrap) {
    std::vector<Cut> made;

    Cut one;
    int much = 0;

    // Dokąd doszło cięcie. Bez zawijania wiersz urywa się na krawędzi i reszty bajtów po prostu
    // nie widać, więc ogon nie może się przyznawać do całej długości wiersza.
    std::size_t ended = what.size();

    const auto flushed = [&made, &one, &much](std::size_t upto) {
        one.upto = upto;

        made.push_back(std::move(one));
        one = Cut{};
        much = 0;
    };

    for (std::size_t at = 0; at < what.size();) {
        const std::size_t was = at;
        const char32_t sign = letter(what, at);

        if (sign == U'\t') {
            const int upto = std::min(((much / tabbing) + 1) * tabbing, wide);

            while (much < upto) {
                one.text.push_back(' ');
                one.bytes.push_back(was);
                ++much;
            }
        } else {
            const int room = std::max(widely(sign), 0);

            if (much + room > wide) {
                if (!wrap) {
                    ended = was;

                    break;
                }

                // Kawałek kończy się **przed** tym znakiem, bo znak się w nim nie zmieścił.
                flushed(was);
            }

            spelled(sign, one.text);

            // Każda zajęta kolumna wskazuje **bajt znaku, który ją zajął** — także druga kolumna
            // znaku podwójnego i każda kolumna rozwiniętego tabulatora. Bez tego nie dałoby się
            // powiedzieć, czy kratka mieści się w zakresie zaznaczenia.
            for (int step = 0; step < room; ++step) one.bytes.push_back(was);

            much += room;
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
    if (!one.text.empty() || made.empty()) {
        one.upto = ended;

        made.push_back(std::move(one));
    }

    return made;
}

}

struct Page::Kept {
    Roll roll;
    std::optional<std::size_t> told;

    std::size_t top = 0;
    int part = 0;

    std::vector<Cut> shown;
    std::size_t walked = 0;

    std::optional<std::size_t> which;

    // Wiersz, który ma być widoczny, a jeszcze nie wiadomo gdzie — bo strona nie zna wysokości.
    std::optional<std::size_t> wish;

    Size sized;
    bool wrapped = true;
    bool fresh = true;

    ScrollStyle style;

    std::string bar = "talcite.scroll";
    std::string chosen = "talcite.viewer.mark.focus";
};

Page::Page() : kept_(std::make_unique<Kept>()) { role("talcite.viewer"); }

Page::Page(ScrollStyle style) : kept_(std::make_unique<Kept>()) {
    role("talcite.viewer");

    kept_->style = style;
}

Page::~Page() = default;

// **Szwy dla podklasy.** Strona rysuje, a o dokument pyta — i zadowala się tym, co dostanie.
// `Viewer` odpowiada ze szpuli, `Editor` z tablicy kawałków, a źródło czysto wierszowe nie
// odpowiada wcale i wtedy pusto znaczy „nie ma o czym mówić", a nie „zepsute".
std::optional<std::size_t> Page::at() const { return {}; }

std::optional<std::size_t> Page::below() const { return {}; }

std::optional<std::size_t> Page::whole() const { return {}; }

void Page::counting() {}

void Page::screen(int) {}

// **Zwój to wszystko, czego strona potrzebuje.** Skąd się bierze — z pamięci, z pliku czytanego
// kawałkami, z tablicy kawałków w edytorze — jej nie obchodzi; o bajtach i o liczbie wierszy pyta
// przez szwy, a podklasa odpowiada tyle, ile wie.
void Page::hold(Roll given, std::size_t lines) {
    Kept& kept = *kept_;

    kept.roll = std::move(given);
    kept.told = lines;

    kept.top = 0;
    kept.part = 0;
    kept.fresh = true;

    stirred();
}

bool Page::held() const { return static_cast<bool>(kept_->roll); }

std::optional<std::size_t> Page::first() const {
    if (!held()) return {};

    return kept_->top;
}

std::optional<std::size_t> Page::lines() const { return kept_->told; }

void Page::first(std::size_t given) {
    Kept& kept = *kept_;

    if (!held()) return;

    const std::optional<std::size_t> many = lines();
    const std::size_t want = many.has_value() && *many > 0 && given >= *many ? *many - 1 : given;

    if (want == kept.top && kept.part == 0) return;

    kept.top = want;
    kept.part = 0;
    kept.fresh = true;

    stirred();
}

// Bez indeksu koniec znajduje się tylko jedną drogą: przechodząc dokument. Szpula policzy wiersze
// przy okazji i od tej chwili `lines()` przestaje być puste.
void Page::end() {
    Kept& kept = *kept_;

    if (!held()) return;

    counting();

    const std::optional<std::size_t> many = lines();

    if (!many.has_value() || *many == 0) return;

    kept.top = *many - 1;
    kept.part = std::max(pieces(kept.top) - 1, 0);
    kept.fresh = true;

    slide(-std::max(kept.sized.high - 1, 0));

    stirred();
}

// Ile wierszy ekranu zajmuje ten wiersz dokumentu. Bez zawijania zawsze jeden; przy zawijaniu tyle,
// ile wyjdzie z pocięcia po kolumnach.
int Page::pieces(std::size_t which) const {
    const Kept& kept = *kept_;

    if (!kept.wrapped || kept.sized.wide <= 0 || !kept.roll) return 1;

    const std::vector<std::string> got = kept.roll(which, 1);

    if (got.empty()) return 1;

    return static_cast<int>(parted(got.front(), kept.sized.wide, true).size());
}

// Krok liczy się w wierszach **ekranu**, nie dokumentu — bo tyle przesuwa się obraz. Przy zawijaniu
// jeden wiersz dokumentu to kilka kroków.
// Przesunięcie **bez zawiadamiania**. Skok do zaznaczenia przewija i zmienia bieżący znacznik
// jednym ruchem, więc musi wysłać jedno zdarzenie, a nie dwa.
bool Page::moved(int lines) {
    Kept& kept = *kept_;

    if (!held() || lines == 0) return false;

    const std::size_t was = kept.top;
    const int part = kept.part;

    for (int one = 0; one < lines; ++one) {
        if (kept.part + 1 < pieces(kept.top)) {
            ++kept.part;

            continue;
        }

        if (kept.roll(kept.top + 1, 1).empty()) break;

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
        kept.part = std::max(pieces(kept.top) - 1, 0);
    }

    if (kept.top == was && kept.part == part) return false;

    kept.fresh = true;

    return true;
}

void Page::slide(int lines) {
    if (moved(lines)) stirred();
}

void Page::page(int pages) { slide(pages * std::max(kept_->sized.high, 1)); }

bool Page::wrapping() const { return kept_->wrapped; }

// Ten sam bajt znaczy co innego w obu trybach, więc wracamy na początek wiersza, w którym stoimy.
// Bez tego przełączenie przerzucałoby ekran w miejsce, którego nikt nie prosił.
void Page::wrapping(bool given) {
    Kept& kept = *kept_;

    if (kept.wrapped == given) return;

    kept.wrapped = given;
    kept.part = 0;
    kept.fresh = true;

    stirred();
}

bool Page::progress() const { return kept_->style.progress; }

void Page::progress(bool given) {
    if (kept_->style.progress == given) return;

    kept_->style.progress = given;
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

    (void)show(marks()[which].line);

    stirred();

    return true;
}

// **Przewija najmniej, jak się da**: wiersz już widoczny nie rusza obrazu, wyżej ląduje na górze,
// niżej na dole. Nie zawiadamia — robi to wołający, żeby złożony ruch dał jedno zdarzenie.
bool Page::show(std::size_t line) {
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

    if (!above) (void)moved(-std::max(kept.sized.high - 1, 0));

    return true;
}

void Page::afresh() { kept_->fresh = true; }

std::size_t Page::topmost() const { return kept_->top; }

// Kratka, na której stoi ten bajt tego wiersza — albo pusto, gdy wiersz jest poza ekranem albo
// bajt wypadł poza to, co się zmieściło. Stąd bierze się miejsce kursora w edytorze.
std::optional<Spot> Page::spotted(std::size_t line, std::size_t byte) const {
    const Kept& kept = *kept_;

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

// Odwrotność `spotted`: z kratki na wiersz dokumentu i bajt w nim. Rząd ekranu **nie jest** numerem
// wiersza — przy zawijaniu jeden wiersz zajmuje ich kilka — a kolumna nie jest bajtem, bo tabulator
// zajmuje osiem kratek, a znak chiński dwie. Obie mapy policzyło już cięcie na kawałki, więc tutaj
// zostaje samo odczytanie.
std::optional<Place> Page::struck(Spot where) const {
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

const std::string& Page::bar() const { return kept_->bar; }

void Page::bar(std::string given) { kept_->bar = std::move(given); }

bool Page::focusable() const { return true; }

// Podgląd nie bierze dzieci — sam jest treścią.
int Page::takes() const { return 0; }

Size Page::wants(Size room) const { return room; }

// Zawiadomienie idzie zwykłą drogą zdarzeń, więc bąbelkuje w górę do przodków. Górny pasek jest
// **rodzeństwem**, nie przodkiem, więc nasłuch trzeba zawiesić wyżej — na pojemniku albo korzeniu.
void Page::stirred() {
    ScrollEvent moved;

    (void)trigger(moved);
}

// Przeliczamy **tylko wtedy, gdy zmienił się przydział albo położenie**. Bez tego każde odświeżenie
// klatki przechodziłoby dokument od nowa, a przy animowanym sąsiedztwie dzieje się to dziesięć razy
// na sekundę.
void Page::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();

    if (!kept.roll || given.wide <= 0 || given.high <= 0) return;

    if (kept.wish.has_value()) {
        (void)show(*kept.wish);

        kept.fresh = false;
    }

    screen(given.high);

    const std::size_t want = static_cast<std::size_t>(given.high);

    std::size_t which = kept.top;
    int skip = kept.part;

    kept.walked = 0;

    while (kept.shown.size() < want) {
        const std::vector<std::string> got = kept.roll(which, batch);

        if (got.empty()) break;

        for (const std::string& one : got) {
            for (Cut& piece : parted(one, given.wide, kept.wrapped)) {
                if (skip > 0) {
                    --skip;

                    continue;
                }

                if (kept.shown.size() >= want) break;

                piece.line = which;

                kept.shown.push_back(std::move(piece));
            }

            ++which;

            if (kept.shown.size() >= want) break;
        }
    }

    kept.walked = which - kept.top;
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

    // Okno w bajtach to rozpiętość tego, co właśnie stoi na ekranie. Podklasa wie, gdzie zaczyna
    // się wiersz **za** ostatnim widocznym; gdy nie wie, bierzemy resztę dokumentu.
    const std::optional<std::size_t> upto = below();

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

        const Style over = Style::merge(look, how.get<Style>(here ? chosen : one.role));

        for (std::size_t col = 0; col < piece.bytes.size(); ++col) {
            const std::size_t byte = piece.bytes[col];

            if (byte < one.from || byte >= one.upto) continue;

            const Spot spot{at.x + static_cast<int>(col), at.y};
            const Cell said = into.at(spot);

            if (said.letter == 0) continue;

            into.letter(spot, said.letter, over);
        }
    }
}

}

View Page::paint(Size given, Area seen) {
    settle();

    View made{seen.size};

    made.bare({{0, 0}, seen.size});

    // Pasek zabiera jedną kolumnę z prawej, więc treść dostaje o tyle mniej miejsca — tak samo,
    // jak w przewijakach.
    const int aside = kept_->style.progress && given.wide > 0 ? 1 : 0;

    spread({given.wide - aside, given.high});

    if (kept_->shown.empty() && aside == 0) return made;

    const Style look = skin().get<Style>(role());

    made.fill({{0, 0}, seen.size}, look);

    for (std::size_t one = 0; one < kept_->shown.size(); ++one) {
        const Cut& piece = kept_->shown[one];
        const int y = static_cast<int>(one) - seen.at.y;

        made.string({-seen.at.x, y}, piece.text, look);

        if (!marks().empty())
            painted(made, piece, {-seen.at.x, y}, look, marks().all(), which(),
                    kept_->chosen, skin());
    }

    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;

    if (aside != 0 && gauged(shift, whole, window))
        edged(made, given, seen, true, given.high, shift, whole, window, skin(), kept_->bar);

    return made;
}

// **Oddajemy fałsz dla wszystkiego, czego nie zużyliśmy.** F-klawisze mają dojść do legendy, która
// jest przodkiem, a wędrówka kończy się na pierwszej prawdzie.
bool Page::react(KeyEvent& what) {
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

bool Page::react(WheelEvent& what) {
    const std::size_t was = kept_->top;
    const int part = kept_->part;

    slide(-what.steps() * stepping);

    return kept_->top != was || kept_->part != part;
}

bool Page::react(ClickEvent&) {
    if (root() != nullptr) focus();

    return true;
}

}

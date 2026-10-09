#include "talcite/table.hpp"

#include <algorithm>
#include <utility>

#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// Ile wierszy bierzemy ze źródła za jednym razem. Ta sama liczba i ten sam powód co przy zwoju:
// pytanie o dokładnie tyle, ile ma wysokości okno, kosztowałoby wołanie na klatkę.
constexpr std::size_t batch = 64;

// Ile wierszy przeskakuje kółko. Ta sama stała co w liście i w przewijaku.
constexpr int stepping = 3;

// Odstęp między kolumnami. Bez niego nazwa dotykałaby rozmiaru i nie dałoby się ich rozróżnić.
constexpr int spacing = 1;

}

struct Table::Kept {
    TableSource source;
    std::size_t rows = 0;

    std::vector<TableColumn> columns;

    std::size_t top = 0;
    std::optional<std::size_t> which;

    // To, co widać. Przeliczane wtedy i tylko wtedy, gdy zmieni się przydział albo położenie —
    // inaczej każda klatka pytałaby źródło od nowa, a przy animowanym sąsiedztwie dzieje się to
    // dziesięć razy na sekundę.
    std::vector<TableRow> shown;
    bool fresh = true;
    Size sized;

    // Policzone szerokości kolumn dla bieżącej szerokości okna.
    std::vector<int> spread;

    // Zaznaczenie po numerze wiersza. Wektor bitów, nie zbiór: przy stu tysiącach wierszy to
    // dwanaście kilobajtów, a „zaznacz wszystkie" jest w panelu równie częste jak „zaznacz trzy",
    // więc zbiór bywałby większy od wektora.

    TableShape shape;

    std::string scrollbar_role = "talcite.scroll";

    Grip grip;
};

Table::Table() : kept_(std::make_unique<Kept>()) { role("talcite.table"); }

Table::Table(TableShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.table");

    kept_->shape = shape;
}

Table::~Table() = default;

// Liczbę wierszy podaje wołający, bo tylko on ją zna: tabela nie ma jak policzyć katalogu,
// a pytanie źródła „ile masz" przy każdej klatce byłoby drogie.
void Table::source(TableSource given, std::size_t rows) {
    Kept& kept = *kept_;

    kept.source = std::move(given);
    kept.rows = rows;

    kept.top = 0;
    kept.which = rows > 0 ? std::optional<std::size_t>{0} : std::optional<std::size_t>{};

    chosen().clear();

    kept.shown.clear();
    kept.fresh = true;
}

bool Table::sourced() const { return static_cast<bool>(kept_->source); }

// Zaznaczeń **nie czyścimy**: po przesortowaniu numery wierszy znaczą co innego niż przed, ale to
// wołający wie, czy tak było, więc `chosen().clear()` woła on, kiedy uzna.
void Table::refresh() {
    kept_->shown.clear();
    kept_->fresh = true;
}

void Table::columns(std::vector<TableColumn> given) {
    kept_->columns = std::move(given);
    kept_->fresh = true;
}

const std::vector<TableColumn>& Table::columns() const { return kept_->columns; }

std::size_t Table::rows() const { return kept_->rows; }

std::optional<std::size_t> Table::which() const { return kept_->which; }

std::size_t Table::first() const { return kept_->top; }

std::size_t Table::shown() const { return kept_->shown.size(); }

bool Table::heading() const { return kept_->shape.heading; }

void Table::heading(bool given) {
    if (kept_->shape.heading == given) return;

    kept_->shape.heading = given;
    kept_->fresh = true;
}

bool Table::scrollbar() const { return kept_->shape.scrollbar; }

void Table::scrollbar(bool given) {
    if (kept_->shape.scrollbar == given) return;

    kept_->shape.scrollbar = given;
    kept_->fresh = true;
}

const std::string& Table::scrollbar_role() const { return kept_->scrollbar_role; }

void Table::scrollbar_role(std::string given) { kept_->scrollbar_role = std::move(given); }

bool Table::focusable() const { return true; }

// Tabela nie przyjmuje dzieci: wiersze przychodzą ze źródła, nie z drzewa węzłów.
int Table::capacity() const { return 0; }

Size Table::wants(Size room) const { return room; }

// Postawienie bieżącego wiersza **dociąga do niego widok**, jeśli wypadł poza ekran. Wysokość
// bierzemy z prostokąta z poprzedniej klatki — tylko on wie, ile tabela naprawdę dostała.
void Table::which(std::size_t given) {
    Kept& kept = *kept_;

    if (kept.rows == 0) return;

    const std::size_t want = std::min(given, kept.rows - 1);

    if (kept.which.has_value() && *kept.which == want) return;

    kept.which = want;

    const int high = std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1);

    if (want < kept.top)
        kept.top = want;
    else if (want >= kept.top + static_cast<std::size_t>(high))
        kept.top = want - static_cast<std::size_t>(high) + 1;

    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

void Table::first(std::size_t given) {
    Kept& kept = *kept_;

    const std::size_t want = kept.rows == 0 ? 0 : std::min(given, kept.rows - 1);

    if (kept.top == want) return;

    kept.top = want;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// Ujemna liczba idzie w górę — ta sama umowa co w `Page::step`, żeby nie było dwóch nazw na jeden
// ruch. Ruch przestawia **wiersz bieżący**, a widok dociąga się za nim.
void Table::step(int rows) {
    Kept& kept = *kept_;

    if (kept.rows == 0 || rows == 0) return;

    const auto at = static_cast<std::ptrdiff_t>(kept.which.value_or(0));
    const auto much = static_cast<std::ptrdiff_t>(kept.rows) - 1;

    which(static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(at + rows, 0, much)));
}

void Table::page(int pages) {
    const int high = std::max(area().size.high - (kept_->shape.heading ? 1 : 0), 1);

    step(pages * high);
}

void Table::end() {
    if (kept_->rows > 0) which(kept_->rows - 1);
}



namespace {

// Szerokości kolumn dla danej szerokości okna. Kolumna z `wide > 0` bierze swoje, a te z zerem
// dzielą resztę po równo. Gdy zabraknie miejsca, ucinamy od prawej — panel nie przewija się
// w poziomie, tylko przycina, i my też nie.
std::vector<int> parted(const std::vector<TableColumn>& columns, int wide) {
    std::vector<int> made(columns.size(), 0);

    if (columns.empty() || wide <= 0) return made;

    const int between = spacing * static_cast<int>(columns.size() - 1);

    int fixed = 0;
    int loose = 0;

    for (const TableColumn& one : columns) {
        if (one.wide > 0)
            fixed += one.wide;
        else
            ++loose;
    }

    int rest = wide - between - fixed;

    for (std::size_t at = 0; at < columns.size(); ++at) {
        if (columns[at].wide > 0) {
            made[at] = columns[at].wide;

            continue;
        }

        // **Kolumna ruchoma dostaje co najmniej jedną kratkę.** Bez tego kolumna o stałej
        // szerokości, która sama nie mieści się w oknie, zabierałaby wszystko i nazwa pliku
        // znikałaby w całości — a to ona jest w panelu treścią, nie rozmiar.
        const int part = loose > 0 ? std::max(rest / loose, 1) : 0;

        made[at] = part;

        rest -= part;
        --loose;
    }

    // Przycinanie od prawej, gdy suma i tak nie mieści się w oknie. Odstęp liczymy **tylko między**
    // kolumnami, a nie za ostatnią — inaczej ostatnia kolumna traciłaby kratkę, której nikt nie
    // zajmuje, i liczba wyrównana do prawej stałaby o jedno miejsce za wcześnie.
    int much = 0;

    for (std::size_t at = 0; at < made.size(); ++at) {
        const int gap = at + 1 < made.size() ? spacing : 0;

        if (much >= wide) {
            made[at] = 0;

            continue;
        }

        made[at] = std::min(made[at], wide - much);
        much += made[at] + gap;
    }

    return made;
}

// Napis dopchnięty do szerokości kolumny wedle wyrównania. Przycinamy `cut`em z `eliding.hpp`,
// czyli po kolumnach i szerokościach znaków, a nie po bajtach.
std::string ranged(const std::string& what, int wide, Justify justify) {
    if (wide <= 0) return {};

    const std::string got = cut(what, wide);
    const int much = columns(got, got.size());
    const int rest = std::max(wide - much, 0);

    if (rest == 0) return got;

    if (justify == Justify::right) return std::string(static_cast<std::size_t>(rest), ' ') + got;

    if (justify == Justify::center) {
        const int left = rest / 2;

        return std::string(static_cast<std::size_t>(left), ' ') + got +
               std::string(static_cast<std::size_t>(rest - left), ' ');
    }

    return got + std::string(static_cast<std::size_t>(rest), ' ');
}

}

// Przeliczamy **tylko wtedy, gdy zmienił się przydział albo położenie**. Ta sama reguła co
// w `Page::spread` i z tego samego powodu.
void Table::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int high = given.high - (kept.shape.heading ? 1 : 0);

    kept.spread = parted(kept.columns, given.wide - aside);

    if (!kept.source || given.wide <= 0 || high <= 0) return;

    const std::size_t want = static_cast<std::size_t>(high);

    while (kept.shown.size() < want) {
        const std::vector<TableRow> got = kept.source(kept.top + kept.shown.size(), batch);

        if (got.empty()) break;

        for (const TableRow& one : got) {
            if (kept.shown.size() >= want) break;

            kept.shown.push_back(one);
        }
    }
}

View Table::paint(Size given, Area seen) {
    Kept& kept = *kept_;

    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    // Zaznaczenie przycinamy do liczby wierszy **tutaj**, bo zbiór nie ma wskazania na tabelę
    // i nie ma jak się o zmianie dowiedzieć — ta sama droga, którą historia przycina swoją listę
    // i którą strona kasuje kursor wędrówki po znacznikach.
    chosen_.keep(kept.rows);

    // Wiersze mogły zniknąć za plecami tabeli — przycinamy tu, bo tu i tak liczymy wszystko od nowa.
    if (kept.which.has_value() && *kept.which >= kept.rows)
        kept.which = kept.rows > 0 ? std::optional<std::size_t>{kept.rows - 1}
                                   : std::optional<std::size_t>{};

    if (kept.rows > 0 && kept.top >= kept.rows) {
        kept.top = kept.rows - 1;
        kept.fresh = true;
    }

    spread(given);

    const Skin& how = skin();

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int wide = given.wide - aside;
    const int head = kept.shape.heading ? 1 : 0;

    // Nagłówek. Tytuły kolumn stoją w tych samych szerokościach co treść, więc kolumna i jej opis
    // nigdy się nie rozjeżdżają.
    if (head != 0 && wide > 0) {
        const Style look = how.style(role() + ".head");

        made.fill({{-seen.at.x, -seen.at.y}, {wide, 1}}, look);

        int at = -seen.at.x;

        for (std::size_t one = 0; one < kept.columns.size() && one < kept.spread.size(); ++one) {
            const int room = kept.spread[one];

            if (room > 0)
                made.put({at, -seen.at.y}, ranged(kept.columns[one].title, room,
                                                     kept.columns[one].justify),
                            look);

            at += room + spacing;
        }
    }

    for (std::size_t row = 0; row < kept.shown.size(); ++row) {
        const std::size_t number = kept.top + row;
        const int y = head + static_cast<int>(row) - seen.at.y;

        const bool here = kept.which.has_value() && *kept.which == number;
        const bool taken = chosen().contains(number);

        const Style look = how.style(here && taken ? role() + ".mark.focus"
                                          : here       ? role() + ".focus"
                                          : taken      ? role() + ".mark"
                                                       : role());

        made.fill({{-seen.at.x, y}, {wide, 1}}, look);

        int at = -seen.at.x;

        for (std::size_t one = 0; one < kept.spread.size(); ++one) {
            const int room = kept.spread[one];

            if (room <= 0) {
                at += room + spacing;

                continue;
            }

            const TableCell& cell = one < kept.shown[row].size() ? kept.shown[row][one] : TableCell{};

            // Rola komórki nakłada się **na** rolę stanu wiersza, więc kolor rodzaju pliku nie
            // unieważnia podświetlenia — zaznaczony wiersz i tak żółknie w całości.
            const Style look_one =
                cell.role.empty() ? look : Style::merge(look, how.style(cell.role));

            const Justify justify =
                one < kept.columns.size() ? kept.columns[one].justify : Justify::left;

            made.put({at, y}, ranged(cell.text, room, justify), look_one);

            at += room + spacing;
        }
    }

    // Pasek tylko wtedy, gdy wierszy jest więcej, niż widać; kolumna na niego zostaje, żeby
    // kolumny tabeli nie skakały przy zmianie listy.
    if (aside != 0 && given.high > 0 && kept.rows > static_cast<std::size_t>(std::max(given.high - head, 0))) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = kept.top,
                            .whole = kept.rows,
                            .window = static_cast<std::size_t>(std::max(given.high - head, 1)),
                            .skin = how,
                            .role = kept.scrollbar_role};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        kept.grip.laid(mine);
    } else {
        kept.grip.laid(std::nullopt);
    }

    return made;
}

// Strzałki, klawisze stron, Home, End i Insert. **Enter i litery idą wyżej**: „wybrano wiersz" to
// sprawa aplikacji, bo w panelu Enter wchodzi do katalogu, a w oknie dialogowym naciska przycisk
// podstawowy.
bool Table::handle(KeyEvent& what) {
    Kept& kept = *kept_;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (kept.rows == 0) return false;

    switch (what.key().code()) {
        case Code::up: step(-1); return true;

        case Code::down: step(1); return true;

        case Code::pageup: page(-1); return true;

        case Code::pagedown: page(1); return true;

        case Code::home: which(0); return true;

        case Code::end: end(); return true;

        // **Insertu tabela nie bierze.** Stało tu zaznaczanie ze zejściem o wiersz niżej — odruch
        // z panelu Midnight Commandera. Był to jedyny klawisz w całej bibliotece, przy którym
        // `handle` **zmieniał dane**, a nie tylko przesuwał kursor, i którego wołający nie miał jak
        // odwołać: przechwycenie „wyżej" nie działa, bo tabela leży bliżej ogniska. Kto chce ten
        // odruch, wiesza `on<KeyEvent>` na tabeli i woła `mark` sam.

        default: break;
    }

    return false;
}

// Kółko przesuwa **sam widok**, a wiersza bieżącego nie rusza — tak samo jak w liście i w panelu
// Midnight Commandera. Na końcu przewijania oddajemy fałsz, żeby ruch dostał przewijak wyżej.
bool Table::handle(WheelEvent& what) {
    Kept& kept = *kept_;

    const int high = std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1);

    const auto much = static_cast<std::ptrdiff_t>(kept.rows);
    const auto was = static_cast<std::ptrdiff_t>(kept.top);

    const std::ptrdiff_t want = std::clamp<std::ptrdiff_t>(
        was - what.steps() * stepping, 0,
        std::max<std::ptrdiff_t>(much - static_cast<std::ptrdiff_t>(high), 0));

    if (want == was) return false;

    first(static_cast<std::size_t>(want));

    return true;
}

// Wiersz danych pod kratką ekranu; nagłówek i pustka pod ostatnim wierszem to nic.
std::optional<std::size_t> Table::row_under(Spot where) const {
    const Kept& kept = *kept_;

    const int head = kept.shape.heading ? 1 : 0;
    const int y = local(where).y - head;

    if (y < 0) return std::nullopt;

    const std::size_t at = kept.top + static_cast<std::size_t>(y);

    if (at >= kept.rows) return std::nullopt;

    return at;
}

// Kolumna pod kratką nagłówka — po szerokościach z ostatniej klatki, z odstępem między kolumnami
// po stronie lewej kolumny. Poza nagłówkiem, na pasku i za ostatnią kolumną — nic.
std::optional<std::size_t> Table::column_under(Spot where) const {
    const Kept& kept = *kept_;

    const Spot here = local(where);

    if (!kept.shape.heading || here.y != 0 || here.x < 0 || kept.grip.over(here)) return std::nullopt;

    int at = 0;

    for (std::size_t one = 0; one < kept.spread.size() && one < kept.columns.size(); ++one) {
        const int room = kept.spread[one];

        if (room > 0 && here.x < at + room + spacing) return one;

        at += room + spacing;
    }

    return std::nullopt;
}

// **Mysz na pasku** (U6 w twa): strzałka — wiersz, tor — strona, suwak — ciągnięcie. Ruszamy sam
// widok, jak kółko, a wiersza bieżącego nie: tak samo jak w MC i jak w przewijaku.
bool Table::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    const Asked asked = kept.grip.mouse(what, local(what.where()), *this);

    const auto high = static_cast<std::size_t>(std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1));
    const std::size_t most = kept.rows > high ? kept.rows - high : 0;

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: first(kept.top > 0 ? kept.top - 1 : 0); break;
        case Part::forth: first(std::min(kept.top + 1, most)); break;
        case Part::before: first(kept.top > high ? kept.top - high : 0); break;
        case Part::after: first(std::min(kept.top + high, most)); break;
        case Part::thumb:
            if (asked.to.has_value()) first(std::min(*asked.to, most));
            break;
    }

    return true;
}

// Klik stawia wiersz bieżący i bierze ognisko. Klik na pasku zrobił już swoje przy wciśnięciu —
// tu nie stawia wiersza, który akurat stoi na tej wysokości.
bool Table::handle(ClickEvent& what) {
    if (kept_->grip.over(local(what.where()))) return true;

    if (root() != nullptr) focus();

    if (const std::optional<std::size_t> at = row_under(what.where())) which(*at);

    return true;
}

// **Dwuklik na wierszu przepuszczamy wyżej**, z tego samego powodu co Enter: co znaczy wybór
// wiersza, wie aplikacja. Wiersz staje się bieżący (klik przed dwuklikiem już go postawił, ale
// lista mogła się w międzyczasie przesunąć). Dwuklik na nagłówku i pod ostatnim wierszem nic nie
// znaczy i dalej nie idzie.
bool Table::handle(DoubleClickEvent& what) {
    if (kept_->grip.over(local(what.where()))) return true;

    const std::optional<std::size_t> at = row_under(what.where());

    if (!at) return true;

    which(*at);

    return false;
}

}

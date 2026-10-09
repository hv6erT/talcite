#include "talcite/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

// --- Różnica napisów ------------------------------------------------------------------------------

namespace {

// **Słowo to ciąg liter i cyfr, a każdy inny znak jest osobnym kawałkiem.** Porównanie po znakach
// rozsypałoby `.1080p.BluRay` na litery pasujące przypadkiem do czegoś po drugiej stronie; po
// słowach wychodzi jedna usunięta całość. Bajt spoza ASCII liczymy jako literę, bo należy do
// litery wielobajtowej — polska nazwa nie rozpada się na ogonku.
bool wordy(unsigned char sign) {
    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z') ||
           sign >= 0x80;
}

std::vector<std::string_view> tokenized(std::string_view text) {
    std::vector<std::string_view> made;

    std::size_t at = 0;

    while (at < text.size()) {
        std::size_t end = at + 1;

        if (wordy(static_cast<unsigned char>(text[at])))
            while (end < text.size() && wordy(static_cast<unsigned char>(text[end]))) ++end;

        made.push_back(text.substr(at, end - at));

        at = end;
    }

    return made;
}

// Ile kroków edycji wolno szukać, zanim uznamy, że napisy są po prostu inne. Dla nazw plików to
// daleko ponad potrzebę; dla wierszy pliku to granica, za którą podświetlanie różnic przestaje
// cokolwiek mówić, a pamięć śladu rośnie z kwadratem.
constexpr int patience = 256;

// Ile kawałków w ogóle porównujemy. Wiersz dłuższy niż tyle słów zaznacza się w całości.
constexpr std::size_t ample = 4096;

// **Najdłuższy wspólny podciąg algorytmem Myersa** — dla środka bez wspólnego początku: przejście po przekątnych z zapamiętanym
// śladem, a potem powrót od końca. Koszt rośnie z długością razy liczba różnic, więc dwa prawie
// równe napisy kosztują tyle, co ich przeczytanie. Oddaje fałsz, gdy różnic jest więcej niż
// `patience` — wtedy wołający zaznacza całość.
bool matched(const std::vector<std::string_view>& left, std::size_t from_left, std::size_t upto_left,
             const std::vector<std::string_view>& right, std::size_t from_right, std::size_t upto_right,
             std::vector<bool>& kept_left, std::vector<bool>& kept_right) {
    const int n = static_cast<int>(upto_left - from_left);
    const int m = static_cast<int>(upto_right - from_right);

    if (n == 0 || m == 0) return true;

    const auto same = [&](int x, int y) {
        return left[from_left + static_cast<std::size_t>(x)] == right[from_right + static_cast<std::size_t>(y)];
    };

    const int most = n + m;
    const int off = most + 1;

    std::vector<int> reach(static_cast<std::size_t>(2 * most + 3), 0);

    // Ślad: dla każdego `d` wartości przekątnych od `-d` do `d`, już po kroku `d`.
    std::vector<std::vector<int>> trace;

    const int limit = std::min(most, patience);

    for (int d = 0; d <= limit; ++d) {
        for (int k = -d; k <= d; k += 2) {
            const std::size_t here = static_cast<std::size_t>(off + k);

            int x = (k == -d || (k != d && reach[here - 1] < reach[here + 1])) ? reach[here + 1] : reach[here - 1] + 1;
            int y = x - k;

            while (x < n && y < m && same(x, y)) {
                ++x;
                ++y;
            }

            reach[here] = x;

            if (x < n || y < m) continue;

            // Powrót: z każdego kroku zostaje jedna edycja i przekątna przed nią, a przekątne to
            // właśnie wspólne kawałki.
            int bx = n;
            int by = m;

            for (int back = d; back > 0; --back) {
                const std::vector<int>& before = trace[static_cast<std::size_t>(back - 1)];

                const auto at = [&before, back](int j) { return before[static_cast<std::size_t>(j + back - 1)]; };

                const int kk = bx - by;
                const bool down = kk == -back || (kk != back && at(kk - 1) < at(kk + 1));
                const int pk = down ? kk + 1 : kk - 1;
                const int px = at(pk);
                const int py = px - pk;

                while (bx > px && by > py) {
                    kept_left[from_left + static_cast<std::size_t>(bx - 1)] = true;
                    kept_right[from_right + static_cast<std::size_t>(by - 1)] = true;
                    --bx;
                    --by;
                }

                bx = px;
                by = py;
            }

            // Przekątnej przed pierwszą edycją nie ma: wspólny początek odciął już wołający, więc
            // pierwsze kawałki obu stron są różne.
            return true;
        }

        trace.emplace_back(reach.begin() + off - d, reach.begin() + off + d + 1);
    }

    return false;
}

// Zakresy bajtów kawałków niezachowanych, sklejone, gdy stykają się ze sobą.
std::vector<DiffSpan> spanned(const std::vector<std::string_view>& tokens, const std::vector<bool>& kept,
                              std::string_view text) {
    std::vector<DiffSpan> made;

    for (std::size_t one = 0; one < tokens.size(); ++one) {
        if (kept[one]) continue;

        const auto from = static_cast<std::size_t>(tokens[one].data() - text.data());
        const std::size_t upto = from + tokens[one].size();

        if (!made.empty() && made.back().upto == from)
            made.back().upto = upto;
        else
            made.push_back(DiffSpan{from, upto});
    }

    return made;
}

}

// **Różnica dwóch napisów po słowach.** Wspólny początek i koniec odcinamy od razu — nazwy plików
// i wiersze po poprawce różnią się zwykle w środku — a środek porównuje Myers. Gdy różnic jest za
// dużo albo napis za długi, cały środek jest różnicą: podświetlenie połowy znaków nic by nie
// powiedziało.
std::pair<DiffSide, DiffSide> diffed(std::string_view left, std::string_view right) {
    std::pair<DiffSide, DiffSide> made;

    made.first.text = std::string{left};
    made.second.text = std::string{right};

    const std::vector<std::string_view> ones = tokenized(made.first.text);
    const std::vector<std::string_view> twos = tokenized(made.second.text);

    std::vector<bool> kept_one(ones.size(), false);
    std::vector<bool> kept_two(twos.size(), false);

    std::size_t head = 0;

    while (head < ones.size() && head < twos.size() && ones[head] == twos[head]) {
        kept_one[head] = true;
        kept_two[head] = true;
        ++head;
    }

    std::size_t tail = 0;

    while (tail < ones.size() - head && tail < twos.size() - head &&
           ones[ones.size() - 1 - tail] == twos[twos.size() - 1 - tail]) {
        kept_one[ones.size() - 1 - tail] = true;
        kept_two[twos.size() - 1 - tail] = true;
        ++tail;
    }

    const std::size_t upto_one = ones.size() - tail;
    const std::size_t upto_two = twos.size() - tail;

    if (upto_one - head + upto_two - head <= ample)
        (void)matched(ones, head, upto_one, twos, head, upto_two, kept_one, kept_two);

    made.first.marked = spanned(ones, kept_one, made.first.text);
    made.second.marked = spanned(twos, kept_two, made.second.text);

    return made;
}

// --- Widżet ---------------------------------------------------------------------------------------

namespace {

// Ile kawałków bierzemy ze źródła za jednym razem i ile trzymamy pod ręką.
constexpr std::size_t batch = 128;

// Ile wierszy przeskakuje kółko. Ta sama stała co w liście, tabeli i przewijaku.
constexpr int stepping = 3;

// Pole wyboru: `[x] `. Cztery kratki, jak w `Tick`.
constexpr int ticking = 4;

// Rozdział stron: spacja, kreska, spacja.
constexpr int parting = 3;

// Poniżej tylu kratek na stronę widok sam przechodzi na jedną kolumnę.
constexpr int narrow = 24;

// Wiersz ekranu w obrębie kawałka: tytuł albo wiersz różnicy — w widoku jednokolumnowym jedna
// z jego stron albo obie naraz, gdy są równe.
enum class Face : std::uint8_t { title, both, left, right };

struct Stripe {
    Face face = Face::both;
    std::size_t row = 0;
};

bool same(const DiffRow& one) {
    return one.left.has_value() && one.right.has_value() && one.left->marked.empty() &&
           one.right->marked.empty() && one.left->text == one.right->text;
}

// Wiersze ekranu kawałka. Zwinięty pokazuje tytuł i pierwszy wiersz — reszta to „+N" przy nim.
std::vector<Stripe> lined(const DiffHunk& hunk, bool open, bool unify) {
    std::vector<Stripe> made;

    if (!hunk.title.empty()) made.push_back(Stripe{Face::title, 0});

    const std::size_t shown = open ? hunk.rows.size() : std::min<std::size_t>(hunk.rows.size(), 1);

    for (std::size_t row = 0; row < shown; ++row) {
        const DiffRow& one = hunk.rows[row];

        if (!unify || same(one)) {
            made.push_back(Stripe{Face::both, row});

            continue;
        }

        if (one.left.has_value()) made.push_back(Stripe{Face::left, row});
        if (one.right.has_value()) made.push_back(Stripe{Face::right, row});
    }

    if (made.empty()) made.push_back(Stripe{Face::title, 0});

    return made;
}

// Znak do narysowania: bajty, szerokość i czy leży w zaznaczonej różnicy.
struct Glyph {
    std::string text;
    int wide = 0;
    bool marked = false;
};

std::vector<Glyph> glyphed(const DiffSide& side) {
    std::vector<Glyph> made;

    std::size_t span = 0;
    int column = 0;

    for (std::size_t at = 0; at < side.text.size();) {
        const std::size_t was = at;
        const char32_t sign = utf8::decode(side.text, at);

        while (span < side.marked.size() && side.marked[span].upto <= was) ++span;

        const bool marked = span < side.marked.size() && side.marked[span].from <= was;

        if (sign == U'\t') {
            const int upto = ((column / tabbing) + 1) * tabbing;

            for (; column < upto; ++column) made.push_back(Glyph{" ", 1, marked});

            continue;
        }

        const int wide = std::max(width(sign), 0);

        made.push_back(Glyph{side.text.substr(was, at - was), wide, marked});

        column += wide;

        if (was == at) break;
    }

    return made;
}

// Przycięcie znaków do szerokości, z wielokropkiem tam, gdzie każe kształt. Tnie się **znaki**,
// a nie napis, żeby zaznaczenia zostały przy swoich literach także po skróceniu środka.
std::vector<Glyph> fitted(std::vector<Glyph> all, int room, Elide how, const std::string& dots) {
    int whole = 0;

    for (const Glyph& one : all) whole += one.wide;

    if (whole <= room) return all;

    if (room <= 0) return {};

    const auto head = [&all](int much) {
        std::vector<Glyph> made;
        int used = 0;

        for (const Glyph& one : all) {
            if (used + one.wide > much) break;

            made.push_back(one);
            used += one.wide;
        }

        return made;
    };

    const auto tail = [&all](int much) {
        std::vector<Glyph> made;
        int used = 0;

        for (auto one = all.rbegin(); one != all.rend(); ++one) {
            if (used + one->wide > much) break;

            made.push_back(*one);
            used += one->wide;
        }

        std::reverse(made.begin(), made.end());

        return made;
    };

    if (how == Elide::none) return head(room);

    const Glyph mark{dots, 1, false};

    if (room == 1) return {mark};

    std::vector<Glyph> made;

    if (how == Elide::right) {
        made = head(room - 1);
        made.push_back(mark);

        return made;
    }

    if (how == Elide::left) {
        made.push_back(mark);

        for (Glyph& one : tail(room - 1)) made.push_back(std::move(one));

        return made;
    }

    const int half = (room - 1) / 2;

    made = head(half);
    made.push_back(mark);

    for (Glyph& one : tail(room - 1 - half)) made.push_back(std::move(one));

    return made;
}

int digits(std::size_t number) {
    int made = 1;

    for (; number >= 10; number /= 10) ++made;

    return made;
}

}

struct DiffViewer::Kept {
    DiffSource source;
    std::size_t hunks = 0;

    std::string left_title;
    std::string right_title;

    // Okno kawałków pod ręką: od `from`, tyle, ile źródło oddało. Pytamy źródło tylko wtedy, gdy
    // potrzebny kawałek wypadł poza okno.
    std::size_t from = 0;
    std::vector<DiffHunk> cached;

    std::vector<bool> open;

    // Pierwszy wiersz ekranu: kawałek i wiersz w nim.
    std::size_t top = 0;
    int part = 0;

    std::optional<std::size_t> which;

    // Czy widać kolumnę pól wyboru. Raz włączona zostaje do nowego źródła — inaczej przewinięcie do
    // miejsca bez pól przesuwałoby treść w bok.
    bool ticks = false;

    std::optional<bool> unify;

    // To, co widać: wiersze ekranu z kopią swojego kawałka. Liczone tylko po zmianie.
    struct Shown {
        std::size_t hunk = 0;
        Stripe line;
        bool first = false;
        DiffHunk data;
    };

    std::vector<Shown> shown;

    // Czy za ostatnim pokazanym wierszem jest jeszcze coś — reszta kawałka albo następny kawałek.
    // Bez tego pasek nie ma czego pokazać: cały plan stoi na ekranie.
    bool more = false;

    bool fresh = true;
    Size sized;
    bool unified_now = false;

    DiffShape shape;

    std::string scrollbar_role = "talcite.scroll";

    // Pasek z ostatniej klatki — pod mysz (U6 w twa).
    Grip grip;

    // Kawałek pod numerem albo pustka — za końcem albo gdy źródło każe czekać. Wskaźnik żyje do
    // następnego pytania: nowe okno zastępuje stare.
    const DiffHunk* fetched(std::size_t which) {
        if (which >= hunks || !source) return nullptr;

        if (which >= from && which < from + cached.size()) return &cached[which - from];

        const std::size_t start = which >= batch / 2 ? which - batch / 2 : 0;

        std::optional<std::vector<DiffHunk>> got = source(start, batch);

        if (!got.has_value()) return nullptr;

        from = start;
        cached = std::move(*got);

        for (const DiffHunk& one : cached)
            if (one.tickable) ticks = true;

        if (which >= from + cached.size()) return nullptr;

        return &cached[which - from];
    }

    [[nodiscard]] bool opened(std::size_t hunk) const { return hunk < open.size() && open[hunk]; }

    // Liczba wierszy ekranu kawałka; pustka, gdy go nie ma.
    std::optional<int> height(std::size_t hunk) {
        const DiffHunk* one = fetched(hunk);

        if (one == nullptr) return {};

        return static_cast<int>(lined(*one, opened(hunk), unified_now).size());
    }

    // Kursor staje tylko na kawałkach z wierszami. Sam tytuł to nagłówek sekcji — rozdziela,
    // a nie jest czymś, co się wybiera.
    bool steady(std::size_t hunk) {
        const DiffHunk* one = fetched(hunk);

        return one != nullptr && !one->rows.empty();
    }

    [[nodiscard]] int high() const { return sized.high - (left_title.empty() && right_title.empty() ? 0 : 1); }

    // **Ekran nie wychodzi poza koniec**: gdy od pierwszego wiersza do końca zostaje mniej niż
    // wysokość, cofamy się, aż ostatni wiersz stanie na dole. Kawałek, który czeka, zatrzymuje
    // cofanie — nie wiadomo, ile ma wierszy.
    void settle() {
        const int room = high();

        if (room <= 0) return;

        int rows = -part;

        for (std::size_t one = top; one < hunks && rows < room; ++one) {
            const std::optional<int> got = height(one);

            if (!got.has_value()) return;

            rows += *got;
        }

        while ((top > 0 || part > 0) && rows < room) {
            if (part > 0) {
                --part;
            } else {
                const std::optional<int> got = height(top - 1);

                if (!got.has_value()) break;

                --top;
                part = *got - 1;
            }

            ++rows;
        }
    }

    // Dociągnięcie widoku do pierwszego wiersza kawałka: wyżej — staje na górze, niżej — na dole.
    void reveal(std::size_t hunk) {
        const int room = high();

        if (hunk < top || (hunk == top && part > 0)) {
            top = hunk;
            part = 0;
        } else if (room > 0) {
            int before = -part;

            for (std::size_t one = top; one < hunk && before < room; ++one) before += height(one).value_or(1);

            if (before >= room) {
                top = hunk;
                part = 0;

                for (int need = room - 1; need > 0 && (top > 0 || part > 0); --need) {
                    if (part > 0) {
                        --part;
                    } else {
                        --top;
                        part = height(top).value_or(1) - 1;
                    }
                }
            }
        }

        settle();

        fresh = true;
    }
};

DiffViewer::DiffViewer() : kept_(std::make_unique<Kept>()) { role("talcite.diff"); }

DiffViewer::DiffViewer(DiffShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.diff");

    kept_->shape = shape;
}

DiffViewer::~DiffViewer() = default;

// Liczbę kawałków podaje wołający, jak przy tabeli. Kursor staje na pierwszym kawałku z wierszami
// — o ile źródło go już ma; inaczej przy pierwszym malowaniu.
void DiffViewer::source(DiffSource given, std::size_t hunks) {
    Kept& kept = *kept_;

    kept.source = std::move(given);
    kept.hunks = hunks;

    kept.from = 0;
    kept.cached.clear();
    kept.open.assign(hunks, false);

    kept.top = 0;
    kept.part = 0;
    kept.which.reset();
    kept.ticks = false;

    chosen().clear();

    for (std::size_t one = 0; one < hunks && one < batch; ++one)
        if (kept.steady(one)) {
            kept.which = one;

            break;
        }

    kept.shown.clear();
    kept.fresh = true;
}

void DiffViewer::refresh() {
    kept_->cached.clear();
    kept_->shown.clear();
    kept_->fresh = true;
}

void DiffViewer::titles(std::string left, std::string right) {
    kept_->left_title = std::move(left);
    kept_->right_title = std::move(right);
    kept_->fresh = true;
}

std::size_t DiffViewer::hunks() const { return kept_->hunks; }

std::optional<std::size_t> DiffViewer::which() const { return kept_->which; }

void DiffViewer::which(std::size_t given) {
    Kept& kept = *kept_;

    if (kept.hunks == 0) return;

    const std::size_t want = std::min(given, kept.hunks - 1);

    if (kept.which == want) return;

    kept.which = want;

    kept.reveal(want);

    (void)trigger(ScrollEvent{});
}

bool DiffViewer::opened(std::size_t hunk) const { return kept_->opened(hunk); }

void DiffViewer::opened(std::size_t hunk, bool given) {
    Kept& kept = *kept_;

    if (hunk >= kept.open.size() || kept.open[hunk] == given) return;

    kept.open[hunk] = given;

    if (kept.which.has_value()) kept.reveal(*kept.which);

    kept.settle();
    kept.fresh = true;
}

std::optional<bool> DiffViewer::unified() const { return kept_->unify; }

void DiffViewer::unified(std::optional<bool> given) {
    kept_->unify = given;
    kept_->fresh = true;
}

bool DiffViewer::scrollbar() const { return kept_->shape.scrollbar; }

void DiffViewer::scrollbar(bool given) {
    kept_->shape.scrollbar = given;
    kept_->fresh = true;
}

const std::string& DiffViewer::scrollbar_role() const { return kept_->scrollbar_role; }

void DiffViewer::scrollbar_role(std::string given) { kept_->scrollbar_role = std::move(given); }

bool DiffViewer::focusable() const { return true; }

int DiffViewer::capacity() const { return 0; }

Size DiffViewer::wants(Size room) const { return room; }

// Wiersze ekranu od pierwszego widocznego, z kopią kawałka przy każdym — źródło może oddać nowe
// okno w połowie drogi, a to, co już stoi na ekranie, nie ma prawa się zmienić.
void DiffViewer::spread(Size given) {
    Kept& kept = *kept_;

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int ticks = kept.ticks ? ticking : 0;
    const int side = (given.wide - aside - ticks - parting) / 2;
    const bool unify = kept.unify.value_or(side < narrow);

    if (!kept.fresh && kept.sized == given && kept.unified_now == unify) return;

    kept.sized = given;
    kept.unified_now = unify;
    kept.fresh = false;
    kept.shown.clear();
    kept.more = kept.top > 0 || kept.part > 0;

    const int room = kept.high();

    if (room <= 0 || given.wide <= 0) return;

    kept.settle();

    int skip = kept.part;

    std::size_t one = kept.top;

    for (; one < kept.hunks && static_cast<int>(kept.shown.size()) < room; ++one) {
        const DiffHunk* got = kept.fetched(one);

        // Kawałek, na który źródło każe czekać, zatrzymuje pętlę przed końcem — to, że jest dalej
        // coś do pokazania, mówi warunek za pętlą.
        if (got == nullptr) break;

        const DiffHunk data = *got;
        const std::vector<Stripe> lines = lined(data, kept.opened(one), unify);

        if (static_cast<int>(kept.shown.size() + lines.size()) - skip > room) kept.more = true;

        for (std::size_t at = 0; at < lines.size() && static_cast<int>(kept.shown.size()) < room; ++at) {
            if (skip > 0) {
                --skip;

                continue;
            }

            kept.shown.push_back(Kept::Shown{one, lines[at], at == 0, data});
        }

        skip = 0;
    }

    if (one < kept.hunks) kept.more = true;
}

View DiffViewer::paint(Size given, Area seen) {
    Kept& kept = *kept_;

    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    chosen_.keep(kept.hunks);

    if (!kept.which.has_value())
        for (std::size_t one = 0; one < kept.hunks && one < batch; ++one)
            if (kept.steady(one)) {
                kept.which = one;

                break;
            }

    spread(given);

    const Skin& how = skin();
    const std::string base = role();
    const std::string dots = utf8::encode(how.glyph("talcite.elide"));

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int wide = given.wide - aside;
    const int ticks = kept.ticks ? ticking : 0;
    const bool unify = kept.unified_now;

    // Numery wierszy: tyle cyfr, ile ma największy widoczny, a gdy żadna strona numeru nie niesie —
    // kolumny nie ma wcale.
    int number = 0;

    for (const Kept::Shown& one : kept.shown) {
        if (one.line.face == Face::title) continue;

        const DiffRow& row = one.data.rows[one.line.row];

        for (const std::optional<DiffSide>& side : {row.left, row.right})
            if (side.has_value() && side->number.has_value()) number = std::max(number, digits(*side->number));
    }

    const int gutter = number > 0 ? number + 1 : 0;

    const int side = unify ? wide - ticks - 2 * gutter - 2 : (wide - ticks - parting) / 2 - gutter;
    const int right_at = unify ? 0 : ticks + gutter + side + parting;

    const auto written = [&made](int x, int y, const std::vector<Glyph>& glyphs, const Style& look,
                                 const Style& marked) {
        for (const Glyph& one : glyphs) {
            made.put({x, y}, one.text, one.marked ? marked : look);

            x += one.wide;
        }
    };

    const auto numbered = [&made, number](int x, int y, const std::optional<DiffSide>& one, const Style& look) {
        if (number == 0 || !one.has_value() || !one->number.has_value()) return;

        std::string said = std::to_string(*one->number);

        said.insert(0, static_cast<std::size_t>(number) - said.size(), ' ');

        made.put({x, y}, said, look);
    };

    int y = -seen.at.y;

    if (!kept.left_title.empty() || !kept.right_title.empty()) {
        const Style look = how.style(base + ".head");

        made.fill({{-seen.at.x, y}, {wide, 1}}, look);

        if (unify) {
            const std::string both = kept.left_title + " → " + kept.right_title;

            made.put({-seen.at.x + ticks, y}, elided(both, wide - ticks, Elide::right, how.glyph("talcite.elide")),
                     look);
        } else {
            made.put({-seen.at.x + ticks + gutter, y},
                     elided(kept.left_title, side, Elide::right, how.glyph("talcite.elide")), look);
            made.put({-seen.at.x + right_at, y},
                     elided(kept.right_title, side, Elide::right, how.glyph("talcite.elide")), look);
        }

        ++y;
    }

    const char32_t upright = how.lines(Border::thin).upright;

    for (const Kept::Shown& one : kept.shown) {
        const bool here = one.first && kept.which == one.hunk;

        Style look = how.style(here ? base + ".focus" : base);

        if (!one.data.role.empty()) look = Style::merge(look, how.style(one.data.role));

        const int x = -seen.at.x;

        made.fill({{x, y}, {wide, 1}}, look);

        if (one.first && one.data.tickable)
            made.put({x, y}, chosen_.contains(one.hunk) ? "[x] " : "[ ] ", look);

        if (one.line.face == Face::title) {
            const Style title = Style::merge(look, how.style(base + ".title"));

            made.put({x + ticks, y}, elided(one.data.title, wide - ticks, Elide::right, how.glyph("talcite.elide")),
                     title);

            ++y;

            continue;
        }

        const DiffRow& row = one.data.rows[one.line.row];
        const bool even = same(row);

        const Style gone = even ? look : Style::merge(look, how.style(base + ".gone"));
        const Style came = even ? look : Style::merge(look, how.style(base + ".came"));
        const Style gone_mark = Style::merge(gone, how.style(base + ".gone.mark"));
        const Style came_mark = Style::merge(came, how.style(base + ".came.mark"));
        const Style faint = Style::merge(look, how.style(base + ".number"));

        // „+N" przy pierwszym wierszu zwiniętego kawałka: ile wierszy czeka za rozwinięciem.
        std::string folded;

        if (one.line.row == 0 && !kept.opened(one.hunk) && one.data.rows.size() > 1)
            folded = " +" + std::to_string(one.data.rows.size() - 1);

        const int tag = static_cast<int>(folded.size());

        if (unify) {
            const int text_at = x + ticks + 2 * gutter;

            if (one.line.face != Face::right) numbered(x + ticks, y, row.left, faint);
            if (one.line.face != Face::left) numbered(x + ticks + gutter, y, row.right, faint);

            const bool left = one.line.face == Face::left || (one.line.face == Face::both && !row.right.has_value());
            const DiffSide& shown_side = left ? *row.left : *row.right;

            const char* sign = even ? "  " : left ? "- " : "+ ";

            made.put({text_at, y}, sign, even ? look : left ? gone : came);

            written(text_at + 2, y, fitted(glyphed(shown_side), side - tag, kept.shape.elide, dots),
                    even ? look : left ? gone : came, left ? gone_mark : came_mark);

            if (tag > 0 && one.line.face != Face::right) made.put({text_at + 2 + side - tag, y}, folded, faint);
        } else {
            numbered(x + ticks, y, row.left, faint);

            if (row.left.has_value())
                written(x + ticks + gutter, y, fitted(glyphed(*row.left), side - tag, kept.shape.elide, dots), gone,
                        gone_mark);

            if (tag > 0) made.put({x + ticks + gutter + side - tag, y}, folded, faint);

            made.put({x + ticks + gutter + side + 1, y}, upright, look);

            numbered(x + right_at, y, row.right, faint);

            if (row.right.has_value())
                written(x + right_at + gutter, y, fitted(glyphed(*row.right), side, kept.shape.elide, dots), came,
                        came_mark);
        }

        ++y;
    }

    // Pasek tylko wtedy, gdy jest czym przewijać; kolumna na niego zostaje, żeby treść nie skakała.
    if (aside != 0 && given.high > 0 && kept.more) {
        std::size_t shown = 0;
        std::optional<std::size_t> last;

        for (const Kept::Shown& one : kept.shown)
            if (last != one.hunk) {
                ++shown;
                last = one.hunk;
            }

        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = kept.top,
                            .whole = kept.hunks,
                            .window = std::max<std::size_t>(shown, 1),
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

// **Klawisze przesuwają kursor po kawałkach, a Spacja, `+` i `-` zmieniają pola.** Tab, Enter
// i litery idą wyżej: Tab należy do okna, które krąży po swoich kontrolkach, Enter naciska
// przycisk podstawowy. Skok po sekcjach bierze więc Ctrl ze strzałką, a nie Tab.
bool DiffViewer::handle(KeyEvent& what) {
    Kept& kept = *kept_;

    if (what.mods().alt || kept.hunks == 0) return false;

    const std::size_t at = kept.which.value_or(0);

    // Następny kawałek z wierszami w danym kierunku, od `from` wyłącznie; pustka, gdy go nie ma.
    const auto next = [&kept](std::size_t from, int by) -> std::optional<std::size_t> {
        for (std::size_t one = from; by > 0 ? one + 1 < kept.hunks : one > 0;) {
            one = by > 0 ? one + 1 : one - 1;

            if (kept.steady(one)) return one;
        }

        return {};
    };

    // Nagłówek sekcji: kawałek z samym tytułem.
    const auto heading = [&kept](std::size_t one) {
        const DiffHunk* got = kept.fetched(one);

        return got != nullptr && got->rows.empty();
    };

    if (what.mods().ctrl) {
        const Code code = what.key().code();

        if (code != Code::down && code != Code::up) return false;

        // Ctrl+↓: pierwszy kawałek za następnym nagłówkiem; Ctrl+↑: pierwszy kawałek sekcji, a gdy
        // już na nim stoimy — poprzedniej.
        if (code == Code::down) {
            for (std::size_t one = at + 1; one < kept.hunks; ++one)
                if (heading(one))
                    if (const std::optional<std::size_t> got = next(one, 1); got.has_value()) {
                        which(*got);

                        return true;
                    }

            return true;
        }

        std::size_t start = at;

        while (start > 0 && !heading(start - 1)) --start;

        if (start == at && start > 0) {
            start = start - 1;

            while (start > 0 && !heading(start - 1)) --start;
        }

        if (kept.steady(start))
            which(start);
        else if (const std::optional<std::size_t> got = next(start, 1); got.has_value() && *got < at)
            which(*got);

        return true;
    }

    const auto moved = [this, &next, at](int by, int times) {
        std::size_t here = at;

        for (int step = 0; step < times; ++step) {
            const std::optional<std::size_t> got = next(here, by);

            if (!got.has_value()) break;

            here = *got;
        }

        which(here);
    };

    const int page = std::max(static_cast<int>(kept.shown.size()), 1);

    switch (what.key().code()) {
        case Code::up: moved(-1, 1); return true;

        case Code::down: moved(1, 1); return true;

        case Code::pageup: moved(-1, page); return true;

        case Code::pagedown: moved(1, page); return true;

        case Code::home:
            if (kept.steady(0))
                which(0);
            else if (const std::optional<std::size_t> got = next(0, 1); got.has_value())
                which(*got);

            return true;

        case Code::end:
            if (kept.steady(kept.hunks - 1))
                which(kept.hunks - 1);
            else if (const std::optional<std::size_t> got = next(kept.hunks - 1, -1); got.has_value())
                which(*got);

            return true;

        case Code::right: opened(at, true); return true;

        case Code::left: opened(at, false); return true;

        default: break;
    }

    const char32_t letter = what.key().letter();

    if (letter == U' ') {
        const DiffHunk* got = kept.fetched(at);

        if (got == nullptr || !got->tickable) return true;

        if (chosen_.contains(at))
            chosen_.remove(at);
        else
            chosen_.add(at);

        kept.fresh = true;

        (void)trigger(PickEvent{});

        return true;
    }

    // `+` i `-` przestawiają całą sekcję kursora: od nagłówka przed nim do następnego nagłówka.
    if (letter == U'+' || letter == U'-') {
        std::size_t start = at;

        while (start > 0 && !heading(start - 1)) --start;

        bool changed = false;

        for (std::size_t one = start; one < kept.hunks && !(one > at && heading(one)); ++one) {
            const DiffHunk* got = kept.fetched(one);

            if (got == nullptr || !got->tickable || chosen_.contains(one) == (letter == U'+')) continue;

            if (letter == U'+')
                chosen_.add(one);
            else
                chosen_.remove(one);

            changed = true;
        }

        if (changed) {
            kept.fresh = true;

            (void)trigger(PickEvent{});
        }

        return true;
    }

    return false;
}

// Kółko przesuwa sam widok, kursora nie rusza — jak w tabeli.
bool DiffViewer::handle(WheelEvent& what) { return slid(-what.steps() * stepping); }

// **Mysz na pasku** (U6 w twa): strzałka — wiersz, tor — strona, suwak — skok na kawałek. Pasek
// liczy kawałki, więc ciągnięty suwak stawia widok na początku kawałka, jak Home na nim.
bool DiffViewer::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    const Asked asked = kept.grip.mouse(what, local(what.where()), *this);

    const int head = kept.left_title.empty() && kept.right_title.empty() ? 0 : 1;
    const int page = std::max(area().size.high - head, 1);

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: (void)slid(-1); break;
        case Part::forth: (void)slid(1); break;
        case Part::before: (void)slid(-page); break;
        case Part::after: (void)slid(page); break;
        case Part::thumb:
            if (asked.to.has_value() && kept.hunks > 0) {
                const std::size_t want = std::min(*asked.to, kept.hunks - 1);

                if (want != kept.top || kept.part != 0) {
                    kept.top = want;
                    kept.part = 0;

                    kept.settle();

                    kept.fresh = true;

                    (void)trigger(ScrollEvent{});
                }
            }
            break;
    }

    return true;
}

// Widok o tyle wierszy, ile kazano — w dół dodatnio — po kawałkach i ich wierszach; kursora nie rusza.
bool DiffViewer::slid(int lines) {
    Kept& kept = *kept_;

    const std::size_t was = kept.top;
    const int part = kept.part;

    for (; lines > 0; --lines) {
        const std::optional<int> got = kept.height(kept.top);

        if (!got.has_value()) break;

        if (kept.part + 1 < *got) {
            ++kept.part;
        } else {
            if (kept.top + 1 >= kept.hunks) break;

            ++kept.top;
            kept.part = 0;
        }
    }

    for (; lines < 0; ++lines) {
        if (kept.part > 0) {
            --kept.part;
        } else {
            if (kept.top == 0) break;

            const std::optional<int> got = kept.height(kept.top - 1);

            if (!got.has_value()) break;

            --kept.top;
            kept.part = *got - 1;
        }
    }

    kept.settle();

    if (kept.top == was && kept.part == part) return false;

    kept.fresh = true;

    (void)trigger(ScrollEvent{});

    return true;
}

// Klik stawia kursor na kawałku pod nim; klik w pole wyboru przestawia je.
bool DiffViewer::handle(ClickEvent& what) {
    Kept& kept = *kept_;

    // Klik na pasku zrobił swoje przy wciśnięciu; kursora nie stawia.
    if (kept.grip.over(local(what.where()))) return true;

    if (root() != nullptr) focus();

    const int head = kept.left_title.empty() && kept.right_title.empty() ? 0 : 1;
    const Spot at = local(what.where());
    const int y = at.y - head;

    if (y < 0 || static_cast<std::size_t>(y) >= kept.shown.size()) return true;

    const Kept::Shown& one = kept.shown[static_cast<std::size_t>(y)];
    const std::size_t hunk = one.hunk;
    const bool box = one.first && one.data.tickable && at.x < ticking;

    if (one.data.rows.empty()) return true;

    which(hunk);

    if (box) {
        if (chosen_.contains(hunk))
            chosen_.remove(hunk);
        else
            chosen_.add(hunk);

        kept.fresh = true;

        (void)trigger(PickEvent{});
    }

    return true;
}

}

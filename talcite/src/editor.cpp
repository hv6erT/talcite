#include "talcite/editor.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "talcite/clipboard.hpp"
#include "talcite/encoding.hpp"

#include "eliding.hpp"
#include "quilt.hpp"
#include "spool.hpp"

#include "placing.hpp"

namespace talcite::ui {

namespace {

// Rola zaznaczenia. Zaznaczenie **nie jest znacznikiem** — ma kotwicę i głowę, których znacznik
// nie ma — ale rysuje się nim, bo malowanie zakresu to dokładnie ta sama robota.
constexpr const char* selecting = "talcite.editor.select";

std::size_t counted(std::string_view what) {
    return static_cast<std::size_t>(std::count(what.begin(), what.end(), '\n'));
}

std::string said(const Bytes& what) {
    std::string made;

    made.reserve(what.size());

    for (const std::byte one : what) made.push_back(static_cast<char>(one));

    return made;
}

// Kończy grupę cofania. Spacja, tabulator i enter zamykają wyraz — dzięki temu jedno cofnięcie
// zabiera cały wyraz, a nie literę, i nie przeskakuje przy tym przez koniec wiersza.
bool sealed(const std::string& what) {
    if (what.empty()) return true;

    const char last = what.back();

    return last == '\n' || last == ' ' || last == '\t';
}

}

struct Editor::Kept {
    // Jedna zmiana dokumentu: **co zniknęło, co przyszło i gdzie stał wtedy kursor**. Kołdra sama
    // się nie cofnie — jej dopiski nigdy nie znikają — więc odwrotność każdej zmiany trzeba
    // zapamiętać osobno. Pamięć rośnie z sumą długości zmian, nie z rozmiarem pliku.
    struct Change {
        std::size_t at = 0;
        std::string gone;
        std::string came;
        std::size_t was = 0;

        // Cofa się razem z poprzednim wpisem. Stąd „zamień wszystkie" wraca jednym ruchem, choć
        // zmian było sto — bez trzymania w pamięci całego dokumentu sprzed zamiany.
        bool tied = false;
    };

    std::shared_ptr<Quilt> quilt;
    std::shared_ptr<Spool> spool;

    std::size_t cursor = 0;
    std::size_t anchor = 0;

    // Zapamiętane miejsce w wierszu dla strzałek w pionie. Trzymamy **bajt**, nie kolumnę: kolumna
    // wymagałaby powtórzenia tu reguły tabulatorów, którą zna już cięcie na kawałki. Skutek jest
    // taki, że przy wierszach z tabulatorami kursor w pionie potrafi się przesunąć — do poprawienia
    // wtedy, gdy zacznie przeszkadzać.
    std::size_t offset = 0;

    bool readonly = false;

    // Czy trwa ciągnięcie myszą. Ruch bez wciśniętego przycisku przychodzi tylko przy `?1003`,
    // którego nie włączamy, ale flaga i tak jest potrzebna: ruch po puszczeniu nad edytorem
    // nie ma prawa rozciągać zaznaczenia.
    bool dragging = false;

    std::vector<Change> log;

    // Ile wpisów z dziennika jest **wykonanych**. Wszystko za tym to gałąź do ponowienia, którą
    // pierwsza nowa zmiana utnie.
    std::size_t done = 0;

    // Ile ich było przy ostatnim zapisie. Stąd `dirty()`: cofnięcie wszystkiego z powrotem daje
    // dokument czysty, bo wraca do tego samego miejsca w dzienniku.
    std::size_t washed = 0;

    // Czy kolejny znak może przedłużyć ostatni wpis. Gaśnie przy każdym ruchu kursora, przy
    // cofaniu i przy ponawianiu — inaczej wyraz wpisany po przeskoku w inne miejsce dokleiłby się
    // do wyrazu sprzed przeskoku.
    bool joining = false;

    // Czy następny wpis ma być przywiązany do poprzedniego.
    bool tying = false;

    [[nodiscard]] std::string text(std::size_t from, std::size_t upto) const;

    void apply(std::size_t at, const std::string& gone, const std::string& came);

    void note(std::size_t at, std::string gone, std::string came, std::size_t was);
};

std::string Editor::Kept::text(std::size_t from, std::size_t upto) const {
    if (!quilt || upto <= from) return {};

    return said(quilt->bytes(from, upto - from));
}

// **Jedyne miejsce, w którym dokument naprawdę się zmienia.** Wpisanie, kasowanie, cofnięcie
// i ponowienie sprowadzają się tutaj, więc poprawka dla szpuli jest napisana raz — a to ona
// decyduje, czy ekran po zmianie pokaże nową treść.
void Editor::Kept::apply(std::size_t at, const std::string& gone, const std::string& came) {
    // Ostatni bajt dokumentu rozstrzyga, czy ostatni wiersz w ogóle istnieje: „abc" to jeden
    // wiersz, „abc\n" **też jeden**, a pusty dokument nie ma żadnego. Sama liczba enterów tego
    // nie powie, więc trzeba zajrzeć na koniec przed zmianą i po niej.
    const auto ending = [this](std::size_t much) {
        if (much == 0) return '\0';

        const std::string one = said(quilt->bytes(much - 1, 1));

        return one.empty() ? '\0' : one.front();
    };

    const std::size_t was = quilt->whole();
    const char before = ending(was);

    if (!gone.empty()) quilt->erase(at, gone.size());

    if (!came.empty()) quilt->insert(at, came);

    const std::size_t now = quilt->whole();
    const char after = ending(now);

    std::ptrdiff_t lines = static_cast<std::ptrdiff_t>(counted(came)) -
                           static_cast<std::ptrdiff_t>(counted(gone));

    if (now > 0 && after != '\n') ++lines;

    if (was > 0 && before != '\n') --lines;

    spool->changed(at, static_cast<std::ptrdiff_t>(now) - static_cast<std::ptrdiff_t>(was), lines);
}

// Nowa zmiana ucina gałąź ponowienia — po cofnięciu i wpisaniu czegoś innego nie ma już dokąd
// ponawiać. Poza tym **pisany wyraz zbiera się w jeden wpis**: znak dopisany dokładnie tam, gdzie
// skończył się poprzedni, przedłuża ostatni zamiast robić nowy. Bez tego cofanie chodziłoby po
// literze i pięć tysięcy znaków dałoby pięć tysięcy kroków.
void Editor::Kept::note(std::size_t at, std::string gone, std::string came, std::size_t was) {
    log.resize(done);

    if (joining && !log.empty()) {
        Change& last = log.back();

        const bool typing = gone.empty() && !came.empty() && last.gone.empty() &&
                            !sealed(last.came) && came.find('\n') == std::string::npos &&
                            last.at + last.came.size() == at;

        if (typing) {
            last.came += came;

            return;
        }

        // Trzymany Backspace zbiera się tak samo, tyle że od tyłu: każdy kolejny znak znika
        // **przed** poprzednim, więc początek wpisu się cofa.
        const bool erasing = came.empty() && !gone.empty() && last.came.empty() &&
                             !last.gone.empty() && gone.find('\n') == std::string::npos &&
                             at + gone.size() == last.at;

        if (erasing) {
            last.gone = gone + last.gone;
            last.at = at;

            return;
        }

        // Trzymany Delete zbiera od przodu: kasowany bajt jest wciąż ten sam.
        const bool ahead = came.empty() && !gone.empty() && last.came.empty() &&
                           !last.gone.empty() && gone.find('\n') == std::string::npos &&
                           at == last.at;

        if (ahead) {
            last.gone += gone;

            return;
        }
    }

    log.push_back(Change{at, std::move(gone), std::move(came), was, tying});

    done = log.size();
    joining = true;
}

Editor::Editor() : kept_(std::make_unique<Kept>()) { role("talcite.editor"); }

Editor::Editor(ScrollStyle style) : Page(style), kept_(std::make_unique<Kept>()) {
    role("talcite.editor");
}

Editor::~Editor() = default;

namespace {

Roll rolled(const std::shared_ptr<Spool>& spool) {
    return [spool](std::size_t which, std::size_t most) { return spool->lines(which, most); };
}

Feed fed(const std::shared_ptr<Quilt>& quilt) {
    return [quilt](std::size_t at, std::size_t most) { return quilt->bytes(at, most); };
}

}

// **Kołdra pod szpulą.** Szpula czyta bajty przez tablicę kawałków, więc widzi dokument razem
// ze zmianami, a oryginału nikt nie wczytuje.
void Editor::hold(Bytes given) {
    kept_->quilt = std::make_shared<Quilt>(std::move(given));
    kept_->spool = std::make_shared<Spool>(fed(kept_->quilt), kept_->quilt->whole());

    kept_->cursor = 0;
    kept_->anchor = 0;
    kept_->offset = 0;

    // Nowy dokument to nowy dziennik: cofnięcie do stanu poprzedniego pliku nie miałoby sensu,
    // a `dirty()` musi zacząć od czystego.
    kept_->log.clear();
    kept_->done = 0;
    kept_->washed = 0;
    kept_->joining = false;
    kept_->tying = false;

    hold(rolled(kept_->spool), 0);

    // Dokument w pamięci liczymy od razu — to przejście po tym, co i tak trzymamy.
    counting();
}

void Editor::hold(Feed given, std::size_t whole) {
    kept_->quilt = std::make_shared<Quilt>(std::move(given), whole);
    kept_->spool = std::make_shared<Spool>(fed(kept_->quilt), kept_->quilt->whole());

    kept_->cursor = 0;
    kept_->anchor = 0;
    kept_->offset = 0;

    kept_->log.clear();
    kept_->done = 0;
    kept_->washed = 0;
    kept_->joining = false;
    kept_->tying = false;

    hold(rolled(kept_->spool), 0);
}

bool Editor::readonly() const { return kept_->readonly; }

void Editor::readonly(bool given) { kept_->readonly = given; }

std::size_t Editor::cursor() const { return kept_->cursor; }

std::size_t Editor::anchor() const { return kept_->anchor; }

bool Editor::selected() const { return kept_->cursor != kept_->anchor; }

std::optional<std::size_t> Editor::at() const {
    if (!kept_->spool) return {};

    const std::optional<std::size_t> which = line();

    if (!which.has_value()) return {};

    return kept_->spool->at(*which);
}

std::optional<std::size_t> Editor::below() const {
    if (!kept_->spool) return {};

    const std::optional<std::size_t> which = line();

    if (!which.has_value()) return {};

    return kept_->spool->at(*which + shown());
}

std::optional<std::size_t> Editor::whole() const {
    if (!kept_->spool) return {};

    return kept_->spool->whole();
}

std::optional<std::size_t> Editor::lines() const {
    if (!kept_->spool) return Page::lines();

    return kept_->spool->counted();
}

void Editor::counting() {
    if (!kept_->spool || kept_->spool->counted().has_value()) return;

    (void)kept_->spool->at(SIZE_MAX);
}

void Editor::screen(int high) {
    if (kept_->spool) kept_->spool->screen(high);
}

// Kursor jako bajt dokumentu, a wiersz i przesunięcie w nim wylicza szpula. Odwrotnie byłoby
// gorzej: zmiany adresuje się bajtem, a wiersze zmieniają numerację przy każdym enterze.
//
// Postawienie kursora **zabiera ze sobą kotwicę** — czyli gasi zaznaczenie. Ruch z klawiszem
// Shift kotwicy nie rusza i dlatego idzie przez `stretch`; poza tym jednym obie drogi są tą samą
// robotą, więc jedna woła drugą.
void Editor::cursor(std::size_t given) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    kept.anchor = std::min(given, kept.spool->whole());

    stretch(given);
}

void Editor::stretch(std::size_t given) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    const std::size_t want = std::min(given, kept.spool->whole());

    kept.cursor = want;

    // Ruch kursora zamyka grupę cofania: wyraz wpisany po przeskoku w inne miejsce nie ma nic
    // wspólnego z wyrazem sprzed przeskoku.
    kept.joining = false;

    const std::size_t which = kept.spool->found(want);

    kept.offset = want - kept.spool->at(which).value_or(want);

    (void)show(which);

    // Zaznaczenie rysuje się znacznikami składanymi przy każdym malowaniu, ale sam obraz trzeba
    // złożyć od nowa — inaczej przesunięty kursor nie zmieniłby nic na ekranie.
    afresh();
    stirred();
}

std::size_t Editor::line() const {
    if (!kept_->spool) return 0;

    return kept_->spool->found(kept_->cursor);
}

// Kolumna liczona w **kratkach**, nie w bajtach: pasek stanu pokazuje to, co widać, a tabulator
// widać jako osiem miejsc. Bajt oddaje `cursor()`, więc obie miary są do wzięcia — tak samo jak
// przy pasku przewijania, który chodzi raz po wierszach, a raz po bajtach.
std::size_t Editor::column() const {
    Kept& kept = *kept_;

    if (!kept.spool) return 0;

    const std::size_t which = kept.spool->found(kept.cursor);
    const std::optional<std::size_t> began = kept.spool->at(which);

    if (!began.has_value() || kept.cursor < *began) return 0;

    const std::vector<std::string> got = kept.spool->lines(which, 1);

    if (got.empty()) return 0;

    return static_cast<std::size_t>(columns(got.front(), kept.cursor - *began));
}

void Editor::select(std::size_t from, std::size_t upto) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    const std::size_t much = kept.spool->whole();

    kept.anchor = std::min(from, much);
    kept.cursor = std::min(upto, much);
    kept.joining = false;

    afresh();
    stirred();
}

void Editor::unselect() {
    kept_->anchor = kept_->cursor;

    afresh();
}

std::string Editor::selection() const {
    Kept& kept = *kept_;

    if (!kept.quilt || kept.cursor == kept.anchor) return {};

    const std::size_t from = std::min(kept.cursor, kept.anchor);
    const std::size_t upto = std::max(kept.cursor, kept.anchor);

    return said(kept.quilt->bytes(from, upto - from));
}

// --- Schowek -------------------------------------------------------------------------------------

// **Schowek jest systemowy, nie nasz.** Biblioteka nie trzyma tu żadnej treści — `copied`
// i `pasted` to drzwi do schowka systemu, a na POSIX-ie do schowka maszyny, przy której siedzi
// użytkownik, przez terminal.
//
// Kopiowanie wolno także w dokumencie tylko do odczytu: niczego nie zmienia.
bool Editor::copy() {
    if (!selected()) return false;

    return copied(selection());
}

// Wycięcie idzie przez to samo kasowanie zaznaczenia, co Backspace, więc jest **jedną zmianą
// w dzienniku** i wraca jednym cofnięciem. Gdy skopiowanie się nie uda, nie kasujemy: dokument
// zostałby bez tekstu, którego nie ma dokąd wkleić.
bool Editor::cut() {
    if (kept_->readonly || !selected() || !copy()) return false;

    back_erase(0);

    return true;
}

bool Editor::paste() {
    if (kept_->readonly) return false;

    const std::optional<std::string> what = pasted();

    if (!what.has_value() || what->empty()) return false;

    insert(*what);

    return true;
}

namespace {

// Zaznaczenie rozpisane na znaczniki — po jednym na wiersz, bo znacznik żyje w jednym wierszu.
// Liczymy je **tylko dla tego, co widać**: zaznaczenie całego dwugigabajtowego pliku ma tyle
// wierszy, ile plik, a na ekranie i tak widać kilkadziesiąt.
std::vector<Mark> spanned(Spool& spool, std::size_t from, std::size_t upto, std::size_t top,
                           std::size_t many) {
    std::vector<Mark> made;

    if (from >= upto) return made;

    const std::size_t first = spool.found(from);
    const std::size_t last = spool.found(upto > 0 ? upto - 1 : 0);

    for (std::size_t which = std::max(first, top); which <= last && which < top + many; ++which) {
        const std::optional<std::size_t> began = spool.at(which);

        if (!began.has_value()) break;

        const std::optional<std::size_t> next = spool.at(which + 1);
        const std::size_t ends = next.value_or(spool.whole());

        const std::size_t left = std::max(from, *began) - *began;
        const std::size_t right = std::min(upto, ends) - *began;

        if (right > left) made.push_back(Mark{which, left, right, selecting});
    }

    return made;
}

}

// Znaczniki zaznaczenia składamy **przed** każdym złożeniem obrazu, a nie przy zmianie zaznaczenia:
// zakres widocznych wierszy zmienia też przewinięcie i zmiana rozmiaru okna, a te nie przechodzą
// przez `select`.
View Editor::paint(Size given, Area seen) {
    Kept& kept = *kept_;

    if (kept.spool && kept.cursor != kept.anchor) {
        const std::size_t from = std::min(kept.cursor, kept.anchor);
        const std::size_t upto = std::max(kept.cursor, kept.anchor);

        marks().all(spanned(*kept.spool, from, upto, topmost(),
                            static_cast<std::size_t>(std::max(given.high, 1))));
    } else if (!marks().empty()) {
        marks().clear();
    }

    return Page::paint(given, seen);
}

std::optional<Spot> Editor::caret() const {
    Kept& kept = *kept_;

    if (!kept.spool) return {};

    const std::size_t which = kept.spool->found(kept.cursor);
    const std::optional<std::size_t> began = kept.spool->at(which);

    if (!began.has_value()) return {};

    return spotted(which, kept.cursor - *began);
}

// Kursor po zmianie: wiersz przelicza szpula, a obraz i tak trzeba złożyć od nowa. Osobno, bo
// robią to wszystkie cztery drogi zmiany — wpisanie, oba kasowania, cofnięcie i ponowienie.
void Editor::settled(std::size_t where) {
    Kept& kept = *kept_;

    kept.cursor = std::min(where, kept.spool->whole());
    kept.anchor = kept.cursor;

    const std::size_t which = kept.spool->found(kept.cursor);

    kept.offset = kept.cursor - kept.spool->at(which).value_or(kept.cursor);

    (void)show(which);

    afresh();
    stirred();
}

// Wpisanie **na zaznaczeniu jest jedną zmianą**, nie kasowaniem i wpisaniem po nim: inaczej jedno
// cofnięcie oddawałoby pusty dokument zamiast tego, co było zaznaczone.
void Editor::insert(std::string what) {
    Kept& kept = *kept_;

    if (kept.readonly || !kept.quilt || what.empty()) return;

    const std::size_t from = std::min(kept.cursor, kept.anchor);
    const std::size_t upto = std::max(kept.cursor, kept.anchor);

    const std::string gone = kept.text(from, upto);
    const std::size_t was = kept.cursor;

    kept.apply(from, gone, what);
    kept.note(from, gone, what, was);

    settled(from + what.size());
}

void Editor::back_erase(std::size_t much) {
    Kept& kept = *kept_;

    if (kept.readonly || !kept.quilt) return;

    std::size_t from = 0;
    std::size_t upto = 0;

    if (kept.cursor != kept.anchor) {
        from = std::min(kept.cursor, kept.anchor);
        upto = std::max(kept.cursor, kept.anchor);
    } else {
        upto = kept.cursor;
        from = much >= kept.cursor ? 0 : kept.cursor - much;
    }

    if (upto <= from) return;

    const std::string gone = kept.text(from, upto);

    kept.apply(from, gone, {});
    kept.note(from, gone, {}, kept.cursor);

    settled(from);
}

void Editor::front_erase(std::size_t much) {
    Kept& kept = *kept_;

    if (kept.readonly || !kept.quilt) return;

    if (kept.cursor != kept.anchor) {
        back_erase(0);

        return;
    }

    const std::size_t whole = kept.quilt->whole();

    if (kept.cursor >= whole) return;

    const std::size_t took = std::min(much, whole - kept.cursor);

    if (took == 0) return;

    const std::size_t at = kept.cursor;
    const std::string gone = kept.text(at, at + took);

    kept.apply(at, gone, {});
    kept.note(at, gone, {}, at);

    settled(at);
}

// --- Szukanie ------------------------------------------------------------------------------------

namespace {

// Ile wierszy bierzemy ze szpuli za jednym razem. Bajt początku pytamy tylko o pierwszy z nich,
// a dalej doliczamy — pytanie o każdy z osobna kosztowałoby przejście od kotwicy raz na wiersz.
constexpr std::size_t combing = 64;

}

// **Szuka grzebień, ale po dokumencie chodzi edytor** — bo tylko on wie, gdzie zaczyna się wiersz.
// Znalezione miejsce od razu się zaznacza i pokazuje; kto chce samo pytanie bez ruszania widoku,
// niech pyta grzebienia o wiersz, który sam sobie weźmie.
//
// Nie zawijamy na koniec dokumentu: kolejne wołanie od zera jest decyzją aplikacji, nie widżetu.
std::optional<Hit> Editor::find(const Comb& how, std::size_t from, bool back) {
    Kept& kept = *kept_;

    if (!kept.spool || !how.ready()) return {};

    const std::size_t at = std::min(from, kept.spool->whole());
    const std::size_t which = kept.spool->found(at);

    // Wiersze od podanego numeru razem z bajtem, na którym każdy się zaczyna.
    const auto batch = [&kept](std::size_t first, std::size_t most) {
        std::vector<std::pair<std::size_t, std::string>> made;

        const std::optional<std::size_t> began = kept.spool->at(first);

        if (!began.has_value()) return made;

        std::size_t where = *began;

        for (std::string& one : kept.spool->lines(first, most)) {
            const std::size_t size = one.size();

            made.emplace_back(where, std::move(one));

            where += size + 1;
        }

        return made;
    };

    const auto picked = [this](std::size_t began, Hit one) {
        select(began + one.from, began + one.upto);

        return std::optional<Hit>{Hit{began + one.from, began + one.upto}};
    };

    if (!back) {
        std::size_t line = which;

        while (true) {
            const auto got = batch(line, combing);

            if (got.empty()) break;

            for (std::size_t one = 0; one < got.size(); ++one) {
                const std::size_t began = got[one].first;
                const std::string& text = got[one].second;

                const std::size_t start = line + one == which ? at - began : 0;

                if (start > text.size()) continue;

                const std::optional<Hit> found = how.forth(text, start);

                if (found.has_value()) return picked(began, *found);
            }

            line += got.size();
        }

        return {};
    }

    std::size_t high = which;

    while (true) {
        const std::size_t low = high >= combing ? high - combing + 1 : 0;

        const auto got = batch(low, high - low + 1);

        if (got.empty()) break;

        for (std::size_t one = got.size(); one > 0; --one) {
            const std::size_t began = got[one - 1].first;
            const std::string& text = got[one - 1].second;

            const std::size_t upto = low + one - 1 == which ? at - began : text.size();

            const std::optional<Hit> found = how.back(text, std::min(upto, text.size()));

            if (found.has_value()) return picked(began, *found);
        }

        if (low == 0) break;

        high = low - 1;
    }

    return {};
}

// Bez `all` zamieniamy **bieżące zaznaczenie, jeśli samo jest dopasowaniem**, i pokazujemy
// następne — tak, jak robi to każdy edytor: raz naciśnięty klawisz zamienia to, co świeci,
// i przechodzi dalej. Z `all` chodzimy od początku dokumentu i wiążemy wszystkie zmiany w jedną,
// żeby cofnięcie zabrało je razem.
std::size_t Editor::replace(const Comb& how, const std::string& with, bool all) {
    Kept& kept = *kept_;

    if (kept.readonly || !kept.quilt || !how.ready()) return 0;

    // Wpisanie pustki nie jest zmianą, więc zamiana na nic idzie przez kasowanie zaznaczenia.
    const auto put = [this, &with] {
        if (with.empty())
            back_erase(0);
        else
            insert(with);
    };

    if (!all) {
        if (selected()) {
            const std::string got = selection();
            const std::optional<Hit> one = how.forth(got, 0);

            if (one.has_value() && one->from == 0 && one->upto == got.size()) {
                const std::size_t began = std::min(kept.cursor, kept.anchor);

                put();

                (void)find(how, began + with.size(), false);

                return 1;
            }
        }

        (void)find(how, kept.cursor, false);

        return 0;
    }

    std::size_t made = 0;
    std::size_t at = 0;

    while (true) {
        const std::optional<Hit> one = find(how, at, false);

        if (!one.has_value()) break;

        kept.tying = made > 0;

        put();

        kept.tying = false;

        ++made;

        // Za zamienionym tekstem, żeby wzorzec ukryty we wstawce nie zapętlił zamiany.
        at = one->from + with.size();

        if (at <= one->from) at = one->from;
    }

    return made;
}

// --- Cofanie -------------------------------------------------------------------------------------

bool Editor::dirty() const { return kept_->done != kept_->washed; }

void Editor::clean() { kept_->washed = kept_->done; }

namespace {

// Ile bajtów naraz idzie do ujścia. Tyle samo, co okno szpuli — dokument bywa większy od pamięci,
// więc przez zapis też musi przepłynąć kawałkami.
constexpr std::size_t pouring = 64 * 1024;

}

// **Biblioteka nie dotyka plików.** Źródło daje aplikacja i ujście daje aplikacja — tędy przepływa
// sama treść. Ujście, które odda `false`, przerywa zapis; dokument zostaje wtedy brudny, bo
// niedokończony zapis nie jest zapisem.
//
// **Pisanie po tym samym pliku, z którego czyta źródło, zniszczy dokument**: oryginał nigdy nie
// został wczytany, odcinki wskazują w plik, więc nadpisanie go podstawia nowe bajty pod stare
// wskazania. Aplikacja pisze obok i przemianowuje — albo woła wcześniej `settle`.
bool Editor::save(const Drain& into) {
    Kept& kept = *kept_;

    if (!kept.quilt || !into) return false;

    const std::size_t whole = kept.quilt->whole();

    std::size_t at = 0;

    while (at < whole) {
        Bytes got = kept.quilt->bytes(at, pouring);

        // Źródło obiecało więcej, niż oddaje. Lepiej powiedzieć „nie zapisano" niż zapisać ogryzek.
        if (got.empty()) return false;

        at += got.size();

        if (!into(std::move(got))) return false;
    }

    clean();

    return true;
}

// Odcięcie od źródła: po nim wolno nadpisać plik, z którego dokument był czytany. Dziennik zostaje
// ważny, bo zmiany adresuje się przesunięciem w dokumencie, a te się nie ruszają.
void Editor::settle() {
    if (kept_->quilt) (void)kept_->quilt->settle();
}

bool Editor::undoable() const { return !kept_->readonly && kept_->done > 0; }

bool Editor::redoable() const { return !kept_->readonly && kept_->done < kept_->log.size(); }

// Cofnięcie to ta sama zmiana czytana od tyłu: wraca to, co zniknęło, znika to, co przyszło.
// Kursor staje tam, gdzie stał **przed** zmianą — bo to miejsce, do którego wraca dokument.
void Editor::undo() {
    Kept& kept = *kept_;

    if (!undoable()) return;

    std::size_t where = kept.cursor;

    // Wpisy przywiązane cofają się razem — „zamień wszystkie" ma wracać jednym ruchem.
    while (kept.done > 0) {
        const Kept::Change& one = kept.log[kept.done - 1];

        kept.apply(one.at, one.came, one.gone);

        --kept.done;
        where = one.was;

        if (!one.tied) break;
    }

    kept.joining = false;

    settled(where);
}

void Editor::redo() {
    Kept& kept = *kept_;

    if (!redoable()) return;

    std::size_t where = kept.cursor;

    while (kept.done < kept.log.size()) {
        const Kept::Change& one = kept.log[kept.done];

        kept.apply(one.at, one.gone, one.came);

        ++kept.done;
        where = one.at + one.came.size();

        if (kept.done >= kept.log.size() || !kept.log[kept.done].tied) break;
    }

    kept.joining = false;

    settled(where);
}

namespace {

// Ile bajtów ma znak **przed** tym miejscem. Kasowanie liczy się w bajtach, a wpisany znak bywa
// wielobajtowy — bez tego Backspace rozcinałby ogonek na pół i zostawiał śmieć.
std::size_t behind(Quilt& quilt, std::size_t at) {
    if (at == 0) return 0;

    const std::size_t look = at < 4 ? at : 4;
    const std::string what = said(quilt.bytes(at - look, look));

    std::size_t much = 1;

    while (much < what.size() &&
           (static_cast<unsigned char>(what[what.size() - much]) & 0xC0) == 0x80)
        ++much;

    return much;
}

std::size_t ahead(Quilt& quilt, std::size_t at) {
    const std::string what = said(quilt.bytes(at, 4));

    if (what.empty()) return 0;

    const std::size_t much = following(static_cast<unsigned char>(what.front())) + 1;

    return std::min(much, what.size());
}

// **Skok po wyrazach.** Regułę „bajt wyrazu" bierzemy z `encoding.hpp` — tę samą, po której
// grzebień szuka całych wyrazów i po której dwuklik bierze wyraz.
//
// W przód: przeskocz ciąg tego samego rodzaju, na którym stoisz, a potem to, co wyrazem nie jest —
// czyli stań na **początku następnego wyrazu**. W tył symetrycznie: przeskocz to, co wyrazem nie
// jest, a potem sam wyraz — czyli stań na **początku tego albo poprzedniego**. Na krańcu wiersza
// przechodzimy do sąsiedniego; szukamy w wierszu, bo wierszami mówi zwój.
std::size_t worded(Spool& spool, std::size_t at, bool forth) {
    const std::size_t which = spool.found(at);
    const std::optional<std::size_t> began = spool.at(which);

    if (!began.has_value() || at < *began) return at;

    const std::vector<std::string> got = spool.lines(which, 1);
    const std::string text = got.empty() ? std::string{} : got.front();

    std::size_t off = std::min(at - *began, text.size());

    const auto lettered = [&text](std::size_t one) {
        return wordly(static_cast<char32_t>(static_cast<unsigned char>(text[one])));
    };

    if (forth) {
        if (off >= text.size()) return spool.at(which + 1).value_or(at);

        const bool kind = lettered(off);

        while (off < text.size() && lettered(off) == kind) ++off;

        while (off < text.size() && !lettered(off)) ++off;

        return *began + off;
    }

    if (off == 0) {
        if (which == 0) return at;

        const std::optional<std::size_t> prev = spool.at(which - 1);

        if (!prev.has_value()) return at;

        const std::vector<std::string> before = spool.lines(which - 1, 1);

        return *prev + (before.empty() ? 0 : before.front().size());
    }

    while (off > 0 && !lettered(off - 1)) --off;

    while (off > 0 && lettered(off - 1)) --off;

    return *began + off;
}

}

bool Editor::react(KeyEvent& what) {
    Kept& kept = *kept_;

    if (!kept.quilt) return false;

    const Mods mods = what.mods();

    // **Jeden komplet klawiszy schowka, nie dwa** — ten z Midnight Commandera, bo to jest wzorzec
    // tej biblioteki. Stał tu obok drugi, Ctrl+C/X/V, i to on był kłopotem: w trybie surowym Ctrl+C
    // nie jest przerwaniem, tylko zwykłym klawiszem, więc edytor połykał go, ilekroć miał ognisko —
    // i aplikacja, która chciała go na wyjście, przestawała go widzieć, nie mogąc tego odwołać.
    // Po usunięciu tamtego kompletu Ctrl+C przechodzi wyżej jak każdy inny klawisz.
    //
    // Kto woli odruch z reszty świata, wiesza `on<KeyEvent>` i woła `copy`, `cut` i `paste` sam —
    // uchwyt widzi klawisz niezależnie od tego, co robi `react`.
    if (what.key().code() == Code::insert && !mods.alt) {
        if (mods.ctrl) {
            (void)copy();

            return true;
        }

        if (mods.shift) {
            (void)paste();

            return true;
        }
    }

    if (what.key().code() == Code::remove && mods.shift && !mods.ctrl && !mods.alt) {
        (void)cut();

        return true;
    }

    if (what.key().letter() != 0 && !mods.ctrl && !mods.alt) {
        insert(spelled(what.key().letter()));

        return true;
    }

    // Ruch z Shiftem ciągnie zaznaczenie, bez Shiftu je gasi. Rozgałęzienie stoi w jednym miejscu,
    // bo docelowy bajt każdy ruch liczy sam, a różnica między nimi jest tylko taka jedna.
    const auto went = [this, &mods](std::size_t where) {
        if (mods.shift)
            stretch(where);
        else
            cursor(where);
    };

    // Wiersz o zadanym numerze i jego początek — potrzebne i pionowym strzałkom, i klawiszom
    // stronicowym, i Home z End.
    const auto along = [&kept](std::size_t which, std::size_t offset) {
        const std::optional<std::size_t> began = kept.spool->at(which);

        if (!began.has_value()) return std::optional<std::size_t>{};

        const std::vector<std::string> got = kept.spool->lines(which, 1);
        const std::size_t much = got.empty() ? 0 : got.front().size();

        return std::optional<std::size_t>{*began + std::min(offset, much)};
    };

    switch (what.key().code()) {
        case Code::enter: insert("\n"); return true;

        case Code::backspace: back_erase(behind(*kept.quilt, kept.cursor)); return true;

        case Code::remove: front_erase(ahead(*kept.quilt, kept.cursor)); return true;

        // Z klawiszem Ctrl strzałka idzie wyrazami, bez niego znakami. Znak, nie bajt: inaczej
        // strzałka stawałaby w środku ogonka.
        case Code::left:
            went(mods.ctrl ? worded(*kept.spool, kept.cursor, false)
                           : kept.cursor - behind(*kept.quilt, kept.cursor));
            return true;

        case Code::right:
            went(mods.ctrl ? worded(*kept.spool, kept.cursor, true)
                           : kept.cursor + ahead(*kept.quilt, kept.cursor));
            return true;

        case Code::up:
        case Code::down:
        case Code::pageup:
        case Code::pagedown: {
            const Code code = what.key().code();
            const bool upwards = code == Code::up || code == Code::pageup;

            const std::size_t much =
                code == Code::up || code == Code::down
                    ? 1
                    : static_cast<std::size_t>(std::max(shown(), std::size_t{1}));

            const std::size_t which = kept.spool->found(kept.cursor);

            // Wyżej niż pierwszy wiersz i niżej niż ostatni nie ma dokąd iść. **Oddajemy wtedy
            // klawisz przodkowi** zamiast go zjadać: karta albo lista wyżej ma prawo go użyć.
            if (upwards && which == 0) return false;

            const std::size_t want = upwards ? (much > which ? 0 : which - much) : which + much;
            const std::size_t offset = kept.offset;

            std::optional<std::size_t> where = along(want, offset);

            // Stronicowanie w dół pod koniec dokumentu ma dojść do ostatniego wiersza, a nie
            // odmówić ruchu — inaczej PageDown przestawałby działać na przedostatniej stronie.
            if (!where.has_value() && !upwards && much > 1) {
                const std::optional<std::size_t> lines = this->lines();

                if (lines.has_value() && *lines > 0 && which + 1 < *lines)
                    where = along(*lines - 1, offset);
            }

            if (!where.has_value()) return false;

            went(*where);

            // `stretch` zapisało nowe przesunięcie, a my chcemy pamiętać stare — inaczej kursor
            // schodząc po krótkim wierszu gubiłby swoją kolumnę na stałe.
            kept.offset = offset;

            return true;
        }

        case Code::home: {
            const std::size_t which = kept.spool->found(kept.cursor);

            went(kept.spool->at(which).value_or(kept.cursor));

            return true;
        }

        case Code::end: {
            const std::optional<std::size_t> where =
                along(kept.spool->found(kept.cursor), SIZE_MAX);

            if (!where.has_value()) return false;

            went(*where);

            return true;
        }

        default: break;
    }

    return Page::react(what);
}

// Kliknięcie stawia kursor tam, gdzie się kliknęło — a nie tylko bierze ognisko, jak w podglądzie.
// Rząd i kolumnę na wiersz i bajt przelicza kartka: rząd ekranu nie jest numerem wiersza, bo przy
// zawijaniu jeden wiersz zajmuje ich kilka, a kolumna nie jest bajtem, bo tabulator zajmuje osiem
// kratek. Liczone tu na piechotę dawało kursor w zupełnie innym miejscu.
bool Editor::react(ClickEvent& what) {
    Kept& kept = *kept_;

    if (root() != nullptr) focus();

    if (!kept.spool) return true;

    const std::optional<Place> where = struck(own(*this, what.where()));

    if (!where.has_value()) return true;

    const std::optional<std::size_t> began = kept.spool->at(where->line);

    if (!began.has_value()) return true;

    cursor(*began + where->byte);

    return true;
}

// **Zaznaczanie ciągnięciem.** Wciśnięcie stawia kursor razem z kotwicą, ruch z wciśniętym
// przyciskiem rozciąga zaznaczenie kotwicą w miejscu — to jest cała różnica między `cursor`
// a `stretch`. Terminal przysyła ruch z wciśniętym przyciskiem sam, bo `?1002` jest włączone;
// `?1003` byłby potrzebny dopiero do ruchu **bez** przycisku, czyli do podświetlania pod kursorem.
//
// `ClickEvent` zostaje bez zmian i nie kłóci się z tym: korzeń wystawia go tylko wtedy, gdy
// puszczenie padło w tej samej kratce co wciśnięcie — przy prawdziwym ciągnięciu nie powstaje
// wcale, a przy zwykłym kliknięciu stawia ten sam kursor, który już postawiło wciśnięcie.
//
// **Samoprzewijanie chodzi zdarzeniami, nie zegarem.** Ruch w skrajnym rzędzie przewija o wiersz,
// więc szybkość schodzenia bierze się z tego, ile użytkownik naprawdę rusza myszą — kiedy stanie,
// przewijanie staje razem z nim. Zegar dałby szybkość zależną od czasu i przewijałby dalej mimo
// nieruchomej myszy. Poza edytor i tak nie wyjdziemy: korzeń rozsyła po położeniu i to jest
// zamierzone, więc skrajny rząd jest ostatnim miejscem, w którym cokolwiek do nas dochodzi.
bool Editor::react(MouseEvent& what) {
    Kept& kept = *kept_;

    if (!kept.spool) return true;

    if (what.press() == Press::up) {
        kept.dragging = false;

        return true;
    }

    const Spot where = own(*this, what.where());

    const auto reached = [this, &kept, &where](bool anchored) {
        const std::optional<Place> place = struck(where);

        if (!place.has_value()) return;

        const std::optional<std::size_t> began = kept.spool->at(place->line);

        if (!began.has_value()) return;

        if (anchored)
            cursor(*began + place->byte);
        else
            stretch(*began + place->byte);
    };

    if (what.press() == Press::down) {
        if (what.button() != Mouse::left) return true;

        if (root() != nullptr) focus();

        reached(true);

        kept.dragging = true;

        return true;
    }

    if (!kept.dragging || what.button() == Mouse::none) return true;

    // Skrajny rząd przewija o wiersz, a dopiero potem czytamy, co pod myszą — bo po przewinięciu
    // stoi tam już co innego.
    const int high = area().size.high;

    if (high > 1) {
        if (where.y >= high - 1)
            slide(1);
        else if (where.y <= 0)
            slide(-1);
    }

    reached(false);

    return true;
}

// Dwuklik bierze wyraz. **Regułę „bajt wyrazu" bierzemy z `encoding.hpp`**, tę samą, po której
// grzebień szuka całych wyrazów — inaczej dwuklik i szukanie zaczęłyby się różnić na ogonkach.
//
// Klik w miejsce, które wyrazem nie jest, bierze ciąg takich samych: same spacje albo same znaki
// przestankowe. Zawsze coś zaznacza, więc dwuklik nigdy nie wygląda na zignorowany.
bool Editor::react(DoubleClickEvent& what) {
    Kept& kept = *kept_;

    if (root() != nullptr) focus();

    if (!kept.spool) return true;

    const std::optional<Place> where = struck(own(*this, what.where()));

    if (!where.has_value()) return true;

    const std::optional<std::size_t> began = kept.spool->at(where->line);

    if (!began.has_value()) return true;

    const std::vector<std::string> got = kept.spool->lines(where->line, 1);

    if (got.empty() || got.front().empty()) return true;

    const std::string& text = got.front();

    const std::size_t at = std::min(where->byte, text.size() - 1);

    const auto lettered = [](char one) {
        return wordly(static_cast<char32_t>(static_cast<unsigned char>(one)));
    };

    const bool kind = lettered(text[at]);

    std::size_t from = at;
    std::size_t upto = at + 1;

    while (from > 0 && lettered(text[from - 1]) == kind) --from;

    while (upto < text.size() && lettered(text[upto]) == kind) ++upto;

    select(*began + from, *began + upto);

    return true;
}

bool Editor::react(PasteEvent& what) {
    insert(what.said());

    return true;
}

}

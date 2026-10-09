#include "talcite/editor.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "talcite/clipboard.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

#include "eliding.hpp"
#include "quilt.hpp"
#include "spool.hpp"


namespace talcite::ui {

namespace {

// Rola zaznaczenia. Zaznaczenie **nie jest znacznikiem** — ma kotwicę i głowę, których znacznik
// nie ma — ale rysuje się nim, bo malowanie zakresu to dokładnie ta sama robota.
constexpr const char* selecting = "talcite.editor.select";

std::size_t counted(std::string_view what) {
    return static_cast<std::size_t>(std::count(what.begin(), what.end(), '\n'));
}


// Kończy grupę cofania. Spacja, tabulator i enter zamykają wyraz — dzięki temu jedno cofnięcie
// zabiera cały wyraz, a nie literę, i nie przeskakuje przy tym przez koniec wiersza.
bool sealed(char last) { return last == '\n' || last == ' ' || last == '\t'; }

}

struct Editor::Kept {
    // Jedna zmiana dokumentu: **co zniknęło, co przyszło i gdzie stał wtedy kursor**. Kołdra sama
    // się nie cofnie, więc odwrotność każdej zmiany trzeba zapamiętać osobno — ale jako skrawki
    // kołdry, nie bajty. Dawniej wpis trzymał kopię skasowanego tekstu, a cofnięcie wstawiało ją do
    // dopisków, które nigdy się nie kurczą: skasowanie gigabajtowego bloku z logu to był gigabajt
    // w dzienniku i kolejny przy każdej parze cofnij–ponów. Skrawek to kilka odcinków, więc pamięć
    // rośnie z liczbą zmian, nie z ich długością, a cofnięcie zszywa dokument z tych samych odcinków.
    // Jedno cięcie „zamień wszystkie": miejsce w dokumencie **sprzed** całej zmiany.
    struct Splice {
        std::size_t at = 0;
        std::string gone;
    };

    struct Change {
        std::size_t at = 0;
        Quilt::Scrap gone;
        Quilt::Scrap came;
        std::size_t was = 0;

        // „Zamień wszystkie" jednym wpisem: cięcia rosnąco, każde z tym, co zniknęło, a na każdym
        // miejscu przychodzi `with`. Pusty wektor znaczy zwykłą zmianę w jednym miejscu.
        std::vector<Splice> many;
        std::string with;

        // Cofa się razem z poprzednim wpisem. Stąd „zamień wszystkie" wraca jednym ruchem, choć
        // zmian było sto — bez trzymania w pamięci całego dokumentu sprzed zamiany.
        bool tied = false;

        // Blok mcedit sprzed zmiany i po niej: cofnięcie przywraca blok tam, gdzie był — także
        // po `erase()`, po którym bloku nie ma — a ponowienie tam, gdzie zmiana go zostawiła.
        std::optional<std::pair<std::size_t, std::size_t>> before;
        std::optional<std::pair<std::size_t, std::size_t>> after;
    };

    using Block = std::optional<std::pair<std::size_t, std::size_t>>;

    // Schowek podany z zewnątrz, żeby próby nie sięgały do systemowego, a program z kilkoma
    // edytorami dawał im jeden wspólny.
    Seam<Clipboard> clipboard;

    std::shared_ptr<Quilt> quilt;

    std::size_t cursor = 0;
    std::size_t anchor = 0;

    // **Blok mcedit** — trwały odcinek dokumentu obok zaznaczenia i schowka. W trakcie
    // rozciągania (`marking`) drugi koniec jest kursorem, więc `block->second` nie znaczy wtedy nic;
    // po zakończeniu oba końce stoją i idą tylko za zmianami dokumentu (`apply`). `held` to stan
    // sprzed bieżącej zmiany, przechwycony w `apply` dla `note`.
    Block block;
    bool marking = false;
    Block held;

    // Zapamiętane miejsce w wierszu dla strzałek w pionie. Trzymamy **bajt**, nie kolumnę: kolumna
    // wymagałaby powtórzenia tu reguły tabulatorów, którą zna już cięcie na kawałki. Skutek jest
    // taki, że przy wierszach z tabulatorami kursor w pionie potrafi się przesunąć — do poprawienia
    // wtedy, gdy zacznie przeszkadzać.
    std::size_t offset = 0;

    bool readonly = false;

    // Czy trwa zapis. Treść nie ma prawa zmienić się pod kawałkami, które właśnie płyną do ujścia,
    // więc do końca zapisu edytor jest tylko do czytania — ale bez ruszania `readonly`, które
    // należy do wołającego.
    bool saving = false;

    // Czy trwa ciągnięcie myszą. Ruch bez wciśniętego przycisku przychodzi tylko przy `?1003`,
    // którego nie włączamy, ale flaga i tak jest potrzebna: ruch po puszczeniu nad edytorem
    // nie ma prawa rozciągać zaznaczenia.
    bool dragging = false;

    std::vector<Change> log;

    // Ile wpisów z dziennika jest **wykonanych**. Wszystko za tym to gałąź do ponowienia, którą
    // pierwsza nowa zmiana utnie.
    std::size_t done = 0;

    // Ile ich było przy ostatnim zapisie. Stąd `dirty()`: cofnięcie wszystkiego z powrotem daje
    // dokument czysty, bo wraca do tego samego miejsca w dzienniku. Dwie pułapki tego rachunku:
    // wyraz pisany dalej po zapisie przedłużałby ten sam wpis, nie zmieniając numeru — dlatego
    // zapis pieczętuje wpis (`joining` gaśnie); a nowa zmiana po cofnięciu poniżej zapisanego
    // miejsca ucina gałąź, na której to miejsce leżało — wtedy numer idzie na `SIZE_MAX`, bo
    // zapisany stan nie jest już osiągalny żadnym cofaniem.
    std::size_t washed = 0;

    // Czy kolejny znak może przedłużyć ostatni wpis. Gaśnie przy każdym ruchu kursora, przy
    // cofaniu i przy ponawianiu — inaczej wyraz wpisany po przeskoku w inne miejsce dokleiłby się
    // do wyrazu sprzed przeskoku.
    bool joining = false;

    // Czy następny wpis ma być przywiązany do poprzedniego.
    bool tying = false;

    [[nodiscard]] std::string text(std::size_t from, std::size_t upto) const;

    void apply(Spool& spool, std::size_t at, const Quilt::Scrap& gone, const Quilt::Scrap& came);

    void splice(Spool& spool, const std::vector<Splice>& many, const std::string& came, bool back);

    [[nodiscard]] static std::size_t ended(const std::vector<Splice>& many, const std::string& came);

    void note(std::size_t at, Quilt::Scrap gone, Quilt::Scrap came, std::size_t was);

    void blocked();
};

// Zakończenie rozciągania bloku: końce w porządku, blok zerowej długości znika — pusty blok
// byłby tylko śmieciem na ekranie. Wołane przez drugie F3 i przez każdą z trzech czynności.
void Editor::Kept::blocked() {
    if (!marking) return;

    marking = false;

    if (!block.has_value()) return;

    const std::size_t from = std::min(block->first, cursor);
    const std::size_t upto = std::max(block->first, cursor);

    if (upto <= from) block.reset();
    else block = std::pair{from, upto};
}

std::string Editor::Kept::text(std::size_t from, std::size_t upto) const {
    if (!quilt || upto <= from) return {};

    return talcite::text(quilt->bytes(from, upto - from));
}

namespace {

// Koniec bloku po zmianie [at, at + gone) → came. Początek: zmiana przed nim albo dokładnie
// w nim przesuwa go (tekst wstawiony na początku bloku idzie **przed** blok), zmiana w środku
// stawia go za wstawionym. Koniec: zmiana za nim albo dokładnie w nim nic (tekst wstawiony na
// końcu bloku nie wchodzi do bloku), zmiana przed nim przesuwa, w środku przycina do `at`.
[[nodiscard]] std::size_t shifted(std::size_t bound, bool start, std::size_t at, std::size_t gone,
                                  std::size_t came) {
    if (start ? bound < at : bound <= at) return bound;

    if (bound >= at + gone) return bound - gone + came;

    return start ? at + came : at;
}

}

// **Jedyne miejsce, w którym dokument naprawdę się zmienia.** Wpisanie, kasowanie, cofnięcie
// i ponowienie sprowadzają się tutaj, więc poprawka dla szpuli jest napisana raz — a to ona
// decyduje, czy ekran po zmianie pokaże nową treść.
void Editor::Kept::apply(Spool& spool, std::size_t at, const Quilt::Scrap& gone, const Quilt::Scrap& came) {
    // Blok idzie za dokumentem: oba końce według `shifted`, a gdy nic z niego nie zostało —
    // znika. W trakcie rozciągania przesuwa się tylko początek, koniec jest kursorem.
    held = block;

    if (block.has_value()) {
        block->first = shifted(block->first, true, at, gone.much, came.much);

        if (!marking) {
            block->second = shifted(block->second, false, at, gone.much, came.much);

            if (block->second <= block->first) block.reset();
        }
    }

    // Ostatni bajt dokumentu rozstrzyga, czy ostatni wiersz w ogóle istnieje: „abc" to jeden
    // wiersz, „abc\n" **też jeden**, a pusty dokument nie ma żadnego. Sama liczba enterów tego
    // nie powie, więc trzeba zajrzeć na koniec przed zmianą i po niej.
    const auto ending = [this](std::size_t much) {
        if (much == 0) return '\0';

        const std::string one = talcite::text(quilt->bytes(much - 1, 1));

        return one.empty() ? '\0' : one.front();
    };

    const std::size_t was = quilt->whole();
    const char before = ending(was);

    quilt->erase(at, gone.much);
    quilt->put(at, came);

    const std::size_t now = quilt->whole();
    const char after = ending(now);

    std::ptrdiff_t lines = static_cast<std::ptrdiff_t>(quilt->lines(came)) -
                           static_cast<std::ptrdiff_t>(quilt->lines(gone));

    if (now > 0 && after != '\n') ++lines;

    if (was > 0 && before != '\n') --lines;

    spool.changed(at, gone.much, came.much, lines);
}

// Koniec ostatniej wstawki po całej zmianie: jej miejsce sprzed zmiany przesunięte o wszystkie
// wcześniejsze cięcia.
std::size_t Editor::Kept::ended(const std::vector<Splice>& many, const std::string& came) {
    std::ptrdiff_t moved = 0;

    for (std::size_t one = 0; one + 1 < many.size(); ++one)
        moved += static_cast<std::ptrdiff_t>(came.size()) - static_cast<std::ptrdiff_t>(many[one].gone.size());

    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(many.back().at) + moved) + came.size();
}

// **Wiele miejsc naraz** — „zamień wszystkie" i jego cofnięcie w jednym przejściu po dokumencie.
// W przód: na miejscu każdego `gone` staje `came`; wstecz: na miejscu każdego `came` (już
// przesuniętego o zmiany przed nim) wraca jego `gone`. W przód blok idzie za wszystkimi cięciami po
// kolei, od lewej, każde w miejscu już przesuniętym przez poprzednie — tak, jakby szły jedno po
// drugim; wstecz bloku nie liczymy, bo cofnięcie i tak stawia go z wpisu tam, gdzie był. Szpula
// dostaje jedno zawiadomienie od pierwszego cięcia, bo dalej każde przesuwa inaczej.
void Editor::Kept::splice(Spool& spool, const std::vector<Splice>& many, const std::string& came, bool back) {
    if (many.empty()) return;

    held = block;

    std::vector<Quilt::Cut> cuts;
    cuts.reserve(many.size());

    std::ptrdiff_t moved = 0;
    std::ptrdiff_t lines = 0;

    const std::ptrdiff_t newlines = static_cast<std::ptrdiff_t>(counted(came));

    for (const Splice& one : many) {
        const std::size_t now = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one.at) + (back ? moved : 0));
        const std::size_t gone = back ? came.size() : one.gone.size();
        const std::size_t comes = back ? one.gone.size() : came.size();

        cuts.push_back(Quilt::Cut{now, gone, back ? std::string_view{one.gone} : std::string_view{came}});

        // Cięcia po kolei od lewej, w dokumencie zmienianym na bieżąco: każde stoi o sumę
        // wcześniejszych różnic dalej niż przed zmianą.
        if (block.has_value() && !back) {
            const std::size_t here = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one.at) + moved);

            block->first = shifted(block->first, true, here, gone, comes);

            if (!marking) block->second = shifted(block->second, false, here, gone, comes);
        }

        const std::ptrdiff_t step = static_cast<std::ptrdiff_t>(came.size()) - static_cast<std::ptrdiff_t>(one.gone.size());

        moved += step;

        const std::ptrdiff_t away = static_cast<std::ptrdiff_t>(counted(one.gone));

        lines += back ? away - newlines : newlines - away;
    }

    if (block.has_value() && !marking && block->second <= block->first) block.reset();

    const auto ending = [this](std::size_t much) {
        if (much == 0) return '\0';

        const std::string one = talcite::text(quilt->bytes(much - 1, 1));

        return one.empty() ? '\0' : one.front();
    };

    const std::size_t was = quilt->whole();
    const char before = ending(was);

    quilt->spliced(cuts);

    const std::size_t now = quilt->whole();
    const char after = ending(now);

    if (now > 0 && after != '\n') ++lines;

    if (was > 0 && before != '\n') --lines;

    spool.rewritten(cuts.front().at, static_cast<std::ptrdiff_t>(now) - static_cast<std::ptrdiff_t>(was), lines);
}

// Nowa zmiana ucina gałąź ponowienia — po cofnięciu i wpisaniu czegoś innego nie ma już dokąd
// ponawiać. Poza tym **pisany wyraz zbiera się w jeden wpis**: znak dopisany dokładnie tam, gdzie
// skończył się poprzedni, przedłuża ostatni zamiast robić nowy. Bez tego cofanie chodziłoby po
// literze i pięć tysięcy znaków dałoby pięć tysięcy kroków.
void Editor::Kept::note(std::size_t at, Quilt::Scrap gone, Quilt::Scrap came, std::size_t was) {
    if (washed > done) washed = SIZE_MAX;

    log.resize(done);

    if (joining && !log.empty()) {
        Change& last = log.back();

        const bool typing = gone.much == 0 && came.much > 0 && last.gone.much == 0 &&
                            last.at + last.came.much == at && !sealed(quilt->last(last.came)) &&
                            quilt->lines(came) == 0;

        if (typing) {
            Quilt::joined(last.came, came, false);
            last.after = block;

            return;
        }

        const bool erasing = came.much == 0 && gone.much > 0 && last.came.much == 0 && last.gone.much > 0;

        // Trzymany Backspace zbiera się tak samo, tyle że od tyłu: każdy kolejny znak znika
        // **przed** poprzednim, więc początek wpisu się cofa, a skrawek rośnie z przodu — w źródle
        // znak leży tuż przed poprzednim, więc odcinek się wydłuża. Dawniej wpis trzymał bajty
        // i trzeba było je dopisywać odwrócone, żeby nie przepisywać wpisu przy każdym znaku.
        if (erasing && at + gone.much == last.at && quilt->lines(gone) == 0) {
            Quilt::joined(last.gone, gone, true);
            last.at = at;
            last.after = block;

            return;
        }

        // Trzymany Delete zbiera od przodu: kasowany bajt jest wciąż ten sam.
        if (erasing && at == last.at && quilt->lines(gone) == 0) {
            Quilt::joined(last.gone, gone, false);
            last.after = block;

            return;
        }
    }

    Change made;
    made.at = at;
    made.gone = std::move(gone);
    made.came = std::move(came);
    made.was = was;
    made.tied = tying;
    made.before = held;
    made.after = block;

    log.push_back(std::move(made));

    done = log.size();
    joining = true;
}

Editor::Editor() : Editor(clipboard()) {}

Editor::Editor(Seam<Clipboard> with) : kept_(std::make_unique<Kept>()), selection_(*this), block_(*this) {
    kept_->clipboard = std::move(with);

    role("talcite.editor");
}

Editor::Editor(ScrollShape shape, Seam<Clipboard> with)
    : Page(shape), kept_(std::make_unique<Kept>()), selection_(*this), block_(*this) {
    kept_->clipboard = std::move(with);

    role("talcite.editor");
}

Editor::~Editor() = default;

namespace {

// Kołdra jest w pamięci i odpowiada od ręki, więc szpula edytora dostaje jej wewnętrzny szew
// `Spool::Peek`, nie `Feed`: edytor czyta bajty przy każdym ruchu kursora i nie ma jak czekać
// na okno. Dlatego też edytor przyjmuje dokument tylko bajtami.
Spool::Peek fed(const std::shared_ptr<Quilt>& quilt) {
    return [quilt](std::size_t at, std::size_t most) { return quilt->bytes(at, most); };
}

}

// **Kołdra pod szpulą.** Szpula czyta bajty przez tablicę kawałków, więc widzi dokument razem
// ze zmianami, a oryginału nikt nie wczytuje.
void Editor::hold(Bytes given) {
    kept_->quilt = std::make_shared<Quilt>(std::move(given));

    kept_->cursor = 0;
    kept_->anchor = 0;
    kept_->offset = 0;
    kept_->block.reset();
    kept_->marking = false;

    // Nowy dokument to nowy dziennik: cofnięcie do stanu poprzedniego pliku nie miałoby sensu,
    // a `dirty()` musi zacząć od czystego.
    kept_->log.clear();
    kept_->done = 0;
    kept_->washed = 0;
    kept_->joining = false;
    kept_->tying = false;
    kept_->saving = false;

    Page::hold(std::make_shared<Spool>(fed(kept_->quilt), kept_->quilt->whole()));

    // Dokument w pamięci liczymy od razu — to przejście po tym, co i tak trzymamy.
    spool()->counting();
}

bool Editor::readonly() const { return kept_->readonly; }

void Editor::readonly(bool given) { kept_->readonly = given; }

std::size_t Editor::cursor() const { return kept_->cursor; }

// Kursor jako bajt dokumentu, a wiersz i przesunięcie w nim wylicza szpula. Odwrotnie byłoby
// gorzej: zmiany adresuje się bajtem, a wiersze zmieniają numerację przy każdym enterze.
//
// Postawienie kursora **zabiera ze sobą kotwicę** — czyli gasi zaznaczenie. Ruch z klawiszem
// Shift kotwicy nie rusza i dlatego idzie przez `Selection::extend`; poza tym jednym obie drogi są
// tą samą robotą, więc jedna woła drugą.
void Editor::cursor(std::size_t given) {
    Kept& kept = *kept_;

    if (!spool()) return;

    kept.anchor = std::min(given, spool()->whole());

    selection_.extend(given);
}

// --- Zaznaczenie ---------------------------------------------------------------------------------
//
// **Zaznaczenie to odcinek od kotwicy do kursora**, i nic więcej: kursor jest jego końcem, więc
// ruch kursora rusza zaznaczenie, a postawienie kursora (`Editor::cursor`) je gasi. Podobiekt nic
// nie trzyma — patrzy w `Kept` edytora przez właściciela, tak samo jak blok. Dawniej było to siedem
// metod edytora (`anchor`, `selected`, `select`, `stretch`, `unselect`, `selection`) i kasowanie
// ukryte w `back_erase(0)`, którego zero po cichu znaczyło „zaznaczenie".
Editor::Selection::Selection(Editor& owner) : owner_(owner) {}

std::size_t Editor::Selection::anchor() const { return owner_.kept_->anchor; }

std::size_t Editor::Selection::from() const { return std::min(owner_.kept_->anchor, owner_.kept_->cursor); }

std::size_t Editor::Selection::upto() const { return std::max(owner_.kept_->anchor, owner_.kept_->cursor); }

bool Editor::Selection::empty() const { return owner_.kept_->cursor == owner_.kept_->anchor; }

std::string Editor::Selection::text() const {
    const Kept& kept = *owner_.kept_;

    if (!kept.quilt || empty()) return {};

    return talcite::text(kept.quilt->bytes(from(), upto() - from()));
}

// Parametry nazwane tym, czym są: `set(5, 2)` zaznacza wstecz, kursor staje na dwójce, a `from()`
// i `upto()` i tak oddają 2 i 5.
void Editor::Selection::set(std::size_t anchor, std::size_t cursor) {
    Kept& kept = *owner_.kept_;

    if (!owner_.spool()) return;

    const std::size_t much = owner_.spool()->whole();

    kept.anchor = std::min(anchor, much);
    kept.cursor = std::min(cursor, much);
    kept.joining = false;

    owner_.invalidate();
    (void)owner_.trigger(ScrollEvent{});
}

void Editor::Selection::clear() {
    owner_.kept_->anchor = owner_.kept_->cursor;

    owner_.invalidate();
}

// Kasowanie zaznaczenia jako **jedna zmiana w dzienniku**, i tak samo dla Backspace, Delete, wycięcia
// i zamiany na nic. Fałsz, gdy nie ma czego kasować albo nie wolno.
bool Editor::Selection::erase() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt || empty()) return false;

    const std::size_t at = from();
    const Quilt::Scrap gone = kept.quilt->taken(at, upto() - at);

    kept.apply(*owner_.spool(), at, gone, {});
    kept.note(at, gone, {}, kept.cursor);

    owner_.settled(at);

    return true;
}

// Kursor idzie, kotwica stoi — ruch z klawiszem Shift i ciągnięcie myszą.
void Editor::Selection::extend(std::size_t to) {
    Editor& owner = owner_;
    Kept& kept = *owner.kept_;

    if (!owner.spool()) return;

    const std::size_t want = std::min(to, owner.spool()->whole());

    kept.cursor = want;

    // Ruch kursora zamyka grupę cofania: wyraz wpisany po przeskoku w inne miejsce nie ma nic
    // wspólnego z wyrazem sprzed przeskoku.
    kept.joining = false;

    const std::size_t which = owner.spool()->found(want);

    kept.offset = want - owner.spool()->at(which).value_or(want);

    (void)owner.reveal(which);

    // Zaznaczenie rysuje się znacznikami składanymi przy każdym malowaniu, ale sam obraz trzeba
    // złożyć od nowa — inaczej przesunięty kursor nie zmieniłby nic na ekranie.
    owner.invalidate();
    (void)owner.trigger(ScrollEvent{});
}

std::size_t Editor::line() const {
    if (!spool()) return 0;

    return spool()->found(kept_->cursor);
}

// Skok do wiersza — para do `line()`: kursor na początek wiersza `which` (od zera), zaznaczenie
// gaśnie, widok go pokazuje. Za ostatnim wierszem staje na ostatnim, bo „idź do 9999" w pliku
// na sto wierszy ma dojść do końca, a nie odmówić. Kursor liczy się w bajtach, a bajt początku
// wiersza zna tylko szpula — stąd metoda tutaj, nie w wołającym.
void Editor::line(std::size_t which) {
    Kept& kept = *kept_;

    if (!spool()) return;

    const std::size_t many = spool()->counted().value_or(0);

    which = many == 0 ? 0 : std::min(which, many - 1);

    cursor(spool()->at(which).value_or(spool()->whole()));
}

// Kolumna liczona w **kratkach**, nie w bajtach: pasek stanu pokazuje to, co widać, a tabulator
// widać jako osiem miejsc. Bajt oddaje `cursor()`, więc obie miary są do wzięcia — tak samo jak
// przy pasku przewijania, który chodzi raz po wierszach, a raz po bajtach.
std::size_t Editor::column() const {
    Kept& kept = *kept_;

    if (!spool()) return 0;

    const std::size_t which = spool()->found(kept.cursor);
    const std::optional<std::size_t> began = spool()->at(which);

    if (!began.has_value() || kept.cursor < *began) return 0;

    const std::optional<std::vector<std::string>> got = spool()->lines(which, 1);

    if (!got.has_value() || got->empty()) return 0;

    return static_cast<std::size_t>(columns(got->front(), kept.cursor - *began));
}

// **Cały dokument wprost.** Dawniej wołający zaznaczał wszystko, czytał zaznaczenie i przywracał
// kursor — trzy miejsca, dwa z nich w twa.
std::string Editor::text() const {
    const Kept& kept = *kept_;

    if (!kept.quilt) return {};

    return kept.text(0, kept.quilt->whole());
}

// --- Schowek -------------------------------------------------------------------------------------

// **Schowek nie jest edytora.** Edytor nie trzyma tu żadnej treści — dostaje `Clipboard` przy
// budowie, a domyślny `clipboard()` to drzwi do schowka systemu, na POSIX-ie do schowka maszyny,
// przy której siedzi użytkownik, przez terminal.
//
// Kopiowanie wolno także w dokumencie tylko do odczytu: niczego nie zmienia.
bool Editor::copy() {
    if (selection_.empty()) return false;

    return kept_->clipboard.provider().copy(selection_.text());
}

// Wycięcie idzie przez to samo kasowanie zaznaczenia, co Backspace, więc jest **jedną zmianą
// w dzienniku** i wraca jednym cofnięciem. Gdy skopiowanie się nie uda, nie kasujemy: dokument
// zostałby bez tekstu, którego nie ma dokąd wkleić.
bool Editor::cut() {
    if (kept_->readonly || kept_->saving || selection_.empty() || !copy()) return false;

    return selection_.erase();
}

bool Editor::paste() {
    if (kept_->readonly || kept_->saving) return false;

    const std::optional<std::string> what = kept_->clipboard.provider().paste();

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

    std::vector<Mark> made;

    if (spool()) {
        const std::size_t many = static_cast<std::size_t>(std::max(given.high, 1));

        // Blok tą samą rolą, co zaznaczenie — w trakcie rozciągania widać go rosnącego z kursorem.
        if (!block_.empty()) {
            const std::size_t from = std::min(block_.from(), block_.upto());
            const std::size_t upto = std::max(block_.from(), block_.upto());

            made = spanned(*spool(), from, upto, topmost(), many);
        }

        if (kept.cursor != kept.anchor) {
            const std::size_t from = std::min(kept.cursor, kept.anchor);
            const std::size_t upto = std::max(kept.cursor, kept.anchor);

            for (Mark& one : spanned(*spool(), from, upto, topmost(), many)) made.push_back(std::move(one));
        }
    }

    if (!made.empty()) marks().all(std::move(made));
    else if (!marks().empty()) marks().clear();

    return Page::paint(given, seen);
}

std::optional<Spot> Editor::caret() const {
    Kept& kept = *kept_;

    if (!spool()) return {};

    const std::size_t which = spool()->found(kept.cursor);
    const std::optional<std::size_t> began = spool()->at(which);

    if (!began.has_value()) return {};

    return spot_of(Place{which, kept.cursor - *began});
}

// Kursor po zmianie: wiersz przelicza szpula, a obraz i tak trzeba złożyć od nowa. Osobno, bo
// robią to wszystkie cztery drogi zmiany — wpisanie, oba kasowania, cofnięcie i ponowienie.
void Editor::settled(std::size_t where) {
    Kept& kept = *kept_;

    kept.cursor = std::min(where, spool()->whole());
    kept.anchor = kept.cursor;

    const std::size_t which = spool()->found(kept.cursor);

    kept.offset = kept.cursor - spool()->at(which).value_or(kept.cursor);

    (void)reveal(which);

    invalidate();
    (void)trigger(ScrollEvent{});
}

// Wpisanie **na zaznaczeniu jest jedną zmianą**, nie kasowaniem i wpisaniem po nim: inaczej jedno
// cofnięcie oddawałoby pusty dokument zamiast tego, co było zaznaczone.
void Editor::insert(std::string what) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt || what.empty()) return;

    const std::size_t from = std::min(kept.cursor, kept.anchor);
    const std::size_t upto = std::max(kept.cursor, kept.anchor);

    const Quilt::Scrap gone = kept.quilt->taken(from, upto - from);
    const Quilt::Scrap came = kept.quilt->stored(what);
    const std::size_t was = kept.cursor;

    kept.apply(*spool(), from, gone, came);
    kept.note(from, gone, came, was);

    settled(from + what.size());
}

void Editor::back_erase(std::size_t much) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return;

    // Przy zaznaczeniu Backspace kasuje zaznaczenie, ile by nie kazano.
    if (selection_.erase()) return;

    const std::size_t upto = kept.cursor;
    const std::size_t from = much >= kept.cursor ? 0 : kept.cursor - much;

    if (upto <= from) return;

    const Quilt::Scrap gone = kept.quilt->taken(from, upto - from);

    kept.apply(*spool(), from, gone, {});
    kept.note(from, gone, {}, kept.cursor);

    settled(from);
}

void Editor::front_erase(std::size_t much) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return;

    if (selection_.erase()) return;

    const std::size_t whole = kept.quilt->whole();

    if (kept.cursor >= whole) return;

    const std::size_t took = std::min(much, whole - kept.cursor);

    if (took == 0) return;

    const std::size_t at = kept.cursor;
    const Quilt::Scrap gone = kept.quilt->taken(at, took);

    kept.apply(*spool(), at, gone, {});
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

    if (!spool() || !how.ready()) return {};

    const std::size_t at = std::min(from, spool()->whole());
    const std::size_t which = spool()->found(at);

    // Wiersze od podanego numeru razem z bajtem, na którym każdy się zaczyna.
    const auto batch = [this](std::size_t first, std::size_t most) {
        std::vector<std::pair<std::size_t, std::string>> made;

        const std::optional<std::size_t> began = spool()->at(first);

        if (!began.has_value()) return made;

        std::size_t where = *began;

        for (std::string& one : spool()->lines(first, most).value_or(std::vector<std::string>{})) {
            const std::size_t size = one.size();

            made.emplace_back(where, std::move(one));

            where += size + 1;
        }

        return made;
    };

    const auto picked = [this](std::size_t began, Hit one) {
        selection_.set(began + one.from, began + one.upto);

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

namespace {

// Wpisanie pustki nie jest zmianą, więc zamiana na nic idzie przez kasowanie zaznaczenia.
void put(Editor& one, const std::string& with) {
    if (with.empty())
        (void)one.selection().erase();
    else
        one.insert(with);
}

}

// **Zamiana jednego trafienia**: bieżące zaznaczenie, jeśli samo jest dopasowaniem, i pokazanie
// następnego — tak, jak robi to każdy edytor: raz naciśnięty klawisz zamienia to, co świeci,
// i przechodzi dalej. Gdy zaznaczenie nie jest trafieniem, tylko szuka następnego. Prawda, gdy
// coś zamieniło. Dawniej był to jeden `replace(…, bool all)` z dwoma algorytmami za przełącznikiem.
bool Editor::replace(const Comb& how, const std::string& with) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt || !how.ready()) return false;

    if (!selection_.empty()) {
        const std::string got = selection_.text();
        const std::optional<Hit> one = how.forth(got, 0);

        if (one.has_value() && one->from == 0 && one->upto == got.size()) {
            const std::size_t began = selection_.from();

            put(*this, with);

            (void)find(how, began + with.size(), false);

            return true;
        }
    }

    (void)find(how, kept.cursor, false);

    return false;
}

// **Zamiana wszystkich**: trafienia zbiera jedno przejście po dokumencie sprzed zamiany — bez
// zaznaczania i bez zdarzeń — a potem jedna zmiana stawia `with` na miejscu każdego z nich, jednym
// wpisem, który cofa się jednym ruchem. Dawniej każde trafienie szło osobno przez szukanie,
// zaznaczenie, wstawienie i zawiadomienie szpuli, a kołdra przebudowywała się przy każdym — przy stu
// tysiącach trafień był to kwadrat, i tak samo przy cofnięciu. Szuka się w tekście sprzed zamiany,
// więc wstawka nigdy nie jest przeszukiwana: wzorzec ukryty w niej nie zapętli zamiany. Oddaje, ile
// zamieniło.
std::size_t Editor::replace_all(const Comb& how, const std::string& with) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt || !how.ready() || !spool()) return 0;

    std::vector<Kept::Splice> many;

    for (std::size_t line = 0;;) {
        const std::optional<std::size_t> began = spool()->at(line);

        if (!began.has_value()) break;

        const std::vector<std::string> got = spool()->lines(line, combing).value_or(std::vector<std::string>{});

        if (got.empty()) break;

        std::size_t where = *began;

        // Grzebień pustych trafień nie oddaje nigdy (pomija je sam), więc każde trafienie przesuwa
        // szukanie naprzód.
        for (const std::string& text : got) {
            for (std::size_t start = 0; start <= text.size();) {
                const std::optional<Hit> one = how.forth(text, start);

                if (!one.has_value()) break;

                many.push_back(Kept::Splice{where + one->from, text.substr(one->from, one->upto - one->from)});

                start = one->upto;
            }

            where += text.size() + 1;
        }

        line += got.size();
    }

    if (many.empty()) return 0;

    const std::size_t was = kept.cursor;

    kept.marking = false;
    kept.splice(*spool(), many, with, false);

    if (kept.washed > kept.done) kept.washed = SIZE_MAX;

    kept.log.resize(kept.done);

    Kept::Change made;
    made.at = many.front().at;
    made.with = with;
    made.was = was;
    made.many = std::move(many);
    made.before = kept.held;
    made.after = kept.block;

    const std::size_t count = made.many.size();
    const std::size_t last = Kept::ended(made.many, with);

    kept.log.push_back(std::move(made));
    kept.done = kept.log.size();
    kept.joining = false;

    settled(last);

    return count;
}

// --- Cofanie -------------------------------------------------------------------------------------

bool Editor::dirty() const { return kept_->done != kept_->washed; }

// Czysty od teraz — i zapieczętowany: znak wpisany po zapisie zaczyna nowy wpis, inaczej
// dokładałby się do ostatniego, nie zmieniając numeru, i `dirty()` mówiłoby „czysty" o tekście,
// który jest inny niż na dysku.
void Editor::clean() {
    kept_->washed = kept_->done;
    kept_->joining = false;
}

namespace {

// Ile bajtów naraz idzie do ujścia. Tyle samo, co okno szpuli — dokument bywa większy od pamięci,
// więc przez zapis też musi przepłynąć kawałkami.
constexpr std::size_t pouring = 64 * 1024;

}

// **Biblioteka nie dotyka plików.** Ujście daje aplikacja, a edytor daje treść — kawałkami przez
// pulę. Każdy kawałek czyta się z kołdry na wątku głównym (to pamięć, mikrosekundy) i idzie do
// ujścia obietnicą; następny czyta się dopiero po jej spełnieniu, więc przez cały zapis biegnie
// jedno pisanie, w kolejności. **Koniec dokumentu ujście widzi jako pusty kawałek** — ostatnie
// wołanie, po którym nic już nie przyjdzie; pusty dokument to samo jedno wołanie. Ujście domyka
// wtedy to, co ma domknąć — plik, przemianowanie — a dziennik pierze się dopiero po spełnieniu
// **tej** obietnicy: „czysty" znaczy wtedy „naprawdę na dysku", a nie „oddane ujściu". Dawny
// zapis prał dziennik przy oddaniu treści, a gdy ujście obiecywało zamiast pisać, edytor mówił
// „czysty" o pliku, którego jeszcze nie było. Odmowa ujścia w dowolnym miejscu zostawia dziennik
// brudny, `busy()` gaśnie, a wołający dostaje odrzucenie ze zdaniem ujścia — niedokończony
// zapis nie jest zapisem.
//
// W trakcie edytor jest zajęty: klawisze zmieniające treść odbijają się, żeby kawałki, które już
// popłynęły, i te, które dopiero pójdą, były jednym dokumentem. Drugi zapis w trakcie pierwszego
// i zapis bez ujścia są odrzucane od razu — to pomyłka wołającego, nie kłopot z dyskiem.
// Aplikacja pisze obok i przemianowuje, jak zawsze.
//
// Łańcuch trzyma edytor słabo: gdy edytor zginie w trakcie, kawałek, który właśnie wrócił, nie
// ma już skąd czytać następnego i zapis kończy się odrzuceniem, a nie dotknięciem zwolnionej
// pamięci.
Promise<void> Editor::save(Drain into) {
    Kept& kept = *kept_;

    if (!kept.quilt || !into || kept.saving)
        return Promise<void>{[](auto, auto reject) {
            reject(std::make_exception_ptr(std::logic_error{"The editor cannot save now."}));
        }};

    kept.saving = true;

    const std::size_t whole = kept.quilt->whole();
    const std::weak_ptr<Node> weak = weak_from_this();

    // Ogniwo wołające samo siebie: `then` spłaszcza obietnicę oddaną z ciągu dalszego, więc
    // każde ogniwo oddaje obietnicę reszty zapisu, a pierwsze — obietnicę całości. **Ogniwo trzyma
    // siebie słabo**; mocno trzyma je tylko ciąg dalszy czekającego kawałka. Mocne wskazanie na
    // samego siebie byłoby cyklem, który nie pęka — i każdy zapis zostawiałby w pamięci ujście
    // razem ze wszystkim, co ono trzyma.
    const auto step = std::make_shared<std::function<Promise<void>(std::size_t)>>();
    const std::weak_ptr<std::function<Promise<void>(std::size_t)>> self = step;

    *step = [weak, into, whole, self](std::size_t at) -> Promise<void> {
        const std::shared_ptr<Node> held = weak.lock();
        auto* mine = dynamic_cast<Editor*>(held.get());

        if (mine == nullptr || !mine->kept_->quilt)
            return Promise<void>{[](auto, auto reject) {
                reject(std::make_exception_ptr(std::runtime_error{"The editor is gone."}));
            }};

        if (at >= whole) return into(Bytes{});

        Bytes got = mine->kept_->quilt->bytes(at, pouring);

        // Kołdra obiecała więcej, niż oddaje. Lepiej powiedzieć „nie zapisano" niż zapisać ogryzek.
        if (got.empty())
            return Promise<void>{[](auto, auto reject) {
                reject(std::make_exception_ptr(std::runtime_error{"The document ended early."}));
            }};

        const std::size_t next = at + got.size();

        return into(std::move(got)).here().then([step = self.lock(), next] { return (*step)(next); });
    };

    // Pierwszy kawałek idzie na wątku wołającego: kołdra albo ujście, które rzuci, zanim odda
    // obietnicę, nie może zostawić edytora zajętym na zawsze — wyjątek staje się odrzuceniem.
    std::optional<Promise<void>> first;

    try {
        first.emplace((*step)(0));
    } catch (...) {
        kept.saving = false;

        return Promise<void>{[why = std::current_exception()](auto, auto reject) { reject(why); }};
    }

    return std::move(*first)
        .here()
        .then([weak] {
            if (auto* mine = dynamic_cast<Editor*>(weak.lock().get()); mine != nullptr) {
                mine->kept_->saving = false;
                mine->clean();
            }
        })
        .fail([weak](const std::exception&) {
            if (auto* mine = dynamic_cast<Editor*>(weak.lock().get()); mine != nullptr)
                mine->kept_->saving = false;
        });
}

bool Editor::busy() const { return kept_->saving; }

bool Editor::undoable() const { return !kept_->readonly && !kept_->saving && kept_->done > 0; }

bool Editor::redoable() const { return !kept_->readonly && !kept_->saving && kept_->done < kept_->log.size(); }

// Cofnięcie to ta sama zmiana czytana od tyłu: wraca to, co zniknęło, znika to, co przyszło.
// Kursor staje tam, gdzie stał **przed** zmianą — bo to miejsce, do którego wraca dokument.
void Editor::undo() {
    Kept& kept = *kept_;

    if (!undoable()) return;

    std::size_t where = kept.cursor;

    // Wpisy przywiązane cofają się razem — „zamień wszystkie" ma wracać jednym ruchem.
    kept.marking = false;

    while (kept.done > 0) {
        Kept::Change& one = kept.log[kept.done - 1];

        if (one.many.empty()) kept.apply(*spool(), one.at, one.came, one.gone);
        else kept.splice(*spool(), one.many, one.with, true);
        kept.block = one.before;

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

    kept.marking = false;

    while (kept.done < kept.log.size()) {
        Kept::Change& one = kept.log[kept.done];

        if (one.many.empty()) kept.apply(*spool(), one.at, one.gone, one.came);
        else kept.splice(*spool(), one.many, one.with, false);

        kept.block = one.after;

        ++kept.done;
        where = one.many.empty() ? one.at + one.came.much : Kept::ended(one.many, one.with);

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
    const std::string what = talcite::text(quilt.bytes(at - look, look));

    std::size_t much = 1;

    while (much < what.size() &&
           (static_cast<unsigned char>(what[what.size() - much]) & 0xC0) == 0x80)
        ++much;

    return much;
}

std::size_t ahead(Quilt& quilt, std::size_t at) {
    const std::string what = talcite::text(quilt.bytes(at, 4));

    if (what.empty()) return 0;

    const std::size_t much = utf8::trailing(static_cast<unsigned char>(what.front())) + 1;

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

    const std::vector<std::string> got = spool.lines(which, 1).value_or(std::vector<std::string>{});
    const std::string text = got.empty() ? std::string{} : got.front();

    std::size_t off = std::min(at - *began, text.size());

    const auto lettered = [&text](std::size_t one) {
        return in_word(static_cast<char32_t>(static_cast<unsigned char>(text[one])));
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

        const std::vector<std::string> before = spool.lines(which - 1, 1).value_or(std::vector<std::string>{});

        return *prev + (before.empty() ? 0 : before.front().size());
    }

    while (off > 0 && !lettered(off - 1)) --off;

    while (off > 0 && lettered(off - 1)) --off;

    return *began + off;
}

}

bool Editor::handle(KeyEvent& what) {
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
    // uchwyt widzi klawisz niezależnie od tego, co robi `handle`.
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
        insert(utf8::encode(what.key().letter()));

        return true;
    }

    // Ruch z Shiftem ciągnie zaznaczenie, bez Shiftu je gasi. Rozgałęzienie stoi w jednym miejscu,
    // bo docelowy bajt każdy ruch liczy sam, a różnica między nimi jest tylko taka jedna.
    const auto went = [this, &mods](std::size_t where) {
        if (mods.shift)
            selection_.extend(where);
        else
            cursor(where);
    };

    // Wiersz o zadanym numerze i jego początek — potrzebne i pionowym strzałkom, i klawiszom
    // stronicowym, i Home z End.
    const auto along = [this](std::size_t which, std::size_t offset) {
        const std::optional<std::size_t> began = spool()->at(which);

        if (!began.has_value()) return std::optional<std::size_t>{};

        const std::vector<std::string> got = spool()->lines(which, 1).value_or(std::vector<std::string>{});
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
            went(mods.ctrl ? worded(*spool(), kept.cursor, false)
                           : kept.cursor - behind(*kept.quilt, kept.cursor));
            return true;

        case Code::right:
            went(mods.ctrl ? worded(*spool(), kept.cursor, true)
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

            const std::size_t which = spool()->found(kept.cursor);

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
            const std::size_t which = spool()->found(kept.cursor);

            went(spool()->at(which).value_or(kept.cursor));

            return true;
        }

        case Code::end: {
            const std::optional<std::size_t> where =
                along(spool()->found(kept.cursor), SIZE_MAX);

            if (!where.has_value()) return false;

            went(*where);

            return true;
        }

        default: break;
    }

    return Page::handle(what);
}

// Kliknięcie stawia kursor tam, gdzie się kliknęło — a nie tylko bierze ognisko, jak w podglądzie.
// Rząd i kolumnę na wiersz i bajt przelicza kartka: rząd ekranu nie jest numerem wiersza, bo przy
// zawijaniu jeden wiersz zajmuje ich kilka, a kolumna nie jest bajtem, bo tabulator zajmuje osiem
// kratek. Liczone tu na piechotę dawało kursor w zupełnie innym miejscu.
bool Editor::handle(ClickEvent& what) {
    Kept& kept = *kept_;

    if (root() != nullptr) focus();

    if (!spool()) return true;

    const std::optional<Place> where = place_of(local(what.where()));

    if (!where.has_value()) return true;

    const std::optional<std::size_t> began = spool()->at(where->line);

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
// nieruchomej myszy.
//
// **Ciągnięcie wychodzi poza edytor.** Wciśnięcie prosi korzeń o przytrzymanie, więc ruch
// i puszczenie wracają tu także spoza edytora. Ruch nad nim albo pod nim przewija o wiersz jak na
// skrajnym rzędzie, a zaznaczenie sięga do pierwszego albo ostatniego widocznego wiersza — rząd
// przycinamy tutaj, bo `place_of` poza obszarem oddaje pustkę; w bok przycina już ono samo. Dawniej
// korzeń rozsyłał ruch po położeniu, skrajny rząd był ostatnim miejscem, do którego cokolwiek
// dochodziło, a puszczenie poza edytorem zostawiało ciągnięcie zapalone do następnego wciśnięcia.
bool Editor::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    if (!spool()) return true;

    if (what.press() == Press::up) {
        kept.dragging = false;

        return true;
    }

    const Spot where = local(what.where());

    const auto reached = [this, &kept](Spot aim, bool anchored) {
        const std::optional<Place> place = place_of(aim);

        if (!place.has_value()) return;

        const std::optional<std::size_t> began = spool()->at(place->line);

        if (!began.has_value()) return;

        if (anchored)
            cursor(*began + place->byte);
        else
            selection_.extend(*began + place->byte);
    };

    if (what.press() == Press::down) {
        if (what.button() != Mouse::left) return true;

        if (Root* top = root(); top != nullptr) {
            focus();
            top->capture(*this);
        }

        reached(where, true);

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

    reached({where.x, std::clamp(where.y, 0, std::max(high - 1, 0))}, false);

    return true;
}

// Dwuklik bierze wyraz. **Regułę „bajt wyrazu" bierzemy z `encoding.hpp`**, tę samą, po której
// grzebień szuka całych wyrazów — inaczej dwuklik i szukanie zaczęłyby się różnić na ogonkach.
//
// Klik w miejsce, które wyrazem nie jest, bierze ciąg takich samych: same spacje albo same znaki
// przestankowe. Zawsze coś zaznacza, więc dwuklik nigdy nie wygląda na zignorowany.
bool Editor::handle(DoubleClickEvent& what) {
    Kept& kept = *kept_;

    if (root() != nullptr) focus();

    if (!spool()) return true;

    const std::optional<Place> where = place_of(local(what.where()));

    if (!where.has_value()) return true;

    const std::optional<std::size_t> began = spool()->at(where->line);

    if (!began.has_value()) return true;

    const std::vector<std::string> got = spool()->lines(where->line, 1).value_or(std::vector<std::string>{});

    if (got.empty() || got.front().empty()) return true;

    const std::string& text = got.front();

    const std::size_t at = std::min(where->byte, text.size() - 1);

    const auto lettered = [](char one) {
        return in_word(static_cast<char32_t>(static_cast<unsigned char>(one)));
    };

    const bool kind = lettered(text[at]);

    std::size_t from = at;
    std::size_t upto = at + 1;

    while (from > 0 && lettered(text[from - 1]) == kind) --from;

    while (upto < text.size() && lettered(text[upto]) == kind) ++upto;

    selection_.set(*began + from, *began + upto);

    return true;
}

bool Editor::handle(PasteEvent& what) {
    insert(what.text());

    return true;
}

// --- Blok mcedit -------------------------------------------------------------------------------------
//
// Trwały odcinek dokumentu, niezależny od zaznaczenia (`anchor..cursor`) i od schowka — jak
// w mcedit, gdzie blok i schowek też są osobno. F3 zaczyna go w kursorze, ruch rozciąga, drugie
// F3 kończy i blok zostaje podświetlony, choć kursor pójdzie dalej; dopiero wtedy F5 wstawia go
// przy kursorze, F6 przenosi, F8 usuwa. Blok jest trzecią pozycją, nie zaznaczeniem, bo `insert`
// zastępuje zaznaczenie, a kursor **jest** jego końcem — „skopiuj blok do kursora" nie miałoby
// w tym modelu sensu. Nic własnego nie trzyma: patrzy w `Kept` edytora przez właściciela.

Editor::Block::Block(Editor& owner) : owner_(owner) {}

bool Editor::Block::empty() const { return !owner_.kept_->block.has_value(); }

bool Editor::Block::pending() const { return owner_.kept_->marking; }

std::size_t Editor::Block::from() const {
    const Kept& kept = *owner_.kept_;

    return kept.block.has_value() ? kept.block->first : 0;
}

// W trakcie rozciągania koniec jest kursorem — może stać przed początkiem, gdy blok idzie w tył;
// `text()` i malowanie porządkują min/max.
std::size_t Editor::Block::upto() const {
    const Kept& kept = *owner_.kept_;

    if (!kept.block.has_value()) return 0;

    return kept.marking ? kept.cursor : kept.block->second;
}

std::string Editor::Block::text() const {
    if (empty()) return {};

    return owner_.kept_->text(std::min(from(), upto()), std::max(from(), upto()));
}


// F3, trzy stany: bez bloku zaczyna go w kursorze; w trakcie kończy; przy bloku gotowym zdejmuje
// stary i zaczyna nowy w kursorze.
void Editor::Block::mark() {
    Kept& kept = *owner_.kept_;

    if (!kept.quilt) return;

    if (kept.marking) {
        kept.blocked();
    } else {
        kept.block = std::pair{kept.cursor, kept.cursor};
        kept.marking = true;
    }

    owner_.invalidate();
}

// Zdjęcie bloku bez śladu w dokumencie — nie idzie do dziennika, bo treść się nie zmienia.
void Editor::Block::clear() {
    Kept& kept = *owner_.kept_;

    kept.block.reset();
    kept.marking = false;

    owner_.invalidate();
}

// F5: treść bloku wchodzi w kursorze, blok zostaje, gdzie był — a gdy leżał za kursorem,
// przesuwa się o swoją długość zwykłą regułą „zmiana przed blokiem". Jedna zmiana w dzienniku,
// zapieczętowana: znak wpisany zaraz potem nie ma prawa się do niej dokleić.
bool Editor::Block::copy() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return false;

    kept.blocked();

    if (!kept.block.has_value()) return false;

    const auto [from, upto] = *kept.block;
    const Quilt::Scrap what = kept.quilt->taken(from, upto - from);
    const std::size_t at = kept.cursor;

    kept.joining = false;
    kept.apply(*owner_.spool(), at, {}, what);
    kept.note(at, {}, what, at);
    kept.joining = false;

    owner_.settled(at + what.much);

    return true;
}

// F6: blok wychodzi ze swojego miejsca i wchodzi tam, gdzie stał kursor **po** wycięciu — gdy
// leżał przed kursorem, cel cofa się o jego długość. Kursor w bloku albo na jego brzegu to
// „przenieś do siebie": nic. Dwa wpisy dziennika związane w jeden — cofnięcie przywraca i tekst,
// i blok tam, skąd wyszedł. Po przeniesieniu blok obejmuje wstawiony tekst, żeby dało się
// przenosić dalej.
bool Editor::Block::move() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return false;

    kept.blocked();

    if (!kept.block.has_value()) return false;

    const auto [from, upto] = *kept.block;
    const std::size_t at = kept.cursor;

    if (at >= from && at <= upto) return false;

    const Quilt::Scrap what = kept.quilt->taken(from, upto - from);
    const std::size_t target = at > upto ? at - what.much : at;

    kept.joining = false;
    kept.apply(*owner_.spool(), from, what, {});
    kept.note(from, what, {}, at);

    kept.tying = true;
    kept.apply(*owner_.spool(), target, {}, what);
    kept.block = std::pair{target, target + what.much};
    kept.note(target, {}, what, at);
    kept.tying = false;
    kept.joining = false;

    owner_.settled(target + what.much);

    return true;
}

// F8: blok znika z dokumentu razem ze sobą. Kursor za blokiem cofa się o jego długość, w bloku
// staje na jego początku, przed — zostaje. Jedna zmiana; cofnięcie przywraca tekst i blok.
bool Editor::Block::erase() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return false;

    kept.blocked();

    if (!kept.block.has_value()) return false;

    const auto [from, upto] = *kept.block;
    const std::size_t at = kept.cursor;

    const Quilt::Scrap what = kept.quilt->taken(from, upto - from);

    kept.joining = false;
    kept.apply(*owner_.spool(), from, what, {});
    kept.block.reset();
    kept.note(from, what, {}, at);
    kept.joining = false;

    owner_.settled(at >= upto ? at - what.much : (at > from ? from : at));

    return true;
}

}

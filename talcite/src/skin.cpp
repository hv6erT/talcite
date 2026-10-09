#include "talcite/skin.hpp"

#include <optional>
#include <type_traits>
#include <utility>

#include "lining.hpp"

namespace talcite::ui {

namespace {

Style faded() {
    Style made{};

    made.faint = true;

    return made;
}

Style turned() {
    Style made{};

    made.reverse = true;

    return made;
}

Style lined() {
    Style made{};

    made.under = true;

    return made;
}

// Wytłuszczenie jako znak zaznaczenia. W Midnight Commanderze zaznaczony plik jest żółty, ale
// wbudowana skórka nie ma kolorów — a wytłuszczenie widać także na monochromatycznym terminalu.
Style bolded() {
    Style made{};

    made.bold = true;

    return made;
}

Style boldly() {
    Style made{};

    made.bold = true;
    made.reverse = true;

    return made;
}

Style pointed() {
    Style made{};

    made.reverse = true;
    made.under = true;

    return made;
}

// **Wbudowana skórka odtwarza dzisiejszy wygląd co do kratki.** Odwrócenie zostaje odwróceniem,
// przygaszenie przygaszeniem — dzięki temu przeniesienie widżetów na role da się sprawdzić twardym
// kryterium: żadna istniejąca próba nie ma prawa się zmienić.
//
// Dwa wpisy są **zastępnikami czekającymi na kolory** i stąd, a nie z kodu, mają kiedyś zniknąć:
// `talcite.input` niesie podkreślenie, bo w Midnight Commanderze pole wyróżnia sam kolor tła,
// a `talcite.list.mark` jest pusty, bo nieaktywny panel MC nie pokazuje kursora — w oknie dialogowym
// pokazuje go innym kolorem, którego jeszcze nie mamy.
//
// **Wszystkie nazwy biblioteki zaczynają się od `talcite.`** — i to nie jest ozdoba. Klucz jest nazwą,
// więc aplikacja może wymyślić dowolną; bez przedrostka jej własne `input` przykryłoby rolę
// biblioteki i nikt by o tym nie wiedział. Cudze nazwy zaczynają się od nazwy programu.
Styles building() {
    return {
        {"talcite.normal", Style{}},

        {"talcite.frame", Style{}},
        {"talcite.frame.title", Style{}},
        {"talcite.title", Style{}},

        // Papier pod kratkami obrazka pokrytymi tylko częściowo — brzeg wyciętego kształtu.
        // Wbudowana zostawia go przezroczystym, więc widać przez niego rodzica.
        {"talcite.image", Style{}},

        // Podgląd tekstu rysuje samą treść, więc wbudowana zostawia go bez wyróżnienia — tak samo
        // jak zwykły napis.
        {"talcite.viewer", Style{}},

        // Zaznaczenie w podglądzie: wbudowana odwraca, bo barw jeszcze nie ma, a odwrócenie widać
        // na każdym terminalu. Bieżące — to, na którym stoi skok — ma **własną rolę**, żeby dało
        // się je odróżnić od pozostałych; wbudowana dokłada mu podkreślenie, bo drugiego wyróżnika
        // bez barw nie ma.
        {"talcite.viewer.mark", turned()},
        {"talcite.viewer.mark.focus", pointed()},

        // Edytor rysuje tę samą treść co podgląd, więc wbudowana nie wyróżnia go niczym.
        // Zaznaczenie odwraca — jak wszystko, co bez barw ma być widoczne.
        {"talcite.editor", Style{}},
        {"talcite.editor.select", turned()},

        {"talcite.input", lined()},
        {"talcite.input.guess", faded()},
        {"talcite.input.disabled", faded()},

        {"talcite.button", Style{}},
        {"talcite.button.focus", turned()},
        {"talcite.button.key", lined()},
        {"talcite.button.disabled", faded()},

        // Komunikat ma **własną rodzinę ról**, a nie te same, co reszta okna: pudełko błędu
        // wygląda inaczej niż zwykłe okno, a jedna rola nie umie być naraz czerwona i granatowa.
        // Przyciski w komunikacie powtarzają wartości zwykłych, żeby wbudowana skórka pokazywała
        // je tak samo — różnicę wnosi dopiero skórka barwna.
        {"talcite.message", Style{}},
        {"talcite.message.title", Style{}},
        {"talcite.message.text", Style{}},
        {"talcite.message.box", Style{}},
        {"talcite.message.button", Style{}},
        {"talcite.message.button.focus", turned()},
        {"talcite.message.button.key", lined()},
        {"talcite.message.button.disabled", faded()},

        // Błąd to **ten sam widżet z innym rdzeniem**, więc i rodzina ról jest ta sama. Bez barw
        // niczym się nie różni i tak ma być: mono-skórka Midnight Commandera też nie odróżnia
        // pudełka błędu od zwykłego okna. Różnicę wnosi paleta, a nie kod.
        {"talcite.error", Style{}},
        {"talcite.error.title", Style{}},
        {"talcite.error.text", Style{}},
        {"talcite.error.box", Style{}},
        {"talcite.error.button", Style{}},
        {"talcite.error.button.focus", turned()},
        {"talcite.error.button.key", lined()},
        {"talcite.error.button.disabled", faded()},

        {"talcite.list", Style{}},
        {"talcite.list.focus", turned()},
        {"talcite.list.mark", Style{}},
        {"talcite.list.disabled", faded()},

        {"talcite.tick", Style{}},
        {"talcite.tick.focus", turned()},
        {"talcite.tick.key", lined()},
        {"talcite.tick.disabled", faded()},

        {"talcite.choice", Style{}},
        {"talcite.choice.focus", turned()},
        {"talcite.choice.disabled", faded()},

        {"talcite.select", Style{}},
        {"talcite.select.focus", turned()},
        {"talcite.select.disabled", faded()},

        {"talcite.table", Style{}},
        {"talcite.table.head", turned()},
        {"talcite.table.focus", turned()},
        {"talcite.table.mark", bolded()},
        {"talcite.table.mark.focus", boldly()},

        // Widok różnic. Bez barw strony nie różnią się niczym — różnicę wnosi paleta — ale
        // zaznaczone części napisu muszą być widać i na monochromatycznym terminalu: usunięte
        // podkreśla, dodane wytłuszcza. Kursor odwraca, jak w tabeli.
        {"talcite.diff", Style{}},
        {"talcite.diff.head", turned()},
        {"talcite.diff.title", bolded()},
        {"talcite.diff.focus", turned()},
        {"talcite.diff.gone", Style{}},
        {"talcite.diff.came", Style{}},
        {"talcite.diff.gone.mark", lined()},
        {"talcite.diff.came.mark", bolded()},
        {"talcite.diff.number", faded()},

        {"talcite.menubar", Style{}},
        {"talcite.menubar.focus", turned()},
        {"talcite.menubar.key", lined()},

        {"talcite.menu", Style{}},
        {"talcite.menu.focus", turned()},
        {"talcite.menu.disabled", faded()},
        {"talcite.menu.key", lined()},
        {"talcite.menu.frame", Style{}},

        {"talcite.shade", faded()},

        {"talcite.legend.tag", Style{}},
        {"talcite.legend.key", lined()},
        {"talcite.legend.text", turned()},
        {"talcite.legend.disabled", faded()},

        {"talcite.scroll", Style{}},
        {"talcite.scroll.thumb", Style{}},

        {"talcite.gauge", Style{}},
        {"talcite.gauge.done", Style{}},
        {"talcite.gauge.text", Style{}},
    };
}

// Znaki ramek leżą pojedynczo, tak jak w pliku skórki Midnight Commandera — komplet składa się
// z nich dopiero w konstruktorze.
void ruled(Glyphs& into, std::string_view named, Border how) {
    const Lines one = drawn(how);
    const std::string prefix{named};

    into.emplace(prefix + ".across", one.across);
    into.emplace(prefix + ".upright", one.upright);
    into.emplace(prefix + ".upper.left", one.upper_left);
    into.emplace(prefix + ".upper.right", one.upper_right);
    into.emplace(prefix + ".lower.left", one.lower_left);
    into.emplace(prefix + ".lower.right", one.lower_right);
    into.emplace(prefix + ".left.joint", one.left_joint);
    into.emplace(prefix + ".right.joint", one.right_joint);
}

// Wbudowana tablica znaków, w parze z `building()`, które składa style. Nazywała się wcześniej
// `marking` — słowem, które w tej bibliotece znaczy literę aktywną i nie ma z tablicą znaków nic
// wspólnego.
Glyphs glyphing() {
    Glyphs made{
        {"talcite.scroll.up", U'▲'},   {"talcite.scroll.down", U'▼'}, {"talcite.scroll.left", U'◀'},
        {"talcite.scroll.right", U'▶'}, {"talcite.scroll.thumb", U'█'}, {"talcite.scroll.track", U'░'},

        {"talcite.gauge.done", U'█'},  {"talcite.gauge.rest", U'░'},  {"talcite.gauge.left", U'['},
        {"talcite.gauge.right", U']'},

        {"talcite.choice.dot", U'•'},
        {"talcite.input.conceal", U'•'},
        {"talcite.select.mark", U'▼'},

        {"talcite.elide", U'…'},
    };

    ruled(made, "talcite.line.thin", Border::thin);
    ruled(made, "talcite.line.twin", Border::twin);
    ruled(made, "talcite.line.thick", Border::thick);

    return made;
}

// Znak dla poziomu; gdy skórka nie ma go na tym poziomie — wbudowany, a gdy i tego nie ma — `?`,
// nigdy zero (powód przy `Skin::style`).
char32_t glyphed(const Glyphs& among, const Glyphs& spare, const std::string& named,
                 bool unicode) {
    if (const auto at = among.find(named); at != among.end())
        if (const std::optional<char32_t> one = at->second.fitted(unicode)) return *one;

    if (const auto at = spare.find(named); at != spare.end())
        if (const std::optional<char32_t> one = at->second.fitted(unicode)) return *one;

    return U'?';
}

// Komplet składa się przy budowaniu i po każdej zmianie poziomu znaków — `Lines` bierze się osiem
// razy na ramkę i nie ma być składane przy każdym malowaniu.
Lines gathered(const Glyphs& among, const Glyphs& spare, std::string_view named, bool unicode) {
    const std::string prefix{named};

    return {glyphed(among, spare, prefix + ".upper.left", unicode),
            glyphed(among, spare, prefix + ".upper.right", unicode),
            glyphed(among, spare, prefix + ".lower.left", unicode),
            glyphed(among, spare, prefix + ".lower.right", unicode),
            glyphed(among, spare, prefix + ".across", unicode),
            glyphed(among, spare, prefix + ".upright", unicode),
            glyphed(among, spare, prefix + ".left.joint", unicode),
            glyphed(among, spare, prefix + ".right.joint", unicode)};
}

// Wbudowana tablica znaków, zbudowana raz — komplety ramek sięgają do niej, gdy skórka nie ma
// znaku na bieżącym poziomie.
const Glyphs& glyphed() {
    static const Glyphs one = glyphing();

    return one;
}

}

// **Skórka jest kompletna od urodzenia.** Konstruktor zaczyna od wbudowanych wartości i nadpisuje
// je tym, co dał wołający — to jest `spare` plus `held`, tylko scalone raz, a nie przy każdym
// odczycie. Wbudowane wchodzą jako jedna wartość na każdy poziom, więc rola biblioteki ma zawsze
// co pokazać; rola podana z zewnątrz **zastępuje** wbudowaną w całości, a to, jakiego poziomu jej
// brakuje, rozstrzyga się dopiero przy odczycie — bo dopiero wtedy wiadomo, na jakim terminalu
// skórka stoi.
Skin::Skin(Styles styles, Glyphs glyphs) : styles_(building()), glyphs_(glyphing()) {
    for (auto& [named, one] : styles) styles_.insert_or_assign(named, one);

    for (const auto& [named, one] : glyphs) glyphs_.insert_or_assign(named, one);

    framed();
}

Skin::Skin() : Skin(Styles{}, Glyphs{}) {}

// Jeden egzemplarz na cały program — bo `Node::skin()` bez korzenia musi oddać **referencję**,
// a nie wartość. Statyczna zmienna w funkcji zbuduje się przy pierwszym pytaniu i ani razu więcej.
const Skin& Skin::plain() {
    static const Skin one;

    return one;
}

// **Odczyt nigdy nie rzuca**, bo woła się go z malowania. Literówka w nazwie roli albo rola
// wymyślona przez aplikację i nieustawiona wywracałyby program w środku składania klatki, i to
// nie w miejscu błędu, tylko przy pierwszym odmalowaniu.
//
// Poziom rozstrzyga się **tu**, nie przy budowaniu: skórka niesie warianty na wszystkie poziomy,
// a to, na jakim terminalu stoi, mówi jej korzeń przez `colors` i `unicode`. Wariantu brak na tym
// poziomie — bierze się wbudowany dla tej samej roli, bez zgadywania i bez przybliżania barw.
// Skórka z samym `rgb` na terminalu `ansi` pokazuje więc to, co skórka wbudowana.
//
// Nieznany znak oddaje `U'?'`, a **nigdy zera**: w `View` zero znaczy dalszy ciąg znaku
// dwukolumnowego i składanie klatki takie kratki pomija, więc zero zrobiłoby dziurę w obrazie
// zamiast widocznego błędu.
Style Skin::style(std::string_view key) const {
    if (const auto at = styles_.find(key); at != styles_.end())
        if (const std::optional<Style> one = at->second.fitted(colors_)) return *one;

    if (this == &plain()) return Style{};

    return plain().style(key);
}

char32_t Skin::glyph(std::string_view key) const {
    return glyphed(glyphs_, glyphed(), std::string{key}, unicode_);
}

// Komplet leży gotowy, złożony przy budowaniu i po każdej zmianie poziomu znaków (`framed`).
// `Border::none` oddaje cienki: widżet bez ramki o znaki nie pyta, a gdyby zapytał, dostaje
// komplet, który da się narysować, a nie spacje udające brak.
Lines Skin::lines(Border border) const { return lines_[static_cast<std::size_t>(border)]; }

void Skin::colors(Colors given) { colors_ = given; }

void Skin::unicode(bool given) {
    unicode_ = given;

    framed();
}

void Skin::framed() {
    for (const Border one : {Border::none, Border::thin, Border::twin, Border::thick})
        lines_[static_cast<std::size_t>(one)] = gathered(glyphs_, glyphed(), named(one), unicode_);
}

// Wariant o poziomie najwyższym nieprzekraczającym podanego. Skórka z samym `rgb` na terminalu
// `ansi` nie ma czego pokazać — i oddaje pustkę, a nie przybliżenie: co wtedy rysować, rozstrzyga
// `Skin::style`, sięgając do wbudowanej.
std::optional<Style> Shades::fitted(Colors level) const {
    const std::array<const std::optional<Style>*, 4> by{&mono, &ansi, &palette, &rgb};

    for (int at = static_cast<int>(level); at >= 0; --at)
        if (by[static_cast<std::size_t>(at)]->has_value()) return *by[static_cast<std::size_t>(at)];

    return std::nullopt;
}

Shades::Shades(Style same) : mono(same), ansi(same), palette(same), rgb(same) {}

// Terminal z unicode bierze znak unicode, a gdy go nie ma — ASCII; terminal bez unicode bierze
// tylko ASCII, bo znak spoza niego wyszedłby jako śmieć.
std::optional<char32_t> Signs::fitted(bool wanted) const {
    if (wanted && unicode.has_value()) return unicode;

    return ascii;
}

Signs::Signs(char32_t same) : ascii(same), unicode(same) {}

}

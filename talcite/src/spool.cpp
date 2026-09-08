#include "spool.hpp"

#include <algorithm>
#include <utility>

namespace talcite {

namespace {

// Okno bajtów trzymane pod ręką. Jedno wołanie `Feed` obsługuje kilkaset wierszy zwykłego tekstu,
// więc przewijanie strzałką prawie nigdy nie schodzi do źródła.
constexpr std::size_t window = 64 * 1024;

// **Górna granica długości wiersza.** Plik bez ani jednego `\n` — a taki jest każdy plik binarny —
// bez niej zassałby się w całości do jednego napisu przy pierwszym otwarciu. Po przekroczeniu
// granicy uznajemy, że wiersz się tu kończy: dokument bez końców wierszy rozpada się na równe
// kawałki i nic się nie wiesza.
constexpr std::size_t brim = 1024 * 1024;

// Kotwic nie może przybywać bez końca, bo przy dużym dokumencie same zajęłyby więcej niż okno.
// Po zapełnieniu wyrzucamy co drugą i podwajamy odstęp — pamięć stoi, a dobieg rośnie dopiero
// tam, gdzie i tak jest daleki skok w miejsce, w którym nigdy nie byliśmy.
constexpr std::size_t plenty = 4096;

// Podłoga odstępu. Przy oknie na trzy wiersze kotwica co trzy wiersze byłaby samą narzutą.
constexpr std::size_t least = 64;

}

Spool::Spool(Feed feed, std::size_t whole) : feed_(std::move(feed)), whole_(whole) {
    anchors_.push_back({0, 0});

    if (whole_ == 0) counted_ = 0;
}

// Dokument podany w całości ma osobne wejście, bo wtedy okno jest nim samym: nie ma czego dobierać
// i nie ma `Feed`. Wiersze liczymy od razu — dla licencji to mikrosekundy, a `Viewer` dostaje
// komplet wartości już przy podaniu.
Spool::Spool(Bytes given) : held_(std::move(given)), whole_(held_.size()) {
    anchors_.push_back({0, 0});

    chunk_ = held_;
    chunked_ = 0;

    std::size_t at = 0;
    std::size_t which = 0;

    while (at < whole_) {
        at = forth(at, nullptr);

        ++which;

        anchored(which, at);
    }

    counted_ = which;
}

// Gwarantuje, że **cały** zakres leży w oknie. Odczyt w tył skanuje od końca ku początkowi, więc
// nie wystarczy trafienie samym początkiem zakresu: bez tego pominęlibyśmy ogon, który został poza
// poprzednim oknem, i znaleźlibyśmy nie ten koniec wiersza, co trzeba.
//
// **Źródło ma prawo oddać mniej, niż poproszono** — plik czytany kawałkami, gniazdo, cokolwiek —
// więc dobieramy w pętli, aż zakres będzie pełny albo aż źródło przestanie cokolwiek dawać.
// Pierwsza wersja brała jedno wołanie i uznawała krótszą odpowiedź za koniec dokumentu; wtedy
// liczba wierszy wychodziła losowa, a dokument urywał się w połowie.
bool Spool::ranged(std::size_t at, std::size_t much) {
    if (at >= whole_) return false;

    const std::size_t upto = std::min(at + much, whole_);

    if (at >= chunked_ && upto <= chunked_ + chunk_.size()) return true;

    if (!feed_) return false;

    chunk_.clear();
    chunked_ = at;

    while (chunked_ + chunk_.size() < upto) {
        const std::size_t from = chunked_ + chunk_.size();

        const Bytes more = feed_(from, std::max(upto - from, window));

        if (more.empty()) break;

        chunk_.insert(chunk_.end(), more.begin(), more.end());
    }

    return chunked_ + chunk_.size() >= upto;
}

char Spool::sign(std::size_t at) const { return static_cast<char>(chunk_[at - chunked_]); }

// Oddaje bajt, od którego zaczyna się **następny** wiersz. Gdy `into` nie jest pusty, dopisuje po
// drodze treść bieżącego — bez `\n` i bez `\r` na końcu.
std::size_t Spool::forth(std::size_t at, std::string* into) {
    if (into != nullptr) into->clear();

    const auto trimmed = [into] {
        if (into != nullptr && !into->empty() && into->back() == '\r') into->pop_back();
    };

    std::size_t go = at;
    std::size_t took = 0;

    while (go < whole_) {
        if (!ranged(go, 1)) break;

        const std::size_t upto = std::min(chunked_ + chunk_.size(), whole_);

        for (std::size_t which = go; which < upto; ++which) {
            if (sign(which) == '\n') {
                trimmed();

                return which + 1;
            }

            if (took >= brim) {
                trimmed();

                return which;
            }

            if (into != nullptr) into->push_back(sign(which));

            ++took;
        }

        go = upto;
    }

    trimmed();

    return whole_;
}

// **W tył nie da się policzyć, tylko znaleźć.** Szukamy ostatniego `\n` przed tym, który kończy
// wiersz poprzedzający `at`, skanując kawałkami od końca ku początkowi. Kolejność jest tu istotna:
// szukamy **ostatniego**, więc region trzeba przejść od tyłu, a nie od pierwszego trafienia.
std::size_t Spool::back(std::size_t at) {
    if (at == 0) return 0;

    // `at` wskazuje początek wiersza, więc bajt tuż przed nim to `\n` kończący poprzedni. Jego
    // samego nie oglądamy.
    const std::size_t upto = at - 1;

    std::size_t end = upto;

    while (end > 0) {
        const std::size_t step = std::min(window, end);
        const std::size_t from = end - step;

        if (!ranged(from, step)) break;

        for (std::size_t which = end; which > from; --which)
            if (sign(which - 1) == '\n') return which;

        if (upto - from >= brim) return upto - brim;

        end = from;
    }

    return 0;
}

Spool::Anchor Spool::nearest(std::size_t which) const {
    Anchor made = anchors_.front();

    for (const Anchor& one : anchors_) {
        if (one.line > which) break;

        made = one;
    }

    return made;
}

void Spool::anchored(std::size_t which, std::size_t at) {
    if (which == 0 || which % stride_ != 0) return;

    if (anchors_.back().line >= which) return;

    anchors_.push_back({which, at});

    if (anchors_.size() <= plenty) return;

    std::vector<Anchor> kept;
    kept.reserve(anchors_.size() / 2 + 1);

    for (std::size_t one = 0; one < anchors_.size(); one += 2) kept.push_back(anchors_[one]);

    anchors_ = std::move(kept);
    stride_ *= 2;
}

// Odstęp bierze się z wysokości okna, bo skok to prawie zawsze strona, `Home`, `End` albo
// pociągnięcie paska — a wtedy każdy z nich ląduje tuż obok znanego punktu. Nie zmniejszamy go
// nigdy: raz przerzedzone kotwice nie mają jak zgęstnieć, a wahanie tam i z powrotem kosztowałoby
// samo przerzedzanie.
void Spool::screen(int high) {
    const std::size_t want = high > 0 ? static_cast<std::size_t>(high) : 0;

    stride_ = std::max(stride_, std::max(least, want));
}

// **Po zmianie dokumentu wszystko za miejscem zmiany jest nieaktualne.** Kotwice wskazują bajty,
// okno trzyma bajty sprzed zmiany, a kursor może stać za nią — więc jedno i drugie idzie do kosza.
// To, co **przed** zmianą, zostaje: tam nic się nie ruszyło.
//
// Liczba wierszy, jeśli była znana, poprawia się o różnicę końców wierszy — tę wołający zna
// lokalnie, bo wie, co wstawił i co skasował. Jeśli była nieznana, zostaje nieznana.
void Spool::changed(std::size_t at, std::ptrdiff_t bytes, std::ptrdiff_t lines) {
    whole_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(whole_) + bytes);

    chunk_.clear();
    chunked_ = 0;

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(),
                                     [at](const Anchor& one) { return one.at >= at && one.line > 0; });

    anchors_.erase(kept, anchors_.end());

    if (here_.at >= at) here_ = {0, 0};

    if (counted_.has_value())
        counted_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*counted_) + lines);
}

std::optional<std::size_t> Spool::at(std::size_t which) {
    if (counted_.has_value() && which >= *counted_) return {};

    if (which == here_.line) return here_.at;

    // Z dwóch dróg bierzemy krótszą: dobieg od kotwicy w przód albo cofanie się od miejsca,
    // w którym stoimy. Przy strzałce w górę to zawsze jest jeden krok w tył.
    if (which < here_.line && here_.line - which <= which - nearest(which).line) {
        std::size_t go = here_.at;

        for (std::size_t one = here_.line; one > which; --one) go = back(go);

        here_ = {which, go};

        return go;
    }

    Anchor from = nearest(which);

    if (which > here_.line && here_.line > from.line) from = here_;

    std::size_t go = from.at;

    for (std::size_t one = from.line; one < which; ++one) {
        if (go >= whole_) {
            counted_ = one;

            return {};
        }

        go = forth(go, nullptr);

        anchored(one + 1, go);
    }

    if (go >= whole_) {
        counted_ = which;

        return {};
    }

    here_ = {which, go};

    return go;
}

// Odwrotność `at`: który wiersz zawiera ten bajt. Kotwice są uporządkowane i po numerze wiersza,
// i po bajcie, więc zaczynamy od ostatniej leżącej nie dalej niż szukane miejsce.
std::size_t Spool::found(std::size_t at) {
    if (whole_ == 0) return 0;

    const std::size_t want = at < whole_ ? at : whole_ - 1;

    Anchor from = anchors_.front();

    for (const Anchor& one : anchors_) {
        if (one.at > want) break;

        from = one;
    }

    if (here_.at <= want && here_.at > from.at) from = here_;

    std::size_t go = from.at;
    std::size_t which = from.line;

    while (go < whole_) {
        const std::size_t next = forth(go, nullptr);

        if (next > want) break;

        go = next;

        ++which;

        anchored(which, go);
    }

    here_ = {which, go};

    return which;
}

std::vector<std::string> Spool::lines(std::size_t which, std::size_t most) {
    std::vector<std::string> made;

    const std::optional<std::size_t> from = at(which);

    if (!from.has_value()) return made;

    made.reserve(most);

    std::size_t go = *from;
    std::size_t began = *from;
    std::size_t one = which;
    std::string said;

    while (made.size() < most && go < whole_) {
        began = go;

        go = forth(go, &said);

        made.push_back(said);

        ++one;

        anchored(one, go);
    }

    if (go >= whole_) counted_ = one;

    if (!made.empty()) here_ = {one - 1, began};

    return made;
}

}

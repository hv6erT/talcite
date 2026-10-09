#include "spool.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace talcite {

namespace {

// Okno bajtów trzymane pod ręką. Jedno wołanie źródła obsługuje kilkaset wierszy zwykłego
// tekstu, więc przewijanie strzałką prawie nigdy nie schodzi do niego drugi raz.
constexpr std::size_t window = 64 * 1024;

// Ile okien wolno trzymać naraz, gdy dochodzą jedno po drugim: dalsze wypychają najstarsze od
// przodu. Bez granicy przejście dokumentu do końca wciągnęłoby go w całości.
constexpr std::size_t stock = 8 * window;

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

// **Rozgrzana szpula.** `head` to bajty od początku dokumentu, które wołający już ma — okno
// nagłówka, czytane, żeby poznać rodzaj pliku — więc okno zerowe stoi od razu i pierwsze pytanie
// o wiersze odpowiada od ręki, bez zamawiania i bez obrotu pętli. Wszystko dalej — dobieranie,
// liczenie wierszy, kotwice, `stalled()` — idzie starą drogą; różni się tylko to, skąd wzięło się
// pierwsze okno. Pusta głowa to szpula jak dotąd. Przycięcie głowy do dokumentu i do zapasu jest
// strażą na pamięć, nie regułą: `whole_` i tak ogranicza każde czytanie, więc mutant bez
// przycięcia przeżył zestaw — kod zostaje, żeby nie trzymać bajtów, których nikt nie przeczyta.
Spool::Spool(Feed feed, std::size_t whole, Bytes head) : feed_(std::move(feed)), whole_(whole) {
    anchors_.push_back({0, 0});

    if (whole_ == 0) counted_ = 0;

    if (head.empty()) return;

    head.resize(std::min({head.size(), whole_, stock}));

    chunk_ = std::move(head);
    chunked_ = 0;
}

Spool::Spool(Peek peek, std::size_t whole) : peek_(std::move(peek)), whole_(whole) {
    anchors_.push_back({0, 0});

    if (whole_ == 0) counted_ = 0;
}

// Dokument podany w całości ma osobne wejście, bo wtedy okno jest nim samym: nie ma czego dobierać
// i nie ma czego zamawiać. Wiersze liczymy od razu — dla licencji to mikrosekundy, a `Viewer` dostaje
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

bool Spool::cached(std::size_t at, std::size_t upto) const {
    return at >= chunked_ && upto <= chunked_ + chunk_.size();
}

// Gwarantuje, że **cały** zakres leży w oknie. Odczyt w tył skanuje od końca ku początkowi, więc
// nie wystarczy trafienie samym początkiem zakresu: bez tego pominęlibyśmy ogon, który został poza
// poprzednim oknem, i znaleźlibyśmy nie ten koniec wiersza, co trzeba.
//
// Szew wewnętrzny (`Peek`, kołdra edytora) **ma prawo oddać mniej, niż poproszono**, więc dobieramy w pętli,
// aż zakres będzie pełny albo aż źródło przestanie cokolwiek dawać. Pierwsza wersja brała jedno
// wołanie i uznawała krótszą odpowiedź za koniec dokumentu; wtedy liczba wierszy wychodziła
// losowa, a dokument urywał się w połowie.
//
// Źródło (`Feed`) nie daje nic od ręki: zamawiamy okno, zapisujemy, że odpowiedź
// jest niepełna, i oddajemy fałsz. Kto pytał, dostanie mniej i zapyta znowu, gdy okno dojdzie.
bool Spool::ranged(std::size_t at, std::size_t much) {
    if (at >= whole_) return false;

    const std::size_t upto = std::min(at + much, whole_);

    if (cached(at, upto)) return true;

    if (peek_) {
        chunk_.clear();
        chunked_ = at;

        while (chunked_ + chunk_.size() < upto) {
            const std::size_t from = chunked_ + chunk_.size();

            const Bytes more = peek_(from, std::max(upto - from, window));

            if (more.empty()) break;

            chunk_.insert(chunk_.end(), more.begin(), more.end());
        }

        return chunked_ + chunk_.size() >= upto;
    }

    if (!feed_) return false;

    stalled_ = true;
    wanted_ = Want{at, upto};

    ask(at, upto);

    return false;
}

// Zamówienie okna: od miejsca, którego brakuje — a gdy początek zakresu już mamy, od końca tego,
// co mamy, żeby dołożyć, a nie zaczynać od nowa. Jedno zamówienie w drodze naraz; następne czeka
// w `wanted_` i idzie po przyjściu tego, które biegnie.
void Spool::ask(std::size_t at, std::size_t upto) {
    if (busy_ || !feed_) return;

    const std::size_t end = chunked_ + chunk_.size();
    const std::size_t from = !chunk_.empty() && at >= chunked_ && at <= end ? end : at;

    if (from >= whole_) return;

    const std::size_t most = std::max(upto > from ? upto - from : 0, window);

    busy_ = true;

    const std::weak_ptr<Spool> weak = weak_from_this();

    ordered_.emplace(feed_(from, most)
                       .here()
                       .then([weak, from](Bytes got) {
                           if (const std::shared_ptr<Spool> mine = weak.lock()) mine->arrived(from, std::move(got));
                       })
                       .fail([weak, from](const std::exception&) {
                           // Źródło odmówiło: dokument kończy się tam, dokąd doszliśmy. Bez tego
                           // każde malowanie zamawiałoby to samo okno od nowa, bez końca.
                           if (const std::shared_ptr<Spool> mine = weak.lock()) mine->arrived(from, {});
                       }));

    ordered_->start();
}

// Okno doszło. Dokładamy do tego, co mamy, gdy przylega — inaczej zaczynamy okno od nowa. Zapas
// odpada od przodu: kto przewija w przód, nie potrzebuje tego, co zostawił daleko za sobą. Potem
// druga kolejka: zamówienie, które czekało, liczenie w toku i kto chciał wiedzieć, że coś doszło.
//
// **Przycinamy dopiero przy dwóch zapasach, i to do jednego.** Dawniej przycięcie szło przy każdym
// oknie ponad `stock`, a przesunięcie siedmiu ósmych zapasu w przód, żeby dołożyć jedną ósmą, to
// siedem bajtów przepisanych na każdy przeczytany — przejście dokumentu do końca (`End`, liczenie
// wierszy) kosztowało tyle co siedem przejść. Teraz zapas przepisuje się raz na tyle bajtów, ile
// sam ma: koszt na bajt stały, a pamięci najwyżej dwa zapasy.
void Spool::arrived(std::size_t from, Bytes got) {
    busy_ = false;

    const bool nothing = got.empty();

    if (!nothing) {
        if (!chunk_.empty() && from == chunked_ + chunk_.size()) {
            chunk_.insert(chunk_.end(), got.begin(), got.end());

            if (chunk_.size() > 2 * stock) {
                const std::size_t drop = chunk_.size() - stock;

                chunk_.erase(chunk_.begin(), chunk_.begin() + static_cast<std::ptrdiff_t>(drop));
                chunked_ += drop;
            }
        } else {
            chunk_ = std::move(got);
            chunked_ = from;
        }
    }

    // Źródło nie oddało nic, choć dokument obiecywał więcej — plik skrócił się pod nami albo
    // przestał się czytać. Dalej nie ma nic; liczba wierszy ustali się przy najbliższym przejściu.
    if (nothing && from < whole_) whole_ = from;

    if (wanted_.has_value()) {
        const Want want = *wanted_;

        wanted_.reset();

        if (!cached(want.at, std::min(want.upto, whole_)) && want.at < whole_) {
            stalled_ = true;
            wanted_ = want;

            ask(want.at, want.upto);
        }
    }

    if (counting_ && !counted_.has_value()) (void)at(SIZE_MAX);

    if (awoken_) awoken_();
}

char Spool::sign(std::size_t at) const { return static_cast<char>(chunk_[at - chunked_]); }

// Oddaje bajt, od którego zaczyna się **następny** wiersz. Gdy `into` nie jest pusty, dopisuje po
// drodze treść bieżącego — bez `\n` i bez `\r` na końcu. Przy zamówionym, a nieobecnym oknie
// oddaje miejsce, do którego doszła — wołający sprawdza `stalled_` i nie wierzy wynikowi.
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

    return stalled_ ? go : whole_;
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

// Kotwice są uporządkowane po wierszu, więc ostatnią nie dalszą niż `which` znajduje szukanie
// połówkowe. Dawniej szło to po kolei — przy czterech tysiącach kotwic cztery tysiące porównań na
// każdy skok. Kotwica zerowa jest zawsze, więc wynik zawsze jest.
Spool::Anchor Spool::nearest(std::size_t which) const {
    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), which,
                                       [](std::size_t want, const Anchor& one) { return want < one.line; });

    return *std::prev(past);
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

// **Zmiana w jednym miejscu: `gone` bajtów od `at` zastąpione przez `came`.** Kotwice za zmienionym
// odcinkiem przesuwają się o różnicę bajtów i wierszy — wiersz, który tam się zaczynał, dalej się
// zaczyna, tylko gdzie indziej. Dawniej szły do kosza wszystkie za miejscem zmiany, a wtedy każdy
// klawisz przy bloku daleko w pliku liczył wiersze od miejsca zmiany do bloku od nowa — przy dużym
// pliku megabajty na klawisz. Giną tylko kotwice **w** skasowanym odcinku (za jego początkiem, do
// końca włącznie): ich wiersz mógł zniknąć albo zacząć się gdzie indziej. Kotwica dokładnie w `at`
// zostaje — bajt przed nią się nie zmienił, więc wiersz dalej zaczyna się tam. Tak samo miejsce, w
// którym szpula stoi. Okno bajtów idzie do kosza w całości: trzyma bajty sprzed zmiany.
//
// `lines` to różnica końców wierszy — wołający zna ją lokalnie, bo wie, co wstawił i co skasował.
// Liczba wierszy, jeśli była znana, poprawia się o nią; jeśli była nieznana, zostaje nieznana.
// Różnica bywa poprawiona o to, czy ostatni wiersz istnieje, ale to dotyczy tylko zmian na samym
// końcu dokumentu — a za końcem kotwic nie ma.
void Spool::changed(std::size_t at, std::size_t gone, std::size_t came, std::ptrdiff_t lines) {
    whole_ = whole_ - gone + came;

    chunk_.clear();
    chunked_ = 0;

    const std::size_t end = at + gone;

    const auto moved = [&](Anchor& one) {
        if (one.at <= at || one.line == 0) return true;

        if (one.at <= end) return false;

        one.at = one.at - gone + came;
        one.line = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one.line) + lines);

        return true;
    };

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(), [&moved](Anchor& one) { return !moved(one); });

    anchors_.erase(kept, anchors_.end());

    if (!moved(here_)) here_ = {0, 0};

    if (counted_.has_value())
        counted_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*counted_) + lines);
}

// **Zmiana w wielu miejscach naraz** („zamień wszystkie"): cięcia przesuwają różne odcinki o różne
// wartości, więc wszystko za pierwszym z nich idzie do kosza, jak dawniej przy każdej zmianie. Przed
// pierwszym cięciem nic się nie ruszyło.
void Spool::rewritten(std::size_t at, std::ptrdiff_t bytes, std::ptrdiff_t lines) {
    whole_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(whole_) + bytes);

    chunk_.clear();
    chunked_ = 0;

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(),
                                     [at](const Anchor& one) { return one.at > at && one.line > 0; });

    anchors_.erase(kept, anchors_.end());

    if (here_.at > at) here_ = {0, 0};

    if (counted_.has_value())
        counted_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*counted_) + lines);
}

// Bajt początku wiersza; pustka znaczy „za końcem" — albo „jeszcze nie wiadomo", gdy `stalled()`.
// Pod zamówionym oknem niczego nie zapamiętujemy: `here_` i liczba wierszy zostają, jakie były,
// bo wynik przejścia po nieobecnych bajtach nic nie znaczy. Kotwice postawione **przed**
// zatrzymaniem zostają — liczyły się z prawdziwych bajtów.
std::optional<std::size_t> Spool::at(std::size_t which) {
    stalled_ = false;

    if (counted_.has_value() && which >= *counted_) return {};

    if (which == here_.line) return here_.at;

    // Z dwóch dróg bierzemy krótszą: dobieg od kotwicy w przód albo cofanie się od miejsca,
    // w którym stoimy. Przy strzałce w górę to zawsze jest jeden krok w tył.
    if (which < here_.line && here_.line - which <= which - nearest(which).line) {
        std::size_t go = here_.at;

        for (std::size_t one = here_.line; one > which; --one) {
            go = back(go);

            if (stalled_) return {};
        }

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

        if (stalled_) return {};

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
// i po bajcie, więc zaczynamy od ostatniej leżącej nie dalej niż szukane miejsce — szukaną
// połówkowo, jak w `nearest`. Pod zamówionym oknem oddaje ostatni wiersz, do którego doszła —
// wołający pyta znowu po `awoken`.
std::size_t Spool::found(std::size_t at) {
    stalled_ = false;

    if (whole_ == 0) return 0;

    const std::size_t want = at < whole_ ? at : whole_ - 1;

    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), want,
                                       [](std::size_t byte, const Anchor& one) { return byte < one.at; });

    Anchor from = *std::prev(past);

    if (here_.at <= want && here_.at > from.at) from = here_;

    std::size_t go = from.at;
    std::size_t which = from.line;

    while (go < whole_) {
        const std::size_t next = forth(go, nullptr);

        if (stalled_) return which;

        if (next > want) break;

        go = next;

        ++which;

        anchored(which, go);
    }

    here_ = {which, go};

    return which;
}

// Wiersze od `which`: tyle, ile poproszono, albo mniej — bo dokument się skończył, albo bo okno
// jeszcze nie doszło (`stalled()`); pustka zamiast wektora, gdy nie ma nawet pierwszego. Wiersz
// przerwany brakiem okna nie wchodzi: wszedłby urwany i przy następnym pytaniu byłby inny.
//
// Gdy okno kończy się tuż za tym, co oddajemy, następne zamawiamy **od razu**, nie czekając na
// pytanie — przewijanie w dół idzie wtedy bez przystanków, bo okno jest już w drodze, gdy człowiek
// naciska strzałkę.
std::optional<std::vector<std::string>> Spool::lines(std::size_t which, std::size_t most) {
    std::vector<std::string> made;

    const std::optional<std::size_t> from = at(which);

    if (!from.has_value()) {
        if (stalled_) return {};

        return made;
    }

    made.reserve(most);

    std::size_t go = *from;
    std::size_t began = *from;
    std::size_t one = which;
    std::string said;

    while (made.size() < most && go < whole_) {
        const std::size_t next = forth(go, &said);

        if (stalled_) break;

        began = go;
        go = next;

        made.push_back(said);

        ++one;

        anchored(one, go);
    }

    if (!stalled_ && go >= whole_) counted_ = one;

    if (!made.empty()) here_ = {one - 1, began};

    if (made.empty() && stalled_) return {};

    if (feed_ && !stalled_ && !busy_ && go < whole_) {
        const std::size_t end = chunked_ + chunk_.size();

        if (end < whole_ && end - go < window / 2) ask(end, end + window);
    }

    return made;
}

// Przejście do końca po to, żeby znać liczbę wierszy: dla źródła synchronicznego od ręki, dla
// asynchronicznego okno po oknie — każde przyjście okna popycha je dalej, aż `counted()` ma
// wartość. Kotwice stawiane po drodze zostają, więc `End` po policzeniu ląduje przy znanym punkcie.
void Spool::counting() {
    if (counted_.has_value()) return;

    counting_ = true;

    (void)at(SIZE_MAX);
}

void Spool::awoken(std::function<void()> given) { awoken_ = std::move(given); }

}

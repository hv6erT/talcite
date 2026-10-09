#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/column.hpp"
#include "talcite/fs.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/mark.hpp"
#include "talcite/promise.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/view.hpp"
#include "talcite/viewer.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

    std::function<void()> sized;

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

    // Górny pasek dopełnia się do szerokości okna, więc po zmianie rozmiaru trzeba go złożyć
    // od nowa. Zdarzenie i tak przechodzi przez korzeń, więc nie ma czego dokładać.
    bool handle(talcite::ui::SizeEvent&) override {
        if (sized) sized();

        return false;
    }

private:
    talcite::Term& term_;
};

// **Tekst wbudowany w program.** Podgląd ma pokazywać sam siebie także wtedy, gdy nikt nie podał
// ścieżki — a przy okazji jest tu wszystko, co potrafi się zepsuć: akapit dłuższy niż okno,
// tabulatory, pusty wiersz i jeden wiersz bardzo długi.
constexpr const char* sample =
    "talcite — podgląd tekstu\n"
    "\n"
    "Ten tekst jest wbudowany w demo. Uruchomienie z argumentem otwiera podany plik, i wtedy\n"
    "podgląd bierze go **strumieniowo**: czyta tylko te kawałki, które akurat są potrzebne, więc\n"
    "otwarcie dziennika na dwa gigabajty kosztuje tyle samo, co otwarcie tej notki.\n"
    "\n"
    "Klawisze\n"
    "\tF2\tzawijanie wierszy\n"
    "\tF3\tnastępne zaznaczenie\n"
    "\tF4\tpozycja w wierszach albo w bajtach\n"
    "\tF5\tpoprzednie zaznaczenie\n"
    "\tF10\twyjście\n"
    "\n"
    "Strzałki, PageUp, PageDown, Home i End przewijają. Kółko myszy też.\n"
    "\n"
    "Co widać na górnym pasku\n"
    "\n"
    "Po lewej źródło, po prawej położenie. Przy tekście wbudowanym liczba wierszy jest znana od\n"
    "razu, bo dokument leży w pamięci i policzenie go kosztuje mikrosekundy. Przy pliku czytanym\n"
    "strumieniowo liczba wierszy jest **nieznana** i pasek pokazuje pytajnik — dopóki nie "
    "dojdziesz\n"
    "do końca, choćby klawiszem End. Wtedy pytajnik znika i już nie wraca. To jest dokładnie to\n"
    "samo, co znaczy plus w Midnight Commanderze.\n"
    "\n"
    "Pozycja bajtowa jest znana zawsze, bo obie drogi prowadzą przez bajty. Gdyby podgląd dostał\n"
    "źródło wierszowe — takie, które samo oddaje gotowe wiersze i o bajtach nic nie wie — pozycja\n"
    "bajtowa byłaby pusta, a pasek pokazywałby wtedy tylko wiersze.\n"
    "\n"
    "Zawijanie\n"
    "\n"
    "Poniżej jest jeden wiersz dłuższy niż jakiekolwiek okno. Przy włączonym zawijaniu rozłoży "
    "się\n"
    "na kilka wierszy ekranu, a strzałka w dół przejdzie przez nie po kolei, nie przeskakując od\n"
    "razu do następnego wiersza dokumentu. Po wyłączeniu zawijania ten sam wiersz urwie się na\n"
    "krawędzi, a numer wiersza zostanie ten sam — przełączenie nigdy nie przenosi w inne miejsce\n"
    "dokumentu.\n"
    "\n"
    "Raz dwa trzy cztery pięć sześć siedem osiem dziewięć dziesięć jedenaście dwanaście trzynaście "
    "czternaście piętnaście szesnaście siedemnaście osiemnaście dziewiętnaście dwadzieścia "
    "dwadzieścia jeden dwadzieścia dwa dwadzieścia trzy dwadzieścia cztery dwadzieścia pięć "
    "dwadzieścia sześć dwadzieścia siedem dwadzieścia osiem dwadzieścia dziewięć trzydzieści.\n"
    "\n"
    "Tabulatory\n"
    "\n"
    "kolumna\tdruga\ttrzecia\n"
    "a\tbb\tccc\n"
    "dłuższe\tpole\tobok\n"
    "\n"
    "Tabulator rozwija się do ośmiu kolumn dopiero przy rysowaniu. W treści zostaje jednym bajtem "
    "—\n"
    "inaczej pozycja bajtowa przestałaby się zgadzać z plikiem na dysku.\n"
    "\n"
    "Znaki szersze niż kratka\n"
    "\n"
    "你好世界 — każdy z tych znaków zajmuje dwie kolumny, więc cięcie wiersza liczy kolumny, a "
    "nie\n"
    "znaki ani bajty. Przy wąskim oknie widać, że żaden z nich nie wystaje poza krawędź.\n"
    "\n"
    "Koniec.\n";

talcite::Bytes lump(const std::string& what) {
    talcite::Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

// **Szukanie robi aplikacja, nie podgląd.** Widżet dostaje gotowe zakresy i tylko je maluje —
// tak samo, jak `Decoration.mark` w CodeMirror nic nie wie o tym, skąd się wziął. Numeracja
// wierszy musi się zgadzać ze `Spool`: koniec wiersza na końcu tekstu nie robi pustego wiersza
// po sobie.
std::vector<talcite::ui::Mark> hunted(const std::string& what, const std::string& word) {
    std::vector<talcite::ui::Mark> made;

    std::size_t line = 0;

    for (std::size_t at = 0;;) {
        const std::size_t end = what.find('\n', at);
        const std::size_t upto = end == std::string::npos ? what.size() : end;

        const std::string one = what.substr(at, upto - at);

        for (std::size_t got = one.find(word); got != std::string::npos;
             got = one.find(word, got + 1))
            made.push_back(talcite::ui::Mark{line, got, got + word.size(), "talcite.viewer.mark"});

        ++line;

        if (end == std::string::npos) break;

        at = end + 1;

        if (at == what.size()) break;
    }

    return made;
}

talcite::Style turned() {
    talcite::Style made{};

    made.reverse = true;

    return made;
}

}

int main(int argc, char** argv) {
    talcite::Term term;

    Sheet sheet{term};

    // Jedna własna rola ponad wbudowaną skórkę: konstruktor `Skin` **dokłada** do wartości
    // wbudowanych, więc reszta wyglądu zostaje nietknięta. Przedrostek `demo.`, bo nazwy
    // biblioteki zaczynają się od `talcite.` i cudze nie mają prawa ich przykrywać.
    sheet.skin(talcite::ui::Skin{{{"demo.bar", turned()}}});

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto bar = std::make_shared<talcite::ui::Span>(std::string{});
    const auto shown = std::make_shared<talcite::ui::Viewer>(talcite::ui::ScrollShape{true});

    bar->role("demo.bar");
    bar->wrapped(false);

    column->children().add(bar);
    column->children().add(shown);

    legend->content(column);

    sheet.children().add(legend);

    const auto source = std::make_shared<std::string>("tekst wbudowany");
    const auto counted = std::make_shared<bool>(false);

    // Położenie w wierszach albo w bajtach — jedno i drugie podgląd oddaje, a wybór należy do
    // aplikacji. Pytajnik znaczy „jeszcze nie wiem", nie „nie ma".
    const auto placed = [shown, counted] {
        if (*counted) {
            if (!shown->at().has_value()) return std::string{"bajty nieznane"};

            return std::to_string(*shown->at()) + "/" + std::to_string(*shown->whole());
        }

        if (!shown->first().has_value()) return std::string{"pusto"};

        std::string made = "wiersz " + std::to_string(*shown->first() + 1) + "/";

        made += shown->lines().has_value() ? std::to_string(*shown->lines()) : std::string{"?"};

        return made;
    };

    const auto naming = [&term, bar, source, placed] {
        const std::string left = *source;
        const std::string right = placed() + "  UTF-8";

        const int room =
            term.size().wide - talcite::measured(left).wide - talcite::measured(right).wide;

        bar->text(left + std::string(room > 1 ? static_cast<std::size_t>(room) : 1, ' ') + right);
    };

    sheet.sized = naming;

    // **Nasłuch wisi na kolumnie, nie na pasku.** Zdarzenie idzie bąbelkiem w górę od podglądu,
    // a pasek jest jego rodzeństwem — na nim nie usłyszałby niczego. Obserwator oddaje fałsz, żeby
    // nie zasłonić zdarzenia nikomu wyżej.
    (void)column->on<talcite::ui::ScrollEvent>([naming](talcite::ui::ScrollEvent&) {
        naming();

        return false;
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Zawijaj"), [shown, naming] {
        shown->wrapped(!shown->wrapped());

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Dalej"),
                      [shown] { (void)shown->step(1); });

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Wstecz"),
                      [shown] { (void)shown->step(-1); });

    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Pozycja"), [counted, naming] {
        *counted = !*counted;

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjście"),
                      [&term] { term.stop(); });

    // **Plik czytany strumieniowo i asynchronicznie.** `fs::open` i `File::read` idą przez pulę,
    // a podgląd dostaje `Feed`, czyli źródło oddające obietnicę: żadne okno nie czeka na dysk na wątku
    // głównym. Otwarcie też nie czeka — do jego przyjścia podgląd jest pusty. Uchwyt pliku żyje
    // w domknięciu, więc nikt go nie zamknie wcześniej niż podgląd.
    std::optional<talcite::Promise<void>> opening;

    if (argc > 1) {
        const std::filesystem::path where = talcite::fs::from_string(argv[1]);

        opening.emplace(
            talcite::fs::open(where)
                .then([](talcite::fs::File file) {
                    return file.size().then([file](std::uintmax_t whole) {
                        return std::pair{file, static_cast<std::size_t>(whole)};
                    });
                })
                .here()
                .then([shown, source, where](std::pair<talcite::fs::File, std::size_t> got) {
                    *source = talcite::fs::to_string(where);

                    shown->hold(
                        [file = got.first](std::size_t at, std::size_t most) { return file.read(at, most); },
                        got.second);
                })
                .fail([shown, source, where](const std::exception&) {
                    *source = "nie udało się otworzyć: " + talcite::fs::to_string(where);

                    shown->hold(lump(sample));
                }));

        opening->start();
    } else {
        const std::string said = sample;

        shown->hold(lump(said));

        // Zaznaczamy każde wystąpienie jednego słowa — po to, żeby było co malować i po czym
        // skakać. Przy pliku z argumentu nie robimy tego: demo nie ma go w pamięci, a przechodzenie
        // dwóch gigabajtów po to, żeby coś podświetlić, byłoby dokładnie tym szukaniem, którego
        // jeszcze nie ma.
        shown->marks().all(hunted(said, "wiersz"));
    }

    term.show(sheet);

    // Bez ogniska klawisze kończą wędrówkę na korzeniu i legenda nigdy ich nie zobaczy. Podgląd
    // bierze je sam, więc `Pane` nie jest tu potrzebny.
    shown->focus();

    naming();

    term.run();

    return 0;
}

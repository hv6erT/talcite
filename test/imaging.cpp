#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "talcite/image.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

[[nodiscard]] Bytes lump(std::string_view hex) {
    Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) -> int {
        if (one >= '0' && one <= '9') return one - '0';

        return one - 'a' + 10;
    };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(std::byte{static_cast<std::uint8_t>(value(hex[at]) * 16 +
                                                            value(hex[at + 1]))});

    return made;
}

// **Gęstość siatki i wymiar kratki leżą na korzeniu**, bo są faktem o powierzchni, nie o widżecie.
// Próba jest tu w tej samej roli co `Term`: to ona je stawia.
class Sheet : public ui::Root {
public:
    explicit Sheet(ui::Size given = {10, 20}, ui::Blocks kind = ui::Blocks::sextants) {
        cell(given);
        blocks(kind);
    }
};

// Węzeł proszący o obudzenie o stałej porze — po to, żeby sprawdzić samo zbieranie w korzeniu,
// bez obrazka i bez zegara.
class Ticking : public ui::Node {
public:
    explicit Ticking(int after) : after_(after) {}

    [[nodiscard]] int takes() const override { return 4; }

    [[nodiscard]] std::optional<std::chrono::milliseconds> due() const override {
        return std::chrono::milliseconds{after_};
    }

private:
    int after_ = 0;
};

// GIF 16x12, cztery klatki po 40, 100, 100 i 120 ms.
constexpr std::string_view leaping =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

// GIF 4x3, dwie klatki: pierwsza dwadzieścia milisekund, druga trzy sekundy. Krótka na początku
// i długa po niej znaczy, że **po stu milisekundach jesteśmy w drugiej na pewno** — z prawie
// trzysekundowym zapasem, więc próba nie zależy od obciążenia maszyny.
constexpr std::string_view quick =
    "47494638396104000300810000ff000000000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90400020000002c00000000040003000008080001081c485060400021f904012c0101002c000000000400"
    "03008100ff0000000000000000000008080001081c48506040003b";

[[nodiscard]] std::shared_ptr<ui::Image> shown(std::string_view hex) {
    const auto one = std::make_shared<ui::Image>();

    one->hold(lump(hex));

    return one;
}

}

// --- Co widżet wie przed układem ---------------------------------------------------------------

TRIAL("Image: pusty nic nie niesie i o nic nie prosi") {
    const ui::Image one;

    ought(!one.held());
    same(one.wants({40, 20}).wide, 0);
    ought(!one.due().has_value());
}

// Rozmiar musi być znany **przed** malowaniem, bo pyta o niego układ — więc `hold` rozpakowuje
// jedną klatkę i o niej zapomina. Liczba klatek znana jest dopiero po pierwszym układzie, bo GIF
// nie mówi jej w nagłówku i policzyć ją znaczy przejść cały plik.
TRIAL("Image: po wczytaniu zna format i rozmiar") {
    const auto one = shown(leaping);

    ought(one->held());
    same(static_cast<int>(one->kind()), static_cast<int>(Picture::gif));
    same(one->shape().wide, 16);
    same(one->shape().high, 12);
}

TRIAL("Image: nieobsługiwany format nie niesie nic") {
    const auto one = std::make_shared<ui::Image>();

    one->hold(lump("ffd8ffe000104a464946000102000000"));

    same(static_cast<int>(one->kind()), static_cast<int>(Picture::jpeg));
    ought(!one->held());
}

// --- Proporcja ---------------------------------------------------------------------------------

// **Kratka nie jest kwadratem.** Obrazek 16 na 12 przy kratce 10 na 20 zajmuje czterdzieści kratek
// wszerz i piętnaście wzdłuż, bo wysokość liczy się z prawdziwej proporcji kratki.
TRIAL("Image: proporcja liczy się z wymiaru kratki") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);

    same(one->wants({40, 20}).wide, 40);
    same(one->wants({40, 20}).high, 15);
}

// Poza drzewem nie ma kogo zapytać o kratkę, więc widżet przyjmuje jeden do dwóch — tak samo, jak
// gdy terminal nie odpowie na `16t`.
TRIAL("Image: bez wymiaru kratki przyjmujemy jeden do dwóch") {
    const auto one = shown(leaping);

    same(one->wants({40, 20}).wide, 40);
    same(one->wants({40, 20}).high, 15);
}

// --- Ustawienie w polu -------------------------------------------------------------------------

// Podgląd staje **na środku**, jak w Midnight Commanderze, a reszta pola zostaje nierysowana —
// czyli widać przez nią tło rodzica.
TRIAL("Image: obrazek staje na środku pola") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {40, 20}});

    // Piętnaście wierszy w polu dwudziestu: dwa puste u góry, trzy u dołu.
    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::none));
    same(static_cast<int>(made.at({0, 1}).shown), static_cast<int>(Shown::none));
    ought(made.at({0, 2}).shown != Shown::none);
    ought(made.at({0, 16}).shown != Shown::none);
    same(static_cast<int>(made.at({0, 17}).shown), static_cast<int>(Shown::none));
    same(static_cast<int>(made.at({0, 19}).shown), static_cast<int>(Shown::none));
}

// --- Zegar -------------------------------------------------------------------------------------

TRIAL("Image: przed układem nie ma o co prosić") {
    const auto one = shown(leaping);

    ought(!one->due().has_value());
}

TRIAL("Image: rozwinięta animacja prosi o obudzenie") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    ought(one->due().has_value());
    ought(*one->due() > std::chrono::milliseconds{0});
    ought(*one->due() <= std::chrono::milliseconds{40});
}

// **Ustawienie klatki jest przewinięciem**: przesuwa początek odliczania tak, jakby animacja
// właśnie do niej dobiegła. Dzięki temu rachunek czasu sprawdza się bez czekania — po przewinięciu
// zostaje dokładnie tyle, ile trwa ta klatka.
TRIAL("Image: przewinięcie ustawia klatkę razem z czasem") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    same(one->frames(), std::size_t{4});

    const int these[4] = {40, 100, 100, 120};

    bool held = true;

    for (std::size_t at = 0; at < 4; ++at) {
        one->still(at);

        held = held && one->still() == at;
        held = held && one->due()->count() <= these[at];
        held = held && one->due()->count() > these[at] - 10;
    }

    ought(held);
}

TRIAL("Image: przewinięcie poza koniec zatrzymuje się na ostatniej klatce") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    one->still(9);

    same(one->still(), std::size_t{3});
}

// Zatrzymanie zamraża **tę klatkę, którą widać**, a nie zerową — inaczej pauza przeskakiwałaby
// obraz.
TRIAL("Image: zatrzymanie zamraża widoczną klatkę") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    one->still(2);
    one->play(false);

    ought(!one->play());
    same(one->still(), std::size_t{2});
    ought(!one->due().has_value());
}

// Jedyna próba, która naprawdę czeka. Pierwsza klatka trwa dwadzieścia milisekund, druga trzy
// sekundy — po stu milisekundach jesteśmy w drugiej z prawie trzysekundowym zapasem.
TRIAL("Image: zegar przesuwa klatkę sam") {
    Sheet sheet;

    const auto one = shown(quick);

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {20, 10}});

    one->still(0);

    same(one->still(), std::size_t{0});

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    same(one->still(), std::size_t{1});
}

// Klatki bez opóźnienia to nie animacja, tylko obrazek. Dekoder GIF-a takich nie wypuszcza, ale
// obrazek zrobiony w kodzie owszem — i wtedy „która klatka teraz" nie ma odpowiedzi.
TRIAL("Image: klatki bez opóźnień nie są animacją") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Image>();

    Bytes first(4 * 4, std::byte{200});
    Bytes second(4 * 4, std::byte{40});

    one->hold(Pixels{{2, 2},
                     {Still{std::move(first), std::chrono::milliseconds{0}},
                      Still{std::move(second), std::chrono::milliseconds{0}}}});

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {20, 10}});

    ought(one->held());
    ought(!one->due().has_value());
}

// --- Zbieranie w korzeniu ----------------------------------------------------------------------

// Korzeń **nie ma własnej prośby** — zbiera cudze i oddaje najbliższą. Dzięki temu `Term` pyta
// jedno miejsce i nie musi wiedzieć, że w środku siedzi obrazek.
TRIAL("Root: bierze najbliższą prośbę z poddrzewa") {
    Sheet sheet;

    const auto outer = std::make_shared<Ticking>(500);
    const auto inner = std::make_shared<Ticking>(70);
    const auto quiet = std::make_shared<ui::Node>();

    // Korzeń przyjmuje jedno dziecko, więc prośby leżą **głębiej niż jeden poziom** — i o to
    // w tej próbie chodzi: zbieranie ma zejść całą drogą w dół.
    outer->children().add(quiet);
    outer->children().add(inner);
    sheet.children().add(outer);

    ought(sheet.due().has_value());
    same(sheet.due()->count(), 70LL);
}

TRIAL("Root: drzewo bez animacji o nic nie prosi") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Node>();
    const auto two = std::make_shared<ui::Node>();

    one->children().add(two);
    sheet.children().add(one);

    ought(!sheet.due().has_value());
}

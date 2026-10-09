#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "talcite/frame.hpp"
#include "talcite/image.hpp"
#include "talcite/loop.hpp"
#include "talcite/picture.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "faking.hpp"

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

// **Gęstość siatki, wymiar kratki, barwność i znaki leżą na korzeniu**, bo są faktem o powierzchni,
// nie o widżecie. Próba jest tu w tej samej roli co `Term`: to ona je stawia — domyślnie jak terminal,
// który umie wszystko.
class Sheet : public ui::Root {
public:
    explicit Sheet(ui::Size given = {10, 20}, ui::Blocks kind = ui::Blocks::sextants) {
        cell(given);
        blocks(kind);
        colors(Colors::rgb);
        unicode(true);
    }
};

// Węzeł proszący o obudzenie o stałej porze — po to, żeby sprawdzić samo zbieranie w korzeniu,
// bez obrazka i bez zegara.
class Ticking : public ui::Node {
public:
    explicit Ticking(int after) : after_(after) {}

    [[nodiscard]] int capacity() const override { return 4; }

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

// Widżet na atrapie: próby sprawdzają widżet — proporcję, zegar, przewijanie — a nie dekoder,
// który mieszka w talcite-adapters razem ze swoimi próbami. Szew i bajty idą w domknięciu
// `figured`; widżet dostaje źródło.
[[nodiscard]] std::shared_ptr<ui::Image> shown(std::string_view hex) {
    const auto one = std::make_shared<ui::Image>();

    one->hold(figured(lump(hex), faking::images()));

    return one;
}

// Plik dekoduje się na puli po pierwszym malowaniu i wraca przez pętlę: obracamy i malujemy, aż
// zamówienie wróci — jak przy szpuli.
[[nodiscard]] bool settled(Loop& loop, ui::Root& top, const ui::Image& one, ui::Size room) {
    for (int tries = 0; tries < 500; ++tries) {
        (void)top.render({{0, 0}, room});
        (void)loop.turn();

        if (!one.busy() && (one.frames() > 0 || !one.held())) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return false;
}

}

// --- Co widżet wie przed układem ---------------------------------------------------------------

// **Dekodowanie liczy, więc idzie torem `cpu`.** Próba zajmuje cały sufit toru liczącego
// czekającymi zadaniami: źródło obrazu ma wtedy stać w kolejce, a ruszyć dopiero po zwolnieniu
// miejsca. Na torze zwykłym ruszyłoby od razu, obok zajętych.
//
// Stan zajmujących zadań leży we wspólnym wskaźniku, nie na stosie próby: zadanie, które śpi
// w chwili zwolnienia, budzi się już po jej końcu, a odczyt z martwej ramki potrafił zwrócić
// „jeszcze nie" i zawiesić robotnika na zawsze.
TRIAL("figured: dekodowanie biegnie torem cpu") {
    struct Hold {
        std::atomic<bool> release{false};
        std::atomic<int> held{0};
        std::atomic<int> left{0};
    };

    const auto hold = std::make_shared<Hold>();

    const int most = static_cast<int>(pooling().most());

    for (int at = 0; at < most; ++at)
        (void)pooling().post(
            [hold] {
                ++hold->held;
                while (!hold->release.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                ++hold->left;
            },
            Lane::cpu);

    for (int at = 0; at < 500 && hold->held.load() < most; ++at)
        std::this_thread::sleep_for(std::chrono::milliseconds{2});

    Promise<Pixels> made =
        figured(lump(leaping), faking::images()).fetch(ui::Size{4, 4});

    made.start();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const bool early = made.progress() == 1.0;

    hold->release.store(true);

    (void)made.await();

    for (int at = 0; at < 500 && hold->left.load() < most; ++at)
        std::this_thread::sleep_for(std::chrono::milliseconds{2});

    ought(!early);
    same(hold->left.load(), most);
}

TRIAL("Image: pusty nic nie niesie i o nic nie prosi") {
    const ui::Image one;

    ought(!one.held());
    same(one.wants({40, 20}).wide, 0);
    ought(!one.due().has_value());
}

// Rozmiar musi być znany **przed** malowaniem, bo pyta o niego układ — więc `hold` czyta go
// z nagłówka (`shaped`), bez dekodera i od ręki. Liczba klatek znana jest dopiero, gdy wróci
// zamówienie po pierwszym układzie, bo GIF nie mówi jej w nagłówku i policzyć ją znaczy przejść
// cały plik.
TRIAL("Image: po wczytaniu zna format i rozmiar, klatki dopiero po zamówieniu") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    ought(one->held());
    ought(!one->busy());
    same(static_cast<int>(one->format()), static_cast<int>(Format::gif));
    same(one->size().wide, 16);
    same(one->size().high, 12);
    same(one->frames(), std::size_t{0});

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    ought(one->busy());
    same(one->frames(), std::size_t{0});

    ought(settled(loop, sheet, *one, {40, 20}));
    ought(!one->busy());
    same(one->frames(), std::size_t{4});
}

// Odmowa dostawcy i brak dostawcy kończą się tak samo: widżet pusty, format z nagłówka. To jedyne
// miejsce, gdzie wyjątek szwu nie idzie wyżej — widżet ma pokazać, co może, a legenda po
// `kind()` powie „JPEG, bez dostawcy".
TRIAL("Image: odmowa dostawcy nie niesie nic, format zostaje z nagłówka") {
    Loop loop;
    Sheet sheet;

    const auto one = std::make_shared<ui::Image>();

    one->hold(figured(lump("ffd8ffe000104a464946000102000000"), faking::images()));

    same(static_cast<int>(one->format()), static_cast<int>(Format::jpeg));
    ought(!one->held());

    // Nagłówek z wymiarami, ale dostawca odmawia: rozmiar jest do pierwszego zamówienia, potem
    // widżet pusty — pokazuje, co może, a legenda po `kind()` powie, czego nie.
    const auto other = std::make_shared<ui::Image>();

    other->hold(figured(lump("ffd8ffe100104578696600000000000000000000ffc0001108001000200301220002110103110100"),
                        faking::images()));

    ought(other->held());

    sheet.children().add(other);

    ought(settled(loop, sheet, *other, {40, 20}));
    ought(!other->held());
    same(static_cast<int>(other->format()), static_cast<int>(Format::jpeg));
}

TRIAL("Image: bez dostawcy plik zostaje pusty, a obrazek z kodu działa") {
    const auto one = std::make_shared<ui::Image>();

    one->hold(figured(lump(leaping), {}));

    same(static_cast<int>(one->format()), static_cast<int>(Format::gif));
    ought(!one->held());

    one->hold(Pixels{{2, 2}, {Still{Bytes(16, std::byte{200}), std::chrono::milliseconds{0}}}});

    ought(one->held());

    one->clear();

    ought(!one->held());
    same(one->frames(), std::size_t{0});
    same(static_cast<int>(one->format()), static_cast<int>(Format::none));
}

// **Widżet zna tylko źródło.** Bajty, szew, dekoder — nic z tego nie jest mu potrzebne: `Figure`
// zrobiony ręcznie, z funkcją oddającą piksele w żądanym rozmiarze, wystarcza. Rozmiar, o który
// prosi, jest rozmiarem kratek pomnożonym przez ziarno siatki — przy znakach blokowych 1×2 na
// kratkę — a po zmianie miejsca zamawia od nowa, z nowym rozmiarem; do tego czasu maluje stare.
TRIAL("Image: źródło woła się z rozmiarem z układu, a po zmianie miejsca od nowa") {
    Loop loop;
    Sheet sheet;

    const auto asked = std::make_shared<std::vector<ui::Size>>();

    Figure made;

    made.format = Format::png;
    made.size = {16, 12};
    made.fetch = [asked](ui::Size want) {
        asked->push_back(want);

        return Promise<Pixels>{[want](auto resolve, auto) {
            resolve(Pixels{want, {Still{Bytes(static_cast<std::size_t>(want.wide * want.high) * 4, std::byte{7}),
                                        std::chrono::milliseconds{0}}}});
        }};
    };

    const auto one = std::make_shared<ui::Image>();

    one->hold(made);

    same(static_cast<int>(one->format()), static_cast<int>(Format::png));
    ought(one->held());
    same(one->frames(), std::size_t{0});
    ought(asked->empty());

    sheet.children().add(one);

    ought(settled(loop, sheet, *one, {40, 20}));
    same(one->frames(), std::size_t{1});
    same(asked->size(), std::size_t{1});
    ought(asked->front().wide > 0);
    ought(asked->front().high > 0);

    // To samo miejsce — nic nowego nie zamawia.
    (void)sheet.render({{0, 0}, {40, 20}});
    (void)loop.turn();

    same(asked->size(), std::size_t{1});

    // Mniejsze miejsce — zamawia od nowa, z mniejszym rozmiarem.
    ought(settled(loop, sheet, *one, {20, 10}));
    same(asked->size(), std::size_t{2});
    ought(asked->back().wide < asked->front().wide);

    // Bez Unicode ziarno jest inne, więc zamawia od nowa — raz, a nie przy każdej klatce.
    sheet.unicode(false);

    ought(settled(loop, sheet, *one, {20, 10}));
    same(asked->size(), std::size_t{3});

    (void)sheet.render({{0, 0}, {20, 10}});
    (void)loop.turn();
    (void)sheet.render({{0, 0}, {20, 10}});

    same(asked->size(), std::size_t{3});
}

// Odmowa źródła — odrzucona obietnica, nie pusty wynik — opróżnia widżet tak samo: rodzaj zostaje,
// kształt znika, żeby układ przestał prosić o miejsce.
TRIAL("Image: odrzucona obietnica źródła opróżnia widżet, rodzaj zostaje") {
    Loop loop;
    Sheet sheet;

    Figure made;

    made.format = Format::bmp;
    made.size = {16, 12};
    made.fetch = [](ui::Size) {
        return Promise<Pixels>{[](auto, auto reject) { reject(std::make_exception_ptr(std::runtime_error{"no"})); }};
    };

    const auto one = std::make_shared<ui::Image>();

    one->hold(made);
    ought(one->held());

    sheet.children().add(one);

    ought(settled(loop, sheet, *one, {40, 20}));
    ought(!one->held());
    ought(!one->busy());
    same(static_cast<int>(one->format()), static_cast<int>(Format::bmp));
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
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);

    ought(settled(loop, sheet, *one, {40, 20}));

    const View made = sheet.render({{0, 0}, {40, 20}});

    // Piętnaście wierszy w polu dwudziestu: dwa puste u góry, trzy u dołu.
    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::none));
    same(static_cast<int>(made.at({0, 1}).shown), static_cast<int>(Shown::none));
    ought(made.at({0, 2}).shown != Shown::none);
    ought(made.at({0, 16}).shown != Shown::none);
    same(static_cast<int>(made.at({0, 17}).shown), static_cast<int>(Shown::none));
    same(static_cast<int>(made.at({0, 19}).shown), static_cast<int>(Shown::none));
}

// **Brzeg obrazu w oknie ma tło okna.** Kratka pokryta częściowo nie ma papieru z roli obrazu, więc idzie
// tuszem i bierze tło ramki, w której stoi. Dawniej zostawała przezroczysta aż do terminala, a ten
// malował tam swoje tło — czarne kratki wokół logo w granatowym oknie twa.
TRIAL("Image: brzeg obrazu bierze tło okna, w którym stoi") {
    Sheet sheet;

    Style frame{};

    frame.background = Color{false, 0, 0, 160};

    sheet.skin(ui::Skin{{{"talcite.frame", frame}}});

    // Obrazek 4x6: lewa kolumna czerwona, reszta przezroczysta — w sekstantach dwie kratki z sylwetką.
    Bytes bytes(4 * 6 * 4, std::byte{0});

    for (int y = 0; y < 6; ++y) {
        const std::size_t at = static_cast<std::size_t>(y * 4) * 4;

        bytes[at] = std::byte{200};
        bytes[at + 3] = std::byte{255};
    }

    const auto one = std::make_shared<ui::Image>();

    one->hold(Pixels{{4, 6}, {Still{bytes, std::chrono::milliseconds{0}}}});

    const auto box = std::make_shared<ui::Frame>(std::string{});

    box->children().add(one);
    sheet.children().add(box);

    const View made = sheet.render({{0, 0}, {6, 4}});

    int seen = 0;

    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x) {
            const Cell cell = made.at({x, y});

            if (cell.style.foreground != (Color{false, 200, 0, 0})) continue;

            ++seen;

            same(cell.style.background, (Color{false, 0, 0, 160}));
        }

    ought(seen > 0);
}

// --- Zegar -------------------------------------------------------------------------------------

TRIAL("Image: przed układem nie ma o co prosić") {
    const auto one = shown(leaping);

    ought(!one->due().has_value());
}

TRIAL("Image: rozwinięta animacja prosi o obudzenie") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    ought(one->due().has_value());
    ought(*one->due() > std::chrono::milliseconds{0});
    ought(*one->due() <= std::chrono::milliseconds{40});
}

// **Ustawienie klatki jest przewinięciem**: przesuwa początek odliczania tak, jakby animacja
// właśnie do niej dobiegła. Dzięki temu rachunek czasu sprawdza się bez czekania — po przewinięciu
// zostaje dokładnie tyle, ile trwa ta klatka.
TRIAL("Image: przewinięcie ustawia klatkę razem z czasem") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

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
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    one->still(9);

    same(one->still(), std::size_t{3});
}

// Zatrzymanie zamraża **tę klatkę, którą widać**, a nie zerową — inaczej pauza przeskakiwałaby
// obraz.
TRIAL("Image: zatrzymanie zamraża widoczną klatkę") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    one->still(2);
    one->play(false);

    ought(!one->play());
    same(one->still(), std::size_t{2});
    ought(!one->due().has_value());
}

// Wznowienie rusza **od zamrożonej klatki**, a nie od tej, na którą wskazywałby zegar puszczony
// przy `hold`. Dawniej po pauzie obraz przeskakiwał o tyle klatek, ile minęło czasu. Klatka pierwsza
// trwa sto milisekund — tyle zapasu ma sprawdzenie zaraz po wznowieniu.
TRIAL("Image: wznowienie rusza od zamrożonej klatki") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    one->still(1);
    one->play(false);

    std::this_thread::sleep_for(std::chrono::milliseconds{250});

    one->play(true);

    ought(one->play());
    same(one->still(), std::size_t{1});
    ought(one->due().has_value());
}

// Jedyna próba, która naprawdę czeka. Pierwsza klatka trwa dwadzieścia milisekund, druga trzy
// sekundy — po stu milisekundach jesteśmy w drugiej z prawie trzysekundowym zapasem.
TRIAL("Image: zegar przesuwa klatkę sam") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(quick);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {20, 10}));

    one->still(0);

    same(one->still(), std::size_t{0});

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    same(one->still(), std::size_t{1});

    // Zatrzymanie zamraża klatkę, do której doszedł zegar — a nie tę, którą ostatnio ustawiono.
    one->play(false);

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

// --- Barwność i znaki z korzenia ---------------------------------------------------------------

// Obrazek w kodzie: czerwony u góry, przezroczysty u dołu, 8×8 — tyle, ile ma jedna kratka klocków.
Pixels halved() {
    Bytes made(8 * 8 * 4, std::byte{0});

    for (std::size_t at = 0; at < 8 * 4 * 4; at += 4) {
        made[at] = std::byte{220};
        made[at + 3] = std::byte{255};
    }

    return Pixels{{8, 8}, {Still{std::move(made), std::chrono::milliseconds{0}}}};
}

// Znaki bierze widżet z korzenia i przelicza obraz, gdy korzeń je zmieni: bez Unicode kratka ma
// siatkę 2×4 i znak z drabiny ASCII, z Unicode — klocki. Barwność też idzie z korzenia: w pełnych
// kolorach połówka ma barwę obrazu.
TRIAL("Image: znaki i barwność idą z korzenia, zmiana znaków przelicza obraz") {
    Sheet sheet{{0, 0}, ui::Blocks::basic};

    const auto one = std::make_shared<ui::Image>();

    one->hold(halved());

    sheet.children().add(one);

    const View drawn_blocks = sheet.render({{0, 0}, {1, 1}});

    same(drawn_blocks.at({0, 0}).letter, U'▀');
    same(drawn_blocks.at({0, 0}).style.foreground, (Color{false, 220, 0, 0}));

    sheet.unicode(false);

    const View drawn_ascii = sheet.render({{0, 0}, {1, 1}});

    same(drawn_ascii.at({0, 0}).letter, U'=');

    sheet.colors(Colors::mono);
    sheet.unicode(true);

    same(sheet.render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');

    // Barwność z korzenia: dwie barwy w pełnych kolorach to połówka, a w czarno-białym obraz
    // z przezroczystością (jeden piksel) to sama sylwetka — pełny klocek.
    Bytes two(8 * 8 * 4, std::byte{0});

    for (std::size_t at = 0; at < two.size(); at += 4) {
        two[at + (at < two.size() / 2 ? 0 : 2)] = std::byte{220};
        two[at + 3] = std::byte{255};
    }

    two[3] = std::byte{254};

    one->hold(Pixels{{8, 8}, {Still{std::move(two), std::chrono::milliseconds{0}}}});

    same(sheet.render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'█');

    sheet.colors(Colors::rgb);

    same(sheet.render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');

    one->hold(halved());

    // Poza drzewem: pełne kolory i Unicode — dwie barwy to połówka, nie sylwetka.
    const auto alone = std::make_shared<ui::Image>();

    alone->hold(halved());

    same(alone->render({{0, 0}, {1, 1}}, {{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');

    Bytes pair(8 * 8 * 4, std::byte{0});

    for (std::size_t at = 0; at < pair.size(); at += 4) {
        pair[at + (at < pair.size() / 2 ? 0 : 2)] = std::byte{220};
        pair[at + 3] = std::byte{255};
    }

    pair[3] = std::byte{254};

    alone->hold(Pixels{{8, 8}, {Still{std::move(pair), std::chrono::milliseconds{0}}}});

    same(alone->render({{0, 0}, {1, 1}}, {{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');
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

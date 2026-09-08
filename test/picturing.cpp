#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "talcite/pixels.hpp"

using namespace talcite;

namespace {

// Pliki próbne stoją tu **jako bajty**, nie jako opis, i to jest rozmyślne: wypisał je cudzy koder,
// a nie mój. Gdyby stały jako opis, próba sprawdzałaby zgodność mojego dekodera z moim koderem —
// czyli nic. Zgodność co do piksela z niezależnym dekoderem i z przeglądarką sprawdzona osobno,
// poza próbami; tutaj pilnujemy, żeby to, co raz uzgodnione, już się nie ruszyło.
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

struct Tone {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;

    bool operator==(const Tone&) const = default;
};

[[nodiscard]] Tone at(const Pixels& one, int x, int y, std::size_t still = 0) {
    const std::size_t where = (static_cast<std::size_t>(y) *
                                   static_cast<std::size_t>(one.size().wide) +
                               static_cast<std::size_t>(x)) *
                              4;

    const Bytes& bytes = one.stills()[still].bytes;

    return {std::to_integer<std::uint8_t>(bytes[where + 0]),
            std::to_integer<std::uint8_t>(bytes[where + 1]),
            std::to_integer<std::uint8_t>(bytes[where + 2]),
            std::to_integer<std::uint8_t>(bytes[where + 3])};
}

[[nodiscard]] long long after(const Pixels& one, std::size_t still) {
    return one.stills()[still].after.count();
}

constexpr std::string_view plain =
    "47494638376107000500810000000000ff000000ff000000ff2c0000000007000500000816000104103040a0"
    "8081051112345830e1c2000c0d0604003b";

constexpr std::string_view flat =
    "4749463839610b000900810000000000ff000000ff000000ff2c000000000b000900000213848c7731ecdd54"
    "4af0bd2957dd2163be615301003b";

constexpr std::string_view woven =
    "4749463839610b000900810000000000ff000000ff000000ff2c000000000b000900400212848c77c9ab0d9f"
    "902d0c8bafce7cfb0e0e05003b";

constexpr std::string_view clear =
    "47494638396107000500810000000000ff000000ff000000ff21f90401000000002c00000000070005000008"
    "16000104103040a08081051112345830e1c2000c0d0604003b";

constexpr std::string_view dot =
    "4749463837610100010081000009c81e0000000000000000002c000000000100010000080400010404003b";

constexpr std::string_view leaving =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

constexpr std::string_view clearing =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90409060000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90409060000002c03000200040004008100000000ff00000000000000080900"
    "03081c48b06080800021f90409060000002c0600040004000400810000000000ff0000000000000809000308"
    "1c48b06080800021f90409060000002c090006000400040081000000ff000000000000000008090003081c48"
    "b0608080003b";

constexpr std::string_view backing =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f9040c060000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f9040d060002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f9040d060002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f9040d06"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

// **Cały prawdziwy plik, bajt w bajt.** Reszta prób sprawdza pojedyncze mechanizmy na plikach
// dobranych tak, żeby dało się je przeczytać; ten jeden jest po to, żeby **cokolwiek w dekoderze
// nie ruszyło się niezauważenie**. Wypisał go cudzy koder i ma w środku wszystko naraz:
//
//   szesnaście obrazków, z tego **piętnaście z paletą lokalną**;
//   **wszystkie trzy sposoby sprzątania** — zostaw, do tła, przywróć poprzednie;
//   **piętnaście różnych rozmiarów prostokątów**, bo koder tnie każdą klatkę do zmienionego miejsca;
//   przezroczysty indeks;
//   opóźnienia zerowe i najkrótsze, czyli **zacisk do stu milisekund**.
//
// Dwa kilobajty w źródle próby. Tyle są warte.
constexpr std::string_view proof =
    "4749463839613000200081000000000025477100000000000021ff0b4e45545343415045322e30030100000021f90409"
    "040000002c00000000300020000008780001081c48b0a0c18308132a5cc8b0e1c100102346744851a1c48b15331abc28"
    "51a347811c277ed41812e24892254f662c1940a5cb970331c274c872e64296266d3ec4995327419e3d7d02002a5228d1"
    "a03a8fb6143af428d3a6449f42c529756acdaa3cabc64ca9f567c7ae60c38a1d4bb6acd9b31e03020021f9040d070000"
    "002c020003002c001900810000004a8ee228388800000008700005081c381080c18308132a5cb890a043861023327448"
    "50a2458b140b5edcd830a3008e20117afc1832e4c8922853aa5cc9b2a5cb970703c8940933e2cc9b346b26c4c953674c"
    "9e3d7d020d5a7328509d4689be4c8a1329d3994e9f0610fad42700a956af26cd6ad02857844d1106040021f904050a00"
    "00002c020003002c001c00810000006fd5532b299f00000008720001081c48b0a0c18308110a58c89061c287100b369c"
    "18b1e2c3890d2d6a3488d1e1c68f003a2e04f951a400921b4da25cc9b2a5cb973063ca9c491362809b376b1ac4c933a7"
    "4e003d83ea0c4ab42651a1338f1695a91469cca63d9342c529756a009a568d4e1ddaf427d0a35e07463d18100021f904"
    "09000000002c0200040029001c0081000000941cc42e1ab6000000086c0001081c48b0a0c183080b0a58c89061c28710"
    "05369c18b1e2c1890d2d6a1c88d1e1468d1d177e041972a4c590024caa5cc9b2a5cb973063ca9c49f361809b376b0ec4"
    "c93327cd9e4067021d2a736850984689be4c7ad425d39e489fe28c2a3540ccaa45a50a655a3329c2800021f9040d1000"
    "00002c020006001e001a0081000000b96335310bcd00000008600005081c381080c18308132a24c850a1c3870c093e9c"
    "883062418a142d0ac4985123c7891a057c1c49b2a4c9932853aa5cc9926580972f5dc29c192025cd9b276fea34a91327"
    "c99e3b4702f5f97128cd9f4661224d5a73e9509e49733e0510100021f90405010000002c020006001e001a0081000000"
    "b96335deaaa600000008620001081c48b0a0c1830203285cb810a143820c233e9c1891e1c487151b5e449851e1468e1d"
    "3f1eec1840a4c9932853aa5cc9d2a080972f5bc29c193325cd9b286fea3ca913a7c89e3b3f02f5b97128cd9f4661224d"
    "2ac024539e49730e5509f461400021f90409070000002c0200020017001e008100000003f117bc544c00000008620005"
    "081c381080c1830811125c98b0e1c185041d3a84585062428a022d5ec4a85121c78e20438a1c49b2a4c9932853360cc0"
    "9225c996305d828c49b323cd9b1a6fd694a813a7c39e3b570285c97368cba24603584c9ad3a84da0217bbe24aa526340"
    "0021f9040d0a0000002c020003000f00190081000000283888bf4563000000084e0005081c381080c18304131e4498b0"
    "e042000d1d2e8c28f021448a16290ab0c8b1a3c78f201f061839b223c9932517a25c7970a54b832e59c69c49b3a6cd9b"
    "3601dc849993e74c91314d9e7c18100021f904050d0000002c0100030010001900810000004d7ff9c2367a0000000851"
    "0001081c48b0a04001081326340840a143860e15428c887022458b11313a0c10c020c78f2039120c4952a4c092244fa2"
    "4cb992654b90005ec294f951e5cb813447b6f48892a14d933e830a1d4ab468c1800021f90409100000002c0100040010"
    "001400810000004a8ee272c66a000000084d0001081c28a060c1810303285cb8d0204286100d0a4808b1a14481152d3a"
    "049051a1c4831d037c9c1872248091280fa6442970e5c5962e0f1274891026cb9a3337e2dcc9b3a7cf9f4083ee0c0800"
    "21f9040d040000002c0200010012000f00810000004d7ff9970ddb00000008420001081c28508041830413163cc85080"
    "c2850d192a8c483121c588000268dcb8f1a2008e203d82e42872a4c69226518ebc98d1640096163d3a7ce8f121c18609"
    "03020021f90405070000002c020000001700100081000000bc544c50701000000008500001081c4870608083070b2a2c"
    "88b061c2850c1d3a846850a2448a002c5e84a8d122c78e0e05881c3912644392284d224449526500002c4baa84195340"
    "4690026bdabce931674d8213310a1d4a9460400021f904090a0000002c020000001d000e0081000000e19bbd53612700"
    "000008510001081c48b0a0c00008132634c8d0a0c28708050a984891a24388100154dc7811a3428d1b2d12f48831a4c8"
    "8124219a9c5830e5c395025aba4c08b363ca860d6706c099f326cf9e187ffe7c28b428ce800021f9040d0d0000002c02"
    "00000024000e008100000006e22e56523e00000008560001081c48b0a0c1820112263cc8b02142851003389cf8302244"
    "8102326ad4481180c58f00368aecf8d16248911c2796fc8832a5c395165b66a40833a24c01346b2abc495267c7973e7f"
    "36ac295465c9a23d15160c080021f90405100000002c020000002900100081000000deaaa62b299f0000000860000108"
    "1c48b0a0c18308132a5c68508043870c23267c4811a2c48b002a6a1418a0a3478f18056a1c09e0a3c9901947562c6912"
    "244695235bbabc0853a3cc8e216b56bc1920a7ce873c7dfe148072e2cfa24675224daa72a942984e19ae2418100021f9"
    "0409040000002c020001002c00120081000000e19bbd50701000000008660001081c48b0a0c18308132a5cc8b0a1c387"
    "10233614409162c1001833669458b0a2478b02358ae438f0a3c99022379234c912404a951c599e7c8971a54c933403d8"
    "bce931e74e9e20491e04ea512842a2158d1e25aa7429d0a64e65424d7873eac28f050302003b";

}

// --- Rozpoznanie ---------------------------------------------------------------------------

TRIAL("guessed: GIF poznaje po obu wiekach podpisu") {
    same(static_cast<int>(guessed(lump(plain))), static_cast<int>(Picture::gif));
    same(static_cast<int>(guessed(lump(clear))), static_cast<int>(Picture::gif));
}

TRIAL("guessed: PNG, JPEG i BMP rozpoznane, choć jeszcze nieczytane") {
    same(static_cast<int>(guessed(lump("89504e470d0a1a0a0000000d49484452"))),
         static_cast<int>(Picture::png));
    same(static_cast<int>(guessed(lump("ffd8ffe000104a464946"))),
         static_cast<int>(Picture::jpeg));
    same(static_cast<int>(guessed(lump("424d3a0000000000000036000000280000000100000001000000"))),
         static_cast<int>(Picture::bmp));
}

// Rozpoznanie bez dekodera **nie jest połowicznością**: dzięki niemu widżet powie „JPEG — tu
// nieobsługiwany" zamiast pokazać puste pole, a to jest różnica między brakiem funkcji
// a wrażeniem zepsucia.
TRIAL("decoded: rozpoznany, ale nieobsługiwany format oddaje pustkę bez wyjątku") {
    const Pixels one = decoded(lump("ffd8ffe000104a464946000102000000"));

    ought(!one.held());
}

TRIAL("guessed: samo BM bez reszty nagłówka to nie obrazek") {
    same(static_cast<int>(guessed(lump("424d"))), static_cast<int>(Picture::none));
    same(static_cast<int>(guessed(lump("424d3a000000"))), static_cast<int>(Picture::none));
}

TRIAL("guessed: śmieci i pustka to nie obrazek") {
    same(static_cast<int>(guessed(Bytes{})), static_cast<int>(Picture::none));
    same(static_cast<int>(guessed(lump("47494638"))), static_cast<int>(Picture::none));
    same(static_cast<int>(guessed(lump("474946383861"))), static_cast<int>(Picture::none));
}

// --- Obrazek nieruchomy --------------------------------------------------------------------

TRIAL("GIF: jedna kratka z jednej barwy") {
    const Pixels one = decoded(lump(dot));

    ought(one.held());
    same(one.size().wide, 1);
    same(one.size().high, 1);
    same(one.stills().size(), std::size_t{1});
    ought(at(one, 0, 0) == (Tone{9, 200, 30, 255}));
}

// Obrazek jest wzorem: barwa kratki to `(x + y) % 4` z palety czerń, czerwień, zieleń, błękit.
// Sprawdzamy go **wzorem, a nie wypisanym oczekiwaniem** — pomyłka w przepisywaniu trzydziestu
// pięciu kratek byłaby wtedy nie do odróżnienia od pomyłki w dekoderze.
TRIAL("GIF: paleta i kolejność kratek") {
    const Pixels one = decoded(lump(plain));

    constexpr Tone palette[4] = {
        {0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};

    same(one.size().wide, 7);
    same(one.size().high, 5);

    bool held = true;

    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x)
            held = held && at(one, x, y) == palette[(x + y) % 4];

    ought(held);
}

// **Ten sam obrazek zapisany na dwa sposoby musi wyjść ten sam.** Przeplot układa wiersze w cztery
// przebiegi — 0 co 8, 4 co 8, 2 co 4, 1 co 2 — i to jest miejsce, w którym najłatwiej się potknąć,
// bo dane idą po kolei, a wiersze nie.
TRIAL("GIF: przeplot daje ten sam obraz co zapis prosty") {
    const Pixels bare = decoded(lump(flat));
    const Pixels laced = decoded(lump(woven));

    ought(bare.held() && laced.held());
    same(bare.size().wide, laced.size().wide);
    same(bare.size().high, laced.size().high);

    bool held = true;

    for (int y = 0; y < bare.size().high; ++y)
        for (int x = 0; x < bare.size().wide; ++x) held = held && at(bare, x, y) == at(laced, x, y);

    ought(held);
}

// Przezroczystość wychodzi jako alfa zero i **spotyka się z resztą biblioteki**: mozaika zrobi
// z takiej kratki `bare()`, czyli widać przez nią tło rodzica.
TRIAL("GIF: przezroczysty indeks daje alfę zero") {
    const Pixels one = decoded(lump(clear));

    ought(one.held());

    bool held = true;

    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 7; ++x) {
            const bool see = (x + y) % 4 == 0;

            held = held && at(one, x, y).alpha == (see ? 0 : 255);
        }

    ought(held);
}

// --- Animacja -------------------------------------------------------------------------------

TRIAL("GIF: animacja oddaje wszystkie klatki") {
    const Pixels one = decoded(lump(leaving));

    same(one.stills().size(), std::size_t{4});
    same(one.size().wide, 16);
    same(one.size().high, 12);
}

// **Naginamy się do świata, nie do litery.** Norma czyta zero jako „tak szybko, jak się da", co
// znaczy sto klatek na sekundę i stroboskop; przeglądarki od dwudziestu lat zamieniają zero
// i jedną setną na dziesięć setnych i pod to jest robiona połowa GIF-ów w sieci. W tym pliku
// stoją kolejno 4, 0, 1 i 12 setnych sekundy — więc pierwsza i ostatnia przechodzą nietknięte,
// a dwie w środku podnoszą się do stu milisekund.
TRIAL("GIF: opóźnienie zerowe i najkrótsze idą na sto milisekund") {
    const Pixels one = decoded(lump(leaving));

    same(after(one, 0), 40LL);
    same(after(one, 1), 100LL);
    same(after(one, 2), 100LL);
    same(after(one, 3), 120LL);
}

// Sprzątanie „zostaw" nie rusza płótna, więc kratka pomalowana i niepomalowana ponownie zostaje
// taka, jaka była — tu tłem jest czerń nieprzezroczysta.
TRIAL("GIF: sprzątanie zostawiające nie czyści płótna") {
    const Pixels one = decoded(lump(leaving));

    ought(at(one, 0, 0, 0) == (Tone{255, 0, 0, 255}));
    ought(at(one, 15, 11, 3) == (Tone{0, 0, 0, 255}));
}

// Sprzątanie „do tła" czyści prostokąt **poprzedniej** klatki, a nie bieżącej — i czyści go do
// przezroczystości, nie do barwy tła z nagłówka. Tak robią przeglądarki i tak pasuje to do modelu
// biblioteki, w którym niepomalowane znaczy „widać rodzica".
TRIAL("GIF: sprzątanie do tła zostawia przezroczystość") {
    const Pixels one = decoded(lump(clearing));

    ought(at(one, 1, 1, 0) == (Tone{255, 0, 0, 255}));
    same(static_cast<int>(at(one, 1, 1, 3).alpha), 0);
    ought(at(one, 10, 7, 3) == (Tone{255, 0, 0, 255}));
}

// Sprzątanie „przywróć poprzednie" wraca do stanu sprzed klatki. Pierwsza klatka poprzedniego nie
// ma, więc wraca do płótna pustego — sprawdzone z przeglądarką, nie zgadnięte.
TRIAL("GIF: przywrócenie poprzedniego wraca do pustego płótna") {
    const Pixels one = decoded(lump(backing));

    ought(at(one, 15, 11, 0) == (Tone{0, 0, 0, 255}));
    same(static_cast<int>(at(one, 15, 11, 1).alpha), 0);
    ought(at(one, 4, 3, 1) == (Tone{0, 255, 0, 255}));
}

// --- Budżet i przerwanie ----------------------------------------------------------------------

TRIAL("decoded: prośba o jedną klatkę zatrzymuje rozwijanie animacji") {
    same(decoded(lump(leaving), 1).stills().size(), std::size_t{1});
    same(decoded(lump(leaving), 3).stills().size(), std::size_t{3});
    same(decoded(lump(leaving), 9).stills().size(), std::size_t{4});
}

// **Postać strumieniowa podaje klatkę i zapomina ją.** Fałsz od wołającego znaczy „dosyć"
// i nie jest błędem — dzięki temu podgląd, który chce tylko pierwszej, nie płaci za resztę.
TRIAL("decoded: strumień daje się przerwać w dowolnej chwili") {
    std::size_t seen = 0;

    decoded(lump(leaving), [&](const Bytes& canvas, ui::Size size, std::chrono::milliseconds) {
        ++seen;

        same(size.wide, 16);
        same(canvas.size(), std::size_t{16 * 12 * 4});

        return seen < 2;
    });

    same(seen, std::size_t{2});
}

TRIAL("decoded: strumień bez czynności nic nie robi") {
    decoded(lump(leaving), Taking{});

    ought(true);
}

// --- Odmowy ------------------------------------------------------------------------------------

// Płótno ponad sufit odrzucamy **przed przydzieleniem czegokolwiek**. GIF dopuszcza 65535 na 65535,
// czyli siedemnaście terabajtów RGBA, i to musi być odmowa, a nie próba.
TRIAL("GIF: płótno ponad sufit jest odmawiane") {
    const Pixels one = decoded(lump("474946383961ffffffff0000003b"));

    ought(!one.held());
}

TRIAL("GIF: sam nagłówek bez obrazka nie daje klatki") {
    ought(!decoded(lump("474946383961040004000000003b")).held());
}

TRIAL("GIF: nieznany blok kończy czytanie zamiast zgadywać dalej") {
    ought(!decoded(lump("47494638396104000400000000ab")).held());
}

// **Plik urwany to nie plik pusty.** Menedżer plików ogląda też pliki w trakcie kopiowania
// i uszkodzone — a tam górna połowa obrazka jest odpowiedzią lepszą niż nic. Ten sam plik ucięty
// o sześć bajtów oddaje cztery wiersze z pięciu, a piąty zostaje przezroczysty.
TRIAL("GIF: plik urwany oddaje tyle obrazka, ile przyszło") {
    const Bytes whole = lump(plain);
    const Bytes cut{whole.begin(), whole.end() - 6};

    const Pixels one = decoded(cut);

    ought(one.held());
    same(one.size().high, 5);
    ought(at(one, 0, 3) == (Tone{0, 0, 255, 255}));
    same(static_cast<int>(at(one, 0, 4).alpha), 0);
}

TRIAL("GIF: ucięcie przed danymi obrazka nie daje klatki") {
    const Bytes whole = lump(plain);
    const Bytes cut{whole.begin(), whole.begin() + 20};

    ought(!decoded(cut).held());
}

// --- Cały prawdziwy plik ------------------------------------------------------------------------

TRIAL("GIF: prawdziwy plik z cudzego kodera czyta się w całości") {
    const Pixels one = decoded(lump(proof));

    ought(one.held());
    same(one.size().wide, 48);
    same(one.size().high, 32);
    same(one.stills().size(), std::size_t{16});
}

// Klatki trzecia i piąta mają w pliku zapisane zero i jedną setną sekundy — po zacisku obie
// wychodzą na sto milisekund. Reszta to 40, 70, 100, 130 i 160, czyli to, co koder zapisał.
TRIAL("GIF: opóźnienia prawdziwego pliku razem z zaciskiem") {
    const Pixels one = decoded(lump(proof));

    const long long these[16] = {40, 70, 100, 100, 160, 100, 70, 100,
                                 130, 160, 40, 70, 100, 130, 160, 40};

    bool held = true;

    for (std::size_t at = 0; at < 16; ++at) held = held && after(one, at) == these[at];

    ought(held);
}

// **Odcisk wszystkich klatek co do bajtu.** Sześćdziesiąt jeden tysięcy kratek po cztery bajty,
// ściśnięte w jedną liczbę — jeśli cokolwiek w dekoderze, w składaniu klatek albo w sprzątaniu po
// nich się ruszy, ta liczba się zmieni. Nie powie, co się zepsuło; powie, że coś.
//
// Wartość wzięta z przebiegu, który **osobno sprawdzono co do piksela z niezależnym dekoderem** —
// bez tego byłaby to tylko pieczęć na własnym wyniku.
TRIAL("GIF: odcisk wszystkich klatek prawdziwego pliku") {
    const Pixels one = decoded(lump(proof));

    std::uint64_t hash = 1469598103934665603ull;

    for (const Still& said : one.stills())
        for (const std::byte what : said.bytes) {
            hash ^= std::to_integer<std::uint8_t>(what);
            hash *= 1099511628211ull;
        }

    same(hash, 0xDD3730739D049EB1ull);
}

// Kilka kratek wypisanych wprost — po to, żeby upadek odcisku dało się od czegoś zacząć.
TRIAL("GIF: wybrane kratki prawdziwego pliku") {
    const Pixels one = decoded(lump(proof));

    // Róg płótna: pierwszy obrazek nie sięga aż tam, więc kratka zostaje przezroczysta.
    same(static_cast<int>(at(one, 0, 0).alpha), 0);

    // Kwadrat w lewym górnym rogu, barwa numer jeden z palety.
    ought(at(one, 4, 4) == (Tone{37, 71, 113, 255}));
}

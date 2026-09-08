#include "proving.hpp"

#include <random>
#include <string>

#include "talcite/bytes.hpp"

#include "quilt.hpp"

using namespace talcite;

namespace {

Bytes lump(const std::string& what) {
    Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

std::string said(const Bytes& what) {
    std::string made;

    made.reserve(what.size());

    for (const std::byte one : what) made.push_back(static_cast<char>(one));

    return made;
}

std::string whole(Quilt& one) { return said(one.bytes(0, one.whole())); }

// Źródło oddające **po trzy bajty na wołanie** — bo tyle mu wolno. Na nim widać, czy dobieranie
// w pętli działa; pierwsza wersja szpuli na tym poległa.
Feed stingy(const Bytes& raw) {
    return [raw](std::size_t at, std::size_t most) {
        Bytes made;

        const std::size_t give = most < 3 ? most : 3;

        for (std::size_t one = at; one < raw.size() && one < at + give; ++one)
            made.push_back(raw[one]);

        return made;
    };
}

}

TRIAL("Quilt: pusty nic nie niesie") {
    Quilt one{lump("")};

    same(one.whole(), std::size_t{0});
    same(whole(one), std::string{});
}

TRIAL("Quilt: nietknięty oddaje oryginał") {
    Quilt one{lump("abcdef")};

    same(one.whole(), std::size_t{6});
    same(whole(one), std::string{"abcdef"});
    same(said(one.bytes(2, 3)), std::string{"cde"});
    same(said(one.bytes(4, 100)), std::string{"ef"});
}

TRIAL("Quilt: wstawianie na początku, w środku i na końcu") {
    Quilt one{lump("abc")};

    one.insert(0, "X");
    same(whole(one), std::string{"Xabc"});

    one.insert(2, "Y");
    same(whole(one), std::string{"XaYbc"});

    one.insert(one.whole(), "Z");
    same(whole(one), std::string{"XaYbcZ"});

}

TRIAL("Quilt: kasowanie na początku, w środku i na końcu") {
    Quilt one{lump("abcdef")};

    one.erase(0, 1);
    same(whole(one), std::string{"bcdef"});

    one.erase(2, 2);
    same(whole(one), std::string{"bcf"});

    one.erase(2, 100);
    same(whole(one), std::string{"bc"});
}

TRIAL("Quilt: wstawienie i skasowanie wraca do oryginału") {
    Quilt one{lump("abcdef")};

    one.insert(3, "XYZ");
    same(whole(one), std::string{"abcXYZdef"});

    one.erase(3, 3);
    same(whole(one), std::string{"abcdef"});
}

// Oryginał zostaje za źródłem, więc wszystko musi działać także wtedy, gdy źródło oddaje po
// kawałku i nigdy nie daje całości naraz.
TRIAL("Quilt: źródło strumieniowe daje to samo, co pamięć") {
    const std::string full = "pierwszy wiersz\ndrugi wiersz\ntrzeci wiersz\n";

    Quilt one{stingy(lump(full)), full.size()};

    same(whole(one), full);

    one.insert(16, "nowy ");

    same(whole(one), std::string{"pierwszy wiersz\nnowy drugi wiersz\ntrzeci wiersz\n"});

    one.erase(0, 9);

    same(whole(one), std::string{"wiersz\nnowy drugi wiersz\ntrzeci wiersz\n"});
}

// Zszycie zbiera dokument do pamięci i odcina źródło. Po nim odpowiedzi mają być co do bajtu te
// same, a zmiany dalej mają działać — tylko oryginał leży już gdzie indziej.
TRIAL("Quilt: zszycie oddaje to samo, co źródło") {
    const std::string full = "pierwszy wiersz\ndrugi wiersz\ntrzeci wiersz\n";

    Quilt one{stingy(lump(full)), full.size()};

    one.insert(16, "nowy ");

    const std::string want = "pierwszy wiersz\nnowy drugi wiersz\ntrzeci wiersz\n";

    same(whole(one), want);

    ought(one.settle());

    same(one.whole(), want.size());
    same(whole(one), want);
    same(said(one.bytes(16, 5)), std::string{"nowy "});

    one.insert(0, "Z");

    same(whole(one), "Z" + want);

    // Drugie zszycie nie ma czego zszywać, ale nie ma prawa niczego zepsuć.
    ought(one.settle());

    same(whole(one), "Z" + want);
}

TRIAL("Quilt: zszycie dokumentu w pamięci nic nie zmienia") {
    Quilt one{lump("abcdef")};

    ought(one.settle());

    same(whole(one), std::string{"abcdef"});
}

// Źródło, które obiecało więcej, niż oddaje: zszycie ma się nie udać i **niczego nie ruszyć**,
// bo skrócenie dokumentu za plecami szpuli byłoby gorsze niż niezrobienie niczego.
TRIAL("Quilt: zszycie ze skąpego źródła nie dochodzi do skutku") {
    const Bytes raw = lump("abcdef");

    Quilt one{[raw](std::size_t at, std::size_t most) {
                  Bytes made;

                  for (std::size_t which = at; which < 4 && which < at + most; ++which)
                      made.push_back(raw[which]);

                  return made;
              },
              raw.size()};

    ought(!one.settle());

    same(one.whole(), std::size_t{6});
    same(said(one.bytes(0, 4)), std::string{"abcd"});
}

// Pisanie w ciągu przedłuża ostatni odcinek zamiast robić nowy. Gdyby tego nie robiło, pięć tysięcy
// znaków dałoby pięć tysięcy odcinków — i to jest ta próba, która by to złapała czasem.
TRIAL("Quilt: pięć tysięcy znaków po jednym") {
    Quilt one{lump("")};

    std::string want;

    for (int which = 0; which < 5000; ++which) {
        one.insert(one.whole(), "x");
        want.push_back('x');
    }

    same(one.whole(), std::size_t{5000});
    same(whole(one), want);
}

// Porównanie z `std::string` na losowym ciągu zmian — to jedyny sposób, żeby złapać błąd
// w rozcinaniu odcinków, którego nie widać na żadnym pojedynczym przypadku.
TRIAL("Quilt: losowe zmiany zgadzają się ze wzorcem") {
    std::mt19937 gen(7);

    for (int round = 0; round < 200; ++round) {
        std::string want;

        const int much = static_cast<int>(gen() % 60);

        for (int one = 0; one < much; ++one)
            want.push_back(static_cast<char>('a' + gen() % 26));

        const Bytes raw = lump(want);

        Quilt one = round % 2 ? Quilt{stingy(raw), raw.size()} : Quilt{lump(want)};

        for (int step = 0; step < 30; ++step) {
            if (gen() % 2 || want.empty()) {
                const std::size_t at = want.empty() ? 0 : gen() % (want.size() + 1);

                std::string what;

                const int size = 1 + static_cast<int>(gen() % 5);

                for (int which = 0; which < size; ++which)
                    what.push_back(static_cast<char>('A' + gen() % 26));

                want.insert(at, what);
                one.insert(at, what);
            } else {
                const std::size_t at = gen() % want.size();
                const std::size_t size = 1 + gen() % (want.size() - at);

                want.erase(at, size);
                one.erase(at, size);
            }

            same(one.whole(), want.size());
            same(whole(one), want);
        }
    }
}

#include "proving.hpp"

#include <cstddef>
#include <string>

#include "talcite/bytes.hpp"
#include "talcite/encoding.hpp"

using namespace talcite;

TRIAL("Encoding: bajt wiodący mówi, ile ciągu dalszego") {
    same(utf8::trailing(0x41), std::size_t{0});
    same(utf8::trailing(0xC5), std::size_t{1});
    same(utf8::trailing(0xE2), std::size_t{2});
    same(utf8::trailing(0xF0), std::size_t{3});
    same(utf8::trailing(0x80), std::size_t{0});
}

TRIAL("Encoding: znak po znaku przez cały napis") {
    const std::string said = "aż💡";

    std::size_t at = 0;

    same(utf8::decode(said, at), U'a');
    same(utf8::decode(said, at), U'ż');
    same(utf8::decode(said, at), U'💡');
    same(at, said.size());
}

TRIAL("Encoding: bajt nie do przyjęcia daje znak zastępczy i jeden krok") {
    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    std::size_t at = 1;

    same(utf8::decode(said, at), U'�');
    same(at, std::size_t{2});
}

TRIAL("Encoding: ogon urwany w połowie też daje znak zastępczy") {
    std::string said;
    said += static_cast<char>(0xE2);
    said += static_cast<char>(0x9D);

    std::size_t at = 0;

    same(utf8::decode(said, at), U'�');
    same(at, std::size_t{1});
}

TRIAL("Encoding: ciąg dalszy bez ciągu dalszego to też śmieć") {
    std::string said;
    said += static_cast<char>(0xC5);
    said += static_cast<char>(0x41);

    std::size_t at = 0;

    same(utf8::decode(said, at), U'�');
    same(at, std::size_t{1});
}

TRIAL("Encoding: koniec napisu oddaje zero i nie rusza wskaźnika") {
    const std::string said = "a";

    std::size_t at = 1;

    same(utf8::decode(said, at), static_cast<char32_t>(0));
    same(at, std::size_t{1});
}

TRIAL("Encoding: zapisanie i odczytanie wraca tym samym") {
    for (const char32_t one : {U'a', U'ż', U'漢', U'💡'}) {
        const std::string said = utf8::encode(one);

        std::size_t at = 0;

        same(utf8::decode(said, at), one);
        same(at, said.size());
    }
}

TRIAL("Encoding: zapisanie dopisuje, a nie zastępuje") {
    std::string said = "a";

    utf8::encode(U'ż', said);

    same(said, std::string{"aż"});
}

TRIAL("Encoding: długość zapisu zgadza się z regułą") {
    same(utf8::encode(U'a').size(), std::size_t{1});
    same(utf8::encode(U'ż').size(), std::size_t{2});
    same(utf8::encode(U'漢').size(), std::size_t{3});
    same(utf8::encode(U'💡').size(), std::size_t{4});
}

TRIAL("Encoding: szerokie są tylko te, które naprawdę zajmują dwie kratki") {
    same(width(U'a'), 1);
    same(width(U'ż'), 1);
    same(width(U'漢'), 2);
    same(width(U'가'), 2);
    same(width(U'😀'), 2);
    same(width(U'→'), 1);
}

// UTF-8 a natywny napis szeroki. Ta para istnieje po to, żeby z Windows dało się rozmawiać jednym
// kodowaniem — on mówi UTF-16, reszta biblioteki UTF-8.
TRIAL("Kodowanie: napis szeroki i UTF-8 przechodzą tam i z powrotem") {
    const std::string said = "Zażółć gęślą jaźń";

    same(talcite::to_string(talcite::to_wstring(said)), said);

    // Sam ASCII też, bo to najczęstszy przypadek i najłatwiej go zepsuć skrótem.
    same(talcite::to_string(talcite::to_wstring(std::string{"plain"})), std::string{"plain"});

    same(talcite::to_string(std::wstring_view{}), std::string{});
    ought(talcite::to_wstring(std::string_view{}).empty());
}

// Znak spoza podstawowej płaszczyzny to jedyne miejsce, gdzie te dwie drogi się rozchodzą: na
// Windows `wchar_t` ma dwa bajty i taki znak wymaga **pary zastępczej**, na POSIX-ie cztery i pary
// nie ma wcale. Próba pyta o wynik, więc mówi prawdę na obu — ale samego składania pary nie widzi
// tu nikt, bo zestaw chodzi na POSIX-ie. To jest znana dziura, nie przeoczenie.
TRIAL("Kodowanie: znak spoza podstawowej płaszczyzny przechodzi tam i z powrotem") {
    const std::string said = "a\U0001F600b";

    const std::wstring wide = talcite::to_wstring(said);

    same(talcite::to_string(wide), said);

    // Na Windows to trzy jednostki (a, para, b), gdzie indziej trzy znaki.
    same(wide.size(), sizeof(wchar_t) == 2 ? std::size_t{4} : std::size_t{3});
}

// Bajt, który nie jest poprawnym UTF-8, kończy jako znak zastępczy — tak samo jak w `utf8::decode`.
// **To jest dokładnie ten objaw**, który widać było w panelach twa, gdy nazwa pliku przychodziła
// w stronie kodowej ANSI zamiast w UTF-8: same znaki zastępcze zamiast polskich liter.
TRIAL("Kodowanie: bajt spoza UTF-8 staje się znakiem zastępczym, a nie cichym śmieciem") {
    const std::string broken = "Za\xbf\xf3\xb3\xe6.txt";

    const std::string made = talcite::to_string(talcite::to_wstring(broken));

    ought(made != broken);
    ought(made.find("\xef\xbf\xbd") != std::string::npos);
}

// Zero w środku napisu. `std::string_view` zna swoją długość, więc zero jest w nim **znakiem**,
// a nie końcem — i tak samo musi wyjść po drugiej stronie. Próba pilnuje tego, żeby przepisywanie
// nie kończyło się na pierwszym zerze i nie gubiło reszty po cichu.
TRIAL("Kodowanie: zero w środku napisu jest znakiem, a nie końcem") {
    const std::string said{"a\0b", 3};

    const std::wstring wide = talcite::to_wstring(said);

    same(wide.size(), std::size_t{3});
    same(wide[1], wchar_t{0});

    same(talcite::to_string(wide), said);
}

// Napis ⇄ bajty bajt w bajt: bez kodowania, z bajtem zerowym i znakami spoza ASCII, w obie strony.
TRIAL("Bytes: bytes i text przepisują bajt w bajt, w obie strony") {
    const std::string said{"za\0\xc5\xbc\xc3\xb3\xc5\x82\xff", 10};
    const Bytes made = bytes(said);

    same(made.size(), std::size_t{10});
    ought(made[2] == std::byte{0});
    ought(made[9] == std::byte{0xff});
    same(text(made), said);
    ought(bytes("").empty());
    same(text(Bytes{}), std::string{});
}

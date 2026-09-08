#include "proving.hpp"

#include <string>

#include "talcite/encoding.hpp"

using namespace talcite;

TRIAL("Encoding: bajt wiodący mówi, ile ciągu dalszego") {
    same(following(0x41), std::size_t{0});
    same(following(0xC5), std::size_t{1});
    same(following(0xE2), std::size_t{2});
    same(following(0xF0), std::size_t{3});
    same(following(0x80), std::size_t{0});
}

TRIAL("Encoding: znak po znaku przez cały napis") {
    const std::string said = "aż💡";

    std::size_t at = 0;

    same(letter(said, at), U'a');
    same(letter(said, at), U'ż');
    same(letter(said, at), U'💡');
    same(at, said.size());
}

TRIAL("Encoding: bajt nie do przyjęcia daje znak zastępczy i jeden krok") {
    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    std::size_t at = 1;

    same(letter(said, at), U'�');
    same(at, std::size_t{2});
}

TRIAL("Encoding: ogon urwany w połowie też daje znak zastępczy") {
    std::string said;
    said += static_cast<char>(0xE2);
    said += static_cast<char>(0x9D);

    std::size_t at = 0;

    same(letter(said, at), U'�');
    same(at, std::size_t{1});
}

TRIAL("Encoding: ciąg dalszy bez ciągu dalszego to też śmieć") {
    std::string said;
    said += static_cast<char>(0xC5);
    said += static_cast<char>(0x41);

    std::size_t at = 0;

    same(letter(said, at), U'�');
    same(at, std::size_t{1});
}

TRIAL("Encoding: koniec napisu oddaje zero i nie rusza wskaźnika") {
    const std::string said = "a";

    std::size_t at = 1;

    same(letter(said, at), static_cast<char32_t>(0));
    same(at, std::size_t{1});
}

TRIAL("Encoding: zapisanie i odczytanie wraca tym samym") {
    for (const char32_t one : {U'a', U'ż', U'漢', U'💡'}) {
        const std::string said = spelled(one);

        std::size_t at = 0;

        same(letter(said, at), one);
        same(at, said.size());
    }
}

TRIAL("Encoding: zapisanie dopisuje, a nie zastępuje") {
    std::string said = "a";

    spelled(U'ż', said);

    same(said, std::string{"aż"});
}

TRIAL("Encoding: długość zapisu zgadza się z regułą") {
    same(spelled(U'a').size(), std::size_t{1});
    same(spelled(U'ż').size(), std::size_t{2});
    same(spelled(U'漢').size(), std::size_t{3});
    same(spelled(U'💡').size(), std::size_t{4});
}

TRIAL("Encoding: szerokie są tylko te, które naprawdę zajmują dwie kratki") {
    same(widely(U'a'), 1);
    same(widely(U'ż'), 1);
    same(widely(U'漢'), 2);
    same(widely(U'가'), 2);
    same(widely(U'😀'), 2);
    same(widely(U'→'), 1);
}

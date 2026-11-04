#include "hexing.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "proving.hpp"

using namespace talcite;

// Hexadecimal digits of either case read the same, an escape is written in capitals, and undoing
// escapes keeps whatever is not a whole escape as it stands.

TRIAL("hex: digit reads both cases and refuses everything else") {
    same(hex::digit('0'), 0);
    same(hex::digit('9'), 9);
    same(hex::digit('a'), 10);
    same(hex::digit('f'), 15);
    same(hex::digit('A'), 10);
    same(hex::digit('F'), 15);
    same(hex::digit('g'), -1);
    same(hex::digit('G'), -1);
    same(hex::digit('/'), -1);
    same(hex::digit(':'), -1);
    same(hex::digit('@'), -1);
    same(hex::digit('`'), -1);
}

TRIAL("hex: escaped writes a byte as %XX in capitals") {
    std::string made = "a";

    hex::escaped(0x20, made);
    hex::escaped(0xC5, made);
    hex::escaped(0x0F, made);

    same(made, std::string{"a%20%C5%0F"});
    same(hex::upper.size(), std::size_t{16});
    same(hex::lower, std::string_view{"0123456789abcdef"});
}

TRIAL("hex: unescaped undoes whole escapes, one at the very end too") {
    same(hex::unescaped("ala%20ma%2fkota"), std::string{"ala ma/kota"});
    same(hex::unescaped("%41"), std::string{"A"});
    same(hex::unescaped("%c5%81%C3%B3d%C5%BA"), std::string{"\xC5\x81\xC3\xB3" "d\xC5\xBA"});
}

TRIAL("hex: unescaped keeps a percent sign that starts no whole escape") {
    same(hex::unescaped("100%"), std::string{"100%"});
    same(hex::unescaped("%4"), std::string{"%4"});
    same(hex::unescaped("%zz%41"), std::string{"%zzA"});
    same(hex::unescaped("%%41"), std::string{"%A"});
}

#include "hexing.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "tal/testing.hpp"

using namespace talcite;

// Hexadecimal digits of either case read the same, an escape is written in capitals, and undoing
// escapes keeps whatever is not a whole escape as it stands.

TRIAL("hex: digit reads both cases and refuses everything else") {
    SAME(hex::digit('0'), 0);
    SAME(hex::digit('9'), 9);
    SAME(hex::digit('a'), 10);
    SAME(hex::digit('f'), 15);
    SAME(hex::digit('A'), 10);
    SAME(hex::digit('F'), 15);
    SAME(hex::digit('g'), -1);
    SAME(hex::digit('G'), -1);
    SAME(hex::digit('/'), -1);
    SAME(hex::digit(':'), -1);
    SAME(hex::digit('@'), -1);
    SAME(hex::digit('`'), -1);
}

TRIAL("hex: escaped writes a byte as %XX in capitals") {
    std::string made = "a";

    hex::escaped(0x20, made);
    hex::escaped(0xC5, made);
    hex::escaped(0x0F, made);

    SAME(made, std::string{"a%20%C5%0F"});
    SAME(hex::upper.size(), std::size_t{16});
    SAME(hex::lower, std::string_view{"0123456789abcdef"});
}

TRIAL("hex: unescaped undoes whole escapes, one at the very end too") {
    SAME(hex::unescaped("ala%20ma%2fkota"), std::string{"ala ma/kota"});
    SAME(hex::unescaped("%41"), std::string{"A"});
    SAME(hex::unescaped("%c5%81%C3%B3d%C5%BA"), std::string{"\xC5\x81\xC3\xB3" "d\xC5\xBA"});
}

TRIAL("hex: unescaped keeps a percent sign that starts no whole escape") {
    SAME(hex::unescaped("100%"), std::string{"100%"});
    SAME(hex::unescaped("%4"), std::string{"%4"});
    SAME(hex::unescaped("%zz%41"), std::string{"%zzA"});
    SAME(hex::unescaped("%%41"), std::string{"%A"});
}

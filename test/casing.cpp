#include "talcite/ascii.hpp"

#include <string>
#include <string_view>

#include "proving.hpp"

using namespace talcite;

// The one rule of ASCII case and white space for talcite and twa: letters lower whatever the
// locale says, other bytes, UTF-8 among them, pass untouched; a text is trimmed of spaces, tabs and
// line ends at its ends, and nothing in the middle changes.

TRIAL("ASCII: lowered lowers the letters A to Z and nothing else") {
    same(ascii::lowered('A'), 'a');
    same(ascii::lowered('Z'), 'z');
    same(ascii::lowered('a'), 'a');
    same(ascii::lowered('@'), '@');
    same(ascii::lowered('['), '[');
    same(ascii::lowered(static_cast<char>(0xC5)), static_cast<char>(0xC5));

    same(ascii::lowered(std::string_view{"Heat.MKV \xC5\x81\xC3\xB3d\xC5\xBA"}), std::string{"heat.mkv \xC5\x81\xC3\xB3d\xC5\xBA"});
}

TRIAL("ASCII: trimmed takes spaces, tabs and line ends off the ends only") {
    same(ascii::trimmed(" \t ala ma\tkota \r\n"), std::string_view{"ala ma\tkota"});
    same(ascii::trimmed("bez"), std::string_view{"bez"});
    same(ascii::trimmed(" \r\n\t "), std::string_view{});
    same(ascii::trimmed(""), std::string_view{});
    same(ascii::trimmed("\vznak\f"), std::string_view{"\vznak\f"});
}

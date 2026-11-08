#include "talcite/ascii.hpp"

#include <string>
#include <string_view>

#include "tal/testing.hpp"

using namespace talcite;

// The one rule of ASCII case and white space for talcite and twa: letters lower whatever the
// locale says, other bytes, UTF-8 among them, pass untouched; a text is trimmed of spaces, tabs and
// line ends at its ends, and nothing in the middle changes.

TRIAL("ASCII: lowered lowers the letters A to Z and nothing else") {
    SAME(ascii::lowered('A'), 'a');
    SAME(ascii::lowered('Z'), 'z');
    SAME(ascii::lowered('a'), 'a');
    SAME(ascii::lowered('@'), '@');
    SAME(ascii::lowered('['), '[');
    SAME(ascii::lowered(static_cast<char>(0xC5)), static_cast<char>(0xC5));

    SAME(ascii::lowered(std::string_view{"Heat.MKV \xC5\x81\xC3\xB3" "d\xC5\xBA"}), std::string{"heat.mkv \xC5\x81\xC3\xB3" "d\xC5\xBA"});
}

TRIAL("ASCII: trimmed takes spaces, tabs and line ends off the ends only") {
    SAME(ascii::trimmed(" \t ala ma\tkota \r\n"), std::string_view{"ala ma\tkota"});
    SAME(ascii::trimmed("bez"), std::string_view{"bez"});
    SAME(ascii::trimmed(" \r\n\t "), std::string_view{});
    SAME(ascii::trimmed(""), std::string_view{});
    SAME(ascii::trimmed("\vznak\f"), std::string_view{"\vznak\f"});
}

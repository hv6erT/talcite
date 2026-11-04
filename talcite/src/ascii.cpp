#include "talcite/ascii.hpp"

namespace talcite::ascii {

namespace {

// Whether the byte is white space at the end of a text: a space, a tab or a line end. The one set
// of talcite and twa, for headers, settings, typed commands and names alike.
bool blank(char one) { return one == ' ' || one == '\t' || one == '\r' || one == '\n'; }

}

// The letter in small case, for the ASCII letters only, whatever the locale says: names, keys and
// protocol words compare this way, and the C library's tolower would follow the locale of the
// program, which a library linked in may change. Any other byte stays as it is, so UTF-8 passes
// untouched.
char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

// The text with its ASCII letters in small case (`lowered` of each byte).
std::string lowered(std::string_view text) {
    std::string made{text};

    for (char& one : made) one = lowered(one);

    return made;
}

// The text without spaces, tabs and line ends at its ends; everything in the middle stays.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);

    return text;
}

}

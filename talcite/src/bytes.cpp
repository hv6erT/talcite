#include "talcite/bytes.hpp"

// String to bytes and back, byte for byte. No encoding and no checking: a string in talcite and in
// twa is UTF-8, and the bytes are the same bytes. That is all it takes for text in memory to go to
// a decoder, a file or an editor, and for bytes to come back as a string.
namespace talcite {

// The bytes of the string, unchanged.
Bytes bytes(std::string_view text) {
    Bytes made(text.size());

    for (std::size_t at = 0; at < text.size(); ++at) made[at] = static_cast<std::byte>(text[at]);

    return made;
}

// The bytes as a string, unchanged.
std::string text(std::span<const std::byte> given) {
    std::string made(given.size(), '\0');

    for (std::size_t at = 0; at < given.size(); ++at) made[at] = static_cast<char>(given[at]);

    return made;
}

}

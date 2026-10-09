#include "talcite/bytes.hpp"

// **Napis ⇄ bajty, bajt w bajt.** Bez kodowania i bez sprawdzania: napis w talcite i w twa to
// UTF-8, a bajty to te same bajty — tyle, ile trzeba, żeby tekst z pamięci poszedł do dekodera,
// pliku albo edytora, i żeby bajty wróciły jako napis. Dawniej ta sama pętla stała w kilkudziesięciu
// miejscach, po obu stronach biblioteki i w próbach.
namespace talcite {

Bytes bytes(std::string_view text) {
    Bytes made(text.size());

    for (std::size_t at = 0; at < text.size(); ++at) made[at] = static_cast<std::byte>(text[at]);

    return made;
}

std::string text(std::span<const std::byte> given) {
    std::string made(given.size(), '\0');

    for (std::size_t at = 0; at < given.size(); ++at) made[at] = static_cast<char>(given[at]);

    return made;
}

}

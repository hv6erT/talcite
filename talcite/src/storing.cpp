#include "storing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace talcite::fs {

// **Ścieżka do przechowania: zawsze poprawny UTF-8 i zawsze z powrotem ta sama ścieżka.**
//
// Nazwa pliku nie musi być tekstem. Na POSIX-ie to dowolne bajty poza zerem i ukośnikiem — stare
// archiwa i płyty przegrane z Windows 98 niosą nazwy w Latin-1 („Am\xE9lie.mkv”). Na Windows to
// jednostki UTF-16, wśród których może stać samotna połowa pary zastępczej. JSON przyjmuje tylko
// poprawny UTF-8: nazwa spoza niego wywracała zapis historii i spisu biblioteki (nlohmann rzuca),
// a zamiana na znak zastępczy zgubiłaby plik — cofnięcie wskazałoby inną nazwę niż zapisana.
//
// Dlatego postać przechowywana jest dwojaka:
//
// - nazwa, która jest poprawnym tekstem, zostaje sobą — czytelna w pliku i taka jak dotąd;
// - każda inna dostaje na początek U+FFFF (znak, którego nie ma w żadnym tekście — Unicode
//   trzyma go poza użyciem) i dalej wszystko, co jest tekstem, jak jest, a resztę ucieczką:
//   bajt — `%XX`, samotna jednostka UTF-16 — `%uXXXX`, sam znak procentu — `%25`.
//
// Nazwa poprawna, ale zaczynająca się od U+FFFF, też idzie ucieczką — inaczej czytający wziąłby
// ją za postać zastępczą. Tak żadna nazwa nie ma dwóch zapisów i żaden zapis nie ma dwóch nazw.
//
// Zapis z jednego systemu czytany na drugim: bajt spoza ASCII na Windows nie ma znaczenia, więc
// staje się znakiem zastępczym; samotna jednostka na POSIX-ie staje się trzema bajtami WTF-8. Taka
// ścieżka i tak nie wskazuje istniejącego pliku — chodzi tylko o to, żeby odczyt się nie wywracał.
namespace {

constexpr std::string_view marker = "\xEF\xBF\xBF";

constexpr char32_t substitute = 0xFFFD;

// Długość poprawnego znaku UTF-8 od `at` i sam znak — albo nic, gdy bajt nie zaczyna poprawnego
// znaku: ciąg ucięty, przedłużony, z nadmiarem bajtów, połowa pary zastępczej, poza U+10FFFF.
struct Sign {
    char32_t value = 0;
    std::size_t much = 0;
};

std::optional<Sign> sound(std::string_view text, std::size_t at) {
    const auto byte = [&text](std::size_t which) { return static_cast<unsigned char>(text[which]); };

    const unsigned char lead = byte(at);

    if (lead < 0x80) return Sign{lead, 1};

    std::size_t much = 0;
    char32_t value = 0;
    char32_t least = 0;

    // Pierwszy bajt mówi tylko, ile bajtów ma znak; zapis przedłużony (C0, C1, E0 80…) i ponad
    // U+10FFFF (F5…) odrzuca dopiero sprawdzenie samej wartości niżej — jedno miejsce na jedną regułę.
    if ((lead & 0xE0) == 0xC0) {
        much = 2;
        value = lead & 0x1F;
        least = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        much = 3;
        value = lead & 0x0F;
        least = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        much = 4;
        value = lead & 0x07;
        least = 0x10000;
    } else {
        return std::nullopt;
    }

    if (at + much > text.size()) return std::nullopt;

    for (std::size_t which = 1; which < much; ++which) {
        const unsigned char next = byte(at + which);

        if ((next & 0xC0) != 0x80) return std::nullopt;

        value = (value << 6) | (next & 0x3F);
    }

    if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return std::nullopt;

    return Sign{value, much};
}

void put(char32_t one, std::string& into) {
    if (one < 0x80) {
        into += static_cast<char>(one);
    } else if (one < 0x800) {
        into += static_cast<char>(0xC0 | (one >> 6));
        into += static_cast<char>(0x80 | (one & 0x3F));
    } else if (one < 0x10000) {
        into += static_cast<char>(0xE0 | (one >> 12));
        into += static_cast<char>(0x80 | ((one >> 6) & 0x3F));
        into += static_cast<char>(0x80 | (one & 0x3F));
    } else {
        into += static_cast<char>(0xF0 | (one >> 18));
        into += static_cast<char>(0x80 | ((one >> 12) & 0x3F));
        into += static_cast<char>(0x80 | ((one >> 6) & 0x3F));
        into += static_cast<char>(0x80 | (one & 0x3F));
    }
}

void put(char32_t one, std::u16string& into) {
    if (one < 0x10000) {
        into += static_cast<char16_t>(one);

        return;
    }

    one -= 0x10000;

    into += static_cast<char16_t>(0xD800 + (one >> 10));
    into += static_cast<char16_t>(0xDC00 + (one & 0x3FF));
}

void escaped(std::uint32_t value, int digits, std::string& into) {
    static constexpr char hex[] = "0123456789ABCDEF";

    into += '%';

    if (digits == 4) into += 'u';

    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) into += hex[(value >> shift) & 0xF];
}

// Znak tekstu w postaci zastępczej: procent ucieczką, reszta jak jest.
void kept(char32_t one, std::string& into) {
    if (one == U'%') escaped('%', 2, into);
    else put(one, into);
}

std::string chosen(bool clean, std::string plain, std::string escapes) {
    if (clean && plain.compare(0, marker.size(), marker) != 0) return plain;

    return std::string{marker} + escapes;
}

std::optional<std::uint32_t> hexed(std::string_view text, std::size_t at, std::size_t digits) {
    if (at + digits > text.size()) return std::nullopt;

    std::uint32_t made = 0;

    for (std::size_t which = 0; which < digits; ++which) {
        const char one = text[at + which];

        made <<= 4;

        if (one >= '0' && one <= '9') made |= static_cast<std::uint32_t>(one - '0');
        else if (one >= 'A' && one <= 'F') made |= static_cast<std::uint32_t>(one - 'A' + 10);
        else if (one >= 'a' && one <= 'f') made |= static_cast<std::uint32_t>(one - 'a' + 10);
        else return std::nullopt;
    }

    return made;
}

// Ucieczka w postaci zastępczej: bajt (`%XX`) albo jednostka UTF-16 (`%uXXXX`). Procent, za którym
// nie stoi poprawna ucieczka, jest zwykłym znakiem — zapis ręcznie poprawiony nie wywraca odczytu.
struct Escape {
    bool unit = false;
    std::uint32_t value = 0;
    std::size_t much = 0;
};

std::optional<Escape> escape_at(std::string_view text, std::size_t at) {
    if (text[at] != '%') return std::nullopt;

    if (at + 1 < text.size() && text[at + 1] == 'u')
        if (const std::optional<std::uint32_t> value = hexed(text, at + 2, 4)) return Escape{true, *value, 6};

    if (const std::optional<std::uint32_t> value = hexed(text, at + 1, 2)) return Escape{false, *value, 3};

    return std::nullopt;
}

}

std::string stored_bytes(std::string_view native) {
    std::string plain;
    std::string escapes;
    bool clean = true;

    for (std::size_t at = 0; at < native.size();) {
        const std::optional<Sign> one = sound(native, at);

        if (!one) {
            clean = false;

            escaped(static_cast<unsigned char>(native[at]), 2, escapes);

            ++at;

            continue;
        }

        put(one->value, plain);
        kept(one->value, escapes);

        at += one->much;
    }

    return chosen(clean, std::move(plain), std::move(escapes));
}

std::string stored_units(std::u16string_view native) {
    std::string plain;
    std::string escapes;
    bool clean = true;

    for (std::size_t at = 0; at < native.size(); ++at) {
        const char32_t one = native[at];

        if (one >= 0xD800 && one <= 0xDBFF && at + 1 < native.size() && native[at + 1] >= 0xDC00 &&
            native[at + 1] <= 0xDFFF) {
            const char32_t whole = 0x10000 + ((one - 0xD800) << 10) + (native[at + 1] - 0xDC00);

            put(whole, plain);
            kept(whole, escapes);

            ++at;

            continue;
        }

        if (one >= 0xD800 && one <= 0xDFFF) {
            clean = false;

            escaped(one, 4, escapes);

            continue;
        }

        put(one, plain);
        kept(one, escapes);
    }

    return chosen(clean, std::move(plain), std::move(escapes));
}

std::string restored_bytes(std::string_view said) {
    if (said.compare(0, marker.size(), marker) != 0) return std::string{said};

    said.remove_prefix(marker.size());

    std::string made;

    for (std::size_t at = 0; at < said.size();) {
        if (const std::optional<Escape> one = escape_at(said, at)) {
            // Samotna jednostka UTF-16 z Windows: trzy bajty WTF-8 — bez znaczenia tutaj, ale bez straty.
            if (one->unit) {
                made += static_cast<char>(0xE0 | (one->value >> 12));
                made += static_cast<char>(0x80 | ((one->value >> 6) & 0x3F));
                made += static_cast<char>(0x80 | (one->value & 0x3F));
            } else {
                made += static_cast<char>(one->value);
            }

            at += one->much;

            continue;
        }

        made += said[at];

        ++at;
    }

    return made;
}

std::u16string restored_units(std::string_view said) {
    const bool escaping = said.compare(0, marker.size(), marker) == 0;

    if (escaping) said.remove_prefix(marker.size());

    std::u16string made;

    for (std::size_t at = 0; at < said.size();) {
        if (escaping) {
            if (const std::optional<Escape> one = escape_at(said, at)) {
                // Bajt spoza ASCII z POSIX-a nie ma tu znaczenia — znak zastępczy.
                if (one->unit) made += static_cast<char16_t>(one->value);
                else put(one->value < 0x80 ? one->value : substitute, made);

                at += one->much;

                continue;
            }
        }

        if (const std::optional<Sign> one = sound(said, at)) {
            put(one->value, made);

            at += one->much;

            continue;
        }

        put(substitute, made);

        ++at;
    }

    return made;
}

}

#include "proving.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

#include "talcite/fs.hpp"

#include "storing.hpp"

using namespace talcite;

// **Ścieżka do przechowania** — zawsze poprawny UTF-8 (JSON nie przyjmie innego) i zawsze z powrotem
// ta sama ścieżka. Obie gałęzie, bajtów POSIX-a i jednostek UTF-16 Windows, są zwykłymi funkcjami,
// więc sprawdzamy je tu obie; na końcu prawdziwy plik o nazwie spoza UTF-8 przechodzi tam i z powrotem.
namespace {

const std::string marker = "\xEF\xBF\xBF";

// Czy napis jest poprawnym UTF-8 — liczone osobno, nie tą samą funkcją, którą sprawdzamy.
bool valid(const std::string& text) {
    std::size_t at = 0;

    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t much = 0;
        std::uint32_t value = 0;

        if (lead < 0x80) {
            ++at;
            continue;
        }

        if ((lead & 0xE0) == 0xC0) {
            much = 2;
            value = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            much = 3;
            value = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            much = 4;
            value = lead & 0x07u;
        } else {
            return false;
        }

        if (at + much > text.size()) return false;

        for (std::size_t which = 1; which < much; ++which) {
            const auto next = static_cast<unsigned char>(text[at + which]);

            if ((next & 0xC0) != 0x80) return false;

            value = (value << 6) | (next & 0x3Fu);
        }

        const std::uint32_t least = much == 2 ? 0x80 : much == 3 ? 0x800 : 0x10000;

        if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return false;

        at += much;
    }

    return true;
}

}

TRIAL("Przechowanie: nazwa, która jest tekstem, zostaje sobą") {
    for (const std::string one : {std::string{}, std::string{"film.mkv"}, std::string{"Żółw – 2001 (100%).mkv"},
                                  std::string{"a\"b\\c\n</x>{}"}, std::string{"\xF0\x9F\x8E\xAC kino"},
                                  std::string{"zespół/płyta 1/01 – utwór.flac"}, std::string{"100%41 i %uD800"}}) {
        same(fs::stored_bytes(one), one);
        same(fs::restored_bytes(one), one);
    }

    same(fs::stored_units(u"Żółw 100%.mkv"), std::string{"Żółw 100%.mkv"});
    same(fs::stored_units(u"\U0001F3AC"), std::string{"\xF0\x9F\x8E\xAC"});
    ought(fs::restored_units("Żółw 100%.mkv") == std::u16string{u"Żółw 100%.mkv"});
    // Ucieczki znaczą coś tylko w postaci zastępczej — w zwykłej nazwie „%41” to trzy znaki.
    ought(fs::restored_units("100%41 i %uD800") == std::u16string{u"100%41 i %uD800"});
}

// Latin-1 ze starego archiwum: bajt spoza UTF-8 idzie ucieczką, procent obok też, reszta jak jest.
TRIAL("Przechowanie: bajty spoza UTF-8 idą ucieczką i wracają co do bajtu") {
    const std::string latin = "Am\xE9lie 100%.mkv";
    const std::string made = fs::stored_bytes(latin);

    same(made, marker + "Am%E9lie 100%25.mkv");
    ought(valid(made));
    same(fs::restored_bytes(made), latin);

    // Każdy rodzaj zepsucia: bajt ciągu bez początku, ucięty znak, przedłużony zapis, połowa pary
    // zastępczej zapisana w UTF-8, ponad U+10FFFF, bajty, których w UTF-8 nie ma wcale.
    for (const std::string broken : {std::string{"\x80"}, std::string{"a\xC3"}, std::string{"\xC0\x80"},
                                     std::string{"\xE0\x80\xAF"}, std::string{"\xED\xA0\x80"},
                                     std::string{"\xF4\x90\x80\x80"}, std::string{"\xF5\x80\x80\x80"},
                                     std::string{"\xFE\xFF"}, std::string{"ok\xE2\x82"}}) {
        const std::string one = fs::stored_bytes(broken);

        ought(one.rfind(marker, 0) == 0);
        ought(valid(one));
        same(fs::restored_bytes(one), broken);
    }
}

// Znak ucięty końcem widoku, choć w pamięci za nim stoi jego dalszy ciąg: liczy się koniec widoku,
// nie to, co leży za nim.
TRIAL("Przechowanie: znak ucięty końcem napisu nie sięga za koniec") {
    const std::string whole = "a\xC3\xA9";

    same(fs::stored_bytes(std::string_view{whole.data(), 2}), marker + "a%C3");
    same(fs::stored_bytes(std::string_view{whole.data() + 1, 1}), marker + "%C3");

    // Ucieczka ucięta końcem widoku też nie sięga za niego.
    const std::string kept = marker + "x%41";

    same(fs::restored_bytes(std::string_view{kept.data(), kept.size() - 1}), std::string{"x%4"});
    ought(fs::restored_units(std::string_view{kept.data(), kept.size() - 1}) == std::u16string{u"x%4"});

    const std::string unit = marker + "%uD8001";

    same(fs::restored_bytes(std::string_view{unit.data(), unit.size() - 2}), std::string{"%uD80"});
}

TRIAL("Przechowanie: samotna połowa pary UTF-16 idzie ucieczką i wraca") {
    const std::u16string lone = u"a" + std::u16string(1, char16_t{0xD800}) + u"b%" + std::u16string(1, char16_t{0xDFFF});
    const std::string made = fs::stored_units(lone);

    same(made, marker + "a%uD800b%25%uDFFF");
    ought(valid(made));
    ought(fs::restored_units(made) == lone);

    // Para w złej kolejności to dwie samotne połowy.
    const std::u16string reversed{char16_t{0xDC00}, char16_t{0xD800}};

    same(fs::stored_units(reversed), marker + "%uDC00%uD800");
    ought(fs::restored_units(fs::stored_units(reversed)) == reversed);
}

// Nazwa, która naprawdę zaczyna się od U+FFFF, też idzie postacią zastępczą — inaczej odczyt wziąłby
// ją za ucieczkę. Żadna nazwa nie ma dwóch zapisów, żaden zapis dwóch nazw.
TRIAL("Przechowanie: nazwa zaczynająca się od znaku znacznika nie myli odczytu") {
    const std::string odd = marker + "%41";

    same(fs::stored_bytes(odd), marker + marker + "%2541");
    same(fs::restored_bytes(fs::stored_bytes(odd)), odd);
    ought(fs::restored_units(fs::stored_units(u"￿%41")) == std::u16string{u"￿%41"});

    // Znacznik w środku nazwy niczego nie zmienia.
    same(fs::stored_bytes("a" + marker), "a" + marker);
}

// Zapis z jednego systemu czytany na drugim nie wywraca odczytu: bajt spoza ASCII na Windows staje
// się znakiem zastępczym, samotna jednostka na POSIX-ie — trzema bajtami WTF-8. Ucieczka popsuta
// ręcznie jest zwykłym tekstem.
TRIAL("Przechowanie: obcy zapis i popsuta ucieczka czytają się bez wywrotki") {
    ought(fs::restored_units(marker + "Am%E9lie%41") == std::u16string{u"Am�lieA"});
    same(fs::restored_bytes(marker + "%uD800"), std::string{"\xED\xA0\x80"});
    same(fs::restored_bytes(marker + "50%zz%4%u12G4%"), std::string{"50%zz%4%u12G4%"});
    ought(fs::restored_units(marker + "50%zz%") == std::u16string{u"50%zz%"});
    ought(fs::restored_units("bad\xFF") == std::u16string{u"bad�"});
}

// Dowolne bajty i dowolne jednostki: zapis zawsze poprawnym UTF-8, odczyt zawsze tym samym.
TRIAL("Przechowanie: losowe nazwy zawsze tam i z powrotem") {
    std::mt19937 drawn{20261005};
    std::uniform_int_distribution<int> length{0, 24};
    std::uniform_int_distribution<int> byte{1, 255};
    std::uniform_int_distribution<int> unit{1, 0xFFFF};

    for (int turn = 0; turn < 3000; ++turn) {
        std::string bytes(static_cast<std::size_t>(length(drawn)), '\0');

        for (char& one : bytes) one = static_cast<char>(byte(drawn));

        const std::string made = fs::stored_bytes(bytes);

        ought(valid(made));
        same(fs::restored_bytes(made), bytes);

        std::u16string units(static_cast<std::size_t>(length(drawn)), u'\0');

        for (char16_t& one : units) one = static_cast<char16_t>(unit(drawn));

        const std::string spelled = fs::stored_units(units);

        ought(valid(spelled));
        ought(fs::restored_units(spelled) == units);
    }
}

// Prawdziwy plik: na POSIX-ie nazwa z bajtem Latin-1, na Windows z samotną połową pary. Ścieżka
// przechowana i odtworzona wskazuje ten sam plik. System, który takiej nazwy nie przyjmie, pomija próbę.
TRIAL("Przechowanie: plik o nazwie spoza UTF-8 odnajduje się po zapisie") {
    namespace stood = std::filesystem;

    std::error_code why;

    const stood::path nook = stood::temp_directory_path() / "talcite-storing-probe";

    stood::remove_all(nook, why);
    stood::create_directories(nook, why);

#ifdef _WIN32
    const stood::path one = nook / stood::path{std::wstring{L"Am"} + wchar_t{0xD800} + L"lie.mkv"};
#else
    const stood::path one = nook / stood::path{std::string{"Am\xE9lie.mkv"}};
#endif

    {
        std::ofstream making{one, std::ios::binary};

        making << "x";
    }

    if (!stood::exists(one, why)) {
        std::cout << "        ten system nie przyjmuje takiej nazwy — próba pominięta\n";

        stood::remove_all(nook, why);

        return;
    }

    const std::string kept = fs::stored(one);

    ought(valid(kept));
    ought(fs::restored(kept) == one);
    ought(stood::exists(fs::restored(kept), why));

#ifdef _WIN32
    // Tekst do pokazania nie rzuca i jest tekstem — zepsuta jednostka staje się znakiem zastępczym.
    same(fs::to_string(one).substr(fs::to_string(one).size() - 10), std::string{"\xEF\xBF\xBDlie.mkv"});
#endif

    stood::remove_all(nook, why);
}

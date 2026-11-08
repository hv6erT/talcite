#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <mutex>
#include <string>

#include "tal/text.hpp"

// The rules of languages that need no translation: the forms of the plural, how numbers and sizes
// are written and the order of a date written in numbers. They follow the Unicode CLDR for the
// languages of twa (cs, de, en, es, fr, it, ja, ko, nl, pl, pt, ru, uk, zh) and for sk and sv;
// another language takes the rules of English. Words (the names of months, of days) belong to the
// texts of a program, not here.

namespace tal::text {

namespace {

// A narrow and a plain no-break space, which hold a number and what follows it together.
constexpr std::string_view narrow_space = "\xE2\x80\xAF";
constexpr std::string_view no_break = "\xC2\xA0";

// How a language writes numbers: the decimal sign, the group separator and the fewest digits
// before the separator that group at all (CLDR's minimumGroupingDigits plus three).
struct Writing {
    std::string_view base;
    std::string_view decimal;
    std::string_view group;
    int grouped;
};

// The languages that differ from English in writing numbers.
constexpr std::array<Writing, 15> writings{{
    {"cs", ",", no_break, 4},
    {"de", ",", ".", 4},
    {"es", ",", ".", 5},
    {"fr", ",", narrow_space, 4},
    {"it", ",", ".", 4},
    {"nl", ",", ".", 4},
    {"pl", ",", no_break, 5},
    {"pt", ",", ".", 4},
    {"ru", ",", no_break, 4},
    {"sk", ",", no_break, 4},
    {"sv", ",", no_break, 4},
    {"uk", ",", no_break, 4},
    {"ja", ".", ",", 4},
    {"ko", ".", ",", 4},
    {"zh", ".", ",", 4},
}};

// How a language writes numbers; English for one the table does not know.
Writing writing_of(const Language& which) {
    const std::string_view base = which.base();

    for (const Writing& one : writings)
        if (one.base == base) return one;

    if (which.tag() == "pt-PT") return Writing{"pt", ",", no_break, 4};

    return Writing{"en", ".", ",", 4};
}

// The program's language, chosen once at its start and read from any thread.
struct Chosen {
    std::mutex guard;
    Language language{"en-US"};
};

// The one choice of the process, never destroyed.
Chosen& chosen() {
    static auto* const made = new Chosen;

    return *made;
}

// The calendar fields of a moment in local time.
std::tm local(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm fields{};

#ifdef _WIN32
    (void)localtime_s(&fields, &seconds);
#else
    (void)localtime_r(&seconds, &fields);
#endif

    return fields;
}

// A number with at least `digits` digits, zeros in front.
std::string padded(int value, int digits) {
    std::string made = std::to_string(value);

    if (static_cast<int>(made.size()) < digits) made.insert(0, static_cast<std::size_t>(digits - static_cast<int>(made.size())), '0');

    return made;
}

}

// A language by its BCP 47 tag; '_' as in "pl_PL" counts as '-', and the language is written in
// small letters and the region in capitals, so "PL-pl" is "pl-PL".
Language::Language(std::string_view tag) {
    std::size_t part = 0;

    for (const char one : tag) {
        if (one == '-' || one == '_') {
            tag_ += '-';
            ++part;

            continue;
        }

        const bool region = part == 1;

        tag_ += region && one >= 'a' && one <= 'z' ? static_cast<char>(one - 'a' + 'A') : !region && one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one;
    }
}

// The language without its region: "pl" for "pl-PL".
std::string_view Language::base() const { return std::string_view{tag_}.substr(0, tag_.find('-')); }

// The program's language: what it chose, or American English.
Language current() {
    Chosen& one = chosen();
    const std::lock_guard<std::mutex> kept{one.guard};

    return one.language;
}

// Chooses the program's language.
void choose(Language which) {
    Chosen& one = chosen();
    const std::lock_guard<std::mutex> kept{one.guard};

    one.language = std::move(which);
}

// The plural form of a whole number in a language, by the CLDR rules. English, German, Dutch and
// Swedish tell one from the rest; Spanish and Italian give whole millions a form of their own,
// French and Portuguese too and count zero as one; Czech and Slovak have few for 2-4; Polish few
// for 2-4 (not 12-14) and many for the rest; Russian and Ukrainian also one for 21, 31 and so on.
// Japanese, Korean and Chinese do not inflect.
Plural plural(std::uint64_t many, Language which) {
    const std::string_view base = which.base();
    const std::uint64_t tens = many % 10;
    const std::uint64_t hundreds = many % 100;
    const bool millions = many != 0 && many % 1'000'000 == 0;
    const bool few = tens >= 2 && tens <= 4 && (hundreds < 12 || hundreds > 14);

    if (base == "ja" || base == "ko" || base == "zh") return Plural::other;

    if (base == "es" || base == "it") return many == 1 ? Plural::one : millions ? Plural::many : Plural::other;

    if (base == "fr" || base == "pt") return many <= 1 ? Plural::one : millions ? Plural::many : Plural::other;

    if (base == "cs" || base == "sk") return many == 1 ? Plural::one : many >= 2 && many <= 4 ? Plural::few : Plural::other;

    if (base == "pl") return many == 1 ? Plural::one : few ? Plural::few : Plural::many;

    if (base == "ru" || base == "uk") return tens == 1 && hundreds != 11 ? Plural::one : few ? Plural::few : Plural::many;

    return many == 1 ? Plural::one : Plural::other;
}

// The plural forms a language uses for whole numbers, each once, in the order numbers first take
// them: the forms a text with a number needs in that language.
std::vector<Plural> plurals(Language which) {
    std::vector<Plural> made;

    for (const std::uint64_t many : {1, 2, 5, 0, 11, 21, 22, 1'000'000})
        if (const Plural one = plural(many, which); std::find(made.begin(), made.end(), one) == made.end()) made.push_back(one);

    return made;
}

// A number with `decimals` digits after the decimal sign, rounded, its digits grouped by
// thousands as the language groups them: "1 234,5" in Polish, "1,234.5" in English. A minus is
// ASCII's, as terminals and fonts all have it.
std::string number(double value, int decimals, Language which) {
    const Writing how = writing_of(which);

    decimals = std::clamp(decimals, 0, 17);

    if (!std::isfinite(value)) return std::isnan(value) ? "NaN" : value < 0 ? "-∞" : "∞";

    std::array<char, 400> buffer{};
    const auto [end, problem] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed, decimals);
    std::string_view plain{buffer.data(), problem == std::errc{} ? static_cast<std::size_t>(end - buffer.data()) : 0};

    std::string made;

    if (plain.starts_with('-')) {
        plain.remove_prefix(1);

        if (plain.find_first_not_of("0.") != std::string_view::npos) made += '-';
    }

    const std::size_t point = plain.find('.');
    const std::string_view whole = plain.substr(0, point);
    const bool grouping = static_cast<int>(whole.size()) >= how.grouped;

    for (std::size_t at = 0; at < whole.size(); ++at) {
        if (grouping && at > 0 && (whole.size() - at) % 3 == 0) made += how.group;

        made += whole[at];
    }

    if (point != std::string_view::npos) {
        made += how.decimal;
        made += plain.substr(point + 1);
    }

    return made;
}

// A size in bytes as people read it, in decimal units: bytes below a thousand, then kB, MB, GB and
// so on, with one decimal below a hundred units and none above: "512 B", "1,2 GB", "123 MB".
// French writes octets (ko, Mo), Russian and Ukrainian their letter (КБ, МБ).
std::string size(std::uint64_t bytes, Language which) {
    const std::string_view base = which.base();
    const bool french = base == "fr";
    const bool cyrillic = base == "ru" || base == "uk";

    static constexpr std::array<std::string_view, 7> latin{"B", "kB", "MB", "GB", "TB", "PB", "EB"};
    static constexpr std::array<std::string_view, 7> octets{"o", "ko", "Mo", "Go", "To", "Po", "Eo"};
    static constexpr std::array<std::string_view, 7> letters{"Б", "кБ", "МБ", "ГБ", "ТБ", "ПБ", "ЭБ"};

    const auto& units = french ? octets : cyrillic ? letters : latin;

    if (bytes < 1000) return std::to_string(bytes) + std::string{no_break} + std::string{units[0]};

    double value = static_cast<double>(bytes);
    std::size_t unit = 0;

    while (value >= 999.95 && unit + 1 < units.size()) {
        value /= 1000;
        ++unit;
    }

    return number(value, value < 99.95 ? 1 : 0, which) + std::string{no_break} + std::string{units[unit]};
}

// A moment in local time written in numbers, in the order and with the signs of the language:
// "08.11.2026" and "14:52" in Polish, "11/8/2026" and "2:52 PM" in American English,
// "2026-11-08" in Swedish, "2026/11/08" in Japanese; a language the rules do not know is written
// as in American English. A stamp is the date and the time.
std::string date(std::chrono::system_clock::time_point when, Style style, Language which) {
    const std::tm fields = local(when);
    const int year = fields.tm_year + 1900;
    const int month = fields.tm_mon + 1;
    const int day = fields.tm_mday;
    const std::string_view base = which.base();
    const bool american = base == "en" ? which.tag() == "en" || which.tag() == "en-US" : writing_of(which).base == "en";

    std::string day_text;

    if (american) {
        day_text = std::to_string(month) + "/" + std::to_string(day) + "/" + std::to_string(year);
    } else if (base == "pl" || base == "de" || base == "ru" || base == "uk") {
        day_text = padded(day, 2) + "." + padded(month, 2) + "." + std::to_string(year);
    } else if (base == "cs" || base == "sk") {
        day_text = std::to_string(day) + ". " + std::to_string(month) + ". " + std::to_string(year);
    } else if (base == "nl") {
        day_text = padded(day, 2) + "-" + padded(month, 2) + "-" + std::to_string(year);
    } else if (base == "sv") {
        day_text = std::to_string(year) + "-" + padded(month, 2) + "-" + padded(day, 2);
    } else if (base == "ja") {
        day_text = std::to_string(year) + "/" + padded(month, 2) + "/" + padded(day, 2);
    } else if (base == "zh") {
        day_text = std::to_string(year) + "/" + std::to_string(month) + "/" + std::to_string(day);
    } else if (base == "ko") {
        day_text = std::to_string(year) + ". " + std::to_string(month) + ". " + std::to_string(day) + ".";
    } else {
        day_text = padded(day, 2) + "/" + padded(month, 2) + "/" + std::to_string(year);
    }

    std::string time_text;

    if (american) {
        const int hour = fields.tm_hour % 12 == 0 ? 12 : fields.tm_hour % 12;

        time_text = std::to_string(hour) + ":" + padded(fields.tm_min, 2) + std::string{no_break} + (fields.tm_hour < 12 ? "AM" : "PM");
    } else {
        time_text = padded(fields.tm_hour, 2) + ":" + padded(fields.tm_min, 2);
    }

    if (style == Style::date) return day_text;

    if (style == Style::time) return time_text;

    return day_text + " " + time_text;
}

}

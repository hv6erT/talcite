#include "talcite/comb.hpp"

#include <cctype>
#include <regex>
#include <utility>

#include "talcite/encoding.hpp"

namespace talcite {

namespace {

char lowered(char one) {
    const unsigned char sign = static_cast<unsigned char>(one);

    return static_cast<char>(std::tolower(sign));
}

// Granica wyrazu liczona **po bajtach**, regułą z `encoding.hpp` — tą samą, którą stosuje
// zaznaczanie wyrazu podwójnym kliknięciem. Poszerzony bajt ciągu UTF-8 daje tam tę samą
// odpowiedź co znak, z którego pochodzi, więc nie trzeba tu odtwarzać znaków.
bool lettered(char one) {
    return wordly(static_cast<char32_t>(static_cast<unsigned char>(one)));
}

bool bounded(std::string_view where, Hit one) {
    if (one.from > 0 && lettered(where[one.from - 1])) return false;

    if (one.upto < where.size() && lettered(where[one.upto])) return false;

    return true;
}

// Zwykłe szukanie napisu, bajt po bajcie. Bez wielkości liter porównujemy **tylko ASCII** —
// zwijanie wielkości poza nim to znowu Unicode, więc „Ą" nie znajdzie się przez „ą".
std::optional<std::size_t> hunted(std::string_view where, std::string_view what, std::size_t from,
                                  bool cased) {
    if (what.empty() || what.size() > where.size()) return {};

    for (std::size_t at = from; at + what.size() <= where.size(); ++at) {
        std::size_t which = 0;

        while (which < what.size()) {
            const char one = where[at + which];
            const char two = what[which];

            if (!(cased ? one == two : lowered(one) == lowered(two))) break;

            ++which;
        }

        if (which == what.size()) return at;
    }

    return {};
}

}

struct Comb::Kept {
    std::string what;
    Sift how;

    std::optional<std::regex> exp;

    std::string wrong;
    bool ready = false;
};

Comb::Comb() : kept_(std::make_unique<Kept>()) { kept_->wrong = "Pusty wzorzec."; }

// **Konstruktor nie rzuca.** Wzorzec przychodzi z pola tekstowego, a użytkownik ma pełne prawo
// wpisać „[" — to nie jest błąd programu, tylko odpowiedź „tak się nie da", którą trzeba pokazać.
Comb::Comb(std::string what, Sift how) : kept_(std::make_unique<Kept>()) {
    kept_->what = std::move(what);
    kept_->how = how;

    if (kept_->what.empty()) {
        kept_->wrong = "Pusty wzorzec.";

        return;
    }

    if (!how.regex) {
        kept_->ready = true;

        return;
    }

    auto flags = std::regex_constants::ECMAScript;

    if (!how.cased) flags |= std::regex_constants::icase;

    try {
        kept_->exp.emplace(kept_->what, flags);
        kept_->ready = true;
    } catch (const std::regex_error&) {
        // Treść od biblioteki standardowej jest po angielsku i zależy od implementacji, więc
        // mówimy swoje. Do rozpoznania, co dokładnie jest nie tak, i tak by nie wystarczyła.
        kept_->wrong = "Niepoprawne wyrażenie regularne.";
    }
}

Comb::~Comb() = default;

Comb::Comb(Comb&& other) noexcept = default;

Comb& Comb::operator=(Comb&& other) noexcept = default;

bool Comb::ready() const { return kept_->ready; }

const std::string& Comb::wrong() const { return kept_->wrong; }

const std::string& Comb::what() const { return kept_->what; }

Sift Comb::how() const { return kept_->how; }

namespace {

// Pierwsze dopasowanie od podanego bajtu, jeszcze **bez** warunku całych wyrazów.
std::optional<Hit> earliest(const std::regex* exp, std::string_view what, std::string_view where,
                            std::size_t from, bool cased) {
    if (from > where.size()) return {};

    if (exp == nullptr) {
        const std::optional<std::size_t> at = hunted(where, what, from, cased);

        if (!at.has_value()) return {};

        return Hit{*at, *at + what.size()};
    }

    const char* end = where.data() + where.size();

    std::size_t at = from;

    // Puste dopasowanie — „x*" wszędzie tam, gdzie nie ma iksa — **pomijamy i szukamy dalej**.
    // Zamiana pustki nie ma sensu, a poddanie się na pierwszej pustce znaczyłoby, że „x*" nie
    // znajduje iksów.
    while (at <= where.size()) {
        std::cmatch got;

        auto flags = std::regex_constants::match_default;

        // Bez tego „^" dopasowałoby się w środku wiersza, a „\b" nie widziałoby bajtu przed sobą.
        if (at > 0) flags |= std::regex_constants::match_prev_avail;

        if (!std::regex_search(where.data() + at, end, got, *exp, flags)) return {};

        const std::size_t began = at + static_cast<std::size_t>(got.position(0));
        const std::size_t much = static_cast<std::size_t>(got.length(0));

        if (much > 0) return Hit{began, began + much};

        at = began + 1;
    }

    return {};
}

}

std::optional<Hit> Comb::forth(std::string_view where, std::size_t from) const {
    const Kept& kept = *kept_;

    if (!kept.ready) return {};

    std::size_t at = from;

    while (at <= where.size()) {
        const std::optional<Hit> one =
            earliest(kept.exp.has_value() ? &*kept.exp : nullptr, kept.what, where, at, kept.how.cased);

        if (!one.has_value()) return {};

        if (!kept.how.whole || bounded(where, *one)) return one;

        at = one->from + 1;
    }

    return {};
}

// Wstecz idziemy od początku wiersza do przodu i zapamiętujemy ostatnie dopasowanie **zaczynające
// się przed** podanym miejscem. Szukanie od tyłu byłoby szybsze, ale wyrażenie regularne czyta się
// tylko w jedną stronę, więc i tak trzeba by przejść wiersz od początku.
std::optional<Hit> Comb::back(std::string_view where, std::size_t upto) const {
    std::optional<Hit> made;

    std::size_t at = 0;

    while (true) {
        const std::optional<Hit> one = forth(where, at);

        if (!one.has_value() || one->from >= upto) break;

        made = one;
        at = one->from + 1;
    }

    return made;
}

std::vector<Hit> Comb::all(std::string_view where) const {
    std::vector<Hit> made;

    std::size_t at = 0;

    while (true) {
        const std::optional<Hit> one = forth(where, at);

        if (!one.has_value()) break;

        made.push_back(*one);

        at = one->upto > one->from ? one->upto : one->from + 1;
    }

    return made;
}

}

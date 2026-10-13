#include "talcite/comb.hpp"

#include <cctype>
#include <regex>
#include <utility>

#include "talcite/encoding.hpp"

namespace talcite {

namespace {

// Lowers an ASCII letter; every other byte comes back unchanged.
char lowered(char one) {
    const unsigned char sign = static_cast<unsigned char>(one);

    return static_cast<char>(std::tolower(sign));
}

// A word boundary counted in bytes, with the rule from `encoding.hpp`, the same one that
// double-click word selection uses. A widened byte of a UTF-8 sequence gives the same answer
// there as the character it comes from, so characters do not have to be rebuilt here.
bool lettered(char one) {
    return in_word(static_cast<char32_t>(static_cast<unsigned char>(one)));
}

// True when the hit stands as a whole word: no word byte touches it from either side.
bool bounded(std::string_view where, Hit one) {
    if (one.from > 0 && lettered(where[one.from - 1])) return false;

    if (one.upto < where.size() && lettered(where[one.upto])) return false;

    return true;
}

// A plain substring search, byte by byte. Without case sensitivity only ASCII is compared:
// case folding beyond it is Unicode again, so "Ą" is not found through "ą".
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

// An empty comb, which finds nothing and reports an empty pattern.
Comb::Comb() : kept_(std::make_unique<Kept>()) { kept_->wrong = "Empty pattern."; }

// The constructor does not throw. The pattern comes from a text field, and the user has every
// right to type "[". That is not a program error but an answer, "this cannot be done", which has
// to be shown; it ends up in `wrong()`.
Comb::Comb(std::string what, Sift how) : kept_(std::make_unique<Kept>()) {
    kept_->what = std::move(what);
    kept_->how = how;

    if (kept_->what.empty()) {
        kept_->wrong = "Empty pattern.";

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
        // The text from the standard library is in English and depends on the implementation, so
        // we say our own. It would not be enough to tell what exactly is wrong anyway.
        kept_->wrong = "Invalid regular expression.";
    }
}

Comb::~Comb() = default;

Comb::Comb(Comb&& other) noexcept = default;

Comb& Comb::operator=(Comb&& other) noexcept = default;

// True when the pattern is usable; otherwise `wrong()` says why it is not.
bool Comb::ready() const { return kept_->ready; }

// Why the pattern cannot be used; meaningful only when `ready()` is false.
const std::string& Comb::wrong() const { return kept_->wrong; }

// The pattern as it was given.
const std::string& Comb::what() const { return kept_->what; }

// The search options the comb was made with.
Sift Comb::how() const { return kept_->how; }

namespace {

// The first match from the given byte on, still without the whole-word condition.
std::optional<Hit> earliest(const std::regex* exp, std::string_view what, std::string_view where,
                            std::size_t from, bool cased, bool cut) {
    if (from > where.size()) return {};

    if (exp == nullptr) {
        const std::optional<std::size_t> at = hunted(where, what, from, cased);

        if (!at.has_value()) return {};

        return Hit{*at, *at + what.size()};
    }

    const char* end = where.data() + where.size();

    std::size_t at = from;

    // An empty match ("x*" wherever there is no x) is skipped and the search goes on. Replacing
    // emptiness makes no sense, and giving up at the first empty match would mean that "x*" does
    // not find any x.
    while (at <= where.size()) {
        std::cmatch got;

        auto flags = std::regex_constants::match_default;

        // Without this "^" would match in the middle of a line and "\b" would not see the byte
        // before it.
        if (at > 0) flags |= std::regex_constants::match_prev_avail;

        // A piece cut out of a longer line does not end where the line does, so "$" must not match
        // at its end.
        if (cut) flags |= std::regex_constants::match_not_eol;

        if (!std::regex_search(where.data() + at, end, got, *exp, flags)) return {};

        const std::size_t began = at + static_cast<std::size_t>(got.position(0));
        const std::size_t much = static_cast<std::size_t>(got.length(0));

        if (much > 0) return Hit{began, began + much};

        at = began + 1;
    }

    return {};
}

}

// The first hit that begins at or after `from`, honoring the whole-word option; nothing when the
// pattern is not ready or there is no further hit. `cut` says that `where` is a piece of a longer
// line that goes on past its end, as when a long line is searched window by window.
std::optional<Hit> Comb::forth(std::string_view where, std::size_t from, bool cut) const {
    const Kept& kept = *kept_;

    if (!kept.ready) return {};

    std::size_t at = from;

    while (at <= where.size()) {
        const std::optional<Hit> one =
            earliest(kept.exp.has_value() ? &*kept.exp : nullptr, kept.what, where, at, kept.how.cased, cut);

        if (!one.has_value()) return {};

        if (!kept.how.whole || bounded(where, *one)) return one;

        at = one->from + 1;
    }

    return {};
}

// Searching backward goes from the start of the line forward and keeps the last match that begins
// before the given place. Searching from the end would be faster, but a regular expression reads
// in one direction only, so the line would have to be walked from the start anyway.
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

// Every hit in the line, from left to right and without overlaps; the next search starts where
// the previous hit ends.
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

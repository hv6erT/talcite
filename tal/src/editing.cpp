#include "tal/editing.hpp"

#include <stdexcept>
#include <utility>

#include "tal/text.hpp"

namespace tal {

namespace {

// The pieces of `what` between its line ends: \n, with \r\n counted as \n.
std::vector<std::string_view> broken(std::string_view what) {
    std::vector<std::string_view> made;
    std::size_t from = 0;

    while (true) {
        const std::size_t end = what.find('\n', from);
        std::string_view piece = what.substr(from, end == std::string_view::npos ? std::string_view::npos : end - from);

        if (end != std::string_view::npos && piece.ends_with('\r')) piece.remove_suffix(1);

        made.push_back(piece);

        if (end == std::string_view::npos) return made;

        from = end + 1;
    }
}

// Whether the byte `at` of `line` starts a character or ends the line: not inside a UTF-8 sequence.
bool bordered(std::string_view line, std::size_t at) {
    return at == line.size() || (static_cast<unsigned char>(line[at]) & 0xC0) != 0x80;
}

// The character starting at the byte `at` of `where`, or none past its end or for broken UTF-8.
std::optional<char32_t> character(std::string_view where, std::size_t at) {
    std::size_t next = at;

    return text::decoded(where, next);
}

// Whether `one` stands in `where` as a whole word: no character of a word touches it on either
// side.
bool bounded(std::string_view where, Hit one) {
    if (one.from > 0) {
        std::size_t start = one.from - 1;

        while (start > 0 && !bordered(where, start)) --start;

        if (const std::optional<char32_t> left = character(where, start); left && text::in_word(*left)) return false;
    }

    const std::optional<char32_t> right = character(where, one.upto);

    return !(right && text::in_word(*right));
}

// Where `what` stands in `where` from the byte `from` on, compared character by character, folded
// unless `cased`; the end of the stretch it covers there goes to `upto`. A start inside a character
// decodes to nothing and matches nothing.
std::optional<std::size_t> hunted(std::string_view where, std::string_view what, std::size_t from, bool cased, std::size_t& upto) {
    for (std::size_t at = from; at < where.size(); ++at) {
        std::size_t here = at;
        std::size_t there = 0;
        bool same = true;

        while (same && there < what.size()) {
            const std::optional<char32_t> one = text::decoded(where, here);
            const std::optional<char32_t> two = text::decoded(what, there);

            same = one && two && (cased ? *one == *two : text::folded(*one) == text::folded(*two));
        }

        if (same) {
            upto = here;

            return at;
        }
    }

    return std::nullopt;
}

}

// A text of the lines of `whole`, split at its line ends; an empty text has one empty line.
Text::Text(std::string_view whole) {
    for (const std::string_view one : broken(whole)) lines_.emplace_back(one);
}

// The line `at`, without its end; a line past the last throws std::out_of_range.
std::string_view Text::line(std::size_t at) const {
    if (at >= lines_.size()) throw std::out_of_range{"tal::Text::line: no such line"};

    return lines_[at];
}

// All the lines joined by \n.
std::string Text::whole() const {
    std::string made;

    for (std::size_t at = 0; at < lines_.size(); ++at) {
        if (at > 0) made += '\n';

        made += lines_[at];
    }

    return made;
}

// The text from `from` to `to`, lines joined by \n. A place outside the text or inside a character
// throws std::out_of_range, and `to` before `from` std::invalid_argument.
std::string Text::between(Place from, Place to) const {
    checked(from);
    checked(to);

    if (to < from) throw std::invalid_argument{"tal::Text::between: the end before the start"};

    if (from.line == to.line) return lines_[from.line].substr(from.byte, to.byte - from.byte);

    std::string made = lines_[from.line].substr(from.byte);

    for (std::size_t at = from.line + 1; at < to.line; ++at) made += '\n' + lines_[at];

    return made + '\n' + lines_[to.line].substr(0, to.byte);
}

// Puts `what` at `at`, its line ends (\n or \r\n) breaking the line; gives where it ends. The views
// hear the line changed and the lines that came after it. A place outside the text or inside a
// character throws std::out_of_range.
Place Text::insert(Place at, std::string_view what) {
    checked(at);

    const std::vector<std::string_view> pieces = broken(what);
    std::string& here = lines_[at.line];
    const std::string tail = here.substr(at.byte);

    here.erase(at.byte);
    here += pieces.front();

    if (pieces.size() == 1) {
        const Place end{at.line, here.size()};

        here += tail;
        told(Changes{Changes::Kind::changed, at.line, 1});

        return end;
    }

    std::vector<std::string> added(pieces.begin() + 1, pieces.end());
    const Place end{at.line + added.size(), added.back().size()};

    added.back() += tail;
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at.line + 1), added.begin(), added.end());
    told(Changes{Changes::Kind::changed, at.line, 1});
    told(Changes{Changes::Kind::inserted, at.line + 1, added.size()});

    return end;
}

// Takes away the text from `from` to `to`, joining their lines. The views hear the line changed and
// the lines that went after it. A place outside the text or inside a character throws
// std::out_of_range, and `to` before `from` std::invalid_argument.
void Text::erase(Place from, Place to) {
    checked(from);
    checked(to);

    if (to < from) throw std::invalid_argument{"tal::Text::erase: the end before the start"};

    if (from == to) return;

    lines_[from.line] = lines_[from.line].substr(0, from.byte) + lines_[to.line].substr(to.byte);

    const std::size_t gone = to.line - from.line;

    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(from.line + 1), lines_.begin() + static_cast<std::ptrdiff_t>(to.line + 1));
    told(Changes{Changes::Kind::changed, from.line, 1});

    if (gone > 0) told(Changes{Changes::Kind::removed, from.line + 1, gone});
}

// A line as an entry of the model: its text, in any column.
Entry Text::entry(std::size_t row, std::size_t column) const {
    (void)column;

    Entry made;

    made.text = std::string{line(row)};

    return made;
}

// Throws std::out_of_range for a place outside the text or inside a character.
void Text::checked(Place at) const {
    if (at.line >= lines_.size() || at.byte > lines_[at.line].size() || !bordered(lines_[at.line], at.byte))
        throw std::out_of_range{"tal::Text: no such place"};
}

// A comb for `what` as `how` says. It does not throw: an empty pattern or a broken expression makes
// a comb that finds nothing and says why in `wrong()`, since a pattern comes from what the user
// typed, and "[" is an answer to show, not a mistake of the program.
Comb::Comb(std::string what, Sift how) : what_(std::move(what)), how_(how) {
    if (what_.empty()) {
        wrong_ = "Empty pattern.";

        return;
    }

    if (!how_.regex) return;

    auto flags = std::regex_constants::ECMAScript;

    if (!how_.cased) flags |= std::regex_constants::icase;

    try {
        expression_.emplace(what_, flags);
    } catch (const std::regex_error&) {
        wrong_ = "Invalid regular expression.";
    }
}

// The first hit in `where` that begins at the byte `from` or after it: on the borders of
// characters, as a whole word when asked. Nothing when there is none or the comb is not ready.
std::optional<Hit> Comb::forth(std::string_view where, std::size_t from) const {
    if (!ready()) return std::nullopt;

    for (std::size_t at = from; at <= where.size();) {
        const std::optional<Hit> one = earliest(where, at);

        if (!one) return std::nullopt;

        if (bordered(where, one->from) && bordered(where, one->upto) && (!how_.whole || bounded(where, *one))) return one;

        at = one->from + 1;
    }

    return std::nullopt;
}

// The last hit that begins before the byte `upto`. A regular expression reads one way only, so the
// line is walked from its start.
std::optional<Hit> Comb::back(std::string_view where, std::size_t upto) const {
    std::optional<Hit> made;

    for (std::optional<Hit> one = forth(where, 0); one && one->from < upto; one = forth(where, one->from + 1)) made = one;

    return made;
}

// Every hit in `where` from the left, none overlapping: the next is sought where the last ends.
std::vector<Hit> Comb::all(std::string_view where) const {
    std::vector<Hit> made;

    for (std::optional<Hit> one = forth(where, 0); one; one = forth(where, one->upto)) made.push_back(*one);

    return made;
}

// The first match from the byte `from` on, before the conditions of `forth`. An empty match of an
// expression is passed over, so that "x*" finds the x after it.
std::optional<Hit> Comb::earliest(std::string_view where, std::size_t from) const {
    if (from > where.size()) return std::nullopt;

    if (!expression_) {
        std::size_t upto = 0;
        const std::optional<std::size_t> at = hunted(where, what_, from, how_.cased, upto);

        if (!at) return std::nullopt;

        return Hit{*at, upto};
    }

    for (std::size_t at = from; at <= where.size();) {
        std::cmatch got;
        auto flags = std::regex_constants::match_default;

        // Without it "^" would match inside the line, and "\b" would not see the byte before.
        if (at > 0) flags |= std::regex_constants::match_prev_avail;

        if (!std::regex_search(where.data() + at, where.data() + where.size(), got, *expression_, flags)) return std::nullopt;

        const std::size_t began = at + static_cast<std::size_t>(got.position(0));
        const auto length = static_cast<std::size_t>(got.length(0));

        if (length > 0) return Hit{began, began + length};

        at = began + 1;
    }

    return std::nullopt;
}

}

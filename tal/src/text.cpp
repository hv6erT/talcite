#include "tal/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include "unicode.hpp"

namespace tal::text {

namespace {

// One run of a case table: from `first` to `last`, every `step`-th code point maps `delta` away.
struct Run {
    char32_t first;
    char32_t last;
    std::int32_t delta;
    std::uint32_t step;
};

#include "casing.inc"

// The code point a case table maps `one` to, or `one` when no run holds it.
template <std::size_t N>
char32_t mapped(const std::array<Run, N>& runs, char32_t one) {
    const auto found = std::lower_bound(runs.begin(), runs.end(), one, [](const Run& run, char32_t point) { return run.last < point; });

    if (found == runs.end() || one < found->first || (one - found->first) % found->step != 0) return one;

    return static_cast<char32_t>(static_cast<std::int64_t>(one) + found->delta);
}

// The next character of the text, by the rule of `span`; what the rule does not let through comes
// back as one replacement character for its maximal subpart, and so does a cut-off tail.
char32_t taken(std::string_view text, std::size_t& at) {
    if (at >= text.size()) return 0;

    return decoded(text, at).value_or(0xFFFD);
}

}

// The first sequence of `from`, by the one rule of talcite for UTF-8 (Unicode, table 3-7): whole
// when it is a well-formed character, of `size` bytes; partial when its bytes so far start one and
// `from` ends before it does, so a stream waits for the rest; bad otherwise, `size` being its
// maximal subpart, which stands for one replacement character. The rule forbids also what fits in
// the bits: a character written longer than it needs ("/" as C0 AF), half of a UTF-16 surrogate
// pair and anything past U+10FFFF. Let through, they would pass for another character (a hidden
// "/"), or for a number the screen uses for something else (past U+10FFFF lie the handles of
// grapheme clusters). The second byte bounds the rest, so a bad sequence is known as soon as
// possible. Nothing at all is a partial sequence of no bytes.
Span span(std::string_view from) {
    if (from.empty()) return Span{Span::Kind::partial, 0};

    const auto lead = static_cast<unsigned char>(from.front());

    if (lead < 0x80) return Span{Span::Kind::whole, 1};

    std::size_t much = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;

    if (lead >= 0xC2 && lead <= 0xDF) much = 1;
    else if (lead >= 0xE0 && lead <= 0xEF) much = 2;
    else if (lead >= 0xF0 && lead <= 0xF4) much = 3;
    else return Span{Span::Kind::bad, 1};

    if (lead == 0xE0) low = 0xA0;
    if (lead == 0xED) high = 0x9F;
    if (lead == 0xF0) low = 0x90;
    if (lead == 0xF4) high = 0x8F;

    for (std::size_t at = 1; at <= much; ++at) {
        if (at >= from.size()) return Span{Span::Kind::partial, at};

        const auto next = static_cast<unsigned char>(from[at]);

        if (next < (at == 1 ? low : 0x80) || next > (at == 1 ? high : 0xBF)) return Span{Span::Kind::bad, at};
    }

    return Span{Span::Kind::whole, much + 1};
}

// The character that starts at `at`, which then moves past it; none for a sequence that is not
// well-formed or is cut off, and then `at` moves past its maximal subpart.
std::optional<char32_t> decoded(std::string_view utf8, std::size_t& at) {
    if (at >= utf8.size()) return std::nullopt;

    const Span one = span(utf8.substr(at));

    if (one.kind != Span::Kind::whole) {
        at += one.size;

        return std::nullopt;
    }

    const auto lead = static_cast<unsigned char>(utf8[at]);

    if (one.size == 1) {
        ++at;

        return lead;
    }

    char32_t made = lead & (0x3FU >> (one.size - 1));

    for (std::size_t which = 1; which < one.size; ++which) made = (made << 6U) | (static_cast<unsigned char>(utf8[at + which]) & 0x3FU);

    at += one.size;

    return made;
}

// Appends the UTF-8 form of the character.
void encoded(std::string& into, char32_t point) {
    if (point < 0x80) {
        into += static_cast<char>(point);
    } else if (point < 0x800) {
        into += static_cast<char>(0xC0U | (point >> 6U));
        into += static_cast<char>(0x80U | (point & 0x3FU));
    } else if (point < 0x10000) {
        into += static_cast<char>(0xE0U | (point >> 12U));
        into += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        into += static_cast<char>(0x80U | (point & 0x3FU));
    } else {
        into += static_cast<char>(0xF0U | (point >> 18U));
        into += static_cast<char>(0x80U | ((point >> 12U) & 0x3FU));
        into += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        into += static_cast<char>(0x80U | (point & 0x3FU));
    }
}

// Whether the whole text is well-formed UTF-8, by the rule of `span`.
bool valid(std::string_view utf8) {
    for (std::size_t at = 0; at < utf8.size();) {
        const Span one = span(utf8.substr(at));

        if (one.kind != Span::Kind::whole) return false;

        at += one.size;
    }

    return true;
}

// The text with every sequence that is not well-formed replaced by U+FFFD, one for each maximal
// subpart; well-formed text comes back as it is.
std::string repaired(std::string_view bytes) {
    std::string made;

    made.reserve(bytes.size());

    for (std::size_t at = 0; at < bytes.size();) {
        const std::size_t from = at;

        if (decoded(bytes, at)) {
            made.append(bytes.substr(from, at - from));
        } else {
            encoded(made, 0xFFFD);
        }
    }

    return made;
}

// The system's native wide string turned into UTF-8 (`wide` goes the other way). The pair
// is needed wherever Windows talks to the library: Windows speaks UTF-16, and all the rest of this
// code speaks UTF-8.
//
// It is written with our own decoder, not through Win32, and that is a decision, not a
// convenience: `MultiByteToWideChar` exists on one system only, so code written with it cannot
// be run in trials anywhere else. Here the same path runs on both; the only difference is whether
// `wchar_t` has two bytes (Windows, UTF-16 with surrogate pairs) or four (POSIX, UTF-32).
//
// Known gap: the 16-bit branch runs only on Windows, so trials running on POSIX do not touch it.
// The surrogate pair is checked by computing in the other direction (a character outside the
// Basic Multilingual Plane goes there and back), but the pair assembly itself is seen by no one
// here.
std::string narrow(std::wstring_view from) {
    std::string made;

    made.reserve(from.size());

    for (std::size_t at = 0; at < from.size(); ++at) {
        char32_t one = static_cast<char32_t>(static_cast<std::uint32_t>(from[at]));

        if constexpr (sizeof(wchar_t) == 2) {
            // A high surrogate without a low one is a damaged string: it becomes the replacement
            // character, as in `decoded` for broken UTF-8, instead of a guess.
            if (one >= 0xD800 && one <= 0xDBFF) {
                const char32_t high = one;

                if (at + 1 < from.size()) {
                    const char32_t low =
                        static_cast<char32_t>(static_cast<std::uint32_t>(from[at + 1]));

                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        one = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);

                        ++at;
                    } else {
                        one = 0xFFFD;
                    }
                } else {
                    one = 0xFFFD;
                }
            } else if (one >= 0xDC00 && one <= 0xDFFF) {
                one = 0xFFFD;
            }
        }

        encoded(made, one);
    }

    return made;
}

// The system's native wide string from UTF-8: UTF-16 with surrogate pairs where `wchar_t` has two
// bytes, UTF-32 where it has four. Broken UTF-8 becomes the replacement character.
std::wstring wide(std::string_view from) {
    std::wstring made;

    made.reserve(from.size());

    std::size_t at = 0;

    while (at < from.size()) {
        // `taken` returns zero only when `at` has gone past the string (and the loop
        // condition already guards that) or when the byte is a real zero. A zero in the middle of
        // the string is therefore copied like any other character: `string_view` knows its length
        // and does not end at NUL, and cutting the string there would silently lose the rest.
        const char32_t one = taken(from, at);

        if constexpr (sizeof(wchar_t) == 2) {
            if (one >= 0x10000) {
                const char32_t rest = one - 0x10000;

                made.push_back(static_cast<wchar_t>(0xD800 + (rest >> 10)));
                made.push_back(static_cast<wchar_t>(0xDC00 + (rest & 0x3FF)));

                continue;
            }
        }

        made.push_back(static_cast<wchar_t>(one));
    }

    return made;
}

// A character to show, never to execute. Text that reaches the screen is often foreign (a file
// name, a title from the network, a description from an NFO), and a terminal executes control
// characters: ESC starts a sequence that can rearrange the screen, change the window title and,
// in some terminals, write something to the clipboard or answer on the input as if a person had
// typed it. A file named with an escape sequence would thus be a command to the terminal. So every
// cell gets its character passed through here:
//
// - C0 and DEL become their pictures from the Control Pictures block (␛, ␊, ␡), so it is visible
//   that something is there;
// - C1 (U+0080–U+009F, among them U+009B, the single-character CSI) becomes the replacement
//   character;
// - writing-direction characters (U+200E, U+200F, U+061C, U+202A–U+202E, U+2066–U+2069) and the
//   line and paragraph separators become the replacement character. Reversing the direction turns
//   the name "film", U+202E, "4pm.exe" into "filmexe.mp4" on the screen: an executable poses as a
//   film.
//
// The width stays the same (one cell), so nothing that counts columns gets out of line.
char32_t shown(char32_t one) {
    if (one < 0x20) return 0x2400 + one;

    if (one == 0x7F) return 0x2421;

    if ((one >= 0x80 && one <= 0x9F) || one == 0x061C || one == 0x200E || one == 0x200F ||
        (one >= 0x2028 && one <= 0x202E) || (one >= 0x2066 && one <= 0x2069))
        return 0xFFFD;

    return one;
}

// Characters on a terminal: their width and the clusters they make.
//
// A cluster is what a terminal draws in one place: a letter with the marks that join it (U+0301
// after an "e", the vowel signs of Hangul jamo), a flag of two regional indicators, and, on a
// terminal that joins them, an emoji with its skin tone or a sequence of emoji held together by
// ZWJ (U+200D). The boundaries follow the extended grapheme clusters of UAX #29, with two
// departures for the terminal's sake. Spacing combining marks (Mc) stand alone, because
// terminals give each of them a cell of its own and a cluster would then be wider than the cells
// it takes. And what an emoji sequence is depends on the terminal: one that does not join them
// draws a woman and a laptop where Unicode sees a programmer, so there the skin tone and the
// emoji after a ZWJ start clusters of their own. Prepend characters (a few Indic and Arabic signs)
// are not attached; terminals do not attach them either.
//
// The width of a cluster is the width of its first character (East Asian W and F take two cells,
// the rest one), with three exceptions: a flag takes two cells, a narrow pictograph with the
// emoji selector (U+FE0F) takes two on a terminal that presents it as an emoji, and the joined
// parts of an emoji sequence take nothing beyond the first. Marks with nothing before them (the
// start of a text, a mark after a control) are drawn on a dotted circle, U+25CC, and take a cell;
// zero-width format characters standing alone (ZWSP, word joiner, BOM, a lone ZWJ) show nothing
// and take nothing, as on a terminal.
//
// `Rendering` says how a terminal draws the emoji sequences; the program takes it from the probe
// of its terminal and passes it in. The default is what Unicode says: sequences joined, the
// selector widening.
namespace {

constexpr char32_t zwj = 0x200D;
constexpr char32_t presenting = 0xFE0F;

// The width of a code point on its own, as it is shown: a control that `shown` replaces by a
// visible picture takes its cell, a format character that shows nothing takes none, a character
// that joins the one before it takes none, and the rest take one or two cells.
int looked_up(char32_t one) {
    if (shown(one) != one) return 1;

    if (one < 0x300) return 1;

    if (unicode::control(one) || unicode::zero(one) || one == zwj || unicode::vowel(one) ||
        unicode::trailing(one))
        return 0;

    return unicode::wide(one) ? 2 : 1;
}

// The width of a code point on its own (`looked_up`), remembered for the Basic Multilingual Plane:
// the tables of Unicode are searched once for each character, and the box drawing of every frame
// and the letters of every row are only read after that. The entries are atomic, since the pool
// measures text too; two threads meeting on a new character both look it up and store the same.
// An entry holds the width plus one, so zero, as the array starts, means not yet known.
int alone(char32_t one) {
    if (one >= 0x10000) return looked_up(one);

    static std::array<std::atomic<std::uint8_t>, 0x10000> known{};

    std::atomic<std::uint8_t>& entry = known[one];

    if (const std::uint8_t held = entry.load(std::memory_order_relaxed); held != 0) return held - 1;

    const int made = looked_up(one);

    entry.store(static_cast<std::uint8_t>(made + 1), std::memory_order_relaxed);

    return made;
}

// Whether the character joins the one before it on this terminal. `pictorial` says whether the
// cluster began with a pictograph, for the emoji after a ZWJ; `before` is the character before.
bool joining(char32_t before, char32_t one, bool pictorial, Rendering how) {
    if (unicode::control(one)) return false;

    if (unicode::modifier(one)) return how.joined;

    if (unicode::zero(one) || one == zwj) return true;

    if (before == zwj) return how.joined && pictorial && unicode::pictograph(one);

    if (unicode::leading(before)) return unicode::leading(one) || unicode::vowel(one) || unicode::syllable(one);

    if (unicode::vowel(before) || unicode::open(before)) return unicode::vowel(one) || unicode::trailing(one);

    if (unicode::trailing(before) || unicode::syllable(before)) return unicode::trailing(one);

    return false;
}

// The most characters a cluster holds after its first (UAX #15, Stream-Safe Text Format).
constexpr int stream_safe = 30;

// The end of the cluster that starts at `at`, for a given rendering. A cluster holds at most 30
// characters after its first, the limit of the Stream-Safe Text Format (UAX #15): a letter followed
// by a million combining marks would otherwise make one cell of a megabyte, read whole to be drawn
// and taken whole by Backspace. The marks past the limit start a cluster of their own, as a mark
// with nothing before it does.
std::size_t ended(std::string_view text, std::size_t at, Rendering how) {
    if (at >= text.size()) return text.size();

    // Plain ASCII followed by ASCII: nothing to join, the common case by far.
    if (static_cast<unsigned char>(text[at]) < 0x80 && (at + 1 == text.size() || static_cast<unsigned char>(text[at + 1]) < 0x80))
        return at + 1;

    std::size_t next = at;

    const char32_t first = taken(text, next);

    if (unicode::control(first)) return next;

    const bool pictorial = unicode::pictograph(first);

    char32_t before = first;
    int flags = unicode::regional(first) ? 1 : 0;
    int joined = 0;

    while (next < text.size() && joined < stream_safe) {
        std::size_t after = next;

        const char32_t one = taken(text, after);

        if (unicode::regional(before) && unicode::regional(one)) {
            if (flags % 2 == 0) break;

            ++flags;
        } else if (!joining(before, one, pictorial, how)) {
            break;
        }

        before = one;
        next = after;

        ++joined;
    }

    return next;
}

// The cells a cluster takes, for a given rendering.
int cells(std::string_view cluster, Rendering how) {
    std::size_t at = 0;

    const char32_t first = taken(cluster, at);

    // A skin tone standing alone shows its colour, two cells wide.
    if (unicode::modifier(first)) return 2;

    const int own = alone(first);

    // A mark with nothing before it goes on a dotted circle; a format character alone shows
    // nothing.
    if (own == 0) return unicode::mark(first) ? 1 : 0;

    if (at == cluster.size()) return own;

    int made = own;
    int flags = unicode::regional(first) ? 1 : 0;

    while (at < cluster.size()) {
        const char32_t one = taken(cluster, at);

        if (unicode::regional(one)) ++flags;

        if (one == presenting && how.presented && unicode::pictograph(first)) made = 2;
    }

    if (flags == 2) made = 2;

    return made;
}

}

// The width of a code point standing alone: a character that joins the one before it takes
// nothing, a skin tone takes nothing on a terminal that joins it to its emoji and two cells on one
// that does not, a control takes the cell of its picture (`shown`).
int width(char32_t one, Rendering how) {
    if (unicode::modifier(one)) return how.joined ? 0 : 2;

    return alone(one);
}

// How many cells the text takes, cluster by cluster. Text of ASCII alone, the common case, takes a
// cell for every byte: nothing in it joins, and a control character is shown as its picture
// (`shown`), one cell wide too.
int width(std::string_view utf8, Rendering how) {
    if (std::all_of(utf8.begin(), utf8.end(), [](char one) { return static_cast<unsigned char>(one) < 0x80; }))
        return static_cast<int>(utf8.size());

    int made = 0;

    for (std::size_t at = 0; at < utf8.size();) {
        const std::size_t end = ended(utf8, at, how);

        made += cells(utf8.substr(at, end - at), how);
        at = end;
    }

    return made;
}

// The end of the cluster that starts at `at`: where the cursor goes to the right.
std::size_t after(std::string_view utf8, std::size_t at, Rendering how) { return ended(utf8, at, how); }

// The start of the cluster that ends at `at` (or holds the byte before it): where the cursor goes
// to the left, and what Backspace takes. It walks back to a character that surely starts a cluster
// (not a joining one, not after a ZWJ, not inside a run of flags or of Hangul jamo) and comes
// forward from there. A pathological run of thousands of marks stops the walk after 512 characters,
// and the answer is then counted from there.
std::size_t before(std::string_view text, std::size_t at, Rendering how) {
    at = std::min(at, text.size());

    if (at == 0) return 0;

    // The start of the code point that ends at `end`.
    const auto stepped = [&text](std::size_t end) {
        std::size_t made = end - 1;

        while (made > 0 && end - made < 4 && (static_cast<unsigned char>(text[made]) & 0xC0) == 0x80) --made;

        return made;
    };

    std::size_t from = stepped(at);

    for (int steps = 0; from > 0 && steps < 512; ++steps) {
        std::size_t at_one = from;

        const char32_t one = taken(text, at_one);

        std::size_t at_before = stepped(from);

        const char32_t before = taken(text, at_before);

        const bool tied = joining(before, one, unicode::pictograph(before) || before == zwj, how) ||
                          (unicode::regional(before) && unicode::regional(one)) || before == zwj;

        if (!tied) break;

        from = stepped(from);
    }

    for (;;) {
        const std::size_t end = ended(text, from, how);

        if (end >= at) return from;

        from = end;
    }
}

// The clusters of the text, in order.
std::vector<std::string_view> clusters(std::string_view utf8, Rendering how) {
    std::vector<std::string_view> made;

    for (std::size_t at = 0; at < utf8.size();) {
        const std::size_t end = ended(utf8, at, how);

        made.push_back(utf8.substr(at, end - at));
        at = end;
    }

    return made;
}

// The text in lines of at most `columns` cells, broken after spaces where it can be and inside a
// word only when the word alone is wider than a line; a line break in the text ends a line too.
// The spaces at a break stay at the end of the line before it, which may then be wider than
// `columns` by those spaces, since they show nothing there.
std::vector<std::string_view> wrapped(std::string_view utf8, int columns, Rendering how) {
    std::vector<std::string_view> lines;

    columns = std::max(columns, 1);

    std::size_t start = 0;

    while (start <= utf8.size()) {
        std::size_t at = start;
        std::size_t broken = std::string_view::npos;
        int used = 0;
        bool ended_line = false;

        while (at < utf8.size()) {
            if (utf8[at] == '\n') {
                lines.push_back(utf8.substr(start, at - start));
                start = at + 1;
                ended_line = true;

                break;
            }

            const std::size_t end = ended(utf8, at, how);
            const int wide = cells(utf8.substr(at, end - at), how);

            if (utf8[at] == ' ') {
                broken = end;
            } else if (used + wide > columns && used > 0) {
                const std::size_t cut = broken != std::string_view::npos ? broken : at;

                lines.push_back(utf8.substr(start, cut - start));
                start = cut;
                ended_line = true;

                break;
            }

            used += wide;
            at = end;
        }

        if (ended_line) continue;

        lines.push_back(utf8.substr(start));

        break;
    }

    return lines;
}

// Whether a character belongs to a word. This is an approximation and is meant to stay one:
// everything outside ASCII counts as a letter, because a real answer would need Unicode tables,
// and talcite has none. As a result "ą" is a letter, but so are a Chinese bracket and a Cyrillic
// punctuation mark, and that is a price paid on purpose.
//
// The rule is also written so that a widened byte of a UTF-8 sequence gives the same answer as
// the character it comes from: both fall under "not ASCII, so a letter". That makes it usable
// both where text is walked by characters and where it is walked by bytes.
//
// It lives here and not in `Comb`, because whole-word search, double-click word selection and a
// future jump by words must all mean the same thing.
bool in_word(char32_t one) {
    if (one >= 0x80) return true;

    const char sign = static_cast<char>(one);

    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') ||
           (sign >= 'A' && sign <= 'Z') || sign == '_';
}

// The simple case folding of Unicode (CaseFolding, statuses C and S): the form in which two texts
// that differ only in letter case are the same.
char32_t folded(char32_t one) { return mapped(folds, one); }

// The text with every character folded (`folded`); broken UTF-8 stays as it is.
std::string folded(std::string_view utf8) {
    std::string made;

    made.reserve(utf8.size());

    for (std::size_t at = 0; at < utf8.size();) {
        const std::size_t from = at;

        if (const auto one = decoded(utf8, at)) {
            encoded(made, folded(*one));
        } else {
            made.append(utf8.substr(from, at - from));
        }
    }

    return made;
}

// The small letter of a character by the simple mappings of Unicode.
char32_t lowered(char32_t one) { return mapped(lowers, one); }

// The text in small letters (`lowered` of each character); broken UTF-8 stays as it is. For ASCII
// this is what the C library's tolower gives in the C locale, whatever locale the program has.
std::string lowered(std::string_view utf8) {
    std::string made;

    made.reserve(utf8.size());

    for (std::size_t at = 0; at < utf8.size();) {
        const std::size_t from = at;

        if (const auto one = decoded(utf8, at)) {
            encoded(made, lowered(*one));
        } else {
            made.append(utf8.substr(from, at - from));
        }
    }

    return made;
}

namespace {

// Whether the byte is white space at the end of a text: a space, a tab or a line end.
bool blank(char one) { return one == ' ' || one == '\t' || one == '\r' || one == '\n'; }

}

// The text without spaces, tabs and line ends at its ends; everything in the middle stays.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);

    return text;
}

}

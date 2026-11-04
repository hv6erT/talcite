#include "talcite/encoding.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "unicode.hpp"

namespace talcite {

namespace utf8 {

// How many continuation bytes this lead byte announces. Zero means "either a plain ASCII
// character or a byte that starts nothing"; the caller settles that itself, because the input
// decoder rejects garbage differently from writing to the screen.
std::size_t trailing(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 1;
    if ((lead & 0xF0) == 0xE0) return 2;
    if ((lead & 0xF8) == 0xF0) return 3;

    return 0;
}

// The next character of the string. A byte that breaks the rules and a tail cut off halfway come
// back as the replacement character, and decoding moves on by one byte: a string with a broken
// middle has to be displayed, not vanish. Where a cut-off tail has to wait for the rest instead
// of vanishing, that is in the input decoder, this function is not used. Breaking the rules
// includes the forms UTF-8 forbids although their bits fit: a character written longer than it
// needs ("/" as C0 AF), half of a UTF-16 surrogate pair, and anything past U+10FFFF. Let through,
// they would pass for another character (a hidden "/"), or for a number the screen uses for
// something else (past U+10FFFF lie the handles of grapheme clusters).
char32_t decode(std::string_view from, std::size_t& at) {
    if (at >= from.size()) return 0;

    const unsigned char one = static_cast<unsigned char>(from[at]);

    if (one < 0x80) {
        ++at;

        return one;
    }

    const std::size_t much = utf8::trailing(one);

    if (much == 0 || at + much >= from.size()) {
        ++at;

        return 0xFFFD;
    }

    char32_t made = one & (0x3F >> much);

    for (std::size_t which = 1; which <= much; ++which) {
        const unsigned char next = static_cast<unsigned char>(from[at + which]);

        if ((next & 0xC0) != 0x80) {
            ++at;

            return 0xFFFD;
        }

        made = (made << 6) | (next & 0x3F);
    }

    // The least value each length may carry: anything less has a shorter form.
    constexpr char32_t least[] = {0, 0x80, 0x800, 0x10000};

    if (made < least[much] || made > 0x10FFFF || (made >= 0xD800 && made <= 0xDFFF)) {
        ++at;

        return 0xFFFD;
    }

    at += much + 1;

    return made;
}

// Appends the UTF-8 form of the character to `into`.
void encode(char32_t one, std::string& into) {
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

// The UTF-8 form of a single character.
std::string encode(char32_t one) {
    std::string made;

    utf8::encode(one, made);

    return made;
}

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
// `Rendering` says how this terminal draws the emoji sequences; `Term` sets it from the probe's
// measurements. Without a terminal (trials, output into a pipe), the default is what Unicode
// says: sequences joined, the selector widening.
namespace {

constexpr char32_t zwj = 0x200D;
constexpr char32_t presenting = 0xFE0F;
constexpr char32_t dotted = 0x25CC;

// Handles of clusters start above the last code point, so a cell tells them apart at a glance.
constexpr char32_t handles = 0x110000;

// Distinct clusters a process remembers. Past this, a new cluster shows as its first character
// alone: text that is foreign and hostile (file names made of thousands of distinct combinations)
// must not make the table grow without end.
constexpr std::size_t most_glyphs = 65536;

std::atomic<unsigned> drawing{3};

// One remembered cluster: what is written to the terminal for it, and how many cells it takes.
struct Remembered {
    std::string text;
    int wide = 1;
};

// The clusters met so far. Entries are never removed, so a handle stays valid for the life of the
// process and two frames with the same cluster have the same handle. It is created once and never
// destroyed: static objects that draw while the process ends must still find it.
struct Remembering {
    std::shared_mutex guard;
    std::deque<Remembered> all;
    std::unordered_map<std::string, char32_t> known;
};

// The one table of the process.
Remembering& remembering() {
    static Remembering* made = new Remembering;

    return *made;
}

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

    const char32_t first = utf8::decode(text, next);

    if (unicode::control(first)) return next;

    const bool pictorial = unicode::pictograph(first);

    char32_t before = first;
    int flags = unicode::regional(first) ? 1 : 0;
    int joined = 0;

    while (next < text.size() && joined < stream_safe) {
        std::size_t after = next;

        const char32_t one = utf8::decode(text, after);

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

    const char32_t first = utf8::decode(cluster, at);

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
        const char32_t one = utf8::decode(cluster, at);

        if (unicode::regional(one)) ++flags;

        if (one == presenting && how.presented && unicode::pictograph(first)) made = 2;
    }

    if (flags == 2) made = 2;

    return made;
}

// What is written to the terminal for a cluster: its characters passed through `shown`, after a
// dotted circle when it starts with a mark.
std::string written(std::string_view cluster) {
    std::string made;

    std::size_t at = 0;

    const char32_t first = utf8::decode(cluster, at);

    if (!unicode::modifier(first) && alone(first) == 0) utf8::encode(dotted, made);

    at = 0;

    while (at < cluster.size()) utf8::encode(shown(utf8::decode(cluster, at)), made);

    return made;
}

// The rendering packed into the bits of the setting and of a remembered cluster's key.
unsigned packed(Rendering how) { return (how.joined ? 1u : 0u) | (how.presented ? 2u : 0u); }

}

// Sets how this terminal draws emoji sequences; `Term` does it once, from the probe.
void rendered(Rendering how) { drawing.store(packed(how)); }

// How this terminal draws emoji sequences.
Rendering rendering() {
    const unsigned bits = drawing.load();

    return Rendering{(bits & 1u) != 0, (bits & 2u) != 0};
}

// The width of one cell's content in cells: of a code point standing alone, or of a remembered
// cluster. A character that joins the one before it takes nothing; a skin tone takes nothing when
// the terminal joins it to its emoji and two cells when it does not.
int width(char32_t one) {
    if (one >= handles) {
        Remembering& known = remembering();

        const std::shared_lock reading{known.guard};

        const std::size_t which = one - handles;

        return which < known.all.size() ? known.all[which].wide : 1;
    }

    if (unicode::modifier(one)) return rendering().joined ? 0 : 2;

    return alone(one);
}

// How many cells the text takes, cluster by cluster. Text of ASCII alone, the common case, takes a
// cell for every byte: nothing in it joins, and a control character is shown as its picture
// (`shown`), one cell wide too.
int width(std::string_view text) {
    if (std::all_of(text.begin(), text.end(), [](char one) { return static_cast<unsigned char>(one) < 0x80; }))
        return static_cast<int>(text.size());

    const Rendering how = rendering();

    int made = 0;

    for (std::size_t at = 0; at < text.size();) {
        const std::size_t next = ended(text, at, how);

        if (next == at + 1 && static_cast<unsigned char>(text[at]) < 0x80) made += alone(static_cast<unsigned char>(text[at]));
        else made += cells(text.substr(at, next - at), how);

        at = next;
    }

    return made;
}

// The end of the cluster that starts at `at`.
std::size_t cluster_after(std::string_view text, std::size_t at) { return ended(text, at, rendering()); }

// The start of the cluster that ends at `at` (or holds the byte before it): where the cursor goes
// to the left, and what Backspace takes. It walks back to a character that surely starts a cluster
// (not a joining one, not after a ZWJ, not inside a run of flags or of Hangul jamo) and comes forward
// from there. A pathological run of thousands of marks stops the walk after 512 characters, and the
// answer is then counted from there.
std::size_t cluster_before(std::string_view text, std::size_t at) {
    at = std::min(at, text.size());

    if (at == 0) return 0;

    const Rendering how = rendering();

    // The start of the code point that ends at `end`.
    const auto stepped = [&text](std::size_t end) {
        std::size_t made = end - 1;

        while (made > 0 && end - made < 4 && (static_cast<unsigned char>(text[made]) & 0xC0) == 0x80) --made;

        return made;
    };

    std::size_t from = stepped(at);

    for (int steps = 0; from > 0 && steps < 512; ++steps) {
        std::size_t at_one = from;

        const char32_t one = utf8::decode(text, at_one);

        std::size_t at_before = stepped(from);

        const char32_t before = utf8::decode(text, at_before);

        const bool tied = joining(before, one, unicode::pictograph(before) || before == zwj, how) ||
                          (unicode::regional(before) && unicode::regional(one)) || before == zwj;

        if (!tied) break;

        from = stepped(from);
    }

    for (;;) {
        const std::size_t next = ended(text, from, how);

        if (next >= at) return from;

        from = next;
    }
}

// The content of one cell for a cluster: a code point standing alone is itself (passed through
// `shown`), a longer cluster gets the handle under which it is remembered, and a cluster that
// shows nothing gives zero.
char32_t glyph(std::string_view cluster) {
    if (cluster.empty()) return 0;

    const Rendering how = rendering();

    std::size_t at = 0;

    const char32_t first = utf8::decode(cluster, at);

    if (at == cluster.size() && alone(first) > 0) return shown(first);

    Remembered made{written(cluster), cells(cluster, how)};

    if (made.wide == 0) return 0;

    std::string key(1, static_cast<char>('0' + packed(how)));

    key += made.text;

    Remembering& known = remembering();

    {
        const std::shared_lock reading{known.guard};

        if (const auto found = known.known.find(key); found != known.known.end()) return found->second;
    }

    const std::unique_lock writing{known.guard};

    if (const auto found = known.known.find(key); found != known.known.end()) return found->second;

    if (known.all.size() >= most_glyphs) return alone(first) > 0 ? shown(first) : dotted;

    const auto handle = static_cast<char32_t>(handles + known.all.size());

    known.all.push_back(std::move(made));
    known.known.emplace(std::move(key), handle);

    return handle;
}

// What is written to the terminal for one cell's content: the remembered text of a cluster, the
// UTF-8 of a code point, and nothing for an empty cell's zero.
std::string spelled(char32_t glyph) {
    if (glyph == 0) return {};

    if (glyph >= handles) {
        Remembering& known = remembering();

        const std::shared_lock reading{known.guard};

        const std::size_t which = glyph - handles;

        return which < known.all.size() ? known.all[which].text : std::string{};
    }

    return utf8::encode(glyph);
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


// The system's native wide string turned into UTF-8 (`to_wstring` goes the other way). The pair
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
std::string to_string(std::wstring_view from) {
    std::string made;

    made.reserve(from.size());

    for (std::size_t at = 0; at < from.size(); ++at) {
        char32_t one = static_cast<char32_t>(static_cast<std::uint32_t>(from[at]));

        if constexpr (sizeof(wchar_t) == 2) {
            // A high surrogate without a low one is a damaged string: it becomes the replacement
            // character, as in `utf8::decode` for broken UTF-8, instead of a guess.
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

        utf8::encode(one, made);
    }

    return made;
}

// The system's native wide string from UTF-8: UTF-16 with surrogate pairs where `wchar_t` has two
// bytes, UTF-32 where it has four. Broken UTF-8 becomes the replacement character.
std::wstring to_wstring(std::string_view from) {
    std::wstring made;

    made.reserve(from.size());

    std::size_t at = 0;

    while (at < from.size()) {
        // `utf8::decode` returns zero only when `at` has gone past the string (and the loop
        // condition already guards that) or when the byte is a real zero. A zero in the middle of
        // the string is therefore copied like any other character: `string_view` knows its length
        // and does not end at NUL, and cutting the string there would silently lose the rest.
        const char32_t one = utf8::decode(from, at);

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

}

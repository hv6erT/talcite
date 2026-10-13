#!/usr/bin/env python3
# Generates talcite/src/unicode.cpp from the Unicode data files (GraphemeBreakProperty.txt,
# emoji-data.txt, EastAsianWidth.txt and UnicodeData.txt from https://www.unicode.org/Public/).
# Usage: python3 scripts/unicode.py DATA_DIR talcite/src/unicode.cpp
import sys, re
D, OUT = sys.argv[1], sys.argv[2]

def ranges_of(path, wanted):
    got = {}
    for line in open(f"{D}/{path}", encoding="utf-8"):
        line = line.split('#', 1)[0].strip()
        if not line: continue
        cps, prop = [x.strip() for x in line.split(';')[:2]]
        if prop not in wanted: continue
        a, _, b = cps.partition('..')
        a = int(a, 16); b = int(b, 16) if b else a
        got.setdefault(prop, set()).update(range(a, b + 1))
    return got

cat = {}
lines = open(f"{D}/UnicodeData.txt", encoding="utf-8").read().splitlines()
i = 0
while i < len(lines):
    f = lines[i].split(';'); cp = int(f[0], 16)
    if f[1].endswith('First>'):
        g = lines[i + 1].split(';'); last = int(g[0], 16)
        for c in range(cp, last + 1): cat[c] = f[2]
        i += 2; continue
    cat[cp] = f[2]; i += 1

gcb = ranges_of("GraphemeBreakProperty.txt", {"Extend", "Control", "CR", "LF", "L", "V", "T", "ZWJ", "Regional_Indicator", "SpacingMark", "Prepend"})
emo = ranges_of("emoji-data.txt", {"Extended_Pictographic"})
eaw = ranges_of("EastAsianWidth.txt", {"W", "F"})

wide = eaw["W"] | eaw["F"]
extend = gcb["Extend"]
zero = {c for c in extend if cat.get(c) != "Mc"}
marks = {c for c in extend if cat.get(c) in ("Mn", "Me")}
control = gcb["Control"] | gcb["CR"] | gcb["LF"]
leading = gcb["L"]; vowel = gcb["V"]; trailing = gcb["T"]
pictographic = emo["Extended_Pictographic"]

def spans(s):
    out = []
    for c in sorted(s):
        if out and out[-1][1] == c - 1: out[-1][1] = c
        else: out.append([c, c])
    return out

def table(name, s):
    sp = spans(s)
    body = []
    row = []
    for a, b in sp:
        row.append(f"{{0x{a:X}, 0x{b:X}}}")
        if len(row) == 4:
            body.append("    " + ", ".join(row) + ",")
            row = []
    if row: body.append("    " + ", ".join(row) + ",")
    return f"constexpr std::array<Span, {len(sp)}> {name}{{{{\n" + "\n".join(body) + "\n}};\n", len(sp)

parts = []
counts = {}
for name, s in [("wides", wide), ("zeros", zero), ("marks", marks), ("controls", control), ("leadings", leading), ("vowels", vowel), ("trailings", trailing), ("pictographs", pictographic)]:
    t, n = table(name, s); parts.append(t); counts[name] = n

out = f'''#include "unicode.hpp"

#include <algorithm>
#include <array>
#include <iterator>

// The properties of characters that drawing on a terminal needs: which ones take two cells, which
// ones join the character before them, which ones are controls, the Hangul jamo and the emoji.
// The tables are generated from the Unicode 15.1 data files (EastAsianWidth, GraphemeBreakProperty,
// emoji-data and UnicodeData) and are not edited by hand; a new version of Unicode means
// generating them again.
//
// `wides` are the characters of East Asian width W and F. `zeros` are the grapheme extenders that
// take no cell of their own: everything of Grapheme_Cluster_Break=Extend except the spacing
// combining marks (Mc), which terminals draw in a cell of their own and which therefore stand
// alone here too. `marks` are the nonspacing and enclosing marks among them (Mn, Me): those are
// drawn on a dotted circle when nothing stands before them. `controls` are Control, CR and LF.
// `leadings`, `vowels` and `trailings` are the Hangul jamo L, V and T; the precomposed syllables
// LV and LVT are recognized by arithmetic. `pictographs` are the characters with
// Extended_Pictographic: the emoji, and the places reserved for future ones.
namespace talcite::unicode {{

namespace {{

struct Span {{
    char32_t from;
    char32_t upto;
}};

{chr(10).join(parts)}
// Whether the character lies in one of the sorted, disjoint spans.
template <std::size_t many>
bool among(const std::array<Span, many>& spans, char32_t one) {{
    const auto found = std::upper_bound(spans.begin(), spans.end(), one,
                                        [](char32_t value, const Span& span) {{ return value < span.from; }});

    return found != spans.begin() && one <= std::prev(found)->upto;
}}

}}

// Whether the character takes two cells.
bool wide(char32_t one) {{ return one >= 0x1100 && among(wides, one); }}

// Whether the character joins the one before it and takes no cell.
bool zero(char32_t one) {{ return one >= 0x300 && among(zeros, one); }}

// Whether the character is a nonspacing or enclosing mark.
bool mark(char32_t one) {{ return one >= 0x300 && among(marks, one); }}

// Whether the character is a control: it stands alone and joins nothing.
bool control(char32_t one) {{ return among(controls, one); }}

// Whether the character is a leading Hangul jamo (L).
bool leading(char32_t one) {{ return among(leadings, one); }}

// Whether the character is a Hangul vowel jamo (V).
bool vowel(char32_t one) {{ return among(vowels, one); }}

// Whether the character is a trailing Hangul jamo (T).
bool trailing(char32_t one) {{ return among(trailings, one); }}

// Whether the character is a precomposed Hangul syllable (LV or LVT).
bool syllable(char32_t one) {{ return one >= 0xAC00 && one <= 0xD7A3; }}

// Whether the precomposed syllable has no trailing consonant yet (LV), so that one can follow.
bool open(char32_t one) {{ return syllable(one) && (one - 0xAC00) % 28 == 0; }}

// Whether the character is Extended_Pictographic.
bool pictograph(char32_t one) {{ return one >= 0xA9 && among(pictographs, one); }}

// Whether the character is a regional indicator, half of a flag.
bool regional(char32_t one) {{ return one >= 0x1F1E6 && one <= 0x1F1FF; }}

// Whether the character is an emoji modifier, a skin tone.
bool modifier(char32_t one) {{ return one >= 0x1F3FB && one <= 0x1F3FF; }}

}}
'''
open(OUT, "w").write(out)
print(counts)

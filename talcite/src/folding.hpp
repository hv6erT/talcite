#pragma once

#include <algorithm>
#include <array>

namespace talcite::ui {

// Letters are compared case-insensitively using Unicode simple case folding (CaseFolding.txt,
// statuses C and S) for Basic Latin, Latin-1, Latin Extended-A, Greek, and Cyrillic together with
// its supplement. Outside these blocks a letter stays as it is.
//
// The blocks are chosen, not arbitrary. The language files of twa have shortcuts with Czech,
// Polish, German, French and Cyrillic letters ("Alt+Л" in Russian and Ukrainian), and the menus in
// those languages underline letters from the same alphabets. The full Unicode tables are several
// hundred ranges that nobody here will press.
//
// Simple folding, not full: a letter maps to a single letter. "ß" stays "ß", "İ" stays "İ" (its
// full folding is two characters), "ς" goes to "σ", and "ſ" to "s", as the table says.
//
// It is defined once and used in two places: matching a key against a set, and finding the letter
// to underline in a label.

struct Fold {
    char32_t first;
    char32_t last;
    int shift;
    bool alternate;  // every other one, starting at `first`: upper, lower, upper...
};

inline constexpr std::array<Fold, 42> folds{{
    {0x0041, 0x005A, 32, false},  {0x00B5, 0x00B5, 775, false}, {0x00C0, 0x00D6, 32, false},
    {0x00D8, 0x00DE, 32, false},  {0x0100, 0x012E, 1, true},    {0x0132, 0x0136, 1, true},
    {0x0139, 0x0147, 1, true},    {0x014A, 0x0176, 1, true},    {0x0178, 0x0178, -121, false},
    {0x0179, 0x017D, 1, true},    {0x017F, 0x017F, -268, false}, {0x0370, 0x0372, 1, true},
    {0x0376, 0x0376, 1, false},   {0x037F, 0x037F, 116, false}, {0x0386, 0x0386, 38, false},
    {0x0388, 0x038A, 37, false},  {0x038C, 0x038C, 64, false},  {0x038E, 0x038F, 63, false},
    {0x0391, 0x03A1, 32, false},  {0x03A3, 0x03AB, 32, false},  {0x03C2, 0x03C2, 1, false},
    {0x03CF, 0x03CF, 8, false},   {0x03D0, 0x03D0, -30, false}, {0x03D1, 0x03D1, -25, false},
    {0x03D5, 0x03D5, -15, false}, {0x03D6, 0x03D6, -22, false}, {0x03D8, 0x03EE, 1, true},
    {0x03F0, 0x03F0, -54, false}, {0x03F1, 0x03F1, -48, false}, {0x03F4, 0x03F4, -60, false},
    {0x03F5, 0x03F5, -64, false}, {0x03F7, 0x03F7, 1, false},   {0x03F9, 0x03F9, -7, false},
    {0x03FA, 0x03FA, 1, false},   {0x03FD, 0x03FF, -130, false}, {0x0400, 0x040F, 80, false},
    {0x0410, 0x042F, 32, false},  {0x0460, 0x0480, 1, true},    {0x048A, 0x04BE, 1, true},
    {0x04C0, 0x04C0, 15, false},  {0x04C1, 0x04CD, 1, true},    {0x04D0, 0x052E, 1, true},
}};

// The simple case folding of one character: finds the range in `folds` that holds it and applies
// its shift; a character outside every range, or a lowercase one in an alternating range, is
// returned unchanged.
inline char32_t folded(char32_t one) {
    const auto after = std::upper_bound(folds.begin(), folds.end(), one,
                                        [](char32_t which, const Fold& fold) { return which < fold.first; });

    if (after == folds.begin()) return one;

    const Fold& fold = *(after - 1);

    if (one > fold.last || (fold.alternate && (one - fold.first) % 2 != 0)) return one;

    return static_cast<char32_t>(static_cast<int>(one) + fold.shift);
}

}

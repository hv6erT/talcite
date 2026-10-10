#include "talcite/keyednode.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "talcite/encoding.hpp"

#include "folding.hpp"

namespace talcite::ui {

// Adds a key to the set. An empty key is not added, just as an empty node is not added to the
// children: a set holding "nothing" would answer to nothing.
//
// Adding the same key twice is not an error, only a no-op: the second copy could never win,
// because the first one ends the search. So it is silently skipped instead of throwing.
void Keys::add(Key one) {
    if (one.empty() || contains(one)) return;

    inside_.push_back(one);
}

// Adds several keys, one by one, with the same rules as a single `add`.
void Keys::add(std::vector<Key> many) {
    for (const Key one : many) add(one);
}

// Removes the key that matches the given one under `alike`; does nothing when there is none.
void Keys::remove(Key one) {
    const auto at = std::find_if(inside_.begin(), inside_.end(),
                                 [one](Key mine) { return alike(mine, one); });

    if (at != inside_.end()) inside_.erase(at);
}

// Empties the set, so it answers to no key.
void Keys::clear() { inside_.clear(); }

// Whether the set answers to the given key. Matching is `alike`; the rule lives next to the key,
// in `key.cpp`, because programs that compare shortcuts themselves, for example ones read from a
// file, need the same rule.
bool Keys::contains(Key one) const {
    if (one.empty()) return false;

    return std::any_of(inside_.begin(), inside_.end(),
                       [one](Key mine) { return alike(mine, one); });
}

// The key at the given position. Reaching past the end is the caller's error, not a state to pass
// over in silence, just as in the set of options, so it throws std::out_of_range. The key is
// returned by value because it is a single number: a reference would carry as much and would tie
// the caller to the layout of the set.
Key Keys::operator[](std::size_t which) const {
    if (which >= inside_.size()) throw std::out_of_range("talcite::ui::Keys: no such key");

    return inside_[which];
}

// The first key of the set; throws std::out_of_range when the set is empty.
Key Keys::front() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Keys: no keys");

    return inside_.front();
}

// The last key of the set; throws std::out_of_range when the set is empty.
Key Keys::back() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Keys: no keys");

    return inside_.back();
}

// The column of the letter to underline, or -1 when none of the keys occurs in the text. This is
// not parsing of the text: the text is plain and drawn in full, and we only look in it for the
// letter that would have to be found anyway in order to underline it.
//
// The first key that occurs in the text decides, at its first occurrence. With one key, which is
// the usual case, there is nothing to decide.
//
// The column is counted in cells, not bytes, and on the text already trimmed: a letter that is not
// visible after trimming has nothing to underline, and a column counted on the whole text would
// land on the ellipsis or on a bracket.
//
// This is a free function and not part of the template body so that case folding stays in src/:
// `KeyedNode` is a template, so its members have to live in the header, and that header is
// public.
int shortcut_column(const Keys& these, const std::string& shown) {
    for (const Key one : these) {
        const char32_t which = one.letter();

        if (which == 0) continue;

        std::size_t at = 0;
        int wide = 0;

        while (at < shown.size()) {
            const std::size_t began = at;
            const char32_t sign = utf8::decode(shown, at);

            if (began == at) break;

            if (folded(sign) == folded(which)) return wide;

            wide += width(sign);
        }
    }

    return -1;
}

}

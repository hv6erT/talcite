#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "tal/diff.hpp"

namespace {

// The lines of `text` as `compared` reads them: up to each '\n', without a '\r' before it, and no
// empty line after the last '\n'.
std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> made;

    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        made.emplace_back(line);

        if (end == std::string_view::npos) break;

        text.remove_prefix(end + 1);
    }

    return made;
}

// Stops the run unless the marks of `side` are whole, in order, apart, inside its text and of
// `part`; gives the text without them.
std::string kept(const tal::Side& side, std::string_view part) {
    std::string made;
    std::size_t at = 0;

    for (const tal::Mark& mark : side.marks) {
        if (mark.from < at || mark.from >= mark.upto || mark.upto > side.text.size() || mark.part != part)
            std::terminate();

        if (at > 0 && mark.from == at) std::terminate();

        made += side.text.substr(at, mark.from - at);
        at = mark.upto;
    }

    return made + side.text.substr(at);
}

// Stops the run unless the two sides of a row keep the same text outside their marks.
void paired(const tal::Side& left, const tal::Side& right) {
    if (kept(left, "gone") != kept(right, "came")) std::terminate();
}


// Stops the run unless a side standing alone is marked whole as `part`, when it has any text.
void alone(const tal::Side& side, std::string_view part) {
    if (side.text.empty() ? !side.marks.empty() : side.marks != std::vector<tal::Mark>{{0, side.text.size(), std::string{part}, {}}})
        std::terminate();
}

}

// One input of libFuzzer as two texts, split at its first byte 0, with the context in the byte
// before them. The words of each two lines keep the same text outside their marks, a line alone is
// marked whole; the hunks
// number their lines one after another and, with the equal lines between them, give both texts
// back; equal texts give no hunks. Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;

    const int context = data[0] % 5;
    const std::string_view text{reinterpret_cast<const char*>(data) + 1, size - 1};
    const std::size_t split = text.find('\0');
    const std::string_view left = text.substr(0, split);
    const std::string_view right = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);

    const auto [one, two] = tal::diffed(left, right);

    if (one.text != left || two.text != right) std::terminate();

    paired(one, two);

    const std::vector<tal::Hunk> made = tal::compared(left, right, context);
    const std::vector<std::string> ones = lines(left);
    const std::vector<std::string> twos = lines(right);

    if (made.empty() != (ones == twos)) std::terminate();

    std::size_t at_one = 0;
    std::size_t at_two = 0;

    for (const tal::Hunk& hunk : made) {
        if (hunk.rows.empty()) std::terminate();

        const tal::Pair& first = hunk.rows.front();

        if (first.left && *first.left->number <= at_one) std::terminate();
        if (first.right && *first.right->number <= at_two) std::terminate();

        const std::size_t gap = first.left ? *first.left->number - 1 - at_one : *first.right->number - 1 - at_two;

        for (std::size_t step = 0; step < gap; ++step, ++at_one, ++at_two)
            if (ones.at(at_one) != twos.at(at_two)) std::terminate();

        for (const tal::Pair& row : hunk.rows) {
            if (!row.left && !row.right) std::terminate();

            if (row.left && (row.left->number != at_one + 1 || ones.at(at_one) != row.left->text)) std::terminate();

            if (row.right && (row.right->number != at_two + 1 || twos.at(at_two) != row.right->text)) std::terminate();

            if (row.left && row.right) paired(*row.left, *row.right);
            else if (row.left) alone(*row.left, "gone");
            else alone(*row.right, "came");

            if (row.left) ++at_one;
            if (row.right) ++at_two;
        }
    }

    if (ones.size() - at_one != twos.size() - at_two) std::terminate();

    for (; at_one < ones.size(); ++at_one, ++at_two)
        if (ones[at_one] != twos[at_two]) std::terminate();

    return 0;
}

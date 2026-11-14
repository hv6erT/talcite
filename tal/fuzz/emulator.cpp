#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tal/emulator.hpp"
#include "tal/text.hpp"

namespace {

// Stops the run unless the screen holds together: its size and cursor within bounds, every row as
// wide as the screen, no half of a wide character alone, no cluster past its bound or not UTF-8,
// every link of a cell with its address, the history within `kept`, the prompt on the screen.
void whole(const tal::Emulator& one, std::size_t kept) {
    const tal::Size size = one.size();
    const tal::Spot cursor = one.cursor();

    if (size.wide < 1 || size.wide > 1024 || size.high < 1 || size.high > 1024) std::terminate();

    if (cursor.x < 0 || cursor.x >= size.wide || cursor.y < 0 || cursor.y >= size.high) std::terminate();

    if (one.history() > kept) std::terminate();

    for (int at = -static_cast<int>(one.history()); at < size.high; ++at) {
        const std::vector<tal::Cell> cells = one.row(at).cells;

        if (cells.size() != static_cast<std::size_t>(size.wide)) std::terminate();

        for (std::size_t cell = 0; cell < cells.size(); ++cell) {
            const tal::Cell& each = cells[cell];
            const bool wide = !each.cluster.empty() && tal::text::width(each.cluster) == 2;

            if (each.cluster.size() > 32 || !tal::text::valid(each.cluster)) std::terminate();

            if (each.cluster.empty() && (cell == 0 || cells[cell - 1].cluster.empty() || tal::text::width(cells[cell - 1].cluster) != 2)) std::terminate();

            if (wide && (cell + 1 >= cells.size() || !cells[cell + 1].cluster.empty())) std::terminate();

            if (each.link != 0 && one.address(each.link).empty()) std::terminate();
        }
    }

    if (const std::optional<int> row = one.prompt(); row && (*row < 0 || *row >= size.high)) std::terminate();
}

// The glyphs of every line the rows make, a line going on over the rows that wrapped: each cell
// that is not a space, with its look and link. A new width must keep them, since it only lays the
// same text out anew; the empty lines at the end do not count.
std::vector<std::vector<tal::Cell>> glyphs(const tal::Emulator& one) {
    std::vector<std::vector<tal::Cell>> made(1);

    for (int at = -static_cast<int>(one.history()); at < one.size().high; ++at) {
        const tal::Emulator::Row row = one.row(at);

        for (const tal::Cell& each : row.cells)
            if (each.cluster != " " && !each.cluster.empty()) made.back().push_back(each);

        if (!row.wrapped) made.emplace_back();
    }

    while (!made.empty() && made.back().empty()) made.pop_back();

    return made;
}

}

// One input of libFuzzer as records: a byte that says what, and what follows. A byte of 0 mod 8
// gives a new size in the next two; any other a length, and that many bytes are fed as the
// program's output. The first two bytes give the first size and the third the lines of history.
// After every step the screen holds together; a new width of two cells or more, with the history
// not full, keeps the glyphs of every line. Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 3) return 0;

    const std::size_t kept = data[2] % 64;
    tal::Emulator one{{1 + data[0] % 48, 1 + data[1] % 16}, kept};

    for (std::size_t at = 3; at < size;) {
        const std::uint8_t what = data[at++];

        if (what % 8 == 0) {
            if (at + 2 > size) break;

            const tal::Size next{std::max(1, data[at] % 64), std::max(1, data[at + 1] % 20)};
            const bool checked = !one.modes().alternate && one.size().wide >= 2 && next.wide >= 2 && next.high == one.size().high;
            const auto before = checked ? glyphs(one) : std::vector<std::vector<tal::Cell>>{};

            at += 2;
            one.resize(next);

            if (checked && one.history() < kept && glyphs(one) != before) std::terminate();
        } else {
            const std::size_t much = std::min<std::size_t>(what, size - at);

            (void)one.feed(std::span<const std::byte>{reinterpret_cast<const std::byte*>(data + at), much});
            at += much;
        }

        whole(one, kept);
    }

    return 0;
}

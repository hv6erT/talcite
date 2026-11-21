#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "tal/docs.hpp"
#include "tal/pages.hpp"
#include "tal/text.hpp"

// A text version (docs::Plain) written as plain text for a Pager, with its links as marks of the
// part "link". Headings start with as many '#' as their level, items with their mark and an
// indent of two spaces a level, quotes with one "> " a level, cells are joined by " | ", a rule is
// "---" and the start of a page "— 3 —"; a further paragraph of an item hangs under the text of
// the item before it. Blocks are parted by an empty line, but items follow items and rows follow
// rows on the next line, as lists and tables are read.
namespace tal {

namespace {

// The number of characters of `text`, which is how wide a mark of a list is.
[[nodiscard]] std::size_t characters(std::string_view text) {
    std::size_t made = 0;
    std::size_t at = 0;

    while (at < text.size()) {
        (void)text::decoded(text, at);
        ++made;
    }

    return made;
}

// The writing of one text version, line by line.
class Writing {
public:
    // The whole of `plain` written.
    [[nodiscard]] Written write(const docs::Plain& plain) {
        const docs::Block* before = nullptr;

        for (const docs::Block& block : plain.blocks) {
            if (before != nullptr && !together(*before, block)) line({});

            wrote(block);
            before = &block;
        }

        return std::move(made_);
    }

private:
    // Whether `next` follows `last` on the next line, without an empty one between.
    [[nodiscard]] static bool together(const docs::Block& last, const docs::Block& next) {
        using Kind = docs::Block::Kind;

        return (last.kind == Kind::item && next.kind == Kind::item) || (last.kind == Kind::row && next.kind == Kind::row);
    }

    // One line ended and the next begun.
    void line(std::string_view text) {
        if (lines_ > 0) made_.text += '\n';

        made_.text += text;
        ++lines_;
    }

    // `block` written.
    void wrote(const docs::Block& block) {
        using Kind = docs::Block::Kind;

        switch (block.kind) {
            case Kind::row: return rowed(block);
            case Kind::rule: return line("---");
            case Kind::page: return line("— " + block.text + " —");
            case Kind::heading: return prefixed(block, std::string(static_cast<std::size_t>(block.level), '#') + " ", std::string(static_cast<std::size_t>(block.level) + 1, ' '));
            case Kind::quote: {
                std::string marks;

                for (int one = 0; one < block.level; ++one) marks += "> ";

                return prefixed(block, marks, marks);
            }
            case Kind::item: {
                const std::string indent(static_cast<std::size_t>(std::max(block.level - 1, 0)) * 2, ' ');

                const auto level = static_cast<std::size_t>(std::max(block.level, 1));

                if (hangs_.size() < level) hangs_.resize(level, 0);

                if (block.mark.empty()) {
                    const std::string under(hangs_[level - 1] > 0 ? hangs_[level - 1] : indent.size() + 2, ' ');

                    return prefixed(block, under, under);
                }

                hangs_[level - 1] = indent.size() + characters(block.mark) + 1;

                return prefixed(block, indent + block.mark + " ", std::string(hangs_[level - 1], ' '));
            }
            case Kind::paragraph:
            case Kind::preformatted: return prefixed(block, {}, {});
        }
    }

    // The lines of the text of `block`, the first after `first` and the others after `rest`, its
    // anchors marked.
    void prefixed(const docs::Block& block, const std::string& first, const std::string& rest) {
        const std::size_t top = lines_;
        std::vector<std::size_t> starts;
        std::vector<std::size_t> fronts;
        std::size_t at = 0;

        while (true) {
            const std::size_t end = std::min(block.text.find('\n', at), block.text.size());
            const std::string& front = starts.empty() ? first : rest;

            starts.push_back(at);
            fronts.push_back(front.size());
            line(front + block.text.substr(at, end - at));

            if (end >= block.text.size()) break;

            at = end + 1;
        }

        const auto place = [&](std::size_t offset) {
            std::size_t which = 0;

            while (which + 1 < starts.size() && starts[which + 1] <= offset) ++which;

            return Place{top + which, fronts[which] + offset - starts[which]};
        };

        for (const docs::Anchor& one : block.anchors)
            if (one.from < one.upto && one.upto <= block.text.size()) made_.marks.push_back(Stretch{place(one.from), place(one.upto), "link", one.address});
    }

    // The row `block` written on one line, its cells joined by " | ", its anchors marked.
    void rowed(const docs::Block& block) {
        std::string text;
        std::vector<std::size_t> starts;

        for (const std::string& cell : block.cells) {
            if (!starts.empty()) text += " | ";

            starts.push_back(text.size());
            text += cell;
        }

        line(text);

        for (const docs::Anchor& one : block.anchors) {
            if (one.cell >= block.cells.size() || one.from >= one.upto || one.upto > block.cells[one.cell].size()) continue;

            made_.marks.push_back(Stretch{Place{lines_ - 1, starts[one.cell] + one.from}, Place{lines_ - 1, starts[one.cell] + one.upto}, "link", one.address});
        }
    }

    Written made_;
    std::size_t lines_ = 0;
    std::vector<std::size_t> hangs_;
};

}

// `plain` as plain text and the marks of its links.
Written written(const docs::Plain& plain) { return Writing{}.write(plain); }

}

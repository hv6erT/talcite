#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>

#include "tal/docs.hpp"
#include "tal/pages.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as an HTML page, its first byte choosing the fallback encoding, read into
// its text version within small bounds and written for a Pager. Nothing may crash, hang or read out
// of bounds; the only trouble is a page over its bytes; the bounds hold; every anchor lies inside
// its text or cell; every mark of the writing lies on a line of it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;

    tal::docs::Bounds bounds;

    bounds.bytes = 1 << 16;
    bounds.text = 1 << 14;
    bounds.blocks = 256;
    bounds.anchors = 64;
    bounds.depth = 32;

    const auto fallback = static_cast<tal::docs::Encoding>(data[0] % 3);
    const std::span<const std::byte> page{reinterpret_cast<const std::byte*>(data + 1), size - 1};
    tal::docs::Plain plain;

    try {
        plain = tal::docs::plain(page, "http://example.test/a/b.html", fallback, bounds);
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::full || page.size() <= bounds.bytes) std::terminate();

        return 0;
    }

    std::size_t text = 0;
    std::size_t anchors = 0;

    for (const tal::docs::Block& block : plain.blocks) {
        text += block.text.size();

        for (const std::string& cell : block.cells) text += cell.size();

        for (const tal::docs::Anchor& one : block.anchors) {
            const std::string& body = block.kind == tal::docs::Block::Kind::row ? block.cells.at(one.cell) : block.text;

            if (one.from >= one.upto || one.upto > body.size()) std::terminate();
        }

        anchors += block.anchors.size();
    }

    if (plain.blocks.size() > bounds.blocks || text > bounds.text || anchors > bounds.anchors) std::terminate();

    const tal::Written written = tal::written(plain);
    std::size_t lines = 1;

    for (const char one : written.text) lines += one == '\n' ? 1 : 0;

    for (const tal::Stretch& one : written.marks)
        if (!(one.from < one.upto) || one.upto.line >= lines) std::terminate();

    return 0;
}

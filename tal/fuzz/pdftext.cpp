#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>

#include "tal/docs.hpp"
#include "tal/executor.hpp"
#include "tal/meta.hpp"
#include "tal/pages.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as a PDF document whose text version is read: its first eight pages,
// each written for a Pager. Nothing may crash, hang, read out of bounds or leak; the only trouble
// is a refusal of the document (not a PDF, encrypted, without a catalog) or of a page (its
// content broken, past the file's budget); every text is UTF-8 and every mark of the writing lies
// on a line of it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static tal::Loop loop;
    const tal::Bytes given(reinterpret_cast<const std::byte*>(data), reinterpret_cast<const std::byte*>(data) + size);
    const auto allowed = [](const tal::Trouble& why) {
        const tal::Kind kind = why.kind();

        return kind == tal::Kind::unsupported || kind == tal::Kind::denied || kind == tal::Kind::malformed || kind == tal::Kind::broken ||
               kind == tal::Kind::full;
    };
    std::shared_ptr<tal::docs::Prose> prose;

    try {
        prose = tal::wait(loop, tal::docs::prose(std::make_shared<tal::meta::Held>(given)));
    } catch (const tal::Trouble& why) {
        if (!allowed(why)) std::terminate();

        return 0;
    }

    for (std::size_t which = 0; which < prose->pages() && which < 8; ++which) {
        tal::docs::Plain plain;

        try {
            plain = tal::wait(loop, prose->page(which));
        } catch (const tal::Trouble& why) {
            if (!allowed(why)) std::terminate();

            continue;
        }

        for (const tal::docs::Block& block : plain.blocks)
            if (!tal::text::valid(block.text)) std::terminate();

        const tal::Written written = tal::written(plain);
        std::size_t lines = 1;

        for (const char one : written.text) lines += one == '\n' ? 1 : 0;

        for (const tal::Stretch& one : written.marks)
            if (!(one.from < one.upto) || one.upto.line >= lines) std::terminate();
    }

    return 0;
}

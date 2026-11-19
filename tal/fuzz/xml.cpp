#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "tal/trouble.hpp"
#include "tal/xml.hpp"

// One input of libFuzzer as an XML text, read with small limits. A text that reads must write back
// to a text that reads equal, but for the declaration the writer puts first where there was none;
// one that does not read must be refused with `Trouble` and nothing else. Neither may crash, hang,
// read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const tal::xml::Limits limits{.bytes = 1 << 16, .depth = 64, .nodes = 4096};
    tal::xml::Document read;

    try {
        read = tal::xml::parsed(text, limits);
    } catch (const tal::Trouble&) {
        return 0;
    }

    if (std::none_of(read.nodes.begin(), read.nodes.end(), [](const tal::xml::Node& one) { return one.kind == tal::xml::Kind::declaration; })) {
        tal::xml::Node declared;

        declared.kind = tal::xml::Kind::declaration;
        declared.name = "xml";
        declared.attributes = {{"version", "1.0"}};
        read.nodes.insert(read.nodes.begin(), declared);
    }

    if (!(tal::xml::parsed(tal::xml::written(read)) == read)) std::terminate();

    return 0;
}

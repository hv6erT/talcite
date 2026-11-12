#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "tal/json.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as a JSON text, read with the limits a program uses. A text that reads
// must write back to a text that reads equal, compact and indented; one that does not read must be
// refused with `Trouble` and nothing else. Neither may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    tal::json::Value read;

    try {
        read = tal::json::parsed(text);
    } catch (const tal::Trouble&) {
        return 0;
    }

    if (!(tal::json::parsed(tal::json::written(read)) == read)) std::terminate();

    if (!(tal::json::parsed(tal::json::written(read, true)) == read)) std::terminate();

    return 0;
}

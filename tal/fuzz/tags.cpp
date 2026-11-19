#include <cstddef>
#include <cstdint>
#include <exception>

#include "tal/executor.hpp"
#include "tal/meta.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as a file for the tag readers: described, then every picture it lists
// asked for. Describing never throws; a picture is either bytes no longer than the file or a
// refusal (missing, full). Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static tal::Loop loop;
    const tal::Bytes given(reinterpret_cast<const std::byte*>(data), reinterpret_cast<const std::byte*>(data) + size);
    tal::meta::Held from{given};
    const tal::meta::Description made = tal::wait(loop, tal::meta::described(from));

    for (std::size_t which = 0; which < made.pictures.size() && which < 4; ++which) {
        try {
            const tal::Bytes picture = tal::wait(loop, tal::meta::pictured(from, which));

            if (picture.size() > size) std::terminate();
        } catch (const tal::Trouble& why) {
            if (why.kind() != tal::Kind::missing && why.kind() != tal::Kind::full) std::terminate();
        }
    }

    return 0;
}

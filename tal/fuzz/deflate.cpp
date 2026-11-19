#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include "tal/bytes.hpp"

#include "deflating.hpp"
#include "inflating.hpp"

// One input of libFuzzer packed by our deflate and taken back by our inflater, fed in pieces of a
// size the first byte gives: what comes back must be the input whole, and the stream must end.
// Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 1) return 0;

    const std::size_t piece = 1 + data[0];
    const std::span<const std::byte> given{reinterpret_cast<const std::byte*>(data + 1), size - 1};
    const tal::Bytes packed = tal::deflating::deflated(given);
    tal::inflating::Inflater one{tal::inflating::Wrapping::zlib};
    tal::Bytes made;

    for (std::size_t at = 0; at < packed.size(); at += piece) one.fed(std::span<const std::byte>{packed}.subspan(at, std::min(piece, packed.size() - at)), made);

    if (!one.ended() || !std::equal(made.begin(), made.end(), given.begin(), given.end())) std::terminate();

    return 0;
}

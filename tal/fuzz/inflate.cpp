#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include "tal/bytes.hpp"
#include "tal/trouble.hpp"

#include "inflating.hpp"

#ifdef TAL_FUZZ_ZLIB
#include <zlib.h>
#endif

// One input of libFuzzer as deflate or gzip, by its first byte, fed in pieces of a size the second
// byte gives. The inflater may refuse with `Trouble` and nothing else, and never crash, hang, read
// out of bounds or leak. Where the fuzz build has zlib, raw deflate is checked against it: what
// both finish they finish alike, what zlib refuses ours refuses or waits on, and what zlib makes of
// the input packed again comes back whole.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;

    const bool gzip = (data[0] & 1U) != 0;
    const std::size_t piece = 1 + data[1];
    const std::span<const std::byte> given{reinterpret_cast<const std::byte*>(data + 2), size - 2};
    tal::inflating::Inflater one{gzip ? tal::inflating::Wrapping::gzip : tal::inflating::Wrapping::raw};
    tal::Bytes made;
    bool refused = false;

    try {
        for (std::size_t at = 0; at < given.size(); at += piece) {
            const tal::Bytes got = one.fed(given.subspan(at, std::min(piece, given.size() - at)));

            made.insert(made.end(), got.begin(), got.end());

            if (made.size() > (std::size_t{4} << 20)) return 0;
        }
    } catch (const tal::Trouble&) {
        refused = true;
    }

#ifdef TAL_FUZZ_ZLIB
    if (!gzip) {
        z_stream stream{};
        tal::Bytes theirs(std::min(std::size_t{4} << 20, given.size() * 1100 + 1024));

        inflateInit2(&stream, -15);
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(given.data()));
        stream.avail_in = static_cast<uInt>(given.size());
        stream.next_out = reinterpret_cast<Bytef*>(theirs.data());
        stream.avail_out = static_cast<uInt>(theirs.size());

        const int said = inflate(&stream, Z_FINISH);
        const bool ended = said == Z_STREAM_END && stream.avail_in == 0;
        const bool broken = said == Z_DATA_ERROR || (said == Z_STREAM_END && stream.avail_in != 0);

        theirs.resize(stream.total_out);
        inflateEnd(&stream);

        if (said != Z_BUF_ERROR || stream.avail_out != 0) {
            if (ended && (refused || !one.ended() || made != theirs)) std::terminate();

            if (broken && !refused) std::terminate();

            if (!ended && !broken && one.ended()) std::terminate();
        }
    }

    {
        z_stream stream{};
        tal::Bytes packed(compressBound(static_cast<uLong>(given.size())) + 64);

        deflateInit2(&stream, data[1] % 10, Z_DEFLATED, gzip ? 31 : -15, 8, (data[0] >> 1) % 5);
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(given.data()));
        stream.avail_in = static_cast<uInt>(given.size());
        stream.next_out = reinterpret_cast<Bytef*>(packed.data());
        stream.avail_out = static_cast<uInt>(packed.size());
        deflate(&stream, Z_FINISH);
        packed.resize(stream.total_out);
        deflateEnd(&stream);

        tal::inflating::Inflater again{gzip ? tal::inflating::Wrapping::gzip : tal::inflating::Wrapping::raw};
        const tal::Bytes back = again.fed(packed);

        if (!again.ended() || !std::equal(back.begin(), back.end(), given.begin(), given.end())) std::terminate();
    }
#else
    (void)refused;
#endif

    return 0;
}

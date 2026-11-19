#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "inflating.hpp"

// Inflate, gzip and zlib: streams that zlib made, read whole and in pieces down to a byte; streams
// written bit by bit here for each mistake the reader must find; gzip's header fields, trailer and
// members, zlib's two bytes and Adler-32. Compared against zlib itself by the fuzzer.

namespace {

using tal::Bytes;
using tal::inflating::Inflater;
using tal::inflating::Rest;
using tal::inflating::Wrapping;

// The bytes of the hexadecimal text `hex`.
Bytes unhexed(std::string_view hex) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2) made.push_back(static_cast<std::byte>(std::stoi(std::string{hex.substr(at, 2)}, nullptr, 16)));

    return made;
}

// The text "Poirot: the little grey cells. " forty times, then every byte from 0 to 255.
Bytes cells() {
    std::string made;

    for (int one = 0; one < 40; ++one) made += "Poirot: the little grey cells. ";

    Bytes all = tal::bytes(made);

    for (int one = 0; one < 256; ++one) all.push_back(static_cast<std::byte>(one));

    return all;
}

// `much` bytes of a skewed alphabet from the generator of C's rand with `seed`, as the vectors
// below were made in Python.
Bytes skewed(std::size_t much, std::uint32_t seed) {
    constexpr std::string_view alphabet = "aaaaaaaabbbbccd \n";
    Bytes made;

    for (std::size_t at = 0; at < much; ++at) {
        seed = (seed * 1103515245U + 12345U) & 0x7FFFFFFFU;
        made.push_back(static_cast<std::byte>(alphabet[(seed >> 16) % alphabet.size()]));
    }

    return made;
}

// The first 300 bytes of `cells()` in a stored block, as zlib made it at level 0.
const std::string_view stored_vector =
    "012c01d3fe506f69726f743a20746865206c6974746c6520677265792063656c6c732e20506f69726f743a20746865206c6974746c6520677265792063656c6c732e20506f69726f743a2074686520"
    "6c6974746c6520677265792063656c6c732e20506f69726f743a20746865206c6974746c6520677265792063656c6c732e20506f69726f743a20746865206c6974746c652067726579206365"
    "6c6c732e20506f69726f743a20746865206c6974746c6520677265792063656c6c732e20506f69726f743a20746865206c6974746c6520677265792063656c6c732e20506f69726f743a2074"
    "6865206c6974746c6520677265792063656c6c732e20506f69726f743a20746865206c6974746c6520677265792063656c6c732e20506f69726f743a20746865206c6974746c65206772";

// `cells()` in zlib's wrapping (RFC 1950), as zlib made it at level 9.
const std::string_view zlib_vector =
    "78da0bc8cf2cca2fb15228c94855c8c92c29c94955482f4aad54484ecdc929d6530818951e951e951e951e6ad20c8c4ccc2cac6cec1c9c5cdc3cbc7cfc028242c222a262e2129252d232b2"
    "72f20a8a4aca2aaa6aea1a9a5ada3aba7afa068646c626a666e6169656d636b676f60e8e4ece2eae6eee1e9e5ede3ebe7efe018141c121a161e1119151d131b171f1098949c929a969e919"
    "9959d939b979f9058545c525a565e5159555d535b575f50d8d4dcd2dad6ded1d9d5ddd3dbd7dfd13264e9a3c65eab4e93366ce9a3d67eebcf90b162e5abc64e9b2e52b56ae5abd66edbaf5"
    "1b366edabc65ebb6ed3b76eedabd67efbefd070e1e3a7ce4e8b1e3274e9e3a7de6ecb9f3172e5eba7ce5eab5eb376edeba7de7eebdfb0f1e3e7afce4e9b3e72f5ebe7afde6edbbf71f3e7e"
    "fafce5ebb7ef3f7efefafde7efbfff005a323c4f";

// `cells()` in the fixed code, as zlib made it with Z_FIXED.
const std::string_view fixed_vector =
    "0bc8cf2cca2fb15228c94855c8c92c29c94955482f4aad54484ecdc929d6530818951e951e951e951e6ad20c8c4ccc2cac6cec1c9c5cdc3cbc7cfc028242c222a262e2129252d232b272f20a"
    "8a4aca2aaa6aea1a9a5ada3aba7afa068646c626a666e6169656d636b676f60e8e4ece2eae6eee1e9e5ede3ebe7efe018141c121a161e1119151d131b171f1098949c929a969e9199959d939b"
    "979f9058545c525a565e5159555d535b575f50d8d4dcd2dad6ded1d9d5ddd3dbd7dfd13264e9a3c65eab4e93366ce9a3d67eebcf90b162e5abc64e9b2e52b56ae5abd66edbaf51b366edabc65e"
    "bb6ed3b76eedabd67efbefd070e1e3a7ce4e8b1e3274e9e3a7de6ecb9f3172e5eba7ce5eab5eb376edeba7de7eebdfb0f1e3e7afce4e9b3e72f5ebe7afde6edbbf71f3e7efafce5ebb7ef3f7ef"
    "efafde7efbfff00";

// `skewed(3000, 7)` in a dynamic code, as zlib made it at level 9.
const std::string_view dynamic_vector =
    "4d56d975c3300cfbe7145c8dc7fe339438e426ed8b635be20182a0766b3aaa263aee72ffbd785455591bd5359379df39f7a8ef57cdbd295c8a8feebf6bf1646f1b36f2b3cd65cdb5117b6fe0"
    "07fb71bd1f39dd3de75296922e9361e03d3eb4cd20a2e2ee229b3e1b378c8d168397c61d23c796a1abdb8185e769e99921225ebedf3987300337b726b1f05cddd7e44b053b98c43d8c7b8ce7"
    "f81eb8fd09f1002c02d1c4106f78b77ac8c0f35cad41c2e3a1ad5e602d48f13589f81938c26226717682c9c00c8dd1481c8cd1ca817b89d6ed6b452298f496bb007829b96610cddd79555e2f"
    "89725ec94c2fe40de17a4c4845d32e7e2a1bae9f038869cfcac5553859c9471386b30aaa8deda1b87254329ab06aa382fbf23e96c4a2cef1151e1ce864660ba343b625569f5170614c93121d"
    "10cd911b21ef3e040c3288dd70625cb185958b7c19cf95e2554ade8ad85d97a0a62a80addec27d69df36f94941f190b709e4f1daa0ab0d021325bfba95e9ad99f68f5cbde21d62df9c03316"
    "30cdbb2299bd50a6ebdcb018d845a9d88228169dbea3eb4c3964b7800060131df1e8c5003c6d10ec694496d9240bc1abf4c93a59f7a80275a3bd4c762f4e36a0a81544dfb51e6074413091"
    "692880d331b46731741abde1b7a55bf298576c9b8065cce19ae3ba662a1b28500324ac1594fd51600b6dacff2244f20dc75677fbaa418d5f4af54d818409d806d3f2d6c979a9274fa74b543"
    "4cd00a512100cfd8c67e627212b36c667a23239e60318eebe87f458efa9479c06c958d7aa01a34d13ad5e13236ced737867d8971947adeb236a6f43a54207ab50a834973e3deef7541e937"
    "3f25d879ed700a073500a9afad4c30e6984849bab6a22f8bf98d8eb660a283130c12a532a42a4de5601249ed1adba3ebb4a4b41bd36dec0ecbfd6898ee4daa8d5a3c25d9e8a9ecfef45341"
    "4967f3e5d6a82f4a479adc7d3c845632c4267d1c475d24845426c633119259461d9815ac28a7c8590f35e67e5464cdd674837c27a34dca198984a268ae7f656079de88a97faeb42202ebdc"
    "e8dc3d39326f3dc5f80ab3b5bb5ee332e7560f8a560cdef469cd8c00f76fd89714ea89a1d5e16989cbe3b91750a572fd91bba4a6b95fb3bbc60c27c149c4c8e0749a6f1070132b210b69ee"
    "385af75026631bae4c48ae69e2dda133c47c946991815dfd441b6da277835145563831f09e63bfe74dc5d2e94585506169efa6dfb26e408dd2303c3ed0044f02ef20c519709dc3b9581e"
    "4468a5fa499e279df6c94ea789e29108d160c48e6794f00dfc3d2563dbbb19926796447137abbee6b20e2ec7e95758c1c35a80b940571e87c7cfe2acf0f0694d6231297ddccaf148f8464c"
    "847add079d5dae99a7546e4e41c814a4ee8361181898d366f63b3931ec5966ac5ef45492f6c873689b8f88fd0a963c54b86af394983dcc35e2ba9428141f8f8fede3ecb039a89938562c55e"
    "80f";

// What `inflater` gives for `given` fed in pieces of `piece` bytes.
Bytes fed(Inflater& inflater, const Bytes& given, std::size_t piece) {
    Bytes made;

    for (std::size_t at = 0; at < given.size(); at += piece) {
        const Bytes got = inflater.fed(std::span<const std::byte>{given}.subspan(at, std::min(piece, given.size() - at)));

        made.insert(made.end(), got.begin(), got.end());
    }

    return made;
}

// What raw deflate `given` comes to, read whole, or the trouble's message.
std::string inflated(const Bytes& given, Wrapping how = Wrapping::raw) {
    Inflater one{how};

    try {
        const Bytes made = one.fed(given);

        return tal::text::of(made) + (one.ended() ? "" : "...");
    } catch (const tal::Trouble& why) {
        return why.what();
    }
}

// Bits written as deflate reads them: values from the lowest bit, codes from the highest.
class Bits {
public:
    // Appends the `many` low bits of `value`, the lowest first.
    Bits& put(std::uint32_t value, int many) {
        for (int one = 0; one < many; ++one) bit((value >> one) & 1U);

        return *this;
    }

    // Appends the code `code` of `length` bits, the highest first.
    Bits& code(std::uint32_t code, int length) {
        for (int one = length - 1; one >= 0; --one) bit((code >> one) & 1U);

        return *this;
    }

    // Appends the symbol `symbol` of the fixed code of lengths.
    Bits& literal(std::uint32_t symbol) {
        if (symbol < 144) return code(0x30 + symbol, 8);

        if (symbol < 256) return code(0x190 + symbol - 144, 9);

        if (symbol < 280) return code(symbol - 256, 7);

        return code(0xC0 + symbol - 280, 8);
    }

    // Pads to a whole byte and appends `bytes`.
    Bits& bytes(const Bytes& given) {
        while (used_ != 0) bit(0);

        made_.insert(made_.end(), given.begin(), given.end());

        return *this;
    }

    // What was written, padded to a whole byte.
    [[nodiscard]] Bytes done() {
        while (used_ != 0) bit(0);

        return made_;
    }

private:
    // Appends one bit.
    void bit(std::uint32_t one) {
        if (used_ == 0) made_.push_back(std::byte{0});

        made_.back() |= static_cast<std::byte>(one << used_);
        used_ = (used_ + 1) % 8;
    }

    Bytes made_;
    int used_ = 0;
};

// `one` and then `two`.
Bytes joined(Bytes one, const Bytes& two) {
    one.insert(one.end(), two.begin(), two.end());

    return one;
}

// The 32 bits of `value`, the lowest byte first.
Bytes little(std::uint32_t value) {
    return Bytes{static_cast<std::byte>(value & 0xFF), static_cast<std::byte>((value >> 8) & 0xFF), static_cast<std::byte>((value >> 16) & 0xFF),
                 static_cast<std::byte>(value >> 24)};
}

// A gzip member of `text` in a stored block, with the header `flags` and `fields` after the fixed
// part of the header, and its trailer.
Bytes gzipped(const std::string& text, std::uint8_t flags = 0, const Bytes& fields = {}) {
    Bytes made{std::byte{0x1F}, std::byte{0x8B}, std::byte{8}, static_cast<std::byte>(flags), std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3}};
    const Bytes plain = tal::bytes(text);

    made.insert(made.end(), fields.begin(), fields.end());

    const auto length = static_cast<std::uint32_t>(plain.size());
    const Bytes body = Bits{}.put(1, 1).put(0, 2).bytes(little(length | ((length ^ 0xFFFFU) << 16))).bytes(plain).done();

    made.insert(made.end(), body.begin(), body.end());

    const Bytes crc = little(tal::inflating::crc32(0, plain));
    const Bytes size = little(static_cast<std::uint32_t>(plain.size()));

    made.insert(made.end(), crc.begin(), crc.end());
    made.insert(made.end(), size.begin(), size.end());

    return made;
}

}

TRIAL("Inflate: CRC-32 of the check string, and streams zlib made: stored, fixed and dynamic, whole and in pieces") {
    SAME(tal::inflating::crc32(0, tal::bytes("123456789")), std::uint32_t{0xCBF43926});
    SAME(tal::inflating::crc32(tal::inflating::crc32(0, tal::bytes("1234")), tal::bytes("56789")), std::uint32_t{0xCBF43926});
    SAME(tal::inflating::crc32(0, Bytes{}), std::uint32_t{0});

    const Bytes all = cells();
    const Bytes stored{all.begin(), all.begin() + 300};

    for (const std::size_t piece : {std::size_t{1}, std::size_t{7}, std::size_t{100000}}) {
        Inflater one{Wrapping::raw};
        Inflater two{Wrapping::raw};
        Inflater three{Wrapping::raw};

        SAME(fed(one, unhexed(stored_vector), piece), stored);
        OUGHT(one.ended());
        SAME(fed(two, unhexed(fixed_vector), piece), all);
        OUGHT(two.ended());
        SAME(fed(three, unhexed(dynamic_vector), piece), skewed(3000, 7));
        OUGHT(three.ended());
    }

    Inflater part{Wrapping::raw};
    const Bytes dynamic = unhexed(dynamic_vector);

    OUGHT(part.fed(std::span<const std::byte>{dynamic}.subspan(0, 500)).size() < 3000);
    OUGHT(!part.ended());
}

TRIAL("Inflate: matches reach back the whole window across the cut of the history, and overlap") {
    Bytes plain;

    for (std::size_t at = 0; at < 70000; ++at) plain.push_back(static_cast<std::byte>((at * 7919) % 251));

    Bits bits;

    bits.put(0, 1).put(0, 2).bytes(little(65535 | (0U << 16))).bytes(Bytes(plain.begin(), plain.begin() + 65535));
    bits.put(0, 1).put(0, 2).bytes(little(4465 | ((4465U ^ 0xFFFFU) << 16))).bytes(Bytes(plain.begin() + 65535, plain.end()));
    bits.put(1, 1).put(1, 2).literal(285).code(29, 5).put(32768 - 24577, 13).literal(65).literal(258).code(0, 5).literal(256);

    Bytes wanted = plain;

    for (std::size_t one = 0; one < 258; ++one) wanted.push_back(wanted[wanted.size() - 32768]);

    wanted.push_back(std::byte{65});

    for (std::size_t one = 0; one < 4; ++one) wanted.push_back(std::byte{65});

    for (const std::size_t piece : {std::size_t{3}, std::size_t{40000}, std::size_t{1000000}}) {
        Inflater one{Wrapping::raw};

        SAME(fed(one, bits.done(), piece), wanted);
        OUGHT(one.ended());
    }
}

TRIAL("Inflate: each mistake of a stream is malformed, as zlib finds it") {
    const auto broken = [](const Bytes& given) {
        const std::string said = inflated(given);

        return said.starts_with("The compressed data is damaged: ") ? said.substr(32) : "not broken: " + said;
    };

    SAME(broken(Bits{}.put(1, 1).put(0, 2).bytes(little(5 | (5U << 16))).done()), std::string{"a stored block's length and its complement differ."});
    SAME(broken(Bits{}.put(1, 1).put(3, 2).done()), std::string{"a block of the reserved type."});
    SAME(broken(Bits{}.put(1, 1).put(2, 2).put(30, 5).put(0, 5).put(0, 4).done()), std::string{"too many codes."});
    SAME(broken(Bits{}.put(1, 1).put(2, 2).put(0, 5).put(30, 5).put(0, 4).done()), std::string{"too many codes."});
    SAME(broken(Bits{}.put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(0, 4).put(1, 3).put(1, 3).put(1, 3).put(1, 3).done()),
         std::string{"the code of code lengths is not complete."});
    SAME(broken(Bits{}.put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(0, 4).put(2, 3).put(2, 3).put(2, 3).put(0, 3).done()),
         std::string{"the code of code lengths is not complete."});

    // The code of code lengths: 0, 16, 17 and 18 of two bits each, so 00, 01, 10 and 11.
    const auto lengthy = [](Bits& bits, std::uint32_t literals, std::uint32_t distances) -> Bits& {
        return bits.put(1, 1).put(2, 2).put(literals - 257, 5).put(distances - 1, 5).put(0, 4).put(2, 3).put(2, 3).put(2, 3).put(2, 3);
    };

    {
        Bits bits;

        SAME(broken(lengthy(bits, 257, 1).code(1, 2).put(0, 2).done()), std::string{"a repeat with no length before it."});
    }
    {
        Bits bits;

        SAME(broken(lengthy(bits, 257, 1).code(3, 2).put(127, 7).code(3, 2).put(127, 7).done()), std::string{"a repeat past the last length."});
    }
    {
        Bits bits;

        lengthy(bits, 257, 1).code(3, 2).put(127, 7).code(3, 2).put(109, 7);

        SAME(broken(bits.done()), std::string{"a block without the code of its end."});
    }
    {
        Bits bits;

        bits.put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(15, 4);

        for (int one = 0; one < 19; ++one) bits.put(0, 3);

        SAME(broken(bits.done()), std::string{"a code that the table does not hold."});
    }

    SAME(broken(Bits{}.put(1, 1).put(1, 2).literal(257).code(0, 5).done()), std::string{"a distance back before the start."});
    SAME(broken(Bits{}.put(1, 1).put(1, 2).literal(65).literal(258).code(1, 5).done()), std::string{"a distance back before the start."});
    SAME(broken(Bits{}.put(1, 1).put(1, 2).literal(286).done()), std::string{"a length symbol out of range."});
    SAME(broken(Bits{}.put(1, 1).put(1, 2).literal(287).done()), std::string{"a length symbol out of range."});
    SAME(broken(Bits{}.put(1, 1).put(1, 2).literal(65).literal(257).code(30, 5).done()), std::string{"a distance symbol out of range."});
    SAME(broken(Bits{}.put(1, 1).put(1, 2).literal(65).literal(257).code(31, 5).done()), std::string{"a distance symbol out of range."});
    SAME(broken(joined(Bits{}.put(1, 1).put(1, 2).literal(256).done(), Bytes{std::byte{0}})), std::string{"something follows the end."});

    SAME(inflated(Bits{}.put(1, 1).put(1, 2).literal(65).literal(257).code(0, 5).literal(256).done()), std::string{"AAAA"});
    SAME(inflated(Bits{}.put(0, 1).put(1, 2).literal(66).literal(256).put(1, 1).put(0, 2).bytes(little(0xFFFF0000U)).done()), std::string{"B"});
    SAME(inflated(Bits{}.put(0, 1).put(1, 2).literal(66).done()), std::string{"B..."});
    SAME(inflated(Bytes{}), std::string{"..."});
}

TRIAL("Inflate: gzip skips its header's fields, checks the trailer, reads members one after another and nothing after") {
    SAME(inflated(gzipped("Hastings"), Wrapping::gzip), std::string{"Hastings"});

    const Bytes fields = joined(joined(Bytes{std::byte{3}, std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}}, tal::bytes(std::string{"name.txt", 9})),
                                joined(tal::bytes(std::string{"a comment", 10}), Bytes{std::byte{0xAB}, std::byte{0xCD}}));

    SAME(inflated(gzipped("Japp", 0x1E, fields), Wrapping::gzip), std::string{"Japp"});

    for (const std::size_t piece : {std::size_t{1}, std::size_t{5}}) {
        Inflater one{Wrapping::gzip};

        SAME(tal::text::of(fed(one, joined(gzipped("Lemon", 0x1E, fields), gzipped("")), piece)), std::string{"Lemon"});
        OUGHT(one.ended());
    }

    Inflater two{Wrapping::gzip};

    SAME(tal::text::of(two.fed(gzipped("Ariadne"))), std::string{"Ariadne"});
    OUGHT(two.ended());
    const Bytes oliver = gzipped("Oliver");

    SAME(tal::text::of(two.fed(Bytes(oliver.begin(), oliver.begin() + 12))), std::string{});
    OUGHT(!two.ended());

    const auto broken = [](Bytes given) {
        const std::string said = inflated(given, Wrapping::gzip);

        return said.starts_with("The compressed data is damaged: ") ? said.substr(32) : "not broken: " + said;
    };

    Bytes bad = gzipped("Miss Lemon");

    bad[0] = std::byte{0x1E};
    SAME(broken(bad), std::string{"not gzip."});

    bad = gzipped("Miss Lemon");
    bad[1] = std::byte{0x8C};
    SAME(broken(bad), std::string{"not gzip."});

    bad = gzipped("Miss Lemon");
    bad[2] = std::byte{7};
    SAME(broken(bad), std::string{"a gzip method other than deflate."});

    for (const std::uint8_t flag : {0x20, 0x40, 0x80}) SAME(broken(gzipped("x", flag)), std::string{"reserved gzip flags."});

    bad = gzipped("Miss Lemon");
    bad[bad.size() - 8] ^= std::byte{1};
    SAME(broken(bad), std::string{"the CRC-32 of a gzip member differs."});

    bad = gzipped("Miss Lemon");
    bad[bad.size() - 1] ^= std::byte{1};
    SAME(broken(bad), std::string{"the size of a gzip member differs."});

    SAME(broken(joined(gzipped("x"), Bytes{std::byte{0}})), std::string{"not gzip."});
    SAME(broken(joined(gzipped("x"), Bytes{std::byte{0x1F}, std::byte{0x8B}, std::byte{8}, std::byte{0}})), std::string{"not broken: x..."});

    const Bytes name(65536, std::byte{'n'});

    SAME(inflated(gzipped("y", 0x08, joined(name, Bytes{std::byte{0}})), Wrapping::gzip), std::string{"y"});
    SAME(broken(gzipped("y", 0x08, joined(joined(name, Bytes{std::byte{'n'}}), Bytes{std::byte{0}}))), std::string{"a gzip name or comment too long."});
    SAME(broken(gzipped("y", 0x10, joined(joined(name, Bytes{std::byte{'n'}}), Bytes{std::byte{0}}))), std::string{"a gzip name or comment too long."});
}

TRIAL("Inflate: codes of fifteen bits, the window to its last byte, and codes left incomplete") {
    Bits fifteen;

    fifteen.put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(15, 4);

    for (const std::uint32_t length : {0U, 0U, 5U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 5U}) fifteen.put(length, 3);

    fifteen.code(31, 5).put(54, 7);

    for (std::uint32_t length = 1; length <= 14; ++length) fifteen.code(length, 4);

    fifteen.code(30, 5).code(31, 5).put(127, 7).code(31, 5).put(27, 7).code(30, 5).code(1, 4);
    fifteen.code(0x7FFE, 15).code(0, 1).code(0x3FFE, 14).code(0x7FFF, 15);

    SAME(inflated(fifteen.done()), std::string{"OAN"});

    Bytes plain;

    for (std::size_t at = 0; at < 65534; ++at) plain.push_back(static_cast<std::byte>((at * 31) % 253));

    Bits far;

    far.put(0, 1).put(0, 2).bytes(little(65534 | ((65534U ^ 0xFFFFU) << 16))).bytes(plain);
    far.put(1, 1).put(1, 2).literal(257).code(29, 5).put(32768 - 24577, 13).literal(256);

    Bytes wanted = plain;

    for (std::size_t one = 0; one < 3; ++one) wanted.push_back(wanted[wanted.size() - 32768]);

    Inflater windowed{Wrapping::raw};

    SAME(windowed.fed(far.done()), wanted);
    OUGHT(windowed.ended());

    // The code of code lengths: 0, 1, 2 and 18 of two bits each, so 00, 01, 10 and 11.
    const auto header = [](Bits& bits) -> Bits& {
        bits.put(1, 1).put(2, 2);

        return bits;
    };
    const auto lengthy = [](Bits& bits) -> Bits& {
        bits.put(14, 4);

        for (const std::uint32_t length : {0U, 0U, 2U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 0U, 2U}) bits.put(length, 3);

        return bits;
    };

    {
        Bits bits;

        header(bits).put(0, 5).put(0, 5);
        lengthy(bits).code(3, 2).put(54, 7).code(2, 2).code(3, 2).put(127, 7).code(3, 2).put(41, 7).code(2, 2).code(1, 2);
        bits.code(0, 2).code(0, 2).code(1, 2);

        SAME(inflated(bits.done()), std::string{"The compressed data is damaged: the code of lengths is not complete."});
    }
    {
        Bits bits;

        header(bits).put(0, 5).put(1, 5);
        lengthy(bits).code(3, 2).put(54, 7).code(1, 2).code(3, 2).put(127, 7).code(3, 2).put(41, 7).code(1, 2).code(2, 2).code(2, 2);
        bits.code(0, 1).code(1, 1);

        SAME(inflated(bits.done()), std::string{"The compressed data is damaged: the code of distances is not complete."});
    }
    {
        Bits bits;

        header(bits).put(0, 5).put(0, 5);
        lengthy(bits).code(3, 2).put(54, 7).code(1, 2).code(3, 2).put(127, 7).code(3, 2).put(41, 7).code(1, 2).code(1, 2);
        bits.code(0, 1).code(0, 1).code(1, 1);

        SAME(inflated(bits.done()), std::string{"AA"});
    }
}

TRIAL("Inflate: gzip's extra field of more than a byte, and a trailer after a block that ends inside a byte") {
    Bytes extra{std::byte{0x2C}, std::byte{0x01}};

    extra.insert(extra.end(), 300, std::byte{'e'});

    SAME(inflated(gzipped("Felicity", 0x04, extra), Wrapping::gzip), std::string{"Felicity"});

    Bytes made{std::byte{0x1F}, std::byte{0x8B}, std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3}};
    const Bytes body = Bits{}.put(1, 1).put(1, 2).literal('H').literal('i').literal(256).done();

    made = joined(joined(joined(made, body), little(tal::inflating::crc32(0, tal::bytes("Hi")))), little(2));

    SAME(inflated(made, Wrapping::gzip), std::string{"Hi"});
}

// zlib's wrapping, as PDF streams and compressed ID3 frames carry it: its two bytes checked
// (deflate, a window of at most 32 KiB, the check by 31, no preset dictionary), the Adler-32 after
// the last block, and nothing after it.
TRIAL("Inflate: zlib checks its two bytes and its Adler-32, whole and byte by byte, and nothing after") {
    const auto said = [](const Bytes& given) {
        try {
            Inflater one{Wrapping::zlib};
            const Bytes made = one.fed(given);

            return std::string{one.ended() ? "ended " : "open "} + std::to_string(made.size());
        } catch (const tal::Trouble& why) {
            return std::string{tal::spoken(why.kind())} + ": " + why.what();
        }
    };
    const Bytes whole = unhexed(zlib_vector);
    Inflater piecemeal{Wrapping::zlib};
    Bytes pieces;

    for (const std::byte one : whole) {
        const Bytes got = piecemeal.fed(std::span<const std::byte>{&one, 1});

        pieces.insert(pieces.end(), got.begin(), got.end());
    }

    OUGHT(piecemeal.ended());
    OUGHT(pieces == cells());
    SAME(said(whole), "ended " + std::to_string(cells().size()));
    SAME(said(Bytes(whole.begin(), whole.end() - 1)), "open " + std::to_string(cells().size()));

    Bytes checked = whole;
    Bytes summed = whole;
    Bytes after = whole;

    checked[1] = std::byte{0xDB};
    summed.back() = std::byte{0x50};
    after.push_back(std::byte{0});

    SAME(said(checked), std::string{"malformed: The compressed data is damaged: the check of a zlib header differs."});
    SAME(said(summed), std::string{"malformed: The compressed data is damaged: the Adler-32 of a zlib stream differs."});
    SAME(said(after), std::string{"malformed: The compressed data is damaged: something follows the end."});
    SAME(said(unhexed("7918")), std::string{"malformed: The compressed data is damaged: not zlib deflate."});
    SAME(said(unhexed("881c")), std::string{"malformed: The compressed data is damaged: not zlib deflate."});
    SAME(said(unhexed("782000000000")), std::string{"malformed: The compressed data is damaged: a zlib preset dictionary."});
    SAME(said(unhexed("78")), std::string{"open 0"});
}

// With the rest left, as PDF needs it, what follows the end of a zlib stream (an end of line some
// writers count into the length) is neither read nor refused, whether it comes with the stream or
// after it; damage before the end is refused all the same, and what came out before it stays in a
// buffer of the caller's.
TRIAL("Inflate: zlib with the rest left ends at its Adler-32 and leaves what follows") {
    Bytes after = unhexed(zlib_vector);

    after.push_back(std::byte{'\r'});
    after.push_back(std::byte{'\n'});

    Inflater lenient{Wrapping::zlib, Rest::left};

    OUGHT(lenient.fed(after) == cells());
    OUGHT(lenient.ended());
    OUGHT(lenient.fed(tal::bytes("more")).empty());
    OUGHT(lenient.ended());

    Bytes summed = unhexed(zlib_vector);

    summed.back() = std::byte{0x50};

    Inflater checked{Wrapping::zlib, Rest::left};

    THREW((void)checked.fed(summed));

    // Fed into a buffer of the caller's, what was decoded before the damage stays there.
    Inflater appending{Wrapping::zlib};
    Bytes kept;

    THREW(appending.fed(summed, kept));
    OUGHT(kept == cells());
}

// With the rest left, raw deflate too ends at its last block and leaves what follows; and the
// appending `fed` adds to what the caller's buffer held, piece after piece.
TRIAL("Inflate: raw deflate with the rest left, and the appending fed keeps what the buffer held") {
    Bytes after = unhexed(fixed_vector);

    after.push_back(std::byte{'\r'});
    after.push_back(std::byte{'\n'});

    Inflater raw{Wrapping::raw, Rest::left};

    OUGHT(raw.fed(after) == cells());
    OUGHT(raw.ended());

    const Bytes whole = unhexed(zlib_vector);
    Inflater appending{Wrapping::zlib};
    Bytes kept = tal::bytes("kept ");

    appending.fed(std::span<const std::byte>{whole}.subspan(0, whole.size() / 2), kept);
    appending.fed(std::span<const std::byte>{whole}.subspan(whole.size() / 2), kept);

    OUGHT(appending.ended());
    OUGHT(kept == joined(tal::bytes("kept "), cells()));
}

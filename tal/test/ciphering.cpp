#include "ciphering.hpp"

#include <string>
#include <string_view>

#include "tal/bytes.hpp"
#include "tal/testing.hpp"

// What every cipher of the port does, whoever brings it: the built-in one of Windows in tal's
// trials, OpenSSL in the adapters'. X25519 against RFC 7748, AES-256-GCM against the vectors of
// its specification, and refusals of whatever was changed.
namespace tal::testing {

namespace {

// The bytes of the hexadecimal digits `text`.
Bytes unhexed(std::string_view text) {
    Bytes made;

    for (std::size_t at = 0; at + 1 < text.size(); at += 2) made.push_back(static_cast<std::byte>(std::stoi(std::string{text.substr(at, 2)}, nullptr, 16)));

    return made;
}

// `what` with its byte `at` changed.
Bytes flipped(Bytes what, std::size_t at) {
    what.at(at) ^= std::byte{1};

    return what;
}

}

// Runs every trial of the port on `cipher`; a failure stops at the first.
void ciphered(crypto::Cipher& cipher) {
    const Bytes alice = unhexed("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Bytes bob = unhexed("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    const Bytes alice_public = unhexed("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    const Bytes bob_public = unhexed("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    const Bytes shared = unhexed("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    const auto ours = cipher.agreement(alice);
    const auto theirs = cipher.agreement(bob);

    SAME(ours->mine(), alice_public);
    SAME(theirs->mine(), bob_public);
    SAME(ours->shared(bob_public).value(), shared);
    SAME(theirs->shared(alice_public).value(), shared);
    OUGHT(!ours->shared(Bytes(32)).has_value());
    OUGHT(!ours->shared(Bytes(31)).has_value());
    THREW((void)cipher.agreement(Bytes(31)));

    const auto one = cipher.agreement();
    const auto two = cipher.agreement();

    SAME(one->mine().size(), std::size_t{32});
    OUGHT(one->mine() != two->mine());
    SAME(one->shared(two->mine()).value(), two->shared(one->mine()).value());

    const Bytes zeros(32);
    const Bytes nonce(12);

    SAME(cipher.sealed(zeros, nonce, {}, {}), unhexed("530f8afbc74536b9a963b4f1c4cb738b"));
    SAME(cipher.sealed(zeros, nonce, {}, Bytes(16)), unhexed("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"));

    const Bytes key = unhexed("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const Bytes iv = unhexed("cafebabefacedbaddecaf888");
    const Bytes plain = unhexed("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    const Bytes extra = unhexed("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    const Bytes sealed = unhexed("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f66276fc6ece0f4e1768cddf8853bb2d551b");

    SAME(cipher.sealed(key, iv, extra, plain), sealed);
    SAME(cipher.opened(key, iv, extra, sealed).value(), plain);
    SAME(cipher.opened(zeros, nonce, {}, unhexed("530f8afbc74536b9a963b4f1c4cb738b")).value(), Bytes{});
    OUGHT(!cipher.opened(key, iv, extra, flipped(sealed, 0)).has_value());
    OUGHT(!cipher.opened(key, iv, extra, flipped(sealed, sealed.size() - 1)).has_value());
    OUGHT(!cipher.opened(flipped(key, 0), iv, extra, sealed).has_value());
    OUGHT(!cipher.opened(key, flipped(iv, 0), extra, sealed).has_value());
    OUGHT(!cipher.opened(key, iv, flipped(extra, 0), sealed).has_value());
    OUGHT(!cipher.opened(key, iv, {}, sealed).has_value());
    OUGHT(!cipher.opened(key, iv, extra, Bytes(15)).has_value());
    THREW((void)cipher.sealed(Bytes(16), iv, {}, plain));
    THREW((void)cipher.sealed(key, Bytes(16), {}, plain));
    THREW((void)cipher.opened(Bytes(16), iv, {}, sealed));
}

}

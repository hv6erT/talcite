#include "tal/testing.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <iostream>
#include <string>
#include <utility>

#include "talcite/digest.hpp"
#include "talcite/random.hpp"
#include "talcite/trouble.hpp"

#include "stunning.hpp"

using namespace talcite;

// Randomness and digests come from the system, not from any library and not from our own code.
// Randomness is available everywhere; digests can be blocked by a system-call filter in a
// container, and there the trials on test vectors skip themselves instead of failing, because a
// missing system capability is not a defect of the library. `summing()` says plainly which case we
// are in.
namespace {


// Spells bytes as lowercase hexadecimal, two digits per byte, so that digests compare against the
// published vectors.
std::string spelled(const Bytes& what) {
    static const char* signs = "0123456789abcdef";

    std::string made;

    for (const std::byte one : what) {
        const unsigned sign = std::to_integer<unsigned>(one);

        made += signs[sign >> 4];
        made += signs[sign & 0xF];
    }

    return made;
}

// Tells whether this system gives no digests; if so, it prints a note that the trial is skipped.
bool missing() {
    if (summing()) return false;

    std::cout << "        this system provides no digests; trial skipped\n";

    return true;
}

}

TRIAL("Randomness: returns as many bytes as requested") {
    SAME(random(0).size(), static_cast<std::size_t>(0));
    SAME(random(1).size(), static_cast<std::size_t>(1));
    SAME(random(12).size(), static_cast<std::size_t>(12));

    // A large request must not be truncated: getrandom without flags returns at most 32 MiB at once
    // and may return fewer bytes than requested, so the loop that fetches the rest has to be there.
    SAME(random(70000).size(), static_cast<std::size_t>(70000));
}

TRIAL("Randomness: two calls differ") {
    const Bytes one = random(32);
    const Bytes two = random(32);

    OUGHT(!(one == two));
}

// This is not a test of the quality of the randomness (no test can check that), only that something
// mixes at all. Thirty-two zero bytes would mean that nobody wrote anything.
TRIAL("Randomness: does not return only zeros") {
    bool something = false;

    for (const std::byte one : random(32))
        if (one != std::byte{0}) something = true;

    OUGHT(something);
}

// The well-known CRC-32 check value: the checksum of "123456789" is 0xCBF43926. This is a checksum,
// not cryptography, so we compute it ourselves and check it completely.
TRIAL("Checksum: CRC-32 matches the check value") {
    SAME(net::tallied(bytes("123456789"), 9), 0xCBF43926u);
    SAME(net::tallied(Bytes{}, 0), 0u);
}

TRIAL("Digest: SHA-1 matches the test vector") {
    if (missing()) return;

    SAME(spelled(summed(Sum::sha1, bytes("abc"))),
         std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});

    SAME(spelled(summed(Sum::sha1, Bytes{})),
         std::string{"da39a3ee5e6b4b0d3255bfef95601890afd80709"});
}

TRIAL("Digest: MD5 matches the test vector") {
    if (missing()) return;

    SAME(spelled(summed(Sum::md5, bytes("abc"))), std::string{"900150983cd24fb0d6963f7d28e17f72"});

    SAME(spelled(summed(Sum::md5, Bytes{})), std::string{"d41d8cd98f00b204e9800998ecf8427e"});
}

TRIAL("Digest: SHA-256 matches the test vector") {
    if (missing()) return;

    SAME(spelled(summed(Sum::sha256, bytes("abc"))),
         std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
}

TRIAL("Digest: SHA-512 matches the test vector") {
    if (missing()) return;

    SAME(spelled(summed(Sum::sha512, bytes("abc"))),
         std::string{"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"});
}

// RFC 4231, test case 2.
TRIAL("Signature: HMAC-SHA-512 matches the RFC 4231 vector") {
    if (missing()) return;

    SAME(spelled(stamped(Sum::sha512, bytes("Jefe"), bytes("what do ya want for nothing?"))),
         std::string{"164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
                     "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"});
}

// RFC 5869, test cases 1 and 3: with salt and info, then without both (the salt is zeros).
TRIAL("Derivation: HKDF matches the RFC 5869 vectors") {
    if (missing()) return;

    Bytes salt;

    for (int at = 0; at <= 0x0c; ++at) salt.push_back(static_cast<std::byte>(at));

    Bytes info;

    for (int at = 0xf0; at <= 0xf9; ++at) info.push_back(static_cast<std::byte>(at));

    SAME(spelled(derived(Sum::sha256, salt, Bytes(22, std::byte{0x0b}), info, 42)),
         std::string{"3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                     "34007208d5b887185865"});

    SAME(spelled(derived(Sum::sha256, {}, Bytes(22, std::byte{0x0b}), {}, 42)),
         std::string{"8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                     "9d201395faa4b61a96c8"});

    // A longer derivation starts with the shorter one: the blocks come in order.
    const Bytes longer = derived(Sum::sha512, {}, bytes("x"), {}, 100);

    SAME(longer.size(), std::size_t{100});
    OUGHT(derived(Sum::sha512, {}, bytes("x"), {}, 64) == Bytes(longer.begin(), longer.begin() + 64));
    THREW((void)derived(Sum::sha256, {}, bytes("x"), {}, 255 * 32 + 1));
}

// RFC 2202, test cases 1 and 2.
TRIAL("Signature: HMAC-SHA1 matches the RFC 2202 vectors") {
    if (missing()) return;

    SAME(spelled(stamped(Sum::sha1, Bytes(20, std::byte{0x0b}), bytes("Hi There"))),
         std::string{"b617318655057264e28bc0b6fb378c8ef146be00"});

    SAME(spelled(stamped(Sum::sha1, bytes("Jefe"), bytes("what do ya want for nothing?"))),
         std::string{"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"});
}

// The TURN long-term key is the digest of the user name, realm and password joined with colons.
TRIAL("Signature: the long-term key is the digest of the joined parts") {
    if (missing()) return;

    SAME(spelled(net::keyed("user", "realm", "pass")),
         spelled(summed(Sum::md5, bytes("user:realm:pass"))));

    SAME(net::keyed("user", "realm", "pass").size(), static_cast<std::size_t>(16));
}

// A signed message must verify with its own key and must not verify with someone else's. The trap
// here is the length field: the signature is computed over content whose length already pretends
// that the signature attribute is present. Computed any other way, it would not agree with any
// peer.
TRIAL("Signature: a message verifies with its own key") {
    if (missing()) return;

    const Bytes key = bytes("tajemnica");

    Bytes made = net::asked(net::numbered());

    net::texting(made, 0x0006, "kto");

    net::stamping(made, key);

    OUGHT(net::trusted(made, key));
    OUGHT(!net::trusted(made, bytes("inna")));

    // Changing even one byte of the content invalidates the signature.
    Bytes other = made;

    other[24] = ~other[24];

    OUGHT(!net::trusted(other, key));
}

TRIAL("Signature: a message without a signature is not trusted") {
    if (missing()) return;

    const Bytes made = net::asked(net::numbered());

    OUGHT(!net::trusted(made, bytes("tajemnica")));
}

// The fingerprint itself needs no digests (it is a checksum), so this trial runs everywhere and
// guards the length-field trap: the fingerprint is computed over content whose length already
// pretends that the fingerprint attribute is present.
TRIAL("Fingerprint: the fingerprint alone is correct without any digests") {
    Bytes made = net::asked(net::numbered());

    net::printing(made);

    SAME(made.size(), static_cast<std::size_t>(28));

    // The length field must count everything after the header, including the fingerprint.
    SAME(static_cast<unsigned>(std::to_integer<unsigned>(made[2]) << 8 |
                               std::to_integer<unsigned>(made[3])),
         8u);

    const std::uint32_t sum = net::tallied(made, made.size() - 8) ^ 0x5354554Eu;

    const std::size_t at = made.size() - 4;

    SAME(static_cast<unsigned>(std::to_integer<unsigned>(made[at]) << 24 |
                               std::to_integer<unsigned>(made[at + 1]) << 16 |
                               std::to_integer<unsigned>(made[at + 2]) << 8 |
                               std::to_integer<unsigned>(made[at + 3])),
         sum);

    net::Message told;

    OUGHT(net::understood(made, told));
}

// The fingerprint goes at the very end and also covers the signature, so both must be correct at
// once.
TRIAL("Fingerprint: a message with a fingerprint and a signature is consistent") {
    if (missing()) return;

    const Bytes key = bytes("tajemnica");

    Bytes made = net::asked(net::numbered());

    net::stamping(made, key);
    net::printing(made);

    OUGHT(net::trusted(made, key));

    net::Message told;

    OUGHT(net::understood(made, told));

    // The fingerprint is checked the way the peer will check it: a checksum over everything before
    // it.
    const std::uint32_t sum = net::tallied(made, made.size() - 8) ^ 0x5354554Eu;

    const std::size_t at = made.size() - 4;

    SAME(static_cast<unsigned>(std::to_integer<unsigned>(made[at]) << 24 |
                               std::to_integer<unsigned>(made[at + 1]) << 16 |
                               std::to_integer<unsigned>(made[at + 2]) << 8 |
                               std::to_integer<unsigned>(made[at + 3])),
         sum);
}

// A digest fed in parts gives the same result as the whole, for every digest, with and without a
// key, and also when some part is empty or when there are no parts at all.
TRIAL("Digest: feeding in parts gives the same as the whole") {
    if (missing()) return;

    std::string whole;

    for (int at = 0; at < 300000; ++at) whole += static_cast<char>('a' + at % 26);

    for (const Sum which : {Sum::md5, Sum::sha1, Sum::sha256, Sum::sha512}) {
        Digest plain{which};
        Digest keyed{which, bytes("klucz")};

        for (std::size_t at = 0; at < whole.size(); at += 70001) {
            const Bytes part = bytes(whole.substr(at, 70001));

            plain.feed(part);
            plain.feed(Bytes{});
            keyed.feed(part);
        }

        SAME(spelled(plain.done()), spelled(summed(which, bytes(whole))));
        SAME(spelled(keyed.done()), spelled(stamped(which, bytes("klucz"), bytes(whole))));

        Digest empty{which};

        SAME(spelled(empty.done()), spelled(summed(which, Bytes{})));
    }
}

// A finished digest does not compute further: a second answer would be the digest of something
// other than what was asked. The same goes for a digest whose state was taken away by a move.
TRIAL("Digest: finished and moved-from digests refuse") {
    if (missing()) return;

    Digest one{Sum::sha1};

    one.feed(bytes("abc"));

    SAME(spelled(one.done()), std::string{"a9993e364706816aba3e25717850c26c9cd0d89d"});

    const auto refused = [](auto doing) {
        try {
            doing();
        } catch (const Trouble&) {
            return true;
        }

        return false;
    };

    OUGHT(refused([&] { (void)one.done(); }));
    OUGHT(refused([&] { one.feed(bytes("x")); }));

    Digest two{Sum::sha256};

    two.feed(bytes("ab"));

    Digest three = std::move(two);

    three.feed(bytes("c"));

    OUGHT(refused([&] { two.feed(bytes("x")); }));

    SAME(spelled(three.done()), std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
}

TRIAL("Digest: when the system provides no digests, it says so plainly") {
    if (summing()) {
        // Where digests are available, we check the opposite: it does not throw and returns the
        // right length.
        SAME(summed(Sum::sha1, Bytes{}).size(), static_cast<std::size_t>(20));

        return;
    }

    bool amiss = false;

    try {
        (void)summed(Sum::sha1, Bytes{});
    } catch (const Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);
}

namespace {

// A spare provider that computes a "digest" its own way: only bytes of the given value, in the
// length it is told. That shows who computed it, and that a wrong length does not pass.
class Pretend : public DigestProvider {
public:
    Pretend(std::byte value, std::size_t wide) : value_(value), wide_(wide) {}

    // The provider's name, as reported to whoever asks which provider is in use.
    [[nodiscard]] std::string_view name() const override { return "pretend digests"; }

    // Starts a digest that yields 0x77 bytes for a keyed digest and the configured value otherwise.
    [[nodiscard]] std::unique_ptr<Digesting> started(Sum, const Bytes* key) override {
        return std::make_unique<Running>(key != nullptr ? std::byte{0x77} : value_, wide_, fed_);
    }

    // How many bytes all digests started by this provider have been fed so far.
    [[nodiscard]] std::size_t fed() const { return *fed_; }

private:
    class Running : public Digesting {
    public:
        Running(std::byte value, std::size_t wide, std::shared_ptr<std::size_t> fed) : value_(value), wide_(wide), fed_(std::move(fed)) {}

        // Counts the fed bytes in the counter shared with the provider.
        void feed(const Bytes& what) override { *fed_ += what.size(); }

        // Returns the configured number of bytes of the configured value.
        [[nodiscard]] Bytes done() override { return Bytes(wide_, value_); }

    private:
        std::byte value_;
        std::size_t wide_;
        std::shared_ptr<std::size_t> fed_;
    };

    std::byte value_;
    std::size_t wide_;
    std::shared_ptr<std::size_t> fed_ = std::make_shared<std::size_t>(0);
};

}

// The spare provider takes over only when the system gives no digests. On a system that does, the
// system computes them and the spare changes nothing; where it does not (as on the machine where
// this trial was written), the spare computes them, and a digest of the wrong length is an
// exception.
TRIAL("Digest: the spare computes only when the system provides none") {
    const bool system = summing();

    const auto spare = std::make_shared<Pretend>(std::byte{0x11}, 32);

    spared(spare);

    OUGHT(summing());

    if (system) {
        SAME(spelled(summed(Sum::sha256, bytes("abc"))),
             std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
        SAME(spare->fed(), std::size_t{0});
    } else {
        SAME(summed(Sum::sha256, bytes("abc")), Bytes(32, std::byte{0x11}));
        SAME(stamped(Sum::sha256, bytes("klucz"), bytes("abc")), Bytes(32, std::byte{0x77}));
        SAME(spare->fed(), std::size_t{6});

        spared(std::make_shared<Pretend>(std::byte{0x11}, 31));

        THREW(static_cast<void>(summed(Sum::sha256, bytes("abc"))));
    }

    spared(nullptr);

    OUGHT(summing() == system);

    if (!system) THREW(static_cast<void>(summed(Sum::sha256, bytes("abc"))));
}

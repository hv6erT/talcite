#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#if defined(__linux__)
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ciphering.hpp"
#include "summing.hpp"

// The cryptography of tal::core: digests, HMAC and HKDF against the vectors of their norms, from
// every source this build and this system have (the system's, and our own where built in), the
// two compared on every length; MD5 and SHA-1 only when built in; randomness, base64, wiping and
// comparing in constant time.

namespace {

using tal::Bytes;
using tal::crypto::Sum;

namespace summing = tal::crypto::summing;

// `what` as hexadecimal digits.
std::string hex(const Bytes& what) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string made;

    for (const std::byte one : what) {
        made += digits[std::to_integer<unsigned>(one) >> 4];
        made += digits[std::to_integer<unsigned>(one) & 15];
    }

    return made;
}

// `much` bytes counting from `from`.
Bytes counted(std::size_t much, unsigned from = 0) {
    Bytes made(much);

    for (std::size_t at = 0; at < much; ++at) made[at] = static_cast<std::byte>((from + at) & 0xff);

    return made;
}

// The digests this build has: MD5 and SHA-1 only when built in.
std::vector<Sum> sums() {
#ifdef TAL_WEAK_SUMS
    return {Sum::md5, Sum::sha1, Sum::sha256, Sum::sha512};
#else
    return {Sum::sha256, Sum::sha512};
#endif
}

// A source of digests: its name and how it makes one, plain or keyed.
struct Source {
    std::string named;
    std::function<std::unique_ptr<summing::Machine>(Sum, summing::Key)> made;
};

// The sources this build and this system have: the system's, when it gives digests, and our own,
// when built in.
std::vector<Source> sources() {
    std::vector<Source> made;

    if (summing::system(Sum::sha256, std::nullopt) != nullptr) made.push_back(Source{"system", [](Sum which, summing::Key key) { return summing::system(which, key); }});

#ifdef TAL_OWN_SUMS
    made.push_back(Source{"own", [](Sum which, summing::Key key) { return summing::own(which, key); }});
#endif

    return made;
}

// What `source` gives for `what`, in one piece, keyed with `key` when given.
std::string through(const Source& source, Sum which, const Bytes& what, std::optional<Bytes> key = std::nullopt) {
    const std::unique_ptr<summing::Machine> one = key ? source.made(which, std::span<const std::byte>{*key}) : source.made(which, std::nullopt);

    one->feed(what);

    return hex(one->done());
}

// Whether this build on this system has digests at all; when it has none, making one says so as
// unsupported.
bool digesting() {
    try {
        (void)tal::crypto::Summing{Sum::sha256};
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::unsupported);

        return false;
    }

    return true;
}

// A vector of a norm: the digest, its content and what it gives.
struct Vector {
    Sum which;
    std::string content;
    std::string wanted;
};

// The vectors of FIPS 180-4 and RFC 1321: nothing, "abc", the two-block texts, a million "a".
std::vector<Vector> vectors() {
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::string four = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    const std::string million(1000000, 'a');

    return {
        {Sum::md5, "", "d41d8cd98f00b204e9800998ecf8427e"},
        {Sum::md5, "abc", "900150983cd24fb0d6963f7d28e17f72"},
        {Sum::md5, two, "8215ef0796a20bcaaae116d3876c664a"},
        {Sum::md5, four, "03dd8807a93175fb062dfb55dc7d359c"},
        {Sum::md5, million, "7707d6ae4e027c70eea2a935c2296f21"},
        {Sum::sha1, "", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {Sum::sha1, "abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {Sum::sha1, two, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {Sum::sha1, four, "a49b2446a02c645bf419f995b67091253a04a259"},
        {Sum::sha1, million, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
        {Sum::sha256, "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {Sum::sha256, "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {Sum::sha256, two, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {Sum::sha256, four, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
        {Sum::sha256, million, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
        {Sum::sha512, "", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"},
        {Sum::sha512, "abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
        {Sum::sha512, two, "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445"},
        {Sum::sha512, four, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"},
        {Sum::sha512, million, "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"},
    };
}

// Whether this build has the digest `which`.
bool built(Sum which) {
    for (const Sum one : sums())
        if (one == which) return true;

    return false;
}

// Whether the system should give digests here: CNG always, the kernel when it has AF_ALG.
bool offered() {
#if defined(_WIN32)
    return true;
#elif defined(__linux__)
    const int one = ::socket(AF_ALG, SOCK_SEQPACKET, 0);

    if (one < 0) return false;

    (void)::close(one);

    return true;
#else
    return false;
#endif
}

}

TRIAL("Digests: the system gives them wherever it has them") {
    SAME(summing::system(Sum::sha256, std::nullopt) != nullptr, offered());
    SAME(summing::system(Sum::sha256, tal::bytes("key")) != nullptr, offered());
}

TRIAL("Digests: the vectors of the norms, from every source there is") {
    for (const Source& source : sources())
        for (const Vector& one : vectors())
            if (built(one.which)) SAME(source.named + " " + through(source, one.which, tal::bytes(one.content)), source.named + " " + one.wanted);

    if (!digesting()) return;

    for (const Vector& one : vectors())
        if (built(one.which)) SAME(hex(tal::crypto::summed(one.which, tal::bytes(one.content))), one.wanted);
}

TRIAL("Digests: the content cut anywhere gives what one piece gives") {
    if (!digesting()) return;

    const Bytes all = counted(300);

    for (const Sum which : sums()) {
        const std::string whole = hex(tal::crypto::summed(which, all));

        for (std::size_t cut = 0; cut <= all.size(); cut += 7) {
            tal::crypto::Summing one{which};

            one.feed(std::span{all}.first(cut));
            one.feed({});
            one.feed(std::span{all}.subspan(cut));

            SAME(hex(one.done()), whole);
        }

        tal::crypto::Summing bytewise{which};

        for (const std::byte one : all) bytewise.feed(std::span<const std::byte>{&one, 1});

        SAME(hex(bytewise.done()), whole);
    }
}

#ifdef TAL_OWN_SUMS
TRIAL("Digests: the system and our own agree on every length, plain and keyed") {
    const std::vector<Source> both = sources();

    if (both.size() < 2) return;

    for (const Sum which : sums())
        for (std::size_t much = 0; much <= 300; ++much) {
            const Bytes what = counted(much, static_cast<unsigned>(much));

            SAME(through(both[0], which, what), through(both[1], which, what));

            for (const std::size_t key : {std::size_t{0}, std::size_t{20}, std::size_t{64}, std::size_t{65}, std::size_t{128}, std::size_t{129}})
                if (much % 37 == 0 || much < 3) SAME(through(both[0], which, what, counted(key, 7)), through(both[1], which, what, counted(key, 7)));
        }
}
#endif

TRIAL("HMAC: the vectors of RFC 2202 and 4231, a key longer than a block, and an empty key") {
    struct Case {
        Sum which;
        Bytes key;
        std::string content;
        std::string wanted;
    };

    const std::vector<Case> cases{
        {Sum::md5, Bytes(20, std::byte{0x0b}), "Hi There", "5ccec34ea9656392457fa1ac27f08fbc"},
        {Sum::md5, tal::bytes("Jefe"), "what do ya want for nothing?", "750c783e6ab0b503eaa86e310a5db738"},
        {Sum::md5, Bytes(131, std::byte{0xaa}), "Test Using Larger Than Block-Size Key - Hash Key First", "bfecaf4efff90a3a668f3922fec3762d"},
        {Sum::md5, {}, "", "74e6f7298a9c2d168935f58c001bad88"},
        {Sum::sha1, Bytes(20, std::byte{0x0b}), "Hi There", "b617318655057264e28bc0b6fb378c8ef146be00"},
        {Sum::sha1, tal::bytes("Jefe"), "what do ya want for nothing?", "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"},
        {Sum::sha1, Bytes(131, std::byte{0xaa}), "Test Using Larger Than Block-Size Key - Hash Key First", "90d0dace1c1bdc957339307803160335bde6df2b"},
        {Sum::sha1, {}, "", "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d"},
        {Sum::sha256, Bytes(20, std::byte{0x0b}), "Hi There", "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {Sum::sha256, tal::bytes("Jefe"), "what do ya want for nothing?", "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {Sum::sha256, Bytes(131, std::byte{0xaa}), "Test Using Larger Than Block-Size Key - Hash Key First",
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {Sum::sha256, {}, "", "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad"},
        {Sum::sha512, Bytes(20, std::byte{0x0b}), "Hi There",
         "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"},
        {Sum::sha512, tal::bytes("Jefe"), "what do ya want for nothing?",
         "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"},
        {Sum::sha512, Bytes(131, std::byte{0xaa}), "Test Using Larger Than Block-Size Key - Hash Key First",
         "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"},
        {Sum::sha512, {}, "", "b936cee86c9f87aa5d3c6f2e84cb5a4239a5fe50480a6ec66b70ab5b1f4ac6730c6c515421b327ec1d69402e53dfb49ad7381eb067b338fd7b0cb22247225d47"},
    };

    for (const Source& source : sources())
        for (const Case& one : cases)
            if (built(one.which)) SAME(source.named + " " + through(source, one.which, tal::bytes(one.content), one.key), source.named + " " + one.wanted);

    if (!digesting()) return;

    for (const Case& one : cases)
        if (built(one.which)) SAME(hex(tal::crypto::stamped(one.which, one.key, tal::bytes(one.content))), one.wanted);
}

TRIAL("HKDF: the vectors of RFC 5869, an empty salt, and no more than 255 digests") {
    if (!digesting()) return;

    SAME(hex(tal::crypto::derived(Sum::sha256, counted(13), Bytes(22, std::byte{0x0b}), counted(10, 0xf0), 42)),
         std::string{"3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"});
    SAME(hex(tal::crypto::derived(Sum::sha256, counted(80, 0x60), counted(80), counted(80, 0xb0), 82)),
         std::string{"b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c"
                     "58179ec3e87c14c01d5c1f3434f1d87"});
    SAME(hex(tal::crypto::derived(Sum::sha256, {}, Bytes(22, std::byte{0x0b}), {}, 42)),
         std::string{"8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"});
    SAME(hex(tal::crypto::derived(Sum::sha512, tal::bytes("salt"), tal::bytes("secret"), tal::bytes("info"), 100)),
         std::string{"90e269f053d383c4b2070be93238adf358f3d67bd7b17ca3de95f10a50a8385e66d8241c8c22c91cee75d7bda92e8111236b71b63c8f05973947428074d272f073de7d10"
                     "6ca1b6f5beeb22fc785a81666a9f00c675fdb5616858d14f472907a57fe282bb"});
    SAME(tal::crypto::derived(Sum::sha256, {}, tal::bytes("x"), {}, 0).size(), std::size_t{0});
    SAME(tal::crypto::derived(Sum::sha256, {}, tal::bytes("x"), {}, 255 * 32).size(), std::size_t{255 * 32});
    THREW((void)tal::crypto::derived(Sum::sha256, {}, tal::bytes("x"), {}, 255 * 32 + 1));
}

TRIAL("Digests: MD5 and SHA-1 only when built in; without digests each says unsupported") {
    for (const Sum which : {Sum::md5, Sum::sha1}) {
        std::optional<tal::Kind> kind;

        try {
            (void)tal::crypto::summed(which, tal::bytes("abc"));
        } catch (const tal::Trouble& why) {
            kind = why.kind();
        }

        if (built(which) && digesting()) OUGHT(!kind.has_value());
        else OUGHT(kind == tal::Kind::unsupported);
    }

    std::optional<tal::Kind> kind;

    try {
        (void)tal::crypto::stamped(Sum::sha256, tal::bytes("k"), tal::bytes("abc"));
    } catch (const tal::Trouble& why) {
        kind = why.kind();
    }

    OUGHT(digesting() ? !kind.has_value() : kind == tal::Kind::unsupported);
}

TRIAL("Digests: a digest is done once, takes nothing after, and a moved one is empty") {
    if (!digesting()) return;

    tal::crypto::Summing one{Sum::sha256};

    one.feed(tal::bytes("abc"));
    SAME(hex(one.done()), std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    THREW(one.feed(tal::bytes("x")));
    THREW((void)one.done());

    tal::crypto::Summing two{Sum::sha256};
    tal::crypto::Summing three = std::move(two);

    THREW(two.feed(tal::bytes("x")));
    SAME(hex(three.done()), std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
}

TRIAL("Randomness: as many bytes as asked, and two draws differ") {
    SAME(tal::crypto::random(0).size(), std::size_t{0});
    SAME(tal::crypto::random(1000).size(), std::size_t{1000});
    OUGHT(tal::crypto::random(32) != tal::crypto::random(32));
}

TRIAL("Base64: the vectors of RFC 4648 both ways; anything not strict is refused") {
    const std::vector<std::pair<std::string, std::string>> vectors{{"", ""},         {"f", "Zg=="},     {"fo", "Zm8="},        {"foo", "Zm9v"},
                                                                   {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};

    for (const auto& [plain, coded] : vectors) {
        SAME(tal::crypto::base64(tal::bytes(plain)), coded);
        SAME(tal::text::of(tal::crypto::unbase64(coded).value()), plain);
    }

    const Bytes every = counted(256);

    SAME(tal::crypto::unbase64(tal::crypto::base64(every)).value(), every);

    for (const std::string_view wrong : {"Zg=", "Zg=a", "Z===", "====", "Zg==Zg==", "Zm9v YmFy", "Zh==", "Zm9=", "Zm9\n", "Zm9!"})
        OUGHT(!tal::crypto::unbase64(wrong).has_value());
}

TRIAL("Secrets: wiping zeroes and keeps the length; the comparison sees every byte") {
    std::string word = "hasło";
    Bytes key = tal::bytes("klucz");

    tal::crypto::wiped(word);
    tal::crypto::wiped(key);

    SAME(word, std::string(6, '\0'));
    SAME(key, Bytes(5));

    OUGHT(tal::crypto::same(tal::bytes("abc"), tal::bytes("abc")));
    OUGHT(tal::crypto::same(Bytes{}, Bytes{}));
    OUGHT(!tal::crypto::same(tal::bytes("abc"), tal::bytes("abd")));
    OUGHT(!tal::crypto::same(tal::bytes("abc"), tal::bytes("xbc")));
    OUGHT(!tal::crypto::same(tal::bytes("abc"), tal::bytes("ab")));
    OUGHT(!tal::crypto::same(tal::bytes("ab"), tal::bytes("abc")));
}

TRIAL("Cipher: the built-in one passes the trials of the port; a build without one says so") {
    const std::shared_ptr<tal::crypto::Cipher> builtin = tal::crypto::Cipher::builtin();

    if (builtin == nullptr) {
        THREW(tal::Port<tal::crypto::Cipher>{});

        return;
    }

    tal::testing::ciphered(*builtin);
    tal::testing::ciphered(*tal::Port<tal::crypto::Cipher>{});
}

#include "tal/crypto.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "tal/trouble.hpp"

#ifndef BCRYPT_ECDH_ALGORITHM
#define BCRYPT_ECDH_ALGORITHM L"ECDH"
#endif

#ifndef BCRYPT_ECC_CURVE_NAME
#define BCRYPT_ECC_CURVE_NAME L"ECCCurveName"
#endif

#ifndef BCRYPT_ECC_CURVE_25519
#define BCRYPT_ECC_CURVE_25519 L"curve25519"
#endif

// The cipher of Windows, ported from talcite-adapters' cng: X25519 (Windows 10 from 1607) and
// AES-256-GCM, from CNG alone. A Windows without curve 25519 has no X25519 here; talcite's own
// ladder is gone with the decision to keep cryptography of that kind with the system.
//
// Byte order: CNG keeps the u coordinate and the scalar in its key blobs least significant byte
// first, as RFC 7748 writes them; the secret of `BCRYPT_KDF_RAW_SECRET` comes most significant byte
// first and is turned round. A key made of a given scalar has its public half computed by CNG
// itself, as the agreement of the scalar with the base point, since CNG wants both halves in a
// private blob.
namespace tal::crypto {

namespace {

constexpr std::size_t wide = 32;

constexpr ULONG tag = 16;

// The providers, opened once and stateless: ECDH on curve 25519 and AES in GCM; either is null
// when this Windows does not have it.
struct Handles {
    BCRYPT_ALG_HANDLE agreeing = nullptr;
    BCRYPT_ALG_HANDLE sealing = nullptr;
};

// The provider of `algorithm` with the property `property` set to `value`; null when the system
// does not have it.
BCRYPT_ALG_HANDLE opened(LPCWSTR algorithm, LPCWSTR property, std::wstring_view value) {
    BCRYPT_ALG_HANDLE made = nullptr;

    if (BCryptOpenAlgorithmProvider(&made, algorithm, nullptr, 0) < 0) return nullptr;

    if (BCryptSetProperty(made, property, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(value.data())),
                          static_cast<ULONG>((value.size() + 1) * sizeof(wchar_t)), 0) < 0) {
        (void)BCryptCloseAlgorithmProvider(made, 0);

        return nullptr;
    }

    return made;
}

// The providers, opened at the first use.
Handles& handles() {
    static Handles only;
    static std::once_flag once;

    std::call_once(once, [] {
        only.agreeing = opened(BCRYPT_ECDH_ALGORITHM, BCRYPT_ECC_CURVE_NAME, BCRYPT_ECC_CURVE_25519);
        only.sealing = opened(BCRYPT_AES_ALGORITHM, BCRYPT_CHAINING_MODE, BCRYPT_CHAIN_MODE_GCM);
    });

    return only;
}

// The provider of X25519; throws `Trouble` when this Windows has none.
BCRYPT_ALG_HANDLE agreeing() {
    BCRYPT_ALG_HANDLE made = handles().agreeing;

    if (made == nullptr) throw Trouble{Kind::unsupported, "This Windows has no X25519."};

    return made;
}

// The provider of AES-GCM; throws `Trouble` when this Windows has none.
BCRYPT_ALG_HANDLE sealing() {
    BCRYPT_ALG_HANDLE made = handles().sealing;

    if (made == nullptr) throw Trouble{Kind::unsupported, "This Windows has no AES-GCM."};

    return made;
}

// The bytes `one` as CNG takes them, which is without const even where it only reads.
PUCHAR raw(const Bytes& one) { return reinterpret_cast<PUCHAR>(const_cast<std::byte*>(one.data())); }

// A peer's u as CNG takes it: bit 255 cleared, as RFC 7748 says to ignore it, and a value from p up
// to 2^255 - 1 reduced modulo p = 2^255 - 19, which leaves only its lowest byte, less 0xed.
Bytes canonical(Bytes u) {
    u.back() &= std::byte{0x7f};

    const bool past = u.back() == std::byte{0x7f} && std::all_of(u.begin() + 1, u.end() - 1, [](std::byte one) { return one == std::byte{0xff}; }) &&
                      std::to_integer<unsigned>(u.front()) >= 0xedU;

    if (past) {
        const auto low = static_cast<std::byte>(std::to_integer<unsigned>(u.front()) - 0xedU);

        std::fill(u.begin(), u.end(), std::byte{0});
        u.front() = low;
    }

    return u;
}

// The base point of curve 25519, u = 9.
Bytes based() {
    Bytes made(wide);

    made.front() = std::byte{9};

    return made;
}

// A key blob of curve 25519 with the magic `magic`: the header, u in the place of X, zeros for Y,
// and the scalar `secret` after them for a private one.
Bytes blob(ULONG magic, const Bytes& u, const Bytes* secret) {
    BCRYPT_ECCKEY_BLOB head{};

    head.dwMagic = magic;
    head.cbKey = static_cast<ULONG>(wide);

    Bytes made(sizeof head + wide * (secret != nullptr ? 3 : 2));

    std::memcpy(made.data(), &head, sizeof head);
    std::memcpy(made.data() + sizeof head, u.data(), wide);

    if (secret != nullptr) std::memcpy(made.data() + sizeof head + 2 * wide, secret->data(), wide);

    return made;
}

// A key of CNG, destroyed with its holder.
struct Key {
    BCRYPT_KEY_HANDLE handle = nullptr;

    // Holds nothing yet.
    Key() = default;

    // Holds `given`.
    explicit Key(BCRYPT_KEY_HANDLE given) : handle(given) {}

    // Destroys the key.
    ~Key() {
        if (handle != nullptr) (void)BCryptDestroyKey(handle);
    }

    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

// A secret agreed by CNG, destroyed with its holder.
struct Secret {
    BCRYPT_SECRET_HANDLE handle = nullptr;

    // Holds nothing yet.
    Secret() = default;

    // Destroys the secret.
    ~Secret() {
        if (handle != nullptr) (void)BCryptDestroySecret(handle);
    }

    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
};

// The secret the key `key` agrees with the public u `theirs`, in the order of RFC 7748; nothing for
// a u of the wrong length, one CNG refuses, and a low-order point, whose secret is zero.
std::optional<Bytes> agreed(BCRYPT_KEY_HANDLE key, const Bytes& theirs) {
    if (theirs.size() != wide) return std::nullopt;

    Bytes given = blob(BCRYPT_ECDH_PUBLIC_GENERIC_MAGIC, canonical(theirs), nullptr);
    Key peer;

    if (BCryptImportKeyPair(agreeing(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer.handle, raw(given), static_cast<ULONG>(given.size()),
                            BCRYPT_NO_KEY_VALIDATION) < 0)
        return std::nullopt;

    Secret secret;

    if (BCryptSecretAgreement(key, peer.handle, &secret.handle, 0) < 0) return std::nullopt;

    Bytes made(wide);
    ULONG much = 0;

    if (BCryptDeriveKey(secret.handle, BCRYPT_KDF_RAW_SECRET, nullptr, raw(made), static_cast<ULONG>(made.size()), &much, 0) < 0 || much != wide)
        return std::nullopt;

    if (std::all_of(made.begin(), made.end(), [](std::byte one) { return one == std::byte{0}; })) return std::nullopt;

    std::reverse(made.begin(), made.end());

    return made;
}

// Our half of X25519, its secret kept by CNG.
class Mine final : public Agreement {
public:
    // The key `key`, whose public u is `u`.
    Mine(BCRYPT_KEY_HANDLE key, Bytes u) : key_(key), public_(std::move(u)) {}

    // Our public u.
    [[nodiscard]] Bytes mine() const override { return public_; }

    // The secret shared with the peer whose public u is `theirs`.
    [[nodiscard]] std::optional<Bytes> shared(const Bytes& theirs) const override { return agreed(key_.handle, theirs); }

private:
    Key key_;
    Bytes public_;
};

// The public u of the key `key`, as CNG exports it.
Bytes exported(BCRYPT_KEY_HANDLE key) {
    ULONG much = 0;

    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &much, 0) < 0) throw Trouble{Kind::broken, "Cannot read the X25519 key."};

    Bytes made(much);

    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, raw(made), much, &much, 0) < 0 || much < sizeof(BCRYPT_ECCKEY_BLOB) + wide)
        throw Trouble{Kind::broken, "Cannot read the X25519 key."};

    return Bytes(made.begin() + sizeof(BCRYPT_ECCKEY_BLOB), made.begin() + static_cast<std::ptrdiff_t>(sizeof(BCRYPT_ECCKEY_BLOB) + wide));
}

// Throws `Trouble` unless the key has 32 bytes (AES-256) and the nonce 12 (GCM).
void checked(const Bytes& key, const Bytes& nonce) {
    if (key.size() != 32) throw Trouble{Kind::other, "An AES-256 key has 32 bytes."};

    if (nonce.size() != 12) throw Trouble{Kind::other, "An AES-GCM nonce has 12 bytes."};
}

// The cipher of CNG.
class Windows final : public Cipher {
public:
    // Its name.
    [[nodiscard]] std::string_view name() const override { return "CNG"; }

    // A fresh key pair of X25519, made by CNG.
    [[nodiscard]] std::unique_ptr<Agreement> agreement() override {
        BCRYPT_KEY_HANDLE made = nullptr;

        if (BCryptGenerateKeyPair(agreeing(), &made, 255, 0) < 0) throw Trouble{Kind::broken, "Cannot make an X25519 key."};

        if (BCryptFinalizeKeyPair(made, 0) < 0) {
            (void)BCryptDestroyKey(made);

            throw Trouble{Kind::broken, "Cannot make an X25519 key."};
        }

        return std::make_unique<Mine>(made, exported(made));
    }

    // The key pair of the scalar `secret`, clamped as RFC 7748 says. CNG takes it in a private blob
    // with no public half, unchecked, and then gives the public half as its agreement with the
    // base point; the copies of the scalar are wiped.
    [[nodiscard]] std::unique_ptr<Agreement> agreement(const Bytes& secret) override {
        if (secret.size() != wide) throw Trouble{Kind::other, "An X25519 key has 32 bytes."};

        Bytes clamped = secret;

        clamped.front() &= std::byte{0xf8};
        clamped.back() &= std::byte{0x7f};
        clamped.back() |= std::byte{0x40};

        Bytes whole = blob(BCRYPT_ECDH_PRIVATE_GENERIC_MAGIC, Bytes(wide), &clamped);
        BCRYPT_KEY_HANDLE made = nullptr;
        const bool taken = BCryptImportKeyPair(agreeing(), nullptr, BCRYPT_ECCPRIVATE_BLOB, &made, raw(whole), static_cast<ULONG>(whole.size()),
                                               BCRYPT_NO_KEY_VALIDATION) >= 0;

        wiped(clamped);
        wiped(whole);

        if (!taken) throw Trouble{Kind::broken, "Cannot take the X25519 key."};

        std::optional<Bytes> u = agreed(made, based());

        if (!u) {
            (void)BCryptDestroyKey(made);

            throw Trouble{Kind::broken, "Cannot take the X25519 key."};
        }

        return std::make_unique<Mine>(made, std::move(*u));
    }

    // `plain` sealed with AES-256-GCM under `key` and `nonce`, `extra` authenticated with it: the
    // ciphertext and then the tag of 16 bytes.
    [[nodiscard]] Bytes sealed(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& plain) override {
        checked(key, nonce);

        Key handle;

        if (BCryptGenerateSymmetricKey(sealing(), &handle.handle, nullptr, 0, raw(key), static_cast<ULONG>(key.size()), 0) < 0)
            throw Trouble{Kind::broken, "Cannot start AES-GCM."};

        Bytes made(plain.size() + tag);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;

        BCRYPT_INIT_AUTH_MODE_INFO(info);

        info.pbNonce = raw(nonce);
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = extra.empty() ? nullptr : raw(extra);
        info.cbAuthData = static_cast<ULONG>(extra.size());
        info.pbTag = raw(made) + plain.size();
        info.cbTag = tag;

        ULONG much = 0;

        if (BCryptEncrypt(handle.handle, plain.empty() ? nullptr : raw(plain), static_cast<ULONG>(plain.size()), &info, nullptr, 0,
                          plain.empty() ? nullptr : raw(made), static_cast<ULONG>(plain.size()), &much, 0) < 0)
            throw Trouble{Kind::broken, "Cannot seal."};

        return made;
    }

    // What `sealed` made, opened with `key`, `nonce` and `extra`; nothing when it is shorter than
    // the tag or anything was changed, the tag included.
    [[nodiscard]] std::optional<Bytes> opened(const Bytes& key, const Bytes& nonce, const Bytes& extra, const Bytes& sealed) override {
        checked(key, nonce);

        if (sealed.size() < tag) return std::nullopt;

        const std::size_t body = sealed.size() - tag;
        Key handle;

        if (BCryptGenerateSymmetricKey(sealing(), &handle.handle, nullptr, 0, raw(key), static_cast<ULONG>(key.size()), 0) < 0)
            throw Trouble{Kind::broken, "Cannot start AES-GCM."};

        Bytes made(body);
        Bytes expected(sealed.begin() + static_cast<std::ptrdiff_t>(body), sealed.end());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;

        BCRYPT_INIT_AUTH_MODE_INFO(info);

        info.pbNonce = raw(nonce);
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = extra.empty() ? nullptr : raw(extra);
        info.cbAuthData = static_cast<ULONG>(extra.size());
        info.pbTag = raw(expected);
        info.cbTag = tag;

        ULONG much = 0;

        if (BCryptDecrypt(handle.handle, body == 0 ? nullptr : raw(sealed), static_cast<ULONG>(body), &info, nullptr, 0, body == 0 ? nullptr : raw(made),
                          static_cast<ULONG>(body), &much, 0) < 0)
            return std::nullopt;

        return made;
    }
};

}

// The cipher of CNG, built into tal on Windows.
std::shared_ptr<Cipher> Cipher::builtin() { return std::make_shared<Windows>(); }

}

#endif

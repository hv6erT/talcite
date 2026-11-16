#include "tal/tls.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#ifndef CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
#define CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
#endif

#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS
#endif

#include <windows.h>

#include <wincrypt.h>

#include <subauth.h>

#include <schannel.h>

#include <ncrypt.h>
#include <security.h>

// TLS of Windows, Schannel, ported from talcite-adapters' schannel. Browsers and the system go the
// same way: the roots, their revocation and the fetching of missing ones belong to Windows. Like
// OpenSSL it never sees a socket: bytes go in and out in memory, and tal does the rest. TLS 1.2 or
// newer. A client checks the server's certificate itself, the same way for the Windows store and
// for roots given: a chain to a trusted root, fit for server authentication, for the name (or
// address) asked for; revocation only with the store, leniently, rejecting only what is revoked.
// A server sends the intermediate authorities of its identity only from the stores of Windows, so
// those of the bundle wait in the user's store of intermediate authorities while the adapter lives;
// that store makes nobody trusted.
namespace tal::net {

namespace {

// SECURITY_FLAG_IGNORE_REVOCATION of wininet.h, the one thing needed from there.
constexpr DWORD revocation_ignored = 0x00000080;

// How long the lists and answers of revocation may be waited for, in milliseconds.
constexpr DWORD revocation_patience = 5000;

// The code `code` as Windows documents its codes: 0x and eight hexadecimal digits.
std::string hexed(unsigned long code) {
    char made[16]{};

    (void)std::snprintf(made, sizeof made, "0x%08lX", code);

    return made;
}

// The trouble of the kind `kind` saying `what`, with the status `status`.
Trouble described(Kind kind, std::string_view what, SECURITY_STATUS status) {
    return Trouble{kind, std::string{what} + " (status " + hexed(static_cast<unsigned long>(status)) + ").", {}, static_cast<int>(status)};
}

// The kind of a refused handshake: no common version or cipher, as Schannel says it or as a
// server's alert comes back, is unsupported; the rest is broken.
Kind refusal(SECURITY_STATUS status) {
    if (status == SEC_E_ALGORITHM_MISMATCH || status == SEC_E_UNSUPPORTED_FUNCTION || status == SEC_E_ILLEGAL_MESSAGE) return Kind::unsupported;

    return Kind::broken;
}

// The UTF-8 text `text` as UTF-16.
std::wstring widened(const std::string& text) {
    if (text.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring made(static_cast<std::size_t>(size), L'\0');

    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), made.data(), size);

    return made;
}

// A store of certificates, closed with its holder.
struct Store {
    HCERTSTORE held = nullptr;

    // Closes the store.
    ~Store() {
        if (held != nullptr) (void)CertCloseStore(held, 0);
    }
};

// A certificate, let go with its holder.
struct Certificate {
    PCCERT_CONTEXT held = nullptr;

    // Lets the certificate go.
    ~Certificate() {
        if (held != nullptr) (void)CertFreeCertificateContext(held);
    }
};

// The intermediate authorities this program put in the user's store, by their encoding, with how
// many adapters need each; those that were there already are not counted and stay.
struct Lent {
    std::mutex guard;
    std::map<std::string, int> held;
};

// The authorities lent, for all the adapters of the program.
Lent& lent() {
    static Lent only;

    return only;
}

// The user's store of intermediate authorities, opened; none when it cannot be.
HCERTSTORE authorities() {
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"CA");
}

// The encoding of `one` as a key.
std::string encoded(PCCERT_CONTEXT one) { return std::string{reinterpret_cast<const char*>(one->pbCertEncoded), one->cbCertEncoded}; }

// Puts `one` in the user's store of intermediate authorities, unless it is there; whether it is
// counted as ours.
bool lending(PCCERT_CONTEXT one) {
    Lent& all = lent();
    const std::lock_guard<std::mutex> kept{all.guard};
    const std::string key = encoded(one);

    if (const auto found = all.held.find(key); found != all.held.end()) {
        ++found->second;

        return true;
    }

    Store store;

    store.held = authorities();

    if (store.held == nullptr) return false;

    if (!CertAddEncodedCertificateToStore(store.held, X509_ASN_ENCODING, one->pbCertEncoded, one->cbCertEncoded, CERT_STORE_ADD_NEW, nullptr)) return false;

    all.held.emplace(key, 1);

    return true;
}

// Gives back the authority of the encoding `key` that `lending` counted; the last adapter that
// needed it takes it out of the user's store.
void returned(const std::string& key) {
    Lent& all = lent();
    const std::lock_guard<std::mutex> kept{all.guard};
    const auto found = all.held.find(key);

    if (found == all.held.end() || --found->second > 0) return;

    all.held.erase(found);

    Store store;

    store.held = authorities();

    if (store.held == nullptr) return;

    const auto* bytes = reinterpret_cast<const BYTE*>(key.data());
    Certificate wanted;

    wanted.held = CertCreateCertificateContext(X509_ASN_ENCODING, bytes, static_cast<DWORD>(key.size()));

    if (wanted.held == nullptr) return;

    if (PCCERT_CONTEXT there = CertFindCertificateInStore(store.held, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, wanted.held, nullptr); there != nullptr)
        (void)CertDeleteCertificateFromStore(there);
}

// Whether `one` signed itself: a root, which a server does not send.
bool rooted(PCCERT_CONTEXT one) { return CertCompareCertificateName(X509_ASN_ENCODING, &one->pCertInfo->Subject, &one->pCertInfo->Issuer) != FALSE; }

// A chain of certificates, let go with its holder.
struct Chain {
    PCCERT_CHAIN_CONTEXT held = nullptr;

    // Lets the chain go.
    ~Chain() {
        if (held != nullptr) CertFreeCertificateChain(held);
    }
};

// Adds the certificates of the PEM text `text` to `into`, each block on its own, since Windows
// reads only the first; how many were added.
int gathered(HCERTSTORE into, const std::string& text) {
    static constexpr std::string_view opening = "-----BEGIN CERTIFICATE-----";
    static constexpr std::string_view closing = "-----END CERTIFICATE-----";
    int added = 0;

    for (std::size_t from = 0;;) {
        const std::size_t begins = text.find(opening, from);

        if (begins == std::string::npos) return added;

        const std::size_t ends = text.find(closing, begins);

        if (ends == std::string::npos) return added;

        from = ends + closing.size();

        const std::string block = text.substr(begins, from - begins);
        Bytes der(block.size());
        auto size = static_cast<DWORD>(der.size());

        if (!CryptStringToBinaryA(block.c_str(), static_cast<DWORD>(block.size()), CRYPT_STRING_BASE64HEADER, reinterpret_cast<BYTE*>(der.data()), &size,
                                  nullptr, nullptr))
            continue;

        if (CertAddEncodedCertificateToStore(into, X509_ASN_ENCODING, reinterpret_cast<const BYTE*>(der.data()), size, CERT_STORE_ADD_ALWAYS, nullptr))
            ++added;
    }
}

// Acquires the credentials of one side into `into`: the new structure, which knows TLS 1.3, and
// the old one with TLS 1.2 where the system takes only that. Old versions are turned off by name,
// so that a new one comes with Windows. A client checks certificates itself.
SECURITY_STATUS acquired(bool client, PCCERT_CONTEXT identity, CredHandle& into) {
    const DWORD flags = SCH_USE_STRONG_CRYPTO | (client ? SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS : 0);
    const ULONG direction = client ? SECPKG_CRED_OUTBOUND : SECPKG_CRED_INBOUND;
    PCCERT_CONTEXT given[1] = {identity};
    TLS_PARAMETERS limits{};

    limits.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3 | SP_PROT_TLS1_0 | SP_PROT_TLS1_1;

    SCH_CREDENTIALS fresh{};

    fresh.dwVersion = SCH_CREDENTIALS_VERSION;
    fresh.dwFlags = flags;
    fresh.cTlsParameters = 1;
    fresh.pTlsParameters = &limits;

    if (identity != nullptr) {
        fresh.cCreds = 1;
        fresh.paCred = given;
    }

    SECURITY_STATUS status =
        AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), direction, nullptr, &fresh, nullptr, nullptr, &into, nullptr);

    if (status == SEC_E_OK) return status;

    SCHANNEL_CRED old{};

    old.dwVersion = SCHANNEL_CRED_VERSION;
    old.dwFlags = flags;
    old.grbitEnabledProtocols = client ? SP_PROT_TLS1_2_CLIENT : SP_PROT_TLS1_2_SERVER;

    if (identity != nullptr) {
        old.cCreds = 1;
        old.paCred = given;
    }

    status = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), direction, nullptr, &old, nullptr, nullptr, &into, nullptr);

    return status;
}

// Whether `one` issued another certificate of `from`: an authority of the chain, not the identity.
bool issuing(HCERTSTORE from, PCCERT_CONTEXT one) {
    PCCERT_CONTEXT other = nullptr;

    while ((other = CertEnumCertificatesInStore(from, other)) != nullptr) {
        if (CertCompareCertificate(X509_ASN_ENCODING, one->pCertInfo, other->pCertInfo)) continue;

        if (CertCompareCertificateName(X509_ASN_ENCODING, &one->pCertInfo->Subject, &other->pCertInfo->Issuer)) {
            (void)CertFreeCertificateContext(other);

            return true;
        }
    }

    return false;
}

// The identity of a PKCS#12 bundle: a certificate with a key that issued no other in the bundle,
// since Wine gives the key to every one; the key is asked for, as Wine knows no search by key.
PCCERT_CONTEXT keyed(HCERTSTORE from) {
    PCCERT_CONTEXT one = nullptr;

    while ((one = CertEnumCertificatesInStore(from, one)) != nullptr) {
        if (issuing(from, one)) continue;

        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
        DWORD spec = 0;
        BOOL owned = FALSE;

        if (CryptAcquireCertificatePrivateKey(one, CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ALLOW_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_CACHE_FLAG, nullptr,
                                              &key, &spec, &owned))
            return one;
    }

    return nullptr;
}

// Removes the key of `one` from the user's store of keys, through CNG or the old CryptoAPI,
// wherever Windows put it when reading the bundle.
void forgotten(PCCERT_CONTEXT one) {
    DWORD size = 0;

    if (!CertGetCertificateContextProperty(one, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size)) return;

    std::vector<std::byte> held(size);
    auto* info = reinterpret_cast<CRYPT_KEY_PROV_INFO*>(held.data());

    if (!CertGetCertificateContextProperty(one, CERT_KEY_PROV_INFO_PROP_ID, info, &size)) return;

    if (info->dwProvType != 0) {
        HCRYPTPROV gone = 0;

        (void)CryptAcquireContextW(&gone, info->pwszContainerName, info->pwszProvName, info->dwProvType, CRYPT_DELETEKEYSET | (info->dwFlags & CRYPT_MACHINE_KEYSET));

        return;
    }

    NCRYPT_PROV_HANDLE provider = 0;

    if (NCryptOpenStorageProvider(&provider, info->pwszProvName, 0) != ERROR_SUCCESS) return;

    NCRYPT_KEY_HANDLE key = 0;

    if (NCryptOpenKey(provider, &key, info->pwszContainerName, info->dwKeySpec, info->dwFlags & NCRYPT_MACHINE_KEY_FLAG) == ERROR_SUCCESS)
        (void)NCryptDeleteKey(key, 0);

    (void)NCryptFreeObject(provider);
}

// What the connections of one adapter share: the credentials of both sides, the roots given with
// their own chain engine, and the server's identity. What cannot be set up is kept as the trouble
// of the side it concerns, and every session of that side reports it at once.
struct Ground {
    // Sets both sides up from `given`.
    explicit Ground(Credentials given) {
        if (!given.trusted.empty()) trusting(given.trusted);

        if (const SECURITY_STATUS status = acquired(true, nullptr, client); status == SEC_E_OK) client_held = true;
        else if (!client_amiss) client_amiss = described(Kind::broken, "The client's credentials cannot be had", status);

        if (given.identity.empty()) server_amiss = Trouble{Kind::missing, "The server has no identity."};
        else identifying(given.identity, given.password);

        crypto::wiped(given.password);
    }

    // Frees the credentials, takes the keys of the identity out of the user's store again, and
    // frees the chain engine.
    ~Ground() {
        if (client_held) (void)FreeCredentialsHandle(&client);
        if (server_held) (void)FreeCredentialsHandle(&server);

        if (identities.held != nullptr) {
            PCCERT_CONTEXT one = nullptr;

            while ((one = CertEnumCertificatesInStore(identities.held, one)) != nullptr) forgotten(one);
        }

        for (const std::string& one : lending_) returned(one);

        if (engine != nullptr) CertFreeCertificateChainEngine(engine);
    }

    Ground(const Ground&) = delete;
    Ground& operator=(const Ground&) = delete;

    CredHandle client{};
    CredHandle server{};
    bool client_held = false;
    bool server_held = false;
    std::optional<Trouble> client_amiss;
    std::optional<Trouble> server_amiss;
    Store trusted;
    HCERTCHAINENGINE engine = nullptr;
    Store identities;
    Certificate identity;

private:
    // Lends the intermediate authorities of the bundle to the user's store, for the server to send.
    void lend() {
        PCCERT_CONTEXT one = nullptr;

        while ((one = CertEnumCertificatesInStore(identities.held, one)) != nullptr) {
            if (CertCompareCertificate(X509_ASN_ENCODING, one->pCertInfo, identity.held->pCertInfo) || rooted(one)) continue;

            if (lending(one)) lending_.push_back(encoded(one));
        }
    }

    std::vector<std::string> lending_;

    // A chain engine whose roots are the certificates of `text` and only they.
    void trusting(const std::string& text) {
        trusted.held = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);

        if (trusted.held == nullptr || gathered(trusted.held, text) == 0) {
            client_amiss = Trouble{Kind::malformed, "No trusted certificate could be read."};

            return;
        }

        CERT_CHAIN_ENGINE_CONFIG config{};

        config.cbSize = sizeof config;
        config.hExclusiveRoot = trusted.held;

        if (CertCreateCertificateChainEngine(&config, &engine)) return;

        config.cbSize = offsetof(CERT_CHAIN_ENGINE_CONFIG, dwExclusiveFlags);

        if (!CertCreateCertificateChainEngine(&config, &engine)) client_amiss = described(Kind::broken, "The trusted certificates cannot be used", static_cast<SECURITY_STATUS>(GetLastError()));
    }

    // The identity of the PKCS#12 bundle `given` opened with `password`. Its key goes to the user's
    // store of keys, since Schannel runs in a process of the system and cannot reach a key kept in
    // memory alone, and leaves it with the adapter.
    void identifying(const Bytes& given, const std::string& password) {
        CRYPT_DATA_BLOB blob{};

        blob.cbData = static_cast<DWORD>(given.size());
        blob.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(given.data()));

        std::wstring spoken = widened(password);

        identities.held = PFXImportCertStore(&blob, spoken.c_str(), CRYPT_USER_KEYSET);

        const DWORD why = GetLastError();

        std::fill(spoken.begin(), spoken.end(), L'\0');

        if (identities.held == nullptr) {
            server_amiss = why == ERROR_INVALID_PASSWORD ? Trouble{Kind::denied, "The identity does not open with this password."}
                                                         : Trouble{Kind::malformed, "The identity cannot be read (status " + hexed(why) + ")."};

            return;
        }

        identity.held = keyed(identities.held);

        if (identity.held == nullptr) {
            server_amiss = Trouble{Kind::malformed, "The identity has no certificate with its key."};

            return;
        }

        lend();

        if (const SECURITY_STATUS status = acquired(false, identity.held, server); status == SEC_E_OK) server_held = true;
        else server_amiss = described(Kind::broken, "The server's credentials cannot be had", status);
    }
};

// One side of one connection: what came waits in `inbound_`, what goes in `outbound_`.
class Context final : public Session {
public:
    // A client of `host`, or a server (`client` false), of `ground`; the trouble of the side's set
    // up at once, when there is one.
    Context(std::shared_ptr<Ground> ground, bool client, const std::string& host) : ground_(std::move(ground)), client_(client), host_(widened(host)), named_(host) {
        failed_ = client_ ? ground_->client_amiss : ground_->server_amiss;

        if (!failed_ && client_ && host.empty()) failed_ = Trouble{Kind::other, "A secure connection needs the server's name."};
    }

    // Deletes the context, if the handshake made one.
    ~Context() override {
        if (begun_) (void)DeleteSecurityContext(&context_);
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Whether the handshake is over and, on a client, the certificate passed.
    [[nodiscard]] bool ready() const override { return settled_ && !failed_; }

    // Keeps what came for the next step or record.
    void push(Bytes given) override { inbound_.insert(inbound_.end(), given.begin(), given.end()); }

    // What there is to send, driving the handshake while it lasts.
    [[nodiscard]] Bytes pull() override {
        if (!settled_ && !failed_) shaken();

        return std::exchange(outbound_, {});
    }

    // Encrypts `given` into records no longer than Schannel allows.
    void write(Bytes given) override {
        if (given.empty() || failed_) return;

        if (!settled_) {
            failed_ = Trouble{Kind::other, "The handshake has not ended yet."};

            return;
        }

        for (std::size_t at = 0; at < given.size();) {
            const std::size_t part = std::min<std::size_t>(given.size() - at, sizes_.cbMaximumMessage);

            if (!sealed(given.data() + at, part)) return;

            at += part;
        }
    }

    // The plaintext of all the whole records that came; a part of one waits for the rest.
    [[nodiscard]] Bytes read() override {
        Bytes made;

        while (settled_ && !failed_ && !over_ && !inbound_.empty()) {
            if (renewing_) {
                shaken();

                if (renewing_) break;

                continue;
            }

            if (!opened(made)) break;
        }

        return made;
    }

    // Puts the goodbye among what is to send.
    void close() override {
        if (!settled_ || closed_ || failed_) return;

        closed_ = true;

        DWORD kind = SCHANNEL_SHUTDOWN;
        SecBuffer token{sizeof kind, SECBUFFER_TOKEN, &kind};
        SecBufferDesc tokens{SECBUFFER_VERSION, 1, &token};

        if (ApplyControlToken(&context_, &tokens) != SEC_E_OK) return;

        SecBuffer none{0, SECBUFFER_EMPTY, nullptr};
        SecBufferDesc input{SECBUFFER_VERSION, 1, &none};

        (void)stepped(&input);
    }

    // Whether the other side said goodbye.
    [[nodiscard]] bool closed() const override { return over_; }

    // What went wrong, if anything.
    [[nodiscard]] std::optional<Trouble> failed() const override { return failed_; }

private:
    // One step of the handshake with `input`: what Schannel sends goes out, what it took leaves
    // `inbound_` (an unread tail stays).
    SECURITY_STATUS stepped(SecBufferDesc* input) {
        SecBuffer out[2]{{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}};
        SecBufferDesc output{SECBUFFER_VERSION, 2, out};
        ULONG got = 0;
        SECURITY_STATUS status = SEC_E_OK;

        if (client_) {
            const ULONG asked = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                                ISC_REQ_EXTENDED_ERROR | ISC_REQ_USE_SUPPLIED_CREDS;

            status = InitializeSecurityContextW(&ground_->client, begun_ ? &context_ : nullptr, host_.data(), asked, 0, 0, input, 0, &context_, &output, &got,
                                                nullptr);
        } else {
            const ULONG asked =
                ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_EXTENDED_ERROR;

            status = AcceptSecurityContext(&ground_->server, begun_ ? &context_ : nullptr, input, asked, 0, &context_, &output, &got, nullptr);
        }

        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) begun_ = true;

        for (SecBuffer& one : out) {
            if (one.pvBuffer == nullptr) continue;

            if (one.BufferType == SECBUFFER_TOKEN) {
                const auto* what = static_cast<const std::byte*>(one.pvBuffer);

                outbound_.insert(outbound_.end(), what, what + one.cbBuffer);
            }

            (void)FreeContextBuffer(one.pvBuffer);
        }

        return status;
    }

    // Runs the handshake as far as what came allows: the client starts with nothing, the server
    // waits for the first bytes. Messages after the handshake (renegotiation, TLS 1.3 tickets) go
    // the same way.
    void shaken() {
        for (;;) {
            if (inbound_.empty() && (begun_ || !client_)) return;

            SecBuffer in[2]{{static_cast<ULONG>(inbound_.size()), SECBUFFER_TOKEN, inbound_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc input{SECBUFFER_VERSION, 2, in};
            const bool first = !begun_ && client_;
            const SECURITY_STATUS status = stepped(first ? nullptr : &input);

            if (status == SEC_E_INCOMPLETE_MESSAGE) return;

            const std::size_t left = in[1].BufferType == SECBUFFER_EXTRA ? in[1].cbBuffer : 0;

            inbound_.erase(inbound_.begin(), inbound_.end() - static_cast<std::ptrdiff_t>(left));

            if (status == SEC_I_CONTINUE_NEEDED) continue;

            if (status != SEC_E_OK) {
                failed_ = described(refusal(status), "The handshake failed", status);

                return;
            }

            if (renewing_) {
                renewing_ = false;

                return;
            }

            settle();

            return;
        }
    }

    // Ends the handshake: the sizes of records, and on a client the check of the certificate.
    void settle() {
        if (const SECURITY_STATUS status = QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_); status != SEC_E_OK) {
            failed_ = described(Kind::broken, "The sizes of the records are unknown", status);

            return;
        }

        if (client_) failed_ = verified();

        settled_ = true;
    }

    // The server's certificate: a chain to a trusted root, fit for server authentication, for the
    // name asked for. Revocation only with the Windows store, rejecting only what is revoked:
    // unknown and unreachable pass.
    [[nodiscard]] std::optional<Trouble> verified() {
        const std::string named = "\"" + named_ + "\"";
        PCCERT_CONTEXT given = nullptr;

        if (QueryContextAttributesW(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &given) != SEC_E_OK || given == nullptr)
            return Trouble{Kind::denied, "The server sent no certificate."};

        const Certificate theirs{given};
        LPSTR usage[1] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        CERT_CHAIN_PARA para{};

        para.cbSize = sizeof para;
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        para.RequestedUsage.Usage.cUsageIdentifier = 1;
        para.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
        para.dwUrlRetrievalTimeout = revocation_patience;

        const bool system = ground_->engine == nullptr;
        const DWORD flags = system ? CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT : 0;
        Chain chain;

        if (!CertGetCertificateChain(ground_->engine, theirs.held, nullptr, theirs.held->hCertStore, &para, flags, nullptr, &chain.held))
            return described(Kind::broken, "The chain of the certificate for " + named + " cannot be built", static_cast<SECURITY_STATUS>(GetLastError()));

        constexpr DWORD unknown = CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION;
        const DWORD wrong = chain.held->TrustStatus.dwErrorStatus & ~unknown;

        if ((wrong & CERT_TRUST_IS_NOT_TIME_VALID) != 0) return Trouble{Kind::denied, "The certificate for " + named + " has expired or is not valid yet."};

        if ((wrong & (CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_IS_PARTIAL_CHAIN)) != 0) return Trouble{Kind::denied, "The certificate for " + named + " is not trusted."};

        if (wrong != 0) return Trouble{Kind::denied, "The certificate for " + named + " does not pass (status " + hexed(wrong) + ")."};

        SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra{};

        extra.cbSize = sizeof extra;
        extra.dwAuthType = AUTHTYPE_SERVER;
        extra.fdwChecks = revocation_ignored;
        extra.pwszServerName = host_.data();

        CERT_CHAIN_POLICY_PARA policy{};

        policy.cbSize = sizeof policy;
        policy.pvExtraPolicyPara = &extra;

        CERT_CHAIN_POLICY_STATUS result{};

        result.cbSize = sizeof result;

        if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain.held, &policy, &result))
            return described(Kind::broken, "The certificate for " + named + " cannot be checked", static_cast<SECURITY_STATUS>(GetLastError()));

        if (result.dwError == static_cast<DWORD>(CERT_E_CN_NO_MATCH)) return Trouble{Kind::denied, "The certificate is not for " + named + "."};

        if (result.dwError != 0) return Trouble{Kind::denied, "The certificate for " + named + " does not pass (status " + hexed(result.dwError) + ")."};

        return std::nullopt;
    }

    // One record of `size` bytes at `plain`: header, data and trailer in one buffer, encrypted in
    // place; false when Schannel refused.
    bool sealed(const std::byte* plain, std::size_t size) {
        Bytes record(sizes_.cbHeader + size + sizes_.cbTrailer);

        std::copy(plain, plain + size, record.begin() + sizes_.cbHeader);

        SecBuffer parts[4]{{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                           {static_cast<ULONG>(size), SECBUFFER_DATA, record.data() + sizes_.cbHeader},
                           {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + size},
                           {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, parts};

        if (const SECURITY_STATUS status = EncryptMessage(&context_, 0, &message, 0); status != SEC_E_OK) {
            failed_ = described(Kind::broken, "A record cannot be encrypted", status);

            return false;
        }

        const std::size_t whole = parts[0].cbBuffer + parts[1].cbBuffer + parts[2].cbBuffer;

        outbound_.insert(outbound_.end(), record.begin(), record.begin() + static_cast<std::ptrdiff_t>(whole));

        return true;
    }

    // One record of what came, its plaintext into `made`; false when there is no whole one, the
    // goodbye came, or it failed. Everything after the goodbye is let be.
    bool opened(Bytes& made) {
        SecBuffer parts[4]{{static_cast<ULONG>(inbound_.size()), SECBUFFER_DATA, inbound_.data()},
                           {0, SECBUFFER_EMPTY, nullptr},
                           {0, SECBUFFER_EMPTY, nullptr},
                           {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc message{SECBUFFER_VERSION, 4, parts};
        const SECURITY_STATUS status = DecryptMessage(&context_, &message, 0, nullptr);

        if (status == SEC_E_INCOMPLETE_MESSAGE) return false;

        if (status == SEC_I_CONTEXT_EXPIRED) {
            over_ = true;
            inbound_.clear();

            return false;
        }

        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE) {
            failed_ = described(Kind::broken, "A record cannot be decrypted", status);

            return false;
        }

        Bytes rest;

        for (const SecBuffer& one : parts) {
            const auto* what = static_cast<const std::byte*>(one.pvBuffer);

            if (one.BufferType == SECBUFFER_DATA && one.cbBuffer > 0) made.insert(made.end(), what, what + one.cbBuffer);
            if (one.BufferType == SECBUFFER_EXTRA && one.cbBuffer > 0) rest.assign(what, what + one.cbBuffer);
        }

        inbound_ = std::move(rest);

        if (status == SEC_I_RENEGOTIATE) renewing_ = true;

        return true;
    }

    std::shared_ptr<Ground> ground_;
    bool client_;
    std::wstring host_;
    std::string named_;
    CtxtHandle context_{};
    bool begun_ = false;
    bool settled_ = false;
    bool renewing_ = false;
    bool over_ = false;
    bool closed_ = false;
    SecPkgContext_StreamSizes sizes_{};
    Bytes inbound_;
    Bytes outbound_;
    std::optional<Trouble> failed_;
};

// The TLS of Schannel, with what it was given.
class Schannel final : public Tls {
public:
    // Sets up both sides from `given`.
    explicit Schannel(Credentials given) : ground_(std::make_shared<Ground>(std::move(given))) {}

    // Its name.
    [[nodiscard]] std::string_view name() const override { return "Schannel"; }

    // A client of the server `host`.
    [[nodiscard]] std::unique_ptr<Session> client(std::string host) override { return std::make_unique<Context>(ground_, true, host); }

    // A server, with the identity given.
    [[nodiscard]] std::unique_ptr<Session> server() override { return std::make_unique<Context>(ground_, false, std::string{}); }

private:
    std::shared_ptr<Ground> ground_;
};

}

// The TLS of Schannel with the Windows store and no identity, built into tal on Windows.
std::shared_ptr<Tls> Tls::builtin() { return std::make_shared<Schannel>(Credentials{}); }

// The TLS of Schannel with `given`.
std::shared_ptr<Tls> Tls::builtin(Credentials given) { return std::make_shared<Schannel>(std::move(given)); }

}

#endif

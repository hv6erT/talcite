#include "sealed.hpp"

#include <memory>
#include <mutex>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

// Cała wiedza o OpenSSL w talcite mieści się w tym jednym pliku, a plik nie należy do biblioteki
// — jest osobnym celem, budowanym tylko wtedy, gdy ktoś tego chce. `talcite` nie ma ani jednego
// #include <openssl/...> i linkuje się bez niego. To nie jest ostrożność: to jedyny sposób, żeby
// „nie piszemy kryptografii" było faktem, a nie obietnicą.
namespace talcite::sealed {

namespace {

std::string spoken() {
    std::string made;

    while (const unsigned long why = ERR_get_error()) {
        char said[256]{};

        ERR_error_string_n(why, said, sizeof said);

        if (!made.empty()) made += "; ";

        made += said;
    }

    return made.empty() ? std::string{"nieznany kłopot w OpenSSL"} : made;
}

// Zabezpieczenie na jedno połączenie. **Biblioteka nigdy nie widzi gniazda** — dostaje dwa bufory
// w pamięci i tyle. Szyfrogram wchodzi przez rbio, wychodzi przez wbio, a całe wejście-wyjście
// robi talcite.
class Sealing : public net::Secure {
public:
    Sealing(SSL_CTX* held, bool client, std::string host) {
        ssl_ = SSL_new(held);

        if (ssl_ == nullptr) {
            amiss_ = spoken();

            return;
        }

        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());

        if (rbio_ == nullptr || wbio_ == nullptr) {
            amiss_ = spoken();

            return;
        }

        SSL_set_bio(ssl_, rbio_, wbio_);

        if (client) {
            SSL_set_connect_state(ssl_);

            if (!host.empty()) {
                (void)SSL_set_tlsext_host_name(ssl_, host.c_str());

                if (SSL_get_verify_mode(ssl_) != SSL_VERIFY_NONE)
                    (void)SSL_set1_host(ssl_, host.c_str());
            }
        } else {
            SSL_set_accept_state(ssl_);
        }
    }

    ~Sealing() override {
        // BIO należą już do SSL — zwolnienie ich osobno byłoby podwójnym zwolnieniem.
        if (ssl_ != nullptr) SSL_free(ssl_);
    }

    [[nodiscard]] bool ready() const override {
        return ssl_ != nullptr && SSL_is_init_finished(ssl_) != 0;
    }

    void push(Bytes given) override {
        if (ssl_ == nullptr || given.empty()) return;

        (void)BIO_write(rbio_, given.data(), static_cast<int>(given.size()));
    }

    // Pytanie o szyfrogram **napędza** uścisk dłoni. Bez tego nikt by go nie zaczął, bo talcite
    // niczego nie wie o stanach TLS.
    [[nodiscard]] Bytes pull() override {
        if (ssl_ == nullptr) return {};

        if (!ready()) {
            ERR_clear_error();

            const int made = SSL_do_handshake(ssl_);

            if (made <= 0) minded(made);
        }

        Bytes made;

        for (;;) {
            char much[4096];

            const int came = BIO_read(wbio_, much, static_cast<int>(sizeof much));

            if (came <= 0) return made;

            const auto* what = reinterpret_cast<const std::byte*>(much);

            made.insert(made.end(), what, what + came);
        }
    }

    void write(Bytes given) override {
        if (ssl_ == nullptr || given.empty()) return;

        if (!ready()) {
            amiss_ = "Nie skończył się jeszcze uścisk dłoni.";

            return;
        }

        std::size_t at = 0;

        while (at < given.size()) {
            ERR_clear_error();

            const int made =
                SSL_write(ssl_, given.data() + at, static_cast<int>(given.size() - at));

            if (made <= 0) {
                minded(made);

                return;
            }

            at += static_cast<std::size_t>(made);
        }
    }

    [[nodiscard]] Bytes read() override {
        if (ssl_ == nullptr) return {};

        Bytes made;

        for (;;) {
            char much[4096];

            ERR_clear_error();

            const int came = SSL_read(ssl_, much, static_cast<int>(sizeof much));

            if (came <= 0) {
                minded(came);

                return made;
            }

            const auto* what = reinterpret_cast<const std::byte*>(much);

            made.insert(made.end(), what, what + came);
        }
    }

    void shut() override {
        if (ssl_ != nullptr && ready()) (void)SSL_shutdown(ssl_);
    }

    [[nodiscard]] std::string amiss() const override { return amiss_; }

private:
    // „Chcę więcej danych" i „druga strona zamknęła" **nie są błędami** — to zwykły przebieg.
    // Wszystko inne jest.
    void minded(int made) {
        const int why = SSL_get_error(ssl_, made);

        if (why == SSL_ERROR_WANT_READ || why == SSL_ERROR_WANT_WRITE ||
            why == SSL_ERROR_ZERO_RETURN || why == SSL_ERROR_NONE)
            return;

        if (amiss_.empty()) amiss_ = spoken();
    }

    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;
    BIO* wbio_ = nullptr;
    std::string amiss_;
};

class Watching : public net::Keeper {
public:
    explicit Watching(Setting setting) : setting_(std::move(setting)) {
        held_ = SSL_CTX_new(TLS_method());

        if (held_ == nullptr) return;

        SSL_CTX_set_min_proto_version(held_, TLS1_2_VERSION);

        if (!setting_.certificate.empty()) taking(setting_.certificate, setting_.key);

        if (!setting_.trusted.empty()) trusting(setting_.trusted);

        SSL_CTX_set_verify(held_, setting_.checking ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

        if (setting_.checking && setting_.trusted.empty())
            (void)SSL_CTX_set_default_verify_paths(held_);
    }

    ~Watching() override {
        if (held_ != nullptr) SSL_CTX_free(held_);
    }

    [[nodiscard]] std::unique_ptr<net::Secure> client(std::string host) override {
        return std::make_unique<Sealing>(held_, true, std::move(host));
    }

    [[nodiscard]] std::unique_ptr<net::Secure> server() override {
        return std::make_unique<Sealing>(held_, false, std::string{});
    }

private:
    void taking(const std::string& certificate, const std::string& key) {
        BIO* one = BIO_new_mem_buf(certificate.data(), static_cast<int>(certificate.size()));

        if (one != nullptr) {
            if (X509* what = PEM_read_bio_X509(one, nullptr, nullptr, nullptr)) {
                (void)SSL_CTX_use_certificate(held_, what);

                X509_free(what);
            }

            BIO_free(one);
        }

        BIO* two = BIO_new_mem_buf(key.data(), static_cast<int>(key.size()));

        if (two != nullptr) {
            if (EVP_PKEY* what = PEM_read_bio_PrivateKey(two, nullptr, nullptr, nullptr)) {
                (void)SSL_CTX_use_PrivateKey(held_, what);

                EVP_PKEY_free(what);
            }

            BIO_free(two);
        }
    }

    void trusting(const std::string& trusted) {
        BIO* one = BIO_new_mem_buf(trusted.data(), static_cast<int>(trusted.size()));

        if (one == nullptr) return;

        X509_STORE* store = SSL_CTX_get_cert_store(held_);

        while (X509* what = PEM_read_bio_X509(one, nullptr, nullptr, nullptr)) {
            (void)X509_STORE_add_cert(store, what);

            X509_free(what);
        }

        BIO_free(one);
    }

    Setting setting_;
    SSL_CTX* held_ = nullptr;
};

}

net::Guard guarding(Setting setting) {
    return net::Guard{std::make_shared<Watching>(std::move(setting))};
}

}

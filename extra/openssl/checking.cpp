#include <cstddef>
#include <iostream>
#include <string>
#include <utility>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include "talcite/gate.hpp"
#include "talcite/secured.hpp"
#include "talcite/stream.hpp"
#include "talcite/trouble.hpp"

#include "sealed.hpp"
#include "speaking.hpp"

using namespace talcite;

namespace {

Bytes spelt(const std::string& what) {
    Bytes made(what.size());

    for (std::size_t at = 0; at < what.size(); ++at) made[at] = static_cast<std::byte>(what[at]);

    return made;
}

std::string spelling(const Bytes& what) {
    std::string made;

    for (const std::byte one : what) made += static_cast<char>(one);

    return made;
}

std::string written(const std::function<int(BIO*)>& what) {
    BIO* one = BIO_new(BIO_s_mem());

    if (one == nullptr) return {};

    std::string made;

    if (what(one) == 1) {
        char* said = nullptr;

        const long much = BIO_get_mem_data(one, &said);

        if (much > 0 && said != nullptr) made.assign(said, static_cast<std::size_t>(much));
    }

    BIO_free(one);

    return made;
}

// Świadectwo na jeden bieg, wystawione samemu sobie i trzymane wyłącznie w pamięci. Nie ma po nim
// pliku, nie ma czego zapomnieć skasować.
bool minted(std::string& certificate, std::string& key, const std::string& named) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);

    if (pkey == nullptr) return false;

    X509* what = X509_new();

    if (what == nullptr) {
        EVP_PKEY_free(pkey);

        return false;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(what), 1);
    X509_gmtime_adj(X509_getm_notBefore(what), 0);
    X509_gmtime_adj(X509_getm_notAfter(what), 3600);
    X509_set_pubkey(what, pkey);

    X509_NAME* who = X509_get_subject_name(what);

    X509_NAME_add_entry_by_txt(who, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(named.c_str()), -1, -1, 0);

    X509_set_issuer_name(what, who);

    // Bez nazwy alternatywnej nowoczesne sprawdzanie nazwy hosta i tak by odmówiło.
    const std::string alt = "DNS:" + named;

    X509_EXTENSION* one =
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, alt.c_str());

    if (one != nullptr) {
        X509_add_ext(what, one, -1);
        X509_EXTENSION_free(one);
    }

    const bool signed_ = X509_sign(what, pkey, EVP_sha256()) > 0;

    if (signed_) {
        certificate = written([what](BIO* into) { return PEM_write_bio_X509(into, what); });

        key = written([pkey](BIO* into) {
            return PEM_write_bio_PrivateKey(into, pkey, nullptr, nullptr, 0, nullptr, nullptr);
        });
    }

    X509_free(what);
    EVP_PKEY_free(pkey);

    return signed_ && !certificate.empty() && !key.empty();
}

}

// Prawdziwy TLS po pętli zwrotnej, ze świadectwem wystawionym na czas biegu — z jego
// sprawdzaniem, nie z wyłączonym. Jeden program, żaden serwer w internecie.
int main() {
    // Konsola Windows domyślnie nie stoi w UTF-8, a ten program pisze zwykłym `std::cout`, bez
    // `Term`. Bez strażnika polskie znaki rozpadają się na pary krzaków. To jest ten sam strażnik,
    // którego używają programy pokazowe — nie kopia, bo dwie kopie tej samej klasy raz już się
    // u nas rozjechały.
    const showing::Speaking speaking;

    const net::Patience soon{15000};

    const std::string named = "talcite.test";

    std::string certificate;
    std::string key;

    if (!minted(certificate, key, named)) {
        std::cout << "nie umiem wystawić świadectwa\n";

        return 1;
    }

    std::cout << "świadectwo dla " << named << " wystawione\n";

    try {
        sealed::Setting theirs;

        theirs.certificate = certificate;
        theirs.key = key;
        theirs.checking = false;

        sealed::Setting ours;

        ours.trusted = certificate;
        ours.checking = true;

        const net::Guard server = sealed::guarding(theirs);
        const net::Guard client = sealed::guarding(ours);

        const net::Gate gate{net::Address::loopback(net::Family::four, 0)};

        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(gate.mine(), soon);

        net::Stream one = meeting.await();
        net::Stream two = joining.await();

        auto there = net::Secured::server(std::move(one), server, soon);
        auto here = net::Secured::client(std::move(two), client, named, soon);

        net::Secured serving = there.await();
        net::Secured asking = here.await();

        std::cout << "uścisk dłoni doszedł do skutku\n";

        asking.write(spelt("witaj przez TLS"), soon).await();

        std::cout << "serwer usłyszał: " << spelling(serving.read(4096, soon).await()) << "\n";

        const std::string much(200000, 'x');

        auto writing = serving.write(spelt(much), soon);

        std::string made;

        while (made.size() < much.size()) {
            const Bytes came = asking.read(65536, soon).await();

            if (came.empty()) break;

            made += spelling(came);
        }

        writing.await();

        std::cout << "przez szyfr przeszło " << made.size() << " bajtów, zgodnych: "
                  << (made == much ? "tak" : "NIE") << "\n";

        asking.shut();

        std::cout << "po zamknięciu serwer czyta: " << serving.read(64, soon).await().size()
                  << " bajtów\n";
    } catch (const std::exception& why) {
        std::cout << "kłopot: " << why.what() << "\n";

        return 1;
    }

    return 0;
}

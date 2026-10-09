#include "proving.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

#include "talcite/base64.hpp"

using namespace talcite;

// Wektory z RFC 4648, rozdział 10, i to, czego odczyt ścisły nie przyjmuje.

TRIAL("base64: wektory z RFC 4648 w obie strony") {
    for (const auto& [plain, coded] : {std::pair<std::string_view, std::string_view>{"", ""},
                                       {"f", "Zg=="},
                                       {"fo", "Zm8="},
                                       {"foo", "Zm9v"},
                                       {"foob", "Zm9vYg=="},
                                       {"fooba", "Zm9vYmE="},
                                       {"foobar", "Zm9vYmFy"}}) {
        same(base64(bytes(plain)), std::string{coded});

        const std::optional<Bytes> back = unbase64(coded);

        ought(back.has_value());
        same(text(*back), std::string{plain});
    }
}

TRIAL("base64: wszystkie wartości bajtu wracają, a alfabet ma + i /") {
    Bytes all;

    for (int one = 0; one < 256; ++one) all.push_back(static_cast<std::byte>(one));

    const std::string coded = base64(all);

    same(coded.size(), std::size_t{344});
    ought(coded.find('+') != std::string::npos);
    ought(coded.find('/') != std::string::npos);
    same(coded.substr(0, 8), std::string{"AAECAwQF"});
    same(coded.substr(coded.size() - 8), std::string{"/P3+/w=="});

    const std::optional<Bytes> back = unbase64(coded);

    ought(back.has_value());
    ought(*back == all);
}

TRIAL("base64: odczyt odrzuca złą długość, obcy znak i dopełnienie nie na końcu") {
    // Długość sprawdza się przed odczytem, nie przez potknięcie o to, co leży za napisem: tu za
    // ucięciem stoją poprawne znaki.
    ought(!unbase64(std::string_view{"Zm9vYmFy", 6}).has_value());

    for (const std::string_view wrong : {"Zg=", "Zm9", "Zg==Zg==", "Zm=v", "Z===", "====", "Zm9v\n", "Zm 9", "Zm9-", "Zm9_"})
        ought(!unbase64(wrong).has_value());
}

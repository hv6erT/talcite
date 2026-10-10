#include "proving.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

#include "talcite/base64.hpp"

using namespace talcite;

// The vectors from RFC 4648, section 10, and what strict decoding does not accept.

TRIAL("base64: the RFC 4648 vectors both ways") {
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

TRIAL("base64: every byte value comes back, and the alphabet has + and /") {
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

TRIAL("base64: decoding rejects a wrong length, a foreign character and padding not at the end") {
    // The length is checked before decoding, not by stumbling over what lies past the text: here
    // valid characters stand after the cut.
    ought(!unbase64(std::string_view{"Zm9vYmFy", 6}).has_value());

    for (const std::string_view wrong : {"Zg=", "Zm9", "Zg==Zg==", "Zm=v", "Z===", "====", "Zm9v\n", "Zm 9", "Zm9-", "Zm9_"})
        ought(!unbase64(wrong).has_value());
}

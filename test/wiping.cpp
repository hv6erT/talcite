#include "secrets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "talcite/bytes.hpp"

#include "proving.hpp"

using namespace talcite;

// A wiped secret is all zeros and keeps its size, whatever holds it; equal secrets compare equal
// and any difference, even in the last byte or in the length, does not.

TRIAL("Secret: wiped zeroes bytes, text and limbs and keeps their size") {
    Bytes key = bytes("tajny klucz");
    std::string password = "hasło";
    std::array<std::int64_t, 4> limbs{1, -2, 3, 1 << 20};

    secret::wiped(key);
    secret::wiped(password);
    secret::wiped(limbs);

    same(key, Bytes(11, std::byte{0}));
    same(password, std::string(6, '\0'));
    ought(limbs == std::array<std::int64_t, 4>{});
}

TRIAL("Secret: alike tells equal bytes from bytes that differ anywhere or in length") {
    ought(secret::alike(bytes("potwierdzenie"), bytes("potwierdzenie")));
    ought(secret::alike(Bytes{}, Bytes{}));
    ought(!secret::alike(bytes("potwierdzenie"), bytes("potwierdzeniE")));
    ought(!secret::alike(bytes("potwierdzenie"), bytes("Potwierdzenie")));
    ought(!secret::alike(bytes("potwierdzenie"), bytes("potwierdzeni")));
    ought(!secret::alike(bytes("ab"), bytes("abc")));
}

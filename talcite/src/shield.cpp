#include "talcite/shield.hpp"

#include "talcite/trouble.hpp"

#include <string>

#ifdef _WIN32

#include <windows.h>

#include <dpapi.h>

#endif

// **Sekret tylko dla tego użytkownika tego komputera.** Na Windows system szyfruje go kluczem
// konta (DPAPI): plik z sekretem skopiowany na inne konto albo inny komputer nic nie mówi. Gdzie
// indziej systemowego odpowiednika bez doinstalowania czegokolwiek nie ma — bajty zostają, jakie
// są, a ochroną jest plik tylko dla właściciela, który zapisuje wołający. `shielding()` mówi,
// która z dróg działa, żeby wołający mógł to zapisać obok sekretu.
namespace talcite {

#ifdef _WIN32

namespace {

// Tablice systemu biorą długość jako DWORD — sekret dłuższy niż 4 GiB nie jest sekretem.
DATA_BLOB blob_of(const Bytes& given) {
    if (given.size() > 0xffffffffu) throw Trouble{"The secret is too long to shield."};

    DATA_BLOB made{};

    // Pusty sekret dostaje wskazanie na bajt, którego nie czytamy: Wine (i nie wiadomo, czy nie
    // któryś Windows) odrzuca pusty wskaźnik jako złe dane, choć długość jest zerowa.
    static BYTE nothing = 0;

    made.cbData = static_cast<DWORD>(given.size());
    made.pbData = given.empty() ? &nothing : reinterpret_cast<BYTE*>(const_cast<std::byte*>(given.data()));

    return made;
}

[[noreturn]] void failed(const char* what, DWORD why) {
    throw Trouble{std::string{what} + " (Windows error " + std::to_string(why) + ").", static_cast<int>(why)};
}

Bytes taken(DATA_BLOB& from) {
    Bytes made(from.cbData);

    for (DWORD at = 0; at < from.cbData; ++at) made[at] = static_cast<std::byte>(from.pbData[at]);

    SecureZeroMemory(from.pbData, from.cbData);
    LocalFree(from.pbData);

    return made;
}

}

bool shielding() { return true; }

Bytes shielded(const Bytes& plain) {
    DATA_BLOB in = blob_of(plain);
    DATA_BLOB out{};

    if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) == 0)
        failed("Cannot shield the secret", GetLastError());

    return taken(out);
}

Bytes unshielded(const Bytes& kept) {
    DATA_BLOB in = blob_of(kept);
    DATA_BLOB out{};

    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) == 0)
        failed("Cannot read the shielded secret", GetLastError());

    return taken(out);
}

#else

bool shielding() { return false; }

Bytes shielded(const Bytes& plain) { return plain; }

Bytes unshielded(const Bytes& kept) { return kept; }

#endif

}

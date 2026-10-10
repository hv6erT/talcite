#include "talcite/shield.hpp"

#include "talcite/trouble.hpp"

#include <string>

#ifdef _WIN32

#include <windows.h>

#include <dpapi.h>

#endif

// A secret readable only by this user on this computer. On Windows the system encrypts it with the
// account's key (DPAPI): a secret file copied to another account or another computer reveals
// nothing. Elsewhere there is no system equivalent without installing something, so the bytes stay
// as they are, and the protection is an owner-only file, which the caller writes. `shielding()`
// says which of the two applies, so that the caller can record it next to the secret.
namespace talcite {

#ifdef _WIN32

namespace {

// The system's blobs take the length as a DWORD; a secret longer than 4 GiB is not a secret, so it
// is refused with `Trouble`.
DATA_BLOB blob_of(const Bytes& given) {
    if (given.size() > 0xffffffffu) throw Trouble{"The secret is too long to shield."};

    DATA_BLOB made{};

    // An empty secret gets a pointer to a byte that is never read: Wine (and possibly some Windows
    // version) rejects a null pointer as bad data, even though the length is zero.
    static BYTE nothing = 0;

    made.cbData = static_cast<DWORD>(given.size());
    made.pbData = given.empty() ? &nothing : reinterpret_cast<BYTE*>(const_cast<std::byte*>(given.data()));

    return made;
}

// Throws `Trouble` with the message, the Windows error number in the text and as the code.
[[noreturn]] void failed(const char* what, DWORD why) {
    throw Trouble{std::string{what} + " (Windows error " + std::to_string(why) + ").", static_cast<int>(why)};
}

// Copies the bytes out of a blob that the system allocated, then wipes and frees it.
Bytes taken(DATA_BLOB& from) {
    Bytes made(from.cbData);

    for (DWORD at = 0; at < from.cbData; ++at) made[at] = static_cast<std::byte>(from.pbData[at]);

    SecureZeroMemory(from.pbData, from.cbData);
    LocalFree(from.pbData);

    return made;
}

}

// Whether secrets are really encrypted here: on Windows they are.
bool shielding() { return true; }

// Encrypts the secret with the current user's DPAPI key, without any prompt; throws `Trouble` on
// failure.
Bytes shielded(const Bytes& plain) {
    DATA_BLOB in = blob_of(plain);
    DATA_BLOB out{};

    if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) == 0)
        failed("Cannot shield the secret", GetLastError());

    return taken(out);
}

// Decrypts a secret made by `shielded` for the same user; throws `Trouble` when that fails (for
// example on another account or computer).
Bytes unshielded(const Bytes& kept) {
    DATA_BLOB in = blob_of(kept);
    DATA_BLOB out{};

    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) == 0)
        failed("Cannot read the shielded secret", GetLastError());

    return taken(out);
}

#else

// Whether secrets are really encrypted here: outside Windows they are not.
bool shielding() { return false; }

// Outside Windows the secret is returned unchanged.
Bytes shielded(const Bytes& plain) { return plain; }

// Outside Windows the kept bytes are the secret itself.
Bytes unshielded(const Bytes& kept) { return kept; }

#endif

}

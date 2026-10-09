#include "talcite/claim.hpp"

#include <string>
#include <system_error>
#include <utility>

#include "fscore.hpp"

#ifdef _WIN32

#include <windows.h>

#include <shlobj.h>

#else

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#endif

// **Katalog tylko dla tego użytkownika i znak „już działam".** Dwie rzeczy, których potrzebuje
// program w tle rozmawiający z programem na ekranie (demon udostępniania w twa): miejsce na gniazdo
// lokalne, do którego nikt inny nie zajrzy, i blokada jednej kopii, która znika sama, gdy program
// padnie.
namespace talcite {

namespace fs {

namespace {

namespace stood = std::filesystem;

#ifdef _WIN32

// FOLDERID_LocalAppData — stała Windows, wpisana tu z tego samego powodu co foldery znane w `fs.cpp`.
const GUID local_app_data{0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};

path runtime_at() {
    PWSTR told = nullptr;

    path made;

    if (SHGetKnownFolderPath(local_app_data, 0, nullptr, &told) == S_OK && told != nullptr) made = path{told};

    CoTaskMemFree(told);

    if (made.empty()) core::amiss("There is no local application data folder.", made);

    return made;
}

#else

// Katalog jest nasz, gdy jest katalogiem (nie dowiązaniem), należy do nas i nikt poza nami nie ma
// do niego żadnego prawa. Cudzy albo otwarty — nie nadaje się, bo przez niego ktoś mógłby podsunąć
// własne gniazdo albo czytać nasze.
bool private_folder(const path& where) {
    struct stat told{};

    if (::lstat(where.c_str(), &told) != 0) return false;

    return S_ISDIR(told.st_mode) && told.st_uid == ::geteuid() && (told.st_mode & 077) == 0;
}

path runtime_at() {
    // XDG_RUNTIME_DIR zakłada system przy logowaniu, z prawami 0700 i tylko dla nas — jeśli taki
    // jest naprawdę.
    if (const char* given = std::getenv("XDG_RUNTIME_DIR"); given != nullptr && *given == '/') {
        const path made{given};

        if (private_folder(made)) return made;
    }

    // Bez niego: własny katalog w katalogu tymczasowym, z numerem użytkownika w nazwie. Katalog
    // tymczasowy jest wspólny, więc ktoś mógł go założyć przed nami — wtedy nie jest nasz i nie
    // bierzemy go, zamiast rozluźniać jego prawa.
    const path made = temp() / ("talcite-" + std::to_string(::geteuid()));

    if (::mkdir(made.c_str(), 0700) != 0 && errno != EEXIST) core::amiss("Cannot create the runtime folder.", made);

    struct stat told{};

    if (::lstat(made.c_str(), &told) == 0 && S_ISDIR(told.st_mode) && told.st_uid == ::geteuid() &&
        (told.st_mode & 077) != 0)
        (void)::chmod(made.c_str(), 0700);

    if (!private_folder(made)) core::amiss("The runtime folder is not private.", made);

    return made;
}

#endif

}

Promise<path> runtime() {
    return Promise<path>{[](auto resolve, auto) { resolve(runtime_at()); }};
}

}

struct Claim::Held {
    ~Held() { release(); }

    void release() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);

        handle = INVALID_HANDLE_VALUE;
#else
        if (descriptor >= 0) ::close(descriptor);

        descriptor = -1;
#endif
    }

    fs::path where;

#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
#endif
};

namespace {

const fs::path& nowhere() {
    static const fs::path one;

    return one;
}

}

// **Blokada jednej kopii.** Plik otwarty na wyłączność — na Windows bez dzielenia, na POSIX-ie
// z `flock` — trzymany, dopóki żyje uchwyt. Proces, który padnie, oddaje go razem z deskryptorami,
// więc po awarii nie zostaje nic do sprzątania: plik stoi dalej, ale nikt go nie trzyma. Pliku
// nie kasujemy przy zwolnieniu — skasowany między otwarciem a blokadą dwóch chętnych dałby dwie
// blokady na dwóch różnych plikach o jednej nazwie.
//
// Zajęte przez kogoś innego to pustka, a nie odmowa: „już działa" jest odpowiedzią, a odmową jest
// tylko to, czego nie dało się sprawdzić (brak katalogu, brak praw). Na POSIX-ie dowiązania nie
// przechodzimy — cudze dowiązanie w naszym miejscu nie ma prawa wskazać pliku, który zablokujemy.
Promise<std::optional<Claim>> Claim::taken(fs::path where) {
    return Promise<std::optional<Claim>>{[where = std::move(where)](auto resolve, auto) {
        auto held = std::make_shared<Held>();

        held->where = where;

#ifdef _WIN32

        held->handle = CreateFileW(where.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);

        if (held->handle == INVALID_HANDLE_VALUE) {
            const DWORD why = GetLastError();

            if (why == ERROR_SHARING_VIOLATION || why == ERROR_LOCK_VIOLATION) {
                resolve(std::optional<Claim>{});

                return;
            }

            fs::core::amiss("Cannot open the claim file.", where);
        }

#else

        held->descriptor = ::open(where.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);

        if (held->descriptor < 0) fs::core::amiss("Cannot open the claim file.", where);

        if (::flock(held->descriptor, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                resolve(std::optional<Claim>{});

                return;
            }

            fs::core::amiss("Cannot lock the claim file.", where);
        }

#endif

        resolve(std::optional<Claim>{Claim{std::move(held)}});
    }};
}

const fs::path& Claim::where() const { return held_ ? held_->where : nowhere(); }

void Claim::release() {
    if (!held_) return;

    held_->release();
    held_.reset();
}

}

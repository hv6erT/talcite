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

// A folder for this user only, and an "I am already running" mark. These are the two things a
// background program talking to a program on the screen needs (the sharing daemon in twa): a place
// for a local socket that nobody else can look into, and a single-instance lock that disappears by
// itself when the program dies.
namespace talcite {

namespace fs {

namespace {

namespace stood = std::filesystem;

#ifdef _WIN32

// FOLDERID_LocalAppData, a Windows constant written out here for the same reason as the known
// folders in `fs.cpp`.
const GUID local_app_data{0xf1b32785, 0x6fba, 0x4fcf, {0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91}};

// The user's local application data folder. Throws when Windows does not give one.
path runtime_at() {
    PWSTR told = nullptr;

    path made;

    if (SHGetKnownFolderPath(local_app_data, 0, nullptr, &told) == S_OK && told != nullptr) made = path{told};

    CoTaskMemFree(told);

    if (made.empty()) core::amiss("There is no local application data folder.", made);

    return made;
}

#else

// Whether the folder is ours: a folder (not a link) that belongs to us, to which nobody else has
// any right. Someone else's or an open one will not do, because through it someone could slip in
// their own socket or read ours.
bool private_folder(const path& where) {
    struct stat told{};

    if (::lstat(where.c_str(), &told) != 0) return false;

    return S_ISDIR(told.st_mode) && told.st_uid == ::geteuid() && (told.st_mode & 077) == 0;
}

// A private folder for runtime files: XDG_RUNTIME_DIR when it really is private, or else our own
// folder in the temporary folder. Throws when no private folder can be had.
path runtime_at() {
    // The system creates XDG_RUNTIME_DIR at login, with mode 0700 and for us only, if it really is
    // such a folder.
    if (const char* given = std::getenv("XDG_RUNTIME_DIR"); given != nullptr && *given == '/') {
        const path made{given};

        if (private_folder(made)) return made;
    }

    // Without it: our own folder in the temporary folder, with the user number in its name. The
    // temporary folder is shared, so someone could have created it before us; then it is not ours,
    // and we do not take it rather than loosen its permissions.
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

// The private runtime folder, found on the pool; the promise fails when there is none.
Promise<path> runtime() {
    return run(io(), []() -> path { return runtime_at(); });
}

}

struct Claim::Held {
    // Gives the lock back when the last owner lets go.
    ~Held() { release(); }

    // Closes the lock file, which releases the lock; safe to call twice.
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

// The empty path a released claim reports.
const fs::path& nowhere() {
    static const fs::path one;

    return one;
}

}

// The single-instance lock. A file opened exclusively (on Windows without sharing, on POSIX with
// `flock`) and held as long as the handle lives. A process that dies gives it back together with
// its descriptors, so after a crash there is nothing to clean up: the file stays, but nobody holds
// it. We do not delete the file on release: deleted between the open and the lock of two
// contenders, it would give two locks on two different files with one name.
//
// The holder may remove the file while it holds it, which is how a lock that is no longer needed
// goes away (the locks of the history entries in twa), and the same on both systems: on Windows the
// handle shares deletion alone, so nobody else can open the file meanwhile, while the holder's own
// removal goes through; on POSIX a contender who opened the file just before it was removed would
// lock a file without a name, so after locking we check that the name still leads to the file we
// hold, and otherwise try again with the file now under that name.
//
// Taken by someone else is an empty result, not a refusal: "already running" is an answer, and a
// refusal is only what could not be checked (no folder, no permissions). On POSIX we do not follow
// links: someone else's link in our place must not point at a file we would lock.
Promise<std::optional<Claim>> Claim::taken(fs::path where) {
    return run(io(), [where = std::move(where)]() -> std::optional<Claim> {
        auto held = std::make_shared<Held>();

        held->where = where;

#ifdef _WIN32

        held->handle = CreateFileW(where.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);

        if (held->handle == INVALID_HANDLE_VALUE) {
            const DWORD why = GetLastError();

            if (why == ERROR_SHARING_VIOLATION || why == ERROR_LOCK_VIOLATION) {
                return std::optional<Claim>{};
            }

            fs::core::amiss("Cannot open the claim file.", where);
        }

#else

        for (;;) {
            held->descriptor = ::open(where.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);

            if (held->descriptor < 0) fs::core::amiss("Cannot open the claim file.", where);

            if (::flock(held->descriptor, LOCK_EX | LOCK_NB) != 0) {
                if (errno == EWOULDBLOCK) {
                    return std::optional<Claim>{};
                }

                fs::core::amiss("Cannot lock the claim file.", where);
            }

            struct stat mine{};
            struct stat named{};

            if (::fstat(held->descriptor, &mine) != 0) fs::core::amiss("Cannot lock the claim file.", where);

            if (::lstat(where.c_str(), &named) == 0 && named.st_dev == mine.st_dev && named.st_ino == mine.st_ino) break;

            held->release();
        }

#endif

        return std::optional<Claim>{Claim{std::move(held)}};
    });
}

// The path of the lock file, or an empty path once released.
const fs::path& Claim::where() const { return held_ ? held_->where : nowhere(); }

// Gives the lock back now instead of when the claim is destroyed.
void Claim::release() {
    if (!held_) return;

    held_->release();
    held_.reset();
}

}

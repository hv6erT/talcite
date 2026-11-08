#include <chrono>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "core.hpp"

#ifdef _WIN32

#include <windows.h>

#include <shellapi.h>

#else

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#endif

// The trash of the system. Windows has its recycle bin behind the shell. Linux and the other
// desktops of freedesktop.org have the Trash specification: a folder `files` with what was thrown
// away and a folder `info` with a `.trashinfo` for each, telling where it was and when; the trash
// of the home folder for what lies on the medium of the home folder, and a trash at the top of any
// other medium (`.Trash/$uid` when the administrator made it, `.Trash-$uid` otherwise), so that
// throwing away is a rename and never a copy.

namespace tal::platform::core {

#ifdef _WIN32

// Throws the entry into the recycle bin through the shell, without asking and without a window.
// The shell takes no \\?\ form, so the path goes as it is.
void trashed_at(const path& where) {
    std::wstring named = where.native();

    named.push_back(L'\0');

    SHFILEOPSTRUCTW how{};

    how.wFunc = FO_DELETE;
    how.pFrom = named.c_str();
    how.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

    const int failed = SHFileOperationW(&how);

    if (failed != 0 || how.fAnyOperationsAborted != 0)
        amiss("Cannot move it to the recycle bin.", where, std::error_code{failed, std::system_category()});
}

#else

namespace {

// The path written as the Trash specification wants it in a `.trashinfo`: every byte that is not
// unreserved in a URI as %XX, the slashes kept.
std::string escaped(const std::string& plain) {
    static constexpr char digits[] = "0123456789ABCDEF";

    std::string made;

    for (const char one : plain) {
        const auto byte = static_cast<unsigned char>(one);
        const bool kept = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                          byte == '-' || byte == '_' || byte == '.' || byte == '~' || byte == '/';

        if (kept) {
            made += one;
        } else {
            made += '%';
            made += digits[byte >> 4U];
            made += digits[byte & 0x0FU];
        }
    }

    return made;
}

// The device a path lies on, or none when it cannot be asked.
std::optional<dev_t> device_of(const path& where) {
    struct stat told {};

    if (::lstat(where.c_str(), &told) != 0) return std::nullopt;

    return told.st_dev;
}

// The top of the medium that holds the path: the highest folder above it on the same device.
path top_of(const path& where) {
    const std::optional<dev_t> device = device_of(where);
    path top = where.parent_path();

    while (top.has_parent_path() && top != top.parent_path()) {
        if (device_of(top.parent_path()) != device) break;

        top = top.parent_path();
    }

    return top;
}

// The home trash: $XDG_DATA_HOME/Trash, or ~/.local/share/Trash.
path home_trash() {
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data == '/') return path{data} / "Trash";

    if (const char* home = std::getenv("HOME"); home != nullptr && *home == '/') return path{home} / ".local/share/Trash";

    return {};
}

// A folder the trash needs, made for its owner alone when it is missing.
void made_private(const path& where) {
    std::error_code trouble;

    stood::create_directories(where, trouble);

    if (trouble) amiss("Cannot make the trash.", where, trouble);

    (void)::chmod(where.c_str(), 0700);
}

// The trash for what lies at `where`, and the folder its paths in `.trashinfo` are relative to
// (empty for the home trash, whose paths are absolute).
std::pair<path, path> trash_for(const path& where) {
    const path home = home_trash();

    if (!home.empty()) {
        made_private(home);

        if (device_of(home) == device_of(where)) return {home, {}};
    }

    const path top = top_of(where);
    const std::string user = std::to_string(::getuid());
    const path shared = top / ".Trash";
    struct stat told {};

    // A `.Trash` the administrator made counts only when it is a real folder with the sticky bit:
    // anything else could be a trap laid by another user.
    if (::lstat(shared.c_str(), &told) == 0 && S_ISDIR(told.st_mode) && (told.st_mode & S_ISVTX) != 0) {
        made_private(shared / user);

        return {shared / user, top};
    }

    made_private(top / (".Trash-" + user));

    return {top / (".Trash-" + user), top};
}

// The local time now as the specification writes it: 2026-11-08T16:04:00.
std::string stamped_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm fields{};

    (void)localtime_r(&now, &fields);

    char made[32] = {};

    (void)std::strftime(made, sizeof made, "%Y-%m-%dT%H:%M:%S", &fields);

    return made;
}

}

// Throws the entry into the trash of its medium: first its `.trashinfo`, made exclusively under a
// free name, then the entry renamed into `files` under the same name. A rename that fails takes
// the `.trashinfo` back, so the trash never tells of something it does not hold.
void trashed_at(const path& where) {
    const auto [trash, relative] = trash_for(where);

    made_private(trash / "files");
    made_private(trash / "info");

    const std::string own = where.filename().native();
    const std::string stem = where.stem().native();
    const std::string extension = where.extension().native();
    const std::string shown = relative.empty() ? where.native() : where.lexically_relative(relative).native();
    const std::string said = "[Trash Info]\nPath=" + escaped(shown) + "\nDeletionDate=" + stamped_now() + "\n";

    for (int number = 1; number <= 1000; ++number) {
        const std::string name = number == 1 ? own : stem + "." + std::to_string(number) + extension;
        const path info = trash / "info" / (name + ".trashinfo");
        const int made = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

        if (made < 0) {
            if (errno == EEXIST) continue;

            amiss("Cannot write into the trash.", info);
        }

        const ssize_t put = ::write(made, said.data(), said.size());
        const std::error_code put_why = last_error();

        ::close(made);

        if (put != static_cast<ssize_t>(said.size())) {
            ::unlink(info.c_str());

            amiss("Cannot write into the trash.", info, put_why);
        }

        const path into = trash / "files" / name;
        struct stat taken {};

        // A name taken in `files` without its `.trashinfo` (another program's leftover) is passed.
        if (::lstat(into.c_str(), &taken) == 0) {
            ::unlink(info.c_str());

            continue;
        }

        if (::rename(where.c_str(), into.c_str()) == 0) return;

        const std::error_code why = last_error();

        ::unlink(info.c_str());

        amiss("Cannot move it to the trash.", where, why);
    }

    amiss("The trash has no free name for it.", where, ::tal::Kind::full);
}

#endif

}

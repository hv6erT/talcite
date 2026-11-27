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

#include <shobjidl.h>

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

namespace {

// A COM object held: released when the holder goes.
template <typename T>
class Held {
public:
    Held() = default;

    ~Held() {
        if (held_ != nullptr) held_->Release();
    }

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    // Where a call that makes the object puts it.
    [[nodiscard]] T** put() { return &held_; }

    // The object.
    T* operator->() const { return held_; }

    // The object, to pass on.
    [[nodiscard]] T* get() const { return held_; }

private:
    T* held_ = nullptr;
};

// A single-threaded COM apartment of this thread while it lives, unless the thread had one of the
// other kind already, which serves as well.
class Apartment {
public:
    Apartment() : made_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

    ~Apartment() {
        if (SUCCEEDED(made_)) CoUninitialize();
    }

    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;

    // Whether the thread has an apartment to work in.
    [[nodiscard]] bool held() const { return SUCCEEDED(made_) || made_ == RPC_E_CHANGED_MODE; }

    // Why it has none.
    [[nodiscard]] HRESULT trouble() const { return made_; }

private:
    HRESULT made_;
};

// The watcher of a deletion into the recycle bin. Before each item goes the shell says whether it
// goes to the bin (`TSF_DELETE_RECYCLE_IF_POSSIBLE`); an item it would destroy instead stops the
// whole operation there, before anything is gone, and the watcher remembers the refusal. After an
// item the first failure is kept. It lives on the stack, so counting its references does nothing.
class Binning final : public IFileOperationProgressSink {
public:
    bool refused = false;
    HRESULT failed = S_OK;
    std::wstring kept;

    // The watcher as the interfaces it has.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID asked, void** out) override {
        if (asked == IID_IUnknown || asked == __uuidof(IFileOperationProgressSink)) {
            *out = static_cast<IFileOperationProgressSink*>(this);

            return S_OK;
        }

        *out = nullptr;

        return E_NOINTERFACE;
    }

    // A reference more, uncounted.
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }

    // A reference less, uncounted.
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // The item `item` about to be deleted as `flags` say: on to the bin, or the operation stopped.
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem* item) override {
        (void)item;

        if ((flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) != 0) return S_OK;

        refused = true;

        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    // The item deleted, or not, as `result` says; the first failure is kept, and so is where the
    // first item went in the bin (`made`), which is the entry itself.
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD flags, IShellItem* item, HRESULT result, IShellItem* made) override {
        (void)flags;
        (void)item;

        if (FAILED(result) && SUCCEEDED(failed)) failed = result;

        PWSTR named = nullptr;

        if (SUCCEEDED(result) && made != nullptr && kept.empty() && SUCCEEDED(made->GetDisplayName(SIGDN_FILESYSPATH, &named))) {
            kept = named;
            CoTaskMemFree(named);
        }

        return S_OK;
    }

    // The start of the operation, which the watcher lets be.
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }

    // The end of the operation, which the caller hears of itself.
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override {
        (void)result;

        return S_OK;
    }

    // A renaming, which a deletion does not do.
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }

    // A renaming done, which a deletion does not do.
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }

    // A move, which a deletion does not do.
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }

    // A move done, which a deletion does not do.
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }

    // A copy, which a deletion does not do.
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }

    // A copy done, which a deletion does not do.
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }

    // A new item, which a deletion does not make.
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }

    // A new item made, which a deletion does not make.
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }

    // The progress, which nobody shows.
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }

    // The timer of the progress, which nobody shows.
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }

    // The timer of the progress paused, which nobody shows.
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }

    // The timer of the progress going on, which nobody shows.
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};

// The code of a failure of COM, as an error of the system.
std::error_code failure_of(HRESULT result) { return std::error_code{static_cast<int>(result), std::system_category()}; }

}

// Throws the entry into the recycle bin through the shell, without asking and without a window,
// and only into the bin: where the shell would destroy the entry or anything in it (a share, an
// entry too large for the bin), nothing is deleted and the refusal is a `Trouble` of the kind
// `unsupported`. The shell takes no \\?\ form, so the path goes as it is, with the system's
// separators. It gives where the bin keeps the entry ($R… in $Recycle.Bin), or nothing when the
// shell did not say.
path trashed_at(const path& where) {
    const Apartment apartment;

    if (!apartment.held()) amiss("Cannot reach the recycle bin.", where, failure_of(apartment.trouble()));

    path named = where;

    named.make_preferred();

    Held<IShellItem> item;
    HRESULT result = SHCreateItemFromParsingName(named.c_str(), nullptr, IID_PPV_ARGS(item.put()));

    if (FAILED(result)) amiss("Cannot move it to the recycle bin.", where, failure_of(result));

    Held<IFileOperation> operation;

    result = CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, IID_PPV_ARGS(operation.put()));

    if (FAILED(result)) amiss("Cannot reach the recycle bin.", where, failure_of(result));

    Binning watcher;
    DWORD cookie = 0;

    result = operation->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE);

    if (SUCCEEDED(result)) result = operation->Advise(&watcher, &cookie);

    if (FAILED(result)) amiss("Cannot move it to the recycle bin.", where, failure_of(result));

    result = operation->DeleteItem(item.get(), nullptr);

    if (SUCCEEDED(result)) result = operation->PerformOperations();

    BOOL aborted = FALSE;

    (void)operation->GetAnyOperationsAborted(&aborted);
    (void)operation->Unadvise(cookie);

    if (watcher.refused) amiss("There is no recycle bin here.", where, ::tal::Kind::unsupported);

    if (SUCCEEDED(result) && FAILED(watcher.failed)) result = watcher.failed;

    if (SUCCEEDED(result) && aborted != FALSE) result = HRESULT_FROM_WIN32(ERROR_CANCELLED);

    if (FAILED(result)) amiss("Cannot move it to the recycle bin.", where, failure_of(result));

    return path{watcher.kept};
}

// Forgets an entry given back from the recycle bin, `kept` being its $R… there: the bin's record of
// it, the $I… beside it of the same tail, goes. One already gone is no trouble.
void untrashed_at(const path& kept) {
    std::wstring named = kept.filename().native();

    if (named.size() < 2 || named.rfind(L"$R", 0) != 0) return;

    named[1] = L'I';

    (void)DeleteFileW(longly(kept.parent_path() / named).c_str());
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
// free name, then the entry renamed into `files` under the same name, which it gives. A rename that
// fails takes the `.trashinfo` back, so the trash never tells of something it does not hold.
path trashed_at(const path& where) {
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

        if (::rename(where.c_str(), into.c_str()) == 0) return into;

        const std::error_code why = last_error();

        ::unlink(info.c_str());

        amiss("Cannot move it to the trash.", where, why);
    }

    amiss("The trash has no free name for it.", where, ::tal::Kind::full);
}

// Forgets an entry given back from the trash, `kept` being where `files` held it: its `.trashinfo`
// goes. One already gone is no trouble.
void untrashed_at(const path& kept) {
    const path info = kept.parent_path().parent_path() / "info" / (kept.filename().native() + ".trashinfo");

    (void)::unlink(info.c_str());
}

#endif

}

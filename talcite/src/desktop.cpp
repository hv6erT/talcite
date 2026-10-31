#include "talcite/desktop.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "talcite/executor.hpp"
#include "talcite/trouble.hpp"

#ifdef _WIN32

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include "talcite/encoding.hpp"

#elif defined(__APPLE__)

#include <CoreServices/CoreServices.h>

#else

#include <unistd.h>

#endif

#include "desktops.hpp"
#include "remoting.hpp"

// The desktop of the user's system: opening a file in the program the system gives its type,
// listing the programs that open it, opening it with one of them or with a program the user names,
// and telling programs and scripts from documents. It stands behind a seam like the launcher, so
// twa is tested with a fake, and every decision that needs no system is in `desktops`, tested on
// written fixtures.
//
// Unlike the launcher, which refuses `file:`, the desktop opens files: the path comes from a person
// pointing at it, not from a page. It runs no program by itself, though: whether a file is a
// program, and whether to run it in a terminal, is the caller's decision (`runnable`), and a
// program that needs a terminal comes back as `Started::terminal` with its command line
// (`command`), because the terminal is the application's, not ours.
//
// Without a graphical session (over ssh, or on Linux without DISPLAY and WAYLAND_DISPLAY) a window
// has nowhere to appear, so the answer is `Started::sessionless`; a program that works in a
// terminal still comes back as `Started::terminal`, as the terminal is there. The session is
// decided on the caller's thread, from its console, as in the launcher; the system is asked on the
// I/O lane, and the answer comes back to the caller's loop, where a program is let loose
// (`Child::detached`).
//
// Windows asks the shell: ShellExecuteEx for the default program, SHAssocEnumHandlers for the
// list, IAssocHandler::Invoke to open with one, SHOpenWithDialog for the system's own choice. Every
// call runs with COM in a single-threaded apartment, as the shell wants it. Linux reads the
// freedesktop.org files itself (globs2, subclasses, mimeapps.list, mimeinfo.cache, desktop files),
// anew at every call, so that a program installed a moment ago is seen; xdg-open would open a
// browser for a type nobody claims and would not say what it did. macOS asks Launch Services and
// starts programs with `open`.
namespace talcite {

namespace {

namespace stood = std::filesystem;

// How many first bytes of a file decide what it is: enough for every header read here, the MZ
// stub and the PE header of an executable included.
constexpr std::size_t peeked = 4096;

// The first bytes of a file, up to `peeked`; empty when it cannot be read.
std::vector<unsigned char> head_of(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};

    if (!in) return {};

    std::vector<unsigned char> made(peeked);

    in.read(reinterpret_cast<char*>(made.data()), static_cast<std::streamsize>(made.size()));

    made.resize(static_cast<std::size_t>(in.gcount()));

    return made;
}

// The whole text of a small file; nothing when it cannot be read.
std::optional<std::string> text_of(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};

    if (!in) return std::nullopt;

    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The extension of the file without its dot, as written.
std::string extension_of(const fs::path& file) {
    std::string made = fs::to_string(file.extension());

    if (!made.empty() && made.front() == '.') made.erase(0, 1);

    return made;
}

// The extension in lower case, ASCII only.
std::string lowered_extension(const fs::path& file) {
    std::string made = extension_of(file);

    std::transform(made.begin(), made.end(), made.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

    return made;
}

// Whether there is a graphical session for a window: not over ssh, always on Windows and macOS,
// and elsewhere when X or Wayland says so.
bool windowed(const Console& from) {
    if (remote(from)) return false;

#if defined(_WIN32) || defined(__APPLE__)

    return true;

#else

    return from.variable("DISPLAY").has_value() || from.variable("WAYLAND_DISPLAY").has_value();

#endif
}

// What the system decided on the I/O lane: an outcome already, or a program to let loose on the
// caller's loop.
struct Plan {
    Started how = Started::opened;
    std::optional<Command> start;
};

// The plan carried out on the caller's loop: the program let loose, or the outcome as it is.
Promise<Started> carried(const Plan& plan) {
    if (!plan.start.has_value()) return ready(plan.how);

    return Child::detached(*plan.start).then([](std::int64_t) { return Started::opened; });
}

// A plain program with the file as its only argument, started in the file's folder.
Command plainly(const fs::path& program, const fs::path& file) {
    Command made;

    made.program = program;
    made.arguments = {fs::to_string(file)};
    made.where = file.parent_path();

    return made;
}

#ifdef _WIN32

// COM in a single-threaded apartment for the time of a shell call, without OLE1 DDE, as the
// launcher does; left on the way out only when it was joined here.
class Apartment {
public:
    // Joins the thread to an apartment.
    Apartment() : joined_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

    // Leaves the apartment when this object joined it.
    ~Apartment() {
        if (SUCCEEDED(joined_)) CoUninitialize();
    }

    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;

private:
    HRESULT joined_;
};

// A COM interface released on the way out.
template <typename Face>
class Held {
public:
    Held() = default;

    // Releases the interface.
    ~Held() {
        if (face_ != nullptr) face_->Release();
    }

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;

    // The place a COM call writes the interface into.
    [[nodiscard]] Face** out() { return &face_; }

    // The interface.
    [[nodiscard]] Face* operator->() const { return face_; }

    // Whether the interface is there.
    [[nodiscard]] bool held() const { return face_ != nullptr; }

    // The interface itself.
    [[nodiscard]] Face* get() const { return face_; }

private:
    Face* face_ = nullptr;
};

// A string the shell allocated, as UTF-8, freed on the way.
std::string taken(LPWSTR given) {
    if (given == nullptr) return {};

    std::string made = fs::to_string(fs::path{std::wstring{given}});

    CoTaskMemFree(given);

    return made;
}

// Whether two paths name the same program: compared without regard to letter case, as Windows
// compares paths.
bool alike(const std::string& one, const std::string& other) {
    const std::wstring left = fs::from_string(one).wstring();
    const std::wstring right = fs::from_string(other).wstring();

    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

// The program the system opens the extension with, its full path; empty when there is none.
std::string default_of(const std::wstring& extension) {
    DWORD size = MAX_PATH;
    std::wstring made(size, L'\0');

    HRESULT told = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, extension.c_str(), nullptr, made.data(), &size);

    if (told == E_POINTER) {
        made.assign(size, L'\0');
        told = AssocQueryStringW(ASSOCF_NOTRUNCATE, ASSOCSTR_EXECUTABLE, extension.c_str(), nullptr, made.data(), &size);
    }

    if (FAILED(told)) return {};

    made.resize(size > 0 ? size - 1 : 0);

    return fs::to_string(fs::path{made});
}

// The shell's handlers of the extension under the filter, each passed to `each` with its path;
// `each` returns true to stop.
template <typename Each>
void handled(const std::wstring& extension, ASSOC_FILTER filter, Each each) {
    Held<IEnumAssocHandlers> listed;

    if (FAILED(SHAssocEnumHandlers(extension.c_str(), filter, listed.out())) || !listed.held()) return;

    while (true) {
        Held<IAssocHandler> one;
        ULONG got = 0;

        if (listed->Next(1, one.out(), &got) != S_OK || got == 0) return;

        LPWSTR name = nullptr;

        if (FAILED(one->GetName(&name))) continue;

        if (each(one, taken(name))) return;
    }
}

// The programs that open files of the extension (with its dot), by the shell's recommendation, with
// the default one first. The shell asks by the extension alone, so a file need not be there.
std::vector<Application> recommended(const std::wstring& extension) {
    const Apartment apartment;

    if (extension.size() < 2 || extension.front() != L'.') return {};

    const std::string usual = default_of(extension);

    std::vector<Application> made;

    handled(extension, ASSOC_FILTER_RECOMMENDED, [&made, &usual](Held<IAssocHandler>& one, std::string path) {
        LPWSTR shown = nullptr;

        Application found;

        found.id = std::move(path);
        found.name = SUCCEEDED(one->GetUIName(&shown)) ? taken(shown) : fs::to_string(fs::from_string(found.id).stem());
        found.preferred = !usual.empty() && alike(found.id, usual);

        made.push_back(std::move(found));

        return false;
    });

    std::stable_partition(made.begin(), made.end(), [](const Application& one) { return one.preferred; });

    return made;
}

// Opens the file in the program the system gives its type; true when it opened, false when the
// type has no program. ShellExecuteEx waits for the shell, not for the program, and keeps its own
// warning for a file from the internet, since no flag here switches that check off. No message box
// of its own: an error comes back to the caller.
bool executed(const fs::path& file) {
    const Apartment apartment;

    const std::wstring path = file.wstring();
    const std::wstring folder = file.parent_path().wstring();

    SHELLEXECUTEINFOW asked{};

    asked.cbSize = sizeof(asked);
    asked.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    asked.lpFile = path.c_str();
    asked.lpDirectory = folder.empty() ? nullptr : folder.c_str();
    asked.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&asked)) return true;

    const DWORD told = GetLastError();

    if (told == ERROR_NO_ASSOCIATION) return false;

    throw Trouble{"The system did not open the file.", static_cast<int>(told)};
}

// Opens the file with the shell's handler whose program is `id`; false when no handler of the
// extension has that program.
bool invoked(const fs::path& file, const std::string& id) {
    const Apartment apartment;

    bool done = false;
    HRESULT told = S_OK;

    handled(file.extension().wstring(), ASSOC_FILTER_NONE, [&](Held<IAssocHandler>& one, const std::string& path) {
        if (!alike(path, id)) return false;

        Held<IShellItem> item;

        told = SHCreateItemFromParsingName(file.wstring().c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.out()));

        if (FAILED(told)) return true;

        Held<IDataObject> data;

        told = item->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject, reinterpret_cast<void**>(data.out()));

        if (FAILED(told)) return true;

        told = one->Invoke(data.get());
        done = SUCCEEDED(told);

        return true;
    });

    if (FAILED(told)) throw Trouble{"The program did not open the file.", static_cast<int>(HRESULT_CODE(told))};

    return done;
}

// The system's own window for choosing a program, which opens the file with the chosen one. It
// does not offer to remember the choice: twa keeps its own and changes nothing in the system.
// Closing the window cancels.
void picked(const fs::path& file) {
    const Apartment apartment;

    const std::wstring path = file.wstring();

    OPENASINFO asked{};

    asked.pcszFile = path.c_str();
    asked.oaifInFlags = OAIF_EXEC;

    const HRESULT told = SHOpenWithDialog(nullptr, &asked);

    if (told == HRESULT_FROM_WIN32(ERROR_CANCELLED)) throw Cancelled{};

    if (FAILED(told)) throw Trouble{"The system window for choosing a program did not open.", static_cast<int>(HRESULT_CODE(told))};
}

// Where the file came from, by its Zone.Identifier stream; here when it has none.
Origin zoned(const fs::path& file) {
    std::ifstream in{fs::path{file.wstring() + L":Zone.Identifier"}, std::ios::binary};

    if (!in) return Origin::here;

    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

    return desktops::zone_of(text);
}

#elif defined(__APPLE__)

// A Core Foundation object released on the way out.
template <typename Ref>
class Owned {
public:
    // Takes the object, which may be null.
    explicit Owned(Ref ref) : ref_(ref) {}

    // Releases the object.
    ~Owned() {
        if (ref_ != nullptr) CFRelease(ref_);
    }

    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    // The object.
    [[nodiscard]] Ref get() const { return ref_; }

private:
    Ref ref_;
};

// The file system path of a URL; empty when it has none.
std::string path_of(CFURLRef url) {
    char made[PATH_MAX];

    if (!CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(made), sizeof(made))) return {};

    return made;
}

// An application of the system by its bundle's path: the name is the bundle's without .app.
Application application_at(const std::string& path, bool preferred) {
    Application made;

    made.id = path;
    made.name = fs::to_string(fs::from_string(path).stem());
    made.preferred = preferred;

    return made;
}

// The application of a bundle id: the first bundle Launch Services knows by it; nothing when none.
std::optional<std::string> bundled_at(CFStringRef id) {
    const Owned<CFArrayRef> found{LSCopyApplicationURLsForBundleIdentifier(id, nullptr)};

    if (found.get() == nullptr || CFArrayGetCount(found.get()) == 0) return std::nullopt;

    std::string where = path_of(static_cast<CFURLRef>(CFArrayGetValueAtIndex(found.get(), 0)));

    if (where.empty()) return std::nullopt;

    return where;
}

// The applications Launch Services knows for files of the extension (with its dot), the default
// one first: the extension's uniform type, then its default handler and all its handlers, by their
// bundle ids, each found as a bundle on the disk. The calls are deprecated in favour of the
// UniformTypeIdentifiers framework of Objective-C and Swift, as are those of `launched_by`.
std::vector<Application> handling(const std::string& extension) {
    if (extension.size() < 2 || extension.front() != '.') return {};

    const Owned<CFStringRef> tag{CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(extension.data() + 1),
                                                         static_cast<CFIndex>(extension.size() - 1), kCFStringEncodingUTF8, false)};

    if (tag.get() == nullptr) return {};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const Owned<CFStringRef> type{UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, tag.get(), nullptr)};

    if (type.get() == nullptr) return {};

    const Owned<CFStringRef> usual{LSCopyDefaultRoleHandlerForContentType(type.get(), kLSRolesAll)};
    const Owned<CFArrayRef> all{LSCopyAllRoleHandlersForContentType(type.get(), kLSRolesAll)};
#pragma clang diagnostic pop

    std::vector<Application> made;
    std::string first;

    if (usual.get() != nullptr)
        if (const std::optional<std::string> where = bundled_at(usual.get())) {
            first = *where;
            made.push_back(application_at(first, true));
        }

    if (all.get() == nullptr) return made;

    for (CFIndex at = 0; at < CFArrayGetCount(all.get()); ++at) {
        const std::optional<std::string> where = bundled_at(static_cast<CFStringRef>(CFArrayGetValueAtIndex(all.get(), at)));

        if (!where || *where == first) continue;

        if (std::none_of(made.begin(), made.end(), [&where](const Application& one) { return one.id == *where; }))
            made.push_back(application_at(*where, false));
    }

    return made;
}

// The applications Launch Services knows for the file, the default one first. The list comes from
// LSCopyApplicationURLsForURL, which Apple marks deprecated in favour of NSWorkspace; it stays the
// only way to ask from C++ without Objective-C, and it works.
std::vector<Application> launched_by(const fs::path& file) {
    const std::string path = fs::to_string(file);

    const Owned<CFURLRef> url{CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), stood::is_directory(file))};

    if (url.get() == nullptr) return {};

    std::vector<Application> made;

    const Owned<CFURLRef> usual{LSCopyDefaultApplicationURLForURL(url.get(), kLSRolesAll, nullptr)};

    std::string first;

    if (usual.get() != nullptr) {
        first = path_of(usual.get());

        if (!first.empty()) made.push_back(application_at(first, true));
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const Owned<CFArrayRef> all{LSCopyApplicationURLsForURL(url.get(), kLSRolesAll)};
#pragma clang diagnostic pop

    if (all.get() == nullptr) return made;

    for (CFIndex at = 0; at < CFArrayGetCount(all.get()); ++at) {
        const auto one = static_cast<CFURLRef>(CFArrayGetValueAtIndex(all.get(), at));
        const std::string where = path_of(one);

        if (where.empty() || where == first) continue;

        made.push_back(application_at(where, false));
    }

    return made;
}

// The `open` program with the arguments.
Command opening(std::vector<std::string> arguments, const fs::path& file) {
    Command made;

    made.program = "/usr/bin/open";
    made.arguments = std::move(arguments);
    made.where = file.parent_path();
    made.merge = true;

    return made;
}

// What `open` answered: opened when it ended well, the type without a program when it says no
// application knows the file, otherwise its own words as the error.
Started answered(const Output& said) {
    if (said.exit.ended == Ended::normal && said.exit.code == 0) return Started::opened;

    std::string text = talcite::text(said.out);

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();

    if (text.find("No application") != std::string::npos || text.find("no application") != std::string::npos) return Started::unassociated;

    throw Trouble{text.empty() ? std::string{"The system did not open the file."} : text};
}

#else

// The environment the Linux desktop is read from, taken on the caller's thread.
struct Seen {
    desktops::Environment environment;
    std::string locale;
    std::string path;
    bool windowed = false;
};

// The value of a variable, empty when unset.
std::string valued(const Console& from, std::string_view name) { return from.variable(name).value_or(std::string{}); }

// What the desktop reads, from the console's variables: the XDG directories, the locale of names
// (LC_ALL, then LC_MESSAGES, then LANG, as gettext orders them) and PATH for TryExec.
Seen seen_from(const Console& from) {
    Seen made;

    made.environment.home = from.variable("HOME");
    made.environment.config_home = from.variable("XDG_CONFIG_HOME");
    made.environment.config_dirs = from.variable("XDG_CONFIG_DIRS");
    made.environment.data_home = from.variable("XDG_DATA_HOME");
    made.environment.data_dirs = from.variable("XDG_DATA_DIRS");
    made.environment.current = from.variable("XDG_CURRENT_DESKTOP");

    for (const char* one : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        made.locale = valued(from, one);

        if (!made.locale.empty()) break;
    }

    made.path = valued(from, "PATH");
    made.windowed = windowed(from);

    return made;
}

// Whether the file can be run by this user.
bool executable(const fs::path& file) {
    std::error_code failed;

    return stood::is_regular_file(file, failed) && ::access(file.c_str(), X_OK) == 0;
}

// Whether a program named in TryExec is there: a path that can be run, or a name found so in a
// folder of PATH.
bool present(const std::string& program, const std::string& path) {
    if (program.find('/') != std::string::npos) return executable(fs::from_string(program));

    std::string_view rest = path;

    while (true) {
        const std::size_t colon = rest.find(':');
        const std::string_view folder = rest.substr(0, colon);

        if (!folder.empty() && executable(fs::from_string(folder) / fs::from_string(program))) return true;

        if (colon == std::string_view::npos) return false;

        rest.remove_prefix(colon + 1);
    }
}

// An installed program: its desktop entry and where the entry lies.
struct Installed {
    desktops::Entry entry;
    fs::path location;
};

// The Linux desktop for one question: the places, the locale and PATH, and what has been read so
// far, so that one question reads each file once.
class Reading {
public:
    // Reads nothing yet.
    explicit Reading(Seen seen) : seen_(std::move(seen)), places_(desktops::places(seen_.environment)) {}

    // The type of the file: a folder is inode/directory; otherwise by the globs, and a name no glob
    // knows is text/plain when its first bytes are text and application/octet-stream when not.
    std::string type_of(const fs::path& file) {
        std::error_code failed;

        if (stood::is_directory(file, failed)) return "inode/directory";

        if (auto found = named(fs::to_string(file.filename()))) return *found;

        const std::vector<unsigned char> head = head_of(file);

        return desktops::textual(head) ? "text/plain" : "application/octet-stream";
    }

    // The type of a name by the globs alone; nothing when no glob knows it.
    std::optional<std::string> named(const std::string& name) {
        if (!globs_.has_value()) {
            globs_.emplace();

            for (const fs::path& folder : places_.mime)
                if (const auto text = text_of(folder / "globs2")) {
                    std::vector<desktops::Glob> more = desktops::globs(*text);

                    globs_->insert(globs_->end(), more.begin(), more.end());
                }
        }

        return desktops::type_of(name, *globs_);
    }

    // The type with the types it descends from, nearest first: the subclasses of every MIME
    // folder, and text/plain under every text/ type, as the specification makes every text a
    // plain text. application/octet-stream, the parent of all, is left out, as it would offer a
    // hex editor for everything.
    std::vector<std::string> lineage(const std::string& type) {
        if (!parents_.has_value()) {
            parents_.emplace();

            for (const fs::path& folder : places_.mime)
                if (const auto text = text_of(folder / "subclasses"))
                    for (auto& [child, list] : desktops::parents(*text)) {
                        std::vector<std::string>& into = (*parents_)[child];

                        into.insert(into.end(), list.begin(), list.end());
                    }
        }

        std::vector<std::string> made{type};

        for (std::size_t at = 0; at < made.size(); ++at) {
            std::vector<std::string> next;

            if (const auto found = parents_->find(made[at]); found != parents_->end()) next = found->second;

            if (made[at].starts_with("text/") && made[at] != "text/plain") next.emplace_back("text/plain");

            for (std::string& one : next)
                if (one != "application/octet-stream" && std::find(made.begin(), made.end(), one) == made.end()) made.push_back(std::move(one));
        }

        return made;
    }

    // The defaults of the type from every mimeapps.list, most important first.
    std::vector<std::string> defaults_of(const std::string& type) {
        lists();

        std::vector<std::string> made;

        for (const desktops::Associations& one : *lists_)
            if (const auto found = one.defaults.find(type); found != one.defaults.end())
                for (const std::string& id : found->second)
                    if (std::find(made.begin(), made.end(), id) == made.end()) made.push_back(id);

        return made;
    }

    // The programs associated with the type: the added associations of each mimeapps.list in
    // order, then the caches of the applications folders, without what a list of higher or equal
    // importance removed. A removal reaches only the lists below its own and the caches.
    std::vector<std::string> associated(const std::string& type) {
        lists();
        caches();

        std::vector<std::string> made;
        std::set<std::string, std::less<>> removed;

        const auto added = [&made, &removed](const desktops::Lists& from, const std::string& kind) {
            if (const auto found = from.find(kind); found != from.end())
                for (const std::string& id : found->second)
                    if (!removed.contains(id) && std::find(made.begin(), made.end(), id) == made.end()) made.push_back(id);
        };

        for (const desktops::Associations& one : *lists_) {
            added(one.added, type);

            if (const auto found = one.removed.find(type); found != one.removed.end()) removed.insert(found->second.begin(), found->second.end());
        }

        for (const desktops::Lists& one : *caches_) added(one, type);

        return made;
    }

    // The installed program of the id: an absolute path is a desktop file itself; otherwise the
    // first applications folder that has the file, also under a subfolder, which the id writes with
    // a dash ("kde4-dolphin.desktop" is kde4/dolphin.desktop). Nothing when it is missing, deleted
    // (Hidden), no application, without a command line, or when its TryExec program is not there.
    std::optional<Installed> installed(const std::string& id) {
        if (const auto found = known_.find(id); found != known_.end()) return found->second;

        std::optional<Installed> made;

        for (const fs::path& at : locations(id)) {
            const auto text = text_of(at);

            if (!text.has_value()) continue;

            auto read = desktops::entry(*text, seen_.locale);

            if (read.has_value() && read->application && !read->hidden && !read->exec.empty() &&
                (read->try_exec.empty() || present(read->try_exec, seen_.path)))
                made = Installed{std::move(*read), at};

            break;
        }

        known_.emplace(id, made);

        return made;
    }

    // The default program of the file's type, searched along its lineage, nearest type first: for
    // each type its defaults, then the first of its associated programs, before its parent is
    // asked, so that a program made for the type wins over the default of a broader one (a reader
    // of .nfo files over the plain text editor).
    std::optional<std::pair<std::string, Installed>> chosen_for(const std::vector<std::string>& line) {
        for (const std::string& type : line) {
            for (const std::string& id : defaults_of(type))
                if (auto found = installed(id)) return std::pair{id, std::move(*found)};

            for (const std::string& id : associated(type))
                if (auto found = installed(id)) return std::pair{id, std::move(*found)};
        }

        return std::nullopt;
    }

    // Whether there is a graphical session.
    [[nodiscard]] bool windowed() const { return seen_.windowed; }

private:
    // The mimeapps.list files, read once.
    void lists() {
        if (lists_.has_value()) return;

        lists_.emplace();

        for (const fs::path& one : places_.lists)
            if (const auto text = text_of(one)) lists_->push_back(desktops::associations(*text));
    }

    // The caches of the applications folders, read once. A folder without mimeinfo.cache (often the
    // user's own, where update-desktop-database is not run) gets one made from its desktop files.
    void caches() {
        if (caches_.has_value()) return;

        caches_.emplace();

        for (const fs::path& folder : places_.applications) {
            if (const auto text = text_of(folder / "mimeinfo.cache")) {
                caches_->push_back(desktops::cached(*text));
                continue;
            }

            desktops::Lists made;
            std::error_code failed;

            for (stood::recursive_directory_iterator at{folder, failed}, end; !failed && at != end; at.increment(failed)) {
                if (at->path().extension() != ".desktop") continue;

                const auto text = text_of(at->path());

                if (!text.has_value()) continue;

                const auto read = desktops::entry(*text, seen_.locale);

                if (!read.has_value()) continue;

                std::string id = fs::to_string(at->path().lexically_relative(folder));

                std::replace(id.begin(), id.end(), '/', '-');

                for (const std::string& type : read->types) made[type].push_back(id);
            }

            caches_->push_back(std::move(made));
        }
    }

    // Where the desktop file of an id may lie, in order.
    std::vector<fs::path> locations(const std::string& id) const {
        if (!id.empty() && id.front() == '/') return {fs::from_string(id)};

        std::vector<fs::path> made;

        for (const fs::path& folder : places_.applications) {
            made.push_back(folder / fs::from_string(id));

            std::string nested = id;

            for (std::size_t dash = nested.find('-'); dash != std::string::npos; dash = nested.find('-', dash + 1)) {
                nested[dash] = '/';

                made.push_back(folder / fs::from_string(nested));
            }
        }

        return made;
    }

    Seen seen_;
    desktops::Places places_;
    std::optional<std::vector<desktops::Glob>> globs_;
    std::optional<desktops::Lists> parents_;
    std::optional<std::vector<desktops::Associations>> lists_;
    std::optional<std::vector<desktops::Lists>> caches_;
    std::map<std::string, std::optional<Installed>, std::less<>> known_;
};

// The programs of a type and the types it descends from (`line`, nearest first): the default one
// first and marked `preferred`; then, type by type, the defaults and the associated programs, each
// once, without those hidden from lists (NoDisplay).
std::vector<Application> offered(Reading& reading, const std::vector<std::string>& line) {
    std::vector<Application> made;
    std::set<std::string, std::less<>> taken;

    const auto put = [&made, &taken](const std::string& id, const Installed& one, bool preferred) {
        if (!taken.insert(id).second) return;

        made.push_back(Application{one.entry.name.empty() ? id : one.entry.name, id, preferred, one.entry.terminal});
    };

    if (const auto first = reading.chosen_for(line)) put(first->first, first->second, true);

    for (const std::string& type : line) {
        std::vector<std::string> ids = reading.defaults_of(type);

        for (std::string& id : reading.associated(type)) ids.push_back(std::move(id));

        for (const std::string& id : ids)
            if (const auto one = reading.installed(id); one.has_value() && one->entry.shown) put(id, *one, false);
    }

    return made;
}

// The command line of an installed program for the file, started in the file's folder.
std::optional<Command> commanded(const Installed& one, const fs::path& file) {
    const std::string named = file.empty() ? std::string{} : fs::to_string(file);
    const auto words = desktops::expanded(one.entry, named, fs::to_string(one.location));

    if (!words.has_value()) return std::nullopt;

    Command made;

    made.program = fs::from_string(words->front());
    made.arguments.assign(words->begin() + 1, words->end());
    made.where = file.empty() ? one.location.parent_path() : file.parent_path();

    return made;
}

// The plan for a program and a file: a terminal program goes back to the caller, nothing opens
// without a session, and otherwise the program is let loose.
Plan planned(const Installed& one, const fs::path& file, bool session) {
    if (one.entry.terminal) return {Started::terminal, std::nullopt};

    if (!session) return {Started::sessionless, std::nullopt};

    auto made = commanded(one, file);

    if (!made.has_value()) throw Trouble{"The program's desktop entry has no usable command line."};

    return {Started::opened, std::move(made)};
}

// Whether the file is a desktop entry that runs as a program: a .desktop file with the execute
// bit, as desktops ask for before they start a launcher they did not install themselves.
bool launcher_file(const fs::path& file) { return file.extension() == ".desktop" && executable(file); }

#endif

// The desktop of the user's system, with the console it learns the session and the environment
// from.
class System final : public Desktop {
public:
    explicit System(Seam<Console> with) : console_(std::move(with)) {}

    // The desktop's name in messages.
    [[nodiscard]] std::string_view name() const override { return "system desktop"; }

    // Opens the file in the default program of its type, as the head of this file says.
    [[nodiscard]] Promise<Started> open(fs::path file) override {
#ifdef _WIN32

        if (!windowed(console_.provider())) return ready(Started::sessionless);

        return run(io(), [file] { return executed(file) ? Started::opened : Started::unassociated; }).here();

#elif defined(__APPLE__)

        if (!windowed(console_.provider())) return ready(Started::sessionless);

        return Child::run(opening({fs::to_string(file)}, file)).here().then([](const Output& said) { return answered(said); });

#else

        return run(io(), [file, seen = seen_from(console_.provider())] {
                   Reading reading{seen};

                   if (launcher_file(file)) {
                       const auto text = text_of(file);
                       const auto read = text.has_value() ? desktops::entry(*text, seen.locale) : std::nullopt;

                       if (!read.has_value() || !read->application || read->exec.empty())
                           throw Trouble{"The desktop entry does not start a program."};

                       return planned(Installed{*read, file}, fs::path{}, reading.windowed());
                   }

                   const auto found = reading.chosen_for(reading.lineage(reading.type_of(file)));

                   if (!found.has_value()) return Plan{Started::unassociated, std::nullopt};

                   return planned(found->second, file, reading.windowed());
               })
            .here()
            .then([](const Plan& plan) { return carried(plan); });

#endif
    }

    // The programs that open the file, the default one first and marked `preferred`, then the
    // others (`offered` on Linux).
    [[nodiscard]] Promise<std::vector<Application>> applications(fs::path file) override {
#ifdef _WIN32

        return run(io(), [file] { return recommended(file.extension().wstring()); }).here();

#elif defined(__APPLE__)

        return run(io(), [file] { return launched_by(file); }).here();

#else

        return run(io(), [file, seen = seen_from(console_.provider())] {
                   Reading reading{seen};

                   return offered(reading, reading.lineage(reading.type_of(file)));
               })
            .here();

#endif
    }

    // The programs that open files of the extension (with its dot), as for such a file, without a
    // file to look at: Linux knows the type by the globs alone, matched against the extension as a
    // whole name (which *.ext fits), and an extension no glob knows has no programs.
    [[nodiscard]] Promise<std::vector<Application>> applications_for(std::string extension) override {
#ifdef _WIN32

        return run(io(), [extension] { return recommended(fs::from_string(extension).wstring()); }).here();

#elif defined(__APPLE__)

        return run(io(), [extension] { return handling(extension); }).here();

#else

        return run(io(), [extension, seen = seen_from(console_.provider())] {
                   if (extension.size() < 2 || extension.front() != '.') return std::vector<Application>{};

                   Reading reading{seen};

                   const std::optional<std::string> type = reading.named(extension);

                   if (!type) return std::vector<Application>{};

                   return offered(reading, reading.lineage(*type));
               })
            .here();

#endif
    }

    // Opens the file with one of the programs `applications` gave.
    [[nodiscard]] Promise<Started> open_with(fs::path file, Application with) override {
#ifdef _WIN32

        if (!windowed(console_.provider())) return ready(Started::sessionless);

        return run(io(), [file, with] {
                   if (invoked(file, with.id)) return Plan{Started::opened, std::nullopt};

                   return Plan{Started::opened, plainly(fs::from_string(with.id), file)};
               })
            .here()
            .then([](const Plan& plan) { return carried(plan); });

#elif defined(__APPLE__)

        if (!windowed(console_.provider())) return ready(Started::sessionless);

        return Child::run(opening({"-a", with.id, fs::to_string(file)}, file)).here().then([](const Output& said) { return answered(said); });

#else

        return run(io(), [file, with, seen = seen_from(console_.provider())] {
                   Reading reading{seen};

                   const auto found = reading.installed(with.id);

                   if (!found.has_value()) throw Trouble{"The program is no longer installed."};

                   return planned(*found, file, reading.windowed());
               })
            .here()
            .then([](const Plan& plan) { return carried(plan); });

#endif
    }

    // Opens the file with a program the user named: a path, or a name looked up in PATH by
    // `Child`. On macOS an application bundle goes through `open -a`.
    [[nodiscard]] Promise<Started> open_by(fs::path file, fs::path program) override {
        if (!windowed(console_.provider())) return ready(Started::sessionless);

#ifdef __APPLE__

        if (program.extension() == ".app") return Child::run(opening({"-a", fs::to_string(program), fs::to_string(file)}, file)).here().then([](const Output& said) { return answered(said); });

#endif

        return Child::detached(plainly(program, file)).then([](std::int64_t) { return Started::opened; });
    }

    // The command line of a program that runs in a terminal, for the caller's terminal. Only Linux
    // desktop entries say they need one; elsewhere there is nothing.
    [[nodiscard]] Promise<std::optional<Command>> command(fs::path file, Application with) override {
#if defined(_WIN32) || defined(__APPLE__)

        (void)file;
        (void)with;

        return ready(std::optional<Command>{});

#else

        return run(io(), [file, with, seen = seen_from(console_.provider())] {
                   Reading reading{seen};

                   const auto found = reading.installed(with.id);

                   if (!found.has_value()) return std::optional<Command>{};

                   return commanded(*found, launcher_file(file) && fs::to_string(file) == with.id ? fs::path{} : file);
               })
            .here();

#endif
    }

    // Whether the file is a program and how it runs, as `desktops` reads it from its name, its
    // first bytes and its execute bit. On macOS an application bundle is a program with windows;
    // on Linux so is a desktop entry with the execute bit.
    [[nodiscard]] Promise<Runnable> runnable(fs::path file) override {
        return run(io(), [file] {
#ifdef _WIN32

                   const std::string kind = lowered_extension(file);

                   return desktops::windows_runnable(kind, kind == "exe" ? head_of(file) : std::vector<unsigned char>{});

#else

                   std::error_code failed;

#ifdef __APPLE__

                   if (stood::is_directory(file, failed)) return file.extension() == ".app" ? Runnable::window : Runnable::no;

#else

                   if (launcher_file(file)) return Runnable::window;

#endif

                   if (!stood::is_regular_file(file, failed)) return Runnable::no;

                   return desktops::posix_runnable(head_of(file), ::access(file.c_str(), X_OK) == 0);

#endif
               })
            .here();
    }

    // Where the file came from: the zone Windows wrote when it was downloaded, elsewhere here.
    [[nodiscard]] Promise<Origin> origin(fs::path file) override {
#ifdef _WIN32

        return run(io(), [file] { return zoned(file); }).here();

#else

        (void)file;

        return ready(Origin::here);

#endif
    }

    // The system's own window for choosing a program: Windows only; elsewhere refused.
    [[nodiscard]] Promise<Started> chosen(fs::path file) override {
#ifdef _WIN32

        if (!windowed(console_.provider())) return ready(Started::sessionless);

        return run(io(), [file] {
                   picked(file);

                   return Started::opened;
               })
            .here();

#else

        (void)file;

        return failed<Started>(std::make_exception_ptr(Trouble{"This system has no window for choosing a program."}));

#endif
    }

private:
    Seam<Console> console_;
};

}

// The desktop of the user's system, on the program's console. One per process: it holds nothing
// but the seam.
Seam<Desktop> desktop() {
    static const Seam<Desktop> shared{std::make_shared<System>(console())};

    return shared;
}

// A desktop that learns the session and the environment from the given console: a trial.
Seam<Desktop> desktop(Seam<Console> with) { return Seam<Desktop>{std::make_shared<System>(std::move(with))}; }

}

#include "tal/desktop.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "desktops.hpp"

#ifdef _WIN32

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include "platform/offload.hpp"

#endif

// The desktop of the user's system: whether a file is a program and how it runs, where it came
// from, opening it in the program the system gives its type, listing the programs that open it,
// opening it with one of them or with a program the user names, and the system's own window for
// choosing. It runs no program to tell what a file is: whether to run one, and in which terminal,
// is the caller's decision (`program` only gives the command), and a program that needs a terminal
// comes back as `Started::terminal` with its command line, as the terminal is the caller's.
//
// Without a graphical session (over ssh, or on Linux without DISPLAY and WAYLAND_DISPLAY) a window
// has nowhere to appear, so the answer is `Started::sessionless`; a program that works in a
// terminal still comes back as `Started::terminal`.
//
// Windows asks the shell: ShellExecuteEx for the default program, SHAssocEnumHandlers for the
// list, IAssocHandler::Invoke to open with one, SHOpenWithDialog for the system's own choice, each
// with COM in a single-threaded apartment, as the shell wants it, on the I/O threads. Linux reads
// the freedesktop.org files itself (globs2, subclasses, mimeapps.list, mimeinfo.cache, desktop
// files) through the port of files, anew at every call, so that a program installed a moment ago
// is seen; xdg-open would open a browser for a type nobody claims and would not say what it did.
// Programs are let loose through the port of processes.
namespace tal {

namespace {

// How many first bytes of a file decide what it is: enough for every header read here, the MZ
// stub and the PE header of an executable included.
constexpr std::size_t peeked = 4096;

#ifdef _WIN32
constexpr bool windows = true;
#else
constexpr bool windows = false;
#endif

// Whether the program runs over ssh.
bool remote(const Console& from) { return from.variable("SSH_CONNECTION").has_value() || from.variable("SSH_TTY").has_value(); }

// Whether there is a graphical session for a window: not over ssh, always on Windows, and
// elsewhere when X or Wayland says so.
bool windowed(const Console& from) {
    if (remote(from)) return false;

    if constexpr (windows) return true;

    return from.variable("DISPLAY").has_value() || from.variable("WAYLAND_DISPLAY").has_value();
}

// The path as the system writes it in a command line: with backslashes on Windows, its own bytes
// elsewhere.
std::string spelled(const Path& one) {
    if constexpr (!windows) return std::string{one.stored()};

    std::string made = one.text();

    std::replace(made.begin(), made.end(), '/', '\\');

    return made;
}

// The first bytes of a file, up to `peeked`; empty when it cannot be read.
Task<Bytes> head_of(Port<fs::Files> files, Path file) {
    try {
        Task<Bytes> reading = files->read(file, fs::Range{0, peeked});

        co_return co_await std::move(reading);
    } catch (const Trouble&) {
        co_return Bytes{};
    }
}

// The whole text of a file; nothing when it cannot be read.
Task<std::optional<std::string>> text_of(Port<fs::Files> files, Path file) {
    try {
        Task<Bytes> reading = files->read(file);
        const Bytes got = co_await std::move(reading);

        co_return std::string{reinterpret_cast<const char*>(got.data()), got.size()};
    } catch (const Trouble&) {
        co_return std::nullopt;
    }
}

// A program started in the file's folder with the file as its only argument, let loose.
Command plainly(const Path& program, const Path& file) {
    Command made;

    made.program = program;
    made.arguments = {spelled(file)};
    made.folder = file.parent();
    made.mode = Mode::detached;

    return made;
}

// Starts the command, which is let loose (`Mode::detached`), through `processes`: opened.
Task<Opened> loosed(Port<Processes> processes, Command what) {
    Task<Process> starting = processes->start(std::move(what));

    (void)co_await std::move(starting);

    co_return Opened{};
}

#ifdef _WIN32

// COM in a single-threaded apartment for the time of a shell call, without OLE1 DDE; left on the
// way out only when it was joined here.
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

    std::string made = text::narrow(given);

    CoTaskMemFree(given);

    return made;
}

// Whether two paths name the same program: compared without regard to letter case, as Windows
// compares paths.
bool alike(const std::string& one, const std::string& other) {
    const std::wstring left = text::wide(one);
    const std::wstring right = text::wide(other);

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

    return text::narrow(made);
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
        found.name = SUCCEEDED(one->GetUIName(&shown)) ? taken(shown) : std::string{Path{found.id}.stem()};
        found.preferred = !usual.empty() && alike(found.id, usual);

        made.push_back(std::move(found));

        return false;
    });

    std::stable_partition(made.begin(), made.end(), [](const Application& one) { return one.preferred; });

    return made;
}

// The extension of the file with its dot, for the shell; empty when it has none.
std::wstring dotted(const Path& file) {
    const std::string_view extension = file.extension();

    return extension.empty() ? std::wstring{} : L"." + text::wide(extension);
}

// Opens the file in the program the system gives its type; true when it opened, false when the
// type has no program. ShellExecuteEx waits for the shell, not for the program, and keeps its own
// warning for a file from the internet, since no flag here switches that check off. No message box
// of its own: an error comes back to the caller.
bool executed(const Path& file) {
    const Apartment apartment;

    const std::wstring path = text::wide(spelled(file));
    const std::wstring folder = text::wide(spelled(file.parent()));

    SHELLEXECUTEINFOW asked{};

    asked.cbSize = sizeof(asked);
    asked.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    asked.lpFile = path.c_str();
    asked.lpDirectory = folder.empty() ? nullptr : folder.c_str();
    asked.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&asked)) return true;

    const DWORD told = GetLastError();

    if (told == ERROR_NO_ASSOCIATION) return false;

    throw Trouble{Kind::other, "The system did not open the file.", file.text(), static_cast<int>(told)};
}

// Opens the file with the shell's handler whose program is `id`; false when no handler of the
// extension has that program.
bool invoked(const Path& file, const std::string& id) {
    const Apartment apartment;

    bool done = false;
    HRESULT told = S_OK;

    handled(dotted(file), ASSOC_FILTER_NONE, [&](Held<IAssocHandler>& one, const std::string& path) {
        if (!alike(path, id)) return false;

        Held<IShellItem> item;

        told = SHCreateItemFromParsingName(text::wide(spelled(file)).c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.out()));

        if (FAILED(told)) return true;

        Held<IDataObject> data;

        told = item->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject, reinterpret_cast<void**>(data.out()));

        if (FAILED(told)) return true;

        told = one->Invoke(data.get());
        done = SUCCEEDED(told);

        return true;
    });

    if (FAILED(told)) throw Trouble{Kind::other, "The program did not open the file.", file.text(), static_cast<int>(HRESULT_CODE(told))};

    return done;
}

// The system's own window for choosing a program, which opens the file with the chosen one. It
// does not offer to remember the choice: the caller keeps its own and changes nothing in the
// system. Closing the window cancels.
void picked(const Path& file) {
    const Apartment apartment;

    const std::wstring path = text::wide(spelled(file));

    OPENASINFO asked{};

    asked.pcszFile = path.c_str();
    asked.oaifInFlags = OAIF_EXEC;

    const HRESULT told = SHOpenWithDialog(nullptr, &asked);

    if (told == HRESULT_FROM_WIN32(ERROR_CANCELLED)) throw Cancelled{};

    if (FAILED(told)) throw Trouble{Kind::other, "The system window for choosing a program did not open.", file.text(), static_cast<int>(HRESULT_CODE(told))};
}

#else

// The entry at `file`; nothing when there is none.
Task<std::optional<fs::Entry>> entry_at(Port<fs::Files> files, Path file) {
    try {
        Task<fs::Entry> looking = files->look(file);

        co_return co_await std::move(looking);
    } catch (const Trouble&) {
        co_return std::nullopt;
    }
}

// Whether the entry is a file, itself or behind a link.
bool filed(const fs::Entry& one) { return one.type == fs::Type::file || (one.type == fs::Type::link && one.reach == fs::Type::file); }

// Whether anyone may run the entry.
bool runs(const fs::Entry& one) { return one.rights.has_value() && (*one.rights & 0111U) != 0; }

// What the Linux desktop reads, from the console's variables: the XDG directories, the locale of
// names (LC_ALL, then LC_MESSAGES, then LANG, as gettext orders them) and PATH for TryExec.
struct Seen {
    desktops::Environment environment;
    std::string locale;
    std::string path;
    bool windowed = false;
};

// What the console says.
Seen seen_from(const Console& from) {
    Seen made;

    made.environment.home = from.variable("HOME");
    made.environment.config_home = from.variable("XDG_CONFIG_HOME");
    made.environment.config_dirs = from.variable("XDG_CONFIG_DIRS");
    made.environment.data_home = from.variable("XDG_DATA_HOME");
    made.environment.data_dirs = from.variable("XDG_DATA_DIRS");
    made.environment.current = from.variable("XDG_CURRENT_DESKTOP");

    for (const char* one : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        made.locale = from.variable(one).value_or(std::string{});

        if (!made.locale.empty()) break;
    }

    made.path = from.variable("PATH").value_or(std::string{});
    made.windowed = windowed(from);

    return made;
}

// An installed program: its desktop entry and where the entry lies.
struct Installed {
    desktops::Entry entry;
    Path location;
};

// The Linux desktop for one question: the places, the locale and PATH, and what has been read so
// far, so that one question reads each file once.
class Reading {
public:
    // Reads nothing yet.
    Reading(Port<fs::Files> files, Seen seen) : files_(std::move(files)), seen_(std::move(seen)), places_(desktops::places(seen_.environment)) {}

    // The type of the file: a folder is inode/directory; otherwise by the globs, and a name no glob
    // knows is text/plain when its first bytes are text and application/octet-stream when not.
    Task<std::string> type_of(Path file) {
        const std::optional<fs::Entry> found = co_await entry_at(files_, file);

        if (found && (found->type == fs::Type::folder || (found->type == fs::Type::link && found->reach == fs::Type::folder)))
            co_return std::string{"inode/directory"};

        std::optional<std::string> known = co_await named(std::string{file.name()});

        if (known) co_return std::move(*known);

        const Bytes head = co_await head_of(files_, file);

        co_return desktops::textual(head) ? std::string{"text/plain"} : std::string{"application/octet-stream"};
    }

    // The type of a name by the globs alone; nothing when no glob knows it.
    Task<std::optional<std::string>> named(std::string name) {
        if (!globs_) {
            globs_.emplace();

            for (const Path& folder : places_.mime) {
                const std::optional<std::string> text = co_await text_of(files_, folder / "globs2");

                if (!text) continue;

                std::vector<desktops::Glob> more = desktops::globs(*text);

                globs_->insert(globs_->end(), more.begin(), more.end());
            }
        }

        co_return desktops::type_of(name, *globs_);
    }

    // The type with the types it descends from, nearest first: the subclasses of every MIME
    // folder, and text/plain under every text/ type, as the specification makes every text a
    // plain text. application/octet-stream, the parent of all, is left out, as it would offer a
    // hex editor for everything.
    Task<std::vector<std::string>> lineage(std::string type) {
        if (!parents_) {
            parents_.emplace();

            for (const Path& folder : places_.mime) {
                const std::optional<std::string> text = co_await text_of(files_, folder / "subclasses");

                if (!text) continue;

                for (auto& [child, list] : desktops::parents(*text)) {
                    std::vector<std::string>& into = (*parents_)[child];

                    into.insert(into.end(), list.begin(), list.end());
                }
            }
        }

        std::vector<std::string> made{std::move(type)};

        for (std::size_t at = 0; at < made.size(); ++at) {
            std::vector<std::string> next;

            if (const auto found = parents_->find(made[at]); found != parents_->end()) next = found->second;

            if (made[at].starts_with("text/")) next.emplace_back("text/plain");

            for (std::string& one : next)
                if (one != "application/octet-stream" && std::find(made.begin(), made.end(), one) == made.end()) made.push_back(std::move(one));
        }

        co_return made;
    }

    // The defaults of the type from every mimeapps.list, most important first.
    Task<std::vector<std::string>> defaults_of(std::string type) {
        co_await lists();

        std::vector<std::string> made;

        for (const desktops::Associations& one : *lists_)
            if (const auto found = one.defaults.find(type); found != one.defaults.end())
                for (const std::string& id : found->second)
                    if (std::find(made.begin(), made.end(), id) == made.end()) made.push_back(id);

        co_return made;
    }

    // The programs associated with the type: the added associations of each mimeapps.list in
    // order, then the caches of the applications folders, without what a list of higher or equal
    // importance removed. A removal reaches only the lists below its own and the caches.
    Task<std::vector<std::string>> associated(std::string type) {
        co_await lists();
        co_await caches();

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

        co_return made;
    }

    // The installed program of the id: an absolute path is a desktop file itself; otherwise the
    // first applications folder that has the file, also under a subfolder, which the id writes with
    // a dash ("kde4-dolphin.desktop" is kde4/dolphin.desktop). Nothing when it is missing, deleted
    // (Hidden), no application, without a command line, or when its TryExec program is not there.
    Task<std::optional<Installed>> installed(std::string id) {
        if (const auto found = known_.find(id); found != known_.end()) co_return found->second;

        std::optional<Installed> made;

        for (const Path& at : locations(id)) {
            const std::optional<std::string> text = co_await text_of(files_, at);

            if (!text) continue;

            std::optional<desktops::Entry> read = desktops::entry(*text, seen_.locale);
            bool there = read && read->application && !read->hidden && !read->exec.empty();

            if (there && !read->try_exec.empty()) there = co_await present(read->try_exec);

            if (there) made = Installed{std::move(*read), at};

            break;
        }

        known_.emplace(id, made);

        co_return made;
    }

    // The default program of the file's type, searched along its lineage, nearest type first: for
    // each type its defaults, then its associated programs, the first one installed, before its
    // parent is asked, so that a program made for the type wins over the default of a broader one
    // (a reader of .nfo files over the plain text editor).
    Task<std::optional<std::pair<std::string, Installed>>> chosen_for(std::vector<std::string> line) {
        for (const std::string& type : line) {
            std::vector<std::string> ids = co_await defaults_of(type);
            std::vector<std::string> more = co_await associated(type);

            ids.insert(ids.end(), more.begin(), more.end());

            for (const std::string& id : ids) {
                std::optional<Installed> found = co_await installed(id);

                if (found) co_return std::pair{id, std::move(*found)};
            }
        }

        co_return std::nullopt;
    }

    // The programs of a type and the types it descends from (`line`, nearest first): the default
    // one first and marked `preferred`; then, type by type, the defaults and the associated
    // programs, each once, without those hidden from lists (NoDisplay).
    Task<std::vector<Application>> offered(std::vector<std::string> line) {
        std::vector<Application> made;
        std::set<std::string, std::less<>> taken;

        const auto put = [&made, &taken](const std::string& id, const Installed& one, bool preferred) {
            if (!taken.insert(id).second) return;

            made.push_back(Application{one.entry.name.empty() ? id : one.entry.name, id, preferred, one.entry.terminal});
        };

        const std::optional<std::pair<std::string, Installed>> first = co_await chosen_for(line);

        if (first) put(first->first, first->second, true);

        for (const std::string& type : line) {
            std::vector<std::string> ids = co_await defaults_of(type);
            std::vector<std::string> more = co_await associated(type);

            ids.insert(ids.end(), more.begin(), more.end());

            for (const std::string& id : ids) {
                const std::optional<Installed> one = co_await installed(id);

                if (one && one->entry.shown) put(id, *one, false);
            }
        }

        co_return made;
    }

    // Whether there is a graphical session.
    [[nodiscard]] bool windowed() const { return seen_.windowed; }

private:
    // The mimeapps.list files, read once.
    Task<void> lists() {
        if (lists_) co_return;

        lists_.emplace();

        for (const Path& one : places_.lists) {
            const std::optional<std::string> text = co_await text_of(files_, one);

            if (text) lists_->push_back(desktops::associations(*text));
        }
    }

    // The caches of the applications folders, read once. A folder without mimeinfo.cache (often the
    // user's own, where update-desktop-database is not run) gets one made from its desktop files.
    Task<void> caches() {
        if (caches_) co_return;

        caches_.emplace();

        for (const Path& folder : places_.applications) {
            const std::optional<std::string> cache = co_await text_of(files_, folder / "mimeinfo.cache");

            if (cache) {
                caches_->push_back(desktops::cached(*cache));
                continue;
            }

            desktops::Lists made;
            std::vector<fs::Entry> all;

            try {
                Task<std::vector<fs::Entry>> walking = files_->walk(folder);

                all = co_await std::move(walking);
            } catch (const Trouble&) {
                all.clear();
            }

            for (const fs::Entry& one : all) {
                if (one.where.extension() != "desktop" || !filed(one)) continue;

                const std::optional<std::string> text = co_await text_of(files_, one.where);

                if (!text) continue;

                const std::optional<desktops::Entry> read = desktops::entry(*text, seen_.locale);

                if (!read) continue;

                std::string id{one.where.stored().substr(folder.stored().size() + 1)};

                std::replace(id.begin(), id.end(), '/', '-');

                for (const std::string& type : read->types) made[type].push_back(id);
            }

            caches_->push_back(std::move(made));
        }
    }

    // Where the desktop file of an id may lie, in order.
    [[nodiscard]] std::vector<Path> locations(const std::string& id) const {
        if (!id.empty() && id.front() == '/') return {Path{id}};

        std::vector<Path> made;

        for (const Path& folder : places_.applications) {
            made.push_back(folder / id);

            std::string nested = id;

            for (std::size_t dash = nested.find('-'); dash != std::string::npos; dash = nested.find('-', dash + 1)) {
                nested[dash] = '/';

                made.push_back(folder / nested);
            }
        }

        return made;
    }

    // Whether a program named in TryExec is there: a path anyone may run, or a name found so in a
    // folder of PATH.
    Task<bool> present(std::string program) {
        if (program.find('/') != std::string::npos) co_return co_await executable(Path{program});

        std::string_view rest = seen_.path;

        while (true) {
            const std::size_t colon = rest.find(':');
            const std::string_view folder = rest.substr(0, colon);

            if (!folder.empty()) {
                const bool found = co_await executable(Path{folder} / program);

                if (found) co_return true;
            }

            if (colon == std::string_view::npos) co_return false;

            rest.remove_prefix(colon + 1);
        }
    }

    // Whether the file is there and anyone may run it.
    Task<bool> executable(Path file) {
        const std::optional<fs::Entry> found = co_await entry_at(files_, file);

        co_return found && filed(*found) && runs(*found);
    }

    Port<fs::Files> files_;
    Seen seen_;
    desktops::Places places_;
    std::optional<std::vector<desktops::Glob>> globs_;
    std::optional<desktops::Lists> parents_;
    std::optional<std::vector<desktops::Associations>> lists_;
    std::optional<std::vector<desktops::Lists>> caches_;
    std::map<std::string, std::optional<Installed>, std::less<>> known_;
};

// The command line of an installed program for the file (none for a launcher started by itself),
// started in the file's folder, or in the entry's when there is no file. Nothing when the Exec line
// cannot be read.
std::optional<Command> commanded(const Installed& one, const Path& file) {
    const std::string named = file.empty() ? std::string{} : spelled(file);
    const std::optional<std::vector<std::string>> words = desktops::expanded(one.entry, named, spelled(one.location));

    if (!words) return std::nullopt;

    Command made;

    made.program = Path{words->front()};
    made.arguments.assign(words->begin() + 1, words->end());
    made.folder = file.empty() ? one.location.parent() : file.parent();
    made.mode = one.entry.terminal ? Mode::terminal : Mode::detached;

    return made;
}

// The program and the file carried out: a terminal program goes back to the caller with its
// command line, nothing opens without a session, and otherwise the program is let loose.
Task<Opened> planned(Port<Processes> processes, const Installed& one, Path file, bool session) {
    std::optional<Command> made = commanded(one, file);

    if (!made) throw Trouble{Kind::malformed, "The program's desktop entry has no usable command line.", one.location.text()};

    if (one.entry.terminal) co_return Opened{Started::terminal, std::move(*made)};

    if (!session) co_return Opened{Started::sessionless, {}};

    co_return co_await loosed(std::move(processes), std::move(*made));
}

// Whether the entry is a desktop entry that runs as a program: a .desktop file anyone may run, as
// desktops ask for before they start a launcher they did not install themselves.
bool launcher(const fs::Entry& one) { return one.where.extension() == "desktop" && filed(one) && runs(one); }

#endif

class System final : public Desktop {
public:
    // A desktop learning the session and the environment from `console`, letting programs loose
    // through `processes` and reading files through `files`.
    System(Port<Console> console, Port<Processes> processes, Port<fs::Files> files)
        : console_(std::move(console)), processes_(std::move(processes)), files_(std::move(files)) {}

    // The desktop's name in messages.
    [[nodiscard]] std::string_view name() const override { return "system"; }

    // The command that runs the file when it is a program, in its folder: a program with windows
    // let loose, a console program and a script on a terminal. Windows knows a program by its
    // extension and its PE header, and runs a batch file through cmd.exe; elsewhere a program has
    // its execute bit and the bytes of one (`desktops::posix_runnable`), and a launcher (a .desktop
    // file anyone may run) runs its Exec line. Nothing for anything else.
    Task<std::optional<Command>> program(Path file) override {
        desktops::Runnable how = desktops::Runnable::no;

#ifdef _WIN32
        const std::string extension = desktops::lowered(file.extension());
        Bytes head;

        if (extension == "exe") head = co_await head_of(files_, file);

        how = desktops::windows_runnable(extension, head);
#else
        const std::optional<fs::Entry> found = co_await entry_at(files_, file);

        if (!found) co_return std::nullopt;

        if (launcher(*found)) {
            const std::optional<std::string> text = co_await text_of(files_, file);
            const std::optional<desktops::Entry> read = text ? desktops::entry(*text, "") : std::nullopt;

            if (!read || !read->application) co_return std::nullopt;

            co_return commanded(Installed{*read, file}, Path{});
        }

        const Bytes head = co_await head_of(files_, file);

        how = desktops::posix_runnable(head, runs(*found));
#endif

        if (how == desktops::Runnable::no) co_return std::nullopt;

        Command made;

        made.program = file;
        made.folder = file.parent();
        made.mode = how == desktops::Runnable::window ? Mode::detached : Mode::terminal;

        if (how == desktops::Runnable::script && windows) {
            made.program = Path{"cmd.exe"};
            made.arguments = {"/c", spelled(file)};
        }

        co_return made;
    }

    // Where the file came from: the zone Windows wrote when it was downloaded (its stream
    // Zone.Identifier), elsewhere here.
    Task<Origin> origin(Path file) override {
        if constexpr (!windows) co_return Origin::here;

        const std::optional<std::string> zone = co_await text_of(files_, Path{std::string{file.stored()} + ":Zone.Identifier"});

        co_return zone ? desktops::zone_of(*zone) : Origin::here;
    }

    // Opens the file in the default program of its type. On Linux a terminal program comes back
    // with its command line even without a session, and a type nobody opens says so.
    Task<Opened> open(Path file) override {
#ifdef _WIN32
        if (!windowed(*console_)) co_return Opened{Started::sessionless, {}};

        const bool done = co_await platform::offloaded([file] { return executed(file); });

        co_return Opened{done ? Started::opened : Started::unassociated, {}};
#else
        Reading reading{files_, seen_from(*console_)};
        const std::optional<fs::Entry> found = co_await entry_at(files_, file);

        if (found && launcher(*found)) {
            const std::optional<std::string> text = co_await text_of(files_, file);
            const std::optional<desktops::Entry> read = text ? desktops::entry(*text, "") : std::nullopt;

            if (!read || !read->application) throw Trouble{Kind::malformed, "The desktop entry does not start a program.", file.text()};

            co_return co_await planned(processes_, Installed{*read, file}, Path{}, reading.windowed());
        }

        std::string type = co_await reading.type_of(file);
        std::vector<std::string> line = co_await reading.lineage(std::move(type));
        const std::optional<std::pair<std::string, Installed>> chosen = co_await reading.chosen_for(std::move(line));

        if (!chosen) co_return Opened{Started::unassociated, {}};

        co_return co_await planned(processes_, chosen->second, file, reading.windowed());
#endif
    }

    // The programs that open the file, the default one first and marked `preferred`, then the
    // others.
    Task<std::vector<Application>> applications(Path file) override {
#ifdef _WIN32
        co_return co_await platform::offloaded([file] { return recommended(dotted(file)); });
#else
        Reading reading{files_, seen_from(*console_)};
        std::string type = co_await reading.type_of(file);
        std::vector<std::string> line = co_await reading.lineage(std::move(type));

        co_return co_await reading.offered(std::move(line));
#endif
    }

    // The programs that open files of the extension (with its dot), as for such a file, without a
    // file to look at: Linux knows the type by the globs alone, matched against the extension as a
    // whole name (which *.ext fits), and an extension no glob knows has no programs.
    Task<std::vector<Application>> applications_for(std::string extension) override {
#ifdef _WIN32
        co_return co_await platform::offloaded([extension] { return recommended(text::wide(extension)); });
#else
        if (extension.size() < 2 || extension.front() != '.') co_return std::vector<Application>{};

        Reading reading{files_, seen_from(*console_)};
        const std::optional<std::string> type = co_await reading.named(extension);

        if (!type) co_return std::vector<Application>{};

        std::vector<std::string> line = co_await reading.lineage(*type);

        co_return co_await reading.offered(std::move(line));
#endif
    }

    // Opens the file with one of the programs `applications` gave.
    Task<Opened> open_with(Path file, Application with) override {
#ifdef _WIN32
        if (!windowed(*console_)) co_return Opened{Started::sessionless, {}};

        const bool done = co_await platform::offloaded([file, id = with.id] { return invoked(file, id); });

        if (done) co_return Opened{};

        co_return co_await loosed(processes_, plainly(Path{with.id}, file));
#else
        Reading reading{files_, seen_from(*console_)};
        const std::optional<Installed> found = co_await reading.installed(with.id);

        if (!found) throw Trouble{Kind::missing, "The program is no longer installed.", with.id};

        co_return co_await planned(processes_, *found, file, reading.windowed());
#endif
    }

    // Opens the file with a program the user named: a path, or a name the system looks up.
    Task<Opened> open_by(Path file, Path program) override {
        if (!windowed(*console_)) co_return Opened{Started::sessionless, {}};

        co_return co_await loosed(processes_, plainly(program, file));
    }

    // The system's own window for choosing a program: Windows only; elsewhere refused, as the
    // task's failure.
    Task<Opened> chosen(Path file) override {
#ifdef _WIN32
        if (!windowed(*console_)) co_return Opened{Started::sessionless, {}};

        co_await platform::offloaded([file] { picked(file); });

        co_return Opened{};
#else
        throw Trouble{Kind::unsupported, "This system has no window for choosing a program.", file.text()};

        co_return Opened{};
#endif
    }

private:
    Port<Console> console_;
    Port<Processes> processes_;
    Port<fs::Files> files_;
};

// The extension of the file in lower case with its dot; empty when it has none.
std::string dotted_name(const Path& file) {
    const std::string_view extension = file.extension();

    return extension.empty() ? std::string{} : "." + desktops::lowered(extension);
}

}

// The desktop of the system on the program's console, processes and disk.
std::shared_ptr<Desktop> Desktop::builtin() { return std::make_shared<System>(Port<Console>{}, Port<Processes>{}, Port<fs::Files>{}); }

// The desktop of the system, learning the session and the environment from `console`, letting
// programs loose through `processes` and reading files through `files`.
Port<Desktop> desktop(Port<Console> console, Port<Processes> processes, Port<fs::Files> files) {
    return Port<Desktop>{std::make_shared<System>(std::move(console), std::move(processes), std::move(files))};
}

// The command set for the file, when there is one.
Task<std::optional<Command>> ScriptedDesktop::program(Path file) {
    const auto found = programs.find(file);

    co_return found == programs.end() ? std::nullopt : std::optional<Command>{found->second};
}

// Internet for a file marked downloaded, here for the rest.
Task<Origin> ScriptedDesktop::origin(Path file) { co_return downloaded.contains(file) ? Origin::internet : Origin::here; }

// Notes the request and answers as set, or fails with the refusal set.
Task<Opened> ScriptedDesktop::open(Path file) {
    requests.push_back(Request{Way::open, std::move(file), {}});

    if (refusal) throw *refusal;

    co_return answer;
}

// The programs offered for the file's extension, in any case of letters.
Task<std::vector<Application>> ScriptedDesktop::applications(Path file) {
    const auto found = offered.find(dotted_name(file));

    co_return found == offered.end() ? std::vector<Application>{} : found->second;
}

// The programs offered for the extension, in any case of letters.
Task<std::vector<Application>> ScriptedDesktop::applications_for(std::string extension) {
    const auto found = offered.find(desktops::lowered(extension));

    co_return found == offered.end() ? std::vector<Application>{} : found->second;
}

// Notes the request with the program's id and answers as set, or fails with the refusal set.
Task<Opened> ScriptedDesktop::open_with(Path file, Application with) {
    requests.push_back(Request{Way::with, std::move(file), std::move(with.id)});

    if (refusal) throw *refusal;

    co_return answer;
}

// Notes the request with the program's path and answers as set, or fails with the refusal set.
Task<Opened> ScriptedDesktop::open_by(Path file, Path program) {
    requests.push_back(Request{Way::by, std::move(file), std::string{program.stored()}});

    if (refusal) throw *refusal;

    co_return answer;
}

// Notes the request and answers as set, or fails with the refusal set.
Task<Opened> ScriptedDesktop::chosen(Path file) {
    requests.push_back(Request{Way::chosen, std::move(file), {}});

    if (refusal) throw *refusal;

    co_return answer;
}

}

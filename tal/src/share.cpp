#include "tal/share.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/fs.hpp"
#include "tal/process.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "sharing.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <lm.h>
#include <wincred.h>
#include <winnetwk.h>

#include "platform/offload.hpp"
#endif

// Network shares, ported from talcite's share.cpp. An address in any of four forms (\\NAS\films,
// //NAS/films, smb://NAS/films, smb://user@192.168.1.10/films) is split into the server, the share,
// the rest of the path and the user (`addressed`), and the share is reached through the system:
//
// - Windows: the UNC path itself. When the system refuses, WNetAddConnection2W without a letter and
//   for this session alone: first without a login (the system uses the stored ones), then with the
//   one given. Remembering is an entry of the Windows Credential Manager (CredWriteW, a domain
//   password of the server, as cmdkey /add does), written only after the connection succeeded, so
//   a wrong password does not stay in the system; and only when the programmer built it in
//   (TAL_REMEMBER_LOGINS). The list of shares comes from NetShareEnum.
// - Elsewhere: GVfs. A mounted share lies in $XDG_RUNTIME_DIR/gvfs under a name such as
//   smb-share:server=...,share=...; one not mounted is mounted by `gio mount`, whose questions get
//   the user, the domain and the password on its input, never in its arguments, which ps shows.
//   The list comes from `gio list`. gio speaks English here (LC_ALL=C), or its reasons could not be
//   read. Without gio, without GVfs's FUSE or without XDG_RUNTIME_DIR it is unsupported.
//
// Every copy of a password that tal holds is wiped right after the call (the one that goes to gio's
// input belongs to the process that writes it).
namespace tal::fs {

namespace {

// The letter `one` in lowercase, ASCII only.
char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

// `text` in lowercase, ASCII only.
std::string lowered(std::string_view text) {
    std::string made{text};

    std::transform(made.begin(), made.end(), made.begin(), [](char one) { return lowered(one); });

    return made;
}

// `text` without the white space around it.
std::string_view trimmed(std::string_view text) {
    const std::size_t from = text.find_first_not_of(" \t\r\n");

    if (from == std::string_view::npos) return {};

    return text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1);
}

// The value of the hexadecimal digit `one`, or -1.
int nibble(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// `text` with its %XX escapes decoded; a % without two digits stays.
std::string unescaped(std::string_view text) {
    std::string made;

    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '%' && at + 2 < text.size() && nibble(text[at + 1]) >= 0 && nibble(text[at + 2]) >= 0) {
            made += static_cast<char>(nibble(text[at + 1]) * 16 + nibble(text[at + 2]));
            at += 2;
        } else {
            made += text[at];
        }
    }

    return made;
}

// The parts of `said` cut at both kinds of slash, none empty.
std::vector<std::string> pieces(std::string_view said) {
    std::vector<std::string> made;
    std::string one;

    for (const char sign : said) {
        if (sign == '/' || sign == '\\') {
            if (!one.empty()) made.push_back(std::move(one));

            one.clear();
        } else {
            one += sign;
        }
    }

    if (!one.empty()) made.push_back(std::move(one));

    return made;
}

// The trouble of the kind `kind` saying `said` about `one`.
Trouble failed(Kind kind, std::string said, const Share& one) {
    if (said.empty()) said = "The share cannot be reached.";

    return Trouble{kind, std::move(said), written(one)};
}

}

namespace sharing {

// The share of the GVfs folder `name` of a mounted share, smb-share:server=nas,share=films with
// user= and domain= perhaps, its values percent-encoded; nothing for any other folder (sftp:..., or
// smb-server:..., a server alone).
std::optional<Share> gvfs_named(std::string_view name) {
    if (!name.starts_with("smb-share:")) return std::nullopt;

    Share made;
    std::string_view rest = name.substr(10);

    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view pair = rest.substr(0, comma);

        if (const std::size_t equal = pair.find('='); equal != std::string_view::npos) {
            const std::string_view key = pair.substr(0, equal);
            std::string value = unescaped(pair.substr(equal + 1));

            if (key == "server") made.server = std::move(value);
            else if (key == "share") made.name = std::move(value);
            else if (key == "user") made.user = std::move(value);
        }

        if (comma == std::string_view::npos) break;

        rest = rest.substr(comma + 1);
    }

    if (made.server.empty() || made.name.empty()) return std::nullopt;

    return made;
}

// The GVfs folder of `one` among the mounted `names`; server and share ignore case, as GVfs writes
// them in lowercase, and a mount of the address's user goes first.
std::optional<std::string> mounted_as(const std::vector<std::string>& names, const Share& one) {
    std::optional<std::string> found;

    for (const std::string& name : names) {
        const std::optional<Share> mounted = gvfs_named(name);

        if (!mounted || lowered(mounted->server) != lowered(one.server) || lowered(mounted->name) != lowered(one.name)) continue;

        if (!one.user.empty() && mounted->user == one.user) return name;

        if (!found) found = name;
    }

    return found;
}

// The input of `gio mount`: the user, the domain and the password, a line each, as gio asks for
// them; the domain from DOMAIN\user or DOMAIN;user, empty for the default.
std::string answers(std::string_view user, std::string_view password) {
    std::string_view domain;

    if (const std::size_t cut = user.find_first_of("\\;"); cut != std::string_view::npos) {
        domain = user.substr(0, cut);
        user = user.substr(cut + 1);
    }

    std::string made;

    made.reserve(user.size() + domain.size() + password.size() + 3);
    made.append(user).append("\n").append(domain).append("\n").append(password).append("\n");

    return made;
}

// The names of the shares that `gio list smb://server/` printed in `said`, a name a line; those
// that end in $ (IPC$, ADMIN$, print$) are hidden and stay out.
std::vector<std::string> listed(std::string_view said) {
    std::vector<std::string> made;

    while (!said.empty()) {
        const std::size_t end = said.find('\n');
        std::string one{trimmed(said.substr(0, end))};

        if (!one.empty() && one.back() == '/') one.pop_back();

        if (!one.empty() && one.back() != '$') made.push_back(std::move(one));

        if (end == std::string_view::npos) break;

        said = said.substr(end + 1);
    }

    return made;
}

// The kind of trouble of gio's English message `said`; the first group that matches wins: the
// network, the login ("cancelled" among it, since so ends a question of a password with no more
// answers), what is not there, what GVfs cannot do.
Kind by_gio(std::string_view said) {
    const std::string text = lowered(said);
    const auto any = [&text](std::initializer_list<std::string_view> among) {
        return std::any_of(among.begin(), among.end(), [&text](std::string_view one) { return text.find(one) != std::string::npos; });
    };

    if (any({"timed out", "connection refused", "no route to host", "network is unreachable", "host is down", "name or service not known", "could not resolve",
             "unable to resolve", "host is unreachable"}))
        return Kind::broken;

    if (any({"permission denied", "access denied", "password", "authentication", "logon failure", "cancelled", "canceled", "aborted"})) return Kind::denied;

    if (any({"no such file", "not found", "does not exist", "no such share"})) return Kind::missing;

    if (any({"not supported", "unsupported", "not mounted", "no such mount", "implement mount"})) return Kind::unsupported;

    return Kind::other;
}

// The kind of trouble of the Windows or network error `code`; the person giving up in the system's
// window (1223) is refused.
Kind by_windows(std::uint32_t code) {
    switch (code) {
        case 5:
        case 86:
        case 1244:
        case 1326:
        case 1327:
        case 1330:
        case 1331:
        case 1385:
        case 2202: return Kind::denied;
        case 67:
        case 2310: return Kind::missing;
        case 51:
        case 53:
        case 59:
        case 64:
        case 121:
        case 1203:
        case 1222:
        case 1231:
        case 1232: return Kind::broken;
        case 1219: return Kind::exists;
        case 50:
        case 1200:
        case 1204: return Kind::unsupported;
        case 1223: return Kind::refused;
        default: return Kind::other;
    }
}

// The program of GVfs, gio; the trials put their own in its place.
Path& gio() {
    static Path one{"gio"};

    return one;
}

}

// The share of the address `said` in UNC, //server or smb:// form; nothing for anything else or
// without a server. A user stands only in a URL (smb://user@nas/films); a password there
// (user:secret@) goes without a trace, as nothing keeps passwords and the address is remembered.
std::optional<Share> addressed(std::string_view said) {
    const std::string whole{trimmed(said)};
    Share made;
    std::string_view inside;
    bool url = false;

    if (lowered(whole).starts_with("smb://")) {
        inside = std::string_view{whole}.substr(6);
        url = true;
    } else if (whole.starts_with("\\\\") || whole.starts_with("//")) {
        inside = std::string_view{whole}.substr(2);
    } else {
        return std::nullopt;
    }

    if (url) {
        const std::size_t slash = inside.find('/');

        if (const std::size_t at = inside.substr(0, slash).rfind('@'); at != std::string_view::npos) {
            const std::string_view who = inside.substr(0, at);

            made.user = unescaped(who.substr(0, who.find(':')));
            inside = inside.substr(at + 1);
        }
    }

    std::vector<std::string> parts = pieces(inside);

    if (parts.empty()) return std::nullopt;

    if (url)
        for (std::string& one : parts) one = unescaped(one);

    made.server = parts[0];

    if (parts.size() > 1) made.name = parts[1];

    for (std::size_t at = 2; at < parts.size(); ++at) made.rest += (at > 2 ? "/" : "") + parts[at];

    return made;
}

// The address of `one` as people see and remember it, in the system's notation: UNC on Windows, a
// URL elsewhere.
std::string written(const Share& one) {
#ifdef _WIN32
    std::string made = "\\\\" + one.server;

    if (!one.name.empty()) made += "\\" + one.name;

    if (!one.rest.empty()) {
        std::string rest = one.rest;

        std::replace(rest.begin(), rest.end(), '/', '\\');
        made += "\\" + rest;
    }

    return made;
#else
    std::string made = "smb://" + (one.user.empty() ? std::string{} : one.user + "@") + one.server;

    if (!one.name.empty()) made += "/" + one.name;
    if (!one.rest.empty()) made += "/" + one.rest;

    return made;
#endif
}

// Whether this build can remember logins of shares: on Windows, with TAL_REMEMBER_LOGINS.
bool keeps_logins() {
#if defined(_WIN32) && defined(TAL_REMEMBER_LOGINS)
    return true;
#else
    return false;
#endif
}

#ifdef _WIN32

namespace {

// The message of the Windows error `code`, in UTF-8.
std::string spoken(DWORD code) {
    wchar_t* said = nullptr;
    const DWORD much = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                      reinterpret_cast<wchar_t*>(&said), 0, nullptr);
    std::string made = much == 0 ? "Windows error " + std::to_string(code) + "." : std::string{trimmed(text::narrow(std::wstring_view{said, much}))};

    if (said != nullptr) (void)LocalFree(said);

    return made;
}

// Zeroes and empties the wide secret `secret`.
void blanked(std::wstring& secret) {
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));

    secret.clear();
}

// The root of `one` in UNC; without a share, IPC$, through which the login to the server itself
// goes (for its list of shares).
std::wstring rooted(const Share& one) { return L"\\\\" + text::wide(one.server) + L"\\" + (one.name.empty() ? std::wstring{L"IPC$"} : text::wide(one.name)); }

// The UNC path of `one`: the root of the share and the rest.
Path whole(const Share& one) {
    std::string rest = one.rest;

    std::replace(rest.begin(), rest.end(), '/', '\\');

    return Path{"\\\\" + one.server + "\\" + one.name + (rest.empty() ? std::string{} : "\\" + rest)};
}

// NO_ERROR when the system answers for the root of `one`, the error otherwise; the rest of the
// path is the caller's business, as with any entry.
DWORD standing(const Share& one) {
    const std::wstring root = rooted(one) + L"\\";

    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) return NO_ERROR;

    return GetLastError();
}

// Connects to `one` (or to the server's IPC$) without a letter and for this session; null logins
// mean the stored ones.
DWORD joined(const Share& one, const wchar_t* user, const wchar_t* password) {
    std::wstring remote = rooted(one);
    NETRESOURCEW resource{};

    resource.dwType = one.name.empty() ? RESOURCETYPE_ANY : RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();

    return WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY);
}

// The UNC path of `one`, reached without asking for a login; a refusal tries once more through a
// connection without one, since then the system takes the stored login, which the bare path does
// not always do.
Path reached_at(const Share& one) {
    DWORD code = standing(one);

    if (code != NO_ERROR && sharing::by_windows(code) == Kind::denied) {
        if (const DWORD tried = joined(one, nullptr, nullptr); tried == NO_ERROR) code = standing(one);
        else code = tried;
    }

    if (code != NO_ERROR) throw failed(sharing::by_windows(code), spoken(code), one);

    return whole(one);
}

// Connects to `one` as `user` with `password`, remembering them when asked, then reaches it; every
// copy of the password is wiped.
Path connected_at(const Share& one, std::string& user, std::string& password, bool remember) {
    std::wstring who = text::wide(user);
    std::wstring secret = text::wide(password);

    crypto::wiped(password);
    password.clear();

    const DWORD code = joined(one, who.c_str(), secret.c_str());

#ifdef TAL_REMEMBER_LOGINS
    if (code == NO_ERROR && remember) {
        std::wstring target = text::wide(one.server);
        CREDENTIALW credential{};

        credential.Type = CRED_TYPE_DOMAIN_PASSWORD;
        credential.TargetName = target.data();
        credential.UserName = who.data();
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
        credential.Persist = CRED_PERSIST_ENTERPRISE;

        (void)CredWriteW(&credential, 0);
    }
#else
    (void)remember;
#endif

    blanked(secret);

    if (code != NO_ERROR) throw failed(sharing::by_windows(code), spoken(code), one);

    if (one.name.empty()) return Path{};

    return reached_at(one);
}

// Connects to `one` through the system's own window, which asks for a login when the server wants
// one the system does not hold, and always when `again` asks so, a connection there was closed
// first; tal never sees the login. The window is the child of the one in front, the terminal's.
Path asked_at(const Share& one, bool again) {
    std::wstring remote = rooted(one);
    NETRESOURCEW resource{};

    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();

    if (again) (void)WNetCancelConnection2W(remote.c_str(), 0, TRUE);

    const DWORD how = CONNECT_TEMPORARY | CONNECT_INTERACTIVE | (again ? CONNECT_PROMPT : 0);
    const DWORD code = WNetAddConnection3W(GetForegroundWindow(), &resource, nullptr, nullptr, how);

    if (code != NO_ERROR) throw failed(sharing::by_windows(code), spoken(code), one);

    return reached_at(one);
}

// The remote names of the disks of the system's `scope` of connections (those there are, or those
// it remembers), each with the letter it is on or nothing.
std::vector<std::pair<std::wstring, std::wstring>> enumerated(DWORD scope) {
    std::vector<std::pair<std::wstring, std::wstring>> made;
    HANDLE handle = nullptr;

    if (WNetOpenEnumW(scope, RESOURCETYPE_DISK, 0, nullptr, &handle) != NO_ERROR) return made;

    std::vector<std::byte> buffer(16 * 1024);

    for (;;) {
        DWORD count = static_cast<DWORD>(-1);
        DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD code = WNetEnumResourceW(handle, &count, buffer.data(), &size);

        if (code == ERROR_MORE_DATA) {
            buffer.resize(std::max<std::size_t>(size, buffer.size() * 2));

            continue;
        }

        if (code != NO_ERROR) break;

        const auto* each = reinterpret_cast<const NETRESOURCEW*>(buffer.data());

        for (DWORD at = 0; at < count; ++at)
            if (each[at].lpRemoteName != nullptr) made.emplace_back(each[at].lpRemoteName, each[at].lpLocalName != nullptr ? each[at].lpLocalName : L"");
    }

    (void)WNetCloseEnum(handle);

    return made;
}

// The shares the system is connected to, then those it remembers and is not, each once.
std::vector<Connection> connections_at() {
    std::vector<Connection> made;
    const auto add = [&made](const std::wstring& remote, const std::wstring& local, bool connected) {
        std::optional<Share> share = addressed(text::narrow(remote));

        if (!share || share->name.empty()) return;

        share->rest.clear();

        std::optional<Path> drive;

        if (!local.empty()) drive = Path{text::narrow(local) + "/"};

        for (Connection& one : made)
            if (lowered(one.share.server) == lowered(share->server) && lowered(one.share.name) == lowered(share->name)) {
                one.connected = one.connected || connected;

                if (!one.drive) one.drive = drive;

                return;
            }

        made.push_back(Connection{std::move(*share), connected, std::move(drive)});
    };

    for (const auto& [remote, local] : enumerated(RESOURCE_CONNECTED)) add(remote, local, true);

    for (const auto& [remote, local] : enumerated(RESOURCE_REMEMBERED)) add(remote, local, false);

    return made;
}

// The names of the ordinary disk shares of the server of `one`, sorted, from NetShareEnum.
std::vector<std::string> offered_at(const Share& one) {
    std::wstring server = L"\\\\" + text::wide(one.server);
    LPBYTE buffer = nullptr;
    DWORD read = 0;
    DWORD total = 0;
    const NET_API_STATUS code = NetShareEnum(server.data(), 1, &buffer, MAX_PREFERRED_LENGTH, &read, &total, nullptr);

    if (code != NERR_Success && code != ERROR_MORE_DATA) {
        if (buffer != nullptr) NetApiBufferFree(buffer);

        throw failed(sharing::by_windows(code), spoken(code), one);
    }

    std::vector<std::string> made;
    const auto* each = reinterpret_cast<const SHARE_INFO_1*>(buffer);

    for (DWORD at = 0; at < read; ++at) {
        const DWORD kind = each[at].shi1_type;

        if ((kind & 0xFFU) != STYPE_DISKTREE || (kind & STYPE_SPECIAL) != 0) continue;

        std::string name = text::narrow(std::wstring_view{each[at].shi1_netname});

        if (!name.empty() && name.back() != '$') made.push_back(std::move(name));
    }

    if (buffer != nullptr) NetApiBufferFree(buffer);

    std::sort(made.begin(), made.end());

    return made;
}

}

#else

namespace {

// What a run of gio said: whether it started, its code (-1 when it did not end of itself), its
// output, and its output and errors together.
struct Said {
    bool started = false;
    int code = -1;
    std::string output;
    std::string text;
};

// gio in English with `arguments` and the input `input`, which is wiped; `started` is false when
// it cannot be run.
Task<Said> told(std::vector<std::string> arguments, std::string input = {}) {
    Command what;
    Bytes given = bytes(input);
    Said made;

    crypto::wiped(input);

    what.program = sharing::gio();
    what.arguments = std::move(arguments);
    what.environment = {{"LC_ALL", "C"}, {"LANG", "C"}, {"LANGUAGE", "C"}};

    try {
        const Output out = co_await Process::run(std::move(what), std::move(given));

        made.started = true;
        made.code = out.exit.ended == Exit::Ended::normal ? out.exit.code : -1;
        made.output = text::of(out.output);
        made.text = made.output + "\n" + text::of(out.errors);
    } catch (const Trouble&) {
        made.started = false;
    }

    co_return made;
}

// The smb:// URL of `one`, or of its server alone without a share.
std::string url(const Share& one) { return "smb://" + one.server + "/" + one.name + (one.name.empty() ? "" : "/"); }

// The arguments of gio's `command` on `one`.
std::vector<std::string> asked(const char* command, const Share& one) {
    std::vector<std::string> made;

    made.emplace_back(command);
    made.push_back(url(one));

    return made;
}

// The folder of GVfs's mounts, $XDG_RUNTIME_DIR/gvfs; nothing without XDG_RUNTIME_DIR.
std::optional<Path> gvfs() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");

    if (runtime == nullptr || *runtime == '\0') return std::nullopt;

    return Path{runtime} / "gvfs";
}

// The path of `one` when it is mounted already, with the rest of the address after it.
Task<std::optional<Path>> looked(Share one) {
    const std::optional<Path> under = gvfs();

    if (!under) co_return std::nullopt;

    std::vector<std::string> names;

    try {
        for (const Entry& each : co_await fs::list(*under)) names.emplace_back(each.where.name());
    } catch (const Trouble&) {
        co_return std::nullopt;
    }

    const std::optional<std::string> found = sharing::mounted_as(names, one);

    if (!found) co_return std::nullopt;

    Path made = *under / *found;

    for (const std::string& part : pieces(one.rest)) made = made / part;

    co_return made;
}

// The path of `one` after `gio mount` said `said`; the trouble of its reason otherwise.
Task<Path> settled(Share one, Said said) {
    if (!said.started) throw failed(Kind::unsupported, "The system has no gio.", one);

    if (said.code != 0 && lowered(said.text).find("already mounted") == std::string::npos) throw failed(sharing::by_gio(said.text), std::string{trimmed(said.text)}, one);

    if (one.name.empty()) co_return Path{};

    std::optional<Path> found = co_await looked(one);

    if (found) co_return std::move(*found);

    throw failed(Kind::unsupported, "The share is mounted, but not as a folder (GVfs without FUSE).", one);
}

// The path of `one`: its mount, or a `gio mount` without a login.
Task<Path> reached_at(Share one) {
    if (!gvfs()) throw failed(Kind::unsupported, "GVfs has no folder (XDG_RUNTIME_DIR).", one);

    std::optional<Path> found = co_await looked(one);

    if (found) co_return std::move(*found);

    Said said = co_await told(asked("mount", one));

    co_return co_await settled(std::move(one), std::move(said));
}

// `one` mounted as `user` with `password`, given to gio on its input; the password is wiped.
Task<Path> connected_at(Share one, std::string user, std::string password) {
    if (!gvfs()) {
        crypto::wiped(password);

        throw failed(Kind::unsupported, "GVfs has no folder (XDG_RUNTIME_DIR).", one);
    }

    std::string input = sharing::answers(user, password);

    crypto::wiped(password);

    Said said = co_await told(asked("mount", one), std::move(input));

    co_return co_await settled(std::move(one), std::move(said));
}

// `one` mounted by `gio mount` with the login the system holds; there is no window of the system
// here to ask for another, so `again` is refused as unsupported.
Task<Path> asked_at(Share one, bool again) {
    if (again) throw failed(Kind::unsupported, "The system has no window here to ask for a login; mount the share in the file manager or with gio mount.", one);

    co_return co_await reached_at(std::move(one));
}

// The shares mounted in GVfs, all connected; GVfs remembers none that are not.
Task<std::vector<Connection>> connections_at() {
    std::vector<Connection> made;
    const std::optional<Path> under = gvfs();

    if (!under) co_return made;

    std::vector<Entry> found;

    try {
        found = co_await fs::list(*under);
    } catch (const Trouble&) {
        co_return made;
    }

    for (const Entry& each : found)
        if (std::optional<Share> one = sharing::gvfs_named(each.where.name())) made.push_back(Connection{std::move(*one), true});

    co_return made;
}

// The names of the visible shares of the server of `one`, sorted, from `gio list`; browsing a
// server takes a mount in GVfs too.
Task<std::vector<std::string>> offered_at(Share one) {
    one.name.clear();

    const Said mounted = co_await told(asked("mount", one));

    if (!mounted.started) throw failed(Kind::unsupported, "The system has no gio.", one);

    const Said said = co_await told(asked("list", one));

    if (said.code != 0) {
        const Said& worse = mounted.code != 0 && lowered(mounted.text).find("already mounted") == std::string::npos ? mounted : said;

        throw failed(sharing::by_gio(worse.text), std::string{trimmed(worse.text)}, one);
    }

    std::vector<std::string> made = sharing::listed(said.output);

    std::sort(made.begin(), made.end());

    co_return made;
}

}

#endif

// The share `where` lies on: the inverse of `reach`. On Windows a share's path is UNC (stored as
// //server/share) and says it itself; elsewhere it lies in a folder of GVfs
// ($XDG_RUNTIME_DIR/gvfs/smb-share:...), whose name gives the server and the share, and what lies
// below it the rest.
std::optional<Share> shared(const Path& where) {
#ifdef _WIN32
    const std::string said = where.text();

    if (!said.starts_with("//")) return std::nullopt;

    return addressed(said);
#else
    const std::optional<Path> under = gvfs();

    if (!under) return std::nullopt;

    const std::string root = under->text() + "/";
    const std::string said = where.text();

    if (!said.starts_with(root)) return std::nullopt;

    const std::vector<std::string> parts = pieces(std::string_view{said}.substr(root.size()));

    if (parts.empty()) return std::nullopt;

    std::optional<Share> made = sharing::gvfs_named(parts[0]);

    if (!made) return std::nullopt;

    for (std::size_t at = 1; at < parts.size(); ++at) made->rest += (at > 1 ? "/" : "") + parts[at];

    return made;
#endif
}

// The path of `one`, reached with the login the system has already; throws `Trouble` (other)
// without a server or a share.
Task<Path> reach(Share one) {
    if (one.server.empty() || one.name.empty()) throw Trouble{Kind::other, "A share needs a server and a name."};

#ifdef _WIN32
    co_return co_await platform::offloaded([one = std::move(one)] { return reached_at(one); });
#else
    co_return co_await reached_at(std::move(one));
#endif
}

// The path of `one` after logging in as `user` with `password`, remembered when `remember` asks
// so; a server alone (no share) gives an empty path. Throws `Trouble` (other) without a server and
// for remembering in a build that cannot (`keeps_logins`), before connecting. The password is
// wiped.
Task<Path> connect(Share one, std::string user, std::string password, bool remember) {
    if (one.server.empty()) {
        crypto::wiped(password);

        throw Trouble{Kind::other, "A share needs a server."};
    }

    if (remember && !keeps_logins()) {
        crypto::wiped(password);

        throw failed(Kind::other, "This build does not remember logins (TAL_REMEMBER_LOGINS).", one);
    }

#ifdef _WIN32
    co_return co_await platform::offloaded([one = std::move(one), user = std::move(user), password = std::move(password), remember]() mutable {
        Path made;

        try {
            made = connected_at(one, user, password, remember);
        } catch (...) {
            crypto::wiped(password);

            throw;
        }

        crypto::wiped(password);

        return made;
    });
#else
    co_return co_await connected_at(std::move(one), std::move(user), std::move(password));
#endif
}

// The path of `one` after connecting through the system, which asks for a login in its own window
// when it needs one, or always when `again` asks so (on Windows; elsewhere the login it holds, and
// `again` unsupported); tal and its program never see the login. Throws `Trouble` (other) without
// a server or a share.
Task<Path> connect(Share one, bool again) {
    if (one.server.empty() || one.name.empty()) throw Trouble{Kind::other, "A share needs a server and a name."};

#ifdef _WIN32
    co_return co_await platform::offloaded([one = std::move(one), again] { return asked_at(one, again); });
#else
    co_return co_await asked_at(std::move(one), again);
#endif
}

// The shares of the network the system is connected to, then those it remembers and is not (on
// Windows; elsewhere those mounted in GVfs), each once, with the letter it is on.
Task<std::vector<Connection>> connections() {
#ifdef _WIN32
    co_return co_await platform::offloaded([] { return connections_at(); });
#else
    co_return co_await connections_at();
#endif
}

// The names of the visible shares of the server of `server`, sorted; throws `Trouble` (other)
// without a server.
Task<std::vector<std::string>> shares(Share server) {
    if (server.server.empty()) throw Trouble{Kind::other, "A share needs a server."};

#ifdef _WIN32
    co_return co_await platform::offloaded([server = std::move(server)] { return offered_at(server); });
#else
    co_return co_await offered_at(std::move(server));
#endif
}

}

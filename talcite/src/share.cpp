#include "talcite/share.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <system_error>
#include <utility>

#include "talcite/ascii.hpp"
#include "talcite/bytes.hpp"
#include "talcite/child.hpp"
#include "talcite/encoding.hpp"

#include "hexing.hpp"
#include "secrets.hpp"
#include "sharing.hpp"

#ifdef _WIN32

#include <windows.h>

#include <lm.h>
#include <wincred.h>
#include <winnetwk.h>

#endif

// Network shares. An address in any of four forms (\\NAS\films, //NAS/films, smb://NAS/films,
// smb://user@192.168.1.10/films) is split into the server, the share, the rest of the path and the
// user (`addressed`), and the share is reached through the system:
//
// - Windows: the UNC path directly. When the system refuses, `WNetAddConnection2W` without a drive
//   letter and with CONNECT_TEMPORARY: first without credentials (the system uses the stored ones),
//   and with the given ones from the login window. Remembering (`remember`) is an entry in the
//   Windows Credential Manager (`CredWriteW`, a domain password for the server, the same as
//   cmdkey /add), made only after a successful connection, so that a wrong password does not stay
//   in the system. The list of shares comes from `NetShareEnum`.
// - POSIX: GVfs. A mounted share lies in $XDG_RUNTIME_DIR/gvfs under a name of the form
//   smb-share:server=...,share=...; an unmounted one is mounted by gio mount, and the user, domain
//   and password go to its standard input, not into the arguments, because arguments are visible
//   in ps. The list comes from gio list. The messages of gio are read in English (LC_ALL=C),
//   because otherwise the reason could not be read from them. Without gio, without GVfs's FUSE or
//   without XDG_RUNTIME_DIR the answer is `Refused::unsupported`.
//
// The password does not stay: every copy (the string, the wide string, the input for gio) is zeroed
// right after the call, by `wiped`, which the compiler does not optimize away.
namespace talcite::fs {

namespace {

using ascii::lowered;
using ascii::trimmed;

// The parts of a path split at both kinds of slash, without empty ones.
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

}

// Splits an address in UNC, //server or smb:// form into a `Share`; `std::nullopt` for anything
// else or for an address without a server.
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

    // A user only in a URL: smb://user@nas/films. A password in the address (user:secret@) is
    // dropped without a trace: twa keeps no passwords, and the address goes into the history.
    if (url) {
        const auto slash = inside.find('/');
        const auto at = inside.substr(0, slash).rfind('@');

        if (at != std::string_view::npos) {
            const std::string_view who = inside.substr(0, at);

            made.user = hex::unescaped(who.substr(0, who.find(':')));
            inside = inside.substr(at + 1);
        }
    }

    std::vector<std::string> parts = pieces(inside);

    if (parts.empty()) return std::nullopt;

    if (url)
        for (std::string& one : parts) one = hex::unescaped(one);

    made.server = parts[0];

    if (parts.size() > 1) made.name = parts[1];

    for (std::size_t at = 2; at < parts.size(); ++at) made.rest += (at > 2 ? "/" : "") + parts[at];

    return made;
}

// The address for display and for the history, in the system's notation: UNC on Windows, a URL
// elsewhere.
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

// What a Windows or network error code means for the user: a login problem, a missing share, the
// network, a credential conflict, no support, or something else.
Refused refused_by_windows(std::uint32_t code) {
    switch (code) {
        case 0: return Refused::none;
        case 5:      // ERROR_ACCESS_DENIED
        case 86:     // ERROR_INVALID_PASSWORD
        case 1244:   // ERROR_NOT_AUTHENTICATED
        case 1326:   // ERROR_LOGON_FAILURE
        case 1327:   // ERROR_ACCOUNT_RESTRICTION
        case 1330:   // ERROR_PASSWORD_EXPIRED
        case 1331:   // ERROR_ACCOUNT_DISABLED
        case 1385:   // ERROR_LOGON_TYPE_NOT_GRANTED
        case 2202:   // ERROR_BAD_USERNAME
            return Refused::login;
        case 67:     // ERROR_BAD_NET_NAME
        case 2310:   // NERR_NetNameNotFound
            return Refused::missing;
        case 51:     // ERROR_REM_NOT_LIST
        case 53:     // ERROR_BAD_NETPATH
        case 59:     // ERROR_UNEXP_NET_ERR
        case 64:     // ERROR_NETNAME_DELETED
        case 121:    // ERROR_SEM_TIMEOUT
        case 1203:   // ERROR_NO_NET_OR_BAD_PATH
        case 1222:   // ERROR_NO_NETWORK
        case 1231:   // ERROR_NETWORK_UNREACHABLE
        case 1232:   // ERROR_HOST_UNREACHABLE
            return Refused::network;
        case 1219:   // ERROR_SESSION_CREDENTIAL_CONFLICT
            return Refused::conflict;
        case 50:     // ERROR_NOT_SUPPORTED
        case 1200:   // ERROR_BAD_DEVICE
        case 1204:   // ERROR_BAD_PROVIDER
            return Refused::unsupported;
        default: return Refused::other;
    }
}

// What an English gio message means. The network comes first: "Failed to retrieve share list from
// server: Connection timed out" is about the server, although it mentions the list. Then the login,
// including "cancelled", because that is how a password prompt ends when the input has no more
// answers.
Refused refused_by_gio(std::string_view said) {
    const std::string text = lowered(said);

    const auto any = [&text](std::initializer_list<std::string_view> among) {
        return std::any_of(among.begin(), among.end(), [&text](std::string_view one) { return text.find(one) != std::string::npos; });
    };

    if (any({"timed out", "connection refused", "no route to host", "network is unreachable", "host is down",
             "name or service not known", "could not resolve", "unable to resolve", "host is unreachable"}))
        return Refused::network;

    if (any({"permission denied", "access denied", "password", "authentication", "logon failure", "cancelled",
             "canceled", "aborted"}))
        return Refused::login;

    if (any({"no such file", "not found", "does not exist", "no such share"})) return Refused::missing;

    if (any({"not supported", "unsupported", "not mounted", "no such mount", "implement mount"})) return Refused::unsupported;

    return Refused::other;
}

// The share named by the GVfs directory of a mounted share:
// smb-share:server=nas,share=films[,user=...][,domain=...]. Values may be percent-encoded (%20), as
// in an address. Any other GVfs directory (sftp:..., or smb-server:..., a server alone and not a
// share) is not a share.
std::optional<Share> gvfs_named(std::string_view name) {
    if (!name.starts_with("smb-share:")) return std::nullopt;

    Share made;

    std::string_view rest = name.substr(10);

    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string_view pair = rest.substr(0, comma);
        const auto equal = pair.find('=');

        if (equal != std::string_view::npos) {
            const std::string_view key = pair.substr(0, equal);
            std::string value = hex::unescaped(pair.substr(equal + 1));

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

// The GVfs directory of the given share among the mounted ones. Server and share are compared
// ignoring case: GVfs writes them in lowercase, people write as they like. When the address has a
// user, that user's mount wins.
std::optional<std::string> mounted_as(const std::vector<std::string>& names, const Share& one) {
    std::optional<std::string> found;

    for (const std::string& name : names) {
        const std::optional<Share> mounted = gvfs_named(name);

        if (!mounted) continue;

        if (lowered(mounted->server) != lowered(one.server) || lowered(mounted->name) != lowered(one.name)) continue;

        if (!one.user.empty() && mounted->user == one.user) return name;

        if (!found) found = name;
    }

    return found;
}

// The input for gio mount: user, domain and password, each on its own line, in the order gio asks
// for them. The domain comes from DOMAIN\user or DOMAIN;user; without one it is empty, which means
// the default.
std::string answers(std::string_view user, std::string_view password) {
    std::string_view domain;

    if (const auto cut = user.find_first_of("\\;"); cut != std::string_view::npos) {
        domain = user.substr(0, cut);
        user = user.substr(cut + 1);
    }

    std::string made;

    made.reserve(user.size() + domain.size() + password.size() + 3);

    made.append(user).append("\n").append(domain).append("\n").append(password).append("\n");

    return made;
}

// The share names from the output of gio list smb://server/, one name per line. Shares ending in $
// (IPC$, ADMIN$, print$) are hidden ones and are left out.
std::vector<std::string> listed(std::string_view said) {
    std::vector<std::string> made;

    while (!said.empty()) {
        const auto end = said.find('\n');
        std::string one{trimmed(said.substr(0, end))};

        if (!one.empty() && one.back() == '/') one.pop_back();

        if (!one.empty() && one.back() != '$') made.push_back(std::move(one));

        if (end == std::string_view::npos) break;

        said = said.substr(end + 1);
    }

    return made;
}

// Zeroes and empties the string in a way the compiler cannot remove as a dead store.
void wiped(std::string& secret) {
    talcite::secret::wiped(secret);

    secret.clear();
}


namespace {

#ifdef _WIN32

// Zeroes and empties a wide string holding a secret.
void blanked(std::wstring& secret) {
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));

    secret.clear();
}

// The root of the share in UNC form; without a share, IPC$, through which the login to the server
// itself goes (for the list of shares).
std::wstring rooted(const Share& one) {
    return L"\\\\" + to_wstring(one.server) + L"\\" + (one.name.empty() ? std::wstring{L"IPC$"} : to_wstring(one.name));
}

// The full UNC path: the share's root and the rest of the path.
path whole(const Share& one) {
    std::string rest = one.rest;

    std::replace(rest.begin(), rest.end(), '/', '\\');

    return path{rooted(one) + (rest.empty() ? std::wstring{} : L"\\" + to_wstring(rest))};
}

// A failed `Reached` for a Windows error code, with the system's message for it.
Reached refusal(DWORD code) {
    return Reached{refused_by_windows(code), {}, std::system_category().message(static_cast<int>(code))};
}

// The share is available when the system answers for its root; returns NO_ERROR or the error
// code. The rest of the path is not the question here: a directory that does not exist is dealt
// with later by the caller, higher up, as with any entry.
DWORD standing(const Share& one) {
    const std::wstring root = rooted(one) + L"\\";

    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) return NO_ERROR;

    return GetLastError();
}

// Connects to the share (or to IPC$ of the server) without a drive letter, for this session only;
// null credentials mean the stored ones.
DWORD joined(const Share& one, const wchar_t* user, const wchar_t* password) {
    std::wstring remote = rooted(one);

    NETRESOURCEW resource{};

    resource.dwType = one.name.empty() ? RESOURCETYPE_ANY : RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();

    return WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY);
}

// Reaches the share without asking for credentials, giving its UNC path or the reason it failed.
Reached reached_at(const Share& one) {
    DWORD code = standing(one);

    // A refused access is retried with a connection without credentials: the system then uses the
    // ones from the Credential Manager, which the plain UNC path does not always do.
    if (code != NO_ERROR && refused_by_windows(code) == Refused::login) {
        if (const DWORD tried = joined(one, nullptr, nullptr); tried == NO_ERROR) code = standing(one);
        else code = tried;
    }

    if (code != NO_ERROR) return refusal(code);

    return Reached{Refused::none, whole(one), {}};
}

// Connects with the given user and password (wiping every copy of the password), optionally
// remembering them, then reaches the share.
Reached connected_at(const Share& one, std::string& user, std::string& password, bool remember) {
    std::wstring who = to_wstring(user);
    std::wstring secret = to_wstring(password);

    wiped(password);

    const DWORD code = joined(one, who.c_str(), secret.c_str());

    // Into the Credential Manager only after a successful connection, so a wrong password does not
    // stay in the system. A failed write does not spoil the existing connection; at worst the system
    // asks again next time.
    if (code == NO_ERROR && remember) {
        std::wstring target = to_wstring(one.server);

        CREDENTIALW credential{};

        credential.Type = CRED_TYPE_DOMAIN_PASSWORD;
        credential.TargetName = target.data();
        credential.UserName = who.data();
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
        credential.Persist = CRED_PERSIST_ENTERPRISE;

        (void)CredWriteW(&credential, 0);
    }

    blanked(secret);

    if (code != NO_ERROR) return refusal(code);

    if (one.name.empty()) return Reached{};

    return reached_at(one);
}

// The sorted names of the server's ordinary disk shares, from `NetShareEnum`.
Offered offered_at(const Share& one) {
    std::wstring server = L"\\\\" + to_wstring(one.server);

    LPBYTE buffer = nullptr;
    DWORD read = 0;
    DWORD total = 0;

    const NET_API_STATUS code = NetShareEnum(server.data(), 1, &buffer, MAX_PREFERRED_LENGTH, &read, &total, nullptr);

    if (code != NERR_Success && code != ERROR_MORE_DATA) {
        if (buffer != nullptr) NetApiBufferFree(buffer);

        return Offered{refused_by_windows(code), {}, std::system_category().message(static_cast<int>(code))};
    }

    Offered made;

    const auto* each = reinterpret_cast<const SHARE_INFO_1*>(buffer);

    for (DWORD at = 0; at < read; ++at) {
        const DWORD kind = each[at].shi1_type;

        // 0xFF is STYPE_MASK, which the MinGW headers lack.
        if ((kind & 0xFFU) != STYPE_DISKTREE || (kind & STYPE_SPECIAL) != 0) continue;

        std::string name = to_string(std::wstring_view{each[at].shi1_netname});

        if (!name.empty() && name.back() != '$') made.names.push_back(std::move(name));
    }

    if (buffer != nullptr) NetApiBufferFree(buffer);

    std::sort(made.names.begin(), made.names.end());

    return made;
}

#else

// What a gio run said: whether it started, its exit code (-1 when it did not end normally), and
// its standard output and error together.
struct Said {
    bool started = false;
    int code = -1;
    std::string text;
};

// Runs gio in English with the given arguments and input (which is wiped); when it cannot be
// started (no such program), `started` is false.
Said told(std::vector<std::string> arguments, std::string input = {}) {
    Command what;

    what.program = path{"gio"};
    what.arguments = std::move(arguments);
    what.environment = {{"LC_ALL", "C"}, {"LANG", "C"}, {"LANGUAGE", "C"}};

    Bytes given = talcite::bytes(input);

    wiped(input);

    Said made;

    try {
        const Output out = Child::run(std::move(what), std::move(given)).get();

        made.started = true;
        made.code = out.exit.ended == Ended::normal ? out.exit.code : -1;
        made.text = talcite::text(out.out) + "\n" + talcite::text(out.err);
    } catch (const std::exception&) {
        made.started = false;
    }

    return made;
}

// The smb:// URL of the share, or of the server alone when there is no share name.
std::string url(const Share& one) { return "smb://" + one.server + "/" + one.name + (one.name.empty() ? "" : "/"); }

// The GVfs mount directory, $XDG_RUNTIME_DIR/gvfs, or `std::nullopt` without XDG_RUNTIME_DIR.
std::optional<path> gvfs() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");

    if (runtime == nullptr || *runtime == '\0') return std::nullopt;

    return path{runtime} / "gvfs";
}

// The local path of a share that is already mounted, with the rest of the address appended.
std::optional<path> looked(const Share& one) {
    const std::optional<path> under = gvfs();

    if (!under) return std::nullopt;

    std::vector<std::string> names;
    std::error_code trouble;

    for (const std::filesystem::directory_entry& each : std::filesystem::directory_iterator{*under, trouble})
        names.push_back(to_string(each.path().filename()));

    const std::optional<std::string> found = mounted_as(names, one);

    if (!found) return std::nullopt;

    path made = *under / *found;

    for (const std::string& part : pieces(one.rest)) made /= from_string(part);

    return made;
}

// Turns the outcome of gio mount into a `Reached`: the local path, or the reason it failed.
Reached settled(const Share& one, const Said& said) {
    if (!said.started) return Reached{Refused::unsupported, {}, {}};

    const bool mounted = said.code == 0 || lowered(said.text).find("already mounted") != std::string::npos;

    if (!mounted) return Reached{refused_by_gio(said.text), {}, std::string{trimmed(said.text)}};

    if (one.name.empty()) return Reached{};

    if (std::optional<path> found = looked(one)) return Reached{Refused::none, std::move(*found), {}};

    // Mounted, yet no directory: GVfs without FUSE, reachable through gio but not as a path.
    return Reached{Refused::unsupported, {}, {}};
}

// Reaches the share: the mounted directory if there is one, otherwise a gio mount without
// credentials.
Reached reached_at(const Share& one) {
    if (!gvfs()) return Reached{Refused::unsupported, {}, {}};

    if (std::optional<path> found = looked(one)) return Reached{Refused::none, std::move(*found), {}};

    return settled(one, told({"mount", url(one)}));
}

// Mounts the share with the given user and password, fed to gio on its input; the password is
// wiped. Remembering is not supported here.
Reached connected_at(const Share& one, std::string& user, std::string& password, bool) {
    if (!gvfs()) {
        wiped(password);

        return Reached{Refused::unsupported, {}, {}};
    }

    std::string input = answers(user, password);

    wiped(password);

    return settled(one, told({"mount", url(one)}, std::move(input)));
}

// The sorted names of the server's visible shares, from gio list.
Offered offered_at(const Share& one) {
    Share server = one;

    server.name.clear();

    // Browsing a server in GVfs needs a mount too; without it gio list says "not mounted".
    const Said mounted = told({"mount", url(server)});

    if (!mounted.started) return Offered{Refused::unsupported, {}, {}};

    const Said said = told({"list", url(server)});

    if (said.code != 0) {
        const Said& worse = mounted.code != 0 && lowered(mounted.text).find("already mounted") == std::string::npos ? mounted : said;

        return Offered{refused_by_gio(worse.text), {}, std::string{trimmed(worse.text)}};
    }

    Offered made;

    made.names = listed(said.text.substr(0, said.text.rfind('\n')));

    std::sort(made.names.begin(), made.names.end());

    return made;
}

#endif

}

// Which share a path lies on: the inverse of `reach`. On Windows a share has a UNC path and speaks
// for itself; on POSIX the path lies in a GVfs directory ($XDG_RUNTIME_DIR/gvfs/smb-share:...), so
// the server and share are read from that directory's name and the rest from what lies below it.
// This lets twa remember a share on Linux under "This computer" just like a share on Windows.
std::optional<Share> shared(const path& where) {
#ifdef _WIN32
    const std::string said = to_string(where);

    if (!said.starts_with("\\\\")) return std::nullopt;

    return addressed(said);
#else
    const std::optional<path> under = gvfs();

    if (!under) return std::nullopt;

    auto mine = where.begin();

    for (const path& piece : *under) {
        if (piece.empty()) continue;

        if (mine == where.end() || *mine != piece) return std::nullopt;

        ++mine;
    }

    if (mine == where.end()) return std::nullopt;

    std::optional<Share> made = gvfs_named(to_string(*mine));

    if (!made) return std::nullopt;

    for (++mine; mine != where.end(); ++mine) {
        const std::string piece = to_string(*mine);

        if (piece.empty()) continue;

        made->rest += (made->rest.empty() ? "" : "/") + piece;
    }

    return made;
#endif
}

// Whether the system can remember logins for shares: only Windows, through the Credential Manager.
bool keeps_logins() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

// Reaches the share in the background with whatever credentials the system already has.
Promise<Reached> reach(Share one) {
    return run(io(), [one = std::move(one)]() -> Reached { return reached_at(one); });
}

// Connects to the share in the background with the given login, optionally remembering it; the
// password is wiped when done.
Promise<Reached> connect(Share one, std::string user, std::string password, bool remember) {
    return run(io(), [one = std::move(one), user = std::move(user), password = std::move(password),
                             remember]() mutable -> Reached {
        Reached made = connected_at(one, user, password, remember);

        wiped(password);

        return made;
    });
}

// Lists the shares of a server in the background.
Promise<Offered> shares(Share server) {
    return run(io(), [server = std::move(server)]() -> Offered { return offered_at(server); });
}

}

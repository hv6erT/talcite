#include "talcite/share.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <system_error>
#include <utility>

#include "talcite/bytes.hpp"
#include "talcite/child.hpp"
#include "talcite/encoding.hpp"

#include "sharing.hpp"

#ifdef _WIN32

#include <windows.h>

#include <lm.h>
#include <wincred.h>
#include <winnetwk.h>

#endif

// **Udziały sieciowe (U4).** Adres w czterech zapisach — `\\NAS\filmy`, `//NAS/filmy`,
// `smb://NAS/filmy`, `smb://hubert@192.168.1.10/filmy` — rozkłada się na serwer, udział, resztę
// ścieżki i użytkownika (`addressed`), a dojście do udziału idzie przez system:
//
// - **Windows**: ścieżka UNC wprost. Gdy system odmawia, `WNetAddConnection2W` bez litery dysku
//   i z `CONNECT_TEMPORARY` — najpierw bez poświadczeń (system bierze zapamiętane), a z oknem
//   logowania z podanymi. Zapamiętanie (`remember`) to wpis w Menedżerze poświadczeń Windows
//   (`CredWriteW`, hasło domenowe dla serwera — to samo, co `cmdkey /add`), robiony dopiero po
//   udanym połączeniu, żeby złe hasło nie zostało w systemie. Wykaz udziałów: `NetShareEnum`.
// - **POSIX**: GVfs. Zamontowany udział leży w `$XDG_RUNTIME_DIR/gvfs` pod nazwą
//   `smb-share:server=…,share=…`; niezamontowany montuje `gio mount`, a użytkownik, domena i hasło
//   idą na jego **wejście**, nie w argumentach — argumenty widać w `ps`. Wykaz: `gio list`.
//   Zdania `gio` czytamy po angielsku (`LC_ALL=C`), bo inaczej nie dałoby się z nich wyczytać
//   powodu. Bez `gio`, bez FUSE GVfs albo bez `XDG_RUNTIME_DIR` — `Refused::unsupported`.
//
// **Hasło nie zostaje**: każda kopia (napis, szeroki napis, wejście dla `gio`) jest zerowana zaraz
// po wywołaniu, przez `wiped`, którego kompilator nie wytnie.
namespace talcite::fs {

namespace {

std::string lowered(std::string_view said) {
    std::string made{said};

    for (char& one : made)
        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

    return made;
}

int hexed(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// Kody procentowe adresu (`%20` → spacja). Krzywy kod zostaje, jak stoi.
std::string unescaped(std::string_view said) {
    std::string made;

    for (std::size_t at = 0; at < said.size(); ++at) {
        if (said[at] == '%' && at + 2 < said.size()) {
            const int high = hexed(said[at + 1]);
            const int low = hexed(said[at + 2]);

            if (high >= 0 && low >= 0) {
                made += static_cast<char>(high * 16 + low);
                at += 2;

                continue;
            }
        }

        made += said[at];
    }

    return made;
}

std::string trimmed(std::string_view said) {
    const auto first = said.find_first_not_of(" \t\r\n");

    if (first == std::string_view::npos) return {};

    const auto last = said.find_last_not_of(" \t\r\n");

    return std::string{said.substr(first, last - first + 1)};
}

// Człony ścieżki po obu rodzajach ukośnika, bez pustych.
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

std::optional<Share> addressed(std::string_view said) {
    const std::string whole = trimmed(said);

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

    // Użytkownik tylko w URL-u: `smb://hubert@nas/filmy`. Hasło w adresie (`hubert:tajne@`)
    // odpada bez śladu — twa haseł nie trzyma, a adres trafia do historii.
    if (url) {
        const auto slash = inside.find('/');
        const auto at = inside.substr(0, slash).rfind('@');

        if (at != std::string_view::npos) {
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

// Adres do pokazania i do historii — w zapisie systemu: UNC na Windows, URL gdzie indziej.
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

// Zdanie `gio` po angielsku. Najpierw sieć: „Failed to retrieve share list from server: Connection
// timed out" mówi o serwerze, choć wspomina wykaz. Potem logowanie — wśród nich „cancelled", bo
// tak kończy się pytanie o hasło, na które wejście nie miało już odpowiedzi.
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

// Nazwa katalogu GVfs zamontowanego udziału: `smb-share:server=nas,share=filmy[,user=…][,domain=…]`.
// Wartości bywają zapisane procentami (`%20`), jak w adresie. Inny katalog GVfs (`sftp:…`,
// `smb-server:…` — sam serwer, nie udział) udziałem nie jest.
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

// Katalog GVfs danego udziału wśród katalogów zamontowanych. Serwer i udział bez wielkości liter —
// GVfs zapisuje je małymi, człowiek pisze, jak chce. Gdy adres ma użytkownika, wygrywa montowanie
// tego użytkownika.
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

// Wejście dla `gio mount`: użytkownik, domena i hasło, każde w osobnym wierszu — w tej kolejności
// pyta `gio`. Domena z zapisu `DOMENA\użytkownik` albo `DOMENA;użytkownik`; bez niej pusta, czyli
// domyślna.
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

// Wykaz z `gio list smb://serwer/`: nazwa w wierszu. Udziały z `$` na końcu (IPC$, ADMIN$, print$)
// są ukryte i do filmów się nie nadają.
std::vector<std::string> listed(std::string_view said) {
    std::vector<std::string> made;

    while (!said.empty()) {
        const auto end = said.find('\n');
        std::string one = trimmed(said.substr(0, end));

        if (!one.empty() && one.back() == '/') one.pop_back();

        if (!one.empty() && one.back() != '$') made.push_back(std::move(one));

        if (end == std::string_view::npos) break;

        said = said.substr(end + 1);
    }

    return made;
}

// Zerowanie, którego kompilator nie usunie jako zapisu, którego nikt nie czyta.
void wiped(std::string& secret) {
    volatile char* at = secret.data();

    for (std::size_t many = secret.size(); many > 0; --many) *at++ = '\0';

    secret.clear();
}


namespace {

#ifdef _WIN32

void blanked(std::wstring& secret) {
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));

    secret.clear();
}

// Korzeń udziału w UNC; bez udziału — `IPC$`, przez który idzie logowanie do samego serwera
// (wykaz udziałów).
std::wstring rooted(const Share& one) {
    return L"\\\\" + to_wstring(one.server) + L"\\" + (one.name.empty() ? std::wstring{L"IPC$"} : to_wstring(one.name));
}

path whole(const Share& one) {
    std::string rest = one.rest;

    std::replace(rest.begin(), rest.end(), '/', '\\');

    return path{rooted(one) + (rest.empty() ? std::wstring{} : L"\\" + to_wstring(rest))};
}

Reached refusal(DWORD code) {
    return Reached{refused_by_windows(code), {}, std::system_category().message(static_cast<int>(code))};
}

// Udział stoi, gdy system odpowiada o jego korzeń. Odpowiedź o resztę ścieżki nie jest tu
// pytaniem: katalogu, którego nie ma, szuka potem wołający — wyżej, jak przy każdym wejściu.
DWORD standing(const Share& one) {
    const std::wstring root = rooted(one) + L"\\";

    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) return NO_ERROR;

    return GetLastError();
}

DWORD joined(const Share& one, const wchar_t* user, const wchar_t* password) {
    std::wstring remote = rooted(one);

    NETRESOURCEW resource{};

    resource.dwType = one.name.empty() ? RESOURCETYPE_ANY : RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();

    return WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY);
}

Reached reached_at(const Share& one) {
    DWORD code = standing(one);

    // Odmowę dostępu próbujemy jeszcze połączeniem bez poświadczeń: system bierze wtedy te
    // z Menedżera poświadczeń, a sama ścieżka UNC nie zawsze to robi.
    if (code != NO_ERROR && refused_by_windows(code) == Refused::login) {
        if (const DWORD tried = joined(one, nullptr, nullptr); tried == NO_ERROR) code = standing(one);
        else code = tried;
    }

    if (code != NO_ERROR) return refusal(code);

    return Reached{Refused::none, whole(one), {}};
}

Reached connected_at(const Share& one, std::string& user, std::string& password, bool remember) {
    std::wstring who = to_wstring(user);
    std::wstring secret = to_wstring(password);

    wiped(password);

    const DWORD code = joined(one, who.c_str(), secret.c_str());

    // Do Menedżera poświadczeń dopiero po udanym połączeniu — złe hasło nie zostaje w systemie.
    // Nieudany zapis nie psuje połączenia, które już jest; najwyżej następnym razem system zapyta.
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

        // 0xFF to STYPE_MASK, którego nagłówki MinGW nie mają.
        if ((kind & 0xFFU) != STYPE_DISKTREE || (kind & STYPE_SPECIAL) != 0) continue;

        std::string name = to_string(std::wstring_view{each[at].shi1_netname});

        if (!name.empty() && name.back() != '$') made.names.push_back(std::move(name));
    }

    if (buffer != nullptr) NetApiBufferFree(buffer);

    std::sort(made.names.begin(), made.names.end());

    return made;
}

#else

struct Said {
    bool started = false;
    int code = -1;
    std::string text;
};

// `gio` po angielsku; nieuruchomione (brak programu) mówi `started == false`.
Said told(std::vector<std::string> arguments, std::string input = {}) {
    Command what;

    what.program = path{"gio"};
    what.arguments = std::move(arguments);
    what.environment = {{"LC_ALL", "C"}, {"LANG", "C"}, {"LANGUAGE", "C"}};

    Bytes given = talcite::bytes(input);

    wiped(input);

    Said made;

    try {
        const Output out = Child::run(std::move(what), std::move(given)).await();

        made.started = true;
        made.code = out.exit.ended == Ended::normal ? out.exit.code : -1;
        made.text = talcite::text(out.out) + "\n" + talcite::text(out.err);
    } catch (const std::exception&) {
        made.started = false;
    }

    return made;
}

std::string url(const Share& one) { return "smb://" + one.server + "/" + one.name + (one.name.empty() ? "" : "/"); }

std::optional<path> gvfs() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");

    if (runtime == nullptr || *runtime == '\0') return std::nullopt;

    return path{runtime} / "gvfs";
}

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

Reached settled(const Share& one, const Said& said) {
    if (!said.started) return Reached{Refused::unsupported, {}, {}};

    const bool mounted = said.code == 0 || lowered(said.text).find("already mounted") != std::string::npos;

    if (!mounted) return Reached{refused_by_gio(said.text), {}, trimmed(said.text)};

    if (one.name.empty()) return Reached{};

    if (std::optional<path> found = looked(one)) return Reached{Refused::none, std::move(*found), {}};

    // Zamontowany, a katalogu nie ma: GVfs bez FUSE — przez `gio` tak, ścieżką nie.
    return Reached{Refused::unsupported, {}, {}};
}

Reached reached_at(const Share& one) {
    if (!gvfs()) return Reached{Refused::unsupported, {}, {}};

    if (std::optional<path> found = looked(one)) return Reached{Refused::none, std::move(*found), {}};

    return settled(one, told({"mount", url(one)}));
}

Reached connected_at(const Share& one, std::string& user, std::string& password, bool) {
    if (!gvfs()) {
        wiped(password);

        return Reached{Refused::unsupported, {}, {}};
    }

    std::string input = answers(user, password);

    wiped(password);

    return settled(one, told({"mount", url(one)}, std::move(input)));
}

Offered offered_at(const Share& one) {
    Share server = one;

    server.name.clear();

    // Przeglądanie serwera GVfs też montuje — bez tego `gio list` mówi „not mounted".
    const Said mounted = told({"mount", url(server)});

    if (!mounted.started) return Offered{Refused::unsupported, {}, {}};

    const Said said = told({"list", url(server)});

    if (said.code != 0) {
        const Said& worse = mounted.code != 0 && lowered(mounted.text).find("already mounted") == std::string::npos ? mounted : said;

        return Offered{refused_by_gio(worse.text), {}, trimmed(worse.text)};
    }

    Offered made;

    made.names = listed(said.text.substr(0, said.text.rfind('\n')));

    std::sort(made.names.begin(), made.names.end());

    return made;
}

#endif

}

// **Na jakim udziale leży ścieżka** — odwrotność `reach`. Na Windows udział ma ścieżkę UNC i mówi
// sam za siebie; na POSIX ścieżka leży w katalogu GVfs (`$XDG_RUNTIME_DIR/gvfs/smb-share:…`), więc
// serwer i udział czytamy z nazwy jego katalogu, a resztę — z tego, co pod nim. Twa zapamiętuje
// dzięki temu udział z Linuksa na „Ten komputer" jak udział z Windows (U6).
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

bool keeps_logins() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

Promise<Reached> reach(Share one) {
    return Promise<Reached>{[one = std::move(one)](auto resolve, auto) { resolve(reached_at(one)); }};
}

Promise<Reached> connect(Share one, std::string user, std::string password, bool remember) {
    return Promise<Reached>{[one = std::move(one), user = std::move(user), password = std::move(password),
                             remember](auto resolve, auto) mutable {
        Reached made = connected_at(one, user, password, remember);

        wiped(password);

        resolve(std::move(made));
    }};
}

Promise<Offered> shares(Share server) {
    return Promise<Offered>{[server = std::move(server)](auto resolve, auto) { resolve(offered_at(server)); }};
}

}

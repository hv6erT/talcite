#include "piping.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "talcite/pool.hpp"
#include "talcite/trouble.hpp"

#include "handing.hpp"
#include "starting.hpp"

#ifdef _WIN32

#include <winsock2.h>

#include <windows.h>

#include <sddl.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

#endif

// **Rury nazwane** (Windows bez AF_UNIX). Każda strona rury jest uchwytem z nakładanym wejściem-
// wyjściem, przypinanym do portu centrali jak gniazdo: czytanie i pisanie to `ReadFile`
// i `WriteFile`, przyjęcie to `ConnectNamedPipe` (iocp.cpp), a nawiązanie — `CreateFileW` w puli,
// bo rura nie ma nakładanego łączenia.
//
// **Nazwa ze ścieżki**: `\\.\pipe\talcite-<skrót ścieżki>-<nazwa pliku>`. Skrót z ukośnikami
// w jednej postaci i bez wielkości liter — Windows ich w ścieżkach nie rozróżnia — więc obie strony,
// które podały tę samą ścieżkę, trafią w tę samą rurę.
//
// **Tylko my**: rura ma listę dostępu z samym użytkownikiem, który ją założył, i nie przyjmuje
// klientów z sieci. Nazwy rur są wspólne dla całej maszyny, więc ktoś mógłby założyć rurę o tej
// nazwie przed nami — dlatego pierwsza instancja bramy żąda, by była pierwsza (inaczej brama
// odmawia), a strona, której zależy, pyta o rozmówcę (`Stream::kindred`). Klient nie pozwala
// serwerowi podszyć się pod siebie (`SECURITY_IDENTIFICATION`).
namespace talcite::net::pipes {

#ifdef _WIN32

namespace {

constexpr DWORD roomy = 64 * 1024;

struct End {
    std::string path;
    bool server = false;
};

// Rejestr żyje do końca procesu: centrala zamyka uchwyty jeszcze w niszczeniu statyków.
struct Ends {
    std::mutex guard;
    std::unordered_map<Handle, End> all;
};

Ends& ends() {
    static auto* held = new Ends;

    return *held;
}

Handle enrolled(HANDLE one, const std::string& path, bool server) {
    const Handle made = reinterpret_cast<Handle>(one);

    const std::lock_guard<std::mutex> kept{ends().guard};

    ends().all[made] = End{path, server};

    return made;
}

std::optional<End> end_of(Handle one) {
    const std::lock_guard<std::mutex> kept{ends().guard};

    const auto found = ends().all.find(one);

    if (found == ends().all.end()) return std::nullopt;

    return found->second;
}

std::wstring wide(const std::string& one) {
    if (one.empty()) return {};

    const int much = MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), made.data(), much);

    return made;
}

std::wstring pipe_name(const std::string& path) {
    std::uint64_t sum = 14695981039346656037ull;

    for (char one : path) {
        if (one == '/') one = '\\';

        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

        sum ^= static_cast<unsigned char>(one);
        sum *= 1099511628211ull;
    }

    static constexpr char digits[] = "0123456789abcdef";

    std::string made = "\\\\.\\pipe\\talcite-";

    for (int shift = 60; shift >= 0; shift -= 4) made += digits[(sum >> shift) & 0xf];

    const std::size_t cut = path.find_last_of("\\/");
    std::string tail = cut == std::string::npos ? path : path.substr(cut + 1);

    if (tail.size() > 64) tail.resize(64);

    std::replace_if(
        tail.begin(), tail.end(),
        [](char one) {
            return !((one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '.' ||
                     one == '_' || one == '-');
        },
        '_');

    if (!tail.empty()) made += "-" + tail;

    return wide(made);
}

// Lista dostępu „tylko ja": SDDL z identyfikatorem użytkownika procesu.
PSECURITY_DESCRIPTOR owner_only() {
    HANDLE token = nullptr;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return nullptr;

    DWORD much = 0;

    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &much);

    std::string room(much, '\0');
    PSECURITY_DESCRIPTOR made = nullptr;

    if (much > 0 && GetTokenInformation(token, TokenUser, room.data(), much, &much) != 0) {
        const auto* told = reinterpret_cast<const TOKEN_USER*>(room.data());
        LPWSTR sid = nullptr;

        if (ConvertSidToStringSidW(told->User.Sid, &sid) != 0) {
            const std::wstring said = L"D:P(A;;GA;;;" + std::wstring{sid} + L")";

            (void)ConvertStringSecurityDescriptorToSecurityDescriptorW(said.c_str(), SDDL_REVISION_1, &made, nullptr);

            LocalFree(sid);
        }
    }

    CloseHandle(token);

    return made;
}

}

struct Gate {
    ~Gate() {
        pipes::shut(*this);

        if (rights != nullptr) LocalFree(rights);
    }

    // Instancja z tą samą nazwą i prawami. Pierwsza musi być pierwsza w ogóle — nazwa zajęta przez
    // kogoś innego to odmowa, a nie dołączenie do cudzej rury.
    [[nodiscard]] HANDLE instance(bool first) {
        SECURITY_ATTRIBUTES how{};

        how.nLength = sizeof how;
        how.lpSecurityDescriptor = rights;

        return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                                roomy, roomy, 0, rights != nullptr ? &how : nullptr);
    }

    // Wolna instancja — bez czekającego przyjęcia. **Słuchająca jest zawsze najwyżej jedna** (wolna
    // albo ta, na której czeka przyjęcie): klient trafia w dowolną słuchającą instancję, więc gdyby
    // obok czekającej leżała druga, klient w niej czekałby na przyjęcie, które patrzy gdzie indziej.
    // Pozostali klienci dostają „zajęte" i czekają (`dialed`), aż instancja się zwolni.
    std::mutex guard;
    std::wstring name;
    std::string path;
    PSECURITY_DESCRIPTOR rights = nullptr;
    Handle free = nothing;
    Handle pending = nothing;
    bool closing = false;
};

bool chosen() {
#if defined(TALCITE_PIPES)
    return true;
#elif defined(TALCITE_IOCP)
    static const bool made = [] {
        starting();

        const SOCKET one = ::socket(AF_UNIX, SOCK_STREAM, 0);

        if (one == INVALID_SOCKET) return true;

        ::closesocket(one);

        return false;
    }();

    return made;
#else
    // Zaplecze przenośne czeka na gotowość gniazd, a rura gotowości nie zgłasza.
    return false;
#endif
}

bool piped(Handle one) { return one != nothing && end_of(one).has_value(); }

bool served(Handle one) {
    const std::optional<End> found = end_of(one);

    return found && found->server;
}

// Jak przy AF_UNIX: koniec serwera zna ścieżkę jako swoją, klient — jako rozmówcy.
Address named(Handle one, bool peer) {
    const std::optional<End> found = end_of(one);

    if (!found || found->server == peer) return {};

    try {
        return Address::local(found->path);
    } catch (const Trouble&) {
        return {};
    }
}

std::optional<unsigned long> facing(Handle one) {
    const std::optional<End> found = end_of(one);

    if (!found) return std::nullopt;

    ULONG pid = 0;
    const HANDLE pipe = reinterpret_cast<HANDLE>(one);
    const BOOL told = found->server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid);

    if (told == 0) return std::nullopt;

    return static_cast<unsigned long>(pid);
}

void closed(Handle one) {
    {
        const std::lock_guard<std::mutex> kept{ends().guard};

        ends().all.erase(one);
    }

    CloseHandle(reinterpret_cast<HANDLE>(one));
}

std::shared_ptr<Gate> gated(const Address& where) {
    auto made = std::make_shared<Gate>();

    made->path = where.path();
    made->name = pipe_name(where.path());
    made->rights = owner_only();

    const HANDLE first = made->instance(true);

    if (first == INVALID_HANDLE_VALUE) {
        const int why = static_cast<int>(GetLastError());

        throw Trouble{"Cannot listen: " + spoken(why) + ".", why, where};
    }

    // Druga zapora na zajętą nazwę — system, który nie pilnuje pierwszej instancji (Wine), i tak
    // powie, ile instancji ma rura.
    DWORD instances = 0;

    if (GetNamedPipeHandleStateW(first, nullptr, &instances, nullptr, nullptr, nullptr, 0) != 0 && instances > 1) {
        CloseHandle(first);

        throw Trouble{"Cannot listen: " + spoken(WSAEADDRINUSE) + ".", WSAEADDRINUSE, where};
    }

    made->free = enrolled(first, made->path, true);

    return made;
}

Handle spare(Gate& gate) {
    const std::lock_guard<std::mutex> kept{gate.guard};

    if (gate.closing) return nothing;

    Handle made = gate.free;

    if (made == nothing) {
        const HANDLE one = gate.instance(false);

        if (one == INVALID_HANDLE_VALUE) return nothing;

        made = enrolled(one, gate.path, true);
    }

    gate.free = nothing;
    gate.pending = made;

    return made;
}

// Przyjęcie na `done` się skończyło: nowa wolna, żeby nazwa nie znikła, a klient miał gdzie trafić
// przed następnym przyjęciem — chyba że następne już czeka na własnej. Tak bywa po anulowaniu:
// miejsce w bramie zwalnia się od razu, a zakończenie anulowanej instancji wraca dopiero potem.
void refill(Gate& gate, Handle done) {
    const std::lock_guard<std::mutex> kept{gate.guard};

    if (gate.pending != done) return;

    gate.pending = nothing;

    if (gate.closing || gate.free != nothing) return;

    const HANDLE one = gate.instance(false);

    if (one != INVALID_HANDLE_VALUE) gate.free = enrolled(one, gate.path, true);
}

// Wolna instancja nie była nigdy w centrali, więc zamyka się od razu.
void shut(Gate& gate) {
    const std::lock_guard<std::mutex> kept{gate.guard};

    gate.closing = true;

    if (gate.free != nothing) closed(gate.free);

    gate.free = nothing;
}

namespace {

struct DialPromise : detail::Result<Stream> {};

}

// Nawiązanie w puli. Zajęte wszystkie instancje — czekamy na wolną kawałkami, żeby anulowanie
// i termin nie czekały na system; brak rury to odmowa połączenia, jak przy gnieździe bez słuchacza.
// Strumień, którego nikt już nie chce (anulowane w ostatniej chwili), ginie razem z uchwytem.
Promise<Stream> dialed(Address where, Patience patience) {
    auto one = std::make_shared<DialPromise>();
    const bool ever = patience.count() > 0;
    const auto until = std::chrono::steady_clock::now() + patience;

    pooling().post([one, where, ever, until] {
        const std::wstring name = pipe_name(where.path());

        for (;;) {
            if (one->quitted.load()) return;

            const HANDLE made = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);

            if (made != INVALID_HANDLE_VALUE) {
                settled(*one, greeting(enrolled(made, where.path(), false), where));

                return;
            }

            const DWORD why = GetLastError();

            if (why == ERROR_PIPE_BUSY) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();

                if (ever && left <= 0) {
                    spoiled(*one, std::make_exception_ptr(Overdue{}));

                    return;
                }

                (void)WaitNamedPipeW(name.c_str(), static_cast<DWORD>(ever ? std::min<long long>(left, 250) : 250));

                continue;
            }

            const int code = why == ERROR_FILE_NOT_FOUND ? WSAECONNREFUSED : static_cast<int>(why);

            spoiled(*one, std::make_exception_ptr(Trouble{"Cannot connect: " + spoken(code) + ".", code, where}));

            return;
        }
    });

    return HandPromise<Stream>{one};
}

#else

struct Gate {};

bool chosen() { return false; }

bool piped(Handle) { return false; }

bool served(Handle) { return false; }

Address named(Handle, bool) { return {}; }

std::optional<unsigned long> facing(Handle) { return std::nullopt; }

void closed(Handle) {}

std::shared_ptr<Gate> gated(const Address& where) { throw Trouble{"Named pipes are a Windows thing.", 0, where}; }

Handle spare(Gate&) { return nothing; }

void refill(Gate&, Handle) {}

void shut(Gate&) {}

Promise<Stream> dialed(Address where, Patience) {
    auto made = std::make_shared<detail::Result<Stream>>();

    detail::spoiled(*made, std::make_exception_ptr(Trouble{"Named pipes are a Windows thing.", 0, std::move(where)}));

    return HandPromise<Stream>{made};
}

#endif

}

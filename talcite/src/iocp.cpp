#include "exchange.hpp"

#include "inside.hpp"

#ifdef TALCITE_IOCP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"

#include "piping.hpp"
#include "starting.hpp"

#include <winsock2.h>

#include <mswsock.h>
#include <windows.h>
#include <ws2tcpip.h>

namespace talcite::net {

namespace {

// Rura po drugiej stronie zamknięta — dla czytającego to koniec strumienia, nie awaria.
bool ended_pipe(DWORD code) { return code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED; }

// Klucz zakończenia, które nie należy do żadnej operacji — mówi tylko „obudź się i policz
// terminy". To zastępuje budzik z pary gniazd, który jest potrzebny przy WSAPoll.
constexpr ULONG_PTR waking = 1;

constexpr std::size_t roomy = sizeof(sockaddr_storage) + 16;

// Operacja w locie. **OVERLAPPED musi przeżyć całe wywołanie systemowe**, a jądro pisze do
// bufora długo po powrocie z funkcji — dlatego to wszystko należy do centrali, a nie do
// domknięcia, które za chwilę zniknie.
struct Flying {
    OVERLAPPED over{};
    std::shared_ptr<Doing> one;
    WSABUF what{};
    sockaddr_storage told{};
    int wide = static_cast<int>(sizeof(sockaddr_storage));
    char space[2 * roomy]{};

    // **Licznik bajtów i znaczniki muszą przeżyć wywołanie.** Stały wcześniej na stosie funkcji
    // rozpoczynającej operację, a ta wraca, gdy tylko jądro powie „w toku" — jądro miało więc
    // adres z ramki, której już nie ma. MSDN mówi wprost: przy niezerowym lpOverlapped ten
    // wskaźnik ma być pusty albo przeżyć operację.
    DWORD much = 0;
    DWORD marks = 0;

    bool late = false;
};

LPFN_ACCEPTEX accepting = nullptr;
LPFN_CONNECTEX joining = nullptr;

// AcceptEx i ConnectEx nie mają zwykłego wpisu w bibliotece — trzeba je wyprosić po numerze
// z otwartego gniazda. Robimy to raz, na gnieździe zrobionym wyłącznie w tym celu.
void asking() {
    static std::once_flag once;

    std::call_once(once, [] {
        starting();

        const SOCKET one = ::socket(AF_INET, SOCK_STREAM, 0);

        if (one == INVALID_SOCKET) return;

        GUID which = WSAID_ACCEPTEX;
        DWORD much = 0;

        (void)::WSAIoctl(one, SIO_GET_EXTENSION_FUNCTION_POINTER, &which, sizeof which,
                         &accepting, sizeof accepting, &much, nullptr, nullptr);

        GUID other = WSAID_CONNECTEX;

        (void)::WSAIoctl(one, SIO_GET_EXTENSION_FUNCTION_POINTER, &other, sizeof other, &joining,
                         sizeof joining, &much, nullptr, nullptr);

        ::closesocket(one);
    });
}

}

struct Exchange::Held {
    mutable std::mutex guard;
    HANDLE port = nullptr;
    std::unordered_map<OVERLAPPED*, std::shared_ptr<Flying>> flying;
    std::unordered_map<Handle, int> counted;
    std::unordered_set<Handle> tied;
    std::unordered_set<Handle> leaving;
    std::thread staff;
    bool closing = false;
};

namespace {

std::atomic<Exchange::Held*> standing{nullptr};

// Gniazdo przypina się do portu raz i na zawsze — powtórzone przypięcie kończy się błędem, więc
// pamiętamy, kogo już przypięliśmy.
//
// **Zero znaczy „przypięte"**, cokolwiek innego to numer błędu. Porażka szła tu wcześniej po cichu:
// funkcja nic nie oddawała, więc operacja startowała mimo wszystko — a jej zakończenie nie miało
// **dokąd wrócić**, bo port o tym uchwycie nic nie wie. Skutek byłby dokładnie taki, jak przy
// porzuconych zgłoszeniach z rozbiórki: `Ticket` zostaje świeży, wątek na `await` nie budzi się
// nigdy i program nie kończy wyjścia.
//
// Numer bierzemy z `GetLastError`, a nie z `amiss()`: to jest wywołanie Win32, nie Winsock.
int tying(Exchange::Held& held, Handle one) {
    if (held.tied.count(one) != 0) return 0;

    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(one), held.port, 0, 0) == nullptr)
        return static_cast<int>(::GetLastError());

    held.tied.insert(one);

    return 0;
}

// Zamknięcie odkładamy, dopóki choć jedno zakończenie jeszcze nie wróciło. CancelIoEx nie kończy
// operacji od razu — jądro dopiero ją zamknie i dostarczy zakończenie z ERROR_OPERATION_ABORTED.
// Uchwyt zamknięty wcześniej zostawiłby jądro piszące do zwolnionej pamięci.
void sweeping(Exchange::Held& held, Handle one) {
    if (const auto found = held.counted.find(one); found != held.counted.end() && found->second > 0)
        return;

    if (held.leaving.count(one) == 0) return;

    held.leaving.erase(one);
    held.tied.erase(one);
    held.counted.erase(one);

    ending(one);
}

// Zakończenie idzie **na wątek z puli**, nie tutaj. Ciąg dalszy obietnicy to cudzy kod — gdyby
// pobiegł na wątku centrali, jeden powolny then zatrzymałby całą sieć.
void handed(std::shared_ptr<Doing> one, std::size_t much, int code, bool late) {
    pooling().post([one = std::move(one), much, code, late]() mutable {
        // **Zapis przerwany terminem nie jest udany, choćby część bajtów zeszła.** Wcześniej
        // gałąź terminu omijało się przy niezerowym liczniku, a kod błędu zerowało — więc
        // przerwany zapis megabajta, z którego poszło 64 KiB, meldował się jako zapisany w całości.
        if (late && (much == 0 || one->kind == Kind::putting)) {
            one->late();

            return;
        }

        one->done(much, late ? 0 : code);
    });
}

// Rozpoczęcie operacji. Na Windows **każda** zaczyna się tutaj: nie ma sygnału „można czytać",
// jest tylko „przeczytano ci tyle". Prawda znaczy „poszło, czekaj na zakończenie".
bool began(const std::shared_ptr<Flying>& fly) {
    Doing& one = *fly->one;

    switch (one.kind) {
        case Kind::joining: {
            asking();

            if (joining == nullptr) {
                handed(fly->one, 0, WSAEOPNOTSUPP, false);

                return false;
            }

            // **Gniazdo lokalne nie idzie przez ConnectEx** i nie jest to obejście, tylko
            // właściwa droga. ConnectEx wymaga gniazda już przypisanego, a adres lokalny nie ma
            // postaci „dowolnej", którą dałoby się przypisać — `Address::any` na nim odmawia,
            // i słusznie, bo taki adres nie istnieje.
            //
            // Ważniejsze jest jednak to, że **połączenie na własnej maszynie nie ma na co
            // czekać**: nie ma uzgadniania, nie ma sieci, jest albo od razu, albo wcale. Nakładane
            // wejście-wyjście nic tu nie wnosi, a wnosi zależność od tego, czy Winsock w ogóle
            // wystawia ConnectEx dla AF_UNIX — czego dokumentacja nie mówi.
            //
            // Rozstrzygamy więc na miejscu i oddajemy fałsz, czyli „zgłoszenie już załatwione".
            if (one.where.family() == Family::local) {
                sockaddr_storage told{};

                const int wide = filling(one.where, &told);

                if (::connect(static_cast<SOCKET>(one.held), reinterpret_cast<sockaddr*>(&told),
                              wide) == 0) {
                    handed(fly->one, 0, 0, false);

                    return false;
                }

                const int why = ::WSAGetLastError();

                // Wszystko poza „jeszcze nie teraz" jest odpowiedzią ostateczną: nie ma pliku, nikt
                // nie słucha, brak praw.
                if (why != WSAEWOULDBLOCK) {
                    handed(fly->one, 0, why, false);

                    return false;
                }

                // **Zmierzone na Windows: nawiązanie lokalne nie kończy się natychmiast.**
                // Zakładałem, że skoro nie ma ani uzgadniania, ani sieci, `connect` zdąży w miejscu
                // — i to było błędne. Gniazdo jest nieblokujące, więc odpowiada `WSAEWOULDBLOCK`
                // i kończy się chwilę później.
                //
                // Portu do tego użyć nie możemy, bo bez `ConnectEx` nikt tam zakończenia nie
                // złoży. Czekamy więc **na wątku z puli**, i to jest właściwe miejsce: pula na
                // pracę zwykłą rośnie bez ograniczeń właśnie dlatego, że taka praca ma prawo
                // poczekać. Czekaniem jest `select` na gotowość do pisania — ta sama rzecz, którą
                // robi zaplecze przenośne, tylko jednym gniazdem zamiast wszystkimi.
                //
                // Zgłoszenia nie ma w mapie centrali (oddajemy fałsz), więc terminu nie pilnuje
                // wątek centrali i musi go odmierzyć samo czekanie.
                const Handle which = one.held;
                const bool ever = one.ever;
                const auto until = one.until;

                pooling().post([mine = fly->one, which, ever, until]() {
                    timeval how{};

                    if (ever) {
                        const auto left = until - std::chrono::steady_clock::now();
                        const auto much =
                            std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
                        const std::int64_t kept = much < 0 ? 0 : much;

                        how.tv_sec = static_cast<long>(kept / 1000);
                        how.tv_usec = static_cast<long>((kept % 1000) * 1000);
                    }

                    fd_set writing;
                    fd_set failing;

                    FD_ZERO(&writing);
                    FD_ZERO(&failing);
                    FD_SET(static_cast<SOCKET>(which), &writing);
                    FD_SET(static_cast<SOCKET>(which), &failing);

                    // Windows melduje nieudane nawiązanie w zbiorze wyjątkowym, a nie w zbiorze do
                    // pisania — dlatego czekamy na oba naraz.
                    const int ready = ::select(0, nullptr, &writing, &failing, ever ? &how : nullptr);

                    if (ready == 0) {
                        handed(mine, 0, 0, true);

                        return;
                    }

                    if (ready < 0) {
                        handed(mine, 0, ::WSAGetLastError(), false);

                        return;
                    }

                    // Gotowość mówi tylko „już po" — **czy się udało**, mówi dopiero `SO_ERROR`.
                    handed(mine, 0, settling(which), false);
                });

                return false;
            }

            // ConnectEx wymaga gniazda już przypisanego — inaczej odmawia.
            if (!bound(one.held, Address::any(one.where.family(), 0))) {
                handed(fly->one, 0, amiss(), false);

                return false;
            }

            const int wide = filling(one.where, &fly->told);

            const BOOL made =
                joining(static_cast<SOCKET>(one.held), reinterpret_cast<sockaddr*>(&fly->told),
                        wide, nullptr, 0, &fly->much, &fly->over);

            if (!made && ::WSAGetLastError() != ERROR_IO_PENDING) {
                handed(fly->one, 0, ::WSAGetLastError(), false);

                return false;
            }

            return true;
        }

        case Kind::meeting: {
            // **Rura czeka na klienta na swojej instancji** (`pipes::spare`); przyjęta instancja
            // jest od razu strumieniem. Klient, który zdążył przed nami, nie daje zakończenia
            // w porcie — rozstrzygamy na miejscu. Klient, który zdążył też odejść, to strumień,
            // który od razu mówi „koniec".
            if (pipes::piped(one.held)) {
                if (::ConnectNamedPipe(reinterpret_cast<HANDLE>(one.held), &fly->over) != 0) return true;

                const DWORD why = ::GetLastError();

                if (why == ERROR_IO_PENDING) return true;

                if (why == ERROR_PIPE_CONNECTED || why == ERROR_NO_DATA) {
                    one.made = one.held;

                    handed(fly->one, 0, 0, false);

                    return false;
                }

                handed(fly->one, 0, static_cast<int>(why), false);

                return false;
            }

            asking();

            if (accepting == nullptr) {
                handed(fly->one, 0, WSAEOPNOTSUPP, false);

                return false;
            }

            const Family family = named(one.held, false).family();

            one.made = family == Family::none ? nothing : opened(family, true);

            if (one.made == nothing) {
                handed(fly->one, 0, amiss(), false);

                return false;
            }

            const BOOL made = accepting(static_cast<SOCKET>(one.held),
                                        static_cast<SOCKET>(one.made), fly->space, 0,
                                        static_cast<DWORD>(roomy), static_cast<DWORD>(roomy),
                                        &fly->much, &fly->over);

            if (!made && ::WSAGetLastError() != ERROR_IO_PENDING) {
                const int why = ::WSAGetLastError();

                ending(one.made);

                one.made = nothing;

                handed(fly->one, 0, why, false);

                return false;
            }

            return true;
        }

        case Kind::getting:
        case Kind::fetching: {
            if (pipes::piped(one.held)) {
                if (::ReadFile(reinterpret_cast<HANDLE>(one.held), one.buffer.data(), static_cast<DWORD>(one.buffer.size()), nullptr,
                               &fly->over) != 0)
                    return true;

                const DWORD why = ::GetLastError();

                if (why == ERROR_IO_PENDING) return true;

                handed(fly->one, 0, ended_pipe(why) ? 0 : static_cast<int>(why), false);

                return false;
            }

            fly->what.buf = reinterpret_cast<CHAR*>(one.buffer.data());
            fly->what.len = static_cast<ULONG>(one.buffer.size());

            const int made =
                one.kind == Kind::getting
                    ? ::WSARecv(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, &fly->marks,
                                &fly->over, nullptr)
                    : ::WSARecvFrom(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, &fly->marks,
                                    reinterpret_cast<sockaddr*>(&fly->told), &fly->wide,
                                    &fly->over, nullptr);

            if (made != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
                const int why = ::WSAGetLastError();

                // Datagram większy niż bufor to nie awaria, tylko obcięcie — i tak samo jak na
                // uniksach mówimy o tym wprost, zamiast oddać kawałek udający całość.
                if (one.kind == Kind::fetching && why == WSAEMSGSIZE) {
                    one.where = facing(&fly->told);
                    one.cut = true;

                    handed(fly->one, one.buffer.size(), 0, false);

                    return false;
                }

                handed(fly->one, 0, why, false);

                return false;
            }

            return true;
        }

        case Kind::putting:
        case Kind::sending: {
            if (pipes::piped(one.held)) {
                // Zapis zera bajtów dochodzi do czytającego jako zero bajtów — czyli jako koniec.
                // Gniazdo nie wysyła wtedy nic i rura też nie ma.
                if (one.much >= one.buffer.size()) {
                    handed(fly->one, one.much, 0, false);

                    return false;
                }

                if (::WriteFile(reinterpret_cast<HANDLE>(one.held), one.buffer.data() + one.much,
                                static_cast<DWORD>(one.buffer.size() - one.much), nullptr, &fly->over) != 0)
                    return true;

                const DWORD why = ::GetLastError();

                if (why == ERROR_IO_PENDING) return true;

                handed(fly->one, one.much, static_cast<int>(why), false);

                return false;
            }

            fly->what.buf = reinterpret_cast<CHAR*>(one.buffer.data()) + one.much;
            fly->what.len = static_cast<ULONG>(one.buffer.size() - one.much);

            int made = 0;

            if (one.kind == Kind::putting) {
                made = ::WSASend(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, 0,
                                 &fly->over, nullptr);
            } else {
                const int wide = filling(one.where, &fly->told);

                made = ::WSASendTo(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, 0,
                                   reinterpret_cast<sockaddr*>(&fly->told), wide, &fly->over,
                                   nullptr);
            }

            if (made != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
                handed(fly->one, one.much, ::WSAGetLastError(), false);

                return false;
            }

            return true;
        }
    }

    return false;
}

// Zakończenie wróciło z jądra. Tu rozstrzyga się, czy operacja jest skończona, czy trzeba ją
// puścić jeszcze raz — pisanie potrafi zejść po kawałku, a przyjęcie i nawiązanie wymagają
// dopowiedzenia systemowi, czym właśnie się stały.
void ended(const std::shared_ptr<Flying>& fly, DWORD much, int code) {
    Doing& one = *fly->one;

    // Koniec rury przy czytaniu: tyle, ile przyszło, i bez błędu — dokładnie jak zamknięte gniazdo.
    if (code != 0 && one.kind == Kind::getting && ended_pipe(static_cast<DWORD>(code)) && pipes::piped(one.held)) code = 0;

    if (code != 0) {
        // Za duży datagram to **nie jest awaria odbioru**, tylko odpowiedź: tyle się zmieściło,
        // reszta przepadła. Obsługa istniała wyłącznie na drodze natychmiastowej, więc zwykły
        // przebieg — zgłoszenie odbioru, zanim cokolwiek przyszło — meldował gołym numerem błędu
        // i gubił zarówno odebrane bajty, jak i adres nadawcy.
        if (one.kind == Kind::fetching && (code == WSAEMSGSIZE || code == ERROR_MORE_DATA)) {
            one.cut = true;
            one.where = facing(&fly->told);

            handed(fly->one, much, 0, fly->late);

            return;
        }

        if (one.kind == Kind::meeting && one.made != nothing) {
            ending(one.made);

            one.made = nothing;
        }

        handed(fly->one, one.kind == Kind::putting ? one.much : 0, code, fly->late);

        return;
    }

    switch (one.kind) {
        case Kind::joining: {
            // Bez tego getpeername i getsockname na świeżo połączonym gnieździe nic nie wiedzą.
            const SOCKET which = static_cast<SOCKET>(one.held);

            (void)::setsockopt(which, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);

            handed(fly->one, 0, 0, false);

            return;
        }

        case Kind::meeting: {
            if (pipes::piped(one.held)) {
                one.made = one.held;

                handed(fly->one, 0, 0, false);

                return;
            }

            const SOCKET which = static_cast<SOCKET>(one.held);

            (void)::setsockopt(static_cast<SOCKET>(one.made), SOL_SOCKET,
                               SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&which),
                               sizeof which);

            one.where = named(one.made, true);

            handed(fly->one, 0, 0, false);

            return;
        }

        case Kind::fetching: one.where = facing(&fly->told); [[fallthrough]];

        case Kind::getting: handed(fly->one, much, 0, fly->late); return;

        case Kind::sending: handed(fly->one, much, 0, fly->late); return;

        // Pisanie kończy się dopiero wtedy, gdy zeszło wszystko. Reszta idzie drugim
        // zgłoszeniem, bo bufor i licznik są tam, gdzie były.
        case Kind::putting: {
            one.much += much;

            if (one.much >= one.buffer.size()) {
                handed(fly->one, one.much, 0, false);

                return;
            }

            // Przerwany zapis **nie jest** powodzeniem, choćby część zeszła: połowy paczki nie da
            // się oddać wołającemu tak, jak oddaje się przeczytane bajty.
            if (fly->late) {
                handed(fly->one, 0, 0, true);

                return;
            }

            submit(fly->one);

            return;
        }
    }
}

}

Exchange::Exchange() : held_(std::make_unique<Held>()) {
    // Pula **musi** powstać przed centralą, choć nic jej tu nie potrzebuje. Obiekty statyczne
    // giną w kolejności odwrotnej do powstawania, a wątek centrali oddaje pracę do puli — więc
    // gdyby pula powstała później, zginęłaby wcześniej i wątek sięgałby do zwolnionej pamięci.
    (void)pooling();

    starting();

    held_->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);

    standing = held_.get();

    held_->staff = std::thread{[this] {
        Held& held = *held_;

        for (;;) {
            std::chrono::steady_clock::time_point soonest{};
            bool timed = false;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                for (const auto& [over, fly] : held.flying)
                    if (fly->one->ever && !fly->late && (!timed || fly->one->until < soonest)) {
                        soonest = fly->one->until;
                        timed = true;
                    }
            }

            DWORD patience = INFINITE;

            if (timed) {
                // **W górę, nie w dół** — ta sama poprawka i ten sam powód co przy zapleczu poll:
                // ucięcie do zera przy terminie bliższym niż milisekunda sprawiało, że port wracał
                // natychmiast, przegląd nie znajdował nic przeterminowanego i pętla kręciła się
                // w kółko na pełnym rdzeniu.
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                    soonest - std::chrono::steady_clock::now());

                // I **z ograniczeniem do zakresu**: `INFINITE` to `0xFFFFFFFF`, więc termin
                // odleglejszy niż mniej więcej czterdzieści dziewięć dni po rzutowaniu znaczyłby
                // „czekaj bez końca" — czyli nie zadziałałby nigdy.
                const std::int64_t much =
                    std::min<std::int64_t>(std::max<std::int64_t>(left.count(), 0),
                                           static_cast<std::int64_t>(INFINITE) - 1);

                patience = static_cast<DWORD>(much);
            }

            DWORD much = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* over = nullptr;

            const BOOL made = ::GetQueuedCompletionStatus(held.port, &much, &key, &over, patience);

            const int why = made ? 0 : static_cast<int>(::GetLastError());

            // Brak wskazania znaczy, że nic nie wróciło — minął czas albo port padł. Tak czy
            // owak trzeba obejść terminy.
            if (over != nullptr && key != waking) {
                std::shared_ptr<Flying> fly;

                {
                    const std::lock_guard<std::mutex> kept{held.guard};

                    if (const auto found = held.flying.find(over); found != held.flying.end()) {
                        fly = found->second;

                        held.flying.erase(found);
                    }

                    if (fly) {
                        if (const auto found = held.counted.find(fly->one->held);
                            found != held.counted.end() && found->second > 0)
                            --found->second;

                        sweeping(held, fly->one->held);
                    }
                }

                if (fly) ended(fly, much, why);
            }

            // Przeterminowane przerywamy **po jednej operacji**, a nie po gnieździe: na jednym
            // strumieniu może wisieć czytanie z terminem i pisanie bez. Zakończenie i tak wróci,
            // z liczbą bajtów, które zdążyły przejść — i to jest ta liczba, która nie może zginąć.
            std::vector<std::pair<Handle, OVERLAPPED*>> cutting;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                const auto now = std::chrono::steady_clock::now();

                for (auto& [which, fly] : held.flying)
                    if (fly->one->ever && !fly->late && fly->one->until <= now) {
                        fly->late = true;

                        cutting.emplace_back(fly->one->held, which);
                    }
            }

            for (const auto& [which, over_one] : cutting)
                (void)::CancelIoEx(reinterpret_cast<HANDLE>(which), over_one);
        }
    }};
}

// Rozbiórka ma kolejność i każdy jej krok ma powód.
Exchange::~Exchange() {
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->closing = true;
    }

    // 1. Wątek centrali staje — od tej chwili nikt już nie odda pracy do puli.
    if (held_->port != nullptr) ::PostQueuedCompletionStatus(held_->port, 0, waking, nullptr);

    if (held_->staff.joinable()) held_->staff.join();

    // 2. Centrala przestaje istnieć dla świata — **przed** opróżnieniem puli, bo zadanie kończone
    //    w puli potrafi zgłosić się z powrotem, a po zatrzymaniu wątku nie miałby go kto obsłużyć.
    standing.store(nullptr);

    // 3. Pula kończy to, co dostała.
    pooling().drain();

    // 4. Operacje, które jeszcze wiszą. **Nie wolno ich po prostu zwolnić**: jądro trzyma
    //    wskaźniki do ich OVERLAPPED, buforów i adresów, więc porzucone zostawiłyby je piszące do
    //    zwolnionej pamięci. Najpierw więc zrywamy je i dobieramy zakończenia, które zaraz wrócą.
    {
        std::vector<Handle> which;

        for (const auto& [over, fly] : held_->flying) which.push_back(fly->one->held);

        for (const Handle one : which) (void)::CancelIoEx(reinterpret_cast<HANDLE>(one), nullptr);
    }

    // **Zgłoszenie jest zarazem wynikiem obietnicy wołającego**, więc każde, które tu przechodzi,
    //    musi zostać rozstrzygnięte — porzucone zostawia `Ticket` w stanie świeżym, a wątek
    //    stojący na `await()` nie budzi się nigdy i program nie kończy wyjścia. Zaplecze POSIX robi
    //    to samo w tym samym miejscu (`exchange.cpp`, `done(0, ECONNABORTED)`) i ma nad tym
    //    komentarz mówiący, że był to prawdziwy błąd znaleziony pomiarem; tutaj tego brakowało.
    //
    //    Wołamy `done()` **wprost, nie przez `ended()`**. Ta druga przechodzi przez `handed()`,
    //    które wrzuca ciąg dalszy do puli — a pulę opróżniliśmy krok wcześniej i jesteśmy
    //    w niszczeniu statyków. POSIX z tego samego powodu woła `done()` bezpośrednio.
    const auto aborted = [](const std::shared_ptr<Flying>& fly) {
        Doing& one = *fly->one;

        // Przyjęcie odrzucone zostawiłoby otwarte gniazdo, na którym miało stanąć — zwykła
        // ścieżka zamyka je w tym samym miejscu.
        if (one.kind == Kind::meeting && one.made != nothing) {
            ending(one.made);

            one.made = nothing;
        }

        one.done(0, WSAECONNABORTED);
    };

    for (int turn = 0; turn < 1000 && !held_->flying.empty() && held_->port != nullptr; ++turn) {
        DWORD much = 0;
        ULONG_PTR whose = 0;
        OVERLAPPED* over = nullptr;

        if (::GetQueuedCompletionStatus(held_->port, &much, &whose, &over, 10) == 0 &&
            over == nullptr)
            continue;

        if (over == nullptr) continue;

        const auto at = held_->flying.find(over);

        if (at != held_->flying.end()) {
            aborted(at->second);

            held_->flying.erase(at);
        }
    }

    // Czego jądro nie oddało w tym czasie, tego i tak nie doczekamy — a wtedy **zostawiamy te
    // operacje przy życiu**, zamiast zwalniać pamięć spod jądra. Wyciek na wyjściu z programu jest
    // mniejszym złem niż zapis do zwolnionego bufora.
    //
    // Przeciek musi być **prawdziwy**, czyli nigdy niezwalniany. Statyk lokalny funkcji tego nie
    // daje: powstaje w trakcie fazy niszczenia statyków, a więc zostanie zniszczony zaraz po
    // powrocie stąd — sprowadzając dokładnie ten zapis do zwolnionej pamięci, przed którym miał
    // bronić. Stąd wskazanie na stertę, którego nikt nie odbiera.
    //
    // Rozstrzygnięcie obietnicy niczego nie zwalnia, więc jedno z drugim się nie gryzie:
    // wołający dostaje odpowiedź, a bufory zostają jądru.
    auto* stranded = new std::vector<std::shared_ptr<Flying>>;

    for (auto& [over, fly] : held_->flying) {
        aborted(fly);

        stranded->push_back(fly);
    }

    held_->flying.clear();

    for (const Handle one : held_->leaving) ending(one);

    if (held_->port != nullptr) ::CloseHandle(held_->port);
}

std::size_t Exchange::waiting() const {
    const std::lock_guard<std::mutex> kept{held_->guard};

    std::size_t much = 0;

    for (const auto& [which, count] : held_->counted)
        if (count > 0) ++much;

    return much;
}

Exchange& Exchange::running() {
    static Exchange one;

    return one;
}

void submit(std::shared_ptr<Doing> one) {
    (void)Exchange::running();

    Exchange::Held* held = standing.load();

    if (held == nullptr) {
        one->done(0, WSAECONNABORTED);

        return;
    }

    auto fly = std::make_shared<Flying>();

    fly->one = std::move(one);

    OVERLAPPED* over = &fly->over;
    const Handle which = fly->one->held;

    int why = 0;

    // Wpisujemy **przed** rozpoczęciem, bo zakończenie potrafi wrócić, zanim wywołanie systemowe
    // odda sterowanie — i wtedy szukalibyśmy operacji, której jeszcze nie ma w mapie.
    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->flying.emplace(over, fly);
        ++held->counted[which];

        // Przypięcie do portu dotyka mapy centrali, więc musi się zdarzyć tu, pod zamkiem —
        // a nie w began, które biegnie już bez niego.
        why = tying(*held, which);
    }

    // **Nieprzypiętego uchwytu nie zaczynamy.** Zakończenie nie miałoby dokąd wrócić, więc operacja
    // biegłaby w jądrze, a obietnica czekałaby na nie w nieskończoność.
    if (why == 0 && began(fly)) {
        // Bez tego centrala spałaby do poprzedniego terminu — a przy pustej centrali w
        // nieskończoność, więc świeży termin nigdy by nie minął.
        if (fly->one->ever)
            (void)::PostQueuedCompletionStatus(held->port, 0, waking, nullptr);

        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->flying.erase(over);

        if (const auto found = held->counted.find(which);
            found != held->counted.end() && found->second > 0)
            --found->second;

        sweeping(*held, which);
    }

    // Gdy `began` oddało fałsz, zgłoszenie jest już rozstrzygnięte — samo to zrobiło. Gdy poległo
    // przypięcie, **nie zrobił tego nikt**, bo `began` nawet nie ruszyło; rozstrzygamy tutaj.
    if (why != 0) handed(fly->one, 0, why, false);
}

// Zdjęcie jednego zgłoszenia. Tu wygląda inaczej niż na uniksach i **inaczej się kończy**.
//
// Tam zgłoszenie leży w mapie i wystarczy je stamtąd wyjąć: nikt go nigdy nie ruszy. Tu operacja
// już biegnie w jądrze, a `CancelIoEx` tylko **prosi** o jej przerwanie — zakończenie i tak
// przyjdzie do portu, z ERROR_OPERATION_ABORTED, i dopiero ono zdejmie znacznik kierunku.
// Dlatego zawsze fałsz: my niczego nie zdjęliśmy i wołającemu nie wolno sprzątać po sobie samemu.
// Od powiedzenia czekającemu właściwej rzeczy jest `Ticket::quitted`, a nie ta funkcja.
//
// Przeglądamy `flying` po kolei świadomie. Wpisów jest tyle, ile operacji w locie, a anulowanie
// nie jest ścieżką gorącą; osobna mapa kosztowałaby wpis przy **każdym** zgłoszeniu, żeby
// przyspieszyć rzecz rzadką.
bool withdraw(const std::shared_ptr<Doing>& one) {
    Exchange::Held* held = standing.load();

    if (held == nullptr || !one || one->held == nothing) return false;

    OVERLAPPED* over = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        for (const auto& [which, fly] : held->flying)
            if (fly->one == one) {
                over = which;

                break;
            }
    }

    if (over != nullptr) (void)::CancelIoEx(reinterpret_cast<HANDLE>(one->held), over);

    return false;
}

// Zamknięcie deskryptora, oddane centrali. **Woła się je dopiero wtedy, gdy ginie ostatni uchwyt
// do gniazda.** Zakończenia w locie trzymają uchwyt przez `kept`, więc dopóki którekolwiek żyje,
// tu nie trafiamy — a numer zamkniętego gniazda system natychmiast oddaje następnemu.
void retiring(Handle one) {
    if (one == nothing) return;

    Exchange::Held* held = standing.load();

    if (held == nullptr) {
        ending(one);

        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->leaving.insert(one);

        sweeping(*held, one);
    }
}

// Zapomnienie przerywa **wszystko**, co na gnieździe wisiało. Deskryptora nie zamyka — od tego
// jest retiring.
void forget(Handle one) {
    Exchange::Held* held = standing.load();

    if (held == nullptr) return;

    (void)::CancelIoEx(reinterpret_cast<HANDLE>(one), nullptr);

    shutting(one);
}

}

#endif

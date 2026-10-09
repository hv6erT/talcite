#include "talcite/child.hpp"

#include "talcite/encoding.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"
#include "talcite/term.hpp"

#include "launching.hpp"
#include "sealed.hpp"

#ifdef _WIN32

#include <windows.h>

#include <cwchar>
#include <cwctype>

#else

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#endif

namespace talcite {

namespace {

// Rata czytania. Ta sama, którą `fs` bierze na udział sieciowy: rura nie ma rozmiaru, o który
// dałoby się zapytać, więc jedyne, co możemy wybrać, to jak często wracać po odpowiedź na pytanie
// „czy jeszcze jestem potrzebny".
constexpr std::size_t brim = 64 * 1024;

#ifdef _WIN32

// Rura anonimowa nie da się czytać z ogranicznikiem czasu, a `ReadFile` na niej stoi do skutku —
// czyli do końca cudzego programu. Zaglądanie co tyle jest jedynym sposobem, żeby anulowanie
// w ogóle miało kiedy zadziałać.
constexpr std::chrono::milliseconds peeking{20};

#endif

[[noreturn]] void amiss(const std::string& said, const fs::path& where) {
    throw fs::Trouble{said, where};
}

}

// Dziecko powstaje w jednym miejscu w tym pliku, ale opakować je musi ktoś, kto ma prawo do
// prywatnego konstruktora. Zamiast otwierać go wszystkim, otwieramy go jednej klasie, której poza
// tym plikiem nie ma.
class Rearing {
public:
    [[nodiscard]] static Child made(std::shared_ptr<Child::Held> held) {
        return Child{std::move(held)};
    }

    [[nodiscard]] static const std::shared_ptr<Child::Held>& inside(const Child& one) {
        return one.held_;
    }
};

// **Grób.** Wszystko, co jest potrzebne, żeby program pochować i żeby go po pochowaniu nie zabić
// drugi raz — trzymane osobno od reszty, bo przeżywa `Child`: zadanie czekające na koniec ma
// prawo dobiec do końca także wtedy, gdy ostatni uchwyt puszczono wcześniej.
//
// Osobno **musi** być jeszcze z jednego powodu: obietnica z `wait()` mieszka w `Held`, a jej
// domknięcie musi trzymać to, na co czeka. Trzymając `Held`, trzymałaby samą siebie i nic by się
// nigdy nie zwolniło.
struct Grave {
    std::mutex guard;
    bool gone = false;
    std::int64_t which = -1;

    // Czy program stoi we **własnej** grupie. Na Windowsie uprzejme zakończenie idzie zdarzeniem
    // konsoli, a to zdarzenie trafia w całą grupę — więc wysłane do programu, który siedzi w
    // naszej, zabiłoby przy okazji nas samych.
    bool grouped = false;

    // Czy program dostał nasz terminal. Tylko taki wznawiamy po zatrzymaniu — program na rurach
    // zatrzymany ręką z zewnątrz ma zostać zatrzymany.
    bool foreground = false;

#ifdef _WIN32
    HANDLE process = nullptr;

    // Zadanie (job) obejmujące program i wszystko, co on uruchomi. `kill()` kończy całe drzewo:
    // plik wsadowy to cmd.exe, który dopiero uruchamia właściwy program, a zabity sam cmd.exe
    // zostawiał go przy życiu. Zamknięcie uchwytu niczego nie zabija — bez
    // `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, tak jak puszczenie `Child` zostawia program swojemu losowi.
    HANDLE job = nullptr;

    ~Grave() {
        if (process != nullptr) CloseHandle(process);
        if (job != nullptr) CloseHandle(job);
    }
#endif
};

struct Child::Held {
    std::mutex guard;

    std::shared_ptr<Grave> grave = std::make_shared<Grave>();

#ifdef _WIN32
    HANDLE into = nullptr;
    HANDLE from = nullptr;
    HANDLE wrong = nullptr;

    // Zdarzenie budzące. `close_input()` je podnosi, a pisarz czeka na nie **razem** z zakończeniem
    // własnego zapisu — bo synchronicznego `WriteFile`, który już stanął na pełnej rurze, nie da
    // się obudzić inaczej niż przerwaniem samego wejścia-wyjścia. Ręczne, bo ma zostać podniesione
    // na zawsze: budzi każdego piszącego, a nie tylko pierwszego, tak samo jak rura budząca
    // na POSIX-ie.
    HANDLE rouse = nullptr;
#else
    int into = -1;
    int from = -1;
    int wrong = -1;

    // Rura budząca. `close_input()` wrzuca do niej bajt, a pisarz stojący na `poll` widzi go obok
    // wejścia i dzwonka — bo **zamknięcie deskryptora nie budzi `poll`, który już na nim stoi**.
    // Bez tego `close_input()` na dziecku, które nie czyta, kończył zapis dopiero ze śmiercią
    // dziecka.
    int rouse[2] = {-1, -1};
#endif

    // **Zamknięcie wejścia jest odroczone.** Zwolniony numer deskryptora system natychmiast
    // rozdaje dalej, więc zamknięcie go spod biegnącego wywołania systemowego grozi tym, że
    // treść dla dziecka trafi w cudzy plik. Zamyka więc ostatni wychodzący, a nie ten, kto
    // poprosił: `close_input()` stawia znacznik, a `poured` liczy siebie.
    int writers = 0;
    bool sealing = false;

    bool merge = false;

    std::optional<Promise<Exit>> waiting;

    fs::path program;

    ~Held();
};

namespace {

#ifdef _WIN32

void closed(HANDLE& one) {
    if (one == nullptr || one == INVALID_HANDLE_VALUE) return;

    CloseHandle(one);
    one = nullptr;
}

#else

void closed(int& one) {
    if (one < 0) return;

    ::close(one);
    one = -1;
}

#endif

}

Child::Held::~Held() {
    closed(into);
    closed(from);
    closed(wrong);

#ifdef _WIN32

    if (rouse != nullptr) CloseHandle(rouse);

#else

    closed(rouse[0]);
    closed(rouse[1]);

#endif
}

namespace {

// Odczyt jednej raty z jednej rury. Wołane z robotnika puli, więc dzwonek tego robotnika jest tym,
// co zamienia `cancel()` na obietnicy w przerwane czekanie — a nie w przerwane wywołanie systemowe.
// Pusty wynik znaczy **koniec rury**, nie błąd: program się skończył i nikt już nic nie napisze.
Bytes sipped(Child::Held& held, bool errors, std::size_t most) {
    Bytes made;

    if (most == 0) return made;

#ifdef _WIN32

    HANDLE one = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        one = errors ? held.wrong : held.from;
    }

    if (one == nullptr) return made;

    Bell* bell = ringing();

    for (;;) {
        DWORD ready = 0;

        if (PeekNamedPipe(one, nullptr, 0, nullptr, &ready, nullptr) == 0) return made;

        if (ready == 0) {
            if (bell != nullptr && bell->wait(peeking)) throw Cancelled{};

            if (bell == nullptr) Sleep(static_cast<DWORD>(peeking.count()));

            continue;
        }

        const DWORD asked = ready < static_cast<DWORD>(most) ? ready
                                                             : static_cast<DWORD>(most);

        made.resize(asked);

        DWORD given = 0;

        if (ReadFile(one, made.data(), asked, &given, nullptr) == 0) {
            made.clear();

            return made;
        }

        made.resize(given);

        return made;
    }

#else

    int one = -1;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        one = errors ? held.wrong : held.from;
    }

    if (one < 0) return made;

    Bell* bell = ringing();

    for (;;) {
        pollfd watched[2];
        int many = 1;

        watched[0] = pollfd{one, POLLIN, 0};

        if (bell != nullptr) {
            watched[1] = pollfd{bell->waited(), POLLIN, 0};
            many = 2;
        }

        const int ready = ::poll(watched, static_cast<nfds_t>(many), -1);

        if (ready < 0) {
            if (errno == EINTR) continue;

            amiss("Cannot read from the program.", held.program);
        }

        if (many == 2 && (watched[1].revents & POLLIN) != 0) throw Cancelled{};

        // **`POLLNVAL` musi być w masce**, choć się o niego nie prosi. `poll` zgłasza go, gdy
        // deskryptor jest już zamknięty, i wraca z nim **natychmiast** — bez tego warunku pętla
        // kręciłaby się bez końca, paląc robotnika puli i nie rozstrzygając obietnicy. To nie jest
        // stan hipotetyczny: rury zamyka `close_input()` i destruktor, oba z innego wątku niż ten.
        if ((watched[0].revents & POLLNVAL) != 0)
            amiss("The program output is already closed.", held.program);

        if ((watched[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

        made.resize(most);

        const ssize_t given = ::read(one, made.data(), most);

        if (given < 0) {
            made.clear();

            if (errno == EINTR || errno == EAGAIN) continue;

            amiss("Cannot read from the program.", held.program);
        }

        made.resize(static_cast<std::size_t>(given));

        return made;
    }

#endif
}

#ifndef _WIN32

// Pisanie do rury, której drugi koniec już nie żyje, wysyła SIGPIPE — a domyślną reakcją na
// SIGPIPE jest śmierć **naszego** procesu. Blokujemy go na czas pisania i tylko na tym wątku:
// ustawienie go na ignorowany raz na zawsze byłoby cichą zmianą warunków całej aplikacji, o którą
// nikt nas nie prosił. Sygnał, który zdążył się zebrać, zdejmujemy z kolejki sami.
class Muffled {
public:
    Muffled() {
        sigemptyset(&mine_);
        sigaddset(&mine_, SIGPIPE);

        held_ = pthread_sigmask(SIG_BLOCK, &mine_, &was_) == 0;
    }

    ~Muffled() {
        if (!held_) return;

        sigset_t waiting;

        if (sigpending(&waiting) == 0 && sigismember(&waiting, SIGPIPE) == 1) {
            int which = 0;

            (void)sigwait(&mine_, &which);
        }

        (void)pthread_sigmask(SIG_SETMASK, &was_, nullptr);
    }

    Muffled(const Muffled&) = delete;
    Muffled& operator=(const Muffled&) = delete;
    Muffled(Muffled&&) = delete;
    Muffled& operator=(Muffled&&) = delete;

private:
    sigset_t mine_{};
    sigset_t was_{};
    bool held_ = false;
};

#endif

// **Ostatni wychodzący zamyka.** Strażnik, bo `poured` wychodzi także wyjątkiem — przez `amiss`,
// przez `Cancelled` z dzwonka i przez `throw` z samego zamknięcia. Gdyby licznik opuszczała zwykła
// linijka na końcu, każde z tych wyjść zostawiałoby wejście otwarte na zawsze.
class Writing {
public:
    explicit Writing(Child::Held& held) : held_(held) {}

    ~Writing() {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (--held_.writers == 0 && held_.sealing) closed(held_.into);
    }

    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;
    Writing(Writing&&) = delete;
    Writing& operator=(Writing&&) = delete;

private:
    Child::Held& held_;
};

std::size_t poured(Child::Held& held, const Bytes& what) {
    if (what.empty()) return 0;

#ifdef _WIN32

    HANDLE one = nullptr;
    HANDLE wake = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (held.sealing || held.into == nullptr)
            amiss("The program input is already closed.", held.program);

        one = held.into;
        wake = held.rouse;

        ++held.writers;
    }

    const Writing counted{held};

    // Zdarzenie zakończenia zapisu. Własne, nie współdzielone: każda rata czeka na swoje.
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (ready == nullptr) amiss("Cannot write to the program.", held.program);

    // Zamykamy je każdą drogą wyjścia — także wyjątkiem z `amiss`.
    const std::shared_ptr<void> closing{ready, [](void* which) { CloseHandle(which); }};

    std::size_t done = 0;

    while (done < what.size()) {
        const std::size_t left = what.size() - done;
        const DWORD asked = left > brim ? static_cast<DWORD>(brim) : static_cast<DWORD>(left);

        OVERLAPPED over{};

        ResetEvent(ready);

        over.hEvent = ready;

        DWORD given = 0;

        // Uchwyt jest nakładany, więc `WriteFile` prawie zawsze wraca „w toku" i to jest sytuacja
        // zwyczajna, a nie awaria. Prawda znaczy „zeszło od razu"; wtedy i tak pytamy o wynik
        // jednym wywołaniem niżej, żeby nie mieć dwóch dróg wyjścia.
        if (WriteFile(one, what.data() + done, asked, &given, &over) == 0) {
            if (GetLastError() != ERROR_IO_PENDING)
                amiss("Cannot write to the program.", held.program);

            // **Czekamy na dwie rzeczy naraz**: na zejście raty i na zapieczętowanie. To jest ten
            // sam kształt, co `poll` na POSIX-ie, i z tego samego powodu — zamknięcie wejścia musi
            // przerwać zapis stojący na pełnej rurze, a nie czekać na śmierć dziecka.
            const HANDLE these[2] = {ready, wake};
            const DWORD many = wake != nullptr ? 2 : 1;

            const DWORD which = WaitForMultipleObjects(many, these, FALSE, INFINITE);

            if (which == WAIT_OBJECT_0 + 1) {
                // Przerwaną ratę **trzeba zabrać jądru**, zanim zniknie jej `OVERLAPPED` i bufor.
                // `CancelIoEx` prosi o przerwanie, a `GetOverlappedResult` z czekaniem oddaje
                // pewność, że jądro już tej pamięci nie dotyka.
                (void)CancelIoEx(one, &over);
                (void)GetOverlappedResult(one, &over, &given, TRUE);

                amiss("The program input is already closed.", held.program);
            }

            if (which != WAIT_OBJECT_0) amiss("Cannot write to the program.", held.program);
        }

        if (GetOverlappedResult(one, &over, &given, TRUE) == 0 || given == 0)
            amiss("Cannot write to the program.", held.program);

        done += given;
    }

    return done;

#else

    int one = -1;
    int wake = -1;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (held.sealing || held.into < 0)
            amiss("The program input is already closed.", held.program);

        one = held.into;
        wake = held.rouse[0];

        ++held.writers;
    }

    const Writing counted{held};

    const Muffled quiet;

    Bell* bell = ringing();

    std::size_t done = 0;

    while (done < what.size()) {
        // Trzy rzeczy naraz: wejście do dziecka, dzwonek robotnika i rura budząca. Dzwonek zamienia
        // `cancel()` w przerwane czekanie, a rura budząca — `close_input()`. Bez tej trzeciej
        // pisarz stojący na pełnej rurze budzi się dopiero, gdy dziecko coś przeczyta albo umrze.
        pollfd watched[3];
        int many = 1;

        watched[0] = pollfd{one, POLLOUT, 0};

        if (bell != nullptr) {
            watched[many] = pollfd{bell->waited(), POLLIN, 0};
            ++many;
        }

        const int roused = wake < 0 ? -1 : many;

        if (roused >= 0) {
            watched[many] = pollfd{wake, POLLIN, 0};
            ++many;
        }

        const int ready = ::poll(watched, static_cast<nfds_t>(many), -1);

        if (ready < 0) {
            if (errno == EINTR) continue;

            amiss("Cannot write to the program.", held.program);
        }

        if (bell != nullptr && (watched[1].revents & POLLIN) != 0) throw Cancelled{};

        // Rura budząca jest **poziomowa i nigdy nie opróżniana**: bajt wrzucony przez
        // `close_input()` zostaje w niej na zawsze, więc budzi wszystkich piszących, a nie tylko
        // pierwszego.
        if (roused >= 0 && (watched[roused].revents & POLLIN) != 0)
            amiss("The program input is already closed.", held.program);

        // Zamknięcie **między** obrotami pętli: wtedy `poll` wraca z tym natychmiast. Zamknięcia
        // w trakcie własnego czekania `poll` nie zauważa — od tego jest rura budząca.
        if ((watched[0].revents & POLLNVAL) != 0)
            amiss("The program input is already closed.", held.program);

        if ((watched[0].revents & (POLLOUT | POLLERR | POLLHUP)) == 0) continue;

        const ssize_t given = ::write(one, what.data() + done, what.size() - done);

        if (given < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;

            amiss("Cannot write to the program.", held.program);
        }

        done += static_cast<std::size_t>(given);
    }

    return done;

#endif
}

// Czekanie na koniec. Stoi na własnym robotniku puli przez cały czas życia programu — pula rośnie
// dla pracy blokującej bez ograniczeń, więc nikogo tym nie zagłodzimy, a odpytywanie co jakiś czas
// kosztowałoby czuwanie w bezczynności.
Exit buried(const std::shared_ptr<Grave>& grave, const fs::path& program) {
    Exit made;

#ifdef _WIN32

    HANDLE one = nullptr;

    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        one = grave->process;
    }

    if (one == nullptr) {
        made.ended = Ended::lost;

        return made;
    }

    if (WaitForSingleObject(one, INFINITE) != WAIT_OBJECT_0)
        amiss("Cannot wait for the program to end.", program);

    DWORD code = 0;

    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        grave->gone = true;

        if (GetExitCodeProcess(one, &code) == 0) {
            made.ended = Ended::lost;

            return made;
        }
    }

    // Program zabity przez nas kończy się kodem, który sami mu podaliśmy. Windows nie zna sygnałów,
    // więc nie ma tu czego rozdzielać — zostaje zwykłe wyjście z kodem.
    made.ended = Ended::normal;
    made.code = static_cast<int>(code);

    return made;

#else

    const std::int64_t which = grave->which;

    int told = 0;

    for (;;) {
        // **Zatrzymanie to nie koniec.** Program z naszym terminalem zatrzymany przez Ctrl+Z
        // (SIGTSTP) dawniej stał na zawsze: `waitpid` bez `WUNTRACED` nie wracał, terminal należał
        // do zatrzymanej grupy, a twa czekała. Kontroli zadań nie prowadzimy, więc zatrzymany
        // program wznawiamy — Ctrl+Z w programie uruchomionym z twa nic nie robi, jak w MC.
        const pid_t done = ::waitpid(static_cast<pid_t>(which), &told, grave->foreground ? WUNTRACED : 0);

        if (done > 0 && WIFSTOPPED(told)) {
            (void)::kill(-static_cast<pid_t>(which), SIGCONT);

            continue;
        }

        if (done > 0) break;

        if (errno == EINTR) continue;

        {
            const std::lock_guard<std::mutex> kept{grave->guard};

            grave->gone = true;
        }

        made.ended = Ended::lost;

        return made;
    }

    // Pochowany. Od tej chwili numer wolno przydzielić komuś innemu, więc każdy sygnał wysłany pod
    // niego trafiłby w obcy program — i dlatego `stop()` oraz `kill()` pytają o ten sam zamek.
    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        grave->gone = true;
    }

    if (WIFEXITED(told)) {
        made.ended = Ended::normal;
        made.code = WEXITSTATUS(told);
    } else if (WIFSIGNALED(told)) {
        made.ended = Ended::killed;
        made.signal = WTERMSIG(told);
    } else {
        made.ended = Ended::lost;
    }

    return made;

#endif
}

#ifdef _WIN32

// Cytowanie według reguł, którymi rozbiera wiersz poleceń biblioteka uruchomieniowa MSVC: ukośnik
// odwrotny jest znakiem ucieczki **tylko** przed cudzysłowem i przed innym ukośnikiem stojącym
// przed cudzysłowem. Robimy to sami, bo `CreateProcessW` bierze jeden napis, a wołający podał nam
// argumenty osobno i nie ma powodu, żeby wiedział o tej regule.
void quoted(const std::wstring& one, std::wstring& into) {
    if (!one.empty() && one.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        into += one;

        return;
    }

    into += L'"';

    for (std::size_t at = 0; at < one.size(); ++at) {
        std::size_t slashes = 0;

        while (at < one.size() && one[at] == L'\\') {
            ++at;
            ++slashes;
        }

        if (at == one.size()) {
            into.append(slashes * 2, L'\\');

            break;
        }

        if (one[at] == L'"') {
            into.append(slashes * 2 + 1, L'\\');
        } else {
            into.append(slashes, L'\\');
        }

        into += one[at];
    }

    into += L'"';
}

// Program po nazwie szukamy **sami**: w katalogu naszego programu, potem w PATH, a w każdym
// miejscu z rozszerzeniami z PATHEXT. `CreateProcessW` z samą nazwą zaglądał po drodze do katalogu
// bieżącego — a to bywa katalog panelu z cudzym `git.exe` w środku — i dokładał wyłącznie `.exe`,
// więc `npm` (npm.cmd) się nie uruchamiał. Ścieżkę z ukośnikiem albo dyskiem bierzemy, jak jest.
std::wstring sought(const fs::path& program) {
    const std::wstring named = program.wstring();

    std::vector<std::wstring> endings{L""};

    if (program.extension().empty()) {
        endings.clear();

        std::wstring listed(32767, L'\0');

        const DWORD much = GetEnvironmentVariableW(L"PATHEXT", listed.data(), static_cast<DWORD>(listed.size()));

        listed.resize(much > 0 && much < listed.size() ? much : 0);

        if (listed.empty()) listed = L".COM;.EXE;.BAT;.CMD";

        for (std::size_t at = 0; at <= listed.size();) {
            const std::size_t next = std::min(listed.find(L';', at), listed.size());

            if (next > at) endings.push_back(listed.substr(at, next - at));

            at = next + 1;
        }
    }

    std::vector<std::wstring> places;

    if (named.find_first_of(L"\\/:") != std::wstring::npos) {
        places.emplace_back();
    } else {
        std::wstring own(32768, L'\0');

        const DWORD much = GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));

        if (much > 0 && much < own.size()) {
            own.resize(much);
            places.push_back(fs::path{own}.parent_path().wstring());
        }

        std::wstring listed(32767, L'\0');

        const DWORD told = GetEnvironmentVariableW(L"PATH", listed.data(), static_cast<DWORD>(listed.size()));

        listed.resize(told > 0 && told < listed.size() ? told : 0);

        for (std::size_t at = 0; at <= listed.size();) {
            const std::size_t next = std::min(listed.find(L';', at), listed.size());

            std::wstring one = listed.substr(at, next - at);

            one.erase(std::remove(one.begin(), one.end(), L'"'), one.end());

            if (!one.empty()) places.push_back(std::move(one));

            at = next + 1;
        }
    }

    for (const std::wstring& place : places)
        for (const std::wstring& ending : endings) {
            const std::wstring tried = place.empty() ? named + ending : (fs::path{place} / (named + ending)).wstring();

            const DWORD told = GetFileAttributesW(tried.c_str());

            if (told != INVALID_FILE_ATTRIBUTES && (told & FILE_ATTRIBUTE_DIRECTORY) == 0) return tried;
        }

    // Nie znaleziony — odmawiamy tutaj. Oddana dalej sama nazwa wróciłaby w `CreateProcessW` do
    // katalogu bieżącego, czyli dokładnie tam, skąd mieliśmy nie brać.
    amiss("Cannot start the program.", program);
}

bool batched(const std::wstring& program) {
    std::wstring ending = fs::path{program}.extension().wstring();

    for (wchar_t& one : ending) one = static_cast<wchar_t>(std::towlower(one));

    return ending == L".bat" || ending == L".cmd";
}

// Plik wsadowy uruchamia cmd.exe, który **sam rozbiera wiersz** według własnych reguł: `&`, `|`
// i `%ZMIENNA%` w argumencie wykonałyby cudze polecenie (BatBadBut, 2024). Wołamy więc cmd.exe
// jawnie — z katalogu systemowego, bez AutoRun (`/d`), bez opóźnionego rozwijania (`/v:off`) —
// a każdy argument idzie w cudzysłowie, w którym `&|<>^()` nic nie znaczą. Zostają znaki, których
// cudzysłów nie chroni: cudzysłów, procent i koniec wiersza. Takiego argumentu nie przekazujemy
// wcale — odmowa jest lepsza niż zgadywanie, co z niego zrobi cmd.
void batch_quoted(const std::wstring& one, std::wstring& into, const fs::path& program) {
    if (one.find_first_of(L"\"%\r\n") != std::wstring::npos || one.find(L'\0') != std::wstring::npos)
        amiss("This argument cannot be passed to a batch file.", program);

    into += L'"';
    into += one;
    into += L'"';
}

struct Launch {
    std::wstring application;
    std::wstring line;
};

Launch commanded(const Command& what) {
    Launch made;

    made.application = sought(what.program);

    if (batched(made.application)) {
        std::wstring system(MAX_PATH, L'\0');

        const UINT much = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));

        system.resize(much > 0 && much < system.size() ? much : 0);

        const std::wstring shell = (fs::path{system} / L"cmd.exe").wstring();

        made.line = L"cmd.exe /d /v:off /s /c \"";

        batch_quoted(made.application, made.line, what.program);

        for (const std::string& one : what.arguments) {
            made.line += L' ';

            batch_quoted(to_wstring(one), made.line, what.program);
        }

        made.line += L'"';
        made.application = shell;

        return made;
    }

    quoted(what.program.wstring(), made.line);

    for (const std::string& one : what.arguments) {
        made.line += L' ';

        quoted(to_wstring(one), made.line);
    }

    return made;
}

// Blok środowiska to ciąg par `nazwa=wartość`, każda zakończona zerem, całość drugim zerem.
std::wstring surrounded(const Command& what) {
    std::vector<std::wstring> among;

    if (what.inherit) {
        wchar_t* whole = GetEnvironmentStringsW();

        if (whole != nullptr) {
            for (wchar_t* at = whole; *at != L'\0';) {
                const std::wstring one{at};

                // Windows trzyma na początku bloku pozycje zaczynające się od `=`; to nie są
                // zmienne, tylko katalogi bieżące dysków, i przepisujemy je bez ruszania.
                among.push_back(one);

                at += one.size() + 1;
            }

            FreeEnvironmentStringsW(whole);
        }
    }

    for (const Variable& one : what.environment) {
        const std::wstring name = to_wstring(one.name);
        const std::wstring said = name + L'=' + to_wstring(one.value);

        const auto at = std::find_if(among.begin(), among.end(), [&name](const std::wstring& had) {
            return had.size() > name.size() && had[name.size()] == L'=' &&
                   _wcsnicmp(had.c_str(), name.c_str(), name.size()) == 0;
        });

        if (at == among.end()) among.push_back(said);
        else *at = said;
    }

    std::wstring made;

    for (const std::wstring& one : among) {
        made += one;
        made += L'\0';
    }

    made += L'\0';

    return made;
}

#else

// Wyszukanie programu po PATH robimy **przed** rozwidleniem, żeby w dziecku wywołać `execv`,
// a nie `execvp`: między `fork` a `exec` wolno wołać tylko to, co jest bezpieczne w obsłudze
// sygnału, a `execvp` po drodze przydziela pamięć. Tu przydzielamy w rodzicu, na spokojnie.
std::string found(const fs::path& program) {
    const std::string named = program.string();

    if (named.find('/') != std::string::npos) return named;

    const char* whole = std::getenv("PATH");

    if (whole == nullptr) return named;

    const std::string said{whole};

    std::size_t at = 0;

    while (at <= said.size()) {
        const std::size_t next = said.find(':', at);
        const std::string one = said.substr(at, next == std::string::npos ? next : next - at);

        const std::string tried = (one.empty() ? std::string{"."} : one) + "/" + named;

        if (::access(tried.c_str(), X_OK) == 0) return tried;

        if (next == std::string::npos) break;

        at = next + 1;
    }

    return named;
}

std::vector<std::string> surrounded(const Command& what) {
    std::vector<std::string> among;

    if (what.inherit && environ != nullptr)
        for (char** at = environ; *at != nullptr; ++at) among.emplace_back(*at);

    for (const Variable& one : what.environment) {
        const std::string said = one.name + "=" + one.value;

        const auto at = std::find_if(among.begin(), among.end(), [&one](const std::string& had) {
            return had.size() > one.name.size() && had[one.name.size()] == '=' &&
                   had.compare(0, one.name.size(), one.name) == 0;
        });

        if (at == among.end()) among.push_back(said);
        else *at = said;
    }

    return among;
}

#endif

// Uruchomienie. `ahead` znaczy „ten program dostaje nasz terminal": wtedy nie zakładamy rur wcale,
// bo jego wejściem jest klawiatura, a wyjściem ekran — i dodatkowo stawiamy go na pierwszym planie,
// żeby Ctrl-C szło do niego, a nie do nas.
std::shared_ptr<Child::Held> reared(const Command& what, bool ahead) {
    auto held = std::make_shared<Child::Held>();

    held->program = what.program;
    held->merge = what.merge;

    if (what.program.empty()) amiss("No program given.", what.program);

#ifdef _WIN32

    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;

    SECURITY_ATTRIBUTES how{};
    how.nLength = sizeof(how);
    how.bInheritHandle = TRUE;

    const auto undone = [&] {
        closed(in_read);
        closed(in_write);
        closed(out_read);
        closed(out_write);
        closed(err_read);
        closed(err_write);
    };

    if (!ahead) {
        // **Rura wejścia powstaje inaczej niż pozostałe dwie**, i to jest cena za działające
        // `close_input()`. `CreatePipe` oddaje wyłącznie uchwyty synchroniczne, a zapis
        // synchroniczny, który stanął na pełnej rurze, da się obudzić tylko przez
        // `CancelSynchronousIo` z uchwytem cudzego wątku — a to zostawia okno, którego nie sposób
        // domknąć: między sprawdzeniem znacznika a wejściem w `WriteFile` przerwanie trafia w
        // próżnię.
        //
        // Nasz koniec robimy więc **nakładany**, a wtedy pisarz czeka na zakończenie zapisu
        // i na zdarzenie budzące **naraz** — dokładnie tak, jak na POSIX-ie `poll` czeka na
        // deskryptor i na rurę budzącą. Dziecko dostaje swój koniec zwykły i dziedziczny,
        // bo tego właśnie oczekuje każdy program czytający z wejścia.
        //
        // Nazwa musi być niepowtarzalna w całym systemie, więc bierze numer procesu i licznik.
        {
            static std::atomic<unsigned> counted{0};

            wchar_t named[64];

            // `swprintf`, a nie `_snwprintf_s`: to drugie jest wyłącznie MSVC, a chcemy, żeby
            // plik zbudował się także pod MinGW.
            (void)swprintf(named, sizeof(named) / sizeof(named[0]), L"\\\\.\\pipe\\talcite-%lu-%u",
                           static_cast<unsigned long>(GetCurrentProcessId()), counted.fetch_add(1));

            in_write = CreateNamedPipeW(named, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, &how);

            if (in_write == INVALID_HANDLE_VALUE) {
                in_write = nullptr;

                undone();
                amiss("Cannot create the input pipe.", what.program);
            }

            in_read = CreateFileW(named, GENERIC_READ, 0, &how, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);

            if (in_read == INVALID_HANDLE_VALUE) {
                in_read = nullptr;

                undone();
                amiss("Cannot create the input pipe.", what.program);
            }
        }

        if (CreatePipe(&out_read, &out_write, &how, 0) == 0) {
            undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (!what.merge && CreatePipe(&err_read, &err_write, &how, 0) == 0) {
            undone();
            amiss("Cannot create the error pipe.", what.program);
        }

        // Nasze końce **nie** mają być dziedziczone: dziecko trzymające koniec do zapisu na
        // własnym wyjściu nigdy nie doczekałoby się końca rury.
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

        // Zdarzenie budzące dla `close_input()`. Gdyby się nie udało, zostaje samo odroczone
        // zamknięcie: pisarz dowie się o zapieczętowaniu przy następnej racie zamiast natychmiast.
        // To jest pogorszenie, a nie awaria, więc dziecka z tego powodu nie odrzucamy.
        held->rouse = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (err_read != nullptr) SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    }

    Launch launch;

    try {
        launch = commanded(what);
    } catch (...) {
        undone();
        throw;
    }

    std::wstring around = surrounded(what);
    const std::wstring where = what.where.empty() ? std::wstring{} : what.where.wstring();

    STARTUPINFOEXW setting{};

    // Rozmiar musi odpowiadać temu, co naprawdę podajemy: rozszerzona postać tylko wtedy, gdy
    // dokładamy listę uchwytów. Podana bez tej flagi byłaby kłamstwem o wielkości struktury.
    setting.StartupInfo.cb =
        ahead ? static_cast<DWORD>(sizeof(STARTUPINFOW)) : static_cast<DWORD>(sizeof(setting));

    std::vector<char> room;
    HANDLE passed[3] = {nullptr, nullptr, nullptr};

    if (!ahead) {
        setting.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        setting.StartupInfo.hStdInput = in_read;
        setting.StartupInfo.hStdOutput = out_write;
        setting.StartupInfo.hStdError = what.merge ? out_write : err_write;

        // Dziedziczenie uchwytów jest na Windowsie **wszystko albo nic**, chyba że poda się listę.
        // Bez niej dziecko dostałoby także nasze gniazda — i trzymało zajęty port długo po tym,
        // jak my go zamkniemy.
        passed[0] = in_read;
        passed[1] = out_write;
        passed[2] = what.merge ? out_write : err_write;

        SIZE_T much = 0;

        InitializeProcThreadAttributeList(nullptr, 1, 0, &much);

        room.resize(much);

        setting.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(room.data());

        if (InitializeProcThreadAttributeList(setting.lpAttributeList, 1, 0, &much) == 0) {
            undone();
            amiss("Cannot prepare the handle list.", what.program);
        }

        if (UpdateProcThreadAttribute(setting.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      passed, sizeof(HANDLE) * (what.merge ? 2 : 3), nullptr,
                                      nullptr) == 0) {
            DeleteProcThreadAttributeList(setting.lpAttributeList);
            undone();
            amiss("Cannot pass the handle list.", what.program);
        }
    }

    PROCESS_INFORMATION told{};

    // Własna grupa procesów dla programu na rurach — dzięki niej `stop()` ma dokąd wysłać
    // Ctrl-Break. Program, który dostaje nasz terminal, zostaje w **naszej** grupie, bo inaczej
    // nie dosięgnąłby go Ctrl-C naciśnięty przez użytkownika.
    //
    // Wstrzymany na start, żeby wszedł do zadania, zanim cokolwiek uruchomi sam.
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                        (ahead ? 0u : (EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP));

    const BOOL done =
        CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr, ahead ? FALSE : TRUE,
                       flags, around.data(), where.empty() ? nullptr : where.c_str(), &setting.StartupInfo, &told);

    if (!ahead) DeleteProcThreadAttributeList(setting.lpAttributeList);

    if (done == 0) {
        undone();
        amiss("Cannot start the program.", what.program);
    }

    // Zadanie, które się nie uda — proces już w cudzym zadaniu bez prawa wyjścia, stary system — nie
    // jest powodem do odmowy: `kill()` zabije wtedy sam program, jak dawniej.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);

    if (job != nullptr && AssignProcessToJobObject(job, told.hProcess) == 0) {
        CloseHandle(job);
        job = nullptr;
    }

    ResumeThread(told.hThread);
    CloseHandle(told.hThread);

    closed(in_read);
    closed(out_write);
    closed(err_write);

    held->into = in_write;
    held->from = out_read;
    held->wrong = err_read;

    held->grave->which = static_cast<std::int64_t>(told.dwProcessId);
    held->grave->process = told.hProcess;
    held->grave->job = job;
    held->grave->grouped = !ahead;

#else

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};

    const auto undone = [&] {
        for (int* one : {&in_pipe[0], &in_pipe[1], &out_pipe[0], &out_pipe[1], &err_pipe[0],
                         &err_pipe[1]})
            closed(*one);
    };

    // Wszystkie trzy rury zamykane przy uruchomieniu **od urodzenia** (`sealed::piped`): drugi
    // program startujący w tej samej chwili z innego wątku nie dostanie końców tego. Dziecko
    // przekłada swoje końce na 0, 1 i 2, a `dup2` znacznika nie przenosi.
    if (!ahead) {
        if (!sealed::piped(in_pipe, false)) amiss("Cannot create the input pipe.", what.program);

        if (!sealed::piped(out_pipe, false)) {
            undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (!what.merge && !sealed::piped(err_pipe, false)) {
            undone();
            amiss("Cannot create the error pipe.", what.program);
        }
    }

    // Wszystko, czego dziecko będzie potrzebowało, składamy **tutaj**. Po rozwidleniu wolno mu
    // wołać tylko to, co jest bezpieczne w obsłudze sygnału, więc żadnego przydzielania pamięci
    // ani składania napisów już tam nie ma.
    const std::string program = found(what.program);
    const std::string where = what.where.empty() ? std::string{} : what.where.string();

    std::vector<std::string> among = surrounded(what);
    std::vector<std::string> words;

    words.push_back(what.program.string());

    for (const std::string& one : what.arguments) words.push_back(one);

    std::vector<char*> argv;
    std::vector<char*> envp;

    for (std::string& one : words) argv.push_back(one.data());

    argv.push_back(nullptr);

    for (std::string& one : among) envp.push_back(one.data());

    envp.push_back(nullptr);

    const pid_t which = ::fork();

    if (which < 0) {
        undone();
        amiss("Cannot start the program.", what.program);
    }

    if (which == 0) {
        if (ahead) {
            // Własna grupa procesów i pierwszy plan: bez tego Ctrl-C poszłoby do nas, a próba
            // czytania z terminala zatrzymałaby dziecko sygnałem SIGTTIN. Samo `tcsetpgrp` też
            // wysyła sygnał — SIGTTOU — więc najpierw go uciszamy.
            ::setpgid(0, 0);

            std::signal(SIGTTOU, SIG_IGN);

            (void)::tcsetpgrp(STDIN_FILENO, ::getpgrp());
        } else {
            // Też własna grupa, ale bez pierwszego planu: `stop()` i `kill()` sięgają wtedy całej
            // grupy, więc z powłoką giną programy, które ona uruchomiła.
            ::setpgid(0, 0);

            (void)::dup2(in_pipe[0], STDIN_FILENO);
            (void)::dup2(out_pipe[1], STDOUT_FILENO);
            (void)::dup2(what.merge ? out_pipe[1] : err_pipe[1], STDERR_FILENO);

            for (const int one : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0],
                                  err_pipe[1]})
                if (one > STDERR_FILENO) ::close(one);

            // Koniec rury, który już był jednym z 0, 1, 2 — rodzic bez wejścia standardowego dostaje
            // od `pipe2` właśnie zero — `dup2` zostawił jak był, ze znacznikiem, a ten zamknąłby go
            // przy `exec`.
            for (const int one : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) ::fcntl(one, F_SETFD, 0);
        }

        // Sygnały ustawione przez nas na ignorowane zostają takie po `exec` — a program, który
        // dostał w spadku zignorowane SIGPIPE, zachowa się inaczej niż uruchomiony z powłoki.
        std::signal(SIGPIPE, SIG_DFL);

        if (!where.empty() && ::chdir(where.c_str()) != 0) ::_exit(127);

        ::execve(program.c_str(), argv.data(), envp.data());

        ::_exit(127);
    }

    // To samo robi dziecko — i robią to oba celowo. Kto zdąży pierwszy, ten ustawia; drugie
    // wywołanie nic nie zmienia. Inaczej zostaje szpara, w której dziecko już czyta, a pierwszego
    // planu jeszcze nie ma, albo `kill()` trafia w grupę, której jeszcze nie ma.
    ::setpgid(which, which);

    if (ahead) {
        void (*was)(int) = std::signal(SIGTTOU, SIG_IGN);

        (void)::tcsetpgrp(STDIN_FILENO, which);

        std::signal(SIGTTOU, was == nullptr ? SIG_DFL : was);
    }

    closed(in_pipe[0]);
    closed(out_pipe[1]);
    closed(err_pipe[1]);

    held->into = in_pipe[1];
    held->from = out_pipe[0];
    held->wrong = err_pipe[0];

    // **Nasze końce rur są nieblokujące.** Pętle czytania i pisania są napisane pod to od początku
    // — pytają `poll`, obsługują `EAGAIN` i mają obok siebie dzwonek — ale bez tego ustawienia
    // `EAGAIN` nigdy nie przychodzi, bo zapis blokujący do rury wraca dopiero po oddaniu
    // **wszystkiego**, o co poproszono. Wtedy robotnik puli stoi w środku wywołania systemowego,
    // gdzie ani dzwonek, ani `close_input()` nie mają jak go dosięgnąć: zmierzone `close_input()`
    // na dziecku, które nie czyta, kończyło zapis dopiero ze śmiercią dziecka.
    //
    // Ustawienie dotyczy **wyłącznie naszych końców**. Rura ma dwa osobne opisy otwarcia — my
    // trzymamy wejście do zapisu i dwa wyjścia do odczytu, dziecko trzyma te drugie — więc
    // dziecko dostaje swoje końce blokujące, tak jak każdy program się tego spodziewa.
    for (const int one : {held->into, held->from, held->wrong}) {
        if (one < 0) continue;

        const int flags = ::fcntl(one, F_GETFL, 0);

        if (flags >= 0) (void)::fcntl(one, F_SETFL, flags | O_NONBLOCK);
    }

    // Rura budząca dla `close_input()`. Gdyby się nie udała, zostaje samo odroczone zamknięcie:
    // pisarz dowie się o zapieczętowaniu przy następnym obrocie pętli zamiast natychmiast. To jest
    // pogorszenie, a nie awaria, więc dziecka z tego powodu nie odrzucamy.
    if (!sealed::piped(held->rouse, true)) held->rouse[0] = held->rouse[1] = -1;

    held->grave->which = static_cast<std::int64_t>(which);
    held->grave->foreground = ahead;

#endif

    const std::shared_ptr<Grave> grave = held->grave;
    const fs::path program_named = what.program;

    held->waiting = Promise<Exit>{[grave, program_named](auto resolve, Reject reject) {
        try {
            resolve(buried(grave, program_named));
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    // Ruszamy **od razu**, nie przy pierwszym pytaniu o koniec. Obietnice w talcite są leniwe, więc
    // dziecko, o którego koniec nikt nigdy nie zapyta, nie zostałoby pochowane i zostałoby po nim
    // widmo w tablicy procesów. A prawo puszczenia uchwytu bez pytania o cokolwiek jest właśnie
    // tym, co obiecuje `close()`.
    held->waiting->start();

    return held;
}

// **Program puszczony wolno** (S1: demon udostępniania w twa). Nie jest naszym dzieckiem: nie ma
// rur, nie dziedziczy niczego poza środowiskiem, nie ginie z nami i nikt go nie chowa — zostaje,
// gdy program, który go uruchomił, się skończy, także wtedy, gdy zamknięto terminal. Oddajemy sam
// numer procesu.
//
// POSIX: dwa rozwidlenia z `setsid` pośrodku — wnuk nie ma terminala sterującego i nie jest liderem
// sesji, więc żadnego nie zdobędzie, a jego rodzicem zostaje system, nie my. Wejście i wyjścia idą
// do /dev/null. Środkowy proces mówi numer wnuka, a wnuk — błąd `exec`, gdy się nie udał; rura ma
// znacznik zamknięcia przy uruchomieniu, więc udane `exec` zamyka ją bez słowa. Każda wiadomość to
// znak i osiem bajtów, jednym zapisem — krócej niż `PIPE_BUF`, więc niepodzielna, w jakiejkolwiek
// kolejności przyjdą obie.
//
// Windows: proces bez konsoli, we własnej grupie, wyjęty z naszego zadania, gdy zadanie na to
// pozwala (inaczej zginąłby razem z nim), bez dziedziczenia uchwytów.
std::int64_t loosed(const Command& what) {
    if (what.program.empty()) amiss("No program given.", what.program);

#ifdef _WIN32

    Launch launch = commanded(what);

    std::wstring around = surrounded(what);
    const std::wstring where = what.where.empty() ? std::wstring{} : what.where.wstring();

    STARTUPINFOW setting{};

    setting.cb = sizeof(setting);

    PROCESS_INFORMATION told{};

    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;

    BOOL done = CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr, FALSE,
                               flags | CREATE_BREAKAWAY_FROM_JOB, around.data(),
                               where.empty() ? nullptr : where.c_str(), &setting, &told);

    // Zadanie, które wyjścia nie pozwala, odmawia całego startu — wtedy program zostaje w nim;
    // to lepsze niż nie wystartować wcale.
    if (done == 0 && GetLastError() == ERROR_ACCESS_DENIED)
        done = CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr, FALSE, flags,
                              around.data(), where.empty() ? nullptr : where.c_str(), &setting, &told);

    if (done == 0) amiss("Cannot start the program.", what.program);

    CloseHandle(told.hThread);
    CloseHandle(told.hProcess);

    return static_cast<std::int64_t>(told.dwProcessId);

#else

    const std::string program = found(what.program);
    const std::string where = what.where.empty() ? std::string{} : what.where.string();

    std::vector<std::string> among = surrounded(what);
    std::vector<std::string> words;

    words.push_back(what.program.string());

    for (const std::string& one : what.arguments) words.push_back(one);

    std::vector<char*> argv;
    std::vector<char*> envp;

    for (std::string& one : words) argv.push_back(one.data());

    argv.push_back(nullptr);

    for (std::string& one : among) envp.push_back(one.data());

    envp.push_back(nullptr);

    int told[2] = {-1, -1};

    if (!sealed::piped(told, false)) amiss("Cannot create the start pipe.", what.program);

    // Znak i osiem bajtów. Bez przydzielania pamięci — to biegnie między `fork` a `exec`.
    const auto say = [&told](char kind, std::int64_t value) {
        char one[9];

        one[0] = kind;

        std::memcpy(one + 1, &value, sizeof value);

        (void)!::write(told[1], one, sizeof one);
    };

    const pid_t middle = ::fork();

    if (middle < 0) {
        ::close(told[0]);
        ::close(told[1]);

        amiss("Cannot start the program.", what.program);
    }

    if (middle == 0) {
        ::close(told[0]);

        (void)::setsid();

        const pid_t last = ::fork();

        if (last < 0) {
            say('e', errno);

            ::_exit(127);
        }

        if (last > 0) {
            say('p', last);

            ::_exit(0);
        }

        const int nothing = ::open("/dev/null", O_RDWR);

        if (nothing >= 0) {
            for (const int one : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) (void)::dup2(nothing, one);

            if (nothing > STDERR_FILENO) ::close(nothing);
        }

        std::signal(SIGPIPE, SIG_DFL);

        if (!where.empty() && ::chdir(where.c_str()) != 0) {
            say('e', errno);

            ::_exit(127);
        }

        ::execve(program.c_str(), argv.data(), envp.data());

        say('e', errno);

        ::_exit(127);
    }

    ::close(told[1]);

    std::string heard;
    char room[64];

    for (;;) {
        const ssize_t much = ::read(told[0], room, sizeof room);

        if (much > 0) {
            heard.append(room, static_cast<std::size_t>(much));

            continue;
        }

        if (much < 0 && errno == EINTR) continue;

        break;
    }

    ::close(told[0]);

    int status = 0;

    while (::waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }

    std::int64_t pid = -1;
    std::int64_t failed = 0;

    for (std::size_t at = 0; at + 9 <= heard.size(); at += 9) {
        std::int64_t value = 0;

        std::memcpy(&value, heard.data() + at + 1, sizeof value);

        if (heard[at] == 'p') pid = value;
        else failed = value == 0 ? -1 : value;
    }

    if (failed != 0 || pid < 0)
        throw fs::Trouble{"Cannot start the program: " +
                              (failed > 0 ? std::string{std::strerror(static_cast<int>(failed))} : std::string{"no process"}) + ".",
                          what.program};

    return pid;

#endif
}

// Pętla czytająca jedną rurę do końca. Osobna obietnica na każdą stronę, bo czytać trzeba **obie
// naraz**: program, który zapełni tę, w którą akurat nie patrzymy, stanie i nie skończy się nigdy.
[[nodiscard]] Promise<void> sipping(std::shared_ptr<Child::Held> held, bool errors,
                                    std::shared_ptr<Bytes> into) {
    return Promise<void>{[held, errors, into](auto resolve, Reject reject) {
        try {
            for (;;) {
                Bytes got = sipped(*held, errors, brim);

                if (got.empty()) break;

                into->insert(into->end(), got.begin(), got.end());
            }

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

}

Promise<std::int64_t> Child::detached(Command what) {
    return Promise<std::int64_t>{[what = std::move(what)](auto resolve, Reject reject) {
        try {
            resolve(loosed(what));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

Promise<Child> Child::start(Command what) {
    return Promise<Child>{[what = std::move(what)](auto resolve, Reject reject) {
        try {
            resolve(Rearing::made(reared(what, false)));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

std::int64_t Child::pid() const { return held_ == nullptr ? -1 : held_->grave->which; }

Promise<std::size_t> Child::write(Bytes what) const {
    std::shared_ptr<Held> held = held_;

    return Promise<std::size_t>{
        [held, what = std::move(what)](auto resolve, Reject reject) mutable {
            try {
                if (held == nullptr) {
                    resolve(0);

                    return;
                }

                resolve(poured(*held, what));
            } catch (...) {
                reject(std::current_exception());
            }
        }};
}

// **Zamyka ostatni wychodzący, nie ten, kto poprosił.** Dopóki jakikolwiek zapis siedzi
// w wywołaniu systemowym na tym deskryptorze, zamknięcie oddałoby jego numer systemowi, a ten
// rozdaje numery natychmiast — więc treść przeznaczona dla dziecka mogłaby trafić w cudzy plik.
// Zamiast tego stawiamy znacznik i budzimy czekających; zamknie ten, kto wyjdzie jako ostatni.
void Child::close_input() const {
    if (held_ == nullptr) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    if (held_->sealing) return;

    held_->sealing = true;

    if (held_->writers == 0) {
        closed(held_->into);

        return;
    }

#ifdef _WIN32

    // Zdarzenie ręczne: raz podniesione zostaje podniesione, więc budzi każdego piszącego, a nie
    // tylko pierwszego — tak samo jak bajt w rurze budzącej na POSIX-ie.
    if (held_->rouse != nullptr) (void)SetEvent(held_->rouse);

#else

    // Bajt zostaje w rurze na zawsze i nikt go nie zbiera — rura jest poziomowa, więc budzi
    // każdego, kto na nią spojrzy, a nie tylko pierwszego.
    if (held_->rouse[1] >= 0) {
        const char one = 0;

        // Wynik nas nie obchodzi: rura jest pusta i jednobajtowy zapis nie ma jak nie przejść,
        // a gdyby jednak nie przeszedł, zostaje odroczone zamknięcie i następny obrót pętli.
        if (::write(held_->rouse[1], &one, 1) < 0) return;
    }

#endif
}

Promise<Bytes> Child::read(std::size_t most) const {
    std::shared_ptr<Held> held = held_;

    return Promise<Bytes>{[held, most](auto resolve, Reject reject) {
        try {
            if (held == nullptr) {
                resolve(Bytes{});

                return;
            }

            resolve(sipped(*held, false, most));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

Promise<Bytes> Child::read_errors(std::size_t most) const {
    std::shared_ptr<Held> held = held_;

    return Promise<Bytes>{[held, most](auto resolve, Reject reject) {
        try {
            if (held == nullptr) {
                resolve(Bytes{});

                return;
            }

            resolve(sipped(*held, true, most));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

Promise<Exit> Child::wait() const {
    if (held_ == nullptr || !held_->waiting.has_value())
        return Promise<Exit>{[](auto resolve, Reject) { resolve(Exit{Ended::lost, 0, 0}); }};

    return *held_->waiting;
}

void Child::stop() const {
    if (held_ == nullptr) return;

    const std::shared_ptr<Grave> grave = held_->grave;

    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

#ifdef _WIN32

    // Zdarzenie konsoli idzie do **całej grupy**, więc wolno je wysłać tylko programowi, któremu
    // grupę założyliśmy. Ten, który dostał nasz terminal, siedzi w naszej — Ctrl-Break wysłany pod
    // jego numer zabiłby razem z nim nas. Jemu zostaje `kill()`.
    if (grave->grouped)
        (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(grave->which));

#else

    // Cała grupa programu: powłoka i to, co uruchomiła.
    (void)::kill(-static_cast<pid_t>(grave->which), SIGTERM);

#endif
}

void Child::kill() const {
    if (held_ == nullptr) return;

    const std::shared_ptr<Grave> grave = held_->grave;

    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

#ifdef _WIN32

    if (grave->job != nullptr) (void)TerminateJobObject(grave->job, 1);
    else if (grave->process != nullptr) (void)TerminateProcess(grave->process, 1);

#else

    (void)::kill(-static_cast<pid_t>(grave->which), SIGKILL);

#endif
}

// Puszczenie uchwytu **nie zabija** programu. Zabijanie w destruktorze byłoby zaskoczeniem — kto
// chce zabić, ma na to `stop()` i `kill()`, a kto tylko przestał się interesować, zostawia go
// swojemu losowi. Robotnik czekający na koniec dokończy i pochowa.
void Child::close() { held_.reset(); }

Promise<Output> Child::run(Command what, Bytes input) {
    return Promise<Output>{[what = std::move(what), input = std::move(input)](auto resolve,
                                                                       Reject reject) mutable {
        try {
            const Child one = Child::start(what).await();

            const std::shared_ptr<Child::Held>& held = Rearing::inside(one);

            auto read = std::make_shared<Bytes>();
            auto errors = std::make_shared<Bytes>();

            // Czytanie ruszamy **przed** pisaniem. Odwrotna kolejność zakleszcza się na wejściu
            // większym niż rura: my czekamy, aż dziecko odbierze, dziecko czeka, aż my odbierzemy
            // to, co zdążyło wypisać.
            Promise<void> reading = sipping(held, false, read);
            Promise<void> moaning =
                what.merge ? Promise<void>{[](auto done, Reject) { done(); }}
                           : sipping(held, true, errors);

            Promise<void> both = Promise<void>::all(reading, moaning);

            both.start();

            if (!input.empty()) (void)one.write(std::move(input)).await();

            one.close_input();

            both.await();

            Output made;

            made.exit = one.wait().await();
            made.out = std::move(*read);
            made.err = std::move(*errors);

            resolve(std::move(made));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

#ifdef _WIN32

// Cały wiersz polecenia z pełną ścieżką programu na czele — dla miejsc, które biorą jeden napis
// i same go uruchamiają później, jak klucz `Run` (`login.cpp`). Reguły cytowania te same, co przy
// uruchamianiu: plik wsadowy przez cmd.exe z katalogu systemowego, każdy argument w cudzysłowie.
std::wstring launched(const Command& what) {
    const Launch made = commanded(what);

    std::wstring line;

    quoted(made.application, line);

    if (batched(sought(what.program))) {
        line += made.line.substr(std::wstring_view{L"cmd.exe"}.size());

        return line;
    }

    for (const std::string& one : what.arguments) {
        line += L' ';

        quoted(to_wstring(one), line);
    }

    return line;
}

#endif

Exit Term::execute(Command what) {
    const Pause step{*this};

    const Child one = Rearing::made(reared(what, true));

    const Exit made = one.wait().await();

#ifndef _WIN32

    // Terminal wraca do nas. Bez tego zostałby przy grupie, której już nie ma, a nasze pierwsze
    // czytanie skończyłoby się zatrzymaniem na SIGTTIN.
    void (*was)(int) = std::signal(SIGTTOU, SIG_IGN);

    (void)::tcsetpgrp(STDIN_FILENO, ::getpgrp());

    std::signal(SIGTTOU, was == nullptr ? SIG_DFL : was);

#endif

    return made;
}

}

#include "talcite/fs.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#include "talcite/bell.hpp"
#include "talcite/pool.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#if defined(__linux__)

#include <poll.h>
#include <sys/inotify.h>

#endif

#endif

namespace talcite::fs {

namespace {

namespace stood = std::filesystem;

constexpr std::size_t on_disk = 1024 * 1024;
constexpr std::size_t afar = 64 * 1024;
constexpr std::size_t astray = 256 * 1024;

[[noreturn]] void amiss(const std::string& said, const path& where) { throw Trouble{said, where}; }

#ifdef _WIN32

std::wstring longly(const path& where);

#endif

// **Wpis, za którym system nie umie podążyć, istnieje mimo to.** `std::filesystem::exists`
// podąża za punktem ponownej analizy, a gdy trafi na znacznik, którego nie zna, melduje „nie ma".
// Tak wygląda **plik gniazda uniksowego na Windows**: leży na miejscu, `GetFileAttributesW` go
// widzi, `DeleteFile` go kasuje, a `exists` odpowiada fałszem z błędem 1920.
//
// Zmierzone, nie zgadnięte: osobny program próbny na Windows pokazał dla tego samego pliku
// atrybuty widoczne, znacznik ponownej analizy ustawiony, `exists` fałszywe i numer błędu 1920.
//
// Pytamy więc o **sam wpis, bez podążania za czymkolwiek** — ale dopiero wtedy, gdy pierwsze
// pytanie zgłosiło **błąd**. Zwykły brak pliku błędem nie jest: `status` oddaje wtedy „nie ma"
// i czyści numer, więc ta gałąź nawet nie rusza i nic nie kosztuje.
//
// Wiszące dowiązanie i wiszące złącze dalej odpowiadają „nie ma", bo za nimi `GetFileAttributesW`
// podąża i też się przewraca — a to jest ta odpowiedź, której się po `exists` spodziewamy.
bool exists_at(const path& where) {
    std::error_code why;

    if (stood::exists(where, why)) return true;

#ifdef _WIN32
    if (why) return GetFileAttributesW(longly(where).c_str()) != INVALID_FILE_ATTRIBUTES;
#endif

    return false;
}

Kind kind_at(const path& where) {
    std::error_code trouble;

    const stood::file_status told = stood::symlink_status(where, trouble);
    if (trouble) return Kind::none;

    if (stood::is_symlink(told)) return Kind::link;
    if (stood::is_regular_file(told)) return Kind::file;
    if (stood::is_directory(told)) return Kind::folder;
    if (!stood::exists(told)) return Kind::none;

    // Urządzenie znakowe albo blokowe to `device`; potok i gniazdo to `special` — tak samo
    // rozróżnia je Midnight Commander i tak samo je maluje.
    if (stood::is_character_file(told) || stood::is_block_file(told)) return Kind::device;

    return Kind::special;
}

std::uintmax_t size_at(const path& where) {
    std::error_code trouble;

    const std::uintmax_t told = stood::file_size(where, trouble);
    if (trouble) amiss("Nie można odczytać rozmiaru pliku.", where);

    return told;
}

// Na Windows atrybuty są jednym słowem bitowym. Na POSIX ukrycie jest umową o nazwie —
// kropka na początku — a tylko-do-odczytu odczytujemy z prawa zapisu właściciela. Znacznika
// systemowego POSIX nie ma i udawać go nie będziemy.
Marks marks_at(const path& where) {
    Marks made;

#ifdef _WIN32

    const DWORD told = GetFileAttributesW(longly(where).c_str());
    if (told == INVALID_FILE_ATTRIBUTES) amiss("Nie można odczytać atrybutów.", where);

    made.hidden = (told & FILE_ATTRIBUTE_HIDDEN) != 0;
    made.readonly = (told & FILE_ATTRIBUTE_READONLY) != 0;
    made.system = (told & FILE_ATTRIBUTE_SYSTEM) != 0;

#else

    std::error_code trouble;

    const stood::file_status told = stood::status(where, trouble);
    if (trouble) amiss("Nie można odczytać atrybutów.", where);

    const std::string named = where.filename().string();

    made.hidden = named.size() > 1 && named.front() == '.';
    made.readonly = (told.permissions() & stood::perms::owner_write) == stood::perms::none;

#endif

    return made;
}

void marked_at(const path& where, Marks what) {
#ifdef _WIN32

    const std::wstring named = longly(where);

    DWORD told = GetFileAttributesW(named.c_str());
    if (told == INVALID_FILE_ATTRIBUTES) amiss("Nie można odczytać atrybutów.", where);

    const auto sway = [&told](DWORD flag, bool on) {
        if (on) told |= flag;
        else told &= ~flag;
    };

    sway(FILE_ATTRIBUTE_HIDDEN, what.hidden);
    sway(FILE_ATTRIBUTE_READONLY, what.readonly);
    sway(FILE_ATTRIBUTE_SYSTEM, what.system);

    if (told == 0) told = FILE_ATTRIBUTE_NORMAL;

    if (SetFileAttributesW(named.c_str(), told) == 0)
        amiss("Nie można ustawić atrybutów.", where);

#else

    std::error_code trouble;

    const stood::file_status told = stood::status(where, trouble);
    if (trouble) amiss("Nie można odczytać atrybutów.", where);

    constexpr stood::perms writing = stood::perms::owner_write | stood::perms::group_write |
                                     stood::perms::others_write;

    stood::perms rights = told.permissions();

    if (what.readonly) rights &= ~writing;
    else rights |= stood::perms::owner_write;

    stood::permissions(where, rights, trouble);
    if (trouble) amiss("Nie można ustawić atrybutów.", where);

#endif
}

bool alike_at(const path& one, const path& two) {
    std::error_code trouble;

    const bool told = stood::equivalent(one, two, trouble);
    if (trouble) amiss("Nie można porównać ścieżek.", one);

    return told;
}

path target_at(const path& where) {
    std::error_code trouble;

    const path told = stood::read_symlink(where, trouble);
    if (trouble) amiss("To nie jest dowiązanie.", where);

    return told;
}

void linking_at(const path& where, const path& onto, bool hard) {
    std::error_code trouble;

    if (hard) stood::create_hard_link(onto, where, trouble);
    else if (kind_at(onto) == Kind::folder) stood::create_directory_symlink(onto, where, trouble);
    else stood::create_symlink(onto, where, trouble);

    if (trouble) amiss("Nie można utworzyć dowiązania.", where);
}

// Odstęp między zegarem plikowym a kalendarzowym bierzemy raz na cały bieg programu. Oba
// idą za czasem rzeczywistym, więc odstęp jest stały — a liczony za każdym razem od nowa
// migotałby o ułamek sekundy, bo oba odczyty nigdy nie wypadną w tej samej chwili. Po
// ścięciu do sekund to migotanie robiło z ustawionego czasu czas o sekundę wcześniejszy.
using Apart = std::chrono::system_clock::duration;

Apart apart_now() {
    static const Apart apart = std::chrono::system_clock::now().time_since_epoch() -
                               std::chrono::duration_cast<Apart>(
                                   stood::file_time_type::clock::now().time_since_epoch());

    return apart;
}

std::chrono::sys_seconds stamped(stood::file_time_type when, Apart apart) {
    return std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::time_point{
            std::chrono::duration_cast<Apart>(when.time_since_epoch()) + apart});
}

// Przeliczenie bez clock_cast, bo zegar plikowy nie na każdej bibliotece standardowej daje
// się nim przerzucić.
std::chrono::sys_seconds written_at(const path& where) {
    std::error_code trouble;

    const stood::file_time_type told = stood::last_write_time(where, trouble);
    if (trouble) amiss("Nie można odczytać czasu zapisu.", where);

    return stamped(told, apart_now());
}

Room roomy_at(const path& where) {
    std::error_code trouble;

    const stood::space_info told = stood::space(where, trouble);
    if (trouble) amiss("Nie można sprawdzić miejsca na nośniku.", where);

    return Room{told.capacity, told.available};
}

// Rata zależy od nośnika, bo anulowanie działa między ratami, a nie w ich środku. Na udziale
// sieciowym krótsza rata to krótsze zawieszenie, gdy udział przestanie odpowiadać; na dysku
// stałym nie ma czego ratować i liczy się przepustowość.
[[maybe_unused]] bool afar_at(const path& where);

// Ścieżka zaczynająca się od dwóch ukośników jest sieciowa z definicji — pytanie systemu
// o rodzaj korzenia UNC bywa zawodne, a od tej odpowiedzi zależą dwie rzeczy naraz: rata
// i to, czy wolno ufać powiadomieniom. Sprawdzian z definicji jest pewniejszy.
[[maybe_unused]] [[nodiscard]] bool unc_at(const path& where) {
    const auto named = where.native();

    if (named.size() < 2) return false;

    using Letter = path::value_type;

    const auto slashed = [](Letter one) {
        return one == static_cast<Letter>('\\') || one == static_cast<Letter>('/');
    };

    return slashed(named[0]) && slashed(named[1]);
}

#ifdef _WIN32

// Windows łamie się na ścieżkach dłuższych niż 260 znaków, chyba że poda się je
// z przedrostkiem \\?\. Przedrostek wyłącza za to wszelkie prostowanie — ścieżka musi być
// już bezwzględna i bez kropek — a udział sieciowy ma własną postać: \\?\UNC\serwer\udział.
// std::filesystem robi to u siebie samo; nasze własne wywołania systemowe muszą to robić
// jawnie, a jest ich sporo: otwarcie pliku, atrybuty, przejście katalogu i obserwacja.
std::wstring longly(const path& where) {
    std::error_code ignored;

    const path whole =
        (where.is_absolute() ? where : stood::absolute(where, ignored)).lexically_normal();

    std::wstring made = whole.wstring();

    if (made.rfind(L"\\\\?\\", 0) == 0 || made.rfind(L"\\\\.\\", 0) == 0) return made;

    for (wchar_t& one : made)
        if (one == L'/') one = L'\\';

    // Korzeń bez ukośnika na końcu Windows odczytuje jako „katalog bieżący na tym dysku".
    if (made.size() == 2 && made[1] == L':') made += L'\\';

    if (made.size() > 1 && made[0] == L'\\' && made[1] == L'\\')
        return L"\\\\?\\UNC\\" + made.substr(2);

    return L"\\\\?\\" + made;
}

#endif

Mount mount_at(const path& where) {
#ifdef _WIN32

    if (unc_at(where)) return Mount::remote;

    std::error_code ignored;

    path root = stood::absolute(where, ignored).root_path();
    if (root.empty()) return Mount::other;

    std::wstring named = root.wstring();
    if (named.back() != L'\\') named += L'\\';

    switch (GetDriveTypeW(named.c_str())) {
        case DRIVE_REMOTE: return Mount::remote;
        case DRIVE_REMOVABLE: return Mount::removable;
        case DRIVE_CDROM: return Mount::optical;
        case DRIVE_FIXED: return Mount::disk;
        default: return Mount::other;
    }

#else

    struct statfs told {};

    const path asked = exists_at(where) ? where : where.parent_path();

    if (statfs(asked.c_str(), &told) != 0) return Mount::other;

    switch (static_cast<unsigned long>(told.f_type)) {
        case 0x6969ul:
        case 0x517Bul:
        case 0xFF534D42ul:
        case 0xFE534D42ul:
        case 0x65735546ul: return Mount::remote;
        case 0x9660ul:
        case 0x15013346ul: return Mount::optical;
        default: return Mount::disk;
    }

#endif
}

std::size_t slice_at(const path& where) {
    switch (mount_at(where)) {
        case Mount::remote: return afar;
        case Mount::removable:
        case Mount::optical: return astray;
        default: return on_disk;
    }
}

// Jedyne miejsce, w którym praca pyta, czy jest jeszcze potrzebna. Dzwonek robotnika
// podnosi drop() z puli, czyli cancel() na obietnicy — a przechodzenie drzewa jest pierwszą
// operacją w talcite, która trwa na tyle długo, żeby miało to znaczenie.
bool stopped() {
    Bell* bell = ringing();

    return bell != nullptr && bell->wait(std::chrono::milliseconds{0});
}

#ifndef _WIN32

Entry noted(const stood::directory_entry& one, Apart apart) {
    std::error_code ignored;

    Entry made;
    made.where = one.path();

    if (one.is_symlink(ignored)) made.kind = Kind::link;
    else if (one.is_regular_file(ignored)) made.kind = Kind::file;
    else if (one.is_directory(ignored)) made.kind = Kind::folder;
    else if (one.is_character_file(ignored) || one.is_block_file(ignored))
        made.kind = Kind::device;
    else made.kind = Kind::special;

    if (made.kind == Kind::file) made.size = one.file_size(ignored);

    const std::string named = one.path().filename().string();

    // Wszystko z jednego `status()`: prawo zapisu właściciela, bit wykonywania (którykolwiek,
    // tak liczy MC) i to, czy dowiązanie ma dokąd prowadzić — `exists()` patrzy przez dowiązanie.
    // Żadnego dodatkowego pytania do systemu na wpis.
    const stood::perms rights = one.status(ignored).permissions();

    made.marks.hidden = named.size() > 1 && named.front() == '.';
    made.marks.readonly = (rights & stood::perms::owner_write) == stood::perms::none;
    made.marks.executable =
        (rights & (stood::perms::owner_exec | stood::perms::group_exec |
                   stood::perms::others_exec)) != stood::perms::none;
    made.marks.stale = made.kind == Kind::link && !one.exists(ignored);

    const stood::file_time_type when = one.last_write_time(ignored);
    if (!ignored) made.written = stamped(when, apart);

    return made;
}

#endif

void sorted(std::vector<Entry>& among) {
    std::sort(among.begin(), among.end(),
              [](const Entry& one, const Entry& two) { return one.where < two.where; });
}

#ifdef _WIN32

// Czas plikowy Windows liczy się w setkach nanosekund od roku 1601. Do epoki uniksowej jest
// 11 644 473 600 sekund. Przeliczamy wprost, bez okrężnej drogi przez różnicę zegarów —
// tutaj znamy dokładną wartość, a nie tylko odstęp między dwoma „teraz".
std::chrono::sys_seconds stamped(FILETIME told) {
    const std::uint64_t much =
        (static_cast<std::uint64_t>(told.dwHighDateTime) << 32) | told.dwLowDateTime;

    return std::chrono::sys_seconds{std::chrono::seconds{
        static_cast<std::int64_t>(much / 10000000ull) - 11644473600ll}};
}

// Rozszerzenia, które Windows uruchamia wprost. PATHEXT ma ich więcej, ale te cztery są tym,
// co człowiek nazywa „programem", a reszta (`.vbs`, `.js`, `.msc`) to skrypty pod cudzy silnik.
bool runnable(const path& where) {
    std::wstring ending = where.extension().wstring();

    for (wchar_t& one : ending) one = static_cast<wchar_t>(std::towlower(one));

    return ending == L".exe" || ending == L".com" || ending == L".bat" || ending == L".cmd";
}

// Jedno pytanie do systemu oddaje nazwę, rodzaj, rozmiar, czas i atrybuty razem.
// directory_iterator nie wystawia atrybutów, więc trzeba było dopytywać o każdy wpis
// z osobna — przy skanowaniu udziału sieciowego to podwajało liczbę wypraw przez sieć.
Entry noted(const WIN32_FIND_DATAW& told, const path& here) {
    Entry made;

    made.where = here / path{told.cFileName};

    if ((told.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) made.kind = Kind::link;
    else if ((told.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) made.kind = Kind::folder;
    else made.kind = Kind::file;

    if (made.kind == Kind::file)
        made.size = (static_cast<std::uintmax_t>(told.nFileSizeHigh) << 32) | told.nFileSizeLow;

    made.written = stamped(told.ftLastWriteTime);

    made.marks.hidden = (told.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    made.marks.readonly = (told.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    made.marks.system = (told.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;

    // Wykonywalny po rozszerzeniu — Windows nie ma bitu. `stale` zostaje fałszem: sprawdzenie
    // celu punktu ponownej analizy to osobna wyprawa do systemu na każdy wpis, a przy udziale
    // sieciowym podwajałaby liczbę wypraw — powód, dla którego `noted` czyta z `FIND_DATA`.
    made.marks.executable = made.kind == Kind::file && runnable(made.where);

    return made;
}

std::vector<Entry> found_in(const path& where, bool quiet) {
    std::vector<Entry> made;

    WIN32_FIND_DATAW told{};

    const HANDLE looking =
        FindFirstFileExW((longly(where) + L"\\*").c_str(), FindExInfoBasic, &told,
                         FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);

    if (looking == INVALID_HANDLE_VALUE) {
        if (quiet) return made;

        amiss("Nie można otworzyć katalogu.", where);
    }

    do {
        const std::wstring named{told.cFileName};

        if (named == L"." || named == L"..") continue;

        if (stopped()) break;

        made.push_back(noted(told, where));
    } while (FindNextFileW(looking, &told) != 0);

    FindClose(looking);

    return made;
}

#endif

std::vector<Entry> list_at(const path& where) {
    if (kind_at(where) != Kind::folder) amiss("To nie jest katalog.", where);

#ifdef _WIN32

    std::vector<Entry> made = found_in(where, false);

#else

    std::error_code trouble;
    stood::directory_iterator at{where, stood::directory_options::skip_permission_denied,
                                 trouble};

    if (trouble) amiss("Nie można otworzyć katalogu.", where);

    const Apart apart = apart_now();

    std::vector<Entry> made;

    for (const stood::directory_entry& one : at) {
        if (stopped()) return made;

        made.push_back(noted(one, apart));
    }

#endif

    sorted(made);

    return made;
}

// Katalog, do którego nie ma wstępu, pomijamy zamiast przewracać całe przejście. Zabranie
// prawa do jednego podkatalogu nie może unieważnić skanowania biblioteki.
std::vector<Entry> walk_at(const path& where) {
    if (kind_at(where) != Kind::folder) amiss("To nie jest katalog.", where);

#ifndef _WIN32
    const Apart apart = apart_now();
#endif

    std::vector<Entry> made;
    std::vector<path> waiting{where};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

#ifdef _WIN32

        for (Entry& one : found_in(here, true)) {
            if (one.kind == Kind::folder) waiting.push_back(one.where);

            made.push_back(std::move(one));
        }

        if (stopped()) return made;

#else

        std::error_code trouble;
        stood::directory_iterator at{here, stood::directory_options::skip_permission_denied,
                                     trouble};

        if (trouble) continue;

        for (const stood::directory_entry& one : at) {
            if (stopped()) return made;

            Entry note = noted(one, apart);
            if (note.kind == Kind::folder) waiting.push_back(note.where);

            made.push_back(std::move(note));
        }

#endif
    }

    sorted(made);

    return made;
}

void make_at(const path& where) {
    if (kind_at(where) == Kind::folder) return;

    std::error_code trouble;

    stood::create_directories(where, trouble);
    if (trouble) amiss("Nie można utworzyć katalogu.", where);
}

void rename_at(const path& from, const path& to) {
    std::error_code trouble;

    stood::rename(from, to, trouble);
    if (trouble) amiss("Nie można zmienić nazwy.", from);
}

}

// Uchwyt jest wspólny, a nie kopiowany: File to typ wartościowy tak samo jak Promise, więc
// kopie dzielą ten sam otwarty plik, a zamyka go dopiero ostatnia. Dzięki temu File przechodzi
// przez obietnicę bez sztuczek z wskaźnikami.
struct File::Held {
    path where;

#ifdef _WIN32
    void* handle = nullptr;

    ~Held() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
#else
    int handle = -1;

    ~Held() {
        if (handle >= 0) ::close(handle);
    }
#endif

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;
};

namespace {

std::shared_ptr<File::Held> open_at(const path& where, Mode how) {
    auto made = std::make_shared<File::Held>();
    made->where = where;

#ifdef _WIN32
    const DWORD reach = how == Mode::read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE;
    const DWORD when = how == Mode::read ? OPEN_EXISTING
                                         : (how == Mode::fresh ? CREATE_ALWAYS : OPEN_ALWAYS);

    made->handle = CreateFileW(longly(where).c_str(), reach, shared, nullptr, when,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    if (made->handle == INVALID_HANDLE_VALUE) amiss("Nie można otworzyć pliku.", where);
#else
    int flags = how == Mode::read ? O_RDONLY : O_RDWR;
    if (how == Mode::write) flags |= O_CREAT;
    if (how == Mode::fresh) flags |= O_CREAT | O_TRUNC;

    // **Zamykany przy uruchomieniu programu potomnego** — inaczej dziecko dziedziczy każdy plik,
    // który akurat mamy otwarty, i trzyma go po naszym `close()`.
    made->handle = ::open(where.c_str(), flags | O_CLOEXEC, 0644);

    if (made->handle < 0) amiss("Nie można otworzyć pliku.", where);
#endif

    return made;
}

Bytes read_at(const File::Held& held, std::uintmax_t at, std::size_t much) {
    Bytes made(much);

    std::size_t got = 0;

    while (got < much) {
#ifdef _WIN32
        OVERLAPPED told{};
        told.Offset = static_cast<DWORD>((at + got) & 0xFFFFFFFFull);
        told.OffsetHigh = static_cast<DWORD>((at + got) >> 32);

        DWORD taken = 0;
        const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(much - got, 1u << 30));

        if (ReadFile(held.handle, made.data() + got, wanted, &taken, &told) == 0) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;

            amiss("Nie można odczytać pliku.", held.where);
        }
#else
        const ssize_t taken = ::pread(held.handle, made.data() + got, much - got,
                                      static_cast<off_t>(at + got));

        if (taken < 0) amiss("Nie można odczytać pliku.", held.where);
#endif

        if (taken == 0) break;

        got += static_cast<std::size_t>(taken);
    }

    made.resize(got);

    return made;
}

std::size_t write_at(const File::Held& held, std::uintmax_t at, const Bytes& what) {
    std::size_t put = 0;

    while (put < what.size()) {
#ifdef _WIN32
        OVERLAPPED told{};
        told.Offset = static_cast<DWORD>((at + put) & 0xFFFFFFFFull);
        told.OffsetHigh = static_cast<DWORD>((at + put) >> 32);

        DWORD given = 0;
        const DWORD wanted =
            static_cast<DWORD>(std::min<std::size_t>(what.size() - put, 1u << 30));

        if (WriteFile(held.handle, what.data() + put, wanted, &given, &told) == 0)
            amiss("Nie można zapisać pliku.", held.where);
#else
        const ssize_t given = ::pwrite(held.handle, what.data() + put, what.size() - put,
                                       static_cast<off_t>(at + put));

        if (given < 0) amiss("Nie można zapisać pliku.", held.where);
#endif

        // **Zerowy zapis to brak postępu**, a nie koniec pracy: wołający kręci pętlę warunkiem na
        // przesunięciu, więc oddanie zera przy niepustym kawałku zapętliłoby kopiowanie na pełnym
        // rdzeniu. Mówimy o tym wprost, zamiast milczeć.
        if (given == 0) amiss("Nie można zapisać pliku.", held.where);

        put += static_cast<std::size_t>(given);
    }

    return put;
}

std::uintmax_t sized(const File::Held& held) {
#ifdef _WIN32
    LARGE_INTEGER told{};

    if (GetFileSizeEx(held.handle, &told) == 0) amiss("Nie można odczytać rozmiaru pliku.",
                                                      held.where);

    return static_cast<std::uintmax_t>(told.QuadPart);
#else
    struct stat told {};

    if (::fstat(held.handle, &told) != 0) amiss("Nie można odczytać rozmiaru pliku.", held.where);

    return static_cast<std::uintmax_t>(told.st_size);
#endif
}

}

const path& File::where() const {
    static const path nowhere;

    return held_ ? held_->where : nowhere;
}

Promise<Bytes> File::read(std::uintmax_t at, std::size_t much) const {
    auto held = held_;

    return Promise<Bytes>{[held, at, much](auto resolve, auto) {
        if (!held) amiss("Plik nie jest otwarty.", path{});

        resolve(read_at(*held, at, much));
    }};
}

Promise<std::size_t> File::write(std::uintmax_t at, Bytes what) const {
    auto held = held_;

    return Promise<std::size_t>{[held, at, what = std::move(what)](auto resolve, auto) {
        if (!held) amiss("Plik nie jest otwarty.", path{});

        resolve(write_at(*held, at, what));
    }};
}

struct Flow::Held {
    std::shared_ptr<File::Held> file;
    std::atomic<std::uintmax_t> at{0};
    std::atomic<bool> busy{false};
};

namespace {

// Zajętość bierzemy w chwili tworzenia obietnicy, nie w chwili jej wykonania — obietnice są
// leniwe, więc dwa odczyty utworzone przed uruchomieniem któregokolwiek wystartowałyby
// z tej samej pozycji, a który padnie, zależałoby od kolejności w puli. Strażnik zwalnia
// strumień, gdy operacja się kończy — także wyjątkiem — i gdy obietnica ginie nieruszona.
class Claim {
public:
    explicit Claim(std::shared_ptr<Flow::Held> held) : held_(std::move(held)) {}

    ~Claim() { free(); }

    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;
    Claim(Claim&&) = delete;
    Claim& operator=(Claim&&) = delete;

    void free() {
        if (!held_) return;

        held_->busy.store(false);
        held_.reset();
    }

private:
    std::shared_ptr<Flow::Held> held_;
};

class Freeing {
public:
    explicit Freeing(std::shared_ptr<Claim> claim) : claim_(std::move(claim)) {}

    ~Freeing() { claim_->free(); }

    Freeing(const Freeing&) = delete;
    Freeing& operator=(const Freeing&) = delete;
    Freeing(Freeing&&) = delete;
    Freeing& operator=(Freeing&&) = delete;

private:
    std::shared_ptr<Claim> claim_;
};

// Funkcja oddająca obietnicę nie powinna rzucać w twarz wołającemu — wszystko, co się nie
// udało, ma przyjść tą samą drogą co reszta błędów modułu. Stąd zamiast wyjątku wracamy
// z pustym strażnikiem i powodem, a wołający robi z tego obietnicę, która odrzuci.
struct Claiming {
    std::shared_ptr<Claim> claim;
    std::string said;
    path where;
};

Claiming claimed(const std::shared_ptr<Flow::Held>& held) {
    if (!held) return {nullptr, "Strumień jest zamknięty.", path{}};

    bool spare = false;

    if (!held->busy.compare_exchange_strong(spare, true))
        return {nullptr, "Strumień jest zajęty.", held->file->where};

    return {std::make_shared<Claim>(held), std::string{}, path{}};
}

}

Flow File::flow() const {
    if (!held_) amiss("Plik nie jest otwarty.", path{});

    auto made = std::make_shared<Flow::Held>();
    made->file = held_;

    return Flow{made};
}

const path& Flow::where() const {
    static const path nowhere;

    return held_ ? held_->file->where : nowhere;
}

std::uintmax_t Flow::at() const { return held_ ? held_->at.load() : 0; }

void Flow::seek(std::uintmax_t at) {
    if (!held_) amiss("Strumień jest zamknięty.", path{});

    if (held_->busy.load()) amiss("Strumień jest zajęty.", held_->file->where);

    held_->at.store(at);
}

Promise<Bytes> Flow::read(std::size_t much) {
    auto held = held_;
    Claiming got = claimed(held);

    if (!got.claim)
        return Promise<Bytes>{
            [said = std::move(got.said), where = std::move(got.where)](auto, auto) {
                amiss(said, where);
            }};

    return Promise<Bytes>{[held, claim = std::move(got.claim), much](auto resolve, auto) {
        Bytes made;

        // Zwolnienie musi wypaść przed rozstrzygnięciem, nie po nim: resolve budzi wątek
        // czekający na await, a ten natychmiast sięga po następny odczyt. Zwalnianie po
        // resolve zostawiało szparę, w której kolejne czytanie z pętli słyszało „zajęty".
        {
            const Freeing freeing{claim};

            const std::uintmax_t at = held->at.load();

            made = read_at(*held->file, at, much);

            held->at.store(at + made.size());
        }

        resolve(std::move(made));
    }};
}

Promise<std::size_t> Flow::write(Bytes what) {
    auto held = held_;
    Claiming got = claimed(held);

    if (!got.claim)
        return Promise<std::size_t>{
            [said = std::move(got.said), where = std::move(got.where)](auto, auto) {
                amiss(said, where);
            }};

    return Promise<std::size_t>{
        [held, claim = std::move(got.claim), what = std::move(what)](auto resolve, auto) {
            std::size_t much = 0;

            {
                const Freeing freeing{claim};

                const std::uintmax_t at = held->at.load();

                much = write_at(*held->file, at, what);

                held->at.store(at + much);
            }

            resolve(much);
        }};
}

Promise<std::uintmax_t> File::size() const {
    auto held = held_;

    return Promise<std::uintmax_t>{[held](auto resolve, auto) {
        if (!held) amiss("Plik nie jest otwarty.", path{});

        resolve(sized(*held));
    }};
}

Promise<File> open(const path& where, Mode how) {
    return Promise<File>{
        [where, how](auto resolve, auto) { resolve(File{open_at(where, how)}); }};
}

namespace {

// Bilet operacji ratami: wie, ile bajtów albo pozycji z ilu już zeszło. Wirtualny progress
// z Ticket jest tu po to, żeby pasek postępu pytał, a nikt niczego nie przepisywał w górę.
template <typename T>
struct Counted : Result<T> {
    std::atomic<std::uintmax_t> done{0};
    std::atomic<std::uintmax_t> whole{0};

    [[nodiscard]] double progress() const override {
        if (over(*this)) return 1.0;

        const std::uintmax_t all = whole.load();
        if (all == 0) return 0.0;

        return std::min(1.0, static_cast<double>(done.load()) / static_cast<double>(all));
    }
};

template <typename T>
class HaulPromise : public Promise<T> {
public:
    HaulPromise(typename Promise<T>::Callback given, std::shared_ptr<Counted<T>> made)
        : Promise<T>(std::move(given), std::move(made)) {}
};

std::size_t rated(const path& from, const path& to) {
    return std::min(slice_at(from), slice_at(to));
}

// Udział sieciowy poznajemy tym samym rozpoznaniem, które dobiera ratę — krótsza rata
// i brak wiarygodnych powiadomień biorą się z tej samej przyczyny.
[[maybe_unused]] bool afar_at(const path& where) { return slice_at(where) == afar; }

#ifdef _WIN32

std::vector<Drive> drives_at() {
    std::vector<Drive> made;

    const DWORD mask = GetLogicalDrives();

    for (int at = 0; at < 26; ++at) {
        if ((mask & (1ul << at)) == 0) continue;

        const wchar_t letter = static_cast<wchar_t>(L'A' + at);
        const std::wstring root = std::wstring{letter} + L":\\";

        Drive one;

        one.where = path{root};
        one.mount = mount_at(one.where);

        std::wstring label(MAX_PATH + 1, L'\0');

        if (GetVolumeInformationW(root.c_str(), label.data(), MAX_PATH, nullptr, nullptr,
                                  nullptr, nullptr, 0) != 0) {
            label.resize(std::wcslen(label.c_str()));
            one.named = path{label}.string();
        }

        std::error_code ignored;

        const stood::space_info told = stood::space(one.where, ignored);

        if (!ignored) {
            one.room.whole = told.capacity;
            one.room.spare = told.available;
        }

        // Dysk mapowany pokazuje się jako litera, ale panel powinien wiedzieć, co za nią stoi.
        if (one.mount == Mount::remote) {
            std::wstring behind(1024, L'\0');
            DWORD much = static_cast<DWORD>(behind.size());

            const std::wstring named = std::wstring{letter} + L":";

            if (WNetGetConnectionW(named.c_str(), behind.data(), &much) == NO_ERROR) {
                behind.resize(std::wcslen(behind.c_str()));
                one.behind = path{behind};
            }
        }

        made.push_back(std::move(one));
    }

    return made;
}

#else

// Nazwy w /proc/mounts mają spacje zapisane ósemkowo, bo pola rozdziela spacja.
std::string plainly(const std::string& said) {
    std::string made;

    for (std::size_t at = 0; at < said.size(); ++at) {
        if (said[at] != '\\' || at + 3 >= said.size()) {
            made += said[at];
            continue;
        }

        const std::string trio = said.substr(at + 1, 3);

        if (trio.find_first_not_of("01234567") != std::string::npos) {
            made += said[at];
            continue;
        }

        made += static_cast<char>(std::stoi(trio, nullptr, 8));
        at += 3;
    }

    return made;
}

// Punkty montowania pod /proc, /sys, /dev i /run to wnętrzności systemu, nie miejsca do
// oglądania. Sam rodzaj systemu plików nie wystarcza, bo /sys/fs/cgroup jest zwykłym tmpfs.
bool tucked(const std::string& where) {
    static const std::vector<std::string> among{"/proc", "/sys", "/dev", "/run"};

    for (const std::string& one : among)
        if (where == one || where.rfind(one + "/", 0) == 0) return true;

    return false;
}

bool phony(const std::string& kind) {
    static const std::vector<std::string> among{
        "proc",     "sysfs",     "cgroup",   "cgroup2",     "devpts",  "securityfs",
        "debugfs",  "tracefs",   "pstore",   "bpf",         "configfs", "fusectl",
        "hugetlbfs", "mqueue",   "binfmt_misc", "autofs",   "devtmpfs", "efivarfs",
        "selinuxfs", "nsfs",     "ramfs",    "rpc_pipefs",  "squashfs"};

    return std::find(among.begin(), among.end(), kind) != among.end();
}

Mount minded(const std::string& kind) {
    if (kind.rfind("nfs", 0) == 0 || kind == "cifs" || kind == "smbfs" || kind == "smb3" ||
        kind.rfind("fuse.", 0) == 0 || kind == "afs" || kind == "9p")
        return Mount::remote;

    if (kind == "iso9660" || kind == "udf") return Mount::optical;

    return Mount::disk;
}

// Etykiety nośników leżą w /dev/disk/by-label jako dowiązania do urządzeń. Nic lepszego bez
// dodatkowych bibliotek nie ma, a to wystarcza, żeby panel pokazał nazwę zamiast /dev/sdb1.
std::map<path, std::string> labelled() {
    std::map<path, std::string> made;

    std::error_code trouble;
    stood::directory_iterator at{"/dev/disk/by-label", trouble};

    if (trouble) return made;

    for (const stood::directory_entry& one : at) {
        std::error_code ignored;

        const path onto = stood::canonical(one.path(), ignored);
        if (ignored) continue;

        made[onto] = plainly(one.path().filename().string());
    }

    return made;
}

std::vector<Drive> drives_at() {
    std::vector<Drive> made;

    std::ifstream reading{"/proc/mounts"};
    if (!reading) reading.open("/etc/mtab");
    if (!reading) return made;

    const std::map<path, std::string> labels = labelled();

    std::string line;

    while (std::getline(reading, line)) {
        std::istringstream taking{line};

        std::string device;
        std::string where;
        std::string kind;

        if (!(taking >> device >> where >> kind)) continue;

        const std::string here = plainly(where);

        if (phony(kind) || tucked(here)) continue;

        Drive one;

        one.where = path{here};
        one.mount = minded(kind);
        one.named = plainly(device);

        std::error_code ignored;

        const path onto = stood::canonical(one.named, ignored);

        if (!ignored) {
            const auto found = labels.find(onto);
            if (found != labels.end()) one.named = found->second;
        }

        if (one.mount == Mount::remote) one.behind = path{plainly(device)};

        const stood::space_info told = stood::space(one.where, ignored);

        if (!ignored) {
            one.room.whole = told.capacity;
            one.room.spare = told.available;
        }

        // Ten sam punkt montowania może wystąpić kilka razy — liczy się ostatni, bo to on
        // przykrywa poprzednie.
        const auto stale = std::find_if(made.begin(), made.end(), [&one](const Drive& mine) {
            return mine.where == one.where;
        });

        if (stale != made.end()) *stale = std::move(one);
        else made.push_back(std::move(one));
    }

    std::sort(made.begin(), made.end(),
              [](const Drive& one, const Drive& two) { return one.where < two.where; });

    return made;
}

#endif

void writing_at(const path& where, std::chrono::sys_seconds when) {
    const Apart apart = apart_now();

    const stood::file_time_type told{std::chrono::duration_cast<stood::file_time_type::duration>(
        std::chrono::duration_cast<Apart>(when.time_since_epoch()) - apart)};

    std::error_code trouble;

    stood::last_write_time(where, told, trouble);
    if (trouble) amiss("Nie można ustawić czasu zapisu.", where);
}

path real_at(const path& where) {
    std::error_code trouble;

    const path told = stood::weakly_canonical(where, trouble);

    return trouble ? where : told;
}

// Rozwijanie dowiązań potrafi zawrócić tam, gdzie już byliśmy — link wskazujący na przodka
// zapętliłby kopiowanie na zawsze. Trzymamy więc rzeczywiste ścieżki rozwiniętych katalogów.
using Seen = std::vector<path>;

bool sighted(Seen& seen, const path& real) {
    if (std::find(seen.begin(), seen.end(), real) != seen.end()) return true;

    seen.push_back(real);

    return false;
}

// Zwraca ścieżkę, pod którą wolno pisać, albo pustą, gdy pozycję należy pominąć.
path settled_on(const path& to, Clash how) {
    if (kind_at(to) == Kind::none) return to;

    switch (how) {
        case Clash::overwrite: return to;
        case Clash::skip: return {};
        case Clash::refuse: amiss("Cel już istnieje.", to);
        case Clash::beside: break;
    }

    const path folder = to.parent_path();
    const std::string stem = to.stem().string();
    const std::string tail = to.extension().string();

    for (int at = 2; at <= 100; ++at) {
        const path other = folder / (stem + " (" + std::to_string(at) + ")" + tail);

        if (kind_at(other) == Kind::none) return other;
    }

    amiss("Nie ma wolnej nazwy obok celu.", to);
}

// Kopia przerwana w połowie nie może zostawić po sobie pliku, który wygląda na kompletny.
// Cel znika, chyba że doszliśmy do końca.
class Sweeping {
public:
    explicit Sweeping(path where) : where_(std::move(where)), was_(exists_at(where_)) {}

    ~Sweeping() {
        if (kept_ || was_) return;

        std::error_code ignored;
        stood::remove_all(where_, ignored);
    }

    Sweeping(const Sweeping&) = delete;
    Sweeping& operator=(const Sweeping&) = delete;
    Sweeping(Sweeping&&) = delete;
    Sweeping& operator=(Sweeping&&) = delete;

    void keep() { kept_ = true; }

private:
    path where_;
    bool was_ = false;
    bool kept_ = false;
};

// Zwraca fałsz, gdy zadanie anulowano w połowie — wtedy obietnica nie rozstrzyga się wcale.
bool file_at(const path& from, const path& to, Counted<void>& held) {
    const std::shared_ptr<File::Held> mine = open_at(from, Mode::read);

    const std::uintmax_t whole = sized(*mine);
    held.whole.store(whole);

    Sweeping sweeping{to};

    const std::shared_ptr<File::Held> yours = open_at(to, Mode::fresh);

    const std::size_t rata = rated(from, to);

    for (std::uintmax_t at = 0; at < whole;) {
        if (stopped()) return false;

        const Bytes part = read_at(*mine, at, rata);
        if (part.empty()) break;

        at += write_at(*yours, at, part);
        held.done.store(at);
    }

    sweeping.keep();

    return true;
}

bool tree_at(const path& from, const path& to, Counted<void>& held, Clash how, Seen& seen,
             int deep);

// Jedna pozycja z wykazu. Dowiązanie rozwijamy — kopiujemy to, na co wskazuje, a nie samo
// dowiązanie. Potok, gniazdo i urządzenie pomijamy: otwarcie potoku do odczytu zawiesiłoby
// wątek puli bez ratunku, a to gorsze niż brak kopii.
bool spread_at(const path& from, Kind kind, const path& to, Clash how, Seen& seen, int deep) {
    if (kind == Kind::folder) {
        make_at(to);
        writing_at(to, written_at(from));

        return true;
    }

    path onto = from;

    if (kind == Kind::link) {
        std::error_code trouble;

        onto = stood::canonical(from, trouble);
        if (trouble) return true;

        kind = kind_at(onto);

        if (kind == Kind::folder) {
            if (sighted(seen, onto)) return true;

            Counted<void> apart;

            return tree_at(onto, to, apart, how, seen, deep + 1);
        }
    }

    if (kind != Kind::file) return true;

    const path where = settled_on(to, how);
    if (where.empty()) return true;

    Counted<void> apart;

    if (!file_at(onto, where, apart)) return false;

    writing_at(where, written_at(onto));

    return true;
}

// Katalog idzie wpis po wpisie; postęp liczymy tutaj we wpisach, nie w bajtach. Wykaz jest
// posortowany ścieżkami, a rodzic zawsze poprzedza swoje dzieci, więc katalogi da się
// tworzyć po kolei. Zawartość rozwiniętych dowiązań ma własny licznik i do tego się nie
// dolicza — inaczej mieszalibyśmy dwie miary w jednym ułamku.
bool tree_at(const path& from, const path& to, Counted<void>& held, Clash how, Seen& seen,
             int deep) {
    if (deep > 64) return true;

    const std::vector<Entry> among = walk_at(from);

    held.whole.store(among.size() + 1);

    Sweeping sweeping{to};

    make_at(to);
    held.done.fetch_add(1);

    for (const Entry& one : among) {
        if (stopped()) return false;

        const path where = to / one.where.lexically_relative(from);

        if (!spread_at(one.where, one.kind, where, how, seen, deep)) return false;

        held.done.fetch_add(1);
    }

    sweeping.keep();

    writing_at(to, written_at(from));

    return true;
}

bool copy_at(const path& from, const path& to, Counted<void>& held, Clash how) {
    Kind what = kind_at(from);

    if (what == Kind::none) amiss("Nie ma czego kopiować.", from);

    path onto = from;

    if (what == Kind::link) {
        std::error_code trouble;

        onto = stood::canonical(from, trouble);
        if (trouble) amiss("Dowiązanie prowadzi donikąd.", from);

        what = kind_at(onto);
    }

    Seen seen;

    if (what == Kind::folder) {
        sighted(seen, real_at(onto));

        return tree_at(onto, to, held, how, seen, 0);
    }

    if (what != Kind::file) amiss("Tej pozycji nie da się skopiować.", from);

    const path where = settled_on(to, how);
    if (where.empty()) return true;

    if (!file_at(onto, where, held)) return false;

    writing_at(where, written_at(onto));

    return true;
}

}

Promise<Bytes> read(const path& where) {
    auto held = std::make_shared<Counted<Bytes>>();

    return HaulPromise<Bytes>{[where, held](auto resolve, auto) {
                              const std::shared_ptr<File::Held> one =
                                  open_at(where, Mode::read);

                              const std::uintmax_t whole = sized(*one);
                              held->whole.store(whole);

                              const std::size_t rata = slice_at(where);

                              Bytes made;
                              made.reserve(static_cast<std::size_t>(whole));

                              for (std::uintmax_t at = 0; at < whole;) {
                                  if (stopped()) return;

                                  Bytes part = read_at(*one, at, rata);
                                  if (part.empty()) break;

                                  at += part.size();
                                  held->done.store(at);

                                  made.insert(made.end(), part.begin(), part.end());
                              }

                              resolve(std::move(made));
                          },
                          held};
}

Promise<void> write(const path& where, Bytes what, Clash how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[where, what = std::move(what), how, held](auto resolve, auto) {
                             const path at_all = settled_on(where, how);

                             if (at_all.empty()) {
                                 resolve();
                                 return;
                             }

                             const std::shared_ptr<File::Held> one =
                                 open_at(at_all, Mode::fresh);

                             held->whole.store(what.size());

                             const std::size_t rata = slice_at(at_all);

                             for (std::size_t at = 0; at < what.size();) {
                                 if (stopped()) return;

                                 const std::size_t much =
                                     std::min<std::size_t>(rata, what.size() - at);

                                 Bytes part{what.begin() + static_cast<std::ptrdiff_t>(at),
                                            what.begin() +
                                                static_cast<std::ptrdiff_t>(at + much)};

                                 at += write_at(*one, at, part);
                                 held->done.store(at);
                             }

                             resolve();
                         },
                         held};
}

Promise<void> copy(const path& from, const path& to, Clash how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[from, to, how, held](auto resolve, auto) {
                             if (!copy_at(from, to, *held, how)) return;

                             resolve();
                         },
                         held};
}

// Najpierw zwykła zmiana nazwy, bo w obrębie jednego nośnika jest natychmiastowa. Dopiero
// gdy się nie uda — a nie uda się między nośnikami — kopiujemy i sprzątamy źródło.
Promise<void> move(const path& from, const path& to, Clash how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[from, to, how, held](auto resolve, auto) {
                             if (kind_at(from) == Kind::none)
                                 amiss("Nie ma czego przenieść.", from);

                             const path where = settled_on(to, how);

                             if (where.empty()) {
                                 held->whole.store(1);
                                 held->done.store(1);

                                 resolve();
                                 return;
                             }

                             std::error_code trouble;
                             stood::rename(from, where, trouble);

                             if (!trouble) {
                                 held->whole.store(1);
                                 held->done.store(1);

                                 resolve();
                                 return;
                             }

                             if (!copy_at(from, where, *held, Clash::overwrite)) return;

                             std::error_code ignored;
                             stood::remove_all(from, ignored);

                             resolve();
                         },
                         held};
}

Promise<void> erase(const path& where) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[where, held](auto resolve, auto) {
                             if (kind_at(where) == Kind::none) {
                                 resolve();
                                 return;
                             }

                             std::vector<Entry> among;

                             if (kind_at(where) == Kind::folder) among = walk_at(where);

                             held->whole.store(among.size() + 1);

                             // Od najgłębszych, bo katalog daje się usunąć dopiero pusty.
                             for (auto at = among.rbegin(); at != among.rend(); ++at) {
                                 if (stopped()) return;

                                 std::error_code ignored;
                                 stood::remove(at->where, ignored);

                                 held->done.fetch_add(1);
                             }

                             std::error_code trouble;
                             stood::remove(where, trouble);

                             if (trouble) amiss("Nie można usunąć.", where);

                             held->done.fetch_add(1);

                             resolve();
                         },
                         held};
}

Trouble::Trouble(const std::string& said, path where)
    : std::runtime_error(said), where_(std::move(where)) {}

Promise<bool> exists(const path& where) {
    return Promise<bool>{[where](auto resolve, auto) { resolve(exists_at(where)); }};
}

Promise<Kind> kind(const path& where) {
    return Promise<Kind>{[where](auto resolve, auto) { resolve(kind_at(where)); }};
}

Promise<std::uintmax_t> size(const path& where) {
    return Promise<std::uintmax_t>{[where](auto resolve, auto) { resolve(size_at(where)); }};
}

Promise<std::chrono::sys_seconds> written(const path& where) {
    return Promise<std::chrono::sys_seconds>{
        [where](auto resolve, auto) { resolve(written_at(where)); }};
}

Promise<void> written(const path& where, std::chrono::sys_seconds when) {
    return Promise<void>{[where, when](auto resolve, auto) {
        writing_at(where, when);
        resolve();
    }};
}

Promise<Room> roomy(const path& where) {
    return Promise<Room>{[where](auto resolve, auto) { resolve(roomy_at(where)); }};
}

Promise<Mount> mount(const path& where) {
    return Promise<Mount>{[where](auto resolve, auto) { resolve(mount_at(where)); }};
}

Promise<std::vector<Drive>> drives() {
    return Promise<std::vector<Drive>>{[](auto resolve, auto) { resolve(drives_at()); }};
}

Promise<std::size_t> slice(const path& where) {
    return Promise<std::size_t>{[where](auto resolve, auto) { resolve(slice_at(where)); }};
}

Promise<Marks> marks(const path& where) {
    return Promise<Marks>{[where](auto resolve, auto) { resolve(marks_at(where)); }};
}

Promise<void> marks(const path& where, Marks what) {
    return Promise<void>{[where, what](auto resolve, auto) {
        marked_at(where, what);
        resolve();
    }};
}

Promise<bool> alike(const path& one, const path& two) {
    return Promise<bool>{[one, two](auto resolve, auto) { resolve(alike_at(one, two)); }};
}

Promise<path> target(const path& where) {
    return Promise<path>{[where](auto resolve, auto) { resolve(target_at(where)); }};
}

Promise<path> real(const path& where) {
    return Promise<path>{[where](auto resolve, auto) { resolve(real_at(where)); }};
}

Promise<void> link(const path& where, const path& onto, bool hard) {
    return Promise<void>{[where, onto, hard](auto resolve, auto) {
        linking_at(where, onto, hard);
        resolve();
    }};
}

// Katalog bieżący jest wspólny dla całego procesu, więc jego zmiana dotyka też wątków puli.
// Stąd osobna obietnica: zmiana jest zdarzeniem, odczyt tylko zajrzeniem.
path here() {
    std::error_code ignored;

    return stood::current_path(ignored);
}

Promise<void> here(const path& where) {
    return Promise<void>{[where](auto resolve, auto) {
        std::error_code trouble;

        stood::current_path(where, trouble);
        if (trouble) amiss("Nie można wejść do katalogu.", where);

        resolve();
    }};
}

path home() {
#ifdef _WIN32

    if (const wchar_t* told = _wgetenv(L"USERPROFILE")) return path{told};

#else

    if (const char* told = std::getenv("HOME")) return path{told};

#endif

    return {};
}

path temp() {
    std::error_code ignored;

    return stood::temp_directory_path(ignored);
}

Promise<void> make(const path& where) {
    return Promise<void>{[where](auto resolve, auto) {
        make_at(where);
        resolve();
    }};
}

Promise<void> rename(const path& from, const path& to, Clash how) {
    return Promise<void>{[from, to, how](auto resolve, auto) {
        const path where = settled_on(to, how);

        if (!where.empty()) rename_at(from, where);

        resolve();
    }};
}

Promise<std::vector<Entry>> list(const path& where) {
    return Promise<std::vector<Entry>>{
        [where](auto resolve, auto) { resolve(list_at(where)); }};
}

Promise<std::vector<Entry>> walk(const path& where) {
    return Promise<std::vector<Entry>>{
        [where](auto resolve, auto) { resolve(walk_at(where)); }};
}


// ---------------------------------------------------------------------------------------
// Obserwowanie katalogów. Jeden wątek na całą bibliotekę, tak samo jak zegar w Time: sto
// obserwowanych katalogów nie może kosztować stu wątków.
// ---------------------------------------------------------------------------------------

struct Watch::Held {
    path where;
    bool deep = false;
    std::uint64_t which = 0;

    struct Wanting {
        std::shared_ptr<Result<std::vector<Change>>> result;
        std::shared_ptr<Waiter> gone;
    };

    std::mutex guard;
    std::deque<Change> pending;
    std::vector<Wanting> waiting;

    ~Held();

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;
};

namespace {

constexpr std::size_t brim = 4096;

class Watching;

Watching& watching();

// Wspólne ujście dla obu dróg. Zaplecze nie wie, komu służy — oddaje zmiany pod numerem,
// a rozdzielacz wie, czy to zwykła obserwacja, czy plik pilnowany przez katalog nadrzędny.
void reached(std::uint64_t which, std::vector<Change> among);

// Obietnica sięgająca po chroniony konstruktor, żeby podstawić bilet, który rozstrzygnie
// wątek obserwatora, a nie własne wywołanie.
class Nexting : public Promise<std::vector<Change>> {
public:
    Nexting(Promise<std::vector<Change>>::Callback given,
            std::shared_ptr<Result<std::vector<Change>>> made)
        : Promise<std::vector<Change>>(std::move(given), std::move(made)) {}
};

// Anulowane zgłoszenie musi zniknąć z kolejki czekających, inaczej pierwsza paczka poszłaby
// do nikogo. To samo ogniwo co Dropping w zegarze.
class Forgetting : public Waiter {
public:
    Forgetting(std::weak_ptr<Watch::Held> whose, Result<std::vector<Change>>* which)
        : whose_(std::move(whose)), which_(which) {}

    void woke() override {
        const std::shared_ptr<Watch::Held> held = whose_.lock();
        if (!held) return;

        const std::lock_guard<std::mutex> kept{held->guard};

        const auto at = std::find_if(
            held->waiting.begin(), held->waiting.end(),
            [this](const Watch::Held::Wanting& one) { return one.result.get() == which_; });

        if (at != held->waiting.end()) held->waiting.erase(at);
    }

private:
    std::weak_ptr<Watch::Held> whose_;
    Result<std::vector<Change>>* which_;
};

// Powtórzone „zmieniony" dla tej samej ścieżki scalamy — system plików produkuje serie, a
// panel i tak odrysuje się raz. Po przepełnieniu zostaje jeden wpis: zgubiłem rachubę.
void noting(Watch::Held& held, Change made) {
    if (made.stir == Stir::changed) {
        std::size_t looked = 0;

        for (auto at = held.pending.rbegin(); at != held.pending.rend() && looked < 32;
             ++at, ++looked) {
            if (at->where != made.where) continue;
            if (at->stir == Stir::changed) return;

            break;
        }
    }

    held.pending.push_back(std::move(made));

    if (held.pending.size() <= brim) return;

    held.pending.clear();
    held.pending.push_back(Change{held.where, path{}, Stir::lost});
}

void handed(const std::shared_ptr<Watch::Held>& held, std::vector<Change> among) {
    std::shared_ptr<Result<std::vector<Change>>> lucky;
    std::vector<Change> giving;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        for (Change& one : among) noting(*held, std::move(one));

        if (!held->waiting.empty() && !held->pending.empty()) {
            lucky = held->waiting.front().result;
            held->waiting.erase(held->waiting.begin());

            giving.assign(std::make_move_iterator(held->pending.begin()),
                          std::make_move_iterator(held->pending.end()));
            held->pending.clear();
        }
    }

    // Poza zamkiem: rozstrzygnięcie budzi obserwatorów, a wśród nich nasze własne ogniwo.
    if (lucky) settled(*lucky, std::move(giving));
}

}

namespace {

#if defined(__linux__)

constexpr std::uint32_t noticed = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                                  IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

struct Post {
    path where;
    bool deep = false;
    std::vector<int> spots;
};

// Katalogi pod korzeniem zbieramy własnym przejściem, a nie przez walk_at — tamto pyta
// dzwonek bieżącego zadania, a wątek obserwatora żadnego zadania nie ma.
std::vector<path> under(const path& where) {
    std::vector<path> made;
    std::vector<path> waiting{where};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        std::error_code trouble;
        stood::directory_iterator at{here, stood::directory_options::skip_permission_denied,
                                     trouble};

        if (trouble) continue;

        for (const stood::directory_entry& one : at) {
            std::error_code ignored;

            if (!one.is_directory(ignored) || one.is_symlink(ignored)) continue;

            made.push_back(one.path());
            waiting.push_back(one.path());
        }
    }

    return made;
}

class Prompt {
public:
    void begin() {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);

        keeper_ = std::thread{[this] { serve(); }};
    }

    [[nodiscard]] bool ready() const { return fd_ >= 0; }

    void add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;

        std::vector<std::pair<int, path>> made;

        const int first = inotify_add_watch(fd_, where.c_str(), noticed);
        if (first < 0) amiss("Nie można obserwować.", where);

        made.emplace_back(first, where);

        if (deep)
            for (const path& one : under(where)) {
                const int more = inotify_add_watch(fd_, one.c_str(), noticed);
                if (more >= 0) made.emplace_back(more, one);
            }

        const std::lock_guard<std::mutex> kept{guard_};

        for (const auto& [spot, where] : made) {
            post->spots.push_back(spot);
            spots_[spot].push_back(Spot{which, where});
        }

        posts_[which] = std::move(post);
    }

    // inotify na to samo miejsce oddaje ten sam deskryptor i tylko scala maski — dwie
    // obserwacje jednego katalogu dzielą więc jeden wpis. Zdejmujemy go dopiero wtedy, gdy
    // nikt już z niego nie korzysta.
    void drop(std::uint64_t which) {
        std::vector<int> spare;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) return;

            const std::shared_ptr<Post> taken = std::move(at->second);
            posts_.erase(at);

            for (const int one : taken->spots) {
                const auto found = spots_.find(one);
                if (found == spots_.end()) continue;

                std::vector<Spot>& among = found->second;

                among.erase(std::remove_if(among.begin(), among.end(),
                                           [which](const Spot& mine) {
                                               return mine.which == which;
                                           }),
                            among.end());

                if (among.empty()) {
                    spots_.erase(found);
                    spare.push_back(one);
                }
            }
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

private:
    struct Spot {
        std::uint64_t which = 0;
        path where;
    };

    void serve() {
        std::vector<char> room(64 * 1024);

        while (true) {
            pollfd told{};
            told.fd = fd_;
            told.events = POLLIN;

            if (::poll(&told, 1, 1000) <= 0) continue;

            const ssize_t given = ::read(fd_, room.data(), room.size());
            if (given <= 0) continue;

            take(room.data(), static_cast<std::size_t>(given));
        }
    }

    void take(const char* room, std::size_t much) {
        std::unordered_map<std::uint64_t, std::vector<Change>> among;
        std::map<std::pair<std::uint64_t, std::uint32_t>, path> moving;
        std::vector<std::pair<std::uint64_t, path>> fresh;

        for (std::size_t at = 0; at + sizeof(inotify_event) <= much;) {
            const auto* told = reinterpret_cast<const inotify_event*>(room + at);
            at += sizeof(inotify_event) + told->len;

            if ((told->mask & IN_Q_OVERFLOW) != 0) {
                spilled();
                return;
            }

            std::vector<Spot> spots;

            {
                const std::lock_guard<std::mutex> kept{guard_};

                const auto found = spots_.find(told->wd);
                if (found == spots_.end()) continue;

                spots = found->second;
            }

            // Ten sam deskryptor może należeć do kilku obserwacji naraz, więc jedno
            // zdarzenie rozchodzi się do każdej z nich osobno.
            for (const Spot& spot : spots) {
                const std::uint64_t which = spot.which;
                const path where = told->len > 0 ? spot.where / path{told->name} : spot.where;

                std::vector<Change>& mine = among[which];

                if ((told->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    mine.push_back(Change{where, path{}, Stir::gone});
                    continue;
                }

                if ((told->mask & IN_MOVED_FROM) != 0) {
                    moving[{which, told->cookie}] = where;
                    mine.push_back(Change{where, path{}, Stir::gone});
                    continue;
                }

                if ((told->mask & IN_MOVED_TO) != 0) {
                    const auto paired = moving.find({which, told->cookie});

                    if (paired == moving.end()) {
                        mine.push_back(Change{where, path{}, Stir::made});
                    } else {
                        // Para „wyszło stąd" i „weszło tam" to jedno przemianowanie, więc
                        // zdejmujemy zgłoszone przed chwilą zniknięcie.
                        const auto stale =
                            std::find_if(mine.begin(), mine.end(), [&paired](const Change& one) {
                                return one.stir == Stir::gone && one.where == paired->second;
                            });

                        if (stale != mine.end()) mine.erase(stale);

                        mine.push_back(Change{where, paired->second, Stir::renamed});
                        moving.erase(paired);
                    }

                    if ((told->mask & IN_ISDIR) != 0) fresh.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_CREATE) != 0) {
                    mine.push_back(Change{where, path{}, Stir::made});

                    if ((told->mask & IN_ISDIR) != 0) fresh.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_DELETE) != 0) {
                    mine.push_back(Change{where, path{}, Stir::gone});
                    continue;
                }

                mine.push_back(Change{where, path{}, Stir::changed});
            }
        }

        for (const auto& [which, where] : fresh) sprout(which, where);

        for (auto& [which, mine] : among) reached(which, std::move(mine));
    }

    // Nowy podkatalog pod obserwacją w głąb trzeba dopisać z osobna — inotify nie zna
    // obserwowania drzewa, zna tylko pojedyncze katalogi. To jest cena, o której mowa
    // w CLAUDE.md: przy dużym drzewie to tysiące deskryptorów.
    void sprout(std::uint64_t which, const path& where) {
        std::vector<path> among{where};

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end() || !at->second->deep) return;
        }

        for (const path& one : under(where)) among.push_back(one);

        for (const path& one : among) {
            const int spot = inotify_add_watch(fd_, one.c_str(), noticed);
            if (spot < 0) continue;

            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) {
                inotify_rm_watch(fd_, spot);
                continue;
            }

            at->second->spots.push_back(spot);
            spots_[spot].push_back(Spot{which, one});
        }
    }

    void spilled() {
        std::vector<std::pair<std::uint64_t, path>> among;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            for (const auto& [which, post] : posts_) among.emplace_back(which, post->where);
        }

        for (const auto& [which, where] : among)
            reached(which, std::vector<Change>{Change{where, path{}, Stir::lost}});
    }

    std::mutex guard_;
    std::unordered_map<int, std::vector<Spot>> spots_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Post>> posts_;
    int fd_ = -1;
    std::thread keeper_;
};

#endif

}

namespace {

#if defined(_WIN32)

constexpr DWORD noticed = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                          FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_LAST_WRITE;

struct Post {
    path where;
    bool deep = false;
    std::uint64_t which = 0;

    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED told{};
    std::vector<char> room = std::vector<char>(64 * 1024);
    bool going = false;
};

class Prompt {
public:
    void begin() {
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);

        keeper_ = std::thread{[this] { serve(); }};
    }

    [[nodiscard]] bool ready() const { return port_ != nullptr; }

    void add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;

        post->handle = CreateFileW(longly(where).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

        if (post->handle == INVALID_HANDLE_VALUE) amiss("Nie można obserwować.", where);

        post->which = which;

        if (CreateIoCompletionPort(post->handle, port_, static_cast<ULONG_PTR>(which), 0) ==
            nullptr) {
            CloseHandle(post->handle);
            amiss("Nie można obserwować.", where);
        }

        if (!armed(*post)) {
            CloseHandle(post->handle);
            amiss("Nie można obserwować.", where);
        }

        const std::lock_guard<std::mutex> kept{guard_};

        posts_[which] = std::move(post);
    }

    // Uchwytu nie zamykamy tutaj. Odczyt jest w locie i system trzyma bufor oraz OVERLAPPED
    // aż do zakończenia; zamknięcie spod ręki zostawiłoby zapis do zwolnionej pamięci.
    // Znaczymy więc wpis i czekamy, aż zerwane zakończenie wróci do wątku obserwatora.
    void drop(std::uint64_t which) {
        const std::lock_guard<std::mutex> kept{guard_};

        const auto at = posts_.find(which);
        if (at == posts_.end()) return;

        at->second->going = true;

        CancelIoEx(at->second->handle, &at->second->told);
    }

private:
    static bool armed(Post& post) {
        DWORD given = 0;

        return ReadDirectoryChangesW(post.handle, post.room.data(),
                                     static_cast<DWORD>(post.room.size()),
                                     post.deep ? TRUE : FALSE, noticed, &given, &post.told,
                                     nullptr) != 0;
    }

    void serve() {
        while (true) {
            DWORD given = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* told = nullptr;

            const BOOL fine = GetQueuedCompletionStatus(port_, &given, &key, &told, INFINITE);

            if (told == nullptr) continue;

            std::shared_ptr<Post> post;

            {
                const std::lock_guard<std::mutex> kept{guard_};

                const auto at = posts_.find(static_cast<std::uint64_t>(key));
                if (at == posts_.end()) continue;

                post = at->second;

                if (post->going || fine == 0) {
                    CloseHandle(post->handle);
                    posts_.erase(at);

                    continue;
                }
            }

            // Zerowy odczyt znaczy, że systemowi zabrakło miejsca na zdarzenia. Nie ma czego
            // odzyskiwać — zostaje przyznanie się i prośba o ponowne przeczytanie katalogu.
            if (given == 0)
                reached(post->which,
                        std::vector<Change>{Change{post->where, path{}, Stir::lost}});
            else reached(post->which, take(*post, given));

            if (!armed(*post)) drop(post->which);
        }
    }

    static std::vector<Change> take(const Post& post, DWORD much) {
        std::vector<Change> among;

        path stale;
        std::size_t at = 0;

        while (at + sizeof(FILE_NOTIFY_INFORMATION) <= much) {
            const auto* told =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(post.room.data() + at);

            const std::wstring named{told->FileName,
                                     told->FileNameLength / sizeof(wchar_t)};

            const path where = post.where / path{named};

            switch (told->Action) {
                case FILE_ACTION_ADDED:
                    among.push_back(Change{where, path{}, Stir::made});
                    break;

                case FILE_ACTION_REMOVED:
                    among.push_back(Change{where, path{}, Stir::gone});
                    break;

                case FILE_ACTION_RENAMED_OLD_NAME:
                    stale = where;
                    break;

                case FILE_ACTION_RENAMED_NEW_NAME:
                    among.push_back(Change{where, stale, Stir::renamed});
                    stale.clear();
                    break;

                default:
                    among.push_back(Change{where, path{}, Stir::changed});
                    break;
            }

            if (told->NextEntryOffset == 0) break;

            at += told->NextEntryOffset;
        }

        return among;
    }

    std::mutex guard_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Post>> posts_;
    HANDLE port_ = nullptr;
    std::thread keeper_;
};

#endif

// Odpytywanie. Tam, gdzie system nie umie powiedzieć sam — a także tam, gdzie umie, ale
// kłamie — zaglądamy do katalogu co jakiś czas i porównujemy z tym, co widzieliśmy poprzednio.
// Nie ma tu przemianowań: znika stara nazwa, pojawia się nowa, i tyle da się uczciwie
// powiedzieć bez pomocy systemu.
struct Sight {
    Kind kind = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds written{};
};

std::map<path, Sight> sighted(const path& where, bool deep) {
    std::map<path, Sight> made;

    const Kind what = kind_at(where);
    if (what == Kind::none) return made;

    if (what != Kind::folder) {
        Sight mine;
        mine.kind = what;

        std::error_code ignored;
        mine.size = stood::file_size(where, ignored);

        try {
            mine.written = written_at(where);
        } catch (...) {
        }

        made[where] = mine;

        return made;
    }

    std::vector<Entry> among;

    try {
        among = deep ? walk_at(where) : list_at(where);
    } catch (...) {
        return made;
    }

    for (const Entry& one : among) made[one.where] = Sight{one.kind, one.size, one.written};

    return made;
}

std::vector<Change> apart(const std::map<path, Sight>& was, const std::map<path, Sight>& now) {
    std::vector<Change> made;

    for (const auto& [where, mine] : now) {
        const auto at = was.find(where);

        if (at == was.end()) {
            made.push_back(Change{where, path{}, Stir::made});
            continue;
        }

        if (at->second.kind != mine.kind || at->second.size != mine.size ||
            at->second.written != mine.written)
            made.push_back(Change{where, path{}, Stir::changed});
    }

    for (const auto& [where, mine] : was)
        if (now.find(where) == now.end()) made.push_back(Change{where, path{}, Stir::gone});

    return made;
}

class Polling {
public:
    void begin() { keeper_ = std::thread{[this] { serve(); }}; }

    // Pierwsze spojrzenie bierzemy tutaj, w wątku wołającego, żeby zmiany zaraz po powrocie
    // z watch() nie przepadły. Od tej chwili `seen` należy już wyłącznie do wątku
    // odpytującego, a przekazanie idzie przez ten zamek.
    void add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;
        post->seen = sighted(where, deep);

        {
            const std::lock_guard<std::mutex> kept{guard_};

            posts_[which] = std::move(post);
        }

        stirred_.notify_all();
    }

    void drop(std::uint64_t which) {
        const std::lock_guard<std::mutex> kept{guard_};

        posts_.erase(which);
    }

private:
    struct Post {
        path where;
        bool deep = false;
        std::map<path, Sight> seen;
    };

    // Nigdy nie zjadamy więcej niż dziesiątej części rdzenia: przerwa jest dziesięciokrotnie
    // dłuższa od samego zaglądania, nie krótsza niż pół sekundy i nie dłuższa niż dziesięć.
    // Mały katalog odpytuje się więc dwa razy na sekundę, a wielkie drzewo — rzadko, bo samo
    // przejście po nim kosztuje. Stała częstotliwość byłaby albo za wolna, albo za droga.
    void serve() {
        while (true) {
            std::vector<std::pair<std::uint64_t, std::shared_ptr<Post>>> among;

            {
                std::unique_lock<std::mutex> kept{guard_};

                if (posts_.empty()) {
                    stirred_.wait(kept);
                    continue;
                }

                among.reserve(posts_.size());
                for (const auto& [which, post] : posts_) among.emplace_back(which, post);
            }

            const auto began = std::chrono::steady_clock::now();

            for (const auto& [which, post] : among) {
                std::map<path, Sight> now = sighted(post->where, post->deep);
                std::vector<Change> mine = apart(post->seen, now);

                post->seen = std::move(now);

                if (!mine.empty()) reached(which, std::move(mine));
            }

            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began);

            const auto rest = std::clamp(spent * 10, std::chrono::milliseconds{500},
                                         std::chrono::milliseconds{10000});

            std::unique_lock<std::mutex> kept{guard_};
            stirred_.wait_for(kept, rest);
        }
    }

    std::mutex guard_;
    std::condition_variable stirred_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Post>> posts_;
    std::thread keeper_;
};

// Rozdzielacz. Zdarzenia z systemu są tańsze i szybsze, więc bierzemy je wszędzie, gdzie
// są wiarygodne — a na udziale sieciowym wiarygodne nie są. inotify nie widzi zmian
// zrobionych przez inną maszynę, bo jądro nie ma o nich pojęcia; polegać można tylko na
// własnym zaglądaniu. Dlatego o wyborze rozstrzyga to samo rozpoznanie nośnika, które daje
// ratę w `slice`.
class Watching {
public:
    void begin() {
        polling_.begin();

#if !defined(TALCITE_WATCH_POLLING) && (defined(__linux__) || defined(_WIN32))
        prompt_.begin();
#endif
    }

    std::uint64_t add(const std::shared_ptr<Watch::Held>& held) {
        Kept kept;

        kept.whose = held;
        kept.asking = asking(held->where);

        // Pojedynczego pliku nie obserwujemy wprost. ReadDirectoryChangesW w ogóle tego nie
        // umie, a inotify przywiązuje się do i-węzła, nie do nazwy — zapis przez podmianę
        // zostawiłby obserwację przy usuniętej treści. Pilnujemy więc katalogu nadrzędnego
        // i przy każdym drgnięciu naszej nazwy zaglądamy do samego pliku.
        if (!kept.asking && kind_at(held->where) != Kind::folder) {
            kept.peering = true;
            kept.only = held->where;
            kept.seen = sighted(held->where, false);
        }

        const path where = kept.peering ? held->where.parent_path() : held->where;
        const bool deep = kept.peering ? false : held->deep;

        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> held_{guard_};

            which = ++next_;
        }

        if (kept.asking) {
            polling_.add(which, where, deep);
        } else {
#if !defined(TALCITE_WATCH_POLLING) && (defined(__linux__) || defined(_WIN32))
            prompt_.add(which, where, deep);
#endif
        }

        // Dopiero po udanym dopisaniu — nieudane nie zostawia po sobie wpisu do zdejmowania.
        const std::lock_guard<std::mutex> held_{guard_};

        among_[which] = std::move(kept);

        return which;
    }

    void drop(std::uint64_t which) {
        bool mine = true;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            mine = at->second.asking;
            among_.erase(at);
        }

        if (mine) {
            polling_.drop(which);
            return;
        }

#if !defined(TALCITE_WATCH_POLLING) && (defined(__linux__) || defined(_WIN32))
        prompt_.drop(which);
#endif
    }

    void reached(std::uint64_t which, std::vector<Change> among) {
        std::shared_ptr<Watch::Held> held;
        bool peering = false;
        path only;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            held = at->second.whose.lock();
            peering = at->second.peering;
            only = at->second.only;
        }

        if (!held) return;

        if (!peering) {
            handed(held, std::move(among));
            return;
        }

        if (!touched(among, only)) return;

        // Katalog powiedział tylko, że coś drgnęło przy naszej nazwie. Co dokładnie — mówi
        // dopiero spojrzenie na sam plik, porównane z poprzednim.
        std::map<path, Sight> now = sighted(only, false);
        std::vector<Change> mine;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            mine = apart(at->second.seen, now);
            at->second.seen = std::move(now);
        }

        if (!mine.empty()) handed(held, std::move(mine));
    }

private:
    struct Kept {
        std::weak_ptr<Watch::Held> whose;
        bool asking = false;
        bool peering = false;
        path only;
        std::map<path, Sight> seen;
    };

    static bool touched(const std::vector<Change>& among, const path& only) {
        for (const Change& one : among) {
            if (one.stir == Stir::lost) return true;
            if (one.where == only || one.was == only) return true;
        }

        return false;
    }

    // Zdarzenia z systemu są tańsze i szybsze, więc bierzemy je wszędzie, gdzie są
    // wiarygodne. Na udziale sieciowym pod Linuksem wiarygodne nie są: inotify nie widzi
    // zmian zrobionych przez inną maszynę, bo jądro nie ma o nich pojęcia — a to jest
    // dokładnie przypadek NAS-a. Rozstrzyga to samo rozpoznanie nośnika, co przy racie.
    // Windows na udziale radzi sobie sam, bo powiadomienia przychodzą z serwera.
    static bool asking(const path& where) {
#if defined(TALCITE_WATCH_POLLING)
        (void)where;

        return true;
#elif defined(_WIN32)
        (void)where;

        return false;
#elif defined(__linux__)
        return afar_at(kind_at(where) == Kind::folder ? where : where.parent_path());
#else
        (void)where;

        return true;
#endif
    }

    std::mutex guard_;
    std::unordered_map<std::uint64_t, Kept> among_;
    std::uint64_t next_ = 0;

    Polling polling_;

#if !defined(TALCITE_WATCH_POLLING) && (defined(__linux__) || defined(_WIN32))
    Prompt prompt_;
#endif
};

// Obserwator jest celowo nieusuwany, z tego samego powodu co zegar: powstaje później niż
// pula, więc zniknąłby pierwszy, w chwili gdy robotnicy jeszcze biegną.
Watching& watching() {
    static Watching* only = [] {
        auto* made = new Watching;
        made->begin();

        return made;
    }();

    return *only;
}

void reached(std::uint64_t which, std::vector<Change> among) {
    watching().reached(which, std::move(among));
}

}


Watch::Held::~Held() {
    if (which != 0) watching().drop(which);
}

const path& Watch::where() const {
    static const path nowhere;

    return held_ ? held_->where : nowhere;
}

Promise<Watch> watch(const path& where, bool deep) {
    return Promise<Watch>{[where, deep](auto resolve, auto) {
        if (kind_at(where) != Kind::folder && kind_at(where) != Kind::file)
            amiss("Nie ma czego obserwować.", where);

        auto held = std::make_shared<Watch::Held>();

        held->where = where;
        held->deep = deep;
        held->which = watching().add(held);

        resolve(Watch{held});
    }};
}

Promise<std::vector<Change>> Watch::next() {
    auto held = held_;

    if (!held)
        return Promise<std::vector<Change>>{
            [](auto, auto) { amiss("Obserwacja jest zamknięta.", path{}); }};

    auto made = std::make_shared<Result<std::vector<Change>>>();

    return Nexting{[held, made](auto, auto) {
                       // Bilet trzyma obserwatorów słabo, więc ogniwo musi mieć właściciela
                       // z tej strony — inaczej ginie zaraz po powrocie stąd, a anulowane
                       // zgłoszenie zostaje w kolejce i połyka pierwszą paczkę.
                       const std::shared_ptr<Waiter> gone =
                           std::make_shared<Forgetting>(held, made.get());

                       std::vector<Change> giving;

                       {
                           const std::lock_guard<std::mutex> kept{held->guard};

                           if (held->pending.empty()) {
                               held->waiting.push_back(Watch::Held::Wanting{made, gone});
                           } else {
                               giving.assign(std::make_move_iterator(held->pending.begin()),
                                             std::make_move_iterator(held->pending.end()));
                               held->pending.clear();
                           }
                       }

                       if (!giving.empty()) {
                           settled(*made, std::move(giving));
                           return;
                       }

                       // Poza zamkiem, tak samo jak w zegarze: bilet już rozstrzygnięty
                       // zawoła ogniwo od razu, a ono wraca tutaj po ten sam zamek.
                       if (hooked(*made, gone)) gone->woke();
                   },
                   made};
}

}

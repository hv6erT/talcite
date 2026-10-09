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
#include <vector>

#include "talcite/bell.hpp"
#include "talcite/encoding.hpp"
#include "talcite/pool.hpp"

#include "folding.hpp"
#include "fscore.hpp"
#include "storing.hpp"

#ifdef _WIN32

#include <windows.h>

#include <aclapi.h>
#include <shlobj.h>
#include <winnetwk.h>

#else

#include <cerrno>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TALCITE_LINUX_FS)

#include <sys/statfs.h>
#include <sys/syscall.h>

#elif defined(TALCITE_BSD_FS)

#include <sys/param.h>
#include <sys/mount.h>

#if defined(__APPLE__)
#include <cstdio>
#endif

#endif

#endif

namespace talcite::fs {

using namespace core;

namespace core {

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
#ifdef _WIN32

// **Ścieżka za długa dla `std::filesystem`.** Program bez `longPathAware` w manifeście (próby,
// każdy program na bibliotece, który go nie ma) albo system bez LongPathsEnabled: MSVC woła system
// bez przedrostka \\?\, a system za 260 znakami odpowiada „nie ma ścieżki" — bez błędu. `exists`
// i `kind` mówiły więc „nie ma" o pliku, który stoi, a zapis z polityką kolizji brał go za wolne
// miejsce. Dla takich ścieżek pytamy sami, z przedrostkiem; krótszych nie ruszamy. Granica 248 to
// granica katalogu, niższa niż pliku.
DWORD attributes_beyond(const path& where) {
    std::error_code ignored;

    const path whole = where.is_absolute() ? where : stood::absolute(where, ignored);

    if (whole.native().size() < MAX_PATH - 12) return INVALID_FILE_ATTRIBUTES;

    return GetFileAttributesW(longly(where).c_str());
}

#endif

bool exists_at(const path& where) {
    std::error_code why;

    if (stood::exists(where, why)) return true;

#ifdef _WIN32
    if (why) return GetFileAttributesW(longly(where).c_str()) != INVALID_FILE_ATTRIBUTES;

    return attributes_beyond(where) != INVALID_FILE_ATTRIBUTES;
#else
    return false;
#endif
}

Kind kind_at(const path& where) {
    std::error_code trouble;

    const stood::file_status told = stood::symlink_status(where, trouble);

#ifdef _WIN32
    if (trouble || !stood::exists(told)) {
        const DWORD seen = attributes_beyond(where);

        if (seen != INVALID_FILE_ATTRIBUTES) {
            if ((seen & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return Kind::link;

            return (seen & FILE_ATTRIBUTE_DIRECTORY) != 0 ? Kind::folder : Kind::file;
        }
    }
#endif

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
    if (trouble) amiss("Cannot read the file size.", where);

    return told;
}

// Na Windows atrybuty są jednym słowem bitowym. Na POSIX ukrycie jest umową o nazwie —
// kropka na początku — a tylko-do-odczytu odczytujemy z prawa zapisu właściciela. Znacznika
// systemowego POSIX nie ma i udawać go nie będziemy.
Attributes attributes_at(const path& where) {
    Attributes made;

#ifdef _WIN32

    const DWORD told = GetFileAttributesW(longly(where).c_str());
    if (told == INVALID_FILE_ATTRIBUTES) amiss("Cannot read attributes.", where);

    made.hidden = (told & FILE_ATTRIBUTE_HIDDEN) != 0;
    made.readonly = (told & FILE_ATTRIBUTE_READONLY) != 0;
    made.system = (told & FILE_ATTRIBUTE_SYSTEM) != 0;

#else

    std::error_code trouble;

    const stood::file_status told = stood::status(where, trouble);
    if (trouble) amiss("Cannot read attributes.", where);

    const std::string named = where.filename().string();

    made.hidden = named.size() > 1 && named.front() == '.';
    made.readonly = (told.permissions() & stood::perms::owner_write) == stood::perms::none;

#endif

    return made;
}

void attributed_at(const path& where, Attributes what) {
#ifdef _WIN32

    const std::wstring named = longly(where);

    DWORD told = GetFileAttributesW(named.c_str());
    if (told == INVALID_FILE_ATTRIBUTES) amiss("Cannot read attributes.", where);

    const auto sway = [&told](DWORD flag, bool on) {
        if (on) told |= flag;
        else told &= ~flag;
    };

    sway(FILE_ATTRIBUTE_HIDDEN, what.hidden);
    sway(FILE_ATTRIBUTE_READONLY, what.readonly);
    sway(FILE_ATTRIBUTE_SYSTEM, what.system);

    if (told == 0) told = FILE_ATTRIBUTE_NORMAL;

    if (SetFileAttributesW(named.c_str(), told) == 0)
        amiss("Cannot set attributes.", where);

#else

    std::error_code trouble;

    const stood::file_status told = stood::status(where, trouble);
    if (trouble) amiss("Cannot read attributes.", where);

    constexpr stood::perms writing = stood::perms::owner_write | stood::perms::group_write |
                                     stood::perms::others_write;

    stood::perms rights = told.permissions();

    if (what.readonly) rights &= ~writing;
    else rights |= stood::perms::owner_write;

    stood::permissions(where, rights, trouble);
    if (trouble) amiss("Cannot set attributes.", where);

#endif
}

bool equivalent_at(const path& one, const path& two) {
    std::error_code trouble;

    const bool told = stood::equivalent(one, two, trouble);
    if (trouble) amiss("Cannot compare the paths.", one);

    return told;
}

path target_at(const path& where) {
    std::error_code trouble;

    const path told = stood::read_symlink(where, trouble);
    if (trouble) amiss("Not a link.", where);

    return told;
}

void linking_at(const path& where, const path& onto, bool hard) {
    std::error_code trouble;

    if (hard) stood::create_hard_link(onto, where, trouble);
    else if (kind_at(onto) == Kind::folder) stood::create_directory_symlink(onto, where, trouble);
    else stood::create_symlink(onto, where, trouble);

    if (trouble) amiss("Cannot create the link.", where);
}

#ifdef _WIN32

// Czas plikowy Windows liczy się w setkach nanosekund od roku 1601. Do epoki uniksowej jest
// 11 644 473 600 sekund. Przeliczamy wprost, bez okrężnej drogi przez różnicę zegarów —
// tutaj znamy dokładną wartość, a nie tylko odstęp między dwoma „teraz".
constexpr std::int64_t epochs_apart = 11644473600LL * 10000000LL;

Instant instant_of(FILETIME told) {
    const std::uint64_t much = (static_cast<std::uint64_t>(told.dwHighDateTime) << 32) | told.dwLowDateTime;

    return Instant{std::chrono::nanoseconds{(static_cast<std::int64_t>(much) - epochs_apart) * 100}};
}

FILETIME filetime_of(Instant when) {
    const auto ticks = std::chrono::floor<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
        when.time_since_epoch());

    const auto much = static_cast<std::uint64_t>(ticks.count() + epochs_apart);

    FILETIME made{};

    made.dwLowDateTime = static_cast<DWORD>(much & 0xFFFFFFFFull);
    made.dwHighDateTime = static_cast<DWORD>(much >> 32);

    return made;
}

#endif

// Czas zapisu prosto od systemu, bez `std::filesystem`. Tamto szło przez zegar plikowy i odstęp
// między nim a kalendarzowym mierzony przy starcie — i gubiło wszystko poniżej sekundy.
Instant instant_at(const path& where) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA told{};

    if (GetFileAttributesExW(longly(where).c_str(), GetFileExInfoStandard, &told) == 0)
        amiss("Cannot read the write time.", where);

    return instant_of(told.ftLastWriteTime);
#else
    struct stat told {};

    if (::stat(where.c_str(), &told) != 0) amiss("Cannot read the write time.", where);

#ifdef __APPLE__
    const timespec& when = told.st_mtimespec;
#else
    const timespec& when = told.st_mtim;
#endif

    return Instant{std::chrono::seconds{when.tv_sec} + std::chrono::nanoseconds{when.tv_nsec}};
#endif
}

std::chrono::sys_seconds modified_at(const path& where) {
    return std::chrono::floor<std::chrono::seconds>(instant_at(where));
}

Space space_at(const path& where) {
    std::error_code trouble;

    const stood::space_info told = stood::space(where, trouble);
    if (trouble) amiss("Cannot check the free space.", where);

    return Space{told.capacity, told.available};
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
// std::filesystem pod MSVC robi to tylko w programie z `longPathAware` na systemie
// z LongPathsEnabled; nasze własne wywołania systemowe muszą to robić jawnie, a jest ich sporo:
// otwarcie pliku, atrybuty, przejście katalogu i obserwacja (`exists` i `kind` — patrz
// `attributes_beyond`).
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

#ifndef _WIN32

Mount minded(const std::string& kind);

std::string format_at(const path& where);

#endif

path present_at(const path& where);

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

#elif defined(TALCITE_LINUX_FS)

    struct statfs told {};

    const path asked = exists_at(where) ? where : where.parent_path();

    if (statfs(asked.c_str(), &told) != 0) return Mount::other;

    // NFS, SMB (stary i nowy), CIFS, Ceph, Lustre, AFS — sieć; ISO 9660 i UDF — płyta. FUSE to
    // tylko sposób podłączenia, więc o nim mówi nazwa systemu plików z tablicy montowań.
    switch (static_cast<unsigned long>(told.f_type)) {
        case 0x6969ul:
        case 0x517Bul:
        case 0xFF534D42ul:
        case 0xFE534D42ul:
        case 0x00C36400ul:
        case 0x0BD00BD0ul:
        case 0x5346414Ful: return Mount::remote;
        case 0x65735546ul: return minded(format_at(asked));
        case 0x9660ul:
        case 0x15013346ul: return Mount::optical;
        default: return Mount::disk;
    }

#elif defined(TALCITE_BSD_FS)

    // macOS i BSD mówią rodzaj nazwą w samym `statfs` — bez tablicy montowań.
    return minded(format_at(exists_at(where) ? where : where.parent_path()));

#else

    // Przenośny POSIX nie ma jak zapytać o rodzaj nośnika. Istniejąca ścieżka to dysk: obserwacja
    // i tak idzie tu odpytywaniem, a rata dysku jest najszybsza.
    return exists_at(present_at(where)) ? Mount::disk : Mount::other;

#endif
}

// Najbliższa istniejąca ścieżka: cel kopii zwykle jeszcze nie istnieje, a pytamy o jego nośnik.
path present_at(const path& where) {
    std::error_code ignored;

    path made = where.is_absolute() ? where : stood::absolute(where, ignored);

    while (!made.empty() && !exists_at(made)) {
        const path up = made.parent_path();

        if (up == made) break;

        made = up;
    }

    return made;
}

void settled_folder([[maybe_unused]] const path& where) {
#ifndef _WIN32
    const int folder = ::open(where.empty() ? "." : where.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (folder < 0) return;

    // Udział sieciowy i część systemów plików odmawia utrwalenia katalogu; zmiana nazwy i tak się
    // stała, więc odmowa nie jest tu błędem.
    (void)::fsync(folder);
    ::close(folder);
#endif
}

bool fat_at(const path& where) {
    const path here = present_at(where);

#ifdef _WIN32
    std::vector<wchar_t> root(32768, L'\0');

    if (GetVolumePathNameW(longly(here).c_str(), root.data(), static_cast<DWORD>(root.size())) == 0) return false;

    std::wstring format(MAX_PATH + 1, L'\0');

    if (GetVolumeInformationW(root.data(), nullptr, 0, nullptr, nullptr, nullptr, format.data(), MAX_PATH) == 0)
        return false;

    // `FAT` i `FAT32`; `exFAT` tej granicy nie ma.
    return format.rfind(L"FAT", 0) == 0;
#elif defined(TALCITE_LINUX_FS)
    struct statfs told {};

    return statfs(here.c_str(), &told) == 0 && static_cast<unsigned long>(told.f_type) == 0x4d44ul;
#elif defined(TALCITE_BSD_FS)
    // `msdos` na macOS, `msdosfs` na FreeBSD.
    return format_at(here).rfind("msdos", 0) == 0;
#else
    (void)here;

    return false;
#endif
}

bool together_at(const path& one, const path& two) {
    const path left = present_at(one);
    const path right = present_at(two);

#ifdef _WIN32
    std::vector<wchar_t> first(32768, L'\0');
    std::vector<wchar_t> second(32768, L'\0');

    if (GetVolumePathNameW(longly(left).c_str(), first.data(), static_cast<DWORD>(first.size())) == 0 ||
        GetVolumePathNameW(longly(right).c_str(), second.data(), static_cast<DWORD>(second.size())) == 0)
        return false;

    return _wcsicmp(first.data(), second.data()) == 0;
#else
    struct stat here {};
    struct stat there {};

    if (::stat(left.c_str(), &here) != 0 || ::stat(right.c_str(), &there) != 0) return false;

    return here.st_dev == there.st_dev;
#endif
}

std::optional<std::uintmax_t> room_at(const path& where) {
    std::error_code trouble;

    const stood::space_info told = stood::space(present_at(where), trouble);

    if (trouble || told.available == static_cast<std::uintmax_t>(-1)) return std::nullopt;

    return told.available;
}

std::size_t chunk_at(const path& where) {
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

// Rodzaj wpisu prosto z trybu, jak w `stat(2)`: dowiązanie rozpoznajemy przed wszystkim innym, bo
// `lstat` mówi o wpisie, a nie o tym, na co wskazuje.
Kind kinded(::mode_t mode) {
    if (S_ISLNK(mode)) return Kind::link;
    if (S_ISREG(mode)) return Kind::file;
    if (S_ISDIR(mode)) return Kind::folder;
    if (S_ISCHR(mode) || S_ISBLK(mode)) return Kind::device;

    return Kind::special;
}

// **Wszystko z jednego pytania.** `std::filesystem` pyta o to samo w kawałkach — osobno o rodzaj,
// osobno o rozmiar, osobno o prawa, osobno o czas, osobno o istnienie celu — i każdy kawałek idzie
// do systemu osobno: zmierzone `strace`, trzy `stat`-y na wpis (150 wpisów, 454 wywołania), a z
// naszym własnym o prawa i właściciela cztery. Jeden `lstat` daje to wszystko naraz.
//
// Dowiązanie kosztuje drugie pytanie — `stat`, czyli **przez** dowiązanie. Płacimy je za trzy
// rzeczy: czy cel w ogóle jest (`stale`), czym jest (`reach` — po nim panel wie, że w dowiązanie
// do katalogu da się wejść) i jakie ma prawa. Znaczniki mówią bowiem o tym, na co wpis
// wskazuje — `readonly` dowiązania do pliku tylko do odczytu ma być prawdą — a `rights` o samym
// wpisie, więc dowiązanie ma swoje `lrwxrwxrwx`. To rozróżnienie było i zostaje; zmienia się tylko
// to, że oba biorą się z jednego miejsca zamiast z czterech.
//
// Wpis, o który nie da się zapytać, wraca z `Kind::none` — tak samo jak dotąd, gdy pytania
// `std::filesystem` po kolei zawodziły.
Entry noted(const stood::directory_entry& one) {
    Entry made;
    made.where = one.path();

    struct stat told {};

    if (::lstat(one.path().c_str(), &told) != 0) return made;

    made.kind = kinded(told.st_mode);
    made.reach = made.kind;

    if (made.kind == Kind::file) made.size = static_cast<std::uintmax_t>(told.st_size);

    made.modified = std::chrono::sys_seconds{std::chrono::seconds{told.st_mtime}};
    made.accessed = std::chrono::sys_seconds{std::chrono::seconds{told.st_atime}};
    made.rights = static_cast<stood::perms>(told.st_mode & 07777);
    made.links = told.st_nlink;
    made.owner = static_cast<std::uint32_t>(told.st_uid);
    made.group = static_cast<std::uint32_t>(told.st_gid);

    const std::string named = one.path().filename().string();

    made.attributes.hidden = named.size() > 1 && named.front() == '.';

    ::mode_t mode = told.st_mode;

    if (made.kind == Kind::link) {
        std::error_code ignored;

        const path where = stood::read_symlink(one.path(), ignored);

        if (!ignored) made.target = where;

        struct stat through {};

        made.reach = Kind::none;

        if (::stat(one.path().c_str(), &through) == 0) {
            mode = through.st_mode;
            made.reach = kinded(through.st_mode);
        } else {
            made.attributes.stale = true;
        }
    }

    // Bit wykonywania **którykolwiek**, tak liczy MC.
    made.attributes.readonly = (mode & S_IWUSR) == 0;
    made.attributes.executable = (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;

    return made;
}

// Jeden wpis tą samą drogą co w wykazie, bez czytania całego katalogu. Wpisu, którego nie ma,
// nie ma też tutaj: wraca `Kind::none`, jak wpis, o który nie da się zapytać.
Entry entry_at(const path& where) {
    std::error_code ignored;

    return noted(stood::directory_entry{where, ignored});
}

#endif

void sorted(std::vector<Entry>& among) {
    std::sort(among.begin(), among.end(),
              [](const Entry& one, const Entry& two) { return one.where < two.where; });
}

#ifdef _WIN32

std::chrono::sys_seconds stamped(FILETIME told) {
    return std::chrono::floor<std::chrono::seconds>(instant_of(told));
}

// Rozszerzenia, które Windows uruchamia wprost. PATHEXT ma ich więcej, ale te cztery są tym,
// co człowiek nazywa „programem", a reszta (`.vbs`, `.js`, `.msc`) to skrypty pod cudzy silnik.
bool runnable(const path& where) {
    std::wstring ending = where.extension().wstring();

    for (wchar_t& one : ending) one = static_cast<wchar_t>(std::towlower(one));

    return ending == L".exe" || ending == L".com" || ending == L".bat" || ending == L".cmd";
}

// Punkt ponownej analizy to nie zawsze dowiązanie. Rodzaj mówi znacznik, który `FIND_DATA` niesie
// w `dwReserved0`. Dowiązanie symboliczne, węzeł montowania (junction), dowiązanie WSL i alias
// aplikacji ze sklepu — to są dowiązania. Plik OneDrive na żądanie, plik po deduplikacji serwera,
// plik z obrazu WIM — to zwykłe pliki i katalogi, które system czyta przezroczyście; dawniej szły
// jako dowiązania, bez rozmiaru, a biblioteka nie widziała w nich filmów. Gniazdo AF_UNIX jest
// tym, czym na POSIX-ie: czymś szczególnym.
constexpr DWORD wsl_link = 0xA000001DUL;
constexpr DWORD app_alias = 0x8000001BUL;
constexpr DWORD unix_socket = 0x80000023UL;

Kind reparsed(const WIN32_FIND_DATAW& told) {
    const bool folder = (told.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if ((told.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return folder ? Kind::folder : Kind::file;

    switch (told.dwReserved0) {
        case IO_REPARSE_TAG_SYMLINK:
        case IO_REPARSE_TAG_MOUNT_POINT:
        case wsl_link:
        case app_alias: return Kind::link;
        case unix_socket: return Kind::special;
        default: return folder ? Kind::folder : Kind::file;
    }
}

// Jedno pytanie do systemu oddaje nazwę, rodzaj, rozmiar, czas i atrybuty razem.
// directory_iterator nie wystawia atrybutów, więc trzeba było dopytywać o każdy wpis
// z osobna — przy skanowaniu udziału sieciowego to podwajało liczbę wypraw przez sieć.
Entry noted(const WIN32_FIND_DATAW& told, const path& here) {
    Entry made;

    made.where = here / path{told.cFileName};

    made.kind = reparsed(told);

    if (made.kind == Kind::file)
        made.size = (static_cast<std::uintmax_t>(told.nFileSizeHigh) << 32) | told.nFileSizeLow;

    made.modified = stamped(told.ftLastWriteTime);
    made.accessed = stamped(told.ftLastAccessTime);

    made.attributes.hidden = (told.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    made.attributes.readonly = (told.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    made.attributes.system = (told.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;

    // Wykonywalny po rozszerzeniu — Windows nie ma bitu. `stale` zostaje fałszem: sprawdzenie
    // celu punktu ponownej analizy to osobna wyprawa do systemu na każdy wpis, a przy udziale
    // sieciowym podwajałaby liczbę wypraw — powód, dla którego `noted` czyta z `FIND_DATA`.
    made.attributes.executable = made.kind == Kind::file && runnable(made.where);

    // Prawa udajemy tak samo, jak robi to `std::filesystem` na tym systemie: wszystko wolno, chyba
    // że plik jest tylko do odczytu. Liczby dowiązań, właściciela ani grupy `FIND_DATA` nie niesie,
    // a wyciągnięcie ich wymagałoby otwarcia każdego pliku z osobna — MC na Windows też ich nie
    // pokazuje, więc zostają puste.
    made.rights = made.attributes.readonly ? stood::perms::owner_read | stood::perms::group_read |
                                            stood::perms::others_read | stood::perms::owner_exec |
                                            stood::perms::group_exec | stood::perms::others_exec
                                      : stood::perms::all;

    // Cel dowiązania jak na POSIX-ie, tylko dla dowiązania. `read_symlink` czyta dowiązania
    // symboliczne, a nie węzły montowania (junction) — dla nich cel zostaje pusty, tak jak `stale`
    // zostaje fałszem, i z tego samego powodu.
    made.reach = made.kind;

    if (made.kind == Kind::link) {
        std::error_code ignored;

        const path told = stood::read_symlink(made.where, ignored);

        if (!ignored) made.target = told;

        const stood::file_status through = stood::status(made.where, ignored);

        made.reach = ignored ? Kind::none
                     : stood::is_directory(through)    ? Kind::folder
                     : stood::is_regular_file(through) ? Kind::file
                     : stood::exists(through)          ? Kind::special
                                                       : Kind::none;
    }

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

        amiss("Cannot open the directory.", where);
    }

    do {
        const std::wstring named{told.cFileName};

        if (named == L"." || named == L"..") continue;

        if (stopped()) break;

        made.push_back(noted(told, where));
    } while (FindNextFileW(looking, &told) != 0);

    // **Koniec wykazu to tylko `ERROR_NO_MORE_FILES`.** Każdy inny błąd — zerwane połączenie
    // z udziałem w połowie wykazu — dawniej też kończył pętlę, więc wykaz wychodził ucięty jak
    // kompletny, a przeniesienie katalogu kasowało to, czego nie zdążyło zobaczyć.
    const DWORD why = GetLastError();

    FindClose(looking);

    if (why != ERROR_NO_MORE_FILES && !stopped()) amiss("Cannot read the directory.", where);

    return made;
}

// Jeden wpis: `FIND_DATA` samego wpisu, jak w wykazie, a do tego **liczba nazw** z otwartego
// uchwytu. W wykazie jej nie ma, bo kosztowałaby otwarcie każdego pliku; przy jednym wpisie to
// jedno otwarcie, a bez niej zapis „obok i przemianuj" nie wie, że zerwałby twarde dowiązanie.
// Wpisu, którego nie ma, nie ma też tutaj: wraca `Kind::none`.
Entry entry_at(const path& where) {
    WIN32_FIND_DATAW told{};

    const HANDLE looking = FindFirstFileExW(longly(where).c_str(), FindExInfoBasic, &told,
                                            FindExSearchNameMatch, nullptr, 0);

    if (looking == INVALID_HANDLE_VALUE) {
        Entry none;

        none.where = where;

        return none;
    }

    FindClose(looking);

    Entry made = noted(told, where.parent_path());

    const HANDLE opened = CreateFileW(longly(where).c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                      nullptr);

    if (opened != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION about{};

        if (GetFileInformationByHandle(opened, &about) != 0) made.links = about.nNumberOfLinks;

        CloseHandle(opened);
    }

    return made;
}

#endif

// Katalog pod ścieżką albo dowiązanie, które do katalogu prowadzi — **tylko u korzenia**. Panel
// wchodzi przez dowiązanie jak MC, a `kind_at` go nie rozwija, bo wpis dowiązania ma pokazywać
// dowiązanie. Dawniej obie drogi odmawiały: „Not a directory." na katalogu, do którego człowiek
// właśnie chciał wejść.
namespace {

bool folder_at(const path& where) {
    const Kind kind = kind_at(where);

    if (kind == Kind::folder) return true;

    if (kind != Kind::link) return false;

    std::error_code trouble;

    return stood::is_directory(stood::status(where, trouble));
}

}

std::vector<Entry> list_at(const path& where) {
    if (!folder_at(where)) amiss("Not a directory.", where);

#ifdef _WIN32

    std::vector<Entry> made = found_in(where, false);

#else

    std::error_code trouble;
    stood::directory_iterator at{where, stood::directory_options::skip_permission_denied,
                                 trouble};

    if (trouble) amiss("Cannot open the directory.", where);

    std::vector<Entry> made;

    for (const stood::directory_entry& one : at) {
        if (stopped()) return made;

        made.push_back(noted(one));
    }

#endif

    sorted(made);

    return made;
}

// Katalog, do którego nie ma wstępu, pomijamy zamiast przewracać całe przejście. Zabranie
// prawa do jednego podkatalogu nie może unieważnić skanowania biblioteki. Za dowiązaniem idziemy
// tylko u korzenia (`folder_at`); w głębi zostaje wpisem, bo dowiązanie do przodka zamknęłoby koło.
std::vector<Entry> walk_at(const path& where) {
    if (!folder_at(where)) amiss("Not a directory.", where);

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

            Entry note = noted(one);
            if (note.kind == Kind::folder) waiting.push_back(note.where);

            made.push_back(std::move(note));
        }

#endif
    }

    sorted(made);

    return made;
}

void make_folder_at(const path& where) {
    if (kind_at(where) == Kind::folder) return;

    std::error_code trouble;

    stood::create_directories(where, trouble);
    if (trouble) amiss("Cannot create the directory.", where);
}

// Nazwa obok: `cel.bin` → `cel (2).bin`. Składana na postaci rodzimej ścieżki, nie przez
// `string()`, które pod MSVC oddaje stronę kodową ANSI i psuje albo odrzuca polskie litery.
path beside(const path& to, int at) {
    const path::string_type tail = path{" (" + std::to_string(at) + ")"}.native();

    path made = to;
    made.replace_filename(to.stem().native() + tail + to.extension().native());

    return made;
}

path settled_on(const path& to, Conflict how) {
    if (kind_at(to) == Kind::none) return to;

    switch (how) {
        case Conflict::overwrite: return to;
        case Conflict::skip: return {};
        case Conflict::fail: amiss("The target already exists.", to);
        case Conflict::rename: break;
    }

    for (int at = 2; at <= 100; ++at) {
        const path other = beside(to, at);

        if (kind_at(other) == Kind::none) return other;
    }

    amiss("No free name beside the target.", to);
}

namespace {

#if defined(TALCITE_LINUX_FS)

constexpr unsigned noreplace = 1U;

#endif

#ifdef _WIN32

// **Ten sam wpis pod inną pisownią — na Windows.** Ten sam katalog i nazwa równa bez wielkości
// liter: dowiązania twarde w jednym katalogu różnią się nazwą także bez wielkości liter, a w różnych
// katalogach — katalogiem. Porównujemy nazwy, nie spis katalogu: `from` podane inną pisownią niż
// na dysku zostawia w spisie wpis o pisowni `to`, choć to ten sam wpis.
bool respelled(const path& from, const path& to) {
    std::error_code trouble;

    const path one = stood::absolute(from, trouble).lexically_normal();
    const path two = stood::absolute(to, trouble).lexically_normal();

    const std::wstring left = one.filename().wstring();
    const std::wstring right = two.filename().wstring();

    if (CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                             static_cast<int>(right.size()), TRUE) != CSTR_EQUAL)
        return false;

    return one.parent_path() == two.parent_path() || stood::equivalent(one.parent_path(), two.parent_path(), trouble);
}

#endif

// Wynik jednej próby przeniesienia przez system.
enum class Push : std::uint8_t { done, taken, away, refused };

// **Przeniesienie, które nie nadpisuje, niepodzielnie.** Sprawdzenie celu i osobne przeniesienie
// zostawiały okno, w którym cel mógł się pojawić — a `std::filesystem::rename` na POSIX-ie nadpisuje
// plik po cichu, pusty katalog też. Dlatego o „cel stoi" pytamy sam system, w tym samym wywołaniu:
// na Windows `MoveFileExW` bez `MOVEFILE_REPLACE_EXISTING`, na Linuksie `renameat2` z
// `RENAME_NOREPLACE`. System plików, który tej flagi nie zna (`EINVAL`), dostaje dawną drogę —
// sprawdzenie i przeniesienie.
//
// **Odmowa ma powód i od powodu zależy, co dalej.** Zajęty cel rozstrzyga polityka kolizji; inny
// nośnik (`EXDEV`, `ERROR_NOT_SAME_DEVICE`) to jedyny powód, dla którego `move` wolno kopiować
// i sprzątać źródło. Każda inna odmowa — plik otwarty w odtwarzaczu, brak praw, za długa nazwa —
// jest błędem. Dawniej każda kończyła się kopią i kasowaniem źródła, także taka, po której kopia
// lądowała we wnętrzu samego źródła.
Push pushed(const path& from, const path& to) {
#ifdef _WIN32
    // Przemianowanie na drugie dowiązanie twarde tego samego pliku `MoveFileExW` bez nadpisania
    // przyjmuje bez słowa, choć to osobny wpis. Pytamy więc najpierw, czy cel stoi — jedno pytanie
    // o atrybuty — i stojący oddajemy jako zajęty. Do systemu idzie dalej tylko ten sam wpis:
    // przeniesienie na samego siebie i inna pisownia (`heat` → `Heat`), którą `MoveFileExW` robi sam.
    if (GetFileAttributesW(longly(to).c_str()) != INVALID_FILE_ATTRIBUTES && !respelled(from, to))
        return Push::taken;

    if (MoveFileExW(longly(from).c_str(), longly(to).c_str(), 0) != 0) return Push::done;

    switch (GetLastError()) {
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return Push::taken;
        case ERROR_NOT_SAME_DEVICE: return Push::away;
        default: return Push::refused;
    }
#else
#if defined(TALCITE_LINUX_FS)
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), noreplace) == 0) return Push::done;

    if (errno == EEXIST) return Push::taken;
    if (errno == EXDEV) return Push::away;
    if (errno != EINVAL && errno != ENOSYS) return Push::refused;
#else
#if defined(TALCITE_BSD_FS) && defined(__APPLE__)
    if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return Push::done;

    if (errno == EEXIST) return Push::taken;
    if (errno == EXDEV) return Push::away;
    if (errno != ENOTSUP && errno != EINVAL) return Push::refused;
#endif

    // **Bez przemianowania „bez nadpisania" w systemie** — BSD, przenośny POSIX — plik idzie
    // dowiązaniem twardym: `linkat` nie nadpisuje nigdy (`EEXIST`), więc nowa nazwa powstaje
    // niepodzielnie, a stara znika potem. Katalogu tak się nie da, a system plików bez dowiązań
    // twardych (FAT, część FUSE) odmawia — wtedy zostaje sprawdzenie i przemianowanie.
    struct stat told {};

    if (::lstat(from.c_str(), &told) == 0 && !S_ISDIR(told.st_mode)) {
        if (::linkat(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 0) == 0) {
            if (::unlink(from.c_str()) == 0) return Push::done;

            (void)::unlink(to.c_str());

            return Push::refused;
        }

        if (errno == EEXIST) return Push::taken;
        if (errno == EXDEV) return Push::away;
        if (errno != EPERM && errno != ENOTSUP && errno != EOPNOTSUPP && errno != EMLINK && errno != ENOSYS)
            return Push::refused;
    }
#endif

    if (kind_at(to) != Kind::none) return Push::taken;

    std::error_code trouble;

    stood::rename(from, to, trouble);

    if (!trouble) return Push::done;

    return trouble == std::errc::cross_device_link ? Push::away : Push::refused;
#endif
}

// Przeniesienie, które **zastępuje** cel. Zapis obok i przemianowanie na cel to droga każdego
// bezpiecznego zapisu, więc przemianowanie ma być trwałe: Windows przepuszcza je przez dysk
// (`MOVEFILE_WRITE_THROUGH`), a na POSIX-ie utrwalamy katalog, w którym zmieniła się nazwa —
// inaczej po zaniku zasilania katalog może wciąż wskazywać stary plik albo żaden.
#ifdef _WIN32

// **Zastąpienie z semantyką POSIX** (Windows 10 1709+): cel otwarty przez kogoś innym z
// `FILE_SHARE_DELETE` — odtwarzacz, indeksowanie, drugie okno twa — zostaje zastąpiony jak na
// Linuksie, a otwarte uchwyty dalej widzą starą treść. `MoveFileExW` w takiej sytuacji odmawia
// dostępu i zapis kończył się błędem. System albo nośnik bez tej drogi (starszy Windows, FAT,
// część udziałów) odpowiada „nie obsługuję", i wtedy idzie `MoveFileExW` jak dawniej.
std::optional<Push> posix_replaced(const path& from, const path& to) {
    const HANDLE one = CreateFileW(longly(from).c_str(), DELETE | SYNCHRONIZE | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (one == INVALID_HANDLE_VALUE) return std::nullopt;

    const std::wstring named = longly(to);
    const std::size_t room = sizeof(FILE_RENAME_INFO) + named.size() * sizeof(wchar_t);

    std::vector<std::byte> held(room);

    auto* told = reinterpret_cast<FILE_RENAME_INFO*>(held.data());

    told->Flags = 0x00000001 | 0x00000002;
    told->RootDirectory = nullptr;
    told->FileNameLength = static_cast<DWORD>(named.size() * sizeof(wchar_t));

    std::memcpy(told->FileName, named.c_str(), named.size() * sizeof(wchar_t));

    // `FileRenameInfoEx` = 22; flagi to `FILE_RENAME_FLAG_REPLACE_IF_EXISTS` i
    // `FILE_RENAME_FLAG_POSIX_SEMANTICS` — liczbami, bo starsze nagłówki MinGW ich nie mają.
    const BOOL done = SetFileInformationByHandle(one, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22), told,
                                                 static_cast<DWORD>(room));

    const DWORD why = done != 0 ? ERROR_SUCCESS : GetLastError();

    // Utrwalenie tego, co zrobiło `MOVEFILE_WRITE_THROUGH`: wpis o nowej nazwie na nośniku.
    if (done != 0) (void)FlushFileBuffers(one);

    CloseHandle(one);

    if (done != 0) return Push::done;

    if (why == ERROR_NOT_SAME_DEVICE) return Push::away;

    if (why == ERROR_INVALID_PARAMETER || why == ERROR_NOT_SUPPORTED || why == ERROR_INVALID_FUNCTION)
        return std::nullopt;

    return Push::refused;
}

#endif

Push replaced(const path& from, const path& to) {
#ifdef _WIN32
    if (const std::optional<Push> made = posix_replaced(from, to)) return *made;

    if (MoveFileExW(longly(from).c_str(), longly(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return Push::done;

    return GetLastError() == ERROR_NOT_SAME_DEVICE ? Push::away : Push::refused;
#else
    if (::rename(from.c_str(), to.c_str()) != 0) return errno == EXDEV ? Push::away : Push::refused;

    settled_folder(to.parent_path());

    return Push::done;
#endif
}

// Wpis nie zmieści się we własnym wnętrzu. Wystarczy porównanie ścieżek bezwzględnych: przodka
// schowanego za dowiązaniem i tak odrzuci sam system (`EINVAL`), a odmowa systemu jest dziś błędem,
// nie zaproszeniem do kopii. Tu chodzi o zdanie, które mówi, co się stało. O rodzaj wpisu nie
// pytamy: plik też nie ma wnętrza, a pytanie kosztowałoby zajrzenie do każdego przenoszonego pliku.
bool within(const path& from, const path& to) {
    std::error_code ignored;

    const path whole = stood::absolute(from, ignored).lexically_normal();
    const path aim = stood::absolute(to, ignored).lexically_normal();

    const auto [left, right] = std::mismatch(whole.begin(), whole.end(), aim.begin(), aim.end());

    return left == whole.end() && right != aim.end();
}

#ifndef _WIN32

// **Ten sam wpis pod inną pisownią.** Na systemie bez wielkości liter `heat` i `Heat` to jeden
// wpis, więc cel „stoi" — ale zmiana nazwy między nimi jest zwyczajną zmianą nazwy. Dowiązanie
// twarde też jest `equivalent`, a jest osobnym wpisem, którego nadpisać nie wolno. Rozróżnia je
// katalog: osobny wpis leży w nim pod dokładnie tą nazwą, inna pisownia tego samego — nie.
bool same_entry(const path& from, const path& to) {
    std::error_code trouble;

    if (!stood::equivalent(from, to, trouble) || trouble) return false;

    // Ta sama ścieżka to ten sam wpis. Sama nazwa nie wystarcza: dowiązanie twarde w innym
    // katalogu pod tą samą nazwą to osobny wpis, a `rename` między dowiązaniami nic nie robi.
    if (stood::absolute(from, trouble).lexically_normal() == stood::absolute(to, trouble).lexically_normal())
        return true;

    for (const stood::directory_entry& one : stood::directory_iterator{to.parent_path(), trouble})
        if (one.path().filename() == to.filename()) return false;

    return !trouble;
}

#endif

}

bool recased(const path& from, const path& to) {
    const std::string one = to_string(from.filename());
    const std::string two = to_string(to.filename());

    if (one == two) return false;

    std::size_t left = 0;
    std::size_t right = 0;

    while (left < one.size() && right < two.size())
        if (ui::folded(utf8::decode(one, left)) != ui::folded(utf8::decode(two, right))) return false;

    if (left != one.size() || right != two.size()) return false;

    std::error_code trouble;

    return stood::absolute(from, trouble).lexically_normal().parent_path() ==
           stood::absolute(to, trouble).lexically_normal().parent_path();
}

bool respelled_twice(const path& from, const path& to, const std::function<bool(const path&, const path&)>& move,
                     const std::function<bool(const path&)>& exact) {
    if (exact(to)) return true;

    path aside = to;

    aside += ".~case";

    if (!move(from, aside)) return false;

    if (move(aside, to)) return true;

    (void)move(aside, from);

    return false;
}

namespace {

// Spis katalogu ma wpis dokładnie pod tą nazwą — nie inną pisownią tego samego.
bool spelled_exactly(const path& to) {
    std::error_code trouble;

    const path folder = to.has_parent_path() ? to.parent_path() : path{"."};

    for (const stood::directory_entry& one : stood::directory_iterator{folder, trouble})
        if (one.path().filename() == to.filename()) return true;

    return false;
}

// Po udanej zmianie samej wielkości liter: czy naprawdę się stała.
Shift confirmed(const path& from, const path& to, Shift made) {
    if (!made.done || !recased(from, to)) return made;

    const auto move = [](const path& one, const path& two) { return pushed(one, two) == Push::done; };

    if (!respelled_twice(from, to, move, spelled_exactly)) amiss("Cannot rename.", from);

    return made;
}

}

Shift shifted(const path& from, const path& to, Conflict how) {
    if (within(from, to)) amiss("Cannot move an entry into itself.", from);

    if (how == Conflict::overwrite) {
        switch (replaced(from, to)) {
            case Push::done: return {to, true};
            case Push::away: return {to, false};
            default: amiss("Cannot rename.", from);
        }
    }

    const auto settled = [&from](const path& aim) -> std::optional<Shift> {
        switch (pushed(from, aim)) {
            case Push::done: return Shift{aim, true};
            case Push::away: return Shift{aim, false};
            case Push::taken: return std::nullopt;
            case Push::refused: break;
        }

        amiss("Cannot rename.", from);
    };

    if (const std::optional<Shift> made = settled(to)) return confirmed(from, to, *made);

#ifndef _WIN32
    // Na Windows ten sam wpis rozstrzyga już `pushed` — tu dochodzi tylko cel naprawdę zajęty.
    if (same_entry(from, to)) {
        std::error_code trouble;

        stood::rename(from, to, trouble);
        if (trouble) amiss("Cannot rename.", from);

        return confirmed(from, to, {to, true});
    }
#endif

    switch (how) {
        case Conflict::skip: return {};
        case Conflict::fail: amiss("The target already exists.", to);
        case Conflict::overwrite:
        case Conflict::rename: break;
    }

    for (int at = 2; at <= 100; ++at)
        if (const std::optional<Shift> made = settled(beside(to, at))) return *made;

    amiss("No free name beside the target.", to);
}

bool replaced_at(const path& from, const path& to) { return replaced(from, to) == Push::done; }

}

// Ścieżka jako tekst **zawsze w UTF-8** i z powrotem. Bez tego programy stojące na bibliotece
// sięgają po `path::string()`, a to pod MSVC oddaje bieżącą stronę kodową ANSI, nie UTF-8 —
// nazwa pliku z polskimi znakami dochodzi do rysowania już zepsuta i wygląda na ekranie jak
// ciąg znaków zastępczych, bo dekoder biblioteki odrzuca bajt po bajcie. To samo w drugą stronę:
// `path{std::string}` czyta bajty jako ANSI, więc katalog założony z pola tekstowego dostaje
// inną nazwę, niż wpisał człowiek.
//
// `u8string()` jest UTF-8 z definicji na każdym systemie, więc **nie ma tu ani jednego `#ifdef`**.
// Przejście przez `std::u8string` to przepisanie bajt w bajt; osobny typ istnieje właśnie po to,
// żeby kodowania nie dało się pomylić, i dlatego nie da się go po prostu przypisać.
//
// Wyjątek od tej reguły jest jeden: Windows. Tam `u8string()` przy samotnej połowie pary
// zastępczej w nazwie (NTFS ją dopuszcza) rzuca albo gubi znaki, zależnie od biblioteki
// standardowej — a nazwy pliku do pokazania nie wolno wywracać. Dlatego na Windows przepisujemy
// sami, ze znakiem zastępczym w miejscu zepsutej jednostki. Tekst do przechowania, bez strat,
// daje `stored`.
std::string to_string(const path& where) {
#ifdef _WIN32
    return ::talcite::to_string(std::wstring_view{where.native()});
#else
    const std::u8string said = where.u8string();

    return std::string{reinterpret_cast<const char*>(said.data()), said.size()};
#endif
}

path from_string(std::string_view said) {
    return path{std::u8string{reinterpret_cast<const char8_t*>(said.data()), said.size()}};
}

// Ścieżka do JSON-u i z powrotem: zawsze poprawny UTF-8, zawsze ta sama ścieżka — opis
// w `storing.cpp`. `to_string` jest dla ludzi, `stored` dla plików.
std::string stored(const path& where) {
#ifdef _WIN32
    const std::wstring& native = where.native();

    return stored_units(std::u16string_view{reinterpret_cast<const char16_t*>(native.data()), native.size()});
#else
    return stored_bytes(where.native());
#endif
}

path restored(std::string_view said) {
#ifdef _WIN32
    const std::u16string made = restored_units(said);

    return path{std::wstring{reinterpret_cast<const wchar_t*>(made.data()), made.size()}};
#else
    return path{restored_bytes(said)};
#endif
}

namespace core {

// Udział sieciowy poznajemy tym samym rozpoznaniem, które dobiera ratę — krótsza rata
// i brak wiarygodnych powiadomień biorą się z tej samej przyczyny.
[[maybe_unused]] bool afar_at(const path& where) { return chunk_at(where) == afar; }

#ifdef _WIN32

// Litery z `GetLogicalDrives`, bez pytania samych nośników: pytanie o etykietę i miejsce potrafi
// czekać sekundami na odłączony dysk sieciowy albo pusty czytnik kart, a wykaz ma stanąć od razu.
// Szczegóły każdej litery bierze `volume_at`, osobno — wolny nośnik spowalnia wtedy tylko siebie.
std::vector<path> roots_at() {
    std::vector<path> made;

    const DWORD mask = GetLogicalDrives();

    for (int at = 0; at < 26; ++at)
        if ((mask & (1ul << at)) != 0) made.emplace_back(std::wstring{static_cast<wchar_t>(L'A' + at)} + L":\\");

    return made;
}

// Jeden nośnik: litera albo udział `\\serwer\zasób`. Korzeń dostaje ukośnik wsteczny na końcu, bo
// bez niego Windows czyta `C:` jako katalog bieżący na tym dysku, a udziału nie rozpoznaje wcale.
Volume volume_at(const path& where) {
    Volume one;

    std::wstring root = where.wstring();

    std::replace(root.begin(), root.end(), L'/', L'\\');

    if (!root.empty() && root.back() != L'\\') root += L'\\';

    one.where = path{root};
    one.mount = mount_at(one.where);

    std::wstring label(MAX_PATH + 1, L'\0');
    std::wstring format(MAX_PATH + 1, L'\0');

    if (GetVolumeInformationW(root.c_str(), label.data(), MAX_PATH, nullptr, nullptr, nullptr, format.data(),
                              MAX_PATH) != 0) {
        label.resize(std::wcslen(label.c_str()));
        format.resize(std::wcslen(format.c_str()));

        // Przez `to_string`, nie `string()`: etykieta spoza strony kodowej ANSI rzucała
        // i zabierała cały wykaz nośników.
        one.label = to_string(path{label});
        one.format = to_string(path{format});
    }

    std::error_code ignored;

    const stood::space_info told = stood::space(one.where, ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    // Dysk mapowany pokazuje się jako litera, ale panel powinien wiedzieć, co za nią stoi. Udział
    // podany wprost jest swoim własnym źródłem.
    if (one.mount == Mount::remote) {
        if (root.size() == 3 && root[1] == L':') {
            std::wstring behind(1024, L'\0');
            DWORD much = static_cast<DWORD>(behind.size());

            const std::wstring named = root.substr(0, 2);

            if (WNetGetConnectionW(named.c_str(), behind.data(), &much) == NO_ERROR) {
                behind.resize(std::wcslen(behind.c_str()));
                one.source = path{behind};
            }
        } else {
            one.source = path{root.substr(0, root.size() - 1)};
        }
    }

    return one;
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

// FUSE bywa dyskiem: `fuseblk` (ntfs-3g, exfat-fuse na dysku USB), mergerfs i bindfs składające
// lokalne katalogi, szyfrujące nakładki na lokalny katalog. Dawniej każdy `fuse.*` szedł jako sieć —
// a z tym mniejsza rata i odpytywanie zamiast powiadomień na zwykłym dysku z filmami. Nieznany
// `fuse.*` zostaje siecią: odpytywanie widzi wszystko, tylko wolniej.
bool fused_locally(const std::string& kind) {
    static const std::vector<std::string> among{
        "fuseblk",       "fuse.mergerfs", "fuse.bindfs", "fuse.ntfs-3g",          "fuse.exfat",
        "fuse.encfs",    "fuse.gocryptfs", "fuse.cryfs", "fuse.fuse-overlayfs",   "fuse.unionfs",
        "fuse.squashfuse", "fuse.securefs"};

    return std::find(among.begin(), among.end(), kind) != among.end();
}

Mount minded(const std::string& kind) {
    if (fused_locally(kind)) return Mount::disk;

    // Nazwy z Linuksa i z `statfs` macOS i BSD: `afpfs`, `webdav`, `macfuse` po stronie sieci,
    // `cd9660` i `cddafs` po stronie płyt.
    if (kind.rfind("nfs", 0) == 0 || kind == "cifs" || kind == "smbfs" || kind == "smb3" || kind == "ceph" ||
        kind == "lustre" || kind.rfind("fuse", 0) == 0 || kind == "afs" || kind == "9p" || kind == "afpfs" ||
        kind == "webdav" || kind == "macfuse" || kind == "osxfuse")
        return Mount::remote;

    if (kind == "iso9660" || kind == "udf" || kind == "cd9660" || kind == "cddafs") return Mount::optical;

    return Mount::disk;
}

#if defined(TALCITE_LINUX_FS)

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

// Nazwa systemu plików pod ścieżką: najdłuższy punkt montowania z tablicy, który jest jej przodkiem.
std::string format_at(const path& where) {
    std::error_code ignored;

    const path here = stood::weakly_canonical(where, ignored);

    std::ifstream reading{"/proc/mounts"};
    if (!reading) return {};

    std::string made;
    std::size_t deepest = 0;
    std::string line;

    while (std::getline(reading, line)) {
        std::istringstream taking{line};

        std::string device;
        std::string mounted;
        std::string kind;

        if (!(taking >> device >> mounted >> kind)) continue;

        const path at{plainly(mounted)};

        const auto [left, right] = std::mismatch(at.begin(), at.end(), here.begin(), here.end());

        if (left != at.end()) continue;

        const auto depth = static_cast<std::size_t>(std::distance(at.begin(), at.end()));

        // Późniejszy wpis na tym samym miejscu przykrywa wcześniejszy, więc remis wygrywa ostatni.
        if (depth >= deepest) {
            deepest = depth;
            made = kind;
        }
    }

    return made;
}

// Tablica montowań bez wnętrzności systemu. Ten sam punkt montowania może wystąpić kilka razy —
// liczy się ostatni, bo to on przykrywa poprzednie.
struct Mounted {
    std::string device;
    path where;
    std::string kind;
};

std::vector<Mounted> mounted_at() {
    std::vector<Mounted> made;

    std::ifstream reading{"/proc/mounts"};
    if (!reading) reading.open("/etc/mtab");
    if (!reading) return made;

    std::string line;

    while (std::getline(reading, line)) {
        std::istringstream taking{line};

        std::string device;
        std::string where;
        std::string kind;

        if (!(taking >> device >> where >> kind)) continue;

        const std::string here = plainly(where);

        if (phony(kind) || tucked(here)) continue;

        Mounted one{plainly(device), path{here}, kind};

        const auto stale = std::find_if(made.begin(), made.end(),
                                        [&one](const Mounted& mine) { return mine.where == one.where; });

        if (stale != made.end()) *stale = std::move(one);
        else made.push_back(std::move(one));
    }

    std::sort(made.begin(), made.end(), [](const Mounted& one, const Mounted& two) { return one.where < two.where; });

    return made;
}

// Same punkty montowania — z tablicy, bez pytania nośników. Martwy udział NFS potrafi zawiesić
// pytanie o miejsce na zawsze; `volume_at` pyta każdy osobno, więc wisi tylko on.
std::vector<path> roots_at() {
    std::vector<path> made;

    for (const Mounted& one : mounted_at()) made.push_back(one.where);

    return made;
}

// Nośnik pod punktem montowania. Ścieżka, która punktem montowania nie jest, dostaje to, co da się
// o niej powiedzieć bez tablicy: rodzaj i system plików nośnika, na którym leży, i własną nazwę.
Volume volume_at(const path& where) {
    Volume one;

    one.where = where;

    const std::vector<Mounted> table = mounted_at();

    const auto found =
        std::find_if(table.begin(), table.end(), [&where](const Mounted& mine) { return mine.where == where; });

    if (found != table.end()) {
        one.mount = minded(found->kind);
        one.label = found->device;
        one.format = found->kind;

        std::error_code ignored;

        const path onto = stood::canonical(one.label, ignored);

        if (!ignored) {
            const std::map<path, std::string> labels = labelled();

            const auto named = labels.find(onto);
            if (named != labels.end()) one.label = named->second;
        }

        if (one.mount == Mount::remote) one.source = path{found->device};
    } else {
        one.mount = mount_at(where);
        one.format = format_at(where);
        one.label = where.has_filename() ? where.filename().string() : where.string();
    }

    std::error_code ignored;

    const stood::space_info told = stood::space(one.where, ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    return one;
}

#elif defined(TALCITE_BSD_FS)

// Rodzaj systemu plików wprost od `statfs`: `apfs`, `msdos`, `smbfs`, `nfs`, `cd9660`.
std::string format_at(const path& where) {
    struct statfs told {};

    if (::statfs(present_at(where).c_str(), &told) != 0) return {};

    return std::string{told.f_fstypename};
}

// Wykaz nośników z `getmntinfo`: punkt montowania, źródło i rodzaj. Etykiet nie ma — macOS nazywa
// punkt montowania etykietą, więc zostaje nazwa katalogu.
std::vector<Volume> mounted_at() {
    std::vector<Volume> made;

    struct statfs* among = nullptr;

    const int many = ::getmntinfo(&among, MNT_NOWAIT);

    for (int at = 0; at < many; ++at) {
        const std::string kind{among[at].f_fstypename};
        const std::string here{among[at].f_mntonname};

        if (phony(kind) || kind == "devfs" || kind == "fdescfs" || kind == "procfs" || tucked(here)) continue;

        Volume one;

        one.where = path{here};
        one.mount = minded(kind);
        one.format = kind;
        one.label = one.where == path{"/"} ? std::string{"/"} : one.where.filename().string();

        if (one.mount == Mount::remote) one.source = path{std::string{among[at].f_mntfromname}};

        one.space.total = static_cast<std::uintmax_t>(among[at].f_blocks) * among[at].f_bsize;
        one.space.free = static_cast<std::uintmax_t>(among[at].f_bavail) * among[at].f_bsize;

        made.push_back(std::move(one));
    }

    std::sort(made.begin(), made.end(),
              [](const Volume& one, const Volume& two) { return one.where < two.where; });

    return made;
}

// `MNT_NOWAIT` oddaje to, co jądro już wie, bez pytania nośników — więc wykaz jest od razu.
std::vector<path> roots_at() {
    std::vector<path> made;

    for (const Volume& one : mounted_at()) made.push_back(one.where);

    return made;
}

Volume volume_at(const path& where) {
    for (Volume& one : mounted_at())
        if (one.where == where) return std::move(one);

    Volume one;

    one.where = where;
    one.mount = mount_at(where);
    one.format = format_at(where);
    one.label = where.has_filename() ? where.filename().string() : where.string();

    std::error_code ignored;

    const stood::space_info told = stood::space(one.where, ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    return one;
}

#else

// Przenośny POSIX: rodzaju nie zna nikt, a wykaz nośników to sam korzeń.
std::string format_at(const path&) { return {}; }

std::vector<path> roots_at() { return {path{"/"}}; }

Volume volume_at(const path& where) {
    Volume one;

    one.where = where;
    one.label = where.has_filename() ? where.filename().string() : where.string();
    one.mount = mount_at(where);

    std::error_code ignored;

    const stood::space_info told = stood::space(one.where, ignored);

    if (!ignored) {
        one.space.total = told.capacity;
        one.space.free = told.available;
    }

    return one;
}

#endif

#endif

// Wykaz nośników to korzenie i nośnik każdego z nich — ta sama droga na każdym systemie.
std::vector<Volume> volumes_at() {
    std::vector<Volume> made;

    for (const path& one : roots_at()) made.push_back(volume_at(one));

    return made;
}

// **Nośnik, na którym leży ścieżka** — jego korzeń, a nie sama ścieżka: `volume_at` mówi o punkcie
// montowania albo literze dysku, a Info w twa pyta o plik. Windows wie to sam
// (`GetVolumePathNameW`, także dla udziału i katalogu zamontowanego w NTFS); gdzie indziej —
// najdłuższy korzeń z tablicy montowań, który jest początkiem ścieżki. Ścieżka spoza wszystkich
// korzeni (nie powinno się zdarzyć, ale tablica bywa niepełna) dostaje własny korzeń.
path mounting_at(const path& where) {
#ifdef _WIN32
    std::wstring room(32768, L'\0');

    if (GetVolumePathNameW(longly(where).c_str(), room.data(), static_cast<DWORD>(room.size())) != 0) {
        room.resize(std::wcslen(room.c_str()));

        // Długi przedrostek `\\?\` od `longly` wraca w odpowiedzi — korzeń ma być taki, jak w wykazie.
        if (room.rfind(L"\\\\?\\UNC\\", 0) == 0) room = L"\\\\" + room.substr(8);
        else if (room.rfind(L"\\\\?\\", 0) == 0) room = room.substr(4);

        return path{room};
    }

    return where.root_path();
#else
    std::error_code ignored;

    path whole = stood::weakly_canonical(where, ignored);

    if (ignored) whole = where;

    path best = whole.root_path();
    std::size_t longest = 0;

    for (const path& root : roots_at()) {
        std::size_t parts = 0;
        auto mine = whole.begin();
        bool under = true;

        for (const path& piece : root) {
            if (piece.empty()) continue;

            if (mine == whole.end() || *mine != piece) {
                under = false;

                break;
            }

            ++mine;
            ++parts;
        }

        if (under && parts > longest) {
            longest = parts;
            best = root;
        }
    }

    return best;
#endif
}

// Przez uchwyt, także dla katalogu: `std::filesystem` z MinGW otwierał katalog bez
// `FILE_FLAG_BACKUP_SEMANTICS` i czasu katalogu nie ustawiał wcale.
void stamp_at(const path& where, Instant when) {
#ifdef _WIN32
    const HANDLE one = CreateFileW(longly(where).c_str(), FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (one == INVALID_HANDLE_VALUE) amiss("Cannot set the write time.", where);

    const FILETIME told = filetime_of(when);
    const BOOL done = SetFileTime(one, nullptr, nullptr, &told);

    CloseHandle(one);

    if (done == 0) amiss("Cannot set the write time.", where);
#else
    const auto whole = std::chrono::floor<std::chrono::seconds>(when);

    const timespec told[2] = {{0, UTIME_OMIT},
                              {static_cast<time_t>(whole.time_since_epoch().count()),
                               static_cast<long>((when - whole).count())}};

    if (::utimensat(AT_FDCWD, where.c_str(), told, 0) != 0) amiss("Cannot set the write time.", where);
#endif
}

path canonical_at(const path& where) {
    std::error_code trouble;

    const path told = stood::weakly_canonical(where, trouble);

    return trouble ? where : told;
}

}

Trouble::Trouble(const std::string& said, path where)
    : talcite::Trouble(said), where_(std::move(where)) {}

Promise<bool> exists(const path& where) {
    return Promise<bool>{[where](auto resolve, auto) { resolve(exists_at(where)); }};
}

Promise<Kind> kind(const path& where) {
    return Promise<Kind>{[where](auto resolve, auto) { resolve(kind_at(where)); }};
}

Promise<std::uintmax_t> size(const path& where) {
    return Promise<std::uintmax_t>{[where](auto resolve, auto) { resolve(size_at(where)); }};
}

Promise<std::chrono::sys_seconds> modified(const path& where) {
    return Promise<std::chrono::sys_seconds>{
        [where](auto resolve, auto) { resolve(modified_at(where)); }};
}

Promise<void> modified(const path& where, std::chrono::sys_seconds when) {
    return Promise<void>{[where, when](auto resolve, auto) {
        stamp_at(where, Instant{when});
        resolve();
    }};
}

Promise<Space> space(const path& where) {
    return Promise<Space>{[where](auto resolve, auto) { resolve(space_at(where)); }};
}

Promise<Mount> mount(const path& where) {
    return Promise<Mount>{[where](auto resolve, auto) { resolve(mount_at(where)); }};
}

Promise<std::vector<Volume>> volumes() {
    return Promise<std::vector<Volume>>{[](auto resolve, auto) { resolve(volumes_at()); }};
}

Promise<std::vector<path>> roots() {
    return Promise<std::vector<path>>{[](auto resolve, auto) { resolve(roots_at()); }};
}

Promise<path> mounted(const path& where) {
    return Promise<path>{[where](auto resolve, auto) { resolve(mounting_at(where)); }};
}

Promise<Volume> volume(const path& where) {
    return Promise<Volume>{[where](auto resolve, auto) { resolve(volume_at(where)); }};
}

Promise<std::size_t> chunk(const path& where) {
    return Promise<std::size_t>{[where](auto resolve, auto) { resolve(chunk_at(where)); }};
}

namespace {

// Nazwy właścicieli i grup pytamy systemu **raz na numer**: odpowiedź jest ta sama przez cały
// przebieg programu, a katalog potrafi mieć tysiąc plików jednego właściciela. Zamek, bo pytanie
// pada z puli wątków.
std::string cached(std::map<std::uint32_t, std::string>& among, std::mutex& guard,
                   std::uint32_t which, const std::function<std::string(std::uint32_t)>& ask) {
    {
        const std::lock_guard<std::mutex> held{guard};

        const auto found = among.find(which);

        if (found != among.end()) return found->second;
    }

    std::string made = ask(which);

    const std::lock_guard<std::mutex> held{guard};

    among.insert_or_assign(which, made);

    return made;
}

std::string owner_at(std::uint32_t which) {
#ifdef _WIN32
    (void)which;

    return {};
#else
    std::vector<char> room(1024);

    ::passwd told{};
    ::passwd* found = nullptr;

    while (::getpwuid_r(static_cast<::uid_t>(which), &told, room.data(), room.size(), &found) ==
           ERANGE)
        room.resize(room.size() * 2);

    return found != nullptr && found->pw_name != nullptr ? std::string{found->pw_name}
                                                         : std::string{};
#endif
}

std::string group_at(std::uint32_t which) {
#ifdef _WIN32
    (void)which;

    return {};
#else
    std::vector<char> room(1024);

    ::group told{};
    ::group* found = nullptr;

    while (::getgrgid_r(static_cast<::gid_t>(which), &told, room.data(), room.size(), &found) ==
           ERANGE)
        room.resize(room.size() * 2);

    return found != nullptr && found->gr_name != nullptr ? std::string{found->gr_name}
                                                         : std::string{};
#endif
}

}

// Nazwa właściciela i grupy po numerze — obietnicą, jak wszystko w `fs`, bo na maszynie z nazwami
// z sieci (LDAP, NIS) pierwsze pytanie o dany numer potrafi się zaciąć. Kto rysuje wykaz, pyta raz
// na listing, nie raz na wiersz. Numer, którego system nie zna, oddaje pusty napis: pokazujący
// wypisze wtedy sam numer, jak `ls`.
Promise<std::string> owner(std::uint32_t which) {
    return Promise<std::string>{[which](auto resolve, auto) {
        static std::map<std::uint32_t, std::string> among;
        static std::mutex guard;

        resolve(cached(among, guard, which, owner_at));
    }};
}

Promise<std::string> group(std::uint32_t which) {
    return Promise<std::string>{[which](auto resolve, auto) {
        static std::map<std::uint32_t, std::string> among;
        static std::mutex guard;

        resolve(cached(among, guard, which, group_at));
    }};
}

Promise<Attributes> attributes(const path& where) {
    return Promise<Attributes>{[where](auto resolve, auto) { resolve(attributes_at(where)); }};
}

Promise<void> attributes(const path& where, Attributes what) {
    return Promise<void>{[where, what](auto resolve, auto) {
        attributed_at(where, what);
        resolve();
    }};
}

Promise<Entry> entry(const path& where) {
    return Promise<Entry>{[where](auto resolve, auto) { resolve(entry_at(where)); }};
}

#ifdef _WIN32

namespace {

// Na Windows prawa to **lista dostępu**, a nie bity: `std::filesystem` przekłada bity tylko na
// atrybut „tylko do odczytu", który niczego nie chroni. Przekład jest więc nasz, i jest wąski,
// bo bity grupy i innych nie mają na Windows odpowiednika jeden do jednego:
//
// - **bez praw grupy i innych** — lista chroniona (bez dziedziczenia po katalogu) z jednym
//   wpisem: pełny dostęp dla użytkownika, który uruchomił program. Tak leży plik z kluczami;
// - **z jakimkolwiek prawem grupy albo innych** — lista wraca do dziedziczonej po katalogu, czyli
//   do tego, co Windows daje każdemu nowemu plikowi.
void guarded(const path& where, stood::perms what) {
    const std::wstring named = longly(where);

    constexpr stood::perms shared = stood::perms::group_all | stood::perms::others_all;

    if ((what & shared) != stood::perms::none) {
        // Lista już dziedziczona nie ma czego zdejmować. Ustawiona na nowo straciłaby wpisy dopisane
        // ręką — komuś dane prawo do odczytu — a `Rewrite` w twa woła to przy każdym zapisie obok.
        PSECURITY_DESCRIPTOR held = nullptr;
        PACL listed = nullptr;

        if (GetNamedSecurityInfoW(named.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &listed,
                                  nullptr, &held) == ERROR_SUCCESS) {
            SECURITY_DESCRIPTOR_CONTROL control = 0;
            DWORD revision = 0;

            const bool inherited = GetSecurityDescriptorControl(held, &control, &revision) != 0 &&
                                   (control & SE_DACL_PROTECTED) == 0;

            LocalFree(held);

            if (inherited) return;
        }

        ACL empty{};

        if (!InitializeAcl(&empty, sizeof empty, ACL_REVISION)) amiss("Cannot change the rights.", where);

        if (SetNamedSecurityInfoW(const_cast<LPWSTR>(named.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                  nullptr, &empty, nullptr) != ERROR_SUCCESS)
            amiss("Cannot change the rights.", where);

        return;
    }

    HANDLE token = nullptr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) amiss("Cannot change the rights.", where);

    DWORD size = 0;

    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);

    std::vector<std::byte> user(size);

    const bool known = GetTokenInformation(token, TokenUser, user.data(), size, &size) != 0;

    CloseHandle(token);

    if (!known) amiss("Cannot change the rights.", where);

    const PSID me = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;

    const DWORD room = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(me);

    std::vector<std::byte> listed(room);

    const auto list = reinterpret_cast<PACL>(listed.data());

    if (!InitializeAcl(list, room, ACL_REVISION) || !AddAccessAllowedAce(list, ACL_REVISION, FILE_ALL_ACCESS, me))
        amiss("Cannot change the rights.", where);

    if (SetNamedSecurityInfoW(const_cast<LPWSTR>(named.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, list,
                              nullptr) != ERROR_SUCCESS)
        amiss("Cannot change the rights.", where);
}

}

#endif

// Prawa idą **za dowiązaniem**, jak `chmod`: dowiązanie symboliczne praw nie ma. Na Windows bity
// przekładają się na atrybut „tylko do odczytu" i na listę dostępu (`guarded`).
Promise<void> rights(const path& where, std::filesystem::perms what) {
    return Promise<void>{[where, what](auto resolve, auto) {
        std::error_code why;

        stood::permissions(where, what, stood::perm_options::replace, why);

        if (why) amiss("Cannot change the rights.", where);

#ifdef _WIN32
        guarded(where, what);
#endif

        resolve();
    }};
}

Promise<bool> equivalent(const path& one, const path& two) {
    return Promise<bool>{[one, two](auto resolve, auto) { resolve(equivalent_at(one, two)); }};
}

Promise<path> target(const path& where) {
    return Promise<path>{[where](auto resolve, auto) { resolve(target_at(where)); }};
}

Promise<path> canonical(const path& where) {
    return Promise<path>{[where](auto resolve, auto) { resolve(canonical_at(where)); }};
}

Promise<void> link(const path& where, const path& onto, bool hard) {
    return Promise<void>{[where, onto, hard](auto resolve, auto) {
        linking_at(where, onto, hard);
        resolve();
    }};
}

// Katalog bieżący jest wspólny dla całego procesu, więc jego zmiana dotyka też wątków puli.
// Stąd osobna obietnica: zmiana jest zdarzeniem, odczyt tylko zajrzeniem.
path current() {
    std::error_code ignored;

    return stood::current_path(ignored);
}

Promise<void> current(const path& where) {
    return Promise<void>{[where](auto resolve, auto) {
        std::error_code trouble;

        stood::current_path(where, trouble);
        if (trouble) amiss("Cannot enter the directory.", where);

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

namespace {

#ifdef _WIN32

// Identyfikatory folderów znanych wpisane tu, a nie wzięte z `knownfolders.h`: tam są deklaracjami,
// których definicje MinGW trzyma w osobnej bibliotece uuid. Wartości to stałe Windows — nie
// zmieniają się między wydaniami, bo od nich zależą programy sprzed lat.
GUID known_of(Known which) {
    switch (which) {
        case Known::home: return {0x5e6c858f, 0x0e22, 0x4760, {0x9a, 0xfe, 0xea, 0x33, 0x17, 0xb6, 0x71, 0x73}};
        case Known::desktop: return {0xb4bfcc3a, 0xdb2c, 0x424c, {0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41}};
        case Known::documents: return {0xfdd39ad0, 0x238f, 0x46af, {0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7}};
        case Known::downloads: return {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};
        case Known::music: return {0x4bd8d571, 0x6d19, 0x48d3, {0xbe, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0e, 0x43}};
        case Known::pictures: return {0x33e28130, 0x4e1e, 0x4676, {0x83, 0x5a, 0x98, 0x39, 0x5c, 0x3b, 0xc3, 0xbb}};
        case Known::videos: return {0x18989b1d, 0x99b5, 0x455b, {0x84, 0x1c, 0xab, 0x7c, 0x74, 0xe4, 0xdd, 0xfc}};
    }

    return {};
}

std::optional<path> asked_folder(Known which) {
    PWSTR told = nullptr;

    const GUID id = known_of(which);

    std::optional<path> made;

    if (SHGetKnownFolderPath(id, 0, nullptr, &told) == S_OK && told != nullptr) made = path{told};

    CoTaskMemFree(told);

    return made;
}

#else

// Nazwa zmiennej w `user-dirs.dirs` i katalog, który stoi w domu, gdy pliku nie ma albo wpisu
// w nim brak. macOS nazywa wideo po swojemu.
std::pair<std::string_view, std::string_view> xdg_of(Known which) {
    switch (which) {
        case Known::home: return {"", ""};
        case Known::desktop: return {"XDG_DESKTOP_DIR", "Desktop"};
        case Known::documents: return {"XDG_DOCUMENTS_DIR", "Documents"};
        case Known::downloads: return {"XDG_DOWNLOAD_DIR", "Downloads"};
        case Known::music: return {"XDG_MUSIC_DIR", "Music"};
        case Known::pictures: return {"XDG_PICTURES_DIR", "Pictures"};
#ifdef __APPLE__
        case Known::videos: return {"XDG_VIDEOS_DIR", "Movies"};
#else
        case Known::videos: return {"XDG_VIDEOS_DIR", "Videos"};
#endif
    }

    return {"", ""};
}

// Ścieżka bez ukośników na końcu — `/home/ala/` i `/home/ala` to ten sam katalog.
std::string bared(const path& where) {
    std::string made = where.lexically_normal().string();

    while (made.size() > 1 && made.back() == '/') made.pop_back();

    return made;
}

// Wpis z `user-dirs.dirs` według xdg-user-dirs: `XDG_DOWNLOAD_DIR="$HOME/Pobrane"` — w cudzysłowie,
// ścieżka bezwzględna albo od `$HOME`. Wpis równy samemu domowi znaczy „wyłączony": wtedy folderu nie
// ma i nie szukamy zastępczego. Brak wpisu i wpis krzywy (ścieżka względna) to co innego — o folderze
// nikt nic nie powiedział i zostaje nazwa zwyczajowa. Stąd dwa poziomy: zewnętrzny mówi, czy wpis był.
std::optional<std::optional<path>> configured(std::string_view key, const path& house) {
    const char* given = std::getenv("XDG_CONFIG_HOME");

    const path where = (given != nullptr && *given == '/' ? path{given} : house / ".config") / "user-dirs.dirs";

    std::ifstream reading{where};

    std::string line;

    while (std::getline(reading, line)) {
        if (line.rfind(std::string{key} + "=", 0) != 0) continue;

        std::string told = line.substr(key.size() + 1);

        if (told.size() >= 2 && told.front() == '"' && told.back() == '"') told = told.substr(1, told.size() - 2);

        std::string made;

        if (told.rfind("$HOME", 0) == 0) made = bared(house) + told.substr(5);
        else if (!told.empty() && told.front() == '/') made = told;
        else continue;

        if (bared(path{made}) == bared(house)) return std::optional<path>{};

        return std::optional<path>{path{bared(path{made})}};
    }

    return std::nullopt;
}

std::optional<path> asked_folder(Known which) {
    const path house = home();

    if (house.empty()) return std::nullopt;

    if (which == Known::home) return house;

    const auto [key, fallback] = xdg_of(which);

    if (std::optional<std::optional<path>> made = configured(key, house)) return *made;

    return house / std::string{fallback};
}

#endif

}

// Folder znany tylko wtedy, gdy naprawdę stoi na dysku: wpis w ustawieniach systemu, którego
// katalogu nikt nie założył, nie jest miejscem, do którego da się wejść.
Promise<std::optional<path>> folder(Known which) {
    return Promise<std::optional<path>>{[which](auto resolve, auto) {
        std::optional<path> made = asked_folder(which);

        std::error_code ignored;

        if (made && !stood::is_directory(*made, ignored)) made.reset();

        resolve(std::move(made));
    }};
}

Promise<void> make_folder(const path& where) {
    return Promise<void>{[where](auto resolve, auto) {
        make_folder_at(where);
        resolve();
    }};
}

Promise<void> rename(const path& from, const path& to, Conflict how) {
    return Promise<void>{[from, to, how](auto resolve, auto) {
        const Shift made = shifted(from, to, how);

        if (!made.where.empty() && !made.done) amiss("Cannot rename.", from);

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

}

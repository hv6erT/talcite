#include "talcite/fs.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
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

#include "fscore.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TALCITE_LINUX_FS)

#include <linux/fs.h>
#include <sys/ioctl.h>

#endif

#endif

namespace talcite::fs {

using namespace core;

// Uchwyt jest wspólny, a nie kopiowany: File to typ wartościowy tak samo jak Promise, więc
// kopie dzielą ten sam otwarty plik. Dzięki temu File przechodzi przez obietnicę bez sztuczek
// z wskaźnikami.
//
// **`close()` zamyka plik, a nie tylko swoją kopię.** Kopia zostaje w każdej obietnicy, przez którą
// File przeszedł — wynik `open` trzyma ją tak długo, jak żyje łańcuch — więc zamknięcie samej
// kopii zostawiało uchwyt otwarty. Na Linuksie nikt tego nie widział; na Windows zastąpienie pliku,
// który ktoś trzyma otwartym, odmawia, i twa padała „Cannot rename." przy drugim zapisie historii
// albo spisu biblioteki. Zamknięcie czeka na wywołania w toku (`busy`): numer deskryptora zwolniony
// spod biegnącego zapisu system oddałby komuś innemu.
struct File::Held {
    path where;

    std::mutex guard;
    int busy = 0;
    bool shut = false;

#ifdef _WIN32
    void* handle = nullptr;

    void released() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);

        handle = nullptr;
    }
#else
    int handle = -1;

    void released() {
        if (handle >= 0) ::close(handle);

        handle = -1;
    }
#endif

    ~Held() { released(); }

    Held() = default;

    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    Held(Held&&) = delete;
    Held& operator=(Held&&) = delete;
};

namespace core {

std::shared_ptr<File::Held> open_at(const path& where, Mode how) {
    auto made = std::make_shared<File::Held>();
    made->where = where;

#ifdef _WIN32
    // **Ucinanie przez `OPEN_ALWAYS` i `SetEndOfFile`, nie przez `CREATE_ALWAYS`.** To drugie z
    // atrybutem zwykłym odmawia dostępu do istniejącego pliku ukrytego albo systemowego — tak
    // padało nadpisanie ukrytego `.nfo`. `FILE_SHARE_DELETE` pozwala zmienić nazwę albo usunąć
    // plik, który akurat kopiujemy albo liczymy, zamiast blokować go w Eksploratorze.
    const DWORD reach = how == Mode::read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    const DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const DWORD when = how == Mode::read ? OPEN_EXISTING : how == Mode::exclusive ? CREATE_NEW : OPEN_ALWAYS;

    made->handle = CreateFileW(longly(where).c_str(), reach, shared, nullptr, when,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    if (made->handle == INVALID_HANDLE_VALUE) {
        const DWORD why = GetLastError();

        if (how == Mode::exclusive && (why == ERROR_FILE_EXISTS || why == ERROR_ALREADY_EXISTS))
            amiss("The file already exists.", where);

        amiss("Cannot open the file.", where);
    }

    if (how == Mode::truncate && SetEndOfFile(made->handle) == 0) amiss("Cannot open the file.", where);
#else
    int flags = how == Mode::read ? O_RDONLY : O_RDWR;
    if (how == Mode::write) flags |= O_CREAT;
    if (how == Mode::truncate) flags |= O_CREAT | O_TRUNC;
    if (how == Mode::exclusive) flags |= O_CREAT | O_EXCL;

    // **Zamykany przy uruchomieniu programu potomnego** — inaczej dziecko dziedziczy każdy plik,
    // który akurat mamy otwarty, i trzyma go po naszym `close()`.
    made->handle = ::open(where.c_str(), flags | O_CLOEXEC, 0644);

    if (made->handle < 0) {
        if (how == Mode::exclusive && errno == EEXIST) amiss("The file already exists.", where);

        amiss("Cannot open the file.", where);
    }
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

            amiss("Cannot read the file.", held.where);
        }
#else
        const ssize_t taken = ::pread(held.handle, made.data() + got, much - got,
                                      static_cast<off_t>(at + got));

        if (taken < 0) amiss("Cannot read the file.", held.where);
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
            amiss("Cannot write the file.", held.where);
#else
        const ssize_t given = ::pwrite(held.handle, what.data() + put, what.size() - put,
                                       static_cast<off_t>(at + put));

        if (given < 0) amiss("Cannot write the file.", held.where);
#endif

        // **Zerowy zapis to brak postępu**, a nie koniec pracy: wołający kręci pętlę warunkiem na
        // przesunięciu, więc oddanie zera przy niepustym kawałku zapętliłoby kopiowanie na pełnym
        // rdzeniu. Mówimy o tym wprost, zamiast milczeć.
        if (given == 0) amiss("Cannot write the file.", held.where);

        put += static_cast<std::size_t>(given);
    }

    return put;
}

// **Utrwalenie.** Zapis kończy się w pamięci systemu; dopiero to wywołanie czeka, aż dane leżą na
// nośniku — także na udziale sieciowym, gdzie `FlushFileBuffers` staje się poleceniem FLUSH dla
// serwera. Bez tego zanik zasilania tuż po zapisie zostawiał plik pusty albo stary, a kopia
// przenoszona między nośnikami kasowała źródło, zanim cel naprawdę istniał. Na macOS `fsync`
// nie opróżnia pamięci samego dysku; robi to dopiero `F_FULLFSYNC`.
void sync_at(const File::Held& held) {
#ifdef _WIN32
    if (FlushFileBuffers(held.handle) == 0) amiss("Cannot flush the file.", held.where);
#else
#ifdef F_FULLFSYNC
    if (::fcntl(held.handle, F_FULLFSYNC) == 0) return;
#endif

    if (::fsync(held.handle) != 0) amiss("Cannot flush the file.", held.where);
#endif
}

void stamp_on(const File::Held& held, Instant when) {
#ifdef _WIN32
    const auto ticks = std::chrono::floor<std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(
        when.time_since_epoch());

    const auto much = static_cast<std::uint64_t>(ticks.count() + 11644473600LL * 10000000LL);

    FILETIME told{};

    told.dwLowDateTime = static_cast<DWORD>(much & 0xFFFFFFFFull);
    told.dwHighDateTime = static_cast<DWORD>(much >> 32);

    if (SetFileTime(held.handle, nullptr, nullptr, &told) == 0) amiss("Cannot set the write time.", held.where);
#else
    const auto whole = std::chrono::floor<std::chrono::seconds>(when);

    const timespec told[2] = {{0, UTIME_OMIT},
                              {static_cast<time_t>(whole.time_since_epoch().count()),
                               static_cast<long>((when - whole).count())}};

    if (::futimens(held.handle, told) != 0) amiss("Cannot set the write time.", held.where);
#endif
}

// Reflink: cel dostaje te same bloki co źródło, a osobne dopiero przy pierwszym zapisie. Na
// Btrfs i XFS kopia 40 GB trwa chwilę i nie zajmuje miejsca. Gdzie indziej system odmawia
// (`EOPNOTSUPP`, `EXDEV`, `EINVAL`) i kopiujemy dalej zwykłą drogą.
bool cloned_on([[maybe_unused]] const File::Held& from, [[maybe_unused]] const File::Held& to) {
#if defined(TALCITE_LINUX_FS) && defined(FICLONE)
    return ::ioctl(to.handle, FICLONE, from.handle) == 0;
#else
    return false;
#endif
}

// `copy_file_range` przenosi dane w jądrze, bez kopiowania przez naszą pamięć; na NFS 4.2 i na
// udziale SMB (cifs) serwer kopiuje u siebie i nic nie idzie przez sieć. Starsze jądro, para
// nośników, której nie obsługuje, albo system plików bez tej operacji mówią „nie umiem"
// (`ENOSYS`, `EXDEV`, `EOPNOTSUPP`, `EINVAL`) — wtedy wraca brak i czytamy sami. Inny błąd jest
// prawdziwym błędem zapisu.
std::optional<std::size_t> spliced_on([[maybe_unused]] const File::Held& from,
                                      [[maybe_unused]] const File::Held& to, [[maybe_unused]] std::uintmax_t at,
                                      [[maybe_unused]] std::size_t much) {
#if defined(TALCITE_LINUX_FS)
    auto there = static_cast<off64_t>(at);
    auto here = static_cast<off64_t>(at);

    for (;;) {
        const ssize_t went = ::copy_file_range(from.handle, &there, to.handle, &here, much, 0);

        if (went >= 0) return static_cast<std::size_t>(went);

        if (errno == EINTR) continue;

        if (errno == ENOSYS || errno == EXDEV || errno == EOPNOTSUPP || errno == EINVAL) return std::nullopt;

        amiss("Cannot write the file.", to.where);
    }
#else
    return std::nullopt;
#endif
}

std::uintmax_t sized(const File::Held& held) {
#ifdef _WIN32
    LARGE_INTEGER told{};

    if (GetFileSizeEx(held.handle, &told) == 0) amiss("Cannot read the file size.",
                                                      held.where);

    return static_cast<std::uintmax_t>(told.QuadPart);
#else
    struct stat told {};

    if (::fstat(held.handle, &told) != 0) amiss("Cannot read the file size.", held.where);

    return static_cast<std::uintmax_t>(told.st_size);
#endif
}

}

namespace {

// Uchwyt w użyciu na czas jednego wywołania. Zamknięty plik odmawia; zamknięcie w trakcie
// zamyka uchwyt dopiero, gdy wyjdzie ostatni.
class Lease {
public:
    explicit Lease(File::Held& held) : held_(held) {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (held_.shut) amiss("The file is not open.", held_.where);

        ++held_.busy;
    }

    ~Lease() {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (--held_.busy == 0 && held_.shut) held_.released();
    }

    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&&) = delete;
    Lease& operator=(Lease&&) = delete;

private:
    File::Held& held_;
};

}

void File::close() {
    if (!held_) return;

    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->shut = true;

        if (held_->busy == 0) held_->released();
    }

    held_.reset();
}

const path& File::where() const {
    static const path nowhere;

    return held_ ? held_->where : nowhere;
}

Promise<Bytes> File::read(std::uintmax_t at, std::size_t much) const {
    auto held = held_;

    return Promise<Bytes>{[held, at, much](auto resolve, auto) {
        if (!held) amiss("The file is not open.", path{});

        Bytes made;

        {
            const Lease lease{*held};

            made = read_at(*held, at, much);
        }

        resolve(std::move(made));
    }};
}

Promise<std::size_t> File::write(std::uintmax_t at, Bytes what) const {
    auto held = held_;

    return Promise<std::size_t>{[held, at, what = std::move(what)](auto resolve, auto) {
        if (!held) amiss("The file is not open.", path{});

        std::size_t made = 0;

        {
            const Lease lease{*held};

            made = write_at(*held, at, what);
        }

        resolve(made);
    }};
}

struct Cursor::Held {
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
    explicit Claim(std::shared_ptr<Cursor::Held> held) : held_(std::move(held)) {}

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
    std::shared_ptr<Cursor::Held> held_;
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

Claiming claimed(const std::shared_ptr<Cursor::Held>& held) {
    if (!held) return {nullptr, "The stream is closed.", path{}};

    bool spare = false;

    if (!held->busy.compare_exchange_strong(spare, true))
        return {nullptr, "The stream is busy.", held->file->where};

    return {std::make_shared<Claim>(held), std::string{}, path{}};
}

}

Cursor File::cursor() const {
    if (!held_) amiss("The file is not open.", path{});

    auto made = std::make_shared<Cursor::Held>();
    made->file = held_;

    return Cursor{made};
}

const path& Cursor::where() const {
    static const path nowhere;

    return held_ ? held_->file->where : nowhere;
}

std::uintmax_t Cursor::at() const { return held_ ? held_->at.load() : 0; }

void Cursor::seek(std::uintmax_t at) {
    if (!held_) amiss("The stream is closed.", path{});

    if (held_->busy.load()) amiss("The stream is busy.", held_->file->where);

    held_->at.store(at);
}

Promise<Bytes> Cursor::read(std::size_t much) {
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

            const Lease lease{*held->file};

            made = read_at(*held->file, at, much);

            held->at.store(at + made.size());
        }

        resolve(std::move(made));
    }};
}

Promise<std::size_t> Cursor::write(Bytes what) {
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

                const Lease lease{*held->file};

                much = write_at(*held->file, at, what);

                held->at.store(at + much);
            }

            resolve(much);
        }};
}

Promise<void> File::sync() const {
    auto held = held_;

    return Promise<void>{[held](auto resolve, auto) {
        if (!held) amiss("The file is not open.", path{});

        {
            const Lease lease{*held};

            sync_at(*held);
        }

        resolve();
    }};
}

Promise<std::uintmax_t> File::size() const {
    auto held = held_;

    return Promise<std::uintmax_t>{[held](auto resolve, auto) {
        if (!held) amiss("The file is not open.", path{});

        std::uintmax_t made = 0;

        {
            const Lease lease{*held};

            made = sized(*held);
        }

        resolve(made);
    }};
}

Promise<File> open(const path& where, Mode how) {
    return Promise<File>{
        [where, how](auto resolve, auto) { resolve(File{open_at(where, how)}); }};
}

}

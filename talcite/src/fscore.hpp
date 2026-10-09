#pragma once

#include <cstddef>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/fs.hpp"

// **Która droga systemu plików.** Linux ma własne: `statfs` z magią rodzaju, /proc/mounts,
// `renameat2`, inotify, `copy_file_range`. macOS i BSD mają `statfs` z nazwą rodzaju i `getmntinfo`.
// Reszta POSIX-u dostaje drogę przenośną, ubogą, ale prawdziwą. `TALCITE_GENERIC_POSIX` wymusza ją
// także na Linuksie — tak jak `TALCITE_POLL` w sieci — żeby dało się ją sprawdzić tam, gdzie
// akurat stoi coś lepszego.
#if !defined(_WIN32) && !defined(TALCITE_GENERIC_POSIX)
#if defined(__linux__)
#define TALCITE_LINUX_FS 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define TALCITE_BSD_FS 1
#endif
#endif

namespace talcite::fs::core {

// **Wspólny rdzeń `fs`** — to, czego potrzebują wszystkie cztery pliki: pytania o ścieżki
// (`fs.cpp`), pliki i kursory (`file.cpp`), kopiowanie (`copy.cpp`) i obserwowanie (`watch.cpp`).
// Dawniej jeden plik na prawie trzy tysiące wierszy trzymał to w przestrzeni anonimowej; po
// podziale (przegląd, krok 7b) funkcje synchroniczne `*_at` mieszkają w `core`, a każdy plik
// woła je bez kwalifikatora przez `using namespace core`.

namespace stood = std::filesystem;

// Raty odczytu według nośnika: dysk stały, udział sieciowy, reszta.
inline constexpr std::size_t on_disk = 1024 * 1024;
inline constexpr std::size_t afar = 64 * 1024;
inline constexpr std::size_t astray = 256 * 1024;

[[noreturn]] inline void amiss(const std::string& said, const path& where) {
    throw Trouble{said, where};
}

// Funkcje synchroniczne wołane z więcej niż jednego pliku.
[[nodiscard]] bool afar_at(const path& where);

[[nodiscard]] path canonical_at(const path& where);

[[nodiscard]] std::size_t chunk_at(const path& where);

[[nodiscard]] bool equivalent_at(const path& one, const path& two);

[[nodiscard]] bool exists_at(const path& where);

[[nodiscard]] Kind kind_at(const path& where);

[[nodiscard]] std::vector<Entry> list_at(const path& where);

void make_folder_at(const path& where);

[[nodiscard]] std::chrono::sys_seconds modified_at(const path& where);

// Czas zapisu z pełną dokładnością systemu: nanosekundy na POSIX, setki nanosekund na Windows.
// Kopia przenosi go w całości — dawniej przez sekundy z `std::filesystem` i przeliczenie zegara
// plikowego, które gubiło ułamek i bywało przesunięte o sekundę na granicy.
using Instant = std::chrono::sys_time<std::chrono::nanoseconds>;

[[nodiscard]] Instant instant_at(const path& where);

// Czas zapisu wpisany przez uchwyt — plik albo katalog, wszystko jedno; na Windows katalog
// otwiera się z `FILE_FLAG_BACKUP_SEMANTICS`, czego `std::filesystem` z MinGW nie robił.
void stamp_on(const File::Held& held, Instant when);

[[nodiscard]] std::shared_ptr<File::Held> open_at(const path& where, Mode how);

[[nodiscard]] Bytes read_at(const File::Held& held, std::uintmax_t at, std::size_t much);

[[nodiscard]] path beside(const path& to, int at);

[[nodiscard]] path settled_on(const path& to, Conflict how);

// Wynik przeniesienia z rozstrzygnięciem kolizji: `where` puste — pominięte (`skip`); `done`
// fałszywe przy niepustym `where` — **wyłącznie** inny nośnik. Każdą inną odmowę `shifted` zgłasza
// sam, bo tylko inny nośnik pozwala przenieść kopią.
struct Shift {
    path where;
    bool done = false;
};

[[nodiscard]] Shift shifted(const path& from, const path& to, Conflict how);

// Zmiana samej wielkości liter (`heat` → `Heat`) w jednym katalogu.
[[nodiscard]] bool recased(const path& from, const path& to);

// Zmiana wielkości liter, którą serwer przyjął bez słowa i nie zrobił (Samba bez rozróżniania,
// część NAS-ów): sprawdzamy spis katalogu (`exact`), a gdy nazwy tam nie ma — dwa kroki przez
// nazwę obok. Ruch (`move`) to `pushed` w prawdziwym biegu; w próbach — podróbka serwera.
// Fałsz, gdy drugi krok się nie udał (pierwszy jest wtedy cofnięty).
[[nodiscard]] bool respelled_twice(const path& from, const path& to,
                                   const std::function<bool(const path&, const path&)>& move,
                                   const std::function<bool(const path&)>& exact);

// Zastąpienie celu przemianowaniem, trwałe; fałsz, gdy system odmówił.
[[nodiscard]] bool replaced_at(const path& from, const path& to);

[[nodiscard]] std::uintmax_t sized(const File::Held& held);

void sync_at(const File::Held& held);

// Katalog utrwalony po zmianie nazwy w nim (POSIX); na Windows robi to `MOVEFILE_WRITE_THROUGH`.
void settled_folder(const path& where);

// Czy nośnik pod ścieżką to FAT (FAT16, FAT32) — sprawdzian przed kopią pyta o granicę 4 GiB.
[[nodiscard]] bool fat_at(const path& where);

// Czy obie ścieżki leżą na jednym nośniku — wtedy przeniesienie jest zmianą nazwy i miejsca nie trzeba.
[[nodiscard]] bool together_at(const path& one, const path& two);

// Ile wolnego miejsca ma nośnik pod ścieżką (albo pod jej najbliższym istniejącym przodkiem);
// brak, gdy system nie powie.
[[nodiscard]] std::optional<std::uintmax_t> room_at(const path& where);

void stamp_at(const path& where, Instant when);

// Treść pliku przez system, bez przechodzenia przez naszą pamięć. `cloned_on` współdzieli bloki
// (reflink na Btrfs i XFS) i oddaje prawdę, gdy cała treść już jest; `spliced_on` przenosi kawałek
// od `at` w jądrze — na NFS 4.2 i SMB jako kopia po stronie serwera — i oddaje, ile przeszło, albo
// nic, gdy system tego nie umie dla tej pary i trzeba czytać i pisać samemu.
[[nodiscard]] bool cloned_on(const File::Held& from, const File::Held& to);

[[nodiscard]] std::optional<std::size_t> spliced_on(const File::Held& from, const File::Held& to,
                                                    std::uintmax_t at, std::size_t much);

[[nodiscard]] bool stopped();

[[nodiscard]] std::vector<Entry> walk_at(const path& where);

[[nodiscard]] std::size_t write_at(const File::Held& held, std::uintmax_t at, const Bytes& what);

#ifdef _WIN32

[[nodiscard]] std::wstring longly(const path& where);

#endif

}

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
#include "talcite/random.hpp"

#include "fscore.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#endif

namespace talcite::fs {

using namespace core;

namespace {

// Bilet operacji ratami: wie, ile bajtów albo pozycji z ilu już zeszło. Wirtualny progress
// z Ticket jest tu po to, żeby pasek postępu pytał, a nikt niczego nie przepisywał w górę.
template <typename T>
struct Counted : detail::Result<T> {
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
    return std::min(chunk_at(from), chunk_at(to));
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
// Czy przy tym rozstrzygnięciu kolizji pytanie „a może to ten sam plik?" ma sens. `skip` ma prawo
// po prostu nic nie zrobić, a `beside` — zrobić kopię obok, i to jest jedyny sensowny sposób
// skopiowania pliku na samego siebie. Zostają `overwrite`, który kasował, i `refuse`, któremu
// należy się zdanie mówiące, o co chodzi.
bool asked(Conflict how) { return how == Conflict::overwrite || how == Conflict::fail; }


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

// Plik obok celu na czas zastępowania: kropka, nazwa celu, sześć losowych znaków i `.part`.
// Serwery multimediów nie biorą `.part` za film, a losowy ogon nie trafi w cudzy plik.
path aside_of(const path& to) {
    static constexpr std::string_view digits = "0123456789abcdef";

    std::string tail;

    for (const std::byte one : random(3)) {
        const auto value = static_cast<unsigned>(one);

        tail += digits[value >> 4U];
        tail += digits[value & 0x0fU];
    }

    path made = to;

    made.replace_filename(path{"."}.native() + to.filename().native() + path{"." + tail + ".part"}.native());

    return made;
}

// Trzy rodzaje pisania do pliku: `Mode::truncate` pisze wprost (cel nie istnieje), zastąpienie
// pisze obok i przemianowuje — przerwane albo nieudane nie rusza starego celu, a drugie dowiązanie
// twarde do starego pliku zostaje ze starą treścią — a `durable` dodatkowo czeka, aż dane leżą na
// nośniku, zanim powie „gotowe". Zastąpienie utrwala zawsze: bez tego zanik zasilania tuż po
// przemianowaniu zostawiał na miejscu celu plik pusty.
//
// `fill` dostaje ścieżkę, pod którą ma powstać treść, i oddaje otwarty uchwyt do niej albo nic,
// gdy zadanie anulowano w połowie — wtedy obietnica nie rozstrzyga się wcale. Czas zapisu, gdy
// jest, wpisujemy przez ten sam uchwyt przed utrwaleniem.
template <typename Fill>
bool poured_at(const path& to, bool durable, std::optional<Instant> when, Fill fill) {
    const bool there = kind_at(to) != Kind::none;
    const path into = there ? aside_of(to) : to;

    Sweeping sweeping{into};

    {
        const std::shared_ptr<File::Held> yours = fill(into);

        if (!yours) return false;

        if (when) stamp_on(*yours, *when);

        if (there || durable) sync_at(*yours);
    }

    if (there && !replaced_at(into, to)) amiss("Cannot replace the file.", to);

    sweeping.keep();

    return true;
}

// Zapis ratami przez nasz bufor — gdy treść już mamy albo gdy system nie umie skopiować sam.
template <typename Pour>
bool poured_at(const path& to, bool durable, Pour pour) {
    return poured_at(to, durable, std::nullopt, [&](const path& into) -> std::shared_ptr<File::Held> {
        std::shared_ptr<File::Held> yours = open_at(into, Mode::truncate);

        if (!pour(*yours)) return nullptr;

        return yours;
    });
}

// Kawałek dla kopii w jądrze: większy niż rata odczytu, bo przy kopii po stronie serwera każdy
// kawałek to jedno pytanie przez sieć, a mały kawałek zjadałby całą korzyść. Anulowanie czeka
// najwyżej na jeden.
constexpr std::size_t spliced_piece = 16 * 1024 * 1024;

#ifdef _WIN32

// `CopyFileExW` kopiuje sam: na udziale SMB po stronie serwera, z odciążeniem ODX na macierzy,
// z atrybutami i strumieniami. Postęp i anulowanie idą przez wywołanie zwrotne, w tym samym
// wątku, więc dzwonek robotnika słychać jak w naszej pętli. Anulowane — system sam usuwa cel.
DWORD CALLBACK copied(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                      LPVOID held) {
    static_cast<Counted<void>*>(held)->done.store(static_cast<std::uintmax_t>(done.QuadPart));

    return stopped() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

#endif

bool file_at(const path& from, const path& to, Counted<void>& held, bool durable = false) {
    const std::shared_ptr<File::Held> mine = open_at(from, Mode::read);

    const std::uintmax_t whole = sized(*mine);
    held.whole.store(whole);

    const Instant when = instant_at(from);

    const std::size_t rata = rated(from, to);

    return poured_at(to, durable, when, [&](const path& into) -> std::shared_ptr<File::Held> {
#ifdef _WIN32
        BOOL cancel = FALSE;

        if (CopyFileExW(longly(from).c_str(), longly(into).c_str(), copied, &held, &cancel,
                        COPY_FILE_FAIL_IF_EXISTS) != 0) {
            // Atrybut „tylko do odczytu" przyszedł ze źródła; zostawiony, zablokowałby utrwalenie
            // tutaj i każde następne nadpisanie celu. Nasza pętla też go nie przenosiła.
            const DWORD told = GetFileAttributesW(longly(into).c_str());

            if (told != INVALID_FILE_ATTRIBUTES && (told & FILE_ATTRIBUTE_READONLY) != 0)
                SetFileAttributesW(longly(into).c_str(), told & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));

            held.done.store(whole);

            return open_at(into, Mode::write);
        }

        if (GetLastError() == ERROR_REQUEST_ABORTED || stopped()) return nullptr;

        // Inna odmowa — system bez tej drogi (Wine), nośnik, który jej nie lubi. Zwykła pętla
        // powie, co naprawdę jest nie tak, albo po prostu skopiuje.
#endif
        std::shared_ptr<File::Held> yours = open_at(into, Mode::truncate);

        if (whole > 0 && cloned_on(*mine, *yours)) {
            held.done.store(whole);

            return yours;
        }

        bool ranged = true;

        for (std::uintmax_t at = 0; at < whole;) {
            if (stopped()) return nullptr;

            if (ranged) {
                const std::optional<std::size_t> went =
                    spliced_on(*mine, *yours, at, static_cast<std::size_t>(std::min<std::uintmax_t>(
                                                      whole - at, std::max(rata, spliced_piece))));

                // Zero przed końcem to nie koniec pliku, tylko system, który tej drogi nie
                // dotrzymał (pliki generowane, część FUSE) — dalej idziemy sami.
                if (went && *went > 0) {
                    at += *went;
                    held.done.store(at);

                    continue;
                }

                ranged = false;
            }

            const Bytes part = read_at(*mine, at, rata);
            if (part.empty()) break;

            at += write_at(*yours, at, part);
            held.done.store(at);
        }

        return yours;
    });
}

// Czas katalogu z pełną dokładnością, a gdy katalog zniknął spod ręki — ten z wykazu.
Instant instant_or(const path& where, std::chrono::sys_seconds otherwise) {
    try {
        return instant_at(where);
    } catch (const Trouble&) {
        return Instant{otherwise};
    }
}

bool tree_at(const path& from, const path& to, Counted<void>& held, Conflict how, Seen& seen,
             int deep);

// Jedna pozycja z wykazu. Dowiązanie rozwijamy — kopiujemy to, na co wskazuje, a nie samo
// dowiązanie. Potok, gniazdo i urządzenie pomijamy: otwarcie potoku do odczytu zawiesiłoby
// wątek puli bez ratunku, a to gorsze niż brak kopii.
bool spread_at(const path& from, Kind kind, const path& to, Conflict how, Seen& seen, int deep) {
    if (kind == Kind::folder) {
        make_folder_at(to);

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

    return file_at(onto, where, apart);
}

// Katalog idzie wpis po wpisie; postęp liczymy tutaj we wpisach, nie w bajtach. Wykaz jest
// posortowany ścieżkami, a rodzic zawsze poprzedza swoje dzieci, więc katalogi da się
// tworzyć po kolei. Zawartość rozwiniętych dowiązań ma własny licznik i do tego się nie
// dolicza — inaczej mieszalibyśmy dwie miary w jednym ułamku.
bool tree_at(const path& from, const path& to, Counted<void>& held, Conflict how, Seen& seen,
             int deep) {
    if (deep > 64) return true;

    const std::vector<Entry> among = walk_at(from);

    held.whole.store(among.size() + 1);

    Sweeping sweeping{to};

    make_folder_at(to);
    held.done.fetch_add(1);

    std::vector<std::pair<path, Instant>> folders;

    for (const Entry& one : among) {
        if (stopped()) return false;

        const path where = to / one.where.lexically_relative(from);

        if (!spread_at(one.where, one.kind, where, how, seen, deep)) return false;

        if (one.kind == Kind::folder) folders.emplace_back(where, instant_or(one.where, one.modified));

        held.done.fetch_add(1);
    }

    sweeping.keep();

    // Czas katalogu dopiero na końcu i od najgłębszych: każdy wpis włożony do środka go przestawiał,
    // więc podkatalog kopii miał dawniej czas kopiowania zamiast czasu źródła.
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) stamp_at(at->first, at->second);

    stamp_at(to, instant_at(from));

    return true;
}

// **Sprawdzian przed kopią.** Kopia 40 GB padała przy 4 GiB na FAT32 albo przy 95% na pełnym
// dysku, po kwadransie pracy i z połową pliku do posprzątania. Pytamy więc wcześniej: o największy
// plik, gdy cel to FAT (exFAT tej granicy nie ma), i o sumę, gdy nośnik celu mówi, ile ma miejsca.
// Nośnik, który nie odpowie, nie blokuje kopii — sprawdzian ma oszczędzać czas, a nie zgadywać.
constexpr std::uintmax_t fat_limit = 0xFFFFFFFFull;

void fits_at(const path& to, std::uintmax_t whole, std::uintmax_t largest) {
    if (largest > fat_limit && fat_at(to)) amiss("The file is too large for this disk.", to);

    if (const std::optional<std::uintmax_t> room = room_at(to); room && *room < whole)
        amiss("Not enough free space.", to);
}

// Ile zajmie kopia: same pliki. Tego, co za dowiązaniami, nie liczymy, więc suma jest dolną
// granicą — wystarczy, żeby odmówić tego, co na pewno się nie zmieści.
void measured(const std::vector<Entry>& among, std::uintmax_t& whole, std::uintmax_t& largest) {
    for (const Entry& one : among) {
        if (one.kind != Kind::file) continue;

        whole += one.size;
        largest = std::max(largest, one.size);
    }
}

bool copy_at(const path& from, const path& to, Counted<void>& held, Conflict how) {
    Kind what = kind_at(from);

    if (what == Kind::none) amiss("Nothing to copy.", from);

    path onto = from;

    if (what == Kind::link) {
        std::error_code trouble;

        onto = stood::canonical(from, trouble);
        if (trouble) amiss("The link leads nowhere.", from);

        what = kind_at(onto);
    }

    Seen seen;

    if (what == Kind::folder) {
        // **Nic na samo siebie.** Katalog kopiowany na własną ścieżkę wchodziłby w to, co
        // właśnie tworzy; MC odmawia i my też.
        if (asked(how) && kind_at(to) != Kind::none && equivalent_at(onto, to))
            amiss("Cannot copy a directory onto itself.", from);

        sighted(seen, canonical_at(onto));

        std::uintmax_t whole = 0;
        std::uintmax_t largest = 0;

        measured(walk_at(onto), whole, largest);
        fits_at(to, whole, largest);

        return tree_at(onto, to, held, how, seen, 0);
    }

    if (what != Kind::file) amiss("This entry cannot be copied.", from);

    // **Plik nie kopiuje się na samego siebie.** Przy `Conflict::overwrite` cel otwierał się do
    // zapisu i ucinał do zera, czyli kasował to, co miało zostać skopiowane — sprawdzone i tak
    // właśnie było. Pytamy **przed** `settled_on`, żeby zdanie mówiło, o co naprawdę chodzi,
    // a nie „cel już istnieje"; tylko gdy cel istnieje, bo `equivalent` bez drugiej strony nie ma
    // czego porównać.
    if (asked(how) && kind_at(to) != Kind::none && equivalent_at(onto, to))
        amiss("Cannot copy a file onto itself.", from);

    const path where = settled_on(to, how);
    if (where.empty()) return true;

    {
        std::error_code ignored;

        const std::uintmax_t much = stood::file_size(onto, ignored);

        if (!ignored) fits_at(where, much, much);
    }

    return file_at(onto, where, held);
}

// **Przeniesienie wpis po wpisie** — między nośnikami albo przy łączeniu katalogu z istniejącym.
// Każdy plik najpierw próbuje zwykłej zmiany nazwy (w obrębie nośnika natychmiastowej); gdy system
// odpowie „inny nośnik", plik się kopiuje, utrwala na nośniku celu i dopiero wtedy znika ze źródła.
// Katalogi źródła schodzą na końcu i tylko puste.
//
// Dawniej było odwrotnie: najpierw cała kopia, potem `remove_all` źródła, którego błąd ginął.
// Wszystko, czego kopia nie zobaczyła — podkatalog bez wstępu, wykaz ucięty zerwanym połączeniem,
// wpis pominięty — szło do kosza razem z resztą.
//
// Przeszkody sprawdzamy przed pierwszym ruchem: wpis, którego przenieść się nie da (potok, urządzenie,
// zerwane dowiązanie), i katalog, do którego nie ma wstępu, odmawiają całości.
std::vector<Entry> surveyed(const path& from) {
    std::vector<Entry> made;
    std::vector<path> waiting{from};

    while (!waiting.empty()) {
        const path here = std::move(waiting.back());
        waiting.pop_back();

        for (Entry& one : list_at(here)) {
            if (one.kind == Kind::folder) waiting.push_back(one.where);

            const bool stale = one.kind == Kind::link && one.reach == Kind::none;

            if (stale || one.kind == Kind::device || one.kind == Kind::special || one.kind == Kind::none)
                amiss("This entry cannot be moved.", one.where);

            made.push_back(std::move(one));
        }
    }

    std::sort(made.begin(), made.end(), [](const Entry& one, const Entry& two) { return one.where < two.where; });

    return made;
}

// Jeden plik albo dowiązanie. Fałsz — anulowano w trakcie kopii; źródło zostaje wtedy całe.
bool carried(const path& from, const path& to, Conflict how) {
    const Shift made = shifted(from, to, how);

    if (made.where.empty() || made.done) return true;

    // Inny nośnik: system odmówił, zanim spojrzał na cel, więc kolizję rozstrzygamy jeszcze raz.
    const path where = settled_on(to, how);
    if (where.empty()) return true;

    Counted<void> apart;
    Seen seen;

    if (kind_at(from) == Kind::link) {
        std::error_code trouble;

        const path onto = stood::canonical(from, trouble);
        if (trouble) amiss("The link leads nowhere.", from);

        if (kind_at(onto) == Kind::folder) {
            sighted(seen, onto);

            if (!tree_at(onto, where, apart, how, seen, 1)) return false;
        } else if (!file_at(onto, where, apart, true)) {
            return false;
        }
    } else if (!file_at(from, where, apart, true)) {
        return false;
    }

    std::error_code trouble;

    stood::remove(from, trouble);
    if (trouble) amiss("Cannot remove the source after copying.", from);

    return true;
}

bool relocated(const path& from, const path& to, Counted<void>& held, Conflict how) {
    // Pojedynczy plik trafia tu tylko z innego nośnika — w obrębie jednego wystarczyła zmiana nazwy.
    if (kind_at(from) != Kind::folder) {
        if (kind_at(from) == Kind::file) {
            std::error_code ignored;

            const std::uintmax_t much = stood::file_size(from, ignored);

            if (!ignored) fits_at(to, much, much);
        }

        held.whole.store(1);

        if (!carried(from, to, how)) return false;

        held.done.store(1);

        return true;
    }

    const std::vector<Entry> among = surveyed(from);

    if (!together_at(from, to)) {
        std::uintmax_t whole = 0;
        std::uintmax_t largest = 0;

        measured(among, whole, largest);
        fits_at(to, whole, largest);
    }

    held.whole.store(among.size() + 1);

    const Instant stamp = instant_at(from);

    make_folder_at(to);
    held.done.fetch_add(1);

    std::vector<std::pair<path, Instant>> folders;

    for (const Entry& one : among) {
        if (stopped()) return false;

        const path where = to / one.where.lexically_relative(from);

        if (one.kind == Kind::folder) {
            make_folder_at(where);
            folders.emplace_back(one.where, instant_or(one.where, one.modified));
        } else if (!carried(one.where, where, how)) {
            return false;
        }

        held.done.fetch_add(1);
    }

    // Od najgłębszych: katalog schodzi tylko pusty, a jego czas zapisu przechodzi na cel dopiero teraz,
    // bo każdy wpis włożony do środka go przestawiał.
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) {
        std::error_code ignored;

        stamp_at(to / at->first.lexically_relative(from), at->second);
        stood::remove(at->first, ignored);
    }

    std::error_code ignored;

    stood::remove(from, ignored);
    stamp_at(to, stamp);

    if (how != Conflict::skip && exists_at(from)) amiss("Some entries stayed behind.", from);

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

                              const std::size_t rata = chunk_at(where);

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

// Zapis istniejącego pliku idzie obok i przemianowaniem na cel (`poured_at`) — przerwany nie
// zostawia ogryzka pod prawdziwą nazwą.
Promise<void> write(const path& where, Bytes what, Conflict how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[where, what = std::move(what), how, held](auto resolve, auto) {
                             const path at_all = settled_on(where, how);

                             if (at_all.empty()) {
                                 resolve();
                                 return;
                             }

                             held->whole.store(what.size());

                             const std::size_t rata = chunk_at(at_all);

                             const bool whole = poured_at(at_all, false, [&](const File::Held& one) {
                                 for (std::size_t at = 0; at < what.size();) {
                                     if (stopped()) return false;

                                     const std::size_t much = std::min<std::size_t>(rata, what.size() - at);

                                     Bytes part{what.begin() + static_cast<std::ptrdiff_t>(at),
                                                what.begin() + static_cast<std::ptrdiff_t>(at + much)};

                                     at += write_at(one, at, part);
                                     held->done.store(at);
                                 }

                                 return true;
                             });

                             if (!whole) return;

                             resolve();
                         },
                         held};
}

Promise<void> copy(const path& from, const path& to, Conflict how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[from, to, how, held](auto resolve, auto) {
                             if (!copy_at(from, to, *held, how)) return;

                             resolve();
                         },
                         held};
}

// Najpierw zwykła zmiana nazwy, bo w obrębie jednego nośnika jest natychmiastowa. Dopiero gdy
// system odpowie „inny nośnik" — i tylko wtedy — przenosimy wpis po wpisie (`relocated`). Każda
// inna odmowa jest błędem: plik otwarty w odtwarzaczu nie może skończyć jako kopia 40 GB i dwa
// egzemplarze. Katalog nadpisywany katalogiem łączy się z nim, też wpis po wpisie.
Promise<void> move(const path& from, const path& to, Conflict how) {
    auto held = std::make_shared<Counted<void>>();

    return HaulPromise<void>{[from, to, how, held](auto resolve, auto) {
                             if (kind_at(from) == Kind::none)
                                 amiss("Nothing to move.", from);

                             if (how == Conflict::overwrite && kind_at(from) == Kind::folder &&
                                 kind_at(to) == Kind::folder && !equivalent_at(from, to)) {
                                 if (!relocated(from, to, *held, how)) return;

                                 resolve();
                                 return;
                             }

                             const Shift made = shifted(from, to, how);

                             // Inny nośnik — system nie musiał nawet spojrzeć na cel. Kolizję
                             // rozstrzygamy więc jeszcze raz, zanim cokolwiek się przeniesie.
                             const path where = made.where.empty() || made.done ? path{} : settled_on(to, how);

                             if (where.empty()) {
                                 held->whole.store(1);
                                 held->done.store(1);

                                 resolve();
                                 return;
                             }

                             if (!relocated(from, where, *held, how)) return;

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

                             if (trouble) amiss("Cannot remove.", where);

                             held->done.fetch_add(1);

                             resolve();
                         },
                         held};
}

}

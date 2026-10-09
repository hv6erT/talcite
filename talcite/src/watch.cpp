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

#include "fscore.hpp"

#ifdef _WIN32

#include <windows.h>

#include <winnetwk.h>

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TALCITE_LINUX_FS)

#include <poll.h>
#include <sys/inotify.h>

#endif

#endif

namespace talcite::fs {

using namespace core;

// ---------------------------------------------------------------------------------------
// Obserwowanie katalogów. Jeden wątek na całą bibliotekę, tak samo jak zegar w Time: sto
// obserwowanych katalogów nie może kosztować stu wątków.
// ---------------------------------------------------------------------------------------

struct Watch::Held {
    path where;
    bool deep = false;
    std::uint64_t which = 0;

    struct Wanting {
        std::shared_ptr<detail::Result<std::vector<Change>>> result;
        std::shared_ptr<detail::Waiter> gone;
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

// Zaplecze zdarzeń straciło słuch dla tej obserwacji — brak deskryptorów, zerwany udział, korzeń
// zniknął — i oddaje ją zaglądaniu.
void forsaken(std::uint64_t which);

// Obietnica sięgająca po chroniony konstruktor, żeby podstawić bilet, który rozstrzygnie
// wątek obserwatora, a nie własne wywołanie.
class Nexting : public Promise<std::vector<Change>> {
public:
    Nexting(Promise<std::vector<Change>>::Callback given,
            std::shared_ptr<detail::Result<std::vector<Change>>> made)
        : Promise<std::vector<Change>>(std::move(given), std::move(made)) {}
};

// Anulowane zgłoszenie musi zniknąć z kolejki czekających, inaczej pierwsza paczka poszłaby
// do nikogo. To samo ogniwo co Dropping w zegarze.
class Forgetting : public detail::Waiter {
public:
    Forgetting(std::weak_ptr<Watch::Held> whose, detail::Result<std::vector<Change>>* which)
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
    detail::Result<std::vector<Change>>* which_;
};

// Powtórzone „zmieniony" dla tej samej ścieżki scalamy — system plików produkuje serie, a
// panel i tak odrysuje się raz. Po przepełnieniu zostaje jeden wpis: zgubiłem rachubę.
void noting(Watch::Held& held, Change made) {
    if (made.kind == Change::Kind::modified) {
        std::size_t looked = 0;

        for (auto at = held.pending.rbegin(); at != held.pending.rend() && looked < 32;
             ++at, ++looked) {
            if (at->where != made.where) continue;
            if (at->kind == Change::Kind::modified) return;

            break;
        }
    }

    held.pending.push_back(std::move(made));

    if (held.pending.size() <= brim) return;

    held.pending.clear();
    held.pending.push_back(Change{held.where, path{}, Change::Kind::overflow});
}

void handed(const std::shared_ptr<Watch::Held>& held, std::vector<Change> among) {
    std::shared_ptr<detail::Result<std::vector<Change>>> lucky;
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
    if (lucky) detail::settled(*lucky, std::move(giving));
}

}

namespace {

#if defined(TALCITE_LINUX_FS)

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

    // Fałsz — systemowi skończyły się deskryptory (`ENOSPC`: granica `max_user_watches`, albo
    // `ENOMEM`) i obserwacja idzie odpytywaniem. Dziurawa, bez części podkatalogów, byłaby gorsza
    // niż wolniejsza: milczałaby o zmianach w miejscach, które panel pokazuje.
    [[nodiscard]] bool add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;

        std::vector<std::pair<int, path>> made;

        const int first = inotify_add_watch(fd_, where.c_str(), noticed);

        if (first < 0) {
            if (spent()) return false;

            amiss("Cannot watch.", where);
        }

        made.emplace_back(first, where);

        if (deep)
            for (const path& one : under(where)) {
                const int more = inotify_add_watch(fd_, one.c_str(), noticed);

                if (more >= 0) {
                    made.emplace_back(more, one);
                } else if (spent()) {
                    unwatched(made);

                    return false;
                }
            }

        const std::lock_guard<std::mutex> kept{guard_};

        for (const auto& [spot, where] : made) {
            post->spots.push_back(spot);
            spots_[spot].push_back(Spot{which, where});
        }

        posts_[which] = std::move(post);

        return true;
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

            for (const int one : taken->spots)
                if (released(one, which)) spare.push_back(one);
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

private:
    struct Spot {
        std::uint64_t which = 0;
        path where;
    };

    static bool spent() { return errno == ENOSPC || errno == ENOMEM; }

    // Zdejmuje wpisy jednej obserwacji z deskryptora; prawda, gdy nikt już z niego nie korzysta.
    // Wołane pod zamkiem.
    bool released(int spot, std::uint64_t which) {
        const auto found = spots_.find(spot);
        if (found == spots_.end()) return false;

        std::vector<Spot>& among = found->second;

        among.erase(std::remove_if(among.begin(), among.end(), [which](const Spot& mine) { return mine.which == which; }),
                    among.end());

        if (!among.empty()) return false;

        spots_.erase(found);

        return true;
    }

    // Wycofanie nieudanego dopisania: deskryptory, których nie dzieli żadna inna obserwacja.
    void unwatched(const std::vector<std::pair<int, path>>& made) {
        std::vector<int> spare;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            for (const auto& [spot, where] : made)
                if (spots_.find(spot) == spots_.end()) spare.push_back(spot);
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

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

    // Korzeń obserwacji ma osobne prawa: tylko o nim zgłaszamy „zniknął" z jego własnego
    // zdarzenia. Podkatalog głębokiej obserwacji zgłasza jego rodzic (`IN_DELETE`,
    // `IN_MOVED_FROM`) — dawniej przychodziło to podwójnie, a do tego jako trzecie „zmieniony"
    // z `IN_IGNORED`, które po usunięciu dostaje każdy deskryptor.
    bool rooted(std::uint64_t which, const path& where) {
        const std::lock_guard<std::mutex> kept{guard_};

        const auto at = posts_.find(which);

        return at != posts_.end() && at->second->where == where;
    }

    void take(const char* room, std::size_t much) {
        std::unordered_map<std::uint64_t, std::vector<Change>> among;
        std::map<std::pair<std::uint64_t, std::uint32_t>, path> moving;
        std::vector<std::pair<std::uint64_t, path>> fresh;
        std::vector<std::pair<std::uint64_t, path>> gone;
        std::vector<std::uint64_t> blind;

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

                // Deskryptor po `IN_IGNORED` już nie istnieje, a jądro może dać jego numer
                // komuś nowemu — zostawiony wpis przypisałby cudze zdarzenia nam.
                if ((told->mask & IN_IGNORED) != 0) {
                    for (const Spot& one : spots) {
                        const auto post = posts_.find(one.which);

                        if (post == posts_.end()) continue;

                        std::vector<int>& mine = post->second->spots;

                        mine.erase(std::remove(mine.begin(), mine.end(), told->wd), mine.end());
                    }

                    spots_.erase(found);
                }
            }

            // Ten sam deskryptor może należeć do kilku obserwacji naraz, więc jedno
            // zdarzenie rozchodzi się do każdej z nich osobno.
            for (const Spot& spot : spots) {
                const std::uint64_t which = spot.which;
                const path where = told->len > 0 ? spot.where / path{told->name} : spot.where;

                std::vector<Change>& mine = among[which];

                if ((told->mask & IN_IGNORED) != 0) {
                    // Korzeń bez deskryptora — usunięty, odmontowany — nic już nie usłyszy.
                    // Zostaje zaglądanie, które zobaczy też, gdy wróci.
                    if (rooted(which, spot.where)) blind.push_back(which);

                    continue;
                }

                if ((told->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    if (rooted(which, spot.where)) mine.push_back(Change{where, path{}, Change::Kind::removed});

                    continue;
                }

                if ((told->mask & IN_MOVED_FROM) != 0) {
                    moving[{which, told->cookie}] = where;
                    mine.push_back(Change{where, path{}, Change::Kind::removed});

                    if ((told->mask & IN_ISDIR) != 0) gone.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_MOVED_TO) != 0) {
                    const auto paired = moving.find({which, told->cookie});

                    if (paired == moving.end()) {
                        mine.push_back(Change{where, path{}, Change::Kind::created});
                    } else {
                        // Para „wyszło stąd" i „weszło tam" to jedno przemianowanie, więc
                        // zdejmujemy zgłoszone przed chwilą zniknięcie.
                        const auto stale =
                            std::find_if(mine.begin(), mine.end(), [&paired](const Change& one) {
                                return one.kind == Change::Kind::removed && one.where == paired->second;
                            });

                        if (stale != mine.end()) mine.erase(stale);

                        mine.push_back(Change{where, paired->second, Change::Kind::renamed});

                        const auto left = std::find(gone.begin(), gone.end(), std::make_pair(which, paired->second));

                        if (left != gone.end()) gone.erase(left);

                        moving.erase(paired);
                    }

                    if ((told->mask & IN_ISDIR) != 0) fresh.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_CREATE) != 0) {
                    mine.push_back(Change{where, path{}, Change::Kind::created});

                    if ((told->mask & IN_ISDIR) != 0) fresh.emplace_back(which, where);

                    continue;
                }

                if ((told->mask & IN_DELETE) != 0) {
                    mine.push_back(Change{where, path{}, Change::Kind::removed});
                    continue;
                }

                mine.push_back(Change{where, path{}, Change::Kind::modified});
            }
        }

        for (const auto& [which, where] : gone) pruned(which, where);

        for (const auto& [which, where] : fresh)
            if (!sprout(which, where)) blind.push_back(which);

        for (auto& [which, mine] : among)
            if (!mine.empty()) reached(which, std::move(mine));

        for (const std::uint64_t which : blind) {
            drop(which);
            forsaken(which);
        }
    }

    // Nowy podkatalog pod obserwacją w głąb trzeba dopisać z osobna — inotify nie zna
    // obserwowania drzewa, zna tylko pojedyncze katalogi. To jest cena, o której mowa
    // w CLAUDE.md: przy dużym drzewie to tysiące deskryptorów.
    //
    // Katalog przemianowany w obrębie drzewa ma już deskryptory — jądro odda te same numery.
    // Wtedy **tylko poprawiamy ścieżki**: dopisane drugi raz dawały każde zdarzenie podwójnie,
    // a stare zostawały przy starej nazwie, więc plik w przemianowanym katalogu zgłaszał się
    // pod ścieżką, której już nie ma.
    //
    // Fałsz — skończyły się deskryptory i obserwacja musi przejść na odpytywanie.
    bool sprout(std::uint64_t which, const path& where) {
        std::vector<path> among{where};

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end() || !at->second->deep) return true;
        }

        for (const path& one : under(where)) among.push_back(one);

        for (const path& one : among) {
            const int spot = inotify_add_watch(fd_, one.c_str(), noticed);

            if (spot < 0) {
                if (spent()) return false;

                continue;
            }

            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) {
                if (spots_.find(spot) == spots_.end()) inotify_rm_watch(fd_, spot);
                continue;
            }

            std::vector<Spot>& sharing = spots_[spot];

            const auto mine =
                std::find_if(sharing.begin(), sharing.end(), [which](const Spot& spot) { return spot.which == which; });

            if (mine != sharing.end()) {
                mine->where = one;
                continue;
            }

            at->second->spots.push_back(spot);
            sharing.push_back(Spot{which, one});
        }

        return true;
    }

    // Katalog wyniesiony poza obserwowane drzewo dalej ma nasze deskryptory — jego i-węzeł żyje
    // gdzie indziej. Zostawione, zgłaszałyby tamte zmiany pod ścieżką w naszym drzewie.
    void pruned(std::uint64_t which, const path& where) {
        std::vector<int> spare;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = posts_.find(which);
            if (at == posts_.end()) return;

            std::vector<int>& mine = at->second->spots;

            for (auto one = mine.begin(); one != mine.end();) {
                const auto found = spots_.find(*one);

                const bool inside =
                    found != spots_.end() &&
                    std::any_of(found->second.begin(), found->second.end(), [&](const Spot& spot) {
                        if (spot.which != which) return false;

                        const auto [left, right] =
                            std::mismatch(where.begin(), where.end(), spot.where.begin(), spot.where.end());

                        return left == where.end();
                    });

                if (!inside) {
                    ++one;
                    continue;
                }

                if (released(*one, which)) spare.push_back(*one);

                one = mine.erase(one);
            }
        }

        for (const int one : spare) inotify_rm_watch(fd_, one);
    }

    void spilled() {
        std::vector<std::pair<std::uint64_t, path>> among;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            for (const auto& [which, post] : posts_) among.emplace_back(which, post->where);
        }

        for (const auto& [which, where] : among)
            reached(which, std::vector<Change>{Change{where, path{}, Change::Kind::overflow}});
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

    // Fałsz — katalog jest, ale system nie umie o nim mówić: udział na serwerze bez powiadomień
    // (część NAS-ów, WebDAV, systemy FUSE pod WinFsp) odpowiada `ERROR_INVALID_FUNCTION` albo
    // `ERROR_NOT_SUPPORTED`. Wtedy zaglądamy sami, zamiast odmawiać obserwacji.
    [[nodiscard]] bool add(std::uint64_t which, const path& where, bool deep) {
        auto post = std::make_shared<Post>();

        post->where = where;
        post->deep = deep;

        post->handle = CreateFileW(longly(where).c_str(), FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

        if (post->handle == INVALID_HANDLE_VALUE) amiss("Cannot watch.", where);

        post->which = which;

        if (CreateIoCompletionPort(post->handle, port_, static_cast<ULONG_PTR>(which), 0) ==
            nullptr) {
            CloseHandle(post->handle);
            amiss("Cannot watch.", where);
        }

        {
            const std::lock_guard<std::mutex> kept{guard_};

            posts_[which] = post;
        }

        if (armed(*post)) return true;

        // Uzbrojenie się nie udało, więc w locie nic nie ma i uchwyt wolno zamknąć od razu.
        {
            const std::lock_guard<std::mutex> kept{guard_};

            posts_.erase(which);
        }

        CloseHandle(post->handle);

        return false;
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
            bool ended = false;

            {
                const std::lock_guard<std::mutex> kept{guard_};

                const auto at = posts_.find(static_cast<std::uint64_t>(key));
                if (at == posts_.end()) continue;

                post = at->second;

                if (post->going || fine == 0) {
                    CloseHandle(post->handle);
                    posts_.erase(at);

                    ended = true;
                }
            }

            // Nie my zerwaliśmy odczyt, tylko system: udział się rozłączył (`ERROR_NETNAME_DELETED`),
            // katalog zniknął. Obserwacja nie może po prostu zamilknąć — przechodzi na zaglądanie
            // i mówi, że zgubiła rachubę.
            if (ended) {
                if (!post->going) forsaken(post->which);

                continue;
            }

            // Zerowy odczyt znaczy, że systemowi zabrakło miejsca na zdarzenia. Nie ma czego
            // odzyskiwać — zostaje przyznanie się i prośba o ponowne przeczytanie katalogu.
            if (given == 0)
                reached(post->which,
                        std::vector<Change>{Change{post->where, path{}, Change::Kind::overflow}});
            else reached(post->which, take(*post, given));

            // Ponowne uzbrojenie odmówiło — w locie nic nie ma, więc uchwyt zamykamy tu, bez
            // czekania na zakończenie, które nie przyjdzie.
            if (!armed(*post)) {
                {
                    const std::lock_guard<std::mutex> kept{guard_};

                    posts_.erase(post->which);
                }

                CloseHandle(post->handle);

                forsaken(post->which);
            }
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
                    among.push_back(Change{where, path{}, Change::Kind::created});
                    break;

                case FILE_ACTION_REMOVED:
                    among.push_back(Change{where, path{}, Change::Kind::removed});
                    break;

                case FILE_ACTION_RENAMED_OLD_NAME:
                    stale = where;
                    break;

                case FILE_ACTION_RENAMED_NEW_NAME:
                    among.push_back(Change{where, stale, Change::Kind::renamed});
                    stale.clear();
                    break;

                default:
                    among.push_back(Change{where, path{}, Change::Kind::modified});
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
//
// Wykaz niesie czas w sekundach, a zapis tej samej długości w tej samej sekundzie, co poprzednie
// spojrzenie, nie zmienia ani sekundy, ani rozmiaru — zmiana by przepadła. Dlatego plik zmieniony
// w ostatniej minucie (`fresh`) pytamy osobno o czas dokładny. Starszych nie: dodatkowe pytanie
// o każdy plik na udziale sieciowym kosztowałoby drugie tyle, a dla nich sekunda wystarcza.
// Minuta, nie dwie sekundy, bo zegar serwera potrafi się rozjechać z naszym.
//
// Sam korzeń stoi w spojrzeniu bez rozmiaru i czasu: jego zniknięcie i powrót są zmianą, a nowy
// plik w środku — nie, tak samo jak w powiadomieniach systemu.
struct Sight {
    Kind kind = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds modified{};
    Instant fine{};
};

constexpr std::chrono::seconds fresh{60};

Instant finely(const path& where, Kind kind, std::chrono::sys_seconds modified,
               std::chrono::system_clock::time_point now) {
    if (kind != Kind::file) return Instant{};

    const auto gap = now - modified;

    if (gap > fresh || gap < -fresh) return Instant{};

    try {
        return instant_at(where);
    } catch (...) {
        return Instant{};
    }
}

std::map<path, Sight> sighted(const path& where, bool deep) {
    std::map<path, Sight> made;

    const Kind what = kind_at(where);
    if (what == Kind::none) return made;

    const auto now = std::chrono::system_clock::now();

    if (what != Kind::folder) {
        Sight mine;
        mine.kind = what;

        std::error_code ignored;
        mine.size = stood::file_size(where, ignored);

        try {
            mine.modified = modified_at(where);
        } catch (...) {
        }

        mine.fine = finely(where, what, mine.modified, now);

        made[where] = mine;

        return made;
    }

    made[where] = Sight{Kind::folder, 0, {}, {}};

    std::vector<Entry> among;

    try {
        among = deep ? walk_at(where) : list_at(where);
    } catch (...) {
        return made;
    }

    for (const Entry& one : among)
        made[one.where] = Sight{one.kind, one.size, one.modified, finely(one.where, one.kind, one.modified, now)};

    return made;
}

std::vector<Change> apart(const std::map<path, Sight>& was, const std::map<path, Sight>& now) {
    std::vector<Change> made;

    for (const auto& [where, mine] : now) {
        const auto at = was.find(where);

        if (at == was.end()) {
            made.push_back(Change{where, path{}, Change::Kind::created});
            continue;
        }

        // Czas dokładny porównujemy tylko wtedy, gdy oba spojrzenia go mają — plik, który właśnie
        // wypadł z ostatniej minuty, nie może udawać zmiany.
        const bool finer = at->second.fine != Instant{} && mine.fine != Instant{} && at->second.fine != mine.fine;

        if (at->second.kind != mine.kind || at->second.size != mine.size ||
            at->second.modified != mine.modified || finer)
            made.push_back(Change{where, path{}, Change::Kind::modified});
    }

    for (const auto& [where, mine] : was)
        if (now.find(where) == now.end()) made.push_back(Change{where, path{}, Change::Kind::removed});

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

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        prompt_.begin();
#endif
    }

    std::uint64_t add(const std::shared_ptr<Watch::Held>& held) {
        Kept kept;

        kept.whose = held;

        bool polled = asking(held->where);

        // Pojedynczego pliku nie obserwujemy wprost. ReadDirectoryChangesW w ogóle tego nie
        // umie, a inotify przywiązuje się do i-węzła, nie do nazwy — zapis przez podmianę
        // zostawiłby obserwację przy usuniętej treści. Pilnujemy więc katalogu nadrzędnego
        // i przy każdym drgnięciu naszej nazwy zaglądamy do samego pliku.
        if (!polled && kind_at(held->where) != Kind::folder) {
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

        kept.where = where;
        kept.deep = deep;

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        if (!polled && !prompt_.add(which, where, deep)) polled = true;
#endif

        if (polled) polling_.add(which, where, deep);

        // Dopiero po udanym dopisaniu — nieudane nie zostawia po sobie wpisu do zdejmowania.
        const std::lock_guard<std::mutex> held_{guard_};

        among_[which] = std::move(kept);

        return which;
    }

    // Zdejmujemy z obu dróg: obserwacja mogła zacząć na zdarzeniach i przejść na zaglądanie
    // (`forsaken`), a zdjęcie z drogi, na której jej nie ma, nic nie robi.
    void drop(std::uint64_t which) {
        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (among_.erase(which) == 0) return;
        }

        polling_.drop(which);

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
        prompt_.drop(which);
#endif
    }

    // Zaplecze zdarzeń oddało obserwację: od teraz zagląda się do niej. Zgłoszenie przepełnienia
    // mówi czekającemu, że coś mogło przepaść w chwili zmiany drogi i warto przeczytać od nowa.
    void forsaken(std::uint64_t which) {
        path where;
        bool deep = false;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            const auto at = among_.find(which);
            if (at == among_.end()) return;

            where = at->second.where;
            deep = at->second.deep;
        }

        polling_.add(which, where, deep);

        // Obserwację zamknięto w trakcie — `drop` szukał jej jeszcze po drodze zdarzeń.
        bool gone = false;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            gone = among_.find(which) == among_.end();
        }

        if (gone) {
            polling_.drop(which);
            return;
        }

        reached(which, std::vector<Change>{Change{where, path{}, Change::Kind::overflow}});
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
        path where;
        bool deep = false;
        bool peering = false;
        path only;
        std::map<path, Sight> seen;
    };

    static bool touched(const std::vector<Change>& among, const path& only) {
        for (const Change& one : among) {
            if (one.kind == Change::Kind::overflow) return true;
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
#elif defined(TALCITE_LINUX_FS)
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

#if !defined(TALCITE_WATCH_POLLING) && (defined(TALCITE_LINUX_FS) || defined(_WIN32))
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

void forsaken(std::uint64_t which) { watching().forsaken(which); }

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
            amiss("Nothing to watch.", where);

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
            [](auto, auto) { amiss("The watch is closed.", path{}); }};

    auto made = std::make_shared<detail::Result<std::vector<Change>>>();

    return Nexting{[held, made](auto, auto) {
                       // Bilet trzyma obserwatorów słabo, więc ogniwo musi mieć właściciela
                       // z tej strony — inaczej ginie zaraz po powrocie stąd, a anulowane
                       // zgłoszenie zostaje w kolejce i połyka pierwszą paczkę.
                       const std::shared_ptr<detail::Waiter> gone =
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

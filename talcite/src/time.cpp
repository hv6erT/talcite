#include "talcite/time.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace talcite {

namespace {

using Moment = std::chrono::steady_clock::time_point;

class Timing;

// Bilet oczekiwania. Postęp jest tu znany dokładnie — minęło tyle z tylu — więc odliczanie
// rysuje pasek za darmo. Chwila startu zapisuje się dopiero przy start(), bo obietnica jest
// leniwa i przed uruchomieniem nie ma czego liczyć.
class Waiting : public detail::Result<void> {
public:
    explicit Waiting(std::chrono::milliseconds whole) : whole_(whole) {}

    void begun(Moment when) {
        began_.store(when.time_since_epoch().count(), std::memory_order_relaxed);
    }

    [[nodiscard]] double progress() const override {
        if (over(*this)) return 1.0;

        const std::int64_t began = began_.load(std::memory_order_relaxed);
        if (began == 0 || whole_.count() <= 0) return 0.0;

        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - Moment{Moment::duration{began}});

        return std::clamp(static_cast<double>(spent.count()) /
                              static_cast<double>(whole_.count()),
                          0.0, 1.0);
    }

private:
    std::atomic<std::int64_t> began_{0};
    std::chrono::milliseconds whole_;
};

// Obietnica, która sięga po chroniony konstruktor Promise, żeby podstawić własny bilet.
class Awaiting : public Promise<void> {
public:
    Awaiting(Promise<void>::Callback given, std::shared_ptr<Waiting> made)
        : Promise<void>(std::move(given), std::move(made)) {}
};

class Dropping : public detail::Waiter {
public:
    Dropping(Timing& whose, std::uint64_t which) : whose_(whose), which_(which) {}

    void woke() override;

private:
    Timing& whose_;
    std::uint64_t which_;
};

// Jeden wątek na całą bibliotekę. Śpi do najbliższego terminu; nowy termin bliższy niż
// obecny budzi go zmienną warunkową. Sto oczekiwań to dalej jeden wątek, a nie sto.
class Timing {
public:
    Timing() = default;

    ~Timing() = default;

    Timing(const Timing&) = delete;
    Timing& operator=(const Timing&) = delete;
    Timing(Timing&&) = delete;
    Timing& operator=(Timing&&) = delete;

    void begin() { keeper_ = std::thread{[this] { serve(); }}; }

    void after(Moment when, std::shared_ptr<detail::Result<void>> result, bool bad) {
        std::shared_ptr<detail::Waiter> watch;
        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> held{guard_};

            which = ++next_;
            watch = std::make_shared<Dropping>(*this, which);

            waiting_.push_back(Entry{which, when, result, watch, bad});
        }

        stirred_.notify_all();

        // Poza zamkiem, bo obietnica już rozstrzygnięta zawoła ogniwo od razu, a ono wraca
        // tutaj po drop() — pod zamkiem byłoby to zakleszczenie na własnym wątku.
        if (detail::hooked(*result, watch)) drop(which);
    }

    void drop(std::uint64_t which) {
        Entry taken;

        {
            const std::lock_guard<std::mutex> held{guard_};

            const auto at = std::find_if(waiting_.begin(), waiting_.end(),
                                         [which](const Entry& one) { return one.which == which; });

            if (at == waiting_.end()) return;

            taken = std::move(*at);
            waiting_.erase(at);
        }

        stirred_.notify_all();
    }

private:
    struct Entry {
        std::uint64_t which = 0;
        Moment when;
        std::shared_ptr<detail::Result<void>> result;
        std::shared_ptr<detail::Waiter> watch;
        bool bad = false;
    };

    void serve() {
        while (true) {
            std::vector<Entry> due;

            {
                std::unique_lock<std::mutex> held{guard_};

                if (waiting_.empty()) {
                    stirred_.wait(held);
                    continue;
                }

                const Moment nearest =
                    std::min_element(waiting_.begin(), waiting_.end(),
                                     [](const Entry& one, const Entry& two) {
                                         return one.when < two.when;
                                     })
                        ->when;

                const Moment now = std::chrono::steady_clock::now();

                if (now < nearest) {
                    stirred_.wait_until(held, nearest);
                    continue;
                }

                for (auto at = waiting_.begin(); at != waiting_.end();) {
                    if (at->when > now) {
                        ++at;
                        continue;
                    }

                    due.push_back(std::move(*at));
                    at = waiting_.erase(at);
                }
            }

            // Rozstrzyganie poza zamkiem: budzi ono obserwatorów, a wśród nich nasze własne
            // ogniwo zdejmujące wpis, które wraca tu po zamek.
            for (Entry& one : due) {
                if (one.bad) detail::spoiled(*one.result, std::make_exception_ptr(Overdue{}));
                else detail::settled(*one.result);
            }
        }
    }

    std::mutex guard_;
    std::condition_variable stirred_;
    std::vector<Entry> waiting_;
    std::uint64_t next_ = 0;
    std::thread keeper_;
};

// Zegar jest celowo nieusuwany. Kolejność niszczenia obiektów statycznych jest odwrotna do
// kolejności powstawania, a `Timing` powstaje później niż `Pool` — więc zniknąłby pierwszy,
// w chwili gdy robotnicy puli jeszcze biegną i mogą go zawołać. Zamiast układać zależność
// między dwoma singletonami, zegar po prostu przeżywa program.
Timing& timing() {
    static Timing* only = [] {
        auto* made = new Timing;
        made->begin();

        return made;
    }();

    return *only;
}

void Dropping::woke() { whose_.drop(which_); }

Promise<void> counted(std::chrono::milliseconds patience, bool bad) {
    auto held = std::make_shared<Waiting>(patience);

    return Awaiting{[patience, held, bad](auto, auto) {
                        const Moment now = std::chrono::steady_clock::now();

                        held->begun(now);
                        timing().after(now + patience, held, bad);
                    },
                    held};
}

}

Overdue::Overdue() : Trouble("Timed out.") {}

Stopwatch::Stopwatch() : began_(std::chrono::steady_clock::now()) {}

std::chrono::milliseconds Stopwatch::spent() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began_);
}

void Stopwatch::reset() { began_ = std::chrono::steady_clock::now(); }

std::chrono::steady_clock::time_point Time::now() { return std::chrono::steady_clock::now(); }

std::chrono::sys_seconds Time::stamp() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

Promise<void> Time::later(std::chrono::milliseconds patience) { return counted(patience, false); }

Promise<void> Time::limit(std::chrono::milliseconds patience) { return counted(patience, true); }

// Termin dla `Promise::timeout`. Nagłówek obietnicy nie może sięgnąć po `time.hpp`, bo ten
// sięga po niego — więc deklaracja stoi w `detail`, a ciało tutaj, przy zegarze.
Promise<void> detail::deadline(std::chrono::milliseconds patience) {
    return counted(patience, true);
}

}

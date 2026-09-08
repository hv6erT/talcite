#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/result.hpp"

namespace talcite {

class Rejected : public std::runtime_error {
public:
    explicit Rejected(const std::string& said) : std::runtime_error(said) {}
};

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Anulowano.") {}

    explicit Cancelled(const std::string& said) : std::runtime_error(said) {}
};

class Reject {
public:
    explicit Reject(std::function<void(std::exception_ptr)> given) : given_(std::move(given)) {}

    void operator()(std::exception_ptr why) const { given_(why); }

    void operator()(const std::string& said) const {
        given_(std::make_exception_ptr(Rejected{said}));
    }

    void operator()(const char* said) const { (*this)(std::string{said}); }

private:
    std::function<void(std::exception_ptr)> given_;
};

template <typename T>
class Promise;

// Obietnica oddana z wnętrza then nie ma być obietnicą w obietnicy. Bez tego łańcuch kroków
// asynchronicznych nie składa się w ogóle: każdy krok dokładałby jedną warstwę opakowania, a
// wołający musiałby ją zdejmować ręcznie — i sam czekać, czyli blokować wątek.
template <typename One>
struct Flattened {
    using type = One;

    static constexpr bool nested = false;
};

template <typename One>
struct Flattened<Promise<One>> {
    using type = One;

    static constexpr bool nested = true;
};

template <typename T>
struct Given {
    using type = std::function<void(T)>;
};

template <>
struct Given<void> {
    using type = std::function<void()>;
};

template <typename T, typename Fn>
struct Yield {
    using type = std::invoke_result_t<Fn, const T&>;
};

template <typename Fn>
struct Yield<void, Fn> {
    using type = std::invoke_result_t<Fn>;
};

// Ogniwo spłaszczające: przepisuje rozstrzygnięcie obietnicy wewnętrznej do zewnętrznej. Trzyma
// tamtą przy życiu, bo obserwatorzy są w bilecie słabi — porzucona obietnica urwałaby łańcuch.
template <typename T>
class Passing : public Waiter {
public:
    Passing(std::shared_ptr<Result<T>> from, std::shared_ptr<Result<T>> to,
            std::shared_ptr<void> kept)
        : from_(std::move(from)), to_(std::move(to)), kept_(std::move(kept)) {}

    void woke() override {
        if (from_->state.load() == State::rejected) {
            spoiled(*to_, from_->why);

            return;
        }

        if constexpr (std::is_void_v<T>) {
            settled(*to_);
        } else {
            settled(*to_, *from_->value);
        }
    }

private:
    std::shared_ptr<Result<T>> from_;
    std::shared_ptr<Result<T>> to_;
    std::shared_ptr<void> kept_;
};

template <typename From, typename Made, typename Fn>
class Carry : public Waiter, public std::enable_shared_from_this<Carry<From, Made, Fn>> {
public:
    using Into = typename Flattened<Made>::type;

    Carry(std::shared_ptr<Result<From>> from, std::shared_ptr<Result<Into>> to, Fn what,
          Loop* where)
        : from_(std::move(from)), to_(std::move(to)), what_(std::move(what)), where_(where) {}

    void woke() override {
        if (where_ == nullptr) {
            carry();
            return;
        }

        auto self = this->shared_from_this();
        where_->post([self] { self->carry(); });
    }

private:
    void carry() {
        if (from_->state.load() == State::rejected) {
            spoiled(*to_, from_->why);
            return;
        }

        try {
            if constexpr (std::is_void_v<Made>) {
                if constexpr (std::is_void_v<From>) what_();
                else what_(*from_->value);

                settled(*to_);
            } else if constexpr (Flattened<Made>::nested) {
                held(std::make_shared<Made>(made()));
            } else {
                if constexpr (std::is_void_v<From>) settled(*to_, what_());
                else settled(*to_, what_(*from_->value));
            }
        } catch (...) {
            spoiled(*to_, std::current_exception());
        }
    }

    [[nodiscard]] Made made() {
        if constexpr (std::is_void_v<From>) return what_();
        else return what_(*from_->value);
    }

    // Obietnica wewnętrzna dopiero teraz rusza — i dopiero jej rozstrzygnięcie kończy zewnętrzną.
    void held(std::shared_ptr<Made> kept) {
        kept->go();

        auto link = std::make_shared<Passing<Into>>(kept->result_, to_, kept);

        keeping_ = link;

        if (hooked(*kept->result_, link)) afterwards(link);
    }

    std::shared_ptr<Result<From>> from_;
    std::shared_ptr<Result<Into>> to_;
    Fn what_;
    std::shared_ptr<void> keeping_;
    Loop* where_ = nullptr;
};

template <typename T>
class Mirror : public Waiter, public std::enable_shared_from_this<Mirror<T>> {
public:
    Mirror(std::shared_ptr<Result<T>> from, std::shared_ptr<Result<T>> to,
           std::function<void(const std::exception&)> amiss, std::function<void()> always,
           Loop* where)
        : from_(std::move(from)), to_(std::move(to)), amiss_(std::move(amiss)),
          always_(std::move(always)), where_(where) {}

    void woke() override {
        if (where_ == nullptr) {
            carry();
            return;
        }

        auto self = this->shared_from_this();
        where_->post([self] { self->carry(); });
    }

private:
    void carry() {
        if (from_->state.load() == State::rejected) {
            if (amiss_) {
                try {
                    std::rethrow_exception(from_->why);
                } catch (const std::exception& why) {
                    amiss_(why);
                } catch (...) {
                    amiss_(Rejected{"Nieznany powód odrzucenia."});
                }
            }

            if (always_) always_();

            spoiled(*to_, from_->why);
            return;
        }

        if (always_) always_();

        if constexpr (std::is_void_v<T>) {
            settled(*to_);
        } else {
            settled(*to_, *from_->value);
        }
    }

    std::shared_ptr<Result<T>> from_;
    std::shared_ptr<Result<T>> to_;
    std::function<void(const std::exception&)> amiss_;
    std::function<void()> always_;
    Loop* where_ = nullptr;
};

template <typename T>
class EasyPromise;

template <typename One>
concept Promised = requires(One one) {
    one.go();
    one.await();
};

template <typename Among>
concept Promises = std::ranges::range<Among> && Promised<std::ranges::range_value_t<Among>>;

enum class Joining : std::uint8_t { all, settled, race, any };

class Gathering {
public:
    Gathering(std::shared_ptr<Result<void>> to, std::size_t wanted, Joining how)
        : to_(std::move(to)), left_(wanted), how_(how) {}

    [[nodiscard]] bool begun() {
        const std::lock_guard<std::mutex> held{guard_};

        if (going_) return true;

        going_ = true;

        return false;
    }

    void keep(std::shared_ptr<Waiter> one) {
        const std::lock_guard<std::mutex> held{guard_};

        if (!over_) kept_.push_back(std::move(one));
    }

    void came(const Ticket& from) {
        std::shared_ptr<Result<void>> to;
        std::exception_ptr why;
        bool bad = false;

        {
            const std::lock_guard<std::mutex> held{guard_};
            if (over_) return;

            const bool spoilt = from.state.load() == State::rejected;

            if (spoilt && !why_) why_ = from.why;

            if (left_ > 0) --left_;

            switch (how_) {
                case Joining::race:
                    bad = spoilt;
                    why = from.why;
                    break;

                case Joining::all:
                    if (spoilt) {
                        bad = true;
                        why = from.why;
                    } else if (left_ != 0) {
                        return;
                    }
                    break;

                case Joining::any:
                    if (!spoilt) break;
                    if (left_ != 0) return;

                    bad = true;
                    why = why_;
                    break;

                case Joining::settled:
                    if (left_ != 0) return;
                    break;
            }

            over_ = true;
            to = to_;
        }

        if (bad) spoiled(*to, why);
        else settled(*to);

        const std::lock_guard<std::mutex> held{guard_};
        kept_.clear();
    }

private:
    std::shared_ptr<Result<void>> to_;
    std::mutex guard_;
    std::vector<std::shared_ptr<Waiter>> kept_;
    std::exception_ptr why_;
    std::size_t left_ = 0;
    Joining how_ = Joining::all;
    bool going_ = false;
    bool over_ = false;
};

class Joined : public Result<void> {
public:
    explicit Joined(Joining how) : how_(how) {}

    void also(std::shared_ptr<Ticket> one) { among_.push_back(std::move(one)); }

    [[nodiscard]] double progress() const override {
        if (over(*this)) return 1.0;
        if (among_.empty()) return 0.0;

        double most = 0.0;
        double whole = 0.0;

        for (const std::shared_ptr<Ticket>& one : among_) {
            const double part = one->progress();

            whole += part;
            most = std::max(most, part);
        }

        if (how_ == Joining::race || how_ == Joining::any) return most;

        return whole / static_cast<double>(among_.size());
    }

private:
    std::vector<std::shared_ptr<Ticket>> among_;
    Joining how_ = Joining::all;
};

inline void afterwards(std::shared_ptr<Waiter> one) {
    pooling().take([kept = std::move(one)] { kept->woke(); });
}

class Arrived : public Waiter {
public:
    Arrived(std::shared_ptr<Ticket> from, std::shared_ptr<Gathering> whole)
        : from_(std::move(from)), whole_(std::move(whole)) {}

    void woke() override { whole_->came(*from_); }

private:
    std::shared_ptr<Ticket> from_;
    std::shared_ptr<Gathering> whole_;
};

template <typename T>
class Promise {
public:
    using Resolve = typename Given<T>::type;
    using Callback = std::function<void(Resolve, Reject)>;

    explicit Promise(Callback given)
        : callback_(std::move(given)), result_(std::make_shared<Result<T>>()) {}

    void go() {
        if (over(*result_)) return;

        if (upstream_) {
            upstream_();
            return;
        }

        if (!callback_) return;

        State want = State::fresh;
        if (!result_->state.compare_exchange_strong(want, State::running)) return;

        auto doing = [callback = callback_, result = result_] {
            // Anulowane, zanim ktokolwiek je podniósł — a jeśli anulowanie trafiło w szparę
            // między zajęciem stanu a oddaniem do puli, to tutaj się o tym dowiemy.
            if (over(*result)) return;

            const Reject reject{[result](std::exception_ptr why) { spoiled(*result, why); }};

            try {
                if constexpr (std::is_void_v<T>) {
                    callback([result] { settled(*result); }, reject);
                } else {
                    callback([result](T given) { settled(*result, std::move(given)); }, reject);
                }
            } catch (...) {
                spoiled(*result, std::current_exception());
            }
        };

        // Numer zadania wpisuje **pula**, pod swoim zamkiem i zanim zadanie stanie się widoczne
        // dla robotników. Zapisany tutaj, po powrocie z `take`, spóźniałby się o całe okno,
        // w którym zadanie już biegnie — i wtedy `cancel()` nie miałby czym w nie trafić.
        (void)(result_->easy.load() ? pooling().easily(std::move(doing), result_->job)
                                    : pooling().take(std::move(doing), result_->job));
    }

    T await() {
        go();

        std::unique_lock<std::mutex> held{result_->guard};
        result_->stirred.wait(held, [this] { return over(*result_); });

        if (result_->state.load() == State::rejected) std::rethrow_exception(result_->why);

        if constexpr (!std::is_void_v<T>) return *result_->value;
    }

    template <typename Fn>
    [[nodiscard]] auto then(Fn what)
        -> Promise<typename Flattened<typename Yield<T, Fn>::type>::type> {
        using Made = typename Yield<T, Fn>::type;
        using Into = typename Flattened<Made>::type;

        Promise<Into> child;

        auto link = std::make_shared<Carry<T, Made, Fn>>(result_, child.result_, std::move(what),
                                                         nullptr);
        child.link_ = link;
        child.upstream_ = upward();

        if (hooked(*result_, link)) afterwards(link);

        return child;
    }

    [[nodiscard]] Promise amiss(std::function<void(const std::exception&)> what) {
        return mirrored(std::move(what), {}, nullptr);
    }

    [[nodiscard]] Promise finally(std::function<void()> what) {
        return mirrored({}, std::move(what), nullptr);
    }

    [[nodiscard]] Promise main() { return mirrored({}, {}, Loop::current()); }

    [[nodiscard]] double progress() const { return result_->progress(); }

    // **Najpierw odrzucenie, potem zdejmowanie z puli**, i ta kolejność jest tu robocza, a nie
    // kosmetyczna. Gdyby numer czytać na początku, robotnik zdążyłby minąć swoje
    // `if (over(*result)) return;` i wejść w ciało, zanim `spoiled` zdąży cokolwiek ustawić —
    // a wtedy nikt by go już nie obudził.
    void cancel() {
        result_->quitted.store(true);
        result_->dropped();

        spoiled(*result_, std::make_exception_ptr(Cancelled{}));

        if (const Pool::Job which = result_->job.load(); which != 0) pooling().drop(which);
    }

    [[nodiscard]] static EasyPromise<T> easy(Promise given);

    template <typename... Rest>
        requires(Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> all(Rest... among) {
        return joined<Joining::all>(std::move(among)...);
    }

    template <typename... Rest>
        requires(Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> all_settled(Rest... among) {
        return joined<Joining::settled>(std::move(among)...);
    }

    template <typename... Rest>
        requires(Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> race(Rest... among) {
        return joined<Joining::race>(std::move(among)...);
    }

    template <typename... Rest>
        requires(Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> any(Rest... among) {
        return joined<Joining::any>(std::move(among)...);
    }

    template <Promises Among>
    [[nodiscard]] static Promise<void> all(Among&& among) {
        return gathered<Joining::all>(std::forward<Among>(among));
    }

    template <Promises Among>
    [[nodiscard]] static Promise<void> all_settled(Among&& among) {
        return gathered<Joining::settled>(std::forward<Among>(among));
    }

    template <Promises Among>
    [[nodiscard]] static Promise<void> race(Among&& among) {
        return gathered<Joining::race>(std::forward<Among>(among));
    }

    template <Promises Among>
    [[nodiscard]] static Promise<void> any(Among&& among) {
        return gathered<Joining::any>(std::forward<Among>(among));
    }

protected:
    Promise(Callback given, std::shared_ptr<Result<T>> made)
        : callback_(std::move(given)), result_(std::move(made)) {}

    void easily() { result_->easy.store(true); }

private:
    template <typename Other> friend class Promise;
    template <typename A, typename B, typename C> friend class Carry;

    Promise() : result_(std::make_shared<Result<T>>()) {}

    template <Joining How, typename... Rest>
    [[nodiscard]] static Promise<void> joined(Rest... among) {
        auto held = std::make_shared<Joined>(How);
        (held->also(among.result_), ...);

        Promise<void> made{typename Promise<void>::Callback{}, held};

        auto whole = std::make_shared<Gathering>(held, sizeof...(Rest), How);

        if constexpr (sizeof...(Rest) == 0) {
            settled(*made.result_);
        } else {
            made.upstream_ = [whole, ... among = among]() mutable {
                if (whole->begun()) return;

                (watched(among, whole), ...);
                (among.go(), ...);
            };
        }

        made.link_ = whole;

        return made;
    }

    template <Joining How, typename Among>
    [[nodiscard]] static Promise<void> gathered(Among&& among) {
        std::vector<std::ranges::range_value_t<Among>> kept{std::ranges::begin(among),
                                                            std::ranges::end(among)};

        auto held = std::make_shared<Joined>(How);
        for (auto& one : kept) held->also(one.result_);

        Promise<void> made{typename Promise<void>::Callback{}, held};

        auto whole = std::make_shared<Gathering>(held, kept.size(), How);

        if (kept.empty()) {
            settled(*made.result_);
        } else {
            made.upstream_ = [whole, kept]() mutable {
                if (whole->begun()) return;

                for (auto& one : kept) watched(one, whole);
                for (auto& one : kept) one.go();
            };
        }

        made.link_ = whole;

        return made;
    }

    template <typename One>
    static void watched(One& one, const std::shared_ptr<Gathering>& whole) {
        auto link = std::make_shared<Arrived>(one.result_, whole);

        whole->keep(link);

        if (hooked(*one.result_, link)) afterwards(link);
    }

    [[nodiscard]] std::function<void()> upward() {
        return [held = *this]() mutable { held.go(); };
    }

    [[nodiscard]] Promise mirrored(std::function<void(const std::exception&)> what,
                                   std::function<void()> always, Loop* where) {
        Promise child;

        auto link = std::make_shared<Mirror<T>>(result_, child.result_, std::move(what),
                                                std::move(always), where);
        child.link_ = link;
        child.upstream_ = upward();

        if (hooked(*result_, link)) afterwards(link);

        return child;
    }

    Callback callback_;
    std::shared_ptr<Result<T>> result_;
    std::shared_ptr<void> link_;
    std::function<void()> upstream_;
};

template <typename T>
class EasyPromise : public Promise<T> {
public:
    explicit EasyPromise(typename Promise<T>::Callback given) : Promise<T>(std::move(given)) {
        this->easily();
    }

    explicit EasyPromise(const Promise<T>& given) : Promise<T>(given) { this->easily(); }
};

template <typename T>
EasyPromise<T> Promise<T>::easy(Promise given) {
    return EasyPromise<T>{given};
}

}

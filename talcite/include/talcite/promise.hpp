#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "talcite/detail/promise.hpp"
#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/trouble.hpp"

namespace talcite {

class Rejected : public Trouble {
public:
    explicit Rejected(const std::string& said) : Trouble(said) {}
};

class Overdue : public Trouble {
public:
    Overdue();
};

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Cancelled.") {}

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
class Promise {
public:
    using Resolve = typename detail::Given<T>::type;
    using Callback = std::function<void(Resolve, Reject)>;

    explicit Promise(Callback given, Lane lane = Lane::io)
        : callback_(kept(std::move(given))), result_(std::make_shared<detail::Result<T>>()) {
        result_->lane = lane;
    }

    Promise(const Promise&) = default;

    Promise(Promise&&) noexcept = default;

    Promise& operator=(Promise other) noexcept {
        std::swap(callback_, other.callback_);
        std::swap(result_, other.result_);
        std::swap(link_, other.link_);
        std::swap(upstream_, other.upstream_);

        return *this;
    }

    ~Promise() { detail::buried(upstream_); }

    void start() {
        if (detail::over(*result_)) return;

        if (upstream_.start) {
            detail::climbed(upstream_.start);
            return;
        }

        if (callback_ == nullptr) return;

        detail::State want = detail::State::fresh;
        if (!result_->state.compare_exchange_strong(want, detail::State::running)) return;

        auto doing = [callback = callback_, result = result_] {
            if (detail::over(*result)) return;

            const Reject reject{
                [result](std::exception_ptr why) { detail::spoiled(*result, why); }};

            try {
                if constexpr (std::is_void_v<T>) {
                    (*callback)([result] { detail::settled(*result); }, reject);
                } else {
                    (*callback)([result](T given) { detail::settled(*result, std::move(given)); },
                             reject);
                }
            } catch (...) {
                detail::spoiled(*result, std::current_exception());
            }
        };

        (void)pooling().post(std::move(doing), result_->lane, result_->job);
    }

    T await() {
        start();

        std::unique_lock<std::mutex> held{result_->guard};

        const auto settled = [this] { return detail::over(*result_); };

        if (Bell* const bell = ringing(); bell == nullptr) {
            result_->stirred.wait(held, settled);
        } else {
            held.unlock();

            const Waiting aside;

            const Bell::Ear was = bell->heed(Bell::Ear{&result_->guard, &result_->stirred});

            held.lock();
            result_->stirred.wait(held, [&] { return settled() || bell->rang(); });
            held.unlock();

            (void)bell->heed(was);

            if (!detail::over(*result_)) cancel();

            held.lock();
            result_->stirred.wait(held, settled);
        }

        if (result_->state.load() == detail::State::rejected) std::rethrow_exception(result_->why);

        if constexpr (!std::is_void_v<T>) return *result_->value;
    }

    template <typename Fn>
    [[nodiscard]] auto then(Fn what)
        -> Promise<typename detail::Flattened<typename detail::Yield<T, Fn>::type>::type> {
        using Made = typename detail::Yield<T, Fn>::type;
        using Into = typename detail::Flattened<Made>::type;

        Promise<Into> child;

        auto link = std::make_shared<detail::Carry<T, Made, Fn>>(result_, child.result_, std::move(what));
        child.link_ = link;
        child.upstream_ = upward();
        child.upstream_.cancel = [held = shared(), link] {
            link->cancel();
            held->cancel();
        };

        if (detail::hooked(*result_, link)) detail::afterwards(link);

        return child;
    }

    [[nodiscard]] Promise fail(std::function<void(const std::exception&)> what) {
        return mirrored(std::move(what), {}, std::nullopt, {});
    }

    [[nodiscard]] Promise finally(std::function<void()> what) {
        return mirrored({}, std::move(what), std::nullopt, {});
    }

    [[nodiscard]] Promise on(Loop& where) { return mirrored({}, {}, where.door_, {}); }

    [[nodiscard]] Promise on(Lane lane) { return mirrored({}, {}, std::nullopt, lane); }

    [[nodiscard]] Promise here() {
        Loop* where = Loop::current();

        if (where == nullptr)
            throw std::logic_error("talcite::Promise::here: no loop on this thread");

        return on(*where);
    }

    [[nodiscard]] Promise shielded() {
        Promise child = mirrored({}, {}, std::nullopt, {});
        child.upstream_.cancel = {};

        return child;
    }

    [[nodiscard]] Promise timeout(std::chrono::milliseconds patience) {
        Promise child;
        Promise<void> clock = detail::deadline(patience);

        auto passed = std::make_shared<detail::Passing<T>>(result_, child.result_, nullptr);
        auto lapsed = std::make_shared<detail::Lapse>(clock.result_, child.result_);
        auto ended = std::make_shared<detail::Epilogue>([held = shared(), clock]() mutable {
            clock.cancel();
            held->cancel();
        });

        child.link_ = std::make_shared<std::vector<std::shared_ptr<detail::Waiter>>>(
            std::vector<std::shared_ptr<detail::Waiter>>{passed, lapsed, ended});

        child.upstream_.start = [held = shared(), clock]() mutable {
            clock.start();
            held->start();
        };

        if (detail::hooked(*child.result_, ended)) detail::afterwards(ended);
        if (detail::hooked(*result_, passed)) detail::afterwards(passed);
        if (detail::hooked(*clock.result_, lapsed)) detail::afterwards(lapsed);

        return child;
    }

    [[nodiscard]] double progress() const { return result_->progress(); }

    void cancel() {
        if (detail::over(*result_)) return;

        result_->quitted.store(true);
        result_->dropped();

        detail::spoiled(*result_, std::make_exception_ptr(Cancelled{}));

        if (const Pool::Job which = result_->job.load(); which != 0) pooling().drop(which);

        if (upstream_.cancel) detail::climbed(upstream_.cancel);
    }

    template <typename... Rest>
        requires(detail::Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> all(Rest... among) {
        return joined<detail::Joining::all>(std::move(among)...);
    }

    template <typename... Rest>
        requires(detail::Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> all_settled(Rest... among) {
        return joined<detail::Joining::settled>(std::move(among)...);
    }

    template <typename... Rest>
        requires(detail::Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> race(Rest... among) {
        return joined<detail::Joining::race>(std::move(among)...);
    }

    template <typename... Rest>
        requires(detail::Promised<Rest> && ...)
    [[nodiscard]] static Promise<void> any(Rest... among) {
        return joined<detail::Joining::any>(std::move(among)...);
    }

    template <detail::Promises Among>
    [[nodiscard]] static Promise<void> all(Among&& among) {
        return gathered<detail::Joining::all>(std::forward<Among>(among));
    }

    template <detail::Promises Among>
    [[nodiscard]] static Promise<void> all_settled(Among&& among) {
        return gathered<detail::Joining::settled>(std::forward<Among>(among));
    }

    template <detail::Promises Among>
    [[nodiscard]] static Promise<void> race(Among&& among) {
        return gathered<detail::Joining::race>(std::forward<Among>(among));
    }

    template <detail::Promises Among>
    [[nodiscard]] static Promise<void> any(Among&& among) {
        return gathered<detail::Joining::any>(std::forward<Among>(among));
    }

protected:
    Promise(Callback given, std::shared_ptr<detail::Result<T>> made)
        : callback_(kept(std::move(given))), result_(std::move(made)) {}

private:
    template <typename Other> friend class Promise;
    template <typename A, typename B, typename C> friend class detail::Carry;

    Promise() : result_(std::make_shared<detail::Result<T>>()) {}

    template <detail::Joining How, typename... Rest>
    [[nodiscard]] static Promise<void> joined(Rest... among) {
        auto held = std::make_shared<detail::Joined>(How);
        (held->also(among.result_), ...);

        Promise<void> made{typename Promise<void>::Callback{}, held};

        auto whole = std::make_shared<detail::Gathering>(held, sizeof...(Rest), How);

        if constexpr (sizeof...(Rest) == 0) {
            detail::settled(*made.result_);
        } else {
            made.upstream_.start = [whole, ... among = among]() mutable {
                if (whole->begun()) return;

                (watched(among, whole), ...);
                (among.start(), ...);
            };

            made.upstream_.cancel = [... among = among]() mutable { (among.cancel(), ...); };
        }

        made.link_ = whole;

        return made;
    }

    template <detail::Joining How, typename Among>
    [[nodiscard]] static Promise<void> gathered(Among&& among) {
        std::vector<std::ranges::range_value_t<Among>> kept{std::ranges::begin(among),
                                                            std::ranges::end(among)};

        auto held = std::make_shared<detail::Joined>(How);
        for (auto& one : kept) held->also(one.result_);

        Promise<void> made{typename Promise<void>::Callback{}, held};

        auto whole = std::make_shared<detail::Gathering>(held, kept.size(), How);

        if (kept.empty()) {
            detail::settled(*made.result_);
        } else {
            made.upstream_.start = [whole, kept]() mutable {
                if (whole->begun()) return;

                for (auto& one : kept) watched(one, whole);
                for (auto& one : kept) one.start();
            };

            made.upstream_.cancel = [kept]() mutable {
                for (auto& one : kept) one.cancel();
            };
        }

        made.link_ = whole;

        return made;
    }

    template <typename One>
    static void watched(One& one, const std::shared_ptr<detail::Gathering>& whole) {
        auto link = std::make_shared<detail::Arrived>(one.result_, whole);

        whole->keep(link);

        if (detail::hooked(*one.result_, link)) detail::afterwards(link);
    }

    [[nodiscard]] static std::shared_ptr<const Callback> kept(Callback given) {
        if (!given) return nullptr;

        return std::make_shared<const Callback>(std::move(given));
    }

    [[nodiscard]] std::shared_ptr<Promise> shared() const { return std::make_shared<Promise>(*this); }

    [[nodiscard]] detail::Upward upward() const {
        const std::shared_ptr<Promise> held = shared();

        return {[held] { held->start(); }, [held] { held->cancel(); }};
    }

    [[nodiscard]] Promise mirrored(std::function<void(const std::exception&)> what,
                                   std::function<void()> always,
                                   std::optional<std::weak_ptr<detail::Door>> where,
                                   std::optional<Lane> lane) {
        Promise child;

        auto link = std::make_shared<detail::Mirror<T>>(result_, child.result_, std::move(what),
                                                        std::move(always), std::move(where), lane);
        child.link_ = link;
        child.upstream_ = upward();

        if (detail::hooked(*result_, link)) detail::afterwards(link);

        return child;
    }

    std::shared_ptr<const Callback> callback_;
    std::shared_ptr<detail::Result<T>> result_;
    std::shared_ptr<void> link_;
    detail::Upward upstream_;
};

}

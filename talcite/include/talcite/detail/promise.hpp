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
#include <type_traits>
#include <utility>
#include <vector>

#include "talcite/detail/result.hpp"
#include "talcite/loop.hpp"
#include "talcite/pool.hpp"

namespace talcite {

template <typename T>
class Promise;

}

namespace talcite::detail {

inline constexpr std::chrono::milliseconds glance{10};

[[nodiscard]] const std::exception& unknown();

[[nodiscard]] Promise<void> deadline(std::chrono::milliseconds patience);

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

    Carry(std::shared_ptr<Result<From>> from, std::shared_ptr<Result<Into>> to, Fn what)
        : from_(std::move(from)), to_(std::move(to)), what_(std::move(what)) {}

    void woke() override { carry(); }

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

    void held(std::shared_ptr<Made> kept) {
        {
            const std::lock_guard<std::mutex> guard{inner_guard_};

            if (over(*to_)) return;

            inner_ = [kept] { kept->cancel(); };
        }

        kept->start();

        auto link = std::make_shared<Passing<Into>>(kept->result_, to_, kept);

        keeping_ = link;

        if (hooked(*kept->result_, link)) afterwards(link);
    }

public:
    void cancel() {
        std::function<void()> inner;

        {
            const std::lock_guard<std::mutex> guard{inner_guard_};

            inner.swap(inner_);
        }

        if (inner) inner();
    }

private:
    std::shared_ptr<Result<From>> from_;
    std::shared_ptr<Result<Into>> to_;
    Fn what_;
    std::shared_ptr<void> keeping_;
    std::mutex inner_guard_;
    std::function<void()> inner_;
};

template <typename T>
class Mirror : public Waiter, public std::enable_shared_from_this<Mirror<T>> {
public:
    Mirror(std::shared_ptr<Result<T>> from, std::shared_ptr<Result<T>> to,
           std::function<void(const std::exception&)> fail, std::function<void()> always,
           std::optional<std::weak_ptr<Door>> where, std::optional<Lane> lane)
        : from_(std::move(from)), to_(std::move(to)), fail_(std::move(fail)),
          always_(std::move(always)), where_(std::move(where)), lane_(lane) {}

    void woke() override {
        if (!where_ && !lane_) {
            carry();
            return;
        }

        auto self = this->shared_from_this();

        if (!where_) {
            pooling().post([self] { self->carry(); }, *lane_);
            return;
        }

        if (const std::shared_ptr<Door> door = where_->lock()) door->post([self] { self->carry(); });
    }

private:
    void carry() {
        if (from_->state.load() == State::rejected) {
            if (fail_) {
                try {
                    std::rethrow_exception(from_->why);
                } catch (const std::exception& why) {
                    fail_(why);
                } catch (...) {
                    fail_(unknown());
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
    std::function<void(const std::exception&)> fail_;
    std::function<void()> always_;
    std::optional<std::weak_ptr<Door>> where_;
    std::optional<Lane> lane_;
};

template <typename One>
concept Promised = requires(One one) {
    one.start();
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
    pooling().post([kept = std::move(one)] { kept->woke(); });
}

class Arrived : public Waiter {
public:
    Arrived(std::shared_ptr<Ticket> from, const std::shared_ptr<Gathering>& whole)
        : from_(std::move(from)), whole_(whole) {}

    void woke() override {
        if (const std::shared_ptr<Gathering> kept = whole_.lock()) kept->came(*from_);
    }

private:
    std::shared_ptr<Ticket> from_;
    std::weak_ptr<Gathering> whole_;
};

class Lapse : public Waiter {
public:
    Lapse(std::shared_ptr<Ticket> from, std::shared_ptr<Ticket> to)
        : from_(std::move(from)), to_(std::move(to)) {}

    void woke() override { spoiled(*to_, from_->why); }

private:
    std::shared_ptr<Ticket> from_;
    std::shared_ptr<Ticket> to_;
};

class Epilogue : public Waiter {
public:
    explicit Epilogue(std::function<void()> what) : what_(std::move(what)) {}

    void woke() override { what_(); }

private:
    std::function<void()> what_;
};

struct Upward {
    std::function<void()> start;
    std::function<void()> cancel;
};

void buried(Upward& gone);

void climbed(const std::function<void()>& step);

}

#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "talcite/clock.hpp"
#include "talcite/detail/promise.hpp"
#include "talcite/place.hpp"
#include "talcite/pool.hpp"
#include "talcite/progress.hpp"
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

class Broken : public Trouble {
public:
    Broken();
};

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("Cancelled.") {}

    explicit Cancelled(const std::string& said) : std::runtime_error(said) {}
};

template <typename T>
class Promise;

template <typename T>
class SharedPromise;

template <typename T>
class Settle;

class Stop;

namespace detail {

struct Access;

template <typename T>
class Frame;

template <typename T>
struct Flat {
    using type = T;
};

template <typename T>
struct Flat<Promise<T>> {
    using type = T;
};

template <typename T>
struct Flat<SharedPromise<T>> {
    using type = T;
};

template <typename T>
inline constexpr bool promised_v = false;

template <typename T>
inline constexpr bool promised_v<Promise<T>> = true;

template <typename T>
inline constexpr bool promised_v<SharedPromise<T>> = true;

template <typename T>
using Kept = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template <typename T, typename F>
struct Called {
    using type = std::invoke_result_t<F&, T&&>;
};

template <typename F>
struct Called<void, F> {
    using type = std::invoke_result_t<F&>;
};

const Stop*& current_stop();

Place& here_loop();

[[nodiscard]] bool homed(const std::shared_ptr<Gate>& home);

}

template <typename T>
class Outcome {
public:
    [[nodiscard]] bool ok() const { return how_ == detail::Ending::value; }

    [[nodiscard]] bool cancelled() const { return how_ == detail::Ending::cancelled; }

    [[nodiscard]] const T& value() const& {
        thrown();

        return *value_;
    }

    [[nodiscard]] T value() && {
        thrown();

        return std::move(*value_);
    }

    [[nodiscard]] const std::exception_ptr& error() const { return why_; }

private:
    friend struct detail::Access;

    void thrown() const {
        if (how_ == detail::Ending::error) std::rethrow_exception(why_);
        if (how_ == detail::Ending::cancelled) throw Cancelled{};
    }

    detail::Ending how_ = detail::Ending::cancelled;
    std::optional<T> value_;
    std::exception_ptr why_;
};

template <>
class Outcome<void> {
public:
    [[nodiscard]] bool ok() const { return how_ == detail::Ending::value; }

    [[nodiscard]] bool cancelled() const { return how_ == detail::Ending::cancelled; }

    void value() const {
        if (how_ == detail::Ending::error) std::rethrow_exception(why_);
        if (how_ == detail::Ending::cancelled) throw Cancelled{};
    }

    [[nodiscard]] const std::exception_ptr& error() const { return why_; }

private:
    friend struct detail::Access;

    detail::Ending how_ = detail::Ending::cancelled;
    std::exception_ptr why_;
};

class [[nodiscard]] Hook {
public:
    Hook() = default;

    Hook(Hook&& other) noexcept : core_(std::move(other.core_)), which_(std::exchange(other.which_, 0)) {}

    Hook& operator=(Hook&& other) noexcept;

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    ~Hook();

private:
    friend class Stop;

    Hook(std::weak_ptr<detail::Core> core, std::uint64_t which) : core_(std::move(core)), which_(which) {}

    std::weak_ptr<detail::Core> core_;
    std::uint64_t which_ = 0;
};

class Stop {
public:
    using Handle = detail::Heard;

    [[nodiscard]] bool stopped() const;

    [[nodiscard]] bool wait(std::chrono::milliseconds patience) const;

    [[nodiscard]] Handle heard() const;

    [[nodiscard]] Hook when(std::function<void()> then) const;

    [[nodiscard]] static const Stop* current();

private:
    friend struct detail::Access;

    explicit Stop(std::shared_ptr<detail::Core> core) : core_(std::move(core)) {}

    std::shared_ptr<detail::Core> core_;
};

class Work {
public:
    [[nodiscard]] const Stop& stop() const { return stop_; }

    void progress(Progress now) const;

private:
    friend struct detail::Access;

    explicit Work(std::shared_ptr<detail::Core> core);

    Stop stop_;
};

void lost(std::function<void(const std::exception&)> sink);

namespace detail {

class Current {
public:
    explicit Current(const Stop* now) : was_(current_stop()) { current_stop() = now; }

    ~Current() { current_stop() = was_; }

    Current(const Current&) = delete;
    Current& operator=(const Current&) = delete;
    Current(Current&&) = delete;
    Current& operator=(Current&&) = delete;

private:
    const Stop* was_;
};

struct Access {
    template <typename T>
    [[nodiscard]] static Promise<T> made(Claim claim) {
        return Promise<T>{std::move(claim)};
    }

    template <typename T>
    [[nodiscard]] static Claim& claim(Promise<T>& one) {
        return one.claim_;
    }

    template <typename T>
    [[nodiscard]] static Claim& claim(SharedPromise<T>& one) {
        return one.claim_;
    }

    [[nodiscard]] static Stop stop(std::shared_ptr<Core> core) { return Stop{std::move(core)}; }

    [[nodiscard]] static Work work(std::shared_ptr<Core> core) { return Work{std::move(core)}; }

    [[nodiscard]] static Core* core(const Stop& stop) { return stop.core_.get(); }

    template <typename T>
    [[nodiscard]] static Settle<T> settle(std::shared_ptr<Node<T>> node) {
        return Settle<T>{std::move(node)};
    }

    template <typename T>
    [[nodiscard]] static Outcome<T> outcome(Node<T>& from) {
        Outcome<T> made;

        made.how_ = from.ending();
        made.why_ = from.why();

        if constexpr (!std::is_void_v<T>) {
            if (made.how_ == Ending::value) made.value_.emplace(from.taken());
        }

        return made;
    }
};

inline std::uint64_t stopping(const Stop& stop, std::function<void()> then) {
    return Access::core(stop)->on_stop(std::move(then));
}

template <typename T>
[[nodiscard]] std::shared_ptr<Node<T>> node_of(const Claim& claim) {
    return std::static_pointer_cast<Node<T>>(claim.core());
}

template <typename T>
bool passed(Node<T>& from, Node<T>& to, Delivery delivery) {
    if (!to.running()) return false;

    switch (from.ending()) {
    case Ending::value:
        if constexpr (std::is_void_v<T>) {
            return to.valued(delivery);
        } else {
            return to.valued(from.taken(), delivery);
        }
    case Ending::error:
        return to.failed(from.why(), delivery);
    default:
        return to.quit(delivery);
    }
}

template <typename T>
class Passing final : public Reaction {
public:
    Passing(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<T>> to) : from_(std::move(from)), to_(std::move(to)) {}

    void ended() override { (void)passed(*from_, *to_, Delivery::here); }

private:
    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<T>> to_;
};

template <typename T>
void linked(const std::shared_ptr<Core>& to, Claim from, std::shared_ptr<Reaction> reaction) {
    const std::shared_ptr<Core> whose = from.core();

    to->hold();
    whose->when_finished([to] { to->let_go(); });
    to->follow(whose);
    to->dwell(whose->home());

    if (reaction) whose->react(from, std::move(reaction));

    to->keep(std::move(from));
}

template <typename U>
void adopted(const std::shared_ptr<Node<U>>& to, Promise<U> given);

template <typename U>
void adopted(const std::shared_ptr<Node<U>>& to, SharedPromise<U> given);

template <typename R, typename U>
void yielded(const std::shared_ptr<Node<U>>& to, R&& made) {
    if constexpr (promised_v<std::decay_t<R>>) {
        adopted(to, std::forward<R>(made));
    } else if constexpr (std::is_void_v<U>) {
        (void)to->valued(Delivery::here);
    } else {
        (void)to->valued(U(std::forward<R>(made)), Delivery::here);
    }
}

template <typename U, typename F, typename... A>
void produced(const std::shared_ptr<Node<U>>& to, F& f, A&&... given) {
    using R = std::invoke_result_t<F&, A...>;

    to->hold();

    try {
        if constexpr (std::is_void_v<R>) {
            std::invoke(f, std::forward<A>(given)...);

            (void)to->valued(Delivery::here);
        } else {
            yielded<R, U>(to, std::invoke(f, std::forward<A>(given)...));
        }
    } catch (...) {
        (void)to->failed(caught(), Delivery::here);
    }

    to->let_go();
}

template <typename T, typename U, typename F>
class Thening final : public Reaction {
public:
    Thening(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<U>> to, F f)
        : from_(std::move(from)), to_(std::move(to)), f_(std::move(f)) {}

    void ended() override {
        to_->hold();

        if (!to_->running()) {
            to_->let_go();

            return;
        }

        switch (from_->ending()) {
        case Ending::value:
            if constexpr (std::is_void_v<T>) {
                produced<U>(to_, f_);
            } else {
                produced<U>(to_, f_, from_->taken());
            }
            break;
        case Ending::error:
            (void)to_->failed(from_->why(), Delivery::here);
            break;
        default:
            (void)to_->quit(Delivery::here);
            break;
        }

        to_->let_go();
    }

private:
    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<U>> to_;
    F f_;
};

template <typename T, typename F>
class Recovering final : public Reaction {
public:
    Recovering(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<T>> to, F f)
        : from_(std::move(from)), to_(std::move(to)), f_(std::move(f)) {}

    void ended() override {
        to_->hold();

        if (!to_->running()) {
            to_->let_go();

            return;
        }

        if (from_->ending() != Ending::error) {
            (void)passed(*from_, *to_, Delivery::here);
        } else {
            try {
                std::rethrow_exception(from_->why());
            } catch (const std::exception& why) {
                produced<T>(to_, f_, why);
            } catch (...) {
                (void)to_->failed(caught(), Delivery::here);
            }
        }

        to_->let_go();
    }

private:
    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<T>> to_;
    F f_;
};

template <typename T, typename F>
class Concluding final : public std::enable_shared_from_this<Concluding<T, F>> {
public:
    Concluding(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<T>> to, F f)
        : from_(std::move(from)), to_(std::move(to)), f_(std::move(f)) {}

    void finished() {
        const std::shared_ptr<Gate> home = to_->home();

        if (!to_->running()) {
            if (home != nullptr && posted(home, [self = this->shared_from_this()] { self->late(); })) return;

            away([self = this->shared_from_this()] { self->late(); });

            return;
        }

        if (home != nullptr && !homed(home) && posted(home, [self = this->shared_from_this()] { self->concluded(); })) return;

        concluded();
    }

private:
    void concluded() {
        if (!to_->running()) {
            late();

            return;
        }

        try {
            f_();
        } catch (...) {
            if (!to_->failed(caught(), Delivery::here)) lose(caught());

            to_->let_go();

            return;
        }

        (void)passed(*from_, *to_, Delivery::here);

        to_->let_go();
    }

    void late() {
        try {
            f_();
        } catch (...) {
            if (!to_->failed(caught(), Delivery::here)) lose(caught());
        }

        to_->let_go();
    }

    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<T>> to_;
    F f_;
};

template <typename T>
class Hopping final : public Reaction {
public:
    Hopping(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<T>> to, std::shared_ptr<Gate> where)
        : from_(std::move(from)), to_(std::move(to)), where_(std::move(where)) {}

    void ended() override {
        to_->hold();

        if (!to_->running()) {
            to_->let_go();

            return;
        }

        const bool taken = posted(where_, [from = from_, to = to_] {
            (void)passed(*from, *to, Delivery::here);

            to->let_go();
        });

        if (taken) return;

        (void)to_->quit(Delivery::here);

        to_->let_go();
    }

private:
    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<T>> to_;
    std::shared_ptr<Gate> where_;
};

template <typename T>
class Reporting final : public Reaction {
public:
    Reporting(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<Outcome<T>>> to) : from_(std::move(from)), to_(std::move(to)) {}

    void ended() override {
        if (!to_->running()) return;

        (void)to_->valued(Access::outcome(*from_), Delivery::here);
    }

private:
    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<Outcome<T>>> to_;
};

[[nodiscard]] std::uint64_t timed(const std::shared_ptr<Core>& to, Clock& clock, std::chrono::milliseconds patience);

template <typename T>
class Timing final : public Reaction {
public:
    Timing(std::shared_ptr<Node<T>> from, std::shared_ptr<Node<T>> to, Clock& clock, std::uint64_t which)
        : from_(std::move(from)), to_(std::move(to)), clock_(&clock), which_(which) {}

    void ended() override {
        (void)clock_->unscheduled(which_);

        (void)passed(*from_, *to_, Delivery::here);
    }

private:
    std::shared_ptr<Node<T>> from_;
    std::shared_ptr<Node<T>> to_;
    Clock* clock_;
    std::uint64_t which_;
};

void launched(Place& where, const std::shared_ptr<Core>& node, std::function<void()> job);

template <typename U, typename F>
class Running {
public:
    Running(std::shared_ptr<Node<U>> node, F body) : node_(std::move(node)), body_(std::move(body)) {}

    void run() {
        if (!node_->begin(worker_bell())) {
            node_->let_go();

            return;
        }

        {
            Work work = Access::work(node_);
            const Current now{&work.stop()};

            if constexpr (std::is_invocable_v<F&, Work&>) {
                produced<U>(node_, body_, work);
            } else {
                produced<U>(node_, body_);
            }

            node_->unlend();
        }

        node_->let_go();
    }

private:
    std::shared_ptr<Node<U>> node_;
    F body_;
};

}

template <typename T>
class [[nodiscard]] Promise {
    static_assert(!detail::promised_v<T> && !std::is_reference_v<T>, "a promise of a promise or a reference");

public:
    using value_type = T;
    using promise_type = detail::Frame<T>;

    Promise() = default;

    Promise(Promise&&) noexcept = default;

    Promise& operator=(Promise&&) noexcept = default;

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    ~Promise() = default;

    [[nodiscard]] explicit operator bool() const { return static_cast<bool>(claim_); }

    template <typename F>
    [[nodiscard]] auto then(F f) && {
        using U = typename detail::Flat<std::remove_cvref_t<typename detail::Called<T, F>::type>>::type;

        auto from = taken();
        auto to = std::make_shared<detail::Node<U>>(0);
        Promise<U> made = detail::Access::made<U>(detail::Core::claimed(to));

        detail::linked<T>(to, std::move(claim_), std::make_shared<detail::Thening<T, U, F>>(from, to, std::move(f)));

        return made;
    }

    template <typename F>
    [[nodiscard]] Promise recover(F f) && {
        auto from = taken();
        auto to = std::make_shared<detail::Node<T>>(0);
        Promise made{detail::Core::claimed(to)};

        detail::linked<T>(to, std::move(claim_), std::make_shared<detail::Recovering<T, F>>(from, to, std::move(f)));

        return made;
    }

    template <typename F>
    [[nodiscard]] Promise finally(F f) && {
        auto from = taken();
        auto to = std::make_shared<detail::Node<T>>(0);
        Promise made{detail::Core::claimed(to)};

        auto closing = std::make_shared<detail::Concluding<T, F>>(from, to, std::move(f));

        to->hold();
        detail::linked<T>(to, std::move(claim_), nullptr);
        from->when_finished([closing] { closing->finished(); });

        return made;
    }

    [[nodiscard]] Promise on(Place& where) && {
        auto from = taken();
        auto to = std::make_shared<detail::Node<T>>(0);
        Promise made{detail::Core::claimed(to)};

        detail::linked<T>(to, std::move(claim_), std::make_shared<detail::Hopping<T>>(from, to, where.gate()));
        to->dwell(where.gate());

        return made;
    }

    [[nodiscard]] Promise here() &&;

    [[nodiscard]] Promise timeout(std::chrono::milliseconds patience, Clock& clock = Clock::real()) && {
        auto from = taken();
        auto to = std::make_shared<detail::Node<T>>(0);
        Promise made{detail::Core::claimed(to)};

        const std::uint64_t which = detail::timed(to, clock, patience);

        detail::linked<T>(to, std::move(claim_), std::make_shared<detail::Timing<T>>(from, to, clock, which));

        return made;
    }

    [[nodiscard]] Promise<Outcome<T>> outcome() && {
        auto from = taken();
        auto to = std::make_shared<detail::Node<Outcome<T>>>(0);
        Promise<Outcome<T>> made = detail::Access::made<Outcome<T>>(detail::Core::claimed(to));

        detail::linked<T>(to, std::move(claim_), std::make_shared<detail::Reporting<T>>(from, to));

        return made;
    }

    [[nodiscard]] SharedPromise<T> share() &&;

    void detach() && {
        auto from = taken();

        from->detach();

        claim_.release();
    }

    void cancel() { claim_.release(); }

    T get() && {
        auto from = taken();
        detail::Claim kept = std::move(claim_);

        const detail::Ending how = waited(*from);

        if (how == detail::Ending::running) {
            kept.release();

            throw Cancelled{};
        }

        if (how == detail::Ending::error) std::rethrow_exception(from->why());
        if (how == detail::Ending::cancelled) throw Cancelled{};

        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return from->taken();
        }
    }

    [[nodiscard]] bool ready() const { return claim_ && !claim_.get()->running(); }

    [[nodiscard]] Progress progress() const { return claim_ ? claim_.get()->progress() : Progress{}; }

    [[nodiscard]] auto operator co_await() &&;

private:
    friend struct detail::Access;

    explicit Promise(detail::Claim claim) : claim_(std::move(claim)) {}

    [[nodiscard]] std::shared_ptr<detail::Node<T>> taken() const {
        if (!claim_) throw std::logic_error("talcite::Promise: an empty promise");

        return detail::node_of<T>(claim_);
    }

    [[nodiscard]] static detail::Ending waited(detail::Node<T>& from) {
        const Stop* now = Stop::current();
        const Waiting aside;

        return from.waited(now == nullptr ? nullptr : detail::Access::core(*now));
    }

    detail::Claim claim_;
};

template <typename T>
class [[nodiscard]] SharedPromise {
public:
    SharedPromise() = default;

    SharedPromise(const SharedPromise& other)
        : claim_(other.claim_ ? detail::Core::claimed(other.claim_.core()) : detail::Claim{}) {}

    SharedPromise(SharedPromise&&) noexcept = default;

    SharedPromise& operator=(SharedPromise other) noexcept {
        claim_ = std::move(other.claim_);

        return *this;
    }

    ~SharedPromise() = default;

    [[nodiscard]] explicit operator bool() const { return static_cast<bool>(claim_); }

    [[nodiscard]] Promise<T> view() const {
        auto from = taken();
        auto to = std::make_shared<detail::Node<T>>(0);
        Promise<T> made = detail::Access::made<T>(detail::Core::claimed(to));

        detail::linked<T>(to, detail::Core::claimed(from), std::make_shared<detail::Passing<T>>(from, to));

        return made;
    }

    decltype(auto) get() const {
        auto from = taken();
        const Stop* now = Stop::current();

        detail::Ending how = detail::Ending::running;

        {
            const Waiting aside;

            how = from->waited(now == nullptr ? nullptr : detail::Access::core(*now));
        }

        if (how == detail::Ending::running) throw Cancelled{};
        if (how == detail::Ending::error) std::rethrow_exception(from->why());
        if (how == detail::Ending::cancelled) throw Cancelled{};

        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return static_cast<const T&>(from->peeked());
        }
    }

    [[nodiscard]] bool ready() const { return claim_ && !claim_.get()->running(); }

    [[nodiscard]] Progress progress() const { return claim_ ? claim_.get()->progress() : Progress{}; }

    void cancel() { claim_.release(); }

private:
    friend struct detail::Access;
    friend class Promise<T>;

    explicit SharedPromise(detail::Claim claim) : claim_(std::move(claim)) {}

    [[nodiscard]] std::shared_ptr<detail::Node<T>> taken() const {
        if (!claim_) throw std::logic_error("talcite::SharedPromise: an empty promise");

        return detail::node_of<T>(claim_);
    }

    detail::Claim claim_;
};

template <typename T>
SharedPromise<T> Promise<T>::share() && {
    static_assert(std::is_void_v<T> || std::is_copy_constructible_v<T>, "a shared promise needs a copyable value");

    auto from = taken();

    from->share();

    return SharedPromise<T>{std::move(claim_)};
}

template <typename T>
Promise<T> Promise<T>::here() && {
    return std::move(*this).on(detail::here_loop());
}

template <typename T>
class Settle {
public:
    Settle(Settle&& other) noexcept
        : node_(std::move(other.node_)), stop_(std::move(other.stop_)), done_(other.done_.exchange(true)) {}

    Settle& operator=(Settle&& other) noexcept {
        if (this != &other) {
            abandoned();

            node_ = std::move(other.node_);
            stop_ = std::move(other.stop_);
            done_.store(other.done_.exchange(true));
        }

        return *this;
    }

    Settle(const Settle&) = delete;
    Settle& operator=(const Settle&) = delete;

    ~Settle() { abandoned(); }

    template <typename V = T>
        requires(!std::is_void_v<V>)
    bool value(V given) {
        if (done_.exchange(true)) return false;

        const bool first = node_->valued(std::move(given), detail::Delivery::away);

        node_->let_go_away();

        return first;
    }

    template <typename V = T>
        requires std::is_void_v<V>
    bool value() {
        if (done_.exchange(true)) return false;

        const bool first = node_->valued(detail::Delivery::away);

        node_->let_go_away();

        return first;
    }

    bool fail(std::exception_ptr why) {
        if (done_.exchange(true)) return false;

        const bool first = node_->failed(std::move(why), detail::Delivery::away);

        node_->let_go_away();

        return first;
    }

    bool fail(const std::string& said) { return fail(std::make_exception_ptr(Rejected{said})); }

    void progress(Progress now) const { node_->report(now); }

    [[nodiscard]] const Stop& stop() const { return *stop_; }

    [[nodiscard]] bool settled() const { return done_.load(); }

private:
    friend struct detail::Access;

    explicit Settle(std::shared_ptr<detail::Node<T>> node)
        : node_(std::move(node)), stop_(std::make_unique<Stop>(detail::Access::stop(node_))) {}

    void abandoned() {
        if (node_ == nullptr || done_.exchange(true)) return;

        (void)node_->failed(std::make_exception_ptr(Broken{}), detail::Delivery::away);

        node_->let_go_away();
    }

    std::shared_ptr<detail::Node<T>> node_;
    std::unique_ptr<Stop> stop_;
    std::atomic<bool> done_{false};
};

template <typename T>
[[nodiscard]] std::pair<Promise<T>, Settle<T>> promised() {
    auto node = std::make_shared<detail::Node<T>>(1);

    return {detail::Access::made<T>(detail::Core::claimed(node)), detail::Access::settle<T>(node)};
}

template <typename T>
[[nodiscard]] Promise<std::decay_t<T>> ready(T&& value) {
    using U = std::decay_t<T>;

    auto node = std::make_shared<detail::Node<U>>(0);
    Promise<U> made = detail::Access::made<U>(detail::Core::claimed(node));

    (void)node->valued(std::forward<T>(value), detail::Delivery::here);

    return made;
}

[[nodiscard]] Promise<void> ready();

template <typename T>
[[nodiscard]] Promise<T> failed(std::exception_ptr why) {
    auto node = std::make_shared<detail::Node<T>>(0);
    Promise<T> made = detail::Access::made<T>(detail::Core::claimed(node));

    (void)node->failed(std::move(why), detail::Delivery::here);

    return made;
}

template <typename F>
[[nodiscard]] auto run(Place& where, F body) {
    using R = std::conditional_t<std::is_invocable_v<F&, Work&>, std::invoke_result<F&, Work&>, std::invoke_result<F&>>;
    using U = typename detail::Flat<std::remove_cvref_t<typename R::type>>::type;

    auto node = std::make_shared<detail::Node<U>>(1);
    Promise<U> made = detail::Access::made<U>(detail::Core::claimed(node));
    auto state = std::make_shared<detail::Running<U, F>>(node, std::move(body));

    detail::launched(where, node, [state] { state->run(); });

    return made;
}

[[nodiscard]] Promise<void> later(std::chrono::milliseconds patience, Clock& clock = Clock::real());

namespace detail {

template <typename U>
void adopted(const std::shared_ptr<Node<U>>& to, Promise<U> given) {
    Claim& claim = Access::claim(given);

    if (!claim) {
        (void)to->failed(std::make_exception_ptr(std::logic_error("talcite::Promise: an empty promise")), Delivery::here);

        return;
    }

    auto from = node_of<U>(claim);

    linked<U>(to, std::move(claim), std::make_shared<Passing<U>>(from, to));
}

template <typename U>
void adopted(const std::shared_ptr<Node<U>>& to, SharedPromise<U> given) {
    if (!given) {
        (void)to->failed(std::make_exception_ptr(std::logic_error("talcite::SharedPromise: an empty promise")), Delivery::here);

        return;
    }

    adopted(to, given.view());
}

template <typename T, typename F>
class Joined final : public Reaction {
public:
    Joined(std::shared_ptr<Node<T>> from, F f) : from_(std::move(from)), f_(std::move(f)) {}

    void ended() override { f_(*from_); }

private:
    std::shared_ptr<Node<T>> from_;
    F f_;
};

template <typename T, typename F>
void joined(const std::shared_ptr<Core>& to, Promise<T>& one, F f) {
    Claim& claim = Access::claim(one);

    if (!claim) throw std::logic_error("talcite: an empty promise in a join");

    auto from = node_of<T>(claim);

    linked<T>(to, std::move(claim), std::make_shared<Joined<T, F>>(from, std::move(f)));
}

template <typename T>
class Gathered {
public:
    explicit Gathered(std::size_t many) : got_(many), left_(many) {}

    [[nodiscard]] bool put(std::size_t at, Kept<T> value) {
        const std::lock_guard<std::mutex> held{guard_};

        got_[at].emplace(std::move(value));

        return --left_ == 0;
    }

    [[nodiscard]] std::vector<T> whole() {
        std::vector<T> made;

        const std::lock_guard<std::mutex> held{guard_};

        made.reserve(got_.size());

        for (std::optional<Kept<T>>& one : got_) made.push_back(std::move(*one));

        return made;
    }

private:
    std::mutex guard_;
    std::vector<std::optional<Kept<T>>> got_;
    std::size_t left_;
};

template <typename T>
class Settling {
public:
    explicit Settling(std::size_t many) : got_(many), left_(many) {}

    [[nodiscard]] bool put(std::size_t at, Outcome<T> value) {
        const std::lock_guard<std::mutex> held{guard_};

        got_[at].emplace(std::move(value));

        return --left_ == 0;
    }

    [[nodiscard]] std::vector<Outcome<T>> whole() {
        std::vector<Outcome<T>> made;

        const std::lock_guard<std::mutex> held{guard_};

        made.reserve(got_.size());

        for (std::optional<Outcome<T>>& one : got_) made.push_back(std::move(*one));

        return made;
    }

private:
    std::mutex guard_;
    std::vector<std::optional<Outcome<T>>> got_;
    std::size_t left_;
};

class Counting {
public:
    explicit Counting(std::size_t many) : left_(many) {}

    [[nodiscard]] bool last() {
        const std::lock_guard<std::mutex> held{guard_};

        return --left_ == 0;
    }

private:
    std::mutex guard_;
    std::size_t left_;
};

}

template <typename T>
[[nodiscard]] Promise<std::vector<T>> all(std::vector<Promise<T>> among) {
    static_assert(!std::is_void_v<T>, "all of void promises: use all_done");

    auto to = std::make_shared<detail::Node<std::vector<T>>>(0);
    Promise<std::vector<T>> made = detail::Access::made<std::vector<T>>(detail::Core::claimed(to));

    if (among.empty()) {
        (void)to->valued(std::vector<T>{}, detail::Delivery::here);

        return made;
    }

    std::vector<std::shared_ptr<detail::Core>> summed;

    for (Promise<T>& one : among) summed.push_back(detail::Access::claim(one).core());

    auto got = std::make_shared<detail::Gathered<T>>(among.size());

    for (std::size_t at = 0; at < among.size(); ++at)
        detail::joined<T>(to, among[at], [to, got, at](detail::Node<T>& from) {
            if (!to->running()) return;

            switch (from.ending()) {
            case detail::Ending::value:
                if (got->put(at, from.taken())) (void)to->valued(got->whole(), detail::Delivery::here);
                break;
            case detail::Ending::error:
                (void)to->failed(from.why(), detail::Delivery::here);
                break;
            default:
                (void)to->quit(detail::Delivery::here);
                break;
            }
        });

    to->sum(std::move(summed));

    return made;
}

[[nodiscard]] Promise<void> all(std::vector<Promise<void>> among);

#if defined(_MSC_VER) && !defined(__clang__)
namespace detail {

template <std::size_t I, typename Whole, typename Got, typename T>
void joined_at(const std::shared_ptr<Node<Whole>>& to, const std::shared_ptr<Got>& got,
               const std::shared_ptr<Counting>& left, Promise<T>& one) {
    joined<T>(to, one, [to, got, left](Node<T>& from) {
        if (!to->running()) return;

        switch (from.ending()) {
        case Ending::value:
            if constexpr (std::is_void_v<T>) {
                std::get<I>(*got).emplace();
            } else {
                std::get<I>(*got).emplace(from.taken());
            }

            if (left->last())
                (void)to->valued(std::apply([](auto&... each) { return Whole{std::move(*each)...}; }, *got), Delivery::here);
            break;
        case Ending::error:
            (void)to->failed(from.why(), Delivery::here);
            break;
        default:
            (void)to->quit(Delivery::here);
            break;
        }
    });
}

template <typename Whole, typename Got, std::size_t... I, typename... P>
void joined_each(const std::shared_ptr<Node<Whole>>& to, const std::shared_ptr<Got>& got,
                 const std::shared_ptr<Counting>& left, std::index_sequence<I...>, P&... among) {
    (joined_at<I>(to, got, left, among), ...);
}

}
#endif

template <typename... P>
    requires(sizeof...(P) > 0 && (detail::promised_v<std::decay_t<P>> && ...))
[[nodiscard]] auto all(P... among) {
    using Whole = std::tuple<detail::Kept<typename P::value_type>...>;

    auto to = std::make_shared<detail::Node<Whole>>(0);
    Promise<Whole> made = detail::Access::made<Whole>(detail::Core::claimed(to));

    auto got = std::make_shared<std::tuple<std::optional<detail::Kept<typename P::value_type>>...>>();
    auto left = std::make_shared<detail::Counting>(sizeof...(P));

    std::vector<std::shared_ptr<detail::Core>> summed{detail::Access::claim(among).core()...};

#if defined(_MSC_VER) && !defined(__clang__)
    detail::joined_each(to, got, left, std::index_sequence_for<P...>{}, among...);
#else
    const auto each = [&]<std::size_t... I>(std::index_sequence<I...>) {
        (detail::joined<typename P::value_type>(
             to, among, [to, got, left](detail::Node<typename P::value_type>& from) {
                 if (!to->running()) return;

                 switch (from.ending()) {
                 case detail::Ending::value:
                     if constexpr (std::is_void_v<typename P::value_type>) {
                         std::get<I>(*got).emplace();
                     } else {
                         std::get<I>(*got).emplace(from.taken());
                     }

                     if (left->last())
                         (void)to->valued(std::apply([](auto&... one) { return Whole{std::move(*one)...}; }, *got),
                                          detail::Delivery::here);
                     break;
                 case detail::Ending::error:
                     (void)to->failed(from.why(), detail::Delivery::here);
                     break;
                 default:
                     (void)to->quit(detail::Delivery::here);
                     break;
                 }
             }),
         ...);
    };

    each(std::index_sequence_for<P...>{});
#endif

    to->sum(std::move(summed));

    return made;
}

template <typename T>
[[nodiscard]] Promise<T> any(std::vector<Promise<T>> among) {
    auto to = std::make_shared<detail::Node<T>>(0);
    Promise<T> made = detail::Access::made<T>(detail::Core::claimed(to));

    if (among.empty()) {
        (void)to->failed(std::make_exception_ptr(Rejected{"Nothing to wait for."}), detail::Delivery::here);

        return made;
    }

    std::vector<std::shared_ptr<detail::Core>> summed;

    for (Promise<T>& one : among) summed.push_back(detail::Access::claim(one).core());

    auto left = std::make_shared<detail::Counting>(among.size());

    for (Promise<T>& one : among)
        detail::joined<T>(to, one, [to, left](detail::Node<T>& from) {
            if (!to->running()) return;

            if (from.ending() == detail::Ending::value) {
                (void)detail::passed(from, *to, detail::Delivery::here);

                return;
            }

            if (!left->last()) return;

            if (from.ending() == detail::Ending::error) (void)to->failed(from.why(), detail::Delivery::here);
            else (void)to->failed(std::make_exception_ptr(Cancelled{}), detail::Delivery::here);
        });

    to->sum(std::move(summed));

    return made;
}

template <typename T>
[[nodiscard]] Promise<T> race(std::vector<Promise<T>> among) {
    auto to = std::make_shared<detail::Node<T>>(0);
    Promise<T> made = detail::Access::made<T>(detail::Core::claimed(to));

    if (among.empty()) {
        (void)to->failed(std::make_exception_ptr(Rejected{"Nothing to wait for."}), detail::Delivery::here);

        return made;
    }

    std::vector<std::shared_ptr<detail::Core>> summed;

    for (Promise<T>& one : among) summed.push_back(detail::Access::claim(one).core());

    for (Promise<T>& one : among)
        detail::joined<T>(to, one, [to](detail::Node<T>& from) { (void)detail::passed(from, *to, detail::Delivery::here); });

    to->sum(std::move(summed));

    return made;
}

template <typename T>
[[nodiscard]] Promise<std::vector<Outcome<T>>> settled(std::vector<Promise<T>> among) {
    auto to = std::make_shared<detail::Node<std::vector<Outcome<T>>>>(0);
    Promise<std::vector<Outcome<T>>> made = detail::Access::made<std::vector<Outcome<T>>>(detail::Core::claimed(to));

    if (among.empty()) {
        (void)to->valued(std::vector<Outcome<T>>{}, detail::Delivery::here);

        return made;
    }

    std::vector<std::shared_ptr<detail::Core>> summed;

    for (Promise<T>& one : among) summed.push_back(detail::Access::claim(one).core());

    auto got = std::make_shared<detail::Settling<T>>(among.size());

    for (std::size_t at = 0; at < among.size(); ++at)
        detail::joined<T>(to, among[at], [to, got, at](detail::Node<T>& from) {
            if (!to->running()) return;

            if (got->put(at, detail::Access::outcome(from))) (void)to->valued(got->whole(), detail::Delivery::here);
        });

    to->sum(std::move(summed));

    return made;
}

namespace detail {

class Suspension {
public:
    struct Waited {
        std::mutex guard;
        Claim claim;
        std::coroutine_handle<> handle;
        bool resumed = false;
        bool cancelled = false;
    };

    explicit Suspension(const Stop* stop) : stop_(stop) {}

    void entered() {
        previous_ = current_stop();
        current_stop() = stop_;
    }

    void left() { current_stop() = previous_; }

    [[nodiscard]] bool waiting(std::shared_ptr<Waited> now) {
        const std::lock_guard<std::mutex> held{guard_};

        waited_ = std::move(now);

        return stopped_;
    }

    void stopped();

private:
    std::mutex guard_;
    std::shared_ptr<Waited> waited_;
    const Stop* stop_ = nullptr;
    const Stop* previous_ = nullptr;
    bool stopped_ = false;
};

class Resuming final : public Reaction {
public:
    explicit Resuming(std::shared_ptr<Suspension::Waited> waited) : waited_(std::move(waited)) {}

    void ended() override;

private:
    std::shared_ptr<Suspension::Waited> waited_;
};

template <typename T>
class FrameBase {
public:
    FrameBase()
        : node_(std::make_shared<Node<T>>(1)),
          stop_(std::make_unique<Stop>(Access::stop(node_))),
          suspension_(std::make_shared<Suspension>(stop_.get())) {
        listening_ = node_->on_stop([suspension = suspension_] { suspension->stopped(); });
    }

    FrameBase(const FrameBase&) = delete;
    FrameBase& operator=(const FrameBase&) = delete;
    FrameBase(FrameBase&&) = delete;
    FrameBase& operator=(FrameBase&&) = delete;

    ~FrameBase() = default;

    Promise<T> get_return_object() { return Access::made<T>(Core::claimed(node_)); }

    [[nodiscard]] auto initial_suspend() noexcept {
        struct Entering {
            Suspension* suspension;

            [[nodiscard]] bool await_ready() const noexcept { return true; }

            void await_suspend(std::coroutine_handle<>) const noexcept {}

            void await_resume() const noexcept { suspension->entered(); }
        };

        return Entering{suspension_.get()};
    }

    [[nodiscard]] auto final_suspend() noexcept {
        struct Leaving {
            FrameBase* frame;

            [[nodiscard]] bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> handle) const noexcept {
                const std::shared_ptr<Node<T>> node = frame->node_;
                const std::shared_ptr<Suspension> suspension = frame->suspension_;

                node->off_stop(frame->listening_);
                suspension->left();

                handle.destroy();

                node->let_go();
            }

            void await_resume() const noexcept {}
        };

        return Leaving{this};
    }

    void unhandled_exception() { (void)node_->failed(caught(), Delivery::here); }

    [[nodiscard]] const std::shared_ptr<Suspension>& suspension() const { return suspension_; }

protected:
    std::shared_ptr<Node<T>> node_;

private:
    std::unique_ptr<Stop> stop_;
    std::shared_ptr<Suspension> suspension_;
    std::uint64_t listening_ = 0;
};

template <typename T>
class Frame : public FrameBase<T> {
public:
    template <typename V>
    void return_value(V&& given) {
        (void)this->node_->valued(T(std::forward<V>(given)), Delivery::here);
    }
};

template <>
class Frame<void> : public FrameBase<void> {
public:
    void return_void() { (void)this->node_->valued(Delivery::here); }
};

template <typename T>
class Awaiting {
public:
    explicit Awaiting(Claim claim) : waited_(std::make_shared<Suspension::Waited>()) {
        node_ = node_of<T>(claim);
        waited_->claim = std::move(claim);
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename P>
    bool await_suspend(std::coroutine_handle<P> handle) {
        waited_->handle = handle;

        if constexpr (requires { handle.promise().suspension(); }) suspension_ = handle.promise().suspension().get();

        if (suspension_ != nullptr) {
            suspension_->left();

            if (suspension_->waiting(waited_)) {
                (void)suspension_->waiting(nullptr);

                waited_->cancelled = true;

                return false;
            }
        }

        const std::lock_guard<std::mutex> held{waited_->guard};

        if (!waited_->resumed) node_->react(waited_->claim, std::make_shared<Resuming>(waited_));

        return true;
    }

    decltype(auto) await_resume() {
        if (suspension_ != nullptr) {
            (void)suspension_->waiting(nullptr);
            suspension_->entered();
        }

        if (waited_->cancelled) throw Cancelled{};

        const Ending how = node_->ending();

        if (how == Ending::error) std::rethrow_exception(node_->why());
        if (how == Ending::cancelled) throw Cancelled{};

        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return node_->taken();
        }
    }

private:
    std::shared_ptr<Node<T>> node_;
    std::shared_ptr<Suspension::Waited> waited_;
    Suspension* suspension_ = nullptr;
};

}

template <typename T>
auto Promise<T>::operator co_await() && {
    (void)taken();

    return detail::Awaiting<T>{std::move(claim_)};
}

}

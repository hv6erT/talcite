#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "tal/executor.hpp"
#include "tal/log.hpp"
#include "tal/trouble.hpp"

namespace tal {

template <typename T = void>
class Task;

namespace detail {

class Waking {
public:
    Waking() = default;
    virtual ~Waking() = default;

    Waking(const Waking&) = delete;
    Waking& operator=(const Waking&) = delete;

    virtual void woken() = 0;
};

class Chain : public std::enable_shared_from_this<Chain> {
public:
    explicit Chain(Executor& where) : where_(&where) {}

    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    [[nodiscard]] Executor& where() const { return *where_.load(); }

    [[nodiscard]] Loop* moved(Executor& to);

    static void returned(Loop& home);

    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }

    [[nodiscard]] const std::atomic<bool>* cancelling() const { return &cancelled_; }

    void rooted(std::coroutine_handle<> root, Work gone);

    void start();

    void cancel();

    void suspended();

    void resume(std::coroutine_handle<> leaf);

    void finished();

    [[nodiscard]] bool over() const;

    void destroyed();

    void heed(Waking* one);

private:
    enum State : int { running, waiting, doomed, ended };

    void doom();

    std::mutex heeding_;
    Waking* heeded_ = nullptr;
    std::atomic<Executor*> where_;
    Loop* home_ = nullptr;
    std::atomic<bool> cancelled_{false};
    std::atomic<int> state_{waiting};
    std::coroutine_handle<> root_;
    Work gone_;
};

struct Leaf {
    std::mutex guard;
    std::shared_ptr<Chain> chain;
    std::coroutine_handle<> handle;
    bool armed = false;
    bool dead = false;

    void fire();

    void bury();
};

template <typename P>
[[nodiscard]] const std::shared_ptr<Chain>& chain_of(std::coroutine_handle<P> handle) {
    return handle.promise().chain;
}

template <typename T>
struct Outcome {
    std::optional<T> value;
    std::exception_ptr failed;

    template <typename U>
    void put(U&& given) {
        value.emplace(std::forward<U>(given));
    }

    T take() {
        if (failed) std::rethrow_exception(failed);

        if (!value) throw Cancelled{};

        return std::move(*value);
    }

    [[nodiscard]] bool held() const { return value.has_value() || failed != nullptr; }
};

template <>
struct Outcome<void> {
    bool value = false;
    std::exception_ptr failed;

    void put() { value = true; }

    void take() {
        if (failed) std::rethrow_exception(failed);

        if (!value) throw Cancelled{};
    }

    [[nodiscard]] bool held() const { return value || failed != nullptr; }
};

struct Started {
    std::coroutine_handle<> task;
    bool finished = false;
};

inline thread_local Started* started = nullptr;

struct Base {
    std::shared_ptr<Chain> chain;
    std::coroutine_handle<> next;
    std::exception_ptr failed;

    struct Final {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template <typename P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> done) noexcept {
            if (started != nullptr && started->task == done) {
                started->finished = true;

                return std::noop_coroutine();
            }

            const std::coroutine_handle<> resumed = done.promise().next;

            return resumed ? resumed : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    std::suspend_always initial_suspend() noexcept { return {}; }

    Final final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept { failed = std::current_exception(); }
};

template <typename T>
struct Returning : Base {
    std::optional<T> value;

    template <typename U>
    void return_value(U&& given) {
        value.emplace(std::forward<U>(given));
    }

    T taken() {
        if (failed) std::rethrow_exception(failed);

        return std::move(*value);
    }
};

template <>
struct Returning<void> : Base {
    void return_void() noexcept {}

    void taken() {
        if (failed) std::rethrow_exception(failed);
    }
};

struct Rooted {
    struct promise_type {
        std::shared_ptr<Chain> chain;

        template <typename... Rest>
        explicit promise_type(const std::shared_ptr<Chain>& given, Rest&&...) : chain(given) {}

        Rooted get_return_object() { return Rooted{std::coroutine_handle<promise_type>::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept {}
    };

    std::coroutine_handle<promise_type> handle;
};

template <typename T, typename Done>
Rooted rooted(std::shared_ptr<Chain> chain, Task<T> work, Done done) {
    Outcome<T> outcome;

    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(work);
            outcome.put();
        } else {
            outcome.put(co_await std::move(work));
        }
    } catch (...) {
        outcome.failed = std::current_exception();
    }

    done(std::move(outcome));
    chain->finished();
}

template <typename T, typename Done>
[[nodiscard]] std::shared_ptr<Chain> launched(Executor& where, Task<T> work, Done done, Work gone = {}) {
    auto chain = std::make_shared<Chain>(where);
    const Rooted root = rooted(chain, std::move(work), std::move(done));

    chain->rooted(root.handle, std::move(gone));

    return chain;
}

class Branches {
public:
    Branches() = default;

    Branches(const Branches&) = delete;
    Branches& operator=(const Branches&) = delete;

    ~Branches() { cancel(); }

    void add(std::shared_ptr<Chain> one) {
        const std::lock_guard<std::mutex> kept{guard_};

        held_.push_back(std::move(one));
    }

    void cancel() {
        std::vector<std::shared_ptr<Chain>> all;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            all.swap(held_);
        }

        for (const auto& one : all) one->cancel();
    }

private:
    std::mutex guard_;
    std::vector<std::shared_ptr<Chain>> held_;
};

struct Here {
    Executor* found = nullptr;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename P>
    bool await_suspend(std::coroutine_handle<P> handle) noexcept {
        found = &chain_of(handle)->where();

        return false;
    }

    [[nodiscard]] Executor& await_resume() const noexcept { return *found; }
};

struct Current {
    std::shared_ptr<Chain> found;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename P>
    bool await_suspend(std::coroutine_handle<P> handle) {
        found = chain_of(handle);

        return false;
    }

    [[nodiscard]] std::shared_ptr<Chain> await_resume() { return std::move(found); }
};

struct Moving {
    Executor* to;
    std::shared_ptr<Leaf> leaf = std::make_shared<Leaf>();

    Moving(Executor* given) : to(given) {}

    Moving(const Moving&) = delete;
    Moving& operator=(const Moving&) = delete;

    ~Moving() { leaf->bury(); }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename P>
    void await_suspend(std::coroutine_handle<P> handle) {
        const std::shared_ptr<Chain>& chain = chain_of(handle);
        const std::shared_ptr<Leaf> mine = leaf;
        const std::lock_guard<std::mutex> kept{mine->guard};

        mine->chain = chain;
        mine->handle = handle;

        Loop* const back = chain->moved(*to);

        chain->suspended();
        mine->armed = true;
        to->post([held = mine] { held->fire(); });

        if (back != nullptr) Chain::returned(*back);
    }

    void await_resume() const noexcept {}
};

struct Checking {
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename P>
    bool await_suspend(std::coroutine_handle<P> handle) {
        const std::shared_ptr<Chain>& chain = chain_of(handle);

        if (!chain->cancelled()) return false;

        chain->suspended();

        return true;
    }

    void await_resume() const noexcept {}
};

struct Sleeping {
    std::chrono::milliseconds wait;
    std::shared_ptr<Leaf> leaf = std::make_shared<Leaf>();
    std::shared_ptr<Alarm> alarm;

    explicit Sleeping(std::chrono::milliseconds given) : wait(given) {}

    Sleeping(const Sleeping&) = delete;
    Sleeping& operator=(const Sleeping&) = delete;

    ~Sleeping() {
        leaf->bury();

        if (alarm != nullptr) unalarmed(*alarm);
    }

    [[nodiscard]] bool await_ready() const noexcept { return wait.count() <= 0; }

    template <typename P>
    void await_suspend(std::coroutine_handle<P> handle) {
        const std::shared_ptr<Chain>& chain = chain_of(handle);
        const std::shared_ptr<Leaf> mine = leaf;
        const std::lock_guard<std::mutex> kept{mine->guard};

        mine->chain = chain;
        mine->handle = handle;
        alarm = alarmed(chain->where(), wait, [held = mine] { held->fire(); });
        chain->suspended();
        mine->armed = true;
    }

    void await_resume() const noexcept {}
};

}

template <typename T>
class [[nodiscard]] Task {
public:
    struct promise_type : detail::Returning<T> {
        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
    };

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();

            handle_ = std::exchange(other.handle_, {});
        }

        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();
    }

    [[nodiscard]] bool ready() const { return handle_ && handle_.done(); }

    struct Awaiter {
        std::coroutine_handle<promise_type> handle;

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template <typename P>
        bool await_suspend(std::coroutine_handle<P> parent) noexcept {
            const std::coroutine_handle<promise_type> child = handle;

            child.promise().chain = detail::chain_of(parent);
            child.promise().next = parent;

            detail::Started mine{child};
            detail::Started* const outer = std::exchange(detail::started, &mine);

            child.resume();
            detail::started = outer;

            return !mine.finished;
        }

        T await_resume() { return handle.promise().taken(); }
    };

    Awaiter operator co_await() && noexcept { return Awaiter{handle_}; }

private:
    explicit Task(std::coroutine_handle<promise_type> given) : handle_(given) {}

    std::coroutine_handle<promise_type> handle_;
};

template <typename T>
class Pending {
public:
    Pending() : state_(std::make_shared<State>()) {}

    template <typename U>
        requires(!std::is_void_v<T>)
    void settle(U&& given) {
        finish([&given](detail::Outcome<T>& into) { into.put(std::forward<U>(given)); });
    }

    void settle()
        requires(std::is_void_v<T>)
    {
        finish([](detail::Outcome<T>& into) { into.put(); });
    }

    void fail(std::exception_ptr why) {
        finish([&why](detail::Outcome<T>& into) { into.failed = std::move(why); });
    }

    template <typename E>
        requires std::derived_from<std::decay_t<E>, std::exception>
    void fail(E&& why) {
        fail(std::make_exception_ptr(std::forward<E>(why)));
    }

    [[nodiscard]] bool settled() const {
        const std::lock_guard<std::mutex> kept{state_->guard};

        return state_->outcome.held();
    }

    Task<T> task() const { return waited(state_); }

private:
    struct State {
        mutable std::mutex guard;
        detail::Outcome<T> outcome;
        std::shared_ptr<detail::Leaf> waiter;
    };

    struct Awaiter {
        std::shared_ptr<State> state;
        std::shared_ptr<detail::Leaf> leaf = std::make_shared<detail::Leaf>();

        explicit Awaiter(std::shared_ptr<State> given) : state(std::move(given)) {}

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;

        ~Awaiter() { leaf->bury(); }

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template <typename P>
        bool await_suspend(std::coroutine_handle<P> handle) {
            const std::shared_ptr<State> held = state;
            const std::shared_ptr<detail::Leaf> mine = leaf;
            const std::lock_guard<std::mutex> kept{held->guard};

            if (held->outcome.held()) return false;

            const std::lock_guard<std::mutex> armed{mine->guard};

            mine->chain = detail::chain_of(handle);
            mine->handle = handle;
            held->waiter = mine;
            mine->chain->suspended();
            mine->armed = true;

            return true;
        }

        T await_resume() {
            const std::lock_guard<std::mutex> kept{state->guard};

            return state->outcome.take();
        }
    };

    static Task<T> waited(std::shared_ptr<State> state) { co_return co_await Awaiter{std::move(state)}; }

    template <typename Put>
    void finish(Put put) {
        std::shared_ptr<detail::Leaf> woken;

        {
            const std::lock_guard<std::mutex> kept{state_->guard};

            if (state_->outcome.held()) return;

            put(state_->outcome);
            woken = std::move(state_->waiter);
        }

        if (woken) woken->fire();
    }

    std::shared_ptr<State> state_;
};

class Scope {
public:
    explicit Scope(Executor& where = loop());

    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    template <typename T>
    void run(Task<T> work) {
        const std::shared_ptr<State> state = state_;
        const std::uint64_t id = state->reserved();

        auto chain = detail::launched(*where_, std::move(work), [](detail::Outcome<T>&& outcome) { failed(outcome.failed); },
                                      [state, id] { state->forget(id); });

        state->keep(id, chain);
        chain->start();
    }

    void cancel();

    [[nodiscard]] bool busy() const;

private:
    struct State {
        mutable std::mutex guard;
        std::map<std::uint64_t, std::weak_ptr<detail::Chain>> chains;
        std::uint64_t numbered = 0;

        std::uint64_t reserved();

        void keep(std::uint64_t id, const std::shared_ptr<detail::Chain>& chain);

        void forget(std::uint64_t id);
    };

    static void failed(const std::exception_ptr& why);

    std::shared_ptr<State> state_;
    Executor* where_;
};

template <typename T>
class Shared {
public:
    explicit Shared(Task<T> work, Executor& where = loop()) : state_(std::make_shared<State>()) {
        const std::weak_ptr<State> weak = state_;

        state_->chain = detail::launched(where, std::move(work), [weak](detail::Outcome<T>&& outcome) {
            const std::shared_ptr<State> state = weak.lock();

            if (state) state->ended(std::move(outcome));
        });
    }

    Shared(Shared&&) noexcept = default;
    Shared& operator=(Shared&&) noexcept = default;
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

    Task<T> get() const { return got(state_); }

private:
    struct State {
        std::mutex guard;
        std::shared_ptr<detail::Chain> chain;
        bool started = false;
        detail::Outcome<T> outcome;
        std::vector<Pending<T>> waiting;

        ~State() {
            if (chain) chain->cancel();
        }

        void ended(detail::Outcome<T>&& given) {
            std::vector<Pending<T>> woken;

            {
                const std::lock_guard<std::mutex> kept{guard};

                outcome = std::move(given);
                woken.swap(waiting);
            }

            for (Pending<T>& one : woken) {
                if (outcome.failed) {
                    one.fail(outcome.failed);
                } else if constexpr (std::is_void_v<T>) {
                    one.settle();
                } else {
                    one.settle(*outcome.value);
                }
            }
        }
    };

    static Task<T> got(std::shared_ptr<State> state) {
        Pending<T> pending;
        bool start = false;

        {
            const std::lock_guard<std::mutex> kept{state->guard};

            if (state->outcome.held()) {
                if (state->outcome.failed) std::rethrow_exception(state->outcome.failed);

                if constexpr (std::is_void_v<T>) {
                    co_return;
                } else {
                    co_return *state->outcome.value;
                }
            }

            state->waiting.push_back(pending);
            start = !std::exchange(state->started, true);
        }

        if (start) state->chain->start();

        co_return co_await pending.task();
    }

    std::shared_ptr<State> state_;
};

[[nodiscard]] inline detail::Moving to(Executor& where) { return detail::Moving{&where}; }

[[nodiscard]] inline detail::Checking checkpoint() { return {}; }

[[nodiscard]] inline detail::Sleeping after(std::chrono::milliseconds wait) { return detail::Sleeping{wait}; }

template <typename... T>
Task<std::tuple<T...>> all(Task<T>... work) {
    static_assert((!std::is_void_v<T> && ...), "tal::all: tasks of void have nothing to gather");

    struct Gathered {
        std::mutex guard;
        std::tuple<std::optional<T>...> values;
        std::size_t left = sizeof...(T);
        Pending<std::tuple<T...>> done;
    };

    const auto gathered = std::make_shared<Gathered>();
    detail::Branches branches;
    Executor& here = co_await detail::Here{};
    std::tuple<Task<T>...> tasks{std::move(work)...};
    std::vector<std::shared_ptr<detail::Chain>> chains;

    [&]<std::size_t... At>(std::index_sequence<At...>) {
        (chains.push_back(detail::launched(here, std::move(std::get<At>(tasks)), [gathered](auto&& outcome) {
             bool last = false;

             {
                 const std::lock_guard<std::mutex> kept{gathered->guard};

                 if (outcome.failed) {
                     gathered->done.fail(outcome.failed);

                     return;
                 }

                 std::get<At>(gathered->values) = std::move(*outcome.value);
                 last = --gathered->left == 0;
             }

             if (last)
                 gathered->done.settle(std::apply([](auto&... got) { return std::tuple<T...>{std::move(*got)...}; }, gathered->values));
         })),
         ...);
    }(std::index_sequence_for<T...>{});

    for (const auto& one : chains) {
        branches.add(one);
        one->start();
    }

    co_return co_await gathered->done.task();
}

template <typename T>
Task<T> first(std::vector<Task<T>> work) {
    if (work.empty()) throw Trouble{Kind::missing, "tal::first: no tasks to wait for"};

    struct Racing {
        std::mutex guard;
        std::size_t left;
        std::exception_ptr last;
        Pending<T> done;
    };

    const auto racing = std::make_shared<Racing>();

    racing->left = work.size();

    detail::Branches branches;
    Executor& here = co_await detail::Here{};
    std::vector<std::shared_ptr<detail::Chain>> chains;

    for (Task<T>& one : work)
        chains.push_back(detail::launched(here, std::move(one), [racing](detail::Outcome<T>&& outcome) {
            const std::lock_guard<std::mutex> kept{racing->guard};

            --racing->left;

            if (!outcome.failed) {
                if constexpr (std::is_void_v<T>) {
                    racing->done.settle();
                } else {
                    racing->done.settle(std::move(*outcome.value));
                }

                return;
            }

            racing->last = outcome.failed;

            if (racing->left == 0) racing->done.fail(racing->last);
        }));

    for (const auto& one : chains) {
        branches.add(one);
        one->start();
    }

    co_return co_await racing->done.task();
}

template <typename T>
Task<T> within(std::chrono::milliseconds most, Task<T> work) {
    const auto done = std::make_shared<Pending<T>>();

    detail::Branches branches;
    Executor& here = co_await detail::Here{};

    auto chain = detail::launched(here, std::move(work), [done](detail::Outcome<T>&& outcome) {
        if (outcome.failed) {
            done->fail(outcome.failed);
        } else if constexpr (std::is_void_v<T>) {
            done->settle();
        } else {
            done->settle(std::move(*outcome.value));
        }
    });

    auto timer = detail::launched(here, [](std::chrono::milliseconds wait) -> Task<void> { co_await after(wait); }(most),
                                  [done](detail::Outcome<void>&&) { done->fail(Overdue{}); });

    branches.add(chain);
    branches.add(timer);
    chain->start();
    timer->start();

    co_return co_await done->task();
}

namespace detail {

template <typename T>
Task<T> homed(Executor& home, Task<T> work) {
    Outcome<T> outcome;

    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(work);
            outcome.put();
        } else {
            outcome.put(co_await std::move(work));
        }
    } catch (...) {
        outcome.failed = std::current_exception();
    }

    co_await to(home);

    co_return outcome.take();
}

}

template <typename T>
T wait(Loop& on, Task<T> work) {
    const auto outcome = std::make_shared<detail::Outcome<T>>();
    const auto chain =
        detail::launched(on, detail::homed(on, std::move(work)), [outcome](detail::Outcome<T>&& got) { *outcome = std::move(got); });

    chain->start();

    while (!chain->over()) {
        if (!on.turn()) on.pause();
    }

    return outcome->take();
}

}

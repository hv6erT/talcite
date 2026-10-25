#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "talcite/bell.hpp"
#include "talcite/progress.hpp"

namespace talcite::detail {

enum class Ending : std::uint8_t { running, value, error, cancelled };

enum class Delivery : std::uint8_t { here, away };

#ifdef _WIN32
using Heard = void*;
#else
using Heard = int;
#endif

class Step {
public:
    Step() = default;

    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, Step>)
    Step(F f) : body_(std::make_unique<Of<std::decay_t<F>>>(std::move(f))) {}

    Step(Step&&) noexcept = default;
    Step& operator=(Step&&) noexcept = default;
    Step(const Step&) = delete;
    Step& operator=(const Step&) = delete;
    ~Step() = default;

    void operator()() { body_->run(); }

    [[nodiscard]] explicit operator bool() const { return body_ != nullptr; }

private:
    struct Base {
        Base() = default;
        virtual ~Base() = default;
        Base(const Base&) = delete;
        Base& operator=(const Base&) = delete;
        Base(Base&&) = delete;
        Base& operator=(Base&&) = delete;

        virtual void run() = 0;
    };

    template <typename F>
    struct Of final : Base {
        explicit Of(F given) : f(std::move(given)) {}

        void run() override { f(); }

        F f;
    };

    std::unique_ptr<Base> body_;
};

struct Gate;

void stepped(Step step);

[[nodiscard]] bool stepping();

void drained();

void away(std::function<void()> what);

void lose(std::exception_ptr why);

[[nodiscard]] std::exception_ptr caught();

[[nodiscard]] bool quitting(const std::exception_ptr& why);

[[nodiscard]] std::exception_ptr quitted();

class Core;

class Reaction {
public:
    Reaction() = default;
    virtual ~Reaction() = default;
    Reaction(const Reaction&) = delete;
    Reaction& operator=(const Reaction&) = delete;
    Reaction(Reaction&&) = delete;
    Reaction& operator=(Reaction&&) = delete;

    virtual void ended() = 0;

    virtual void abandoned() = 0;

    [[nodiscard]] virtual bool routed() const { return false; }
};

class Claim {
public:
    Claim() = default;

    Claim(Claim&& other) noexcept
        : core_(std::move(other.core_)), reaction_(std::exchange(other.reaction_, 0)) {}

    Claim& operator=(Claim&& other) noexcept {
        if (this != &other) {
            release();

            core_ = std::move(other.core_);
            reaction_ = std::exchange(other.reaction_, 0);
        }

        return *this;
    }

    Claim(const Claim&) = delete;
    Claim& operator=(const Claim&) = delete;

    ~Claim() { release(); }

    void release();

    [[nodiscard]] Core* get() const { return core_.get(); }

    [[nodiscard]] const std::shared_ptr<Core>& core() const { return core_; }

    [[nodiscard]] explicit operator bool() const { return core_ != nullptr; }

private:
    friend class Core;

    Claim(std::shared_ptr<Core> core, std::uint64_t reaction) : core_(std::move(core)), reaction_(reaction) {}

    std::shared_ptr<Core> core_;
    std::uint64_t reaction_ = 0;
};

class Core : public std::enable_shared_from_this<Core> {
public:
    explicit Core(std::size_t work);

    virtual ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

    [[nodiscard]] static Claim claimed(const std::shared_ptr<Core>& core);

    void react(Claim& claim, std::shared_ptr<Reaction> reaction);

    void keep(Claim claim);

    void follow(std::shared_ptr<Core> whose);

    void dwell(std::shared_ptr<Gate> where);

    void inherit(std::shared_ptr<Gate> where);

    [[nodiscard]] std::shared_ptr<Gate> home() const;

    void sum(std::vector<std::shared_ptr<Core>> among);

    void dwell_among(const std::vector<std::shared_ptr<Core>>& among);

    void detach();

    void share();

    [[nodiscard]] bool shared() const;

    [[nodiscard]] Ending ending() const;

    [[nodiscard]] bool running() const;

    [[nodiscard]] std::exception_ptr why() const;

    bool failed(std::exception_ptr why, Delivery delivery);

    bool quit(Delivery delivery);

    void hold();

    void let_go();

    void let_go_away();

    void when_finished(std::function<void()> then);

    void when_ended(std::function<void()> then);

    [[nodiscard]] bool begin(Bell* bell);

    [[nodiscard]] bool started() const;

    void unlend();

    [[nodiscard]] bool stopped() const { return stopped_.load(); }

    [[nodiscard]] bool wait_stop(std::chrono::milliseconds patience) const;

    [[nodiscard]] Heard heard();

    [[nodiscard]] std::uint64_t on_stop(std::function<void()> then);

    void off_stop(std::uint64_t which);

    [[nodiscard]] std::uint64_t hooked(std::function<void()> then);

    void unhooked(std::uint64_t which);

    [[nodiscard]] Ending waited(Core* also);

    void report(Progress now);

    [[nodiscard]] Progress progress() const;

protected:
    struct Concluded {
        std::vector<std::shared_ptr<Reaction>> reactions;
        std::vector<std::function<void()>> ended;
        std::vector<std::function<void()>> finished;
        std::vector<Claim> upstream;
        std::exception_ptr lost;
    };

    [[nodiscard]] Concluded concluded_locked(Ending how, std::exception_ptr why);

    static void delivered(Concluded made, Delivery delivery);

    [[nodiscard]] Ending ending_locked() const { return ending_; }

    mutable std::mutex guard_;
    bool shared_ = false;

private:
    friend class Claim;

    struct Hooking {
        std::function<void()> then;
        int state = 0;
        std::thread::id runner;
    };

    void released(std::uint64_t reaction);

    void hook_run(std::uint64_t which);

    [[nodiscard]] std::vector<std::function<void()>> finishing_locked();

    mutable std::condition_variable stirred_;
    Ending ending_ = Ending::running;
    std::exception_ptr why_;
    std::size_t owners_ = 0;
    bool detached_ = false;
    std::size_t work_ = 0;
    bool finished_ = false;
    bool started_ = false;
    std::map<std::uint64_t, std::shared_ptr<Reaction>> reactions_;
    std::uint64_t next_ = 0;
    std::vector<std::function<void()>> on_ended_;
    std::vector<std::function<void()>> on_finished_;
    std::vector<Claim> upstream_;
    std::atomic<bool> stopped_{false};
    Bell* lent_ = nullptr;
    std::unique_ptr<Bell> own_;
    std::map<std::uint64_t, std::function<void()>> on_stop_;
    std::map<std::uint64_t, Hooking> hooks_;
    Progress progress_;
    std::shared_ptr<Core> following_;
    std::vector<std::shared_ptr<Core>> summing_;
    std::shared_ptr<Gate> home_;
};

template <typename T>
class Node : public Core {
public:
    explicit Node(std::size_t work) : Core(work) {}

    bool valued(T given, Delivery delivery) {
        Concluded made;

        {
            const std::lock_guard<std::mutex> held{guard_};

            if (!running_locked()) return false;

            value_.emplace(std::move(given));

            made = concluded_locked(Ending::value, nullptr);
        }

        delivered(std::move(made), delivery);

        return true;
    }

    [[nodiscard]] T taken() {
        const std::lock_guard<std::mutex> held{guard_};

        if constexpr (std::is_copy_constructible_v<T>) {
            if (shared_) return *value_;
        }

        T made = std::move(*value_);

        value_.reset();

        return made;
    }

    [[nodiscard]] const T& peeked() const {
        const std::lock_guard<std::mutex> held{guard_};

        return *value_;
    }

private:
    [[nodiscard]] bool running_locked() const { return ending_locked() == Ending::running; }

    std::optional<T> value_;
};

template <>
class Node<void> : public Core {
public:
    explicit Node(std::size_t work) : Core(work) {}

    bool valued(Delivery delivery);

    void taken() {}

    void peeked() const {}
};

Bell* worker_bell();

}

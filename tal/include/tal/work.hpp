#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace tal {

class Work {
public:
    Work() = default;

    template <typename F>
        requires(!std::same_as<std::decay_t<F>, Work> && std::invocable<std::decay_t<F>&>)
    Work(F&& what) : held_(std::make_unique<Held<std::decay_t<F>>>(std::forward<F>(what))) {}

    Work(Work&&) noexcept = default;
    Work& operator=(Work&&) noexcept = default;
    Work(const Work&) = delete;
    Work& operator=(const Work&) = delete;

    explicit operator bool() const { return held_ != nullptr; }

    void operator()() { held_->run(); }

private:
    struct Base {
        virtual ~Base() = default;
        virtual void run() = 0;
    };

    template <typename F>
    struct Held final : Base {
        explicit Held(F given) : what(std::move(given)) {}

        void run() override { what(); }

        F what;
    };

    std::unique_ptr<Base> held_;
};

}

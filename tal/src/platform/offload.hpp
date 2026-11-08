#pragma once

#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "tal/executor.hpp"
#include "tal/task.hpp"

namespace tal::platform {

class Bell : public detail::Waking {
public:
    Bell();
    ~Bell() override;

    void woken() override;

    void clear();

    [[nodiscard]] bool rang() const;

#ifdef _WIN32
    [[nodiscard]] void* waited() const { return event_; }
#else
    [[nodiscard]] int waited() const { return heard_; }
#endif

private:
    std::atomic<bool> rang_{false};
#ifdef _WIN32
    void* event_ = nullptr;
#else
    int heard_ = -1;
    int rung_ = -1;
#endif
};

namespace core {

[[nodiscard]] Bell* bell();

void belled(Bell* one, const std::atomic<bool>* flag);

}

template <typename F>
auto offloaded(F work) -> Task<std::invoke_result_t<F&>> {
    using Result = std::invoke_result_t<F&>;

    Executor& back = co_await detail::Here{};
    const std::shared_ptr<detail::Chain> chain = co_await detail::Current{};

    co_await to(io());

    Bell* const bell = core::bell();
    std::exception_ptr failed;
    std::optional<std::conditional_t<std::is_void_v<Result>, bool, Result>> made;

    core::belled(bell, chain->cancelling());
    chain->heed(bell);

    try {
        if constexpr (std::is_void_v<Result>) {
            work();
            made.emplace(true);
        } else {
            made.emplace(work());
        }
    } catch (...) {
        failed = std::current_exception();
    }

    chain->heed(nullptr);
    core::belled(nullptr, nullptr);

    co_await to(back);

    if (failed) std::rethrow_exception(failed);

    if constexpr (!std::is_void_v<Result>) co_return std::move(*made);
}

}

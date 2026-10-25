#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace talcite {

namespace detail {

struct Gate;

}

class Executor {
public:
    Executor();

    virtual ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    Executor(Executor&&) = delete;
    Executor& operator=(Executor&&) = delete;

    virtual bool post(std::function<void()> what) = 0;

    [[nodiscard]] const std::shared_ptr<detail::Gate>& gate() const { return gate_; }

protected:
    void close();

private:
    std::shared_ptr<detail::Gate> gate_;
};

class ManualExecutor : public Executor {
public:
    ManualExecutor() = default;

    ~ManualExecutor() override;

    ManualExecutor(const ManualExecutor&) = delete;
    ManualExecutor& operator=(const ManualExecutor&) = delete;
    ManualExecutor(ManualExecutor&&) = delete;
    ManualExecutor& operator=(ManualExecutor&&) = delete;

    bool post(std::function<void()> what) override;

    std::size_t turn();

    [[nodiscard]] std::size_t waiting() const;

private:
    mutable std::mutex guard_;
    std::vector<std::function<void()>> queued_;
};

[[nodiscard]] Executor& io();

[[nodiscard]] Executor& cpu();

namespace detail {

struct Gate {
    std::mutex guard;
    Executor* executor = nullptr;
};

class Withdrawing {
public:
    virtual ~Withdrawing() = default;

    [[nodiscard]] virtual std::uint64_t queued(std::function<void()> what) = 0;

    [[nodiscard]] virtual bool withdrawn(std::uint64_t which) = 0;
};

[[nodiscard]] bool posted(const std::shared_ptr<Gate>& where, std::function<void()> what, std::function<void()> abandoned);

}

}

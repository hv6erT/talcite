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

class Place {
public:
    Place();

    virtual ~Place();

    Place(const Place&) = delete;
    Place& operator=(const Place&) = delete;
    Place(Place&&) = delete;
    Place& operator=(Place&&) = delete;

    virtual bool post(std::function<void()> what) = 0;

    [[nodiscard]] const std::shared_ptr<detail::Gate>& gate() const { return gate_; }

protected:
    void close();

private:
    std::shared_ptr<detail::Gate> gate_;
};

class ManualPlace : public Place {
public:
    ManualPlace() = default;

    ~ManualPlace() override;

    ManualPlace(const ManualPlace&) = delete;
    ManualPlace& operator=(const ManualPlace&) = delete;
    ManualPlace(ManualPlace&&) = delete;
    ManualPlace& operator=(ManualPlace&&) = delete;

    bool post(std::function<void()> what) override;

    std::size_t turn();

    [[nodiscard]] std::size_t waiting() const;

private:
    mutable std::mutex guard_;
    std::vector<std::function<void()>> queued_;
};

[[nodiscard]] Place& io();

[[nodiscard]] Place& cpu();

namespace detail {

struct Gate {
    std::mutex guard;
    Place* place = nullptr;
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

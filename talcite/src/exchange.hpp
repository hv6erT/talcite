#pragma once

#include <cstddef>
#include <memory>

namespace talcite::net {

class Exchange {
public:
    struct Held;

    Exchange();

    ~Exchange();

    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;
    Exchange(Exchange&&) = delete;
    Exchange& operator=(Exchange&&) = delete;

    [[nodiscard]] std::size_t waiting() const;

    [[nodiscard]] static Exchange& running();

private:
    std::unique_ptr<Held> held_;
};

}

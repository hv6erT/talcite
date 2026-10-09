#pragma once

#include <memory>
#include <optional>

#include "talcite/fs.hpp"
#include "talcite/promise.hpp"

namespace talcite {

class Claim {
public:
    struct Held;

    Claim() = default;

    [[nodiscard]] static Promise<std::optional<Claim>> taken(fs::path where);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const fs::path& where() const;

    void release();

private:
    explicit Claim(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

}

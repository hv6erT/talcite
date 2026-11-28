#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"

namespace tal {

class Secrets : public Adapter {
public:
    static constexpr std::string_view kind = "secrets";

    [[nodiscard]] static std::shared_ptr<Secrets> builtin();

    [[nodiscard]] virtual std::string_view store() const = 0;

    [[nodiscard]] virtual Task<std::optional<std::string>> read(std::string name) = 0;

    [[nodiscard]] virtual Task<void> write(std::string name, std::string secret) = 0;

    [[nodiscard]] virtual Task<void> remove(std::string name) = 0;
};

class MemorySecrets : public Secrets {
public:
    [[nodiscard]] std::string_view name() const override { return "memory"; }

    [[nodiscard]] std::string_view store() const override { return "memory"; }

    [[nodiscard]] Task<std::optional<std::string>> read(std::string name) override;

    [[nodiscard]] Task<void> write(std::string name, std::string secret) override;

    [[nodiscard]] Task<void> remove(std::string name) override;

    std::map<std::string, std::string> held;
    bool refusing = false;
};

namespace secreting {

[[nodiscard]] Path& tool();

}

}

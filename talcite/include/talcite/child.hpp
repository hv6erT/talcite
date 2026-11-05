#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/fs.hpp"
#include "talcite/promise.hpp"

namespace talcite {

enum class Ended : std::uint8_t { normal, killed, lost };

struct Exit {
    Ended ended = Ended::normal;
    int code = 0;
    int signal = 0;
};

struct Variable {
    std::string name;
    std::string value;
};

struct Command {
    fs::path program;
    std::vector<std::string> arguments;
    fs::path where;
    std::vector<Variable> environment;
    bool inherit = true;
    bool merge = false;
};

struct Output {
    Exit exit;
    Bytes out;
    Bytes err;
};

class Child {
public:
    struct Held;

    Child() = default;

    [[nodiscard]] static Promise<Child> start(Command what);

    [[nodiscard]] static Promise<Output> run(Command what, Bytes input = {});

    [[nodiscard]] static Promise<std::int64_t> detached(Command what);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] std::int64_t pid() const;

    [[nodiscard]] Promise<std::size_t> write(Bytes what) const;

    void close_input() const;

    [[nodiscard]] Promise<Bytes> read(std::size_t most) const;

    [[nodiscard]] Promise<Exit> wait() const;

    void stop() const;

    void kill() const;

    void close();

private:
    friend class Rearing;

    explicit Child(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

}

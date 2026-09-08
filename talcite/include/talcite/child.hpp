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

class Term;

enum class Ended : std::uint8_t { normal, killed, lost };

struct Leave {
    Ended ended = Ended::normal;
    int code = 0;
    int signal = 0;
};

struct Around {
    std::string name;
    std::string value;
};

struct Recipe {
    fs::path program;
    std::vector<std::string> arguments;
    fs::path where;
    std::vector<Around> around;
    bool inherit = true;
    bool merge = false;
};

class Child {
public:
    struct Held;

    Child() = default;

    [[nodiscard]] static Promise<Child> start(Recipe what);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] std::int64_t pid() const;

    [[nodiscard]] Promise<std::size_t> write(Bytes what) const;

    void seal() const;

    [[nodiscard]] Promise<Bytes> read(std::size_t most) const;

    [[nodiscard]] Promise<Bytes> read_errors(std::size_t most) const;

    [[nodiscard]] Promise<Leave> leave() const;

    void stop() const;

    void kill() const;

    void close();

private:
    friend class Rearing;

    explicit Child(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

struct Done {
    Leave leave;
    Bytes read;
    Bytes errors;
};

[[nodiscard]] Promise<Done> taken(Recipe what, Bytes fed = {});

[[nodiscard]] Leave handed(Term& given, Recipe what);

}

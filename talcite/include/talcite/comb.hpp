#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talcite {

struct Sift {
    bool regex = false;
    bool cased = false;
    bool whole = false;
};

struct Hit {
    std::size_t from = 0;
    std::size_t upto = 0;
};

class Comb {
public:
    Comb();

    Comb(std::string what, Sift how);

    ~Comb();

    Comb(Comb&& other) noexcept;

    Comb& operator=(Comb&& other) noexcept;

    [[nodiscard]] bool ready() const;

    [[nodiscard]] const std::string& wrong() const;

    [[nodiscard]] const std::string& what() const;

    [[nodiscard]] Sift how() const;

    [[nodiscard]] std::optional<Hit> forth(std::string_view where, std::size_t from) const;

    [[nodiscard]] std::optional<Hit> back(std::string_view where, std::size_t upto) const;

    [[nodiscard]] std::vector<Hit> all(std::string_view where) const;

private:
    struct Kept;

    std::unique_ptr<Kept> kept_;
};

}

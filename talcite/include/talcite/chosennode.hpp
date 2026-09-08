#pragma once

#include <concepts>
#include <cstddef>
#include <vector>

#include "talcite/node.hpp"

namespace talcite::ui {

class Chosen {
public:
    Chosen(const Chosen&) = delete;
    Chosen& operator=(const Chosen&) = delete;
    Chosen(Chosen&&) = delete;
    Chosen& operator=(Chosen&&) = delete;

    void add(std::size_t which);

    void remove(std::size_t which);

    void clear();

    [[nodiscard]] bool contains(std::size_t which) const;

    [[nodiscard]] std::size_t many() const;

    [[nodiscard]] bool empty() const { return many() == 0; }

    [[nodiscard]] std::vector<std::size_t> all() const;

private:
    template <std::derived_from<Node> Above>
    friend class ChosenNode;

    Chosen() = default;

    ~Chosen() = default;

    void keep(std::size_t rows);

    std::vector<bool> inside_;
};

template <std::derived_from<Node> Above>
class ChosenNode : public Above {
public:
    [[nodiscard]] Chosen& chosen() { return chosen_; }

    [[nodiscard]] const Chosen& chosen() const { return chosen_; }

protected:
    using Above::Above;

    void settle(std::size_t rows) { chosen_.keep(rows); }

private:
    Chosen chosen_;
};

}

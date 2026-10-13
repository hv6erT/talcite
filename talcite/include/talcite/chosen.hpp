#pragma once

#include <cstddef>
#include <vector>

namespace talcite::ui {

class DiffViewer;
class Table;

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
    friend class DiffViewer;
    friend class Table;

    Chosen() = default;

    ~Chosen() = default;

    void keep(std::size_t rows);

    void moved(std::size_t at, std::size_t gone, std::size_t came);

    std::vector<bool> inside_;
    std::size_t many_ = 0;
};

}

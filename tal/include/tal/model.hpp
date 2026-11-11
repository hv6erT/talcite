#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tal/controls.hpp"
#include "tal/event.hpp"

namespace tal {

struct Entry {
    std::string text;
    std::string part;
    std::vector<Mark> marks;
    Elide elide = Elide::none;
    bool spans = false;

    friend bool operator==(const Entry&, const Entry&) = default;
};

struct Changes : Event {
    enum class Kind { inserted, removed, changed, reset };

    explicit Changes(Kind made, std::size_t at = 0, std::size_t many = 0) : kind(made), from(at), count(many) {}

    Kind kind;
    std::size_t from;
    std::size_t count;
};

class Model {
public:
    Model() = default;

    virtual ~Model();

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    [[nodiscard]] virtual std::size_t rows() const = 0;

    [[nodiscard]] virtual Entry entry(std::size_t row, std::size_t column) const = 0;

    template <typename E>
        requires std::same_as<E, Changes>
    [[nodiscard]] Hook on(Fn<E> what) {
        return hooked(std::make_shared<detail::Typed<E>>(std::move(what)));
    }

protected:
    void told(Changes what);

private:
    [[nodiscard]] Hook hooked(std::shared_ptr<detail::Slot> slot);

    std::vector<std::shared_ptr<detail::Slot>> slots_;
};

enum class Branch { leaf, closed, open };

class TreeModel : public Model {
public:
    [[nodiscard]] virtual int depth(std::size_t row) const = 0;

    [[nodiscard]] virtual Branch branch(std::size_t row) const = 0;

    virtual void open(std::size_t row, bool on) = 0;
};

}

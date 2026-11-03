#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "talcite/mark.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Marks {
public:
    Marks(const Marks&) = delete;
    Marks& operator=(const Marks&) = delete;
    Marks(Marks&&) = delete;
    Marks& operator=(Marks&&) = delete;

    void add(Mark given);

    void all(std::vector<Mark> given);

    void clear();

    [[nodiscard]] const std::vector<Mark>& all() const { return inside_; }

    [[nodiscard]] std::size_t many() const { return inside_.size(); }

    [[nodiscard]] bool empty() const { return inside_.empty(); }

    [[nodiscard]] const Mark& operator[](std::size_t which) const;

    using walk = std::vector<Mark>::const_iterator;

    [[nodiscard]] walk begin() const { return inside_.begin(); }

    [[nodiscard]] walk end() const { return inside_.end(); }

private:
    template <std::derived_from<Node> Base>
    friend class MarkedMixin;

    Marks() = default;

    ~Marks() = default;

    std::vector<Mark> inside_;
};

template <std::derived_from<Node> Base>
class MarkedMixin : public Base {
public:
    [[nodiscard]] Marks& marks() {
        this->Node::changed();

        return marks_;
    }

    [[nodiscard]] const Marks& marks() const { return marks_; }

    [[nodiscard]] std::optional<std::size_t> which() const {
        settle();

        return which_;
    }

    bool step(int much) {
        settle();

        if (marks_.empty() || much == 0) return false;

        if (which_.has_value()) {
            const auto at = static_cast<std::ptrdiff_t>(*which_) + much;

            if (at < 0 || at >= static_cast<std::ptrdiff_t>(marks_.many())) return false;

            return reach(static_cast<std::size_t>(at));
        }

        const std::size_t top = topmost();

        if (much > 0) {
            for (std::size_t one = 0; one < marks_.many(); ++one)
                if (marks_[one].line >= top) return reach(one);

            return false;
        }

        for (std::size_t one = marks_.many(); one > 0; --one)
            if (marks_[one - 1].line < top) return reach(one - 1);

        return false;
    }

protected:
    using Base::Base;

    void which(std::size_t given) {
        settle();

        which_ = given;

        this->Node::changed();
    }

    void settle() const {
        if (seen_ == marks_.many()) return;

        seen_ = marks_.many();
        which_.reset();
    }

    [[nodiscard]] virtual std::size_t topmost() const { return 0; }

    virtual bool reach(std::size_t given) {
        which(given);

        return true;
    }

private:
    Marks marks_;
    mutable std::size_t seen_ = 0;
    mutable std::optional<std::size_t> which_;
};

}

#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Keys {
public:
    Keys(const Keys&) = delete;
    Keys& operator=(const Keys&) = delete;
    Keys(Keys&&) = delete;
    Keys& operator=(Keys&&) = delete;

    void add(Key one);

    void add(std::vector<Key> many);

    void remove(Key one);

    void clear();

    [[nodiscard]] std::size_t many() const { return inside_.size(); }

    [[nodiscard]] bool empty() const { return inside_.empty(); }

    [[nodiscard]] bool contains(Key one) const;

    [[nodiscard]] Key operator[](std::size_t which) const;

    [[nodiscard]] Key front() const;

    [[nodiscard]] Key back() const;

    using walk = std::vector<Key>::const_iterator;

    [[nodiscard]] walk begin() const { return inside_.begin(); }

    [[nodiscard]] walk end() const { return inside_.end(); }

private:
    template <std::derived_from<Node> Base>
    friend class KeyedMixin;

    Keys() = default;

    ~Keys() = default;

    std::vector<Key> inside_;
};

[[nodiscard]] int shortcut_column(const Keys& these, const std::string& shown);

template <std::derived_from<Node> Base>
class KeyedMixin : public Base {
public:
    [[nodiscard]] Keys& keys() {
        this->Node::changed();

        return keys_;
    }

    [[nodiscard]] const Keys& keys() const { return keys_; }

protected:
    using Base::Base;
    using Base::handle;

    [[nodiscard]] int shortcut_column(const std::string& shown) const {
        return talcite::ui::shortcut_column(keys_, shown);
    }

    virtual void activate() = 0;

    bool handle(ShortcutEvent& what) override {
        if constexpr (requires(const Base& one) { one.enabled(); })
            if (!this->enabled()) return false;

        if (!keys_.contains(what.key())) return false;

        activate();

        return true;
    }

private:
    Keys keys_;
};

}

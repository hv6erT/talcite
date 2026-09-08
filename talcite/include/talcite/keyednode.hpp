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
    template <std::derived_from<Node> Above>
    friend class KeyedNode;

    Keys() = default;

    ~Keys() = default;

    std::vector<Key> inside_;
};

[[nodiscard]] int marking(const Keys& these, const std::string& shown);

template <std::derived_from<Node> Above>
class KeyedNode : public Above {
public:
    [[nodiscard]] Keys& keys() { return keys_; }

    [[nodiscard]] const Keys& keys() const { return keys_; }

    [[nodiscard]] virtual bool able() const { return true; }

protected:
    using Above::Above;

    [[nodiscard]] int marking(const std::string& shown) const {
        return talcite::ui::marking(keys_, shown);
    }

    virtual void act() = 0;

    bool react(HotEvent<KeyEvent>& what) override {
        if (!able() || !keys_.contains(what.held().key())) return false;

        act();

        return true;
    }

private:
    Keys keys_;
};

}

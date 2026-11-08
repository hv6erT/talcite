#pragma once

#include <concepts>
#include <memory>
#include <mutex>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include "tal/trouble.hpp"

namespace tal {

class Adapter {
public:
    virtual ~Adapter() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
};

class Defaults {
public:
    template <std::derived_from<Adapter> T>
    void set(std::shared_ptr<T> adapter) {
        const std::lock_guard<std::mutex> kept{guard_};

        held_[std::type_index{typeid(T)}] = std::move(adapter);
    }

    template <std::derived_from<Adapter> T>
    void clear() {
        const std::lock_guard<std::mutex> kept{guard_};

        held_.erase(std::type_index{typeid(T)});
    }

    template <std::derived_from<Adapter> T>
    [[nodiscard]] std::shared_ptr<T> get() {
        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (const auto at = held_.find(std::type_index{typeid(T)}); at != held_.end())
                return std::static_pointer_cast<T>(at->second);
        }

        if constexpr (requires { T::builtin(); }) {
            return T::builtin();
        } else {
            return nullptr;
        }
    }

private:
    std::mutex guard_;
    std::unordered_map<std::type_index, std::shared_ptr<Adapter>> held_;
};

[[nodiscard]] Defaults& defaults();

[[noreturn]] void unbound(std::string_view kind);

template <std::derived_from<Adapter> T>
class Port {
public:
    Port() : held_(defaults().get<T>()) {
        if (held_ == nullptr) unbound(T::kind);
    }

    explicit Port(std::shared_ptr<T> given) : held_(std::move(given)) {
        if (held_ == nullptr) unbound(T::kind);
    }

    [[nodiscard]] T& operator*() const { return *held_; }

    [[nodiscard]] T* operator->() const { return held_.get(); }

    [[nodiscard]] const std::shared_ptr<T>& shared() const { return held_; }

private:
    std::shared_ptr<T> held_;
};

template <std::derived_from<Adapter> T>
class Optional {
public:
    Optional() = default;

    explicit Optional(std::shared_ptr<T> given) : held_(std::move(given)) {}

    [[nodiscard]] explicit operator bool() const { return held_ != nullptr; }

    [[nodiscard]] T& operator*() const { return *held_; }

    [[nodiscard]] T* operator->() const { return held_.get(); }

private:
    std::shared_ptr<T> held_;
};

}

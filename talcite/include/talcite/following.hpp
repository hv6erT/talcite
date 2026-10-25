#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/launcher.hpp"
#include "talcite/mark.hpp"
#include "talcite/node.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"

namespace talcite::ui {

class FollowEvent : public Event {
public:
    explicit FollowEvent(std::string address) : address_(std::move(address)) {}

    [[nodiscard]] const std::string& address() const { return address_; }

private:
    std::string address_;
};

class FollowedEvent : public Event {
public:
    FollowedEvent(std::string address, Launched how) : address_(std::move(address)), how_(how) {}

    [[nodiscard]] const std::string& address() const { return address_; }

    [[nodiscard]] Launched how() const { return how_; }

private:
    std::string address_;
    Launched how_;
};

template <std::derived_from<Node> Base>
class FollowingMixin : public Base {
public:
    void launcher(Seam<Launcher> with) { launcher_ = std::move(with); }

protected:
    using Base::Base;

    void followed(std::string address) {
        if (address.empty()) return;

        if (this->trigger(FollowEvent{address})) return;

        const Seam<Launcher> with = launcher_.bound() ? launcher_ : talcite::launcher();
        const std::weak_ptr<Node> self = this->weak_from_this();

        following_ = with.provider().open(address).here().then([self, address](Launched how) {
            if (const std::shared_ptr<Node> kept = self.lock())
                (void)static_cast<FollowingMixin&>(*kept).trigger(FollowedEvent{address, how});
        });
    }

private:
    Seam<Launcher> launcher_;
    std::optional<Promise<void>> following_;
};

[[nodiscard]] std::vector<Mark> addressed(std::string_view text, const std::string& role);

}

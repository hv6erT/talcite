#pragma once

#include <optional>
#include <string>
#include <utility>

#include "talcite/enablednode.hpp"
#include "talcite/event.hpp"
#include "talcite/launcher.hpp"
#include "talcite/promise.hpp"
#include "talcite/view.hpp"

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
    FollowedEvent(std::string address, Opened how) : address_(std::move(address)), how_(how) {}

    [[nodiscard]] const std::string& address() const { return address_; }

    [[nodiscard]] Opened how() const { return how_; }

private:
    std::string address_;
    Opened how_;
};

class Link : public EnabledNode<Node> {
public:
    Link(std::string text, std::string address, Seam<Launcher> with = launcher());

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] const std::string& address() const { return target_.address; }

    void address(std::string given);

    void follow();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    [[nodiscard]] const std::string& shown() const;

    std::string text_;
    Hyperlink target_;
    Seam<Launcher> launcher_;
    std::optional<Promise<void>> following_;
};

}

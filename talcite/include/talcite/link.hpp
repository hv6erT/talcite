#pragma once

#include <string>
#include <utility>

#include "talcite/enabledmixin.hpp"
#include "talcite/event.hpp"
#include "talcite/following.hpp"
#include "talcite/launcher.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

class Link : public FollowingMixin<EnabledMixin<Node>> {
public:
    Link(std::string text, std::string address, Seam<Launcher> with = {});

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
};

}

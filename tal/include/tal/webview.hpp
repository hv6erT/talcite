#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "tal/docs.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/image.hpp"
#include "tal/node.hpp"
#include "tal/task.hpp"

namespace tal {

struct Visited : Event {
    Visited(std::string where, std::string named) : address(std::move(where)), title(std::move(named)) {}

    std::string address;
    std::string title;
};

struct Refused : Event {
    explicit Refused(docs::Refusal given) : refusal(std::move(given)) {}

    docs::Refusal refusal;
};

class Webview : public Node {
public:
    Webview(std::shared_ptr<docs::Browser> browser, std::string address, docs::Browsing how = {});

    ~Webview() override;

    Property<std::size_t> zoom{*this, Effect::none, 0};

    [[nodiscard]] static std::size_t zooms();

    [[nodiscard]] const std::string& address() const { return address_; }

    [[nodiscard]] const std::string& title() const { return title_; }

    [[nodiscard]] bool busy() const;

    void go(std::string address);

    void step(docs::Step step);

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    enum class Mode { none, pictures, text };

    [[nodiscard]] media::Size pixels() const;

    void scheduled();

    void refreshed();

    void shown(std::shared_ptr<Node> child);

    void act(Task<void> work);

    void shoot();

    void ticking();

    [[nodiscard]] Task<void> opened();

    [[nodiscard]] Task<void> read_text(std::string why);

    [[nodiscard]] Task<void> watched(std::shared_ptr<docs::Browsed> view);

    [[nodiscard]] Task<void> shot(std::shared_ptr<docs::Browsed> view);

    [[nodiscard]] Task<void> resized(std::shared_ptr<docs::Browsed> view, media::Size size);

    [[nodiscard]] Task<void> acted(std::shared_ptr<docs::Browsed> view, Task<void> work);

    void failed(std::string why);

    std::shared_ptr<docs::Browser> browser_;
    std::string address_;
    std::string title_;
    docs::Browsing how_;
    std::shared_ptr<docs::Browsed> view_;
    std::shared_ptr<Image> image_;
    std::shared_ptr<Node> shown_;
    Area inside_;
    Mode mode_ = Mode::none;
    media::Size size_{};
    std::uint64_t seen_ = 0;
    std::uint64_t skipped_ = 0;
    bool opening_ = false;
    bool reading_ = false;
    bool broken_ = false;
    bool shooting_ = false;
    bool again_ = false;
    bool loading_ = false;
    std::optional<Scope> scope_;
    std::optional<Scope> watching_;
    Timer refresh_;
    Timer tick_;
    Hooks hooks_;
};

}

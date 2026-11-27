#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/docs.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/image.hpp"
#include "tal/node.hpp"
#include "tal/pixels.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/views.hpp"

namespace tal {

struct Turned : Event {
    explicit Turned(std::size_t at) : page(at) {}

    std::size_t page;
};

struct Light {
    std::size_t page = 0;
    docs::Region region;
    bool current = false;

    friend bool operator==(const Light&, const Light&) = default;
};

class Leaves : public Scrollable {
public:
    enum class Layout { continuous, single };

    explicit Leaves(std::shared_ptr<docs::Document> document);

    ~Leaves() override;

    Property<Layout> layout{*this, Effect::layout, Layout::continuous};

    Property<std::size_t> zoom{*this, Effect::layout, 0};

    Property<std::uint64_t> most{*this, Effect::layout, std::uint64_t{64} << 20};

    [[nodiscard]] static std::size_t zooms();

    [[nodiscard]] std::size_t page() const { return current_; }

    void page(std::size_t which);

    [[nodiscard]] std::size_t left() const { return left_; }

    void left(std::size_t column);

    [[nodiscard]] std::uint64_t held() const { return held_; }

    [[nodiscard]] bool busy() const;

    [[nodiscard]] const std::vector<Light>& lights() const { return lights_; }

    void lights(std::vector<Light> given);

    [[nodiscard]] std::size_t extent() const override { return extent_; }

    [[nodiscard]] std::size_t window() const override { return window_; }

    [[nodiscard]] std::size_t top() const override { return top_; }

    void top(std::size_t row) override;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    struct Key {
        std::size_t page = 0;
        std::uint32_t wide = 0;
        std::uint32_t high = 0;
        std::uint32_t across = 0;
        std::uint32_t down = 0;

        friend auto operator<=>(const Key&, const Key&) = default;
    };

    struct Tile {
        media::Pixels pixels;
        std::uint64_t bytes = 0;
        std::uint64_t used = 0;
    };

    struct Sheet {
        int columns = 0;
        int rows = 0;
        int from = 0;
    };

    struct Slice {
        std::size_t page = 0;
        Area area;
        Area part;
    };

    struct Shown {
        std::shared_ptr<Image> image;
        std::tuple<std::size_t, Size, Area, Size> held;
    };

    void sized(Size view);

    [[nodiscard]] std::vector<Slice> slicing() const;

    [[nodiscard]] std::optional<Slice> sliced(std::size_t page, int from, std::size_t top) const;

    [[nodiscard]] std::tuple<std::size_t, Size, Area, Size> wanting(const Slice& slice) const;

    [[nodiscard]] bool showing(const Slice& slice) const;

    [[nodiscard]] bool complete(const Slice& slice) const;

    void refreshed();

    void scheduled();

    void asked();

    void anchored(bool single);

    void moved(std::size_t row);

    [[nodiscard]] std::size_t under(std::size_t row) const;

    [[nodiscard]] std::vector<Key> covering(std::size_t page, Area part) const;

    [[nodiscard]] std::optional<media::Pixels> composed(std::size_t page, Area part);

    void wanted(const std::vector<Key>& visible, const std::vector<Key>& ahead);

    void lit(std::size_t page, std::uint32_t wide, std::uint32_t high, Area part, Bytes& rgba) const;

    void pump();

    void kept(const Key& key, media::Pixels pixels);

    void trimmed(const std::vector<Key>& pinned);

    [[nodiscard]] Task<void> fetched(Key key);

    std::shared_ptr<docs::Document> document_;
    std::vector<docs::Page> pages_;
    std::vector<Sheet> sheets_;
    Area inside_;
    Size view_;
    Layout laid_ = Layout::continuous;
    Size step_;
    std::size_t extent_ = 0;
    std::size_t window_ = 0;
    std::size_t top_ = 0;
    std::size_t left_ = 0;
    std::size_t current_ = 0;
    std::optional<std::pair<std::size_t, double>> anchor_;
    std::vector<Slice> slices_;
    std::map<std::size_t, Shown> shown_;
    std::map<Key, Tile> tiles_;
    std::uint64_t held_ = 0;
    std::uint64_t uses_ = 0;
    std::vector<Key> wanted_;
    std::vector<Key> pinned_;
    std::set<Key> flying_;
    std::map<std::size_t, std::string> failed_;
    std::vector<Light> lights_;
    std::optional<Scope> scope_;
    Timer refresh_;
    Hooks hooks_;
};

[[nodiscard]] std::shared_ptr<Node> transcript(const docs::Plain& plain, std::string why);

[[nodiscard]] Task<std::shared_ptr<Node>> previewed(std::shared_ptr<docs::Renderer> renderer, std::shared_ptr<meta::Reader> from, std::string password = {});

}

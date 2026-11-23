#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "tal/capabilities.hpp"
#include "tal/document.hpp"
#include "tal/drawing.hpp"
#include "tal/event.hpp"
#include "tal/node.hpp"
#include "tal/pixels.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

namespace tal {

class Drawing : public Node {
public:
    using Painter = std::function<void(drawing::Pen& pen, media::Size size, std::chrono::nanoseconds since)>;

    explicit Drawing(Painter painter, drawing::Blending blending = drawing::Blending::linear);

    ~Drawing() override;

    void rate(double frames);

    void redraw();

    void force(std::optional<Graphics> graphics);

    [[nodiscard]] media::Size pixels() const { return size_; }

    [[nodiscard]] Graphics graphics() const { return fine_; }

    [[nodiscard]] std::optional<drawing::Point> pointed(const MouseEvent& event) const;

    [[nodiscard]] bool busy() const;

    [[nodiscard]] const std::optional<Trouble>& trouble() const { return trouble_; }

    [[nodiscard]] std::uint64_t frames() const { return frames_; }

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    struct Band {
        std::uint64_t hash = 0;
        int rows = 0;
        Picture picture;
    };

    struct Encoded {
        std::vector<Band> bands;
        media::Pixels coarse;
    };

    void drawn();

    void ticking();

    [[nodiscard]] Task<void> encoding(std::uint64_t order, media::Pixels frame);

    [[nodiscard]] static Encoded encoded(const media::Pixels& frame, Graphics fine, Size cell, Size room, Size grain, const std::vector<Band>& before);

    Painter painter_;
    drawing::Blending blending_;
    double rate_ = 0;
    std::optional<Graphics> forced_;
    Size room_;
    Graphics fine_ = Graphics::none;
    Blocks blocks_ = Blocks::basic;
    bool unicode_ = true;
    Size cell_;
    media::Size size_;
    media::Pixels frame_;
    media::Pixels coarse_;
    std::vector<Band> bands_;
    std::optional<std::chrono::steady_clock::time_point> started_;
    std::optional<Trouble> trouble_;
    std::uint64_t orders_ = 0;
    std::uint64_t frames_ = 0;
    bool encoding_ = false;
    bool pending_ = false;
    Timer timer_;
    std::optional<Scope> scope_;
};

}

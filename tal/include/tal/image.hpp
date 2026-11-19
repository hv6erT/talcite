#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/capabilities.hpp"
#include "tal/document.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/pixels.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"

namespace tal {

class Image : public Node {
public:
    Image();

    ~Image() override;

    void hold(Bytes given);

    void hold(media::Pixels given);

    void clear();

    [[nodiscard]] bool held() const;

    [[nodiscard]] bool busy() const;

    [[nodiscard]] media::Format format() const { return format_; }

    [[nodiscard]] media::Size size() const { return size_; }

    [[nodiscard]] std::size_t frames() const { return frames_; }

    void play(bool on);

    [[nodiscard]] bool play() const { return play_; }

    void still(std::size_t which);

    [[nodiscard]] std::size_t still() const { return shown_; }

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    void reset();

    void spread(Size given);

    void sharpened();

    void beating();

    [[nodiscard]] Task<void> decoding(std::uint64_t order, Bytes given, media::Size want, Size room, Graphics fine);

    [[nodiscard]] Task<void> encoding(std::uint64_t order, media::Pixels given, Size room);

    [[nodiscard]] bool started(Task<void> work);

    Bytes bytes_;
    media::Pixels whole_;
    media::Format format_ = media::Format::none;
    media::Size size_;
    std::size_t frames_ = 0;

    std::uint64_t orders_ = 0;
    bool ordered_ = false;
    media::Size ordering_;

    media::Pixels fit_;
    media::Pixels coarse_;
    std::vector<Picture> pictures_;
    Size shaped_;
    Size placed_;
    Graphics fine_ = Graphics::none;
    Blocks made_ = Blocks::basic;
    bool unicode_ = true;
    Size cell_;

    std::size_t shown_ = 0;
    bool play_ = true;
    Timer frame_;
    std::optional<Scope> scope_;
};

}

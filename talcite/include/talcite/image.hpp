#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "talcite/mosaic.hpp"
#include "talcite/node.hpp"
#include "talcite/pixels.hpp"
#include "talcite/promise.hpp"
#include "talcite/timer.hpp"

namespace talcite::ui {

class Image : public Node {
public:
    Image();

    void hold(Figure given);

    void hold(Pixels given);

    void clear();

    [[nodiscard]] bool held() const;

    [[nodiscard]] bool busy() const;

    [[nodiscard]] Format format() const { return format_; }

    [[nodiscard]] Size size() const { return size_; }

    [[nodiscard]] std::size_t frames() const { return frames_; }

    void play(bool given);

    [[nodiscard]] bool play() const { return play_; }

    void still(std::size_t given);

    [[nodiscard]] std::size_t still() const;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

private:
    void spread(Size given);

    [[nodiscard]] std::size_t chosen() const;

    [[nodiscard]] std::optional<std::chrono::milliseconds> until_next() const;

    void sharpened();

    Fetch fetch_;
    std::optional<Promise<void>> ordered_;
    Size ordering_;
    Pixels whole_;
    Format format_ = Format::none;
    Size size_;
    std::size_t frames_ = 0;

    Pixels fit_;
    Pixels coarse_;
    Graphics fine_ = Graphics::none;
    std::vector<Picture> pictures_;
    std::optional<Promise<void>> encoded_;
    Size shaped_;
    Size placed_;
    Blocks made_ = Blocks::basic;
    bool unicode_ = true;
    std::vector<std::chrono::milliseconds> upto_;
    std::chrono::milliseconds total_{0};

    std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();
    std::size_t still_ = 0;
    std::size_t shown_ = 0;
    bool play_ = true;
    Timer frame_;
};

}

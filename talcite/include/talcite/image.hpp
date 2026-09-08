#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/mosaic.hpp"
#include "talcite/node.hpp"
#include "talcite/pixels.hpp"

namespace talcite::ui {

class Image : public Node {
public:
    Image();

    void hold(Bytes raw);

    void hold(Pixels given);

    [[nodiscard]] bool held() const;

    [[nodiscard]] Picture kind() const { return kind_; }

    [[nodiscard]] Size shape() const { return shape_; }

    [[nodiscard]] std::size_t frames() const { return frames_; }

    void play(bool given);

    [[nodiscard]] bool play() const { return play_; }

    void still(std::size_t given);

    [[nodiscard]] std::size_t still() const;

    [[nodiscard]] int takes() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] std::optional<std::chrono::milliseconds> due() const override;

protected:
    View paint(Size given, Area seen) override;

private:
    void spread(Size given);

    [[nodiscard]] std::size_t chosen() const;

    Bytes raw_;
    Pixels whole_;
    Picture kind_ = Picture::none;
    Size shape_;
    std::size_t frames_ = 0;

    Pixels fit_;
    Size shaped_;
    Size placed_;
    Blocks made_ = Blocks::basic;
    std::vector<std::chrono::milliseconds> upto_;
    std::chrono::milliseconds total_{0};

    std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();
    std::size_t still_ = 0;
    bool play_ = true;
};

}

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "talcite/bytes.hpp"
#include "talcite/feed.hpp"
#include "talcite/page.hpp"

namespace talcite::ui {

class Viewer : public Page {
public:
    Viewer();

    explicit Viewer(ScrollStyle style);

    ~Viewer() override;

    using Page::hold;

    void hold(Bytes given);

    void hold(Feed given, std::size_t whole);

    [[nodiscard]] std::optional<std::size_t> at() const override;

    [[nodiscard]] std::optional<std::size_t> whole() const override;

    [[nodiscard]] std::optional<std::size_t> lines() const override;

    void at(std::size_t given);

protected:
    [[nodiscard]] std::optional<std::size_t> below() const override;

    void counting() override;

    void screen(int high) override;

private:
    struct Kept;

    std::unique_ptr<Kept> kept_;
};

}

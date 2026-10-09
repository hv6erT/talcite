#pragma once

#include <cstddef>

#include "talcite/bytes.hpp"
#include "talcite/feed.hpp"
#include "talcite/page.hpp"

namespace talcite::ui {

class Viewer : public Page {
public:
    Viewer();

    explicit Viewer(ScrollShape shape);

    ~Viewer() override;

    void hold(Bytes given);

    void hold(Feed given, std::size_t whole, Bytes head = {});

    void hold(Roll given, std::size_t lines);
};

}

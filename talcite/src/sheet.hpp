#pragma once

#include <vector>

#include "talcite/area.hpp"
#include "talcite/snapshot.hpp"
#include "talcite/style.hpp"

namespace talcite {

class Canvas;

}

namespace talcite::detail {

struct Placement {
    ui::Spot at;
    Picture picture;
};

class Sheet : public Snapshot {
public:
    Sheet() = default;

    explicit Sheet(ui::Size given);

    void put(ui::Spot at, char32_t what, const Style& how);

    void fill(const ui::Area& where, const Style& how);

    void clear(const ui::Area& where);

    void picture(const ui::Area& where, Picture what);

    void link(const ui::Area& where, Hyperlink what);

    [[nodiscard]] std::vector<Placement> bared(const ui::Area& room);

    void forget();

private:
    friend class talcite::Canvas;

    Cell& cell(ui::Spot where);

    void broken(ui::Spot where);
};

}

#include "tal/testing.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/span.hpp"
#include "talcite/status.hpp"

using namespace talcite;

// A status line keeps its row while it says nothing, where an empty label takes no room: the
// window under it does not jump when the text comes and goes.
TRIAL("Status: an empty one keeps its row, an empty span takes none") {
    const ui::Status empty;

    SAME(empty.wants({20, 5}), (ui::Size{0, 1}));
    SAME(empty.wants({20, 0}), (ui::Size{0, 0}));
    SAME(ui::Span{""}.wants({20, 5}), (ui::Size{0, 0}));

    ui::Status said{"Ready"};

    SAME(said.wants({20, 5}), (ui::Size{5, 1}));

    said.wrapped(true);
    said.text("one two three");

    SAME(said.wants({7, 5}).high, 2);
}

// In a column the row stays: what stands under the line keeps its place when the text goes.
TRIAL("Status: in a column what stands under it keeps its place") {
    ui::Column one;

    const auto line = std::make_shared<ui::Status>("Missing a name");
    const auto under = std::make_shared<ui::Span>("OK");

    one.children().add(line);
    one.children().add(under);

    (void)one.render({{0, 0}, {20, 3}});

    SAME(under->area().at.y, 1);

    line->text({});

    (void)one.render({{0, 0}, {20, 3}});

    SAME(under->area().at.y, 1);
}

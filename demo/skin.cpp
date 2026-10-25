#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/gauge.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/keybar.hpp"
#include "talcite/line.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    // F10 stops the terminal; every other key is reported as handled.
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

// A style with the given foreground and background and nothing else set.
talcite::Style painted(talcite::Color foreground, talcite::Color background) {
    talcite::Style made{};

    made.foreground = foreground;
    made.background = background;

    return made;
}

// A true-color value from its three components.
talcite::Color tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return talcite::Color{false, red, green, blue};
}

// A colored skin: the same roles, different values. No widget knows about it; each asks for its
// role and gets what this skin says.
talcite::ui::Skin coloured() {
    const talcite::Color background = tinted(0, 0, 140);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color white = tinted(255, 255, 255);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, background)},      {"talcite.frame", painted(light, background)},
        {"talcite.title", painted(yellow, background)},

        {"talcite.list", painted(light, background)},        {"talcite.list.focus", painted(black, cyan)},
        {"talcite.list.mark", painted(white, background)},

        {"talcite.menubar", painted(black, light)},     {"talcite.menubar.focus", painted(black, cyan)},
        {"talcite.menu", painted(black, light)},        {"talcite.menu.focus", painted(black, cyan)},
        {"talcite.menu.frame", painted(black, light)},

        {"talcite.legend.key", painted(light, black)},  {"talcite.legend.text", painted(black, cyan)},

        {"talcite.input", painted(black, cyan)},        {"talcite.button", painted(black, light)},
        {"talcite.button.focus", painted(black, cyan)}, {"talcite.button.key", painted(yellow, light)},

        {"talcite.scroll", painted(light, background)},      {"talcite.scroll.thumb", painted(cyan, background)},

        {"talcite.gauge", painted(light, background)},       {"talcite.gauge.done", painted(cyan, background)},
        {"talcite.gauge.text", painted(yellow, background)},
    }};
}

// A skin for a terminal without box-drawing characters. This is the whole ASCII fallback: not a
// branch in the code, only a second palette.
talcite::ui::Skin plainly() {
    return talcite::ui::Skin{{},
                          {
                              {"talcite.line.thin.across", U'-'},
                              {"talcite.line.thin.upright", U'|'},
                              {"talcite.line.thin.upper.left", U'+'},
                              {"talcite.line.thin.upper.right", U'+'},
                              {"talcite.line.thin.lower.left", U'+'},
                              {"talcite.line.thin.lower.right", U'+'},
                              {"talcite.line.thin.left.joint", U'+'},
                              {"talcite.line.thin.right.joint", U'+'},

                              {"talcite.scroll.thumb", U'#'},
                              {"talcite.scroll.track.upright", U'|'},
                              {"talcite.scroll.track.across", U'-'},

                              {"talcite.gauge.done", U'#'},
                              {"talcite.gauge.rest", U'.'},

                              {"talcite.elide", U'~'},
                          }};
}

}

// One screen, three palettes. Switching is one call on the root: no notifications, no walk over the
// tree and no rebuilding of anything; the next frame simply reads something else.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto bar = std::make_shared<talcite::ui::KeyBar>();
    const auto menus = std::make_shared<talcite::ui::Menubar>();

    // The application gives the key that opens the menus; the library assigns none by itself.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — skins");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("built-in skin");

    const auto list =
        std::make_shared<talcite::ui::List>(talcite::ui::plenty, 6, talcite::ui::ScrollShape{true});

    for (int which = 0; which < 30; ++which)
        list->children().add(std::make_shared<talcite::ui::ListItem>("a very long file name, number " +
                                                       std::to_string(which) + ".txt"));

    const auto field = std::make_shared<talcite::ui::Field>(30, "name");

    const auto gauge = std::make_shared<talcite::ui::Gauge>(40);

    gauge->part(0.62);

    const auto yes = std::make_shared<talcite::ui::Button>("Change");
    const auto no = std::make_shared<talcite::ui::Button>("Stop");

    yes->keys().add(U'C');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);

    column->children().add(told);
    column->children().add(std::make_shared<talcite::ui::Line>("Files"));
    column->children().add(list);
    column->children().add(std::make_shared<talcite::ui::Line>("Name"));
    column->children().add(field);
    column->children().add(gauge);
    column->children().add(row);

    frame->children().add(column);
    menus->content(frame);
    bar->content(menus);

    const auto file = menus->add("File");

    file->children().add(std::make_shared<talcite::ui::MenuItem>("Copy", "F5"));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Exit", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("View");

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f2, "Plain"), [&sheet, told] {
        sheet.skin(talcite::ui::Skin{});
        told->text("built-in skin");
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f3, "Color"), [&sheet, told] {
        sheet.skin(coloured());
        told->text("color skin — the same roles, different values");
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f4, "ASCII"), [&sheet, told] {
        sheet.skin(plainly());
        told->text("ASCII skin — plain characters, no box drawing");
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f9, "PullDn"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(bar);

    list->focus();

    term.show(sheet);
    term.title("talcite — skins");

    term.run();

    return 0;
}

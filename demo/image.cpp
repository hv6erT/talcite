#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/box.hpp"
#include "talcite/column.hpp"
#include "talcite/focusholder.hpp"
#include "talcite/frame.hpp"
#include "talcite/image.hpp"
#include "talcite/keybar.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/mosaic.hpp"
#include "talcite/pixels.hpp"
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

// A picture of the given size whose every pixel is computed by `fn`, from coordinates running from
// -1 to 1 across the picture, as a single still frame of RGBA bytes.
talcite::Pixels painted(int wide, int high, void (*fn)(double, double, std::uint8_t*)) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(wide) * high * 4);

    for (int y = 0; y < high; ++y)
        for (int x = 0; x < wide; ++x)
            fn((x + 0.5) / wide * 2 - 1, (y + 0.5) / high * 2 - 1,
               bytes.data() + (static_cast<std::size_t>(y) * wide + x) * 4);

    talcite::Bytes lump;

    lump.reserve(bytes.size());

    for (const std::uint8_t one : bytes) lump.push_back(std::byte{one});

    return talcite::Pixels{{wide, high}, {talcite::Still{std::move(lump), std::chrono::milliseconds{0}}}};
}

// A color wheel: hue from the angle, saturation from the radius. It checks the cell proportion (the
// wheel has to be round) and whether the cuts follow the edge of the color rather than the
// brightness.
void wheeled(double x, double y, std::uint8_t* at) {
    const double r = std::sqrt(x * x + y * y);

    if (r > 1.0) {
        at[3] = 0;

        return;
    }

    const double turn = (std::atan2(y, x) / 3.14159265358979 + 1.0) * 3.0;
    const double part = turn - std::floor(turn);

    const double up = 255.0 * r;
    const double down = 255.0 * r * (1.0 - part);
    const double rise = 255.0 * r * part;
    const double rest = 255.0 * (1.0 - r);

    double one[3] = {0, 0, 0};

    switch (static_cast<int>(turn) % 6) {
    case 0:
        one[0] = up;
        one[1] = rise;
        break;
    case 1:
        one[0] = down;
        one[1] = up;
        break;
    case 2:
        one[1] = up;
        one[2] = rise;
        break;
    case 3:
        one[1] = down;
        one[2] = up;
        break;
    case 4:
        one[0] = rise;
        one[2] = up;
        break;
    default:
        one[0] = up;
        one[2] = down;
        break;
    }

    for (std::size_t band = 0; band < 3; ++band)
        at[band] = static_cast<std::uint8_t>(one[band] + rest);

    at[3] = 255;
}

// A ring with a hole: transparency shows when the frame's background shines through the middle.
void ringed(double x, double y, std::uint8_t* at) {
    const double r = std::sqrt(x * x + y * y);

    at[0] = 255;
    at[1] = 190;
    at[2] = 0;
    at[3] = r <= 1.0 && r >= 0.5 ? 255 : 0;
}

// A slant: the hardest test for shapes. Block characters have quarters and eighths for it, sextants
// a denser grid, and this is where the difference shows best.
void slanted(double x, double y, std::uint8_t* at) {
    const bool over = y < x;

    at[0] = over ? 250 : 20;
    at[1] = over ? 240 : 30;
    at[2] = over ? 210 : 90;
    at[3] = 255;
}

// A gradient: it checks averaging. Averaging the raw bytes makes the middle noticeably darker than
// it should be.
void faded(double x, double y, std::uint8_t* at) {
    const double one = (x + 1.0) / 2.0;
    const double two = (y + 1.0) / 2.0;

    at[0] = static_cast<std::uint8_t>(255.0 * one);
    at[1] = static_cast<std::uint8_t>(255.0 * two);
    at[2] = static_cast<std::uint8_t>(255.0 * (1.0 - one));
    at[3] = 255;
}

// The name of the grid density shown on the status line.
std::string told(talcite::ui::Blocks kind) {
    return kind >= talcite::ui::Blocks::sextants ? "sextants (2x3)" : "block characters (8x8)";
}

}

// No file and no built-in GIF. The decoders live in talcite-adapters, and talcite does not link the
// adapters, so the demo shows what the widget does with pictures made in code: the cell proportion,
// cuts that follow the edge, transparency that lets the frame's background through, and the
// difference between the two grid densities. Animation from a file and real files from the disk are
// viewed by the program that checks the adapter (talcite-stb-check).
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto bar = std::make_shared<talcite::ui::KeyBar>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — images");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto said = std::make_shared<talcite::ui::Span>(std::string{});

    const auto shown = std::make_shared<talcite::ui::Image>();

    shown->hold(painted(240, 240, wheeled));

    const auto holder = std::make_shared<talcite::ui::FocusHolder>();

    column->children().add(said);
    column->children().add(shown);

    frame->children().add(column);
    holder->children().add(frame);

    // The holder covers the whole content of the window, whatever the frame wants: a click anywhere
    // in it takes the focus, and without focus the key bar would not get the keys.
    const auto room = std::make_shared<talcite::ui::Box>();

    room->children().add(holder);
    bar->content(room);

    // What is shown now, kept separately rather than extracted from the text on the screen: the
    // text is for a human, not a store of program state.
    const auto subject = std::make_shared<std::string>("color wheel");

    // The application's wish for the terminal. We keep it ourselves, because `reconfigure` goes
    // through the loop and takes effect only at a frame boundary, while we want the caption right
    // away.
    const auto wish = std::make_shared<talcite::Setup>();

    // The motion state comes first and is always shown, so that F7 never looks dead at the very
    // moment one most wants to know whether it works.
    const auto naming = [said, shown, subject, wish] {
        // The text gives the state of the switch, not whether something happens to be moving,
        // because only then does F7 show from the first moment. The frame count appears only after
        // the first layout, because a GIF does not give it in its header, and it is the only thing
        // here that shows up late.
        std::string more = shown->play() ? "moving" : "stop";

        more += " · " + *subject;

        if (shown->frames() > 1) more += ", frames " + std::to_string(shown->frames());

        more += " · " + told(wish->blocks.value_or(talcite::ui::Blocks::basic));

        said->text(more);
    };

    const auto picking = [shown, naming, subject](const char* what, talcite::Pixels given) {
        return [shown, naming, subject, what, given = std::move(given)] {
            shown->hold(given);

            *subject = what;

            naming();
        };
    };

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f2, "Wheel"),
                      picking("color wheel", painted(240, 240, wheeled)));

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f3, "Ring"),
                      picking("ring with a hole", painted(240, 240, ringed)));

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f4, "Slant"),
                      picking("slant", painted(240, 240, slanted)));

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f5, "Gradient"),
                      picking("gradient", painted(240, 240, faded)));

    // Sextants are not turned on from a measurement. The probe only tells whether the character
    // takes one cell, not whether the font has it, so here the user makes the wish, after one key
    // press and one look at the screen. Windows Terminal substitutes the font and shows them even
    // without Cascadia.
    //
    // The widget has nothing to switch: the density lies on the root, and `Term` puts it there. So
    // the key tells the terminal, and the picture learns about it on the next frame by itself,
    // through the comparison in `spread()`. This is the only call to `reconfigure` in the demos.
    (void)demo::doing(heard, bar->add(talcite::ui::Code::f6, "Grid"), [&term, wish, naming] {
        wish->blocks = wish->blocks.value_or(talcite::ui::Blocks::basic) <
                               talcite::ui::Blocks::sextants
                           ? talcite::ui::Blocks::sextants
                           : talcite::ui::Blocks::basic;

        term.reconfigure(*wish);

        naming();
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f7, "Motion"), [shown, naming] {
        shown->play(!shown->play());

        naming();
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(bar);

    holder->focus();

    // Animation runs by itself when there is something to animate: the widget tells the root when
    // it wants to be woken at the latest, and `Term` puts that into its limiter; F7 only stops and
    // resumes it. Pictures made in code do not move, so here F7 shows only the state of the switch;
    // motion from a file is viewed in talcite-stb-check.
    naming();

    term.show(sheet);
    term.title("talcite — images");

    term.run();

    return 0;
}

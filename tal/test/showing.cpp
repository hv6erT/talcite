#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tal/app.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/crypto.hpp"
#include "tal/document.hpp"
#include "tal/node.hpp"
#include "tal/pixels.hpp"
#include "tal/surface.hpp"

#include "tal/testing.hpp"

// Pictures on the terminal: `App` sends pixels only for a picture seen whole, a sixel after the
// cells it lies on and only when they changed, a kitty picture sent once and then only placed and
// taken away. The scripted console shows every byte, so one sees when a picture goes and when not.

namespace {

using tal::App;
using tal::Canvas;
using tal::Color;
using tal::Document;
using tal::Graphics;
using tal::Look;
using tal::ManualClock;
using tal::Node;
using tal::Picture;
using tal::Port;
using tal::ScriptedConsole;
using tal::Setup;
using tal::Size;
using tal::Spot;
using tal::media::Pixels;
using tal::media::Still;

const std::string sixel_head = "\x1bP0;1;0q";
const std::string kitty_send = "\x1b_Ga=t,";

// The text as the shared bytes of a picture.
std::shared_ptr<const std::string> encoded(std::string said) { return std::make_shared<const std::string>(std::move(said)); }

// A look of only the paper red, green, blue.
Look paper(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Look made;

    made.paper = Color{red, green, blue};

    return made;
}

// Whether `what` occurs in `where`.
bool holds(const std::string& where, const std::string& what) { return where.find(what) != std::string::npos; }

// How many times `what` occurs in `where`, overlaps included.
std::size_t counted(const std::string& where, const std::string& what) {
    std::size_t made = 0;

    for (std::size_t at = where.find(what); at != std::string::npos; at = where.find(what, at + 1)) ++made;

    return made;
}

// The pixels of a picture of `width` by `height` with `stills` frames, each of other bytes, so that
// one sees which went.
std::shared_ptr<const Pixels> dotted(std::uint32_t width, std::uint32_t height, int stills = 1) {
    std::vector<Still> made;

    for (int still = 0; still < stills; ++still) {
        tal::Bytes one;

        for (std::uint32_t at = 0; at < width * height * 4; ++at) one.push_back(static_cast<std::byte>(at + 40 * static_cast<std::uint32_t>(still)));

        made.push_back(Still{one, std::chrono::milliseconds{100}});
    }

    return std::make_shared<const Pixels>(tal::media::Size{width, height}, std::move(made));
}

// The kitty command that sends frame `still` of `pixels` in one piece under the mark `mark`.
std::string sent_whole(std::uint32_t mark, const Pixels& pixels, std::size_t still) {
    return "\x1b_Ga=t,f=32,s=" + std::to_string(pixels.size().width) + ",v=" + std::to_string(pixels.size().height) + ",i=" + std::to_string(mark) +
           ",q=2,m=0;" + tal::crypto::base64(pixels.stills()[still].rgba) + "\x1b\\";
}

// The jump to `spot` and the kitty command that places the picture `mark` in 3 by 2 cells.
std::string stood_at(const std::string& spot, std::uint32_t mark) {
    return "\x1b[" + spot + "H\x1b_Ga=p,i=" + std::to_string(mark) + ",c=3,r=2,C=1,q=2\x1b\\";
}

// The kitty command that takes the picture `mark` away: from the screen with `i`, its pixels too
// with `I`.
std::string taken(std::uint32_t mark, char how = 'i') { return "\x1b_Ga=d,d=" + std::string(1, how) + ",i=" + std::to_string(mark) + ",q=2\x1b\\"; }

// A node over the whole screen on a blue paper, with a 3 by 2 picture of characters (▒) at `at`
// and letters elsewhere; the picture can be moved, covered, replaced, or given no bytes.
class Framed : public Node {
public:
    Spot at{2, 1};
    Size size{3, 2};
    std::uint32_t mark = 5;
    bool covered = false;
    bool bare = false;
    bool blank = false;
    std::vector<tal::Area> shapes;
    std::string elsewhere = " ";
    Spot other{0, 0};
    std::string yonder = " ";
    Spot afar{0, 0};
    std::shared_ptr<const Pixels> pixels = dotted(3, 2, 2);
    std::size_t still = 0;

    // Paints it again with what changed.
    void again() { repaint(); }

    // Paints the cells of `part` again, and only them.
    void again(tal::Area part) { repaint(part); }

protected:
    // The whole room.
    Size measure(tal::Limits room) const override { return room.most; }

    // The paper, the letters, the picture's characters marked with the picture, and the letter that
    // covers one of them when `covered`.
    void paint(Canvas& onto) const override {
        onto.fill({{0, 0}, onto.size()}, paper(0, 0, 90));
        (void)onto.put(other, elsewhere, paper(0, 0, 90));

        if (yonder != " ") (void)onto.put(afar, yonder, paper(0, 0, 90));

        for (int y = 0; y < size.high; ++y)
            for (int x = 0; x < size.wide; ++x) (void)onto.put({at.x + x, at.y + y}, blank && x == 2 && y == 1 ? " " : "▒", paper(200, 100, 0));

        const Picture picture{mark, size, bare ? nullptr : encoded("\x1bP0;1;0q\"1;1;30;40#0;2;100;0;0\x1b\\"), bare ? nullptr : pixels, still};

        if (shapes.empty()) onto.picture({at, size}, picture);

        for (const tal::Area& one : shapes) onto.picture(one, picture);

        if (covered) (void)onto.put({at.x + 1, at.y}, "x", paper(0, 0, 90));
    }
};

// Setup with graphics of `kind` and a cell of `cell` pixels.
Setup pixeled(Size cell = {10, 20}, Graphics kind = Graphics::sixel) {
    Setup made;

    made.graphics = kind;
    made.cell = cell;

    return made;
}

// Setup with kitty and a cell of 10 by 20 pixels.
Setup kittied() { return pixeled({10, 20}, Graphics::kitty); }

// An application on a scripted console of `window` showing a `Framed` node.
struct Rig {
    std::shared_ptr<ScriptedConsole> line = std::make_shared<ScriptedConsole>();
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    Document document{Port<tal::Clock>{clock}};
    Framed& root = document.children().add<Framed>();
    std::unique_ptr<App> app;

    // The application with `wish` on a window of `window` cells.
    explicit Rig(Setup wish = pixeled(), Size window = {12, 6}) {
        line->window = window;
        app = std::make_unique<App>(std::move(wish), Port<tal::Console>{line}, Port<tal::Clock>{clock});
        app->show(document);
    }

    // What one turn wrote to the console.
    std::string drawn() {
        line->written.clear();
        (void)app->turn();

        return line->written;
    }

    // Changes the wish and lets the application take it.
    void reconfigured(Setup wish) {
        app->reconfigure(std::move(wish));
        (void)app->loop().turn();
    }
};

}

// --- Sixel ---------------------------------------------------------------------------------------

TRIAL("App: a picture seen whole goes as a sixel, after its cells, in its place") {
    Rig rig;
    const std::string said = rig.drawn();
    const std::size_t at = said.find(sixel_head);

    OUGHT(at != std::string::npos);
    OUGHT(said.compare(at - 6, 6, "\x1b[2;3H") == 0);
    OUGHT(said.find("\x1b[0;48;5;") < at);
    OUGHT(!holds(said, "▒"));
    SAME(counted(said, sixel_head), std::size_t{1});
}

TRIAL("App: an unchanged picture does not go again, nor does a change beside it send it") {
    Rig rig;

    (void)rig.drawn();

    SAME(rig.drawn(), std::string{});

    rig.root.elsewhere = "k";
    rig.root.again();

    const std::string said = rig.drawn();

    OUGHT(holds(said, "k"));
    OUGHT(!holds(said, sixel_head));
}

// A changed word the terminal may count otherwise takes the rest of its row with it, the picture's
// cells too; writing them erases the pixels, so the picture goes again. The row goes again from
// the first such word, not the last; a word after the picture leaves it standing.
TRIAL("App: a picture in a row going out again after a word the terminal counts otherwise goes anew") {
    Rig rig;

    rig.root.other = {0, 1};
    rig.root.elsewhere = "ा";
    rig.root.again();

    (void)rig.drawn();

    rig.root.elsewhere = "ी";
    rig.root.again();

    SAME(counted(rig.drawn(), sixel_head), std::size_t{1});

    rig.root.yonder = "ा";
    rig.root.afar = {7, 1};
    rig.root.again();

    (void)rig.drawn();

    rig.root.elsewhere = "ा";
    rig.root.yonder = "ी";
    rig.root.again();

    SAME(counted(rig.drawn(), sixel_head), std::size_t{1});

    rig.root.other = {0, 4};
    rig.root.again();

    (void)rig.drawn();

    rig.root.elsewhere = "ी";
    rig.root.again();

    OUGHT(!holds(rig.drawn(), sixel_head));
}

// Covering and uncovering change one cell of one row, and yet every row of the picture goes out
// again: whether it shows whole depends on all of them, and so do the blank cells under it.
TRIAL("App: a covered picture goes back to characters, uncovered it gets its pixels anew") {
    Rig rig;

    (void)rig.drawn();

    rig.root.covered = true;
    rig.root.again({{3, 1}, {1, 1}});

    const std::string hidden = rig.drawn();

    OUGHT(!holds(hidden, sixel_head));
    OUGHT(holds(hidden, "x"));
    SAME(counted(hidden, "▒"), std::size_t{5});

    rig.root.covered = false;
    rig.root.again({{3, 1}, {1, 1}});

    const std::string back = rig.drawn();

    SAME(counted(back, sixel_head), std::size_t{1});
    OUGHT(!holds(back, "▒"));
    OUGHT(holds(back, "\x1b[3;3H"));
}

// A blank cell of the picture, blank in characters too, is written again when the picture is
// covered: the screen holds the picture's pixels there, which only writing the cell erases. The
// picture's blank cells go out in the paper the cells had.
TRIAL("App: a covered picture writes its blank cells again, and they go in the cells' paper") {
    Rig rig;

    rig.root.blank = true;
    rig.root.again();

    (void)rig.drawn();

    rig.root.covered = true;
    rig.root.again();

    OUGHT(holds(rig.drawn(), "▒▒ "));

    Setup plain;

    plain.cell = Size{10, 20};

    Rig none{plain};
    const std::string letters = none.drawn();
    const std::size_t lettered = letters.find("▒");
    const std::size_t dressed = letters.rfind("\x1b[0", lettered);
    const std::string look = letters.substr(dressed, lettered - dressed);
    Rig pixels;

    OUGHT(look.find("48;5;") != std::string::npos);
    OUGHT(holds(pixels.drawn(), look + "   "));
}

// A picture shows whole only in the shape it was given: the same number of cells in another shape
// (a row of four for 2 by 2, or two rows of two that do not stand one above the other) stays in
// characters.
TRIAL("App: cells in a shape other than the picture's are no picture seen whole") {
    Rig row;

    row.root.size = {2, 2};
    row.root.shapes = {{{2, 1}, {4, 1}}};
    row.root.again();

    OUGHT(!holds(row.drawn(), sixel_head));

    Rig slanted;

    slanted.root.size = {2, 2};
    slanted.root.shapes = {{{3, 1}, {2, 1}}, {{2, 2}, {2, 1}}};
    slanted.root.again();

    OUGHT(!holds(slanted.drawn(), sixel_head));

    Rig square;

    square.root.size = {2, 2};
    square.root.shapes = {{{2, 1}, {2, 1}}, {{2, 2}, {2, 1}}};
    square.root.again();

    OUGHT(holds(square.drawn(), sixel_head));
}

TRIAL("App: another mark in the same place, another frame of an animation, goes anew") {
    Rig rig;

    (void)rig.drawn();

    rig.root.mark = 6;
    rig.root.again();

    SAME(counted(rig.drawn(), sixel_head), std::size_t{1});
}

TRIAL("App: a moved picture goes to its new place, its old cells back to the paper") {
    Rig rig;

    (void)rig.drawn();

    rig.root.at = {5, 2};
    rig.root.again();

    const std::string said = rig.drawn();

    OUGHT(holds(said, "\x1b[3;6H" + sixel_head));
    OUGHT(holds(said, "\x1b[2;3H"));
}

// After a picture the cursor stands where the terminal put it: the cell right after the picture
// gets its own jump.
TRIAL("App: after a picture the next character goes with its own jump") {
    Rig rig;

    (void)rig.drawn();

    rig.root.mark = 9;
    rig.root.again();

    OUGHT(holds(rig.drawn(), sixel_head));

    rig.root.elsewhere = "k";
    rig.root.other = {5, 2};
    rig.root.again();

    OUGHT(holds(rig.drawn(), "\x1b[3;6H"));
}

// A picture without bytes has its cells blank and sends nothing, and does not stop a picture after
// it; one reaching the last row stays in characters, since after a sixel the cursor goes below it.
TRIAL("App: a picture without bytes sends nothing, one in the last row stays in characters") {
    Rig rig;

    rig.root.bare = true;
    rig.root.yonder = "y";
    rig.root.again();

    const std::string said = rig.drawn();

    OUGHT(!holds(said, sixel_head));
    OUGHT(!holds(said, "▒"));

    Rig low{pixeled(), {12, 3}};
    const std::string last = low.drawn();

    OUGHT(!holds(last, sixel_head));
    OUGHT(holds(last, "▒"));
}

TRIAL("App: without sixel, without a cell's size, or wishing no graphics, characters only") {
    Setup plain;

    plain.cell = Size{10, 20};

    Rig none{plain};

    OUGHT(!holds(none.drawn(), sixel_head));

    Setup blind;

    blind.graphics = Graphics::sixel;

    Rig unknown{blind};
    const std::string said = unknown.drawn();

    OUGHT(!holds(said, sixel_head));
    OUGHT(holds(said, "▒"));
}

// The document is told the terminal's graphics, and after a configuration the screen goes again
// whole: without graphics in characters, with them the picture anew.
TRIAL("App: a new configuration redraws the screen, the picture with it or in characters") {
    Rig rig;

    (void)rig.drawn();

    rig.reconfigured(pixeled());

    SAME(counted(rig.drawn(), sixel_head), std::size_t{1});

    Setup none = pixeled();

    none.graphics = Graphics::none;
    rig.reconfigured(none);

    const std::string said = rig.drawn();

    OUGHT(!holds(said, sixel_head));
    OUGHT(holds(said, "▒"));
}

// --- Kitty ---------------------------------------------------------------------------------------

TRIAL("App: kitty sends a picture once and places it in its cells, the last row too") {
    Rig rig{kittied()};
    const std::string said = rig.drawn();
    const std::size_t sending = said.find(sent_whole(5, *rig.root.pixels, 0));
    const std::size_t standing = said.find(stood_at("2;3", 5));

    OUGHT(sending != std::string::npos);
    OUGHT(standing != std::string::npos);
    OUGHT(sending < standing);
    OUGHT(!holds(said, sixel_head));
    OUGHT(!holds(said, "▒"));
    SAME(rig.drawn(), std::string{});

    rig.root.elsewhere = "k";
    rig.root.again();

    const std::string beside = rig.drawn();

    OUGHT(holds(beside, "k"));
    OUGHT(!holds(beside, "\x1b_G"));

    Rig low{kittied(), {12, 3}};

    OUGHT(holds(low.drawn(), stood_at("2;3", 5)));
}

TRIAL("App: a covered kitty picture goes before the characters, uncovered it comes back unsent") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.covered = true;
    rig.root.again();

    const std::string hidden = rig.drawn();

    OUGHT(holds(hidden, taken(5)));
    OUGHT(hidden.find(taken(5)) < hidden.find("▒"));
    SAME(counted(hidden, "▒"), std::size_t{5});
    OUGHT(!holds(hidden, kitty_send));
    OUGHT(!holds(hidden, "a=p"));

    rig.root.covered = false;
    rig.root.again();

    const std::string back = rig.drawn();

    OUGHT(holds(back, stood_at("2;3", 5)));
    OUGHT(!holds(back, kitty_send));
    OUGHT(!holds(back, "a=d"));
}

TRIAL("App: a moved kitty picture is taken away and placed anew, unsent") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.at = {5, 2};
    rig.root.again();

    const std::string said = rig.drawn();

    OUGHT(holds(said, taken(5)));
    OUGHT(holds(said, stood_at("3;6", 5)));
    OUGHT(!holds(said, kitty_send));
}

TRIAL("App: the frames of a kitty animation go once, then they are only switched") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.mark = 6;
    rig.root.still = 1;
    rig.root.again();

    const std::string second = rig.drawn();

    OUGHT(holds(second, taken(5)));
    OUGHT(holds(second, sent_whole(6, *rig.root.pixels, 1)));
    OUGHT(holds(second, stood_at("2;3", 6)));
    OUGHT(!holds(second, taken(5, 'I')));

    rig.root.mark = 5;
    rig.root.still = 0;
    rig.root.again();

    const std::string again = rig.drawn();

    OUGHT(holds(again, taken(6)));
    OUGHT(holds(again, stood_at("2;3", 5)));
    OUGHT(!holds(again, kitty_send));
}

// The terminal forgets a picture whose pixels nobody here holds, in the frame that let go of them;
// a covered one keeps them, since its node still holds them.
TRIAL("App: the terminal forgets a kitty picture once nobody holds its pixels") {
    Rig rig{kittied()};

    (void)rig.drawn();

    const std::weak_ptr<const Pixels> old = rig.root.pixels;

    rig.root.pixels = dotted(3, 2);
    rig.root.mark = 8;
    rig.root.again();

    const std::string said = rig.drawn();

    OUGHT(holds(said, taken(5)));
    OUGHT(old.expired());
    OUGHT(holds(said, taken(5, 'I')));
    OUGHT(holds(said, sent_whole(8, *rig.root.pixels, 0)));

    rig.root.covered = true;
    rig.root.again();

    OUGHT(!holds(rig.drawn(), taken(8, 'I')));
}

// Big pixels go in pieces of 4096 characters, m=0 only on the last; base64 that splits evenly
// leaves no empty piece at the end.
TRIAL("App: kitty sends big pixels in pieces of 4096 characters") {
    Rig rig{kittied()};

    rig.root.pixels = dotted(40, 40);
    rig.root.again();

    const std::string said = rig.drawn();
    const std::string coded = tal::crypto::base64(rig.root.pixels->stills()[0].rgba);

    SAME(coded.size(), std::size_t{8536});
    OUGHT(holds(said, "\x1b_Ga=t,f=32,s=40,v=40,i=5,q=2,m=1;" + coded.substr(0, 4096) + "\x1b\\" + "\x1b_Gq=2,m=1;" + coded.substr(4096, 4096) + "\x1b\\" +
                          "\x1b_Gq=2,m=0;" + coded.substr(8192) + "\x1b\\"));
    SAME(counted(said, "m=0;"), std::size_t{1});

    rig.root.pixels = dotted(48, 32);
    rig.root.mark = 6;
    rig.root.again();

    const std::string even = rig.drawn();
    const std::string whole = tal::crypto::base64(rig.root.pixels->stills()[0].rgba);

    SAME(whole.size(), std::size_t{8192});
    OUGHT(holds(even, "\x1b_Gq=2,m=0;" + whole.substr(4096) + "\x1b\\"));
    SAME(counted(even, "q=2,m="), std::size_t{2});
}

// A frame the pixels do not have is no picture for kitty: the cells stay in characters.
TRIAL("App: a frame beyond the pixels does not go to kitty") {
    Rig rig{kittied()};

    rig.root.still = 5;
    rig.root.again();

    const std::string said = rig.drawn();

    OUGHT(!holds(said, "\x1b_G"));
    OUGHT(holds(said, "▒"));
}

// Clearing the screen leaves kitty pictures, so a redraw takes them away first and places them
// again without sending; a switch to characters takes them away, and the terminal forgets them
// when the pixels go.
TRIAL("App: a redraw takes kitty pictures away and places them unsent; characters take them away") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.reconfigured(kittied());

    const std::string said = rig.drawn();

    OUGHT(holds(said, "\x1b_Ga=d,d=a,q=2\x1b\\"));
    OUGHT(said.find("\x1b_Ga=d,d=a") < said.find("\x1b[2J"));
    OUGHT(holds(said, stood_at("2;3", 5)));
    OUGHT(!holds(said, kitty_send));

    Setup none = kittied();

    none.graphics = Graphics::none;
    rig.reconfigured(none);

    const std::string plain = rig.drawn();

    OUGHT(holds(plain, "\x1b_Ga=d,d=a"));
    OUGHT(holds(plain, "▒"));
    OUGHT(!holds(plain, "a=p"));

    const std::weak_ptr<const Pixels> old = rig.root.pixels;

    rig.root.pixels = dotted(3, 2);
    rig.root.again();

    const std::string again = rig.drawn();

    OUGHT(old.expired());
    OUGHT(holds(again, taken(5, 'I')));
}

// Leaving and handing the terminal over free every kitty picture, also one the terminal only holds
// while it is covered: the other program gets a clean screen, and after coming back the picture
// goes anew. Without kitty pictures nothing is said.
TRIAL("App: leaving and handing over free all kitty pictures, and only when there are some") {
    std::shared_ptr<ScriptedConsole> line;

    {
        Rig rig{kittied()};

        line = rig.line;

        (void)rig.drawn();

        {
            line->written.clear();

            const App::Pause away{*rig.app};

            OUGHT(holds(line->written, "\x1b_Ga=d,d=A,q=2\x1b\\"));
        }

        OUGHT(holds(rig.drawn(), sent_whole(5, *rig.root.pixels, 0)));

        rig.root.covered = true;
        rig.root.again();

        OUGHT(holds(rig.drawn(), taken(5)));

        line->written.clear();
    }

    OUGHT(holds(line->written, "\x1b_Ga=d,d=A,q=2\x1b\\"));

    std::shared_ptr<ScriptedConsole> quiet = std::make_shared<ScriptedConsole>();

    {
        App app{Setup{}, Port<tal::Console>{quiet}};

        quiet->written.clear();
    }

    OUGHT(!holds(quiet->written, "\x1b_G"));
}

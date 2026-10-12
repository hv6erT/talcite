#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/base64.hpp"
#include "talcite/image.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/term.hpp"

using namespace talcite;

// Images in pixels: the cells of an image carry its mark, composition carries the mark upward, and
// `Term` gets pixels only for an image that is visible as a whole. The fake terminal shows every
// byte, so one can see when a sixel goes out and when it does not.

namespace {

// The text as shared encoded bytes of a picture.
std::shared_ptr<const std::string> encoded(std::string said) { return std::make_shared<const std::string>(std::move(said)); }

// A style with only the given background color.
Style paper(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Style made;

    made.background = Color{false, red, green, blue};

    return made;
}

// A 6×4 sheet on a blue background, with a 2×2 image at (1, 1) in it: two cells with a letter, one
// transparent (foreground only) and one empty.
View sheet(std::uint32_t mark = 7) {
    View made{{6, 4}};

    made.fill({{0, 0}, {6, 4}}, paper(0, 0, 200));

    View image{{2, 2}};

    image.clear({{0, 0}, {2, 2}});
    image.put({0, 0}, U'▀', paper(250, 0, 0));
    image.put({1, 0}, U'▄', paper(0, 250, 0));
    image.foreground({0, 1}, U'▖', Style{});
    image.picture({{0, 0}, {2, 2}}, Picture{mark, {2, 2}, encoded("SIX")});

    made.draw(image, {1, 1});

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

// The pixels of an image: each frame has different bytes, so that one can see which was sent.
std::shared_ptr<const Pixels> dotted(ui::Size size, int stills = 1) {
    std::vector<Still> made;

    for (int still = 0; still < stills; ++still) {
        Bytes one;

        for (int at = 0; at < size.wide * size.high * 4; ++at) one.push_back(static_cast<std::byte>(at + 40 * still));

        made.push_back(Still{one, std::chrono::milliseconds{100}});
    }

    return std::make_shared<const Pixels>(size, std::move(made));
}

// A root that draws a sheet with an image; the image can be moved, covered or replaced.
class Framed : public ui::Root {
public:
    ui::Spot at{2, 1};
    ui::Size size{3, 2};
    std::uint32_t mark = 5;
    bool covered = false;
    bool bare = false;
    char32_t elsewhere = U' ';
    ui::Spot other{0, 0};
    std::shared_ptr<const Pixels> pixels = dotted({3, 2}, 2);
    std::size_t still = 0;

protected:
    // Fills the background, puts the extra letter, fills the image's cells and marks them with the
    // picture, and covers one of them when `covered` is set.
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.fill({{0, 0}, given}, paper(0, 0, 90));
        made.put(other, elsewhere, paper(0, 0, 90));

        for (int y = 0; y < size.high; ++y)
            for (int x = 0; x < size.wide; ++x) made.put({at.x + x, at.y + y}, U'▒', paper(200, 100, 0));

        made.picture({at, size}, Picture{mark, size, bare ? nullptr : encoded("\x1bP0;1;0q\"1;1;30;40#0;2;100;0;0\x1b\\"),
                                         bare ? nullptr : pixels, still});

        if (covered) made.put({at.x + 1, at.y}, U'x', paper(0, 0, 90));

        return made;
    }
};

// Terminal settings with graphics of the given kind and the given cell size in pixels.
Setup pixeled(ui::Size cell = {10, 20}, Graphics kind = Graphics::sixel) {
    Setup made;

    made.graphics = kind;
    made.cell = cell;

    return made;
}

// Terminal settings with kitty graphics and a 10×20 cell.
Setup kittied() { return pixeled({10, 20}, Graphics::kitty); }

struct Rig {
    std::shared_ptr<faking::Terminal> line = faking::terminal();
    Framed root;
    std::unique_ptr<Term> term;

    // A terminal on the fake console with the given settings and window, showing `root`.
    explicit Rig(Setup given = pixeled(), ui::Size window = {12, 6}) {
        line->window = window;
        term = std::make_unique<Term>(std::move(given), Seam<Console>{line});
        term->show(root);
    }

    // Draws once and returns everything written to the console by that draw.
    std::string drawn() {
        line->written.clear();
        term->draw();

        return line->written;
    }
};

const std::string sixel_head = "\x1bP0;1;0q";

}

// --- Sheet ---------------------------------------------------------------------------------------

TRIAL("View: an image's cells carry its mark, a transparent one becomes an empty foreground") {
    View made{{3, 1}};

    made.clear({{0, 0}, {3, 1}});
    made.put({0, 0}, U'a', paper(1, 2, 3));
    made.picture({{0, 0}, {2, 1}}, Picture{9, {2, 1}, encoded("x")});

    same(made.at({0, 0}).picture, std::uint32_t{9});
    same(made.at({0, 0}).letter, char32_t{U'a'});
    same(made.at({1, 0}).picture, std::uint32_t{9});
    same(static_cast<int>(made.at({1, 0}).shown), static_cast<int>(Shown::foreground));
    same(made.at({1, 0}).letter, char32_t{U' '});
    same(made.at({2, 0}).picture, std::uint32_t{0});
    same(static_cast<int>(made.at({2, 0}).shown), static_cast<int>(Shown::none));
    same(made.pictures().size(), std::size_t{1});
}

TRIAL("View: the same mark a second time replaces the entry instead of adding another") {
    View made{{2, 1}};

    made.picture({{0, 0}, {2, 1}}, Picture{3, {2, 1}, encoded("a")});
    made.picture({{0, 0}, {2, 1}}, Picture{3, {2, 1}, encoded("b")});

    same(made.pictures().size(), std::size_t{1});
    same(*made.pictures().front().sixel, std::string{"b"});
}

TRIAL("View: an image travels up with its cells and stands where it was pasted") {
    View made = sheet();

    same(made.pictures().size(), std::size_t{1});
    same(made.at({1, 1}).picture, std::uint32_t{7});
    same(made.at({2, 2}).picture, std::uint32_t{7});
    same(made.at({0, 0}).picture, std::uint32_t{0});

    // The transparent cell took the background from below when pasted.
    ought(made.at({1, 2}).style.background == paper(0, 0, 200).background);

    const std::vector<Placement> found = made.bared({{0, 0}, {6, 4}});

    same(found.size(), std::size_t{1});
    ought(found.front().at == (ui::Spot{1, 1}));
    same(*found.front().picture.sixel, std::string{"SIX"});
}

TRIAL("View: an image visible as a whole has empty cells with their background and its mark") {
    View made = sheet();

    (void)made.bared({{0, 0}, {6, 4}});

    same(made.at({1, 1}), (Cell{U' ', paper(250, 0, 0), Shown::own, 7}));
    same(made.at({2, 1}), (Cell{U' ', paper(0, 250, 0), Shown::own, 7}));
    same(made.at({1, 2}), (Cell{U' ', paper(0, 0, 200), Shown::own, 7}));
    same(made.at({2, 2}), (Cell{U' ', paper(0, 0, 200), Shown::own, 7}));

    // An empty image cell and an ordinary empty cell with the same background are not equal.
    ought(!(made.at({2, 2}) == made.at({0, 0})));
}

TRIAL("View: an image covered in even one cell stays in characters and loses its mark") {
    View made = sheet();
    View dialog{{1, 1}};

    dialog.put({0, 0}, U'x', paper(9, 9, 9));
    made.draw(dialog, {2, 2});

    same(made.bared({{0, 0}, {6, 4}}).size(), std::size_t{0});
    same(made.at({1, 1}).letter, char32_t{U'▀'});
    same(made.at({1, 1}).picture, std::uint32_t{0});
    same(made.at({2, 1}).picture, std::uint32_t{0});
}

TRIAL("View: a shadow over an image covers it, since pixels cannot be dimmed") {
    View made = sheet();
    View shadow{{1, 1}};

    shadow.shade({{0, 0}, {1, 1}}, paper(10, 10, 10));
    made.draw(shadow, {1, 1});

    same(made.at({1, 1}).picture, std::uint32_t{0});
    same(made.bared({{0, 0}, {6, 4}}).size(), std::size_t{0});
}

TRIAL("View: an image clipped by the paste edge or sticking out of the field gets no pixels") {
    // Clipped: only the left column of the image made it into the sheet.
    View narrow{{2, 4}};

    narrow.draw(sheet(), {0, 0});

    same(narrow.pictures().size(), std::size_t{1});
    same(narrow.bared({{0, 0}, {2, 4}}).size(), std::size_t{0});
    same(narrow.at({1, 1}).picture, std::uint32_t{0});

    // Whole, but the field ends one row earlier.
    View whole = sheet();

    same(whole.bared({{0, 0}, {6, 2}}).size(), std::size_t{0});

    View fits = sheet();

    same(fits.bared({{0, 0}, {6, 3}}).size(), std::size_t{1});

    // The field ends one column earlier, and the field starts one column later.
    View right = sheet();

    same(right.bared({{0, 0}, {2, 4}}).size(), std::size_t{0});

    View left = sheet();

    same(left.bared({{2, 0}, {4, 4}}).size(), std::size_t{0});
}

TRIAL("View: an image without a visible cell and an image without a mark give nothing") {
    View away{{4, 4}};

    away.picture({{10, 10}, {2, 2}}, Picture{4, {2, 2}, encoded("a")});

    same(away.bared({{0, 0}, {4, 4}}).size(), std::size_t{0});

    // Zero means "no image"; every ordinary cell carries it, so a sheet that happens to be the size
    // of the image would look like an image as a whole.
    View plain{{2, 2}};

    plain.picture({{0, 0}, {2, 2}}, Picture{0, {2, 2}, encoded("b")});

    same(plain.bared({{0, 0}, {2, 2}}).size(), std::size_t{0});
}

TRIAL("View: the same image pasted twice is listed once") {
    View made{{8, 4}};

    made.draw(sheet(), {0, 0});
    made.draw(sheet(), {0, 0});

    same(made.pictures().size(), std::size_t{1});
    same(made.bared({{0, 0}, {8, 4}}).size(), std::size_t{1});
}

TRIAL("View: cells in a shape other than the image's are not an image as a whole") {
    View made{{3, 3}};

    made.picture({{0, 0}, {3, 1}}, Picture{8, {1, 3}, encoded("a")});

    same(made.bared({{0, 0}, {3, 3}}).size(), std::size_t{0});
}

TRIAL("View: two images side by side, both whole, each in its place") {
    View made{{8, 3}};

    made.draw(sheet(1), {0, 0});

    View second{{2, 1}};

    second.put({0, 0}, U'a', paper(1, 1, 1));
    second.put({1, 0}, U'b', paper(1, 1, 1));
    second.picture({{0, 0}, {2, 1}}, Picture{2, {2, 1}, encoded("two")});
    made.draw(second, {5, 0});

    const std::vector<Placement> found = made.bared({{0, 0}, {8, 3}});

    same(found.size(), std::size_t{2});
    same(found[0].picture.mark, std::uint32_t{1});
    ought(found[0].at == (ui::Spot{1, 1}));
    same(found[1].picture.mark, std::uint32_t{2});
    ought(found[1].at == (ui::Spot{5, 0}));
}

// --- Terminal ------------------------------------------------------------------------------------

TRIAL("Term: an image visible as a whole goes as a sixel, after the cells, in its place") {
    Rig rig;

    const std::string said = rig.drawn();
    const std::size_t at = said.find(sixel_head);

    ought(at != std::string::npos);
    ought(said.compare(at - 6, 6, "\x1b[2;3H") == 0);

    // The image's cells went out empty, with the mark's background, and before the image.
    ought(said.find("\x1b[0;48;5;") < at);
    same(said.find(std::string{"▒"}), std::string::npos);
    same(counted(said, sixel_head), std::size_t{1});
}

TRIAL("Term: an unchanged image does not go a second time, and a change beside it does not send it") {
    Rig rig;

    (void)rig.drawn();

    same(rig.drawn(), std::string{});

    rig.root.elsewhere = U'k';

    const std::string said = rig.drawn();

    ought(holds(said, "k"));
    ought(!holds(said, sixel_head));
}

TRIAL("Term: a covered image goes back to characters, an uncovered one gets its pixels anew") {
    Rig rig;

    (void)rig.drawn();

    rig.root.covered = true;

    const std::string hidden = rig.drawn();

    ought(!holds(hidden, sixel_head));
    ought(holds(hidden, "x"));

    // All the image's cells rewritten with characters; otherwise the pixels would stay under them.
    same(counted(hidden, std::string{"▒"}), std::size_t{5});

    rig.root.covered = false;

    const std::string back = rig.drawn();

    same(counted(back, sixel_head), std::size_t{1});
    ought(!holds(back, std::string{"▒"}));
}

TRIAL("Term: another mark in the same place, another animation frame, goes anew") {
    Rig rig;

    (void)rig.drawn();

    rig.root.mark = 6;

    same(counted(rig.drawn(), sixel_head), std::size_t{1});
}

TRIAL("Term: a moved image goes to the new place, the old cells go back to the background") {
    Rig rig;

    (void)rig.drawn();

    rig.root.at = {5, 2};

    const std::string said = rig.drawn();

    ought(holds(said, "\x1b[3;6H" + sixel_head));
    ought(holds(said, "\x1b[2;3H"));
}

// After an image the cursor stands where the terminal put it: the cell right after the image gets
// its own jump, even though before the image the cursor stood right at it.
TRIAL("Term: after an image the next character goes with its own jump") {
    Rig rig;

    (void)rig.drawn();

    rig.root.mark = 9;

    const std::string again = rig.drawn();

    ought(holds(again, sixel_head));

    rig.root.elsewhere = U'k';
    rig.root.other = {5, 2};

    ought(holds(rig.drawn(), "\x1b[3;6H"));
}

TRIAL("Term: an image without bytes has empty cells and sends nothing") {
    Rig rig;

    rig.root.bare = true;

    const std::string said = rig.drawn();

    ought(!holds(said, sixel_head));
    ought(!holds(said, std::string{"▒"}));
}

// An image without bytes, before another in the list, does not stop the ones after it.
TRIAL("Term: an image without bytes does not stop the image after it") {
    class Two : public ui::Root {
    protected:
        // A background with an image without bytes, followed by an image with a sixel.
        View paint(ui::Size given, ui::Area seen) override {
            (void)seen;

            View made{given};

            made.fill({{0, 0}, given}, paper(0, 0, 90));
            made.picture({{0, 0}, {2, 1}}, Picture{1, {2, 1}, nullptr});
            made.picture({{4, 1}, {2, 1}}, Picture{2, {2, 1}, encoded("\x1bP0;1;0qSECOND\x1b\\")});

            return made;
        }
    };

    auto line = faking::terminal();

    line->window = ui::Size{10, 4};

    Two root;
    Term term{pixeled(), Seam<Console>{line}};

    term.show(root);
    line->written.clear();
    term.draw();

    ought(holds(line->written, "\x1b[2;5H\x1bP0;1;0qSECOND"));
}

TRIAL("Term: an image reaching the last row stays in characters") {
    Rig rig{pixeled(), {12, 3}};

    const std::string said = rig.drawn();

    ought(!holds(said, sixel_head));
    ought(holds(said, std::string{"▒"}));
}

TRIAL("Term: without sixel, without a cell size, or with a wish for no graphics, characters only") {
    Setup plain;

    plain.cell = ui::Size{10, 20};

    Rig none{plain};

    ought(!holds(none.drawn(), sixel_head));

    Setup blind;

    blind.graphics = Graphics::sixel;

    Rig unknown{blind};

    ought(!holds(unknown.drawn(), sixel_head));

    Setup kitty = pixeled();

    kitty.graphics = Graphics::kitty;

    Rig other{kitty};

    ought(!holds(other.drawn(), sixel_head));
}

TRIAL("Term: the root gets its graphics from the terminal, also after a configuration change") {
    Rig rig;

    same(static_cast<int>(rig.root.graphics()), static_cast<int>(Graphics::sixel));

    Setup none = pixeled();

    none.graphics = Graphics::none;

    rig.term->reconfigure(none);
    (void)rig.term->loop().turn();

    same(static_cast<int>(rig.root.graphics()), static_cast<int>(Graphics::none));

    // After a configuration change the screen is redrawn, now without the image.
    const std::string said = rig.drawn();

    ought(!holds(said, sixel_head));
    ought(holds(said, std::string{"▒"}));
}

TRIAL("Term: refreshing the whole screen sends the image again") {
    Rig rig;

    (void)rig.drawn();

    rig.term->reconfigure(pixeled());
    (void)rig.term->loop().turn();

    same(counted(rig.drawn(), sixel_head), std::size_t{1});
}

// --- Widget --------------------------------------------------------------------------------------

namespace {

// An opaque image of the given size in a single color with the given red.
Pixels square(ui::Size size, std::uint8_t red) {
    Bytes made;

    for (int at = 0; at < size.wide * size.high; ++at)
        for (const std::uint8_t one : {red, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{255}}) made.push_back(std::byte{one});

    return Pixels{size, {Still{made, std::chrono::milliseconds{0}}}};
}

// A root like a terminal with sixel: a 10×20 cell, sextants. The image gets a 20×5 field, so a row
// is left below it; the last row of the screen does not take pixels anyway.
class Sheet : public ui::Root {
public:
    std::shared_ptr<ui::Image> image = std::make_shared<ui::Image>();

    // A root with the given graphics and cell size, sextants, full color and Unicode, holding the
    // image.
    explicit Sheet(Graphics given = Graphics::sixel, ui::Size grid = {10, 20}) {
        cell(grid);
        blocks(ui::Blocks::sextants);
        colors(Colors::rgb);
        unicode(true);
        graphics(given);
        children().add(image);
    }

protected:
    // Paints a black background and the image in a 20×5 field at the top.
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.fill({{0, 0}, given}, paper(0, 0, 0));
        made.draw(image->render({{0, 0}, {20, 5}}), {0, 0});

        return made;
    }
};

// Renders and turns the loop until the image is no longer busy; false when it did not settle in
// 500 tries.
bool settled(Loop& loop, ui::Root& top, const ui::Image& one) {
    for (int tries = 0; tries < 500; ++tries) {
        (void)top.render({{0, 0}, {20, 6}});
        (void)loop.turn();

        if (!one.busy()) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return false;
}

// The raster attributes of a sixel, from the quote up to the first color.
std::string raster(const std::string& said) {
    const std::size_t at = said.find('"');

    if (at == std::string::npos) return {};

    return said.substr(at, said.find('#', at) - at);
}

}

TRIAL("Image: with sixel it orders the field's pixels and marks the cells with its image") {
    Loop loop;
    Sheet top;

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{1});

    const Picture& one = made.pictures().front();

    // A 20×5 field of 10×20-pixel cells: the square has five rows and ten columns.
    ought(one.size == (ui::Size{10, 5}));
    same(raster(*one.sixel), std::string{"\"1;1;100;96"});
    same(made.at({5, 0}).picture, one.mark);
    same(made.at({14, 4}).picture, one.mark);
    same(made.at({4, 0}).picture, std::uint32_t{0});
    same(made.at({15, 0}).picture, std::uint32_t{0});

    // The image's characters are computed from the pixels: red, not emptiness.
    ought(made.at({5, 0}).style.foreground.red > 150 || made.at({5, 0}).style.background.red > 150);
}

TRIAL("Image: without sixel the image is in characters and leaves no image mark") {
    Loop loop;
    Sheet top{Graphics::none};

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{0});
    same(made.at({5, 0}).picture, std::uint32_t{0});
    ought(made.at({5, 0}).shown != Shown::none);
}

TRIAL("Image: an unknown cell size means characters, with sixel and with kitty") {
    for (const auto& [given, grid] : std::vector<std::pair<Graphics, ui::Size>>{
             {Graphics::kitty, {0, 20}}, {Graphics::sixel, {0, 20}}, {Graphics::sixel, {10, 0}}}) {
        Loop loop;
        Sheet top{given, grid};

        top.image->hold(square({40, 40}, 200));

        ought(settled(loop, top, *top.image));

        const View made = top.render({{0, 0}, {20, 6}});

        same(made.pictures().size(), std::size_t{0});

        // The image is there, in characters.
        bool red = false;

        for (int x = 0; x < 20; ++x) red = red || made.at({x, 2}).style.foreground.red > 150;

        ought(red);
    }
}

// The image's characters next to pixels are computed from the field's pixels, reduced to the grid,
// not from their corner.
TRIAL("Image: the fallback characters are the whole image, reduced") {
    Loop loop;
    Sheet top;

    Bytes halves;

    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            for (const std::uint8_t one : {static_cast<std::uint8_t>(x < 20 ? 250 : 0), std::uint8_t{0}, static_cast<std::uint8_t>(x < 20 ? 0 : 250), std::uint8_t{255}})
                halves.push_back(std::byte{one});

    top.image->hold(Pixels{{40, 40}, {Still{halves, std::chrono::milliseconds{0}}}});

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});
    const Cell left = made.at({5, 2});
    const Cell right = made.at({14, 2});

    ought(left.style.foreground.red > 200 && left.style.foreground.blue < 50);
    ought(right.style.foreground.blue > 200 && right.style.foreground.red < 50);
}

// An encoding result for an image that is gone does not get in; it is recognized by its sequence
// number.
TRIAL("Image: a late sixel of the previous image is dropped") {
    Loop loop;
    Sheet top;

    top.image->hold(square({40, 40}, 255));
    (void)top.render({{0, 0}, {20, 6}});

    top.image->hold(square({40, 40}, 51));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{1});
    ought(holds(*made.pictures().front().sixel, "#0;2;20;0;0"));
}

TRIAL("Image: switching to sixel while running recomputes the image, switching back removes it") {
    Loop loop;
    Sheet top{Graphics::none};

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    top.graphics(Graphics::sixel);

    ought(settled(loop, top, *top.image));
    same(top.render({{0, 0}, {20, 6}}).pictures().size(), std::size_t{1});

    top.graphics(Graphics::none);

    ought(settled(loop, top, *top.image));
    same(top.render({{0, 0}, {20, 6}}).pictures().size(), std::size_t{0});
}

TRIAL("Image: a new image gets a new mark") {
    Loop loop;
    Sheet top;

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));

    const std::uint32_t first = top.render({{0, 0}, {20, 6}}).pictures().front().mark;

    top.image->hold(square({40, 40}, 90));

    ought(settled(loop, top, *top.image));

    const View made = top.render({{0, 0}, {20, 6}});

    same(made.pictures().size(), std::size_t{1});
    ought(made.pictures().front().mark != first);
}

TRIAL("Image: an image from a file with sixel also orders the field's pixels") {
    Loop loop;
    Sheet top;

    std::vector<ui::Size> asked;

    Figure figure;

    figure.size = {40, 40};
    figure.fetch = [&asked](ui::Size want) {
        asked.push_back(want);

        return ready(square(want, 200));
    };

    top.image->hold(std::move(figure));

    ought(settled(loop, top, *top.image));

    ought(!asked.empty());
    ought(asked.back() == (ui::Size{100, 100}));
    same(top.render({{0, 0}, {20, 6}}).pictures().size(), std::size_t{1});
}

TRIAL("Image: through the terminal, characters, and once the sixel is ready, the image") {
    auto line = faking::terminal();

    line->window = ui::Size{20, 6};

    Sheet top{Graphics::none};
    Term term{pixeled(), Seam<Console>{line}};

    term.show(top);
    top.image->hold(square({40, 40}, 200));

    line->written.clear();
    term.draw();

    ought(!holds(line->written, sixel_head));

    for (int tries = 0; tries < 500 && top.image->busy(); ++tries) {
        (void)term.loop().turn();

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    line->written.clear();
    term.draw();

    ought(holds(line->written, "\x1b[1;6H" + sixel_head + "\"1;1;100;96"));
}

// --- Kitty ---------------------------------------------------------------------------------------

namespace {

const std::string kitty_send = "\x1b_Ga=t,";

// The kitty command that transmits frame `still` of the pixels in one piece under id `mark`.
std::string sent_whole(std::uint32_t mark, const Pixels& pixels, std::size_t still) {
    return "\x1b_Ga=t,f=32,s=" + std::to_string(pixels.size().wide) + ",v=" + std::to_string(pixels.size().high) +
           ",i=" + std::to_string(mark) + ",q=2,m=0;" + base64(pixels.stills()[still].bytes) + "\x1b\\";
}

// The cursor jump to `spot` followed by the kitty command that places image `mark` in 3×2 cells.
std::string stood_at(std::string spot, std::uint32_t mark) {
    return "\x1b[" + spot + "H\x1b_Ga=p,i=" + std::to_string(mark) + ",c=3,r=2,C=1,q=2\x1b\\";
}

// The kitty command that deletes image `mark`: placements only with `i`, the data too with `I`.
std::string taken(std::uint32_t mark, char how = 'i') {
    return "\x1b_Ga=d,d=" + std::string(1, how) + ",i=" + std::to_string(mark) + ",q=2\x1b\\";
}

}

TRIAL("Term: kitty sends the image once and places it in its cells") {
    Rig rig{kittied()};

    const std::string said = rig.drawn();
    const std::size_t sending = said.find(sent_whole(5, *rig.root.pixels, 0));
    const std::size_t standing = said.find(stood_at("2;3", 5));

    ought(sending != std::string::npos);
    ought(standing != std::string::npos);
    ought(sending < standing);
    ought(!holds(said, sixel_head));
    ought(!holds(said, std::string{"▒"}));

    same(rig.drawn(), std::string{});
}

TRIAL("Term: a covered kitty image is removed before the characters, an uncovered one comes back without sending") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.covered = true;

    const std::string hidden = rig.drawn();

    ought(holds(hidden, taken(5)));
    ought(hidden.find(taken(5)) < hidden.find("▒"));
    same(counted(hidden, std::string{"▒"}), std::size_t{5});
    ought(!holds(hidden, kitty_send));
    ought(!holds(hidden, "a=p"));

    rig.root.covered = false;

    const std::string back = rig.drawn();

    ought(holds(back, stood_at("2;3", 5)));
    ought(!holds(back, kitty_send));
    ought(!holds(back, "a=d"));
}

TRIAL("Term: a moved kitty image is removed and placed in the new place, without sending") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.at = {5, 2};

    const std::string said = rig.drawn();

    ought(holds(said, taken(5)));
    ought(holds(said, stood_at("3;6", 5)));
    ought(!holds(said, kitty_send));
}

TRIAL("Term: kitty animation frames are sent once, then only switched") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.mark = 6;
    rig.root.still = 1;

    const std::string second = rig.drawn();

    ought(holds(second, taken(5)));
    ought(holds(second, sent_whole(6, *rig.root.pixels, 1)));
    ought(holds(second, stood_at("2;3", 6)));
    ought(!holds(second, taken(5, 'I')));

    rig.root.mark = 5;
    rig.root.still = 0;

    const std::string again = rig.drawn();

    ought(holds(again, taken(6)));
    ought(holds(again, stood_at("2;3", 5)));
    ought(!holds(again, kitty_send));
}

TRIAL("Term: a kitty image whose pixels nobody holds is forgotten by the terminal at once") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.root.pixels = dotted({3, 2});
    rig.root.mark = 8;

    const std::string said = rig.drawn();

    ought(holds(said, taken(5)));
    ought(holds(said, taken(5, 'I')));
    ought(holds(said, sent_whole(8, *rig.root.pixels, 0)));

    // A covered one is not forgotten: someone still holds its pixels.
    rig.root.covered = true;

    ought(!holds(rig.drawn(), taken(8, 'I')));
}

TRIAL("Term: a large kitty image goes in chunks of 4096 characters, m=0 only at the end") {
    Rig rig{kittied()};

    rig.root.pixels = dotted({40, 40});

    const std::string said = rig.drawn();
    const std::string coded = base64(rig.root.pixels->stills()[0].bytes);

    same(coded.size(), std::size_t{8536});

    const std::string wanted = "\x1b_Ga=t,f=32,s=40,v=40,i=5,q=2,m=1;" + coded.substr(0, 4096) + "\x1b\\" +
                               "\x1b_Gq=2,m=1;" + coded.substr(4096, 4096) + "\x1b\\" + "\x1b_Gq=2,m=0;" +
                               coded.substr(8192) + "\x1b\\";

    ought(holds(said, wanted));
    same(counted(said, "m=0;"), std::size_t{1});
}

TRIAL("Term: base64 that splits evenly into chunks gets no empty chunk at the end") {
    Rig rig{kittied()};

    rig.root.pixels = dotted({48, 32});

    const std::string said = rig.drawn();
    const std::string coded = base64(rig.root.pixels->stills()[0].bytes);

    same(coded.size(), std::size_t{8192});
    ought(holds(said, "\x1b_Gq=2,m=0;" + coded.substr(4096) + "\x1b\\"));
    same(counted(said, "\x1b_G"), std::size_t{3});
}

TRIAL("Term: a frame beyond the image's pixels does not go to kitty") {
    Rig rig{kittied()};

    rig.root.still = 5;

    const std::string said = rig.drawn();

    ought(!holds(said, "\x1b_G"));
    ought(!holds(said, std::string{"▒"}));
}

TRIAL("Term: kitty places an image in the last row too") {
    Rig rig{kittied(), {12, 3}};

    ought(holds(rig.drawn(), stood_at("2;3", 5)));
}

TRIAL("Term: refreshing the screen removes kitty images and places them without sending") {
    Rig rig{kittied()};

    (void)rig.drawn();

    rig.term->reconfigure(kittied());
    (void)rig.term->loop().turn();

    const std::string said = rig.drawn();

    ought(holds(said, "\x1b_Ga=d,d=a,q=2\x1b\\"));
    ought(said.find("\x1b_Ga=d,d=a") < said.find("\x1b[2J"));
    ought(holds(said, stood_at("2;3", 5)));
    ought(!holds(said, kitty_send));
}

TRIAL("Term: switching from kitty to characters removes the image, and the terminal forgets it when the pixels go") {
    Rig rig{kittied()};

    (void)rig.drawn();

    Setup none = kittied();

    none.graphics = Graphics::none;

    rig.term->reconfigure(none);
    (void)rig.term->loop().turn();

    const std::string said = rig.drawn();

    ought(holds(said, "\x1b_Ga=d,d=a"));
    ought(holds(said, std::string{"▒"}));
    ought(!holds(said, "a=p"));

    rig.root.pixels = dotted({3, 2});

    ought(holds(rig.drawn(), taken(5, 'I')));
}

TRIAL("Term: leaving and handing over the terminal free all kitty images") {
    auto line = faking::terminal();

    line->window = ui::Size{12, 6};

    Framed root;

    {
        Term term{kittied(), Seam<Console>{line}};

        term.show(root);
        term.draw();

        {
            line->written.clear();

            const Term::Pause away{term};

            ought(holds(line->written, "\x1b_Ga=d,d=A,q=2\x1b\\"));
        }

        line->written.clear();
        term.draw();

        // After coming back the image goes anew: the terminal forgot it.
        ought(holds(line->written, sent_whole(5, *root.pixels, 0)));

        line->written.clear();
    }

    ought(holds(line->written, "\x1b_Ga=d,d=A,q=2\x1b\\"));
}

TRIAL("Term: without kitty images, leaving sends nothing about graphics") {
    auto line = faking::terminal();

    {
        Term term{Setup{}, Seam<Console>{line}};

        line->written.clear();
    }

    ought(!holds(line->written, "\x1b_G"));
}

TRIAL("Image: with kitty the images are ready at once, with the field's pixels, one per frame") {
    Loop loop;
    Sheet top{Graphics::kitty};

    Bytes red;

    for (int at = 0; at < 40 * 40; ++at)
        for (const std::uint8_t one : {std::uint8_t{200}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{255}})
            red.push_back(std::byte{one});

    top.image->hold(Pixels{{40, 40}, {Still{red, std::chrono::milliseconds{100}}, Still{red, std::chrono::milliseconds{100}}}});

    const View made = top.render({{0, 0}, {20, 6}});

    ought(!top.image->busy());
    same(made.pictures().size(), std::size_t{1});

    const Picture& one = made.pictures().front();

    ought(one.pixels != nullptr);
    ought(one.sixel == nullptr);
    ought(one.pixels->size() == (ui::Size{100, 100}));
    same(one.pixels->stills().size(), std::size_t{2});
    ought(one.size == (ui::Size{10, 5}));

    // The second frame is another mark on the same pixels.
    top.image->play(false);
    top.image->still(1);

    const View later = top.render({{0, 0}, {20, 6}});
    const Picture& two = later.pictures().front();

    ought(two.mark != one.mark);
    ought(two.pixels == one.pixels);
    same(two.still, std::size_t{1});

    (void)loop;
}

TRIAL("Image: switching sixel ↔ kitty while running recomputes the image") {
    Loop loop;
    Sheet top{Graphics::sixel};

    top.image->hold(square({40, 40}, 200));

    ought(settled(loop, top, *top.image));
    ought(top.render({{0, 0}, {20, 6}}).pictures().front().sixel != nullptr);

    top.graphics(Graphics::kitty);

    const View kitty = top.render({{0, 0}, {20, 6}});

    same(kitty.pictures().size(), std::size_t{1});
    ought(kitty.pictures().front().pixels != nullptr);

    top.graphics(Graphics::sixel);

    ought(settled(loop, top, *top.image));
    ought(top.render({{0, 0}, {20, 6}}).pictures().front().sixel != nullptr);
}

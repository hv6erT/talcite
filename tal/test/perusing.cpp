#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "tal/docs.hpp"
#include "tal/document.hpp"
#include "tal/executor.hpp"
#include "tal/leaves.hpp"
#include "tal/pixels.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// The pages of a document in the tree (`Leaves`) on a scene with a loop, over a document of the
// trials whose every pixel tells where on its page it lies: its red is the page's number, its
// green and blue the column and row of the page's pixels at the scale asked, a quarter each. So a
// picture on the screen says which page it shows and which part of it, whatever tiles it was
// composed from. The document counts what it is asked, and can hold its answers back, one by one,
// or fail a page.

namespace {

using namespace std::chrono_literals;

using tal::Blocks;
using tal::Display;
using tal::Document;
using tal::Graphics;
using tal::Leaves;
using tal::Size;
using tal::Task;
using tal::docs::Page;
using tal::docs::Region;
using tal::testing::Scene;

// What the document was asked: a page, at a size, of a region.
struct Asked {
    std::size_t page = 0;
    tal::media::Size size;
    Region region;
};

// A document of `count` pages of 600 by 800 points.
class Booklet final : public tal::docs::Document {
public:
    explicit Booklet(std::size_t count) : pages_(count, Page{600, 800}) {}

    // A document of the pages `given`.
    explicit Booklet(std::vector<Page> given) : pages_(std::move(given)) {}

    const std::vector<Page>& pages() const override { return pages_; }

    // The picture of the region asked, telling in every pixel where it lies; held back while
    // `holding`, until its turn in `waiting` is settled; a trouble for a page in `failing`.
    Task<tal::media::Pixels> rendered(std::size_t page, tal::media::Size size, Region region) override {
        Asked one;

        one.page = page;
        one.size = size;
        one.region = region;
        asked.push_back(one);

        if (holding) {
            tal::Pending<void> mine;

            waiting.push_back(mine);
            co_await mine.task();
        }

        if (failing.contains(page)) throw tal::Trouble{tal::Kind::broken, "The engine died."};

        if (throwing.contains(page)) throw std::runtime_error{"The disk is gone."};

        co_return painted(page, size, region);
    }

    std::vector<Asked> asked;
    bool holding = false;
    std::vector<tal::Pending<void>> waiting;
    std::set<std::size_t> failing;
    std::set<std::size_t> throwing;

private:
    // The pixels of `region` of page `page` at `size`: red the page, green and blue a quarter of
    // the column and the row of the whole page at that scale.
    static tal::media::Pixels painted(std::size_t page, tal::media::Size size, Region region) {
        const auto left = static_cast<std::uint32_t>(std::lround(region.left * size.width / region.width));
        const auto top = static_cast<std::uint32_t>(std::lround(region.top * size.height / region.height));
        tal::Bytes rgba;

        rgba.reserve(std::size_t{size.width} * size.height * 4);

        for (std::uint32_t y = 0; y < size.height; ++y)
            for (std::uint32_t x = 0; x < size.width; ++x)
                for (const std::uint32_t band : {static_cast<std::uint32_t>(page), (left + x) / 4, (top + y) / 4, 255U})
                    rgba.push_back(static_cast<std::byte>(band & 0xFF));

        std::vector<tal::media::Still> stills(1);

        stills.front().rgba = std::move(rgba);

        return tal::media::Pixels{size, std::move(stills)};
    }

    std::vector<Page> pages_;
};

// A terminal of 40 by 12 cells drawing sextants, of cells of unknown size.
Display characters() { return Display{.screen = {40, 12}, .blocks = Blocks::sextants}; }

// A terminal of 120 by 40 cells of 10 by 20 pixels that takes kitty pictures.
Display kitty() { return Display{.screen = {120, 40}, .graphics = Graphics::kitty, .blocks = Blocks::sextants, .cell = {10, 20}}; }

// A scene and a loop with a document holding only the pages of a booklet of `count` pages, focused.
// The first loop made is the program's, on which the work of the pages comes back, so a trial has
// one of these at a time.
struct Shown {
    Scene scene;
    tal::Loop loop{scene.time()};
    std::shared_ptr<Booklet> booklet;
    Document document{scene.time()};
    Leaves& leaves;
    std::vector<std::size_t> turned;
    tal::Hook told;

    // On a terminal as `given` describes it.
    explicit Shown(Display given = characters(), std::size_t count = 500) : Shown(given, std::make_shared<Booklet>(count)) {}

    // On a terminal as `given` describes it, over the document `pages`.
    Shown(Display given, std::shared_ptr<Booklet> pages)
        : scene(given), booklet(std::move(pages)), leaves(document.children().add<Leaves>(booklet)) {
        told = leaves.on<tal::Turned>([this](tal::Turned& one) { turned.push_back(one.page); });
    }

    // Lets the renders asked come back before the loop they come back to goes.
    ~Shown() {
        booklet->holding = false;

        for (tal::Pending<void>& one : booklet->waiting)
            if (!one.settled()) one.settle();

        UNTIL(loop, !leaves.busy());
    }

    Shown(const Shown&) = delete;
    Shown& operator=(const Shown&) = delete;

    // Shows the document with the pages focused and lets everything asked come back and be drawn.
    void settled() {
        scene.show(document);

        if (!leaves.focused()) leaves.focus();

        for (int round = 0; round < 50; ++round) {
            scene.wait(0ms);
            UNTIL(loop, !leaves.busy() || booklet->holding);
            scene.wait(0ms);

            if (!leaves.busy()) break;
        }
    }

    // The pages the document was asked for.
    std::set<std::size_t> pages() const {
        std::set<std::size_t> made;

        for (const Asked& one : booklet->asked) made.insert(one.page);

        return made;
    }

    // The picture at cell `at`, or nothing.
    const tal::media::Pixels* picture(tal::Spot at) const {
        const std::uint32_t mark = scene.at(at).picture;
        const tal::Picture* shown = mark == 0 ? nullptr : scene.screen().picture(mark);

        return shown == nullptr || shown->pixels == nullptr ? nullptr : shown->pixels.get();
    }

    // Whether row `y` shows something other than blanks.
    bool drawn(int y) const {
        for (int x = 0; x < scene.screen().size().wide; ++x)
            if (scene.at({x, y}).cluster != " ") return true;

        return false;
    }
};

// The bands of the pixel at `x`, `y` of `pixels`.
std::vector<int> pixel(const tal::media::Pixels& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * pixels.size().width + x) * 4;
    std::vector<int> made;

    for (std::size_t band = 0; band < 4; ++band) made.push_back(std::to_integer<int>(pixels.stills().front().rgba[at + band]));

    return made;
}

}

// Continuous pages as wide as the view: in characters a page of 600 by 800 points on 40 columns
// takes 27 rows (cells of 1 to 2, the grain of sextants), with a row between pages. Only what the
// view shows and the view after it is asked for; the end of a long document asks for its last
// page, and the rows show it.
TRIAL("Leaves: continuous pages as wide as the view, only what it shows asked for") {
    Shown shown;

    shown.settled();

    SAME(shown.leaves.extent(), std::size_t{500 * 28 - 1});
    SAME(shown.leaves.window(), std::size_t{12});
    OUGHT(shown.pages() == (std::set<std::size_t>{0}));
    OUGHT(shown.drawn(0));
    OUGHT(shown.drawn(11));

    shown.scene.press(tal::Key{tal::Code::end});
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{500 * 28 - 1 - 12});
    SAME(shown.leaves.page(), std::size_t{499});
    OUGHT(shown.pages() == (std::set<std::size_t>{0, 499}));
    OUGHT(shown.turned == (std::vector<std::size_t>{499}));
    OUGHT(shown.drawn(11));
}

// In pixels the part of a page in the view is one picture, composed from the tiles of 512 pixels
// that cover it (six for a view of 1200 by 800 pixels): every pixel of it is the pixel of the page
// at its place. The next view's tiles come ahead (six more), so scrolling by a row composes the
// picture anew from tiles kept, without asking for more. A wider terminal makes the page wider,
// and the picture is of the page at its new scale; back to the old width, the page is made anew
// of the tiles kept for it.
TRIAL("Leaves: in pixels, the part in the view composed from tiles, pixel for pixel") {
    Shown shown{kitty()};

    shown.settled();

    const tal::media::Pixels* first = shown.picture({0, 0});

    OUGHT(first != nullptr);
    OUGHT(first->size() == (tal::media::Size{1200, 800}));
    OUGHT(pixel(*first, 0, 0) == (std::vector<int>{0, 0, 0, 255}));
    OUGHT(pixel(*first, 1199, 799) == (std::vector<int>{0, (1199 / 4) % 256, (799 / 4) % 256, 255}));
    OUGHT(pixel(*first, 600, 520) == (std::vector<int>{0, 150, 130, 255}));

    const std::size_t asked = shown.booklet->asked.size();

    SAME(asked, std::size_t{12});

    shown.scene.press(tal::Key{tal::Code::down});
    shown.settled();

    const tal::media::Pixels* second = shown.picture({0, 0});

    OUGHT(second != nullptr);
    OUGHT(pixel(*second, 0, 0) == (std::vector<int>{0, 0, 20 / 4, 255}));
    SAME(shown.booklet->asked.size(), asked);

    shown.scene.resize({130, 40});
    shown.settled();

    const tal::media::Pixels* wider = shown.picture({0, 0});

    OUGHT(wider != nullptr);
    OUGHT(wider->size() == (tal::media::Size{1300, 800}));
    OUGHT(pixel(*wider, 1299, 0) == (std::vector<int>{0, (1299 / 4) % 256, 20 / 4, 255}));

    const std::size_t before = shown.booklet->asked.size();

    shown.scene.resize({120, 40});
    shown.settled();

    const tal::media::Pixels* back = shown.picture({0, 0});

    OUGHT(back != nullptr);
    OUGHT(back->size() == (tal::media::Size{1200, 800}));
    OUGHT(pixel(*back, 1199, 0) == (std::vector<int>{0, (1199 / 4) % 256, 20 / 4, 255}));
    SAME(shown.booklet->asked.size(), before);
}

// At most two tiles are asked for at a time; leaving for the end of the document asks for its
// tiles next, not for the rest of those it left.
TRIAL("Leaves: two tiles at a time, those left behind not asked for") {
    Shown shown{kitty()};

    shown.booklet->holding = true;
    shown.settled();
    UNTIL(shown.loop, shown.booklet->asked.size() >= 2);
    shown.scene.wait(0ms);

    SAME(shown.booklet->asked.size(), std::size_t{2});

    shown.scene.press(tal::Key{tal::Code::end});
    shown.scene.wait(0ms);
    shown.scene.wait(0ms);

    SAME(shown.booklet->asked.size(), std::size_t{2});

    shown.booklet->holding = false;

    for (tal::Pending<void>& one : shown.booklet->waiting) one.settle();

    shown.settled();

    std::size_t of_first = 0;

    for (const Asked& one : shown.booklet->asked) of_first += one.page == 0 ? 1 : 0;

    SAME(of_first, std::size_t{2});
    OUGHT(shown.picture({0, 39}) != nullptr);
}

// Zoom: a step up makes the page wider than the view, the arrows move along it, and the picture
// is the part of the page there; a step down and back to the first step, where the page fits the
// view again; a zoom past the last step is the last.
TRIAL("Leaves: zoom and moving along a page wider than the view") {
    Shown shown{kitty()};

    shown.settled();
    shown.scene.press(tal::Key{U'+'});
    shown.settled();

    SAME(*shown.leaves.zoom, std::size_t{1});

    shown.scene.press(tal::Key{tal::Code::right});
    shown.settled();

    SAME(shown.leaves.left(), std::size_t{4});

    const tal::media::Pixels* moved = shown.picture({0, 0});

    OUGHT(moved != nullptr);
    OUGHT(pixel(*moved, 0, 0) == (std::vector<int>{0, 40 / 4, 0, 255}));

    shown.scene.press(tal::Key{U'+'});
    shown.scene.press(tal::Key{U'-'});
    shown.settled();

    SAME(*shown.leaves.zoom, std::size_t{1});

    shown.scene.press(tal::Key{U'0'});
    shown.settled();

    SAME(*shown.leaves.zoom, std::size_t{0});
    SAME(shown.leaves.left(), std::size_t{0});

    shown.leaves.zoom = 99;

    SAME(*shown.leaves.zoom, Leaves::zooms() - 1);
}

// One page at a time: the page fits the view whole, in its middle; page down and up turn to the
// next and the one before, end to the last, each telling `Turned`; the continuous layout comes
// back to the page shown.
TRIAL("Leaves: one page at a time, and back to the continuous layout at that page") {
    Shown shown{kitty()};

    shown.settled();
    shown.scene.press(tal::Key{U'v'});
    shown.settled();

    OUGHT(*shown.leaves.layout == Leaves::Layout::single);
    SAME(shown.leaves.extent(), std::size_t{40});

    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{1});
    OUGHT(shown.turned == (std::vector<std::size_t>{1}));

    const tal::media::Pixels* page = shown.picture({60, 20});

    OUGHT(page != nullptr);
    SAME(pixel(*page, 0, 0)[0], 1);
    OUGHT(shown.picture({29, 20}) == nullptr);

    shown.scene.press(tal::Key{tal::Code::end});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{499});

    shown.scene.press(tal::Key{tal::Code::pageup});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{498});
    OUGHT(shown.turned == (std::vector<std::size_t>{1, 499, 498}));

    shown.leaves.page(1);
    shown.settled();

    shown.scene.press(tal::Key{U'v'});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{1});
    SAME(shown.leaves.top(), std::size_t{81});
}

// A page the engine fails on shows the trouble in its place and is not asked again until the view
// moves; then it is, and the page comes.
TRIAL("Leaves: a trouble of the engine in place of its page, asked again when the view moves") {
    Shown shown;

    shown.booklet->failing = {0};
    shown.settled();

    OUGHT(shown.scene.where("The engine died.").has_value());
    SAME(shown.booklet->asked.size(), std::size_t{1});

    shown.booklet->failing.clear();
    shown.scene.press(tal::Key{tal::Code::down});
    shown.settled();

    OUGHT(!shown.scene.where("The engine died.").has_value());
    OUGHT(shown.drawn(0));
}

// The tiles kept hold at most `most` bytes while the view shows less than that, scrolling from
// the first page to the last.
TRIAL("Leaves: the tiles kept within their limit through the whole document") {
    Shown shown{kitty(), 60};

    shown.leaves.most = std::uint64_t{16} << 20;
    shown.settled();

    for (int step = 0; step < 200 && shown.leaves.page() < 59; ++step) {
        shown.scene.press(tal::Key{tal::Code::pagedown});
        shown.settled();
        OUGHT(shown.leaves.held() <= *shown.leaves.most);
    }

    SAME(shown.leaves.page(), std::size_t{59});
}

// The wheel moves the view three rows a step, page up and down a view less a row, home to the first
// page; each move tells `Scrolled`, and a move that shows most of another page `Turned`. Along a
// page wider than the view the wheel across moves the view sideways. A window of another height
// tells `Scrolled` too.
TRIAL("Leaves: the wheel and the keys move the view, and tell it") {
    Shown shown;
    std::size_t scrolled = 0;
    const tal::Hook counted = shown.leaves.on<tal::Scrolled>([&scrolled](tal::Scrolled&) { ++scrolled; });

    shown.settled();
    scrolled = 0;
    shown.scene.wheel({5, 5}, -1);
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{3});
    SAME(scrolled, std::size_t{1});

    shown.leaves.top(3);

    SAME(scrolled, std::size_t{1});

    shown.scene.press(tal::Key{tal::Code::up});
    shown.scene.press(tal::Key{tal::Code::down});
    shown.scene.press(tal::Key{tal::Code::down});
    shown.scene.press(tal::Key{tal::Code::up});
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{3});

    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{14});

    shown.scene.press(tal::Key{tal::Code::pageup});
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{3});

    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{1});
    OUGHT(shown.turned == (std::vector<std::size_t>{1}));

    shown.scene.press(tal::Key{tal::Code::home});
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{0});
    OUGHT(shown.turned == (std::vector<std::size_t>{1, 0}));

    shown.scene.press(tal::Key{U'+'});
    shown.scene.press(tal::Key{U'+'});
    shown.settled();
    shown.scene.wheel({5, 5}, -1, tal::Axis::horizontal);
    shown.settled();

    SAME(shown.leaves.left(), std::size_t{3});

    scrolled = 0;
    shown.scene.resize({40, 10});
    shown.settled();

    SAME(shown.leaves.window(), std::size_t{10});
    SAME(scrolled, std::size_t{1});
}

// Tiles held to a limit smaller than the view needs: the view's own tiles stay, so it shows whole
// and nothing is asked twice; none of the next view's is asked ahead, since none would fit.
TRIAL("Leaves: a limit smaller than the view keeps the view's tiles and asks for none ahead") {
    Shown shown{kitty()};

    shown.leaves.most = std::uint64_t{1} << 20;
    shown.settled();

    OUGHT(shown.picture({0, 0}) != nullptr);
    SAME(shown.booklet->asked.size(), std::size_t{6});
    OUGHT(shown.leaves.held() > *shown.leaves.most);
}

// The page shown most is the current one, the earlier of two shown as much (six rows each of a
// view of 13); a page asked for before the view is laid out is the first one it shows. Pages of no
// document are none.
TRIAL("Leaves: the current page, and a page asked for before the layout") {
    THREW(Leaves{nullptr});

    {
        Shown even{Display{.screen = {40, 13}, .blocks = Blocks::sextants}};

        even.settled();
        even.leaves.top(21);
        even.settled();

        SAME(even.leaves.page(), std::size_t{0});

        even.leaves.top(22);
        even.settled();

        SAME(even.leaves.page(), std::size_t{1});
    }

    Shown early;

    early.leaves.page(5);
    early.settled();

    SAME(early.leaves.page(), std::size_t{5});
    SAME(early.leaves.top(), std::size_t{5 * 28});
}

// A change of layout keeps the place on the page: a quarter down a page zoomed twice in the
// continuous layout is a quarter down it one page at a time. Tiles kept show again at once: back
// to a zoom whose tiles are kept, while the other zoom's are still being rendered, the page shows
// without asking anew.
TRIAL("Leaves: the place on the page kept across layouts, kept tiles shown at once") {
    {
        Shown shown{kitty()};

        shown.settled();
        shown.scene.press(tal::Key{U'+'});
        shown.scene.press(tal::Key{U'+'});
        shown.scene.press(tal::Key{U'+'});
        shown.settled();
        shown.leaves.top(161 + 40);
        shown.settled();
        shown.scene.press(tal::Key{U'v'});
        shown.settled();

        SAME(shown.leaves.page(), std::size_t{1});
        SAME(shown.leaves.top(), std::size_t{20});
    }

    Shown again{kitty()};

    again.settled();

    const std::size_t asked = again.booklet->asked.size();

    again.booklet->holding = true;
    again.scene.press(tal::Key{U'+'});
    again.scene.wait(0ms);
    again.scene.wait(0ms);
    UNTIL(again.loop, again.booklet->asked.size() == asked + 2);
    again.scene.press(tal::Key{U'0'});
    again.settled();

    OUGHT(again.picture({0, 0}) != nullptr);
    SAME(again.booklet->asked.size(), asked + 2);
}

// What the review of the pages found, each held to it: a terminal that tells its cell's size
// after the first frame gets the page in pixels at once; an exception that is no trouble shows its
// words in place of its page and keeps no tile from being asked for; a narrower or wider view
// keeps the page in it.
TRIAL("Leaves: a cell told late, an exception of another kind, a resize deep in the document") {
    {
        Shown shown;

        shown.settled();

        OUGHT(shown.picture({0, 0}) == nullptr);

        shown.scene.display(Display{.screen = {40, 12}, .graphics = Graphics::kitty, .blocks = Blocks::sextants, .cell = {10, 20}});
        shown.settled();

        const tal::media::Pixels* sharp = shown.picture({0, 0});

        OUGHT(sharp != nullptr);
        OUGHT(sharp->size() == (tal::media::Size{400, 240}));
        SAME(pixel(*sharp, 399, 0)[1], 399 / 4);
    }

    {
        Shown shown;

        shown.booklet->throwing = {0};
        shown.settled();

        OUGHT(shown.scene.where("The disk is gone.").has_value());

        shown.scene.press(tal::Key{tal::Code::end});
        shown.settled();

        SAME(shown.leaves.page(), std::size_t{499});
        OUGHT(shown.drawn(11));
        OUGHT(!shown.scene.where("The disk is gone.").has_value());
    }

    Shown shown;

    shown.settled();
    shown.leaves.page(250);
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{250 * 28});

    shown.scene.resize({60, 12});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{250});
    SAME(shown.leaves.top(), std::size_t{250 * 41});
}

// Pages of two shapes one at a time, zoomed four times: the view moves along the page in it only
// as far as its own right edge, which shows; a narrower page brings the view back within it. A
// page far taller than wide fits the view one at a time all the same. A
// document of no pages shows none and takes keys in peace.
TRIAL("Leaves: pages of two shapes moved along, and a document of no pages") {
    {
        Shown shown{kitty(), std::make_shared<Booklet>(std::vector<Page>{{600, 800}, {1600, 800}, {10, 2000}})};

        shown.settled();
        shown.scene.press(tal::Key{U'v'});

        for (int step = 0; step < 5; ++step) shown.scene.press(tal::Key{U'+'});

        shown.settled();

        for (int step = 0; step < 40; ++step) shown.scene.press(tal::Key{tal::Code::right});

        shown.settled();

        SAME(shown.leaves.left(), std::size_t{120});
        OUGHT(shown.picture({119, 0}) != nullptr);

        shown.leaves.left(1000);

        SAME(shown.leaves.left(), std::size_t{120});

        shown.scene.press(tal::Key{tal::Code::pagedown});
        shown.settled();

        for (int step = 0; step < 60; ++step) shown.scene.press(tal::Key{tal::Code::right});

        shown.settled();

        SAME(shown.leaves.left(), std::size_t{360});

        shown.scene.press(tal::Key{tal::Code::pageup});
        shown.settled();

        SAME(shown.leaves.left(), std::size_t{120});
        OUGHT(shown.picture({119, 0}) != nullptr);

        shown.scene.press(tal::Key{U'0'});
        shown.scene.press(tal::Key{tal::Code::end});
        shown.settled();

        SAME(shown.leaves.page(), std::size_t{2});
        SAME(shown.leaves.extent(), std::size_t{40});
    }

    Shown none{characters(), 0};

    none.settled();
    none.scene.press(tal::Key{tal::Code::end});
    none.scene.press(tal::Key{tal::Code::pagedown});
    none.scene.press(tal::Key{U'v'});
    none.settled();

    SAME(none.leaves.extent(), std::size_t{0});
    OUGHT(none.booklet->asked.empty());
}

// A lower limit lets go of all but the view's own tiles at once; one page at a time, turning the
// page, or going back to the top of the page in the view, tells `Scrolled`; so does a zoom that
// makes the document longer without moving the view; a change of layout and of zoom in one turn
// keeps the place on the page as the layout laid out last had it; page down on the last page asks
// nothing again, not even a page that failed.
TRIAL("Leaves: a lower limit, a turn that scrolls, two changes in one turn") {
    Shown shown{kitty()};
    std::size_t scrolled = 0;
    const tal::Hook counted = shown.leaves.on<tal::Scrolled>([&scrolled](tal::Scrolled&) { ++scrolled; });

    shown.settled();

    OUGHT(shown.leaves.held() > std::uint64_t{4915200});

    shown.leaves.most = std::uint64_t{1} << 20;

    SAME(shown.leaves.held(), std::uint64_t{(512 + 512 + 176) * 1024 * 4});

    shown.leaves.most = std::uint64_t{64} << 20;
    shown.leaves.top(81 + 8);
    shown.settled();
    shown.leaves.layout = Leaves::Layout::single;
    shown.leaves.zoom = 1;
    shown.settled();

    OUGHT(*shown.leaves.layout == Leaves::Layout::single);
    SAME(shown.leaves.page(), std::size_t{1});
    SAME(shown.leaves.top(), std::size_t{5});

    scrolled = 0;
    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.settled();

    SAME(shown.leaves.page(), std::size_t{2});
    SAME(scrolled, std::size_t{1});

    shown.scene.press(tal::Key{U'+'});
    shown.scene.press(tal::Key{U'+'});
    shown.settled();
    shown.scene.press(tal::Key{tal::Code::down});
    shown.settled();
    scrolled = 0;
    shown.leaves.page(2);
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{0});
    SAME(scrolled, std::size_t{1});

    shown.scene.press(tal::Key{U'0'});
    shown.scene.press(tal::Key{U'v'});
    shown.scene.press(tal::Key{tal::Code::home});
    shown.settled();
    scrolled = 0;
    shown.scene.press(tal::Key{U'+'});
    shown.settled();

    SAME(shown.leaves.top(), std::size_t{0});
    SAME(scrolled, std::size_t{1});

    shown.booklet->failing = {499};
    shown.scene.press(tal::Key{U'v'});
    shown.scene.press(tal::Key{tal::Code::end});
    shown.settled();

    const std::size_t asked = shown.booklet->asked.size();

    shown.scene.press(tal::Key{tal::Code::pagedown});
    shown.settled();

    SAME(shown.booklet->asked.size(), asked);
}

// Pages let go while their tiles are being rendered: the work is cancelled with them, and what
// comes back later finds nobody.
TRIAL("Leaves: let go while tiles are being rendered") {
    Scene scene{kitty()};
    tal::Loop loop{scene.time()};
    const auto booklet = std::make_shared<Booklet>(5);
    Document document{scene.time()};
    Leaves& leaves = document.children().add<Leaves>(booklet);

    booklet->holding = true;
    scene.show(document);
    scene.wait(0ms);
    UNTIL(loop, booklet->asked.size() >= 2);

    (void)document.children().remove(leaves);
    scene.wait(0ms);

    for (tal::Pending<void>& one : booklet->waiting) one.settle();

    scene.wait(0ms);
    UNTIL(loop, true);

    SAME(booklet->asked.size(), std::size_t{2});
}


// Lights tint the parts of the pages they mark as a marker would, each band of a pixel times the
// band of the light's colour, to the last pixel the light reaches: a marker's yellow, the current
// light orange, unless the sheet gives the part "light" (and the light in the state current) a
// colour of its own. What no light marks stays as it was; lights let go of leave the pages as they
// were.
TRIAL("Leaves: lights tint the parts of pages they mark") {
    Shown shown{kitty()};

    shown.settled();
    shown.leaves.lights({tal::Light{0, Region{0.5, 0.25, 0.25, 0.125}}, tal::Light{0, Region{0, 0, 0.1, 0.05}, true}, tal::Light{1, Region{}}});
    shown.settled();

    const tal::media::Pixels* lit = shown.picture({0, 0});

    OUGHT(lit != nullptr);
    OUGHT(pixel(*lit, 700, 450) == (std::vector<int>{0, 175 * 224 / 255, 112 * 102 / 255, 255}));
    OUGHT(pixel(*lit, 100, 60) == (std::vector<int>{0, 25 * 153 / 255, 15 * 51 / 255, 255}));
    OUGHT(pixel(*lit, 300, 300) == (std::vector<int>{0, 75, 75, 255}));
    OUGHT(pixel(*lit, 599, 450) == (std::vector<int>{0, 149, 112, 255}));
    OUGHT(pixel(*lit, 899, 599) == (std::vector<int>{0, 224 * 224 / 255, 149 * 102 / 255, 255}));
    OUGHT(pixel(*lit, 900, 450) == (std::vector<int>{0, 225, 112, 255}));
    OUGHT(pixel(*lit, 700, 600) == (std::vector<int>{0, 175, 150, 255}));

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "leaves::light", "style": {"paper": "#80ff80"}},
        {"select": "leaves:current::light", "style": {"paper": "#ff8080"}}], "glyphs": {}})"));
    shown.leaves.lights({tal::Light{0, Region{0.5, 0.25, 0.25, 0.125}}, tal::Light{0, Region{0, 0, 0.1, 0.05}, true}});
    shown.settled();

    const tal::media::Pixels* sheeted = shown.picture({0, 0});

    OUGHT(sheeted != nullptr);
    OUGHT(pixel(*sheeted, 700, 450) == (std::vector<int>{0, 175, 112 * 128 / 255, 255}));
    OUGHT(pixel(*sheeted, 100, 60) == (std::vector<int>{0, 25 * 128 / 255, 15 * 128 / 255, 255}));

    shown.leaves.lights({});
    shown.settled();

    const tal::media::Pixels* plain = shown.picture({0, 0});

    OUGHT(plain != nullptr);
    OUGHT(pixel(*plain, 700, 450) == (std::vector<int>{0, 175, 112, 255}));
    OUGHT(shown.leaves.lights().empty());
}

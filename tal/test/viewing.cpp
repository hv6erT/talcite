#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/model.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/views.hpp"

#include "tal/testing.hpp"

// Models and their views on a scene: a list paints only the rows it shows and only those that
// changed, follows the model's changes without moving the screen, keeps its marks, answers keys and
// the mouse; a table shares its width among columns under their titles; a tree opens and closes its
// branches; a bar shows and moves the view.

namespace {

using namespace std::chrono_literals;

using tal::Area;
using tal::Branch;
using tal::Changes;
using tal::Code;
using tal::Document;
using tal::Edge;
using tal::Elide;
using tal::Entry;
using tal::Heading;
using tal::Hooks;
using tal::Key;
using tal::List;
using tal::Mods;
using tal::Node;
using tal::Picked;
using tal::Row;
using tal::Scroll;
using tal::Scrolled;
using tal::Share;
using tal::Size;
using tal::Sorted;
using tal::Spot;
using tal::State;
using tal::Table;
using tal::Tree;
using tal::TreeModel;
using tal::testing::Scene;

// Whether `what` throws an exception of type E.
template <typename E, typename F>
bool throws(F what) {
    try {
        what();
    } catch (const E&) {
        return true;
    }

    return false;
}

// Whether the row `said` begins with `start`.
bool begins(const std::string& said, const std::string& start) { return said.rfind(start, 0) == 0; }

// Rows of text in columns, counting the entries a view asks for; each change tells its views.
class Rows : public tal::Model {
public:
    // `many` rows "r<number>" in one column, or in `columns` columns "r<number>c<column>".
    explicit Rows(std::size_t many, std::size_t columns = 1) {
        for (std::size_t row = 0; row < many; ++row) data.push_back(made(row, columns));
    }

    std::vector<std::vector<std::string>> data;
    std::map<std::size_t, Entry> special;
    std::shared_ptr<std::size_t> asked = std::make_shared<std::size_t>(0);

    // How many rows there are.
    std::size_t rows() const override { return data.size(); }

    // The text of a cell, or the special entry of the row's first column; a missing column is
    // empty.
    Entry entry(std::size_t row, std::size_t column) const override {
        ++*asked;

        if (column == 0 && special.contains(row)) return special.at(row);

        Entry one;

        one.text = column < data[row].size() ? data[row][column] : std::string{};

        return one;
    }

    // Inserts `many` rows "n<number>" at `at` and tells it.
    void insert(std::size_t at, std::size_t many) {
        for (std::size_t one = 0; one < many; ++one) data.insert(data.begin() + static_cast<std::ptrdiff_t>(at + one), {"n" + std::to_string(one)});

        told(Changes{Changes::Kind::inserted, at, many});
    }

    // Removes `many` rows from `at` and tells it.
    void remove(std::size_t at, std::size_t many) {
        data.erase(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + many));

        told(Changes{Changes::Kind::removed, at, many});
    }

    // Changes the text of the row `at` and tells it.
    void change(std::size_t at, std::string text) {
        data[at] = {std::move(text)};

        told(Changes{Changes::Kind::changed, at, 1});
    }

    // Tells that `many` rows from `at` changed, without changing them.
    void touched(std::size_t at, std::size_t many) { told(Changes{Changes::Kind::changed, at, many}); }

    // Keeps the first `many` rows and tells a reset.
    void reset(std::size_t many) {
        data.resize(many);

        told(Changes{Changes::Kind::reset});
    }

private:
    // The cells of the row `row`.
    static std::vector<std::string> made(std::size_t row, std::size_t columns) {
        if (columns == 1) return {"r" + std::to_string(row)};

        std::vector<std::string> cells;

        for (std::size_t one = 0; one < columns; ++one) cells.push_back("r" + std::to_string(row) + "c" + std::to_string(one));

        return cells;
    }
};

// A list of `many` rows filling a scene of `size`, laid out and focused.
struct Shown {
    Scene scene;
    Document document{scene.time()};
    std::shared_ptr<Rows> rows;
    List* list = nullptr;
    std::vector<std::size_t> picked;
    Hooks hooks;

    // Builds and shows it.
    explicit Shown(std::size_t many, Size size = {12, 5}, bool focused = true) : scene({.screen = size}), rows(std::make_shared<Rows>(many)) {
        list = &document.children().add<List>(rows);
        hooks.add(list->on<Picked>([this](Picked& what) { picked.push_back(what.row); }));
        scene.show(document);

        if (focused) list->focus();

        scene.wait(0ms);
    }

    // How many entries the views asked for since the last call.
    std::size_t asked() {
        const std::size_t made = *rows->asked;

        *rows->asked = 0;

        return made;
    }
};

// The rows from the view's top, one to a row of the screen; the current one in the look of the
// focus only while the list has the focus, and in the look of `current` always. Past the last row
// the ground stays plain.
TRIAL("List: the rows it shows, the current one lit only with the focus") {
    Shown shown{3};

    SAME(shown.scene.row(0), std::string{"r0          "});
    SAME(shown.scene.row(2), std::string{"r2          "});
    SAME(shown.scene.row(3), std::string{"            "});
    OUGHT(shown.scene.at({0, 0}).look.reverse);
    OUGHT(shown.scene.at({11, 0}).look.reverse);
    OUGHT(!shown.scene.at({0, 1}).look.reverse);
    OUGHT(!shown.scene.at({0, 3}).look.reverse);
    SAME(shown.list->window(), std::size_t{5});
    SAME(shown.list->extent(), std::size_t{3});

    shown.list->current = 1;
    shown.scene.wait(0ms);

    OUGHT(shown.scene.at({0, 1}).look.reverse);
    OUGHT(!shown.scene.at({0, 0}).look.reverse);

    Shown idle{3, {12, 5}, false};

    OUGHT(!idle.scene.at({0, 0}).look.reverse);

    idle.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "list:current", "style": {"bold": true}}], "glyphs": {}})"));
    idle.list->current = 2;
    idle.scene.wait(0ms);

    OUGHT(idle.scene.at({0, 2}).look.bold);
    OUGHT(!idle.scene.at({0, 0}).look.bold);

    idle.list->enable(false);
    idle.scene.wait(0ms);

    OUGHT(idle.scene.at({0, 1}).look.faint);
    OUGHT(idle.scene.at({0, 4}).look.faint);
}

// The arrows, the page keys, Home and End move the current row, the view following it; Enter tells
// `Picked`. Keys with modifiers, and any key in an empty list, are left to the program.
TRIAL("List: keys move the current row and the view, Enter picks") {
    Shown shown{100};
    List& list = *shown.list;
    const std::vector<std::tuple<Key, std::size_t, std::size_t>> steps{
        {Key{Code::up}, 0, 0},       {Key{Code::down}, 1, 0},     {Key{Code::down}, 2, 0},     {Key{Code::down}, 3, 0},
        {Key{Code::down}, 4, 0},     {Key{Code::down}, 5, 1},     {Key{Code::pagedown}, 10, 6}, {Key{Code::pageup}, 5, 1},
        {Key{Code::pageup}, 0, 0},   {Key{Code::end}, 99, 95},    {Key{Code::down}, 99, 95},   {Key{Code::pagedown}, 99, 95},
        {Key{Code::pageup}, 94, 90}, {Key{Code::home}, 0, 0},     {Key{Code::pagedown}, 5, 5},  {Key{Code::up}, 4, 4},
        {Key{Code::down, Mods{true, false, false}}, 4, 4}};

    for (const auto& [key, current, top] : steps) {
        shown.scene.press(key);

        SAME(*list.current, current);
        SAME(list.top(), top);
    }

    SAME(shown.scene.row(0), std::string{"r4          "});

    shown.scene.press(Key{Code::down});
    shown.scene.press(Key{Code::enter});

    OUGHT(shown.picked == (std::vector<std::size_t>{5}));

    Shown empty{0};

    empty.scene.press(Key{Code::down});
    empty.scene.press(Key{Code::enter});

    OUGHT(empty.picked.empty());
    SAME(*empty.list->current, std::size_t{0});
}

// A view asks the model only for the rows it paints: a step inside the view paints the two rows the
// light moves between, a step past its end all of them; a change of a row out of view paints
// nothing, of one in view only that row.
TRIAL("List: only the rows that changed are painted") {
    Shown shown{20000};

    (void)shown.asked();

    shown.scene.press(Key{Code::down});

    SAME(shown.asked(), std::size_t{2});

    for (int one = 0; one < 3; ++one) shown.scene.press(Key{Code::down});

    (void)shown.asked();

    shown.scene.press(Key{Code::down});

    SAME(shown.asked(), std::size_t{5});
    SAME(shown.list->top(), std::size_t{1});

    shown.rows->change(100, "far");
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{0});

    shown.rows->change(3, "near");
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{1});
    SAME(shown.scene.row(2), std::string{"near        "});

    shown.rows->touched(0, 3);
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{2});

    shown.scene.press(Key{Code::pagedown});
    (void)shown.asked();
    shown.list->top(0);
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{5});
    SAME(*shown.list->current, std::size_t{10});
}

// Rows that come or go above the view move the view, the current row and the marks with them, so
// that the screen stays as it was and nothing is painted; in the view the rows from the change on
// are painted again. A row taken away under the current one leaves the next one current.
TRIAL("List: rows coming and going above the view leave the screen as it was") {
    Shown shown{100};
    List& list = *shown.list;

    list.top(50);
    list.current = 52;
    list.mark(53, true);
    list.mark(10, true);
    shown.scene.wait(0ms);
    (void)shown.asked();

    const std::string before = shown.scene.text();

    shown.rows->insert(5, 3);
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{0});
    SAME(list.top(), std::size_t{53});
    SAME(*list.current, std::size_t{55});
    OUGHT(list.marks() == (std::vector<std::size_t>{13, 56}));
    SAME(shown.scene.text(), before);

    shown.rows->remove(0, 4);
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{0});
    SAME(list.top(), std::size_t{49});
    SAME(*list.current, std::size_t{51});
    OUGHT(list.marks() == (std::vector<std::size_t>{9, 52}));
    SAME(shown.scene.text(), before);

    shown.rows->insert(51, 1);
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{3});
    SAME(*list.current, std::size_t{52});
    SAME(shown.scene.row(2), std::string{"n0          "});

    shown.rows->remove(52, 1);
    shown.scene.wait(0ms);

    SAME(*list.current, std::size_t{52});
    SAME(shown.scene.row(2), std::string{"n0          "});
    SAME(shown.scene.row(3), std::string{"r53         "});
    OUGHT(list.marks() == (std::vector<std::size_t>{9, 52}));

    shown.rows->remove(8, 2);
    shown.scene.wait(0ms);

    OUGHT(list.marks() == (std::vector<std::size_t>{50}));
}

// Removing rows near the end pulls the view back, so that it stays full; a reset takes the marks
// away and keeps the view and the current row inside the rows.
TRIAL("List: the end pulls the view back, a reset clears the marks") {
    Shown shown{20};
    List& list = *shown.list;

    shown.scene.press(Key{Code::end});
    list.mark(18, true);
    shown.rows->remove(10, 6);
    shown.scene.wait(0ms);

    SAME(list.top(), std::size_t{9});
    SAME(*list.current, std::size_t{13});
    SAME(shown.scene.row(0), std::string{"r9          "});
    SAME(shown.scene.row(4), std::string{"r19         "});

    shown.rows->reset(3);
    shown.scene.wait(0ms);

    SAME(list.top(), std::size_t{0});
    OUGHT(list.marks().empty());
    SAME(shown.scene.row(2), std::string{"r2          "});
    OUGHT(shown.scene.at({0, 2}).look.reverse);

    shown.rows->insert(0, 2);
    shown.scene.wait(0ms);

    SAME(*list.current, std::size_t{15});
    OUGHT(shown.scene.at({0, 4}).look.reverse);
}

// Marks are the view's: a row marked shows the look of the mark, the marks are listed in order and
// taken away all at once; a row past the last throws.
TRIAL("List: marks of rows, in order, taken away at once") {
    Shown shown{10};
    List& list = *shown.list;

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "list:mark", "style": {"bold": true}}], "glyphs": {}})"));
    shown.scene.wait(0ms);
    list.mark(3, true);
    list.mark(1, true);
    list.mark(3, true);

    OUGHT(list.marks() == (std::vector<std::size_t>{1, 3}));

    list.mark(2, false);
    shown.scene.wait(0ms);

    OUGHT(list.marks() == (std::vector<std::size_t>{1, 3}));
    OUGHT(list.marked(1) && !list.marked(2));
    OUGHT(shown.scene.at({0, 3}).look.bold);
    OUGHT(!shown.scene.at({0, 2}).look.bold);

    list.mark(1, false);
    list.unmark();
    shown.scene.wait(0ms);

    OUGHT(list.marks().empty());
    OUGHT(!shown.scene.at({0, 3}).look.bold);
    OUGHT(throws<std::out_of_range>([&list] { list.mark(10, true); }));
    OUGHT(throws<std::invalid_argument>([] { List none{nullptr}; }));
}

// A click makes a row current and takes the focus, a double click tells `Picked`, the wheel moves
// the view and not the current row; under the last row a click does nothing.
TRIAL("List: the mouse makes current, picks and scrolls") {
    Shown shown{50};
    List& list = *shown.list;

    shown.scene.click({3, 2});

    SAME(*list.current, std::size_t{2});
    OUGHT(list.focused());

    shown.scene.twice({3, 4});

    SAME(*list.current, std::size_t{4});
    OUGHT(shown.picked == (std::vector<std::size_t>{4}));

    shown.scene.wheel({3, 1}, -2);

    SAME(list.top(), std::size_t{6});
    SAME(*list.current, std::size_t{4});

    shown.scene.wheel({3, 1}, 1);

    SAME(list.top(), std::size_t{3});

    shown.scene.wheel({3, 1}, 5);

    SAME(list.top(), std::size_t{0});

    Shown few{2};

    few.scene.click({3, 4});

    SAME(*few.list->current, std::size_t{0});
}

// An entry's part fills its cell in that part's look at the row's states, past its text too, its
// marks take theirs, a long text is cut as it asks.
TRIAL("List: an entry's part, marks and elision") {
    Shown shown{3};
    Entry special;

    special.text = "a very long name";
    special.part = "folder";
    special.elide = Elide::middle;
    special.marks = {tal::Mark{0, 1, "sign", {}}};
    shown.rows->special[1] = special;

    Entry folder;

    folder.text = "dir";
    folder.part = "folder";
    shown.rows->special[2] = folder;
    shown.document.sheet(tal::StyleSheet::parsed(
        R"({"rules": [{"select": "list::folder", "style": {"bold": true, "under": true}}, {"select": "list:focus::folder", "style": {"italic": true}},
                      {"select": "list::sign", "style": {"under": true}}], "glyphs": {}})"));
    shown.rows->change(1, "x");
    shown.scene.wait(0ms);

    SAME(shown.scene.row(1), std::string{"a very… name"});
    OUGHT(shown.scene.at({1, 1}).look.bold);
    OUGHT(shown.scene.at({11, 1}).look.bold);
    OUGHT(shown.scene.at({11, 2}).look.under);
    OUGHT(shown.scene.at({0, 1}).look.under);
    OUGHT(!shown.scene.at({0, 1}).look.bold);

    shown.scene.press(Key{Code::down});

    OUGHT(shown.scene.at({1, 1}).look.italic);
    OUGHT(shown.scene.at({1, 1}).look.bold);
    OUGHT(!shown.scene.at({1, 0}).look.italic);
}

// The columns share the width as a row does, a column apart, under their titles standing as their
// cells; a cell that spans takes the rest of its row, and a changed row is painted under the
// titles. A click on a title or the gap after it tells `Sorted` with its column; under the rows or
// without the titles nothing does, and without them the rows begin at the top.
TRIAL("Table: columns under their titles, a spanning cell, sorting asked by a click") {
    Scene scene{{.screen = {20, 4}}};
    Document document{scene.time()};
    auto rows = std::make_shared<Rows>(2, 3);
    Table& table = document.children().add<Table>(
        rows, std::vector<Heading>{{"Name", Share{1, 0}, Edge::start}, {"Sz", Share{0, 6}, Edge::end}, {"Date", Share{0, 7}, Edge::middle}});
    std::vector<std::size_t> sorted;
    Hooks hooks;

    hooks.add(table.on<Sorted>([&sorted](Sorted& what) { sorted.push_back(what.column); }));

    Entry wide;

    wide.text = "spanning the whole row";
    wide.spans = true;
    rows->special[1] = wide;
    scene.show(document);

    SAME(scene.row(0), std::string{"Name      Sz  Date  "});
    OUGHT(scene.at({0, 0}).look.reverse);
    OUGHT(scene.at({5, 0}).look.reverse);
    SAME(scene.row(1), std::string{"r0c0    r0c1  r0c2  "});
    SAME(scene.row(2), std::string{"spanning the whole r"});
    SAME(table.window(), std::size_t{3});

    rows->change(0, "x");
    scene.wait(0ms);

    SAME(scene.row(1), std::string{"x                   "});
    SAME(scene.row(0), std::string{"Name      Sz  Date  "});

    scene.click({5, 0});
    scene.click({12, 0});
    scene.click({19, 0});
    scene.click({3, 3});

    OUGHT(sorted == (std::vector<std::size_t>{0, 1, 2}));

    table.heading = false;
    scene.wait(0ms);

    SAME(scene.row(0), std::string{"x                   "});
    SAME(table.window(), std::size_t{4});

    scene.wait(1s);
    scene.click({3, 3});
    scene.wait(1s);
    scene.click({10, 0});

    OUGHT(sorted.size() == 3);
    SAME(*table.current, std::size_t{0});

    table.columns = std::vector<Heading>{{"N", Share{0, 3}, Edge::start}};
    scene.wait(0ms);

    SAME(scene.row(0), std::string{"x                   "});
    SAME(scene.row(1), std::string{"spanning the whole r"});
    rows->reset(0);
    scene.wait(1s);
    scene.click({3, 0});

    OUGHT(sorted.size() == 3);
}

// Columns that yield give up their place when the table is too narrow for all, the highest yield
// first: they show neither title nor cells, no gap is left for them, and a click lands on the
// columns that are there.
TRIAL("Table: columns that yield leave a narrow table first") {
    Scene scene{{.screen = {14, 3}}};
    Document document{scene.time()};
    auto rows = std::make_shared<Rows>(1, 3);
    Table& table = document.children().add<Table>(
        rows, std::vector<Heading>{{"Name", Share{1, 4}, Edge::start}, {"Sz", Share{0, 4, 1}, Edge::end}, {"Date", Share{0, 6, 2}, Edge::start}});
    std::vector<std::size_t> sorted;
    Hooks hooks;

    hooks.add(table.on<Sorted>([&sorted](Sorted& what) { sorted.push_back(what.column); }));
    scene.show(document);

    SAME(scene.row(0), std::string{"Name        Sz"});
    SAME(scene.row(1), std::string{"r0c0      r0c1"});

    scene.click({13, 0});

    OUGHT(sorted == (std::vector<std::size_t>{1}));

    scene.resize({20, 3});
    scene.wait(0ms);

    SAME(scene.row(0), std::string{"Name       Sz Date  "});
}

// A tree over a model of nested rows: two columns in for each level, the mark of the branch, the
// text. The right arrow opens a closed branch, the left one closes an open one and goes from any
// other row to its parent; a click on the mark opens or closes it, and on a leaf does nothing.
TRIAL("Tree: indents and marks, opened and closed by keys and clicks") {
    // A root holding two branches, the first holding two leaves.
    class Nested : public TreeModel {
    public:
        struct Line {
            std::string text;
            int depth = 0;
            Branch branch = Branch::leaf;
        };

        std::vector<Line> lines{{"root", 0, Branch::open}, {"one", 1, Branch::closed}, {"two", 1, Branch::closed}};
        int asked = 0;

        // How many rows show.
        std::size_t rows() const override { return lines.size(); }

        // The text of a row.
        Entry entry(std::size_t row, std::size_t column) const override {
            (void)column;

            Entry made;

            made.text = lines[row].text;

            return made;
        }

        // The depth of a row.
        int depth(std::size_t row) const override { return lines[row].depth; }

        // The branch of a row.
        Branch branch(std::size_t row) const override { return lines[row].branch; }

        // Opens a branch with its children (the root's two branches, a branch's two leaves), or
        // closes an open one, taking its children away.
        void open(std::size_t row, bool on) override {
            ++asked;

            if (on) {
                const std::vector<Line> children = lines[row].depth == 0 ? std::vector<Line>{{"one", 1, Branch::closed}, {"two", 1, Branch::closed}}
                                                                         : std::vector<Line>{{"a", 2, Branch::leaf}, {"b", 2, Branch::leaf}};

                lines[row].branch = Branch::open;
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(row + 1), children.begin(), children.end());
                told(Changes{Changes::Kind::changed, row, 1});
                told(Changes{Changes::Kind::inserted, row + 1, 2});

                return;
            }

            std::size_t end = row + 1;

            while (end < lines.size() && lines[end].depth > lines[row].depth) ++end;

            lines[row].branch = Branch::closed;
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(row + 1), lines.begin() + static_cast<std::ptrdiff_t>(end));
            told(Changes{Changes::Kind::changed, row, 1});
            told(Changes{Changes::Kind::removed, row + 1, end - row - 1});
        }
    };

    Scene scene{{.screen = {12, 6}}};
    Document document{scene.time()};
    auto model = std::make_shared<Nested>();
    Tree& tree = document.children().add<Tree>(model);

    scene.show(document);
    tree.focus();
    scene.wait(0ms);

    SAME(scene.row(0), std::string{"▾ root      "});
    SAME(scene.row(1), std::string{"  ▸ one     "});
    SAME(scene.row(2), std::string{"  ▸ two     "});

    scene.press(Key{Code::down});
    scene.press(Key{Code::right});

    SAME(scene.row(1), std::string{"  ▾ one     "});
    SAME(scene.row(2), std::string{"      a     "});
    SAME(*tree.current, std::size_t{1});

    scene.press(Key{Code::right});
    scene.press(Key{Code::down});
    scene.press(Key{Code::down});
    scene.press(Key{Code::left});

    SAME(*tree.current, std::size_t{1});

    scene.press(Key{Code::left});

    SAME(model->lines.size(), std::size_t{3});
    SAME(scene.row(2), std::string{"  ▸ two     "});

    scene.press(Key{Code::left});

    SAME(*tree.current, std::size_t{0});

    scene.press(Key{Code::left});

    SAME(model->lines.size(), std::size_t{1});

    scene.press(Key{Code::left});
    scene.press(Key{Code::right, Mods{false, false, true}});

    SAME(model->lines.size(), std::size_t{1});

    scene.click({0, 0});

    SAME(model->lines.size(), std::size_t{3});

    scene.wait(1s);
    scene.click({3, 1});
    scene.wait(1s);
    scene.click({1, 1});

    SAME(model->lines.size(), std::size_t{3});

    scene.wait(1s);
    scene.click({2, 1});

    SAME(model->lines.size(), std::size_t{5});
    SAME(*tree.current, std::size_t{1});

    scene.press(Key{Code::right});

    SAME(model->lines.size(), std::size_t{5});

    const int before = model->asked;

    scene.wait(1s);
    scene.click({4, 2});

    SAME(model->asked, before);

    scene.wait(1s);
    scene.click({2, 1});

    SAME(model->lines.size(), std::size_t{3});

    document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "tree", "style": {"border": "thin", "padding": [0, 0, 0, 1]}}], "glyphs": {}})"));
    scene.wait(1s);
    scene.click({4, 2});

    SAME(model->lines.size(), std::size_t{5});
}

// With `branches` the indent of a tree holds the lines of its branches: down from each level that
// goes on below, a joint into a row that more follow at its level, a corner into the last one; a
// row at the top has none, and the marks stay where a click finds them.
TRIAL("Tree: the lines of the branches") {
    // A root holding a branch of two leaves and a branch of one.
    class Fixed : public TreeModel {
    public:
        struct Line {
            std::string text;
            int depth = 0;
            Branch branch = Branch::leaf;
        };

        std::vector<Line> lines{{"root", 0, Branch::open}, {"one", 1, Branch::open}, {"a", 2, Branch::leaf}, {"b", 2, Branch::leaf}, {"two", 1, Branch::open}, {"c", 2, Branch::leaf}};
        int asked = 0;

        // How many rows show.
        std::size_t rows() const override { return lines.size(); }

        // The text of a row.
        Entry entry(std::size_t row, std::size_t column) const override {
            (void)column;

            Entry made;

            made.text = lines[row].text;

            return made;
        }

        // The depth of a row.
        int depth(std::size_t row) const override { return lines[row].depth; }

        // The branch of a row.
        Branch branch(std::size_t row) const override { return lines[row].branch; }

        // Counts what it is asked to open or close.
        void open(std::size_t row, bool on) override {
            (void)row;
            (void)on;
            ++asked;
        }
    };

    Scene scene{{.screen = {12, 7}}};
    Document document{scene.time()};
    auto model = std::make_shared<Fixed>();
    Tree& tree = document.children().add<Tree>(model);

    tree.branches = true;
    scene.show(document);
    scene.wait(0ms);

    SAME(scene.row(0), std::string{"▾ root      "});
    SAME(scene.row(1), std::string{"├─▾ one     "});
    SAME(scene.row(2), std::string{"│ ├─  a     "});
    SAME(scene.row(3), std::string{"│ └─  b     "});
    SAME(scene.row(4), std::string{"└─▾ two     "});
    SAME(scene.row(5), std::string{"  └─  c     "});

    scene.click({2, 4});

    SAME(model->asked, 1);

    tree.branches = false;
    scene.wait(0ms);

    SAME(scene.row(2), std::string{"      a     "});
}

// The bar stands beside its view: the track down its column, the thumb as long as the window is to
// all rows and where the view stands. A press of the left button above or below the thumb moves
// the view a window, dragging moves it along, the wheel as over the view. It shows the focus of the
// view; a view gone leaves it empty, and one no one holds throws.
TRIAL("Scroll: the thumb shows and moves the view") {
    Scene scene{{.screen = {8, 10}}};
    Document document{scene.time()};
    auto rows = std::make_shared<Rows>(40);
    tal::Column& column = document.children().add<tal::Column>();
    Row& row = column.children().add<Row>();

    row.share(Share{1, 0});

    List& list = row.children().add<List>(rows);
    tal::Button& other = column.children().add<tal::Button>(std::make_shared<tal::Action>("x"));

    list.share(Share{1, 0});
    scene.show(document);
    list.focus();

    Scroll& bar = row.children().add<Scroll>(list);

    scene.wait(0ms);

    const auto track = [&scene](int x = 7) {
        std::string made;

        for (int y = 0; y < 9; ++y) made += scene.at({x, y}).cluster == "\xe2\x96\x88" ? "#" : scene.at({x, y}).cluster == "\xe2\x94\x82" ? "|" : "?";

        return made;
    };

    SAME(track(), std::string{"##|||||||"});
    OUGHT(bar.states().has(State::focus));

    other.focus();
    scene.wait(0ms);

    OUGHT(!bar.states().has(State::focus));

    scene.click({7, 5});

    SAME(list.top(), std::size_t{9});
    SAME(track(), std::string{"||##|||||"});

    scene.click({7, 7}, tal::MouseButton::right);

    SAME(list.top(), std::size_t{9});

    for (int one = 0; one < 3; ++one) {
        scene.wait(1s);
        scene.click({7, 8});
    }

    SAME(list.top(), std::size_t{31});
    SAME(track(), std::string{"|||||||##"});

    scene.wait(1s);
    scene.click({7, 0});

    SAME(list.top(), std::size_t{22});

    document.mouse(tal::Press::down, tal::MouseButton::left, Spot{7, 4});
    document.mouse(tal::Press::move, tal::MouseButton::left, Spot{0, 9});
    scene.wait(0ms);

    SAME(list.top(), std::size_t{31});

    document.mouse(tal::Press::move, tal::MouseButton::left, Spot{0, 2});
    scene.wait(0ms);

    SAME(list.top(), std::size_t{9});

    document.mouse(tal::Press::move, tal::MouseButton::left, Spot{0, 3});
    document.mouse(tal::Press::up, tal::MouseButton::left, Spot{0, 3});
    scene.wait(0ms);

    SAME(list.top(), std::size_t{13});

    document.mouse(tal::Press::down, tal::MouseButton::left, Spot{7, 3});
    document.mouse(tal::Press::move, tal::MouseButton::left, Spot{0, 0});
    document.mouse(tal::Press::up, tal::MouseButton::left, Spot{0, 0});
    scene.wait(0ms);

    SAME(list.top(), std::size_t{0});

    list.top(13);
    document.mouse(tal::Press::move, tal::MouseButton::none, Spot{7, 8});
    scene.wait(0ms);

    SAME(list.top(), std::size_t{13});

    scene.wheel({7, 3}, -1);

    SAME(list.top(), std::size_t{16});

    scene.wheel({7, 3}, 1, tal::Axis::horizontal);

    SAME(list.top(), std::size_t{16});

    rows->reset(9);
    scene.wait(0ms);

    SAME(track(), std::string{"|||||||||"});

    scene.click({7, 8});

    SAME(list.top(), std::size_t{0});

    rows->reset(0);
    rows->insert(0, 20000);
    scene.wait(0ms);

    SAME(track(), std::string{"#||||||||"});

    row.children().remove(list);
    scene.wait(0ms);

    SAME(track(0), std::string{"|||||||||"});

    List alone{rows};

    OUGHT(throws<std::invalid_argument>([&alone] { Scroll none{alone}; }));
}

// A new size of the bar takes where the thumb stands then, so that a later move back to that place
// paints it again.
TRIAL("Scroll: a new size, then the view moving back") {
    Scene scene{{.screen = {8, 10}}};
    Document document{scene.time()};
    auto rows = std::make_shared<Rows>(40);
    Row& row = document.children().add<Row>();
    List& list = row.children().add<List>(rows);

    list.share(Share{1, 0});

    Scroll& bar = row.children().add<Scroll>(list);

    (void)bar;
    scene.show(document);
    list.top(30);
    scene.wait(0ms);
    scene.resize(Size{8, 20});
    list.top(0);
    scene.wait(0ms);

    SAME(scene.at({7, 0}).cluster, std::string{"\xe2\x96\x88"});
    SAME(scene.at({7, 19}).cluster, std::string{"\xe2\x94\x82"});
}

// What the list does not take goes on to the nodes above it: Enter in an empty list, a sideways
// wheel, the wheel at the end of the rows, a press of another button. A click takes the focus from
// another node.
TRIAL("List: what it leaves to the nodes above") {
    // A column that hears the keys and the mouse nothing under it took.
    class Catcher : public tal::Column {
    public:
        Catcher() {
            hooks_.add(otherwise<tal::KeyEvent>([this](tal::KeyEvent& what) { heard.push_back(tal::to_string(what.key)); }));
            hooks_.add(otherwise<tal::MouseEvent>([this](tal::MouseEvent& what) {
                if (what.press == tal::Press::wheel) heard.push_back("wheel");
                if (what.press == tal::Press::click) heard.push_back("click");
            }));
        }

        std::vector<std::string> heard;

    private:
        Hooks hooks_;
    };

    Scene scene{{.screen = {12, 6}}};
    Document document{scene.time()};
    Catcher& catcher = document.children().add<Catcher>();
    tal::Button& other = catcher.children().add<tal::Button>(std::make_shared<tal::Action>("x"));
    auto rows = std::make_shared<Rows>(10);
    List& list = catcher.children().add<List>(rows);

    list.share(Share{1, 0});
    scene.show(document);
    other.focus();
    scene.click({2, 1});

    OUGHT(list.focused());
    SAME(*list.current, std::size_t{0});

    scene.press(Key{Code::enter});
    scene.wheel({2, 2}, 1);
    scene.wheel({2, 2}, -1, tal::Axis::horizontal);

    SAME(list.top(), std::size_t{0});

    scene.wheel({2, 2}, -1);
    scene.wheel({2, 2}, -2);
    scene.wheel({2, 2}, -1);
    scene.wait(1s);
    scene.click({2, 3}, tal::MouseButton::right);

    SAME(list.top(), std::size_t{5});
    SAME(*list.current, std::size_t{0});
    OUGHT(catcher.heard == (std::vector<std::string>{"wheel", "wheel", "wheel", "click"}));

    rows->reset(0);
    scene.press(Key{Code::enter});

    OUGHT(catcher.heard.back() == "Enter");
}

// A list in a frame keeps its rows inside it: they start past the border and the padding, a click
// on the border picks no row, and a tree's mark is found inside it too.
TRIAL("List: inside a frame and padding") {
    Shown shown{20, {12, 6}};

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "list", "style": {"border": "thin", "padding": [0, 0, 0, 1]}}], "glyphs": {}})"));
    shown.scene.wait(0ms);

    SAME(shown.list->window(), std::size_t{4});
    OUGHT(begins(shown.scene.row(1), "\xe2\x94\x82 r0"));

    shown.scene.click({3, 3});

    SAME(*shown.list->current, std::size_t{2});

    shown.scene.wait(1s);
    shown.scene.click({3, 5});
    shown.scene.wait(1s);
    shown.scene.click({0, 2});
    shown.scene.wait(1s);
    shown.scene.click({11, 2});

    SAME(*shown.list->current, std::size_t{2});
}

// Rows that come at a marked row move its mark; rows coming into an empty list leave the first one
// current; a row coming just above the view paints nothing; rows going around the view's top pull
// the view to where they were.
TRIAL("List: rows at a mark, into an empty list, just above the view, around its top") {
    Shown shown{100};
    List& list = *shown.list;

    list.mark(5, true);
    shown.rows->insert(5, 1);

    OUGHT(list.marks() == (std::vector<std::size_t>{6}));

    list.top(10);
    list.current = 12;
    shown.scene.wait(0ms);
    (void)shown.asked();
    shown.rows->insert(9, 1);
    shown.scene.wait(0ms);

    SAME(shown.asked(), std::size_t{0});
    SAME(list.top(), std::size_t{11});

    list.top(50);
    list.current = 52;
    shown.scene.wait(0ms);
    shown.rows->remove(48, 5);
    shown.scene.wait(0ms);

    SAME(list.top(), std::size_t{48});
    SAME(*list.current, std::size_t{48});
    SAME(shown.scene.row(0), std::string{"r51         "});

    Shown empty{0};

    empty.rows->insert(0, 3);
    empty.scene.wait(0ms);

    SAME(*empty.list->current, std::size_t{0});
    OUGHT(empty.scene.at({0, 0}).look.reverse);

    empty.scene.press(Key{Code::down});

    OUGHT(!empty.scene.at({0, 0}).look.reverse);
    OUGHT(empty.scene.at({0, 1}).look.reverse);
}

// Rows going near the end pull the view back to stay full, and the rows are painted where they now
// stand, though the current row stays where it was; rows going in the view move up the rows under
// them.
TRIAL("List: the view pulled back at the end paints its rows") {
    Shown shown{20};
    List& list = *shown.list;

    list.top(15);
    list.current = 16;
    shown.scene.wait(0ms);
    shown.rows->remove(18, 2);
    shown.scene.wait(0ms);

    SAME(list.top(), std::size_t{13});
    SAME(shown.scene.row(0), std::string{"r13         "});
    SAME(shown.scene.row(4), std::string{"r17         "});

    Shown longer{30};

    longer.list->top(10);
    longer.list->current = 11;
    longer.scene.wait(0ms);
    longer.rows->remove(12, 1);
    longer.scene.wait(0ms);

    SAME(longer.list->top(), std::size_t{10});
    SAME(longer.scene.row(2), std::string{"r13         "});
    SAME(longer.scene.row(4), std::string{"r15         "});
}

// A view's hook gone, the model no longer calls it; a node painted again in part paints nothing
// beyond its box.
TRIAL("Model and Node: a hook gone hears nothing, a part painted stays in the box") {
    // A node of one row that paints parts of itself again on demand.
    class Patch : public Node {
    public:
        // Paints `part` again.
        void dab(Area part) { repaint(part); }

    protected:
        // Ten cells of one row.
        Size measure(tal::Limits room) const override {
            (void)room;

            return Size{10, 1};
        }
    };

    Rows rows{3};
    int heard = 0;
    tal::Hook hook = rows.on<Changes>([&heard](Changes&) { ++heard; });

    rows.change(0, "a");
    hook = tal::Hook{};
    rows.change(0, "b");

    SAME(heard, 1);

    Scene scene{{.screen = {10, 3}}};
    Document document{scene.time()};
    tal::Column& column = document.children().add<tal::Column>();
    Patch& first = column.children().add<Patch>();

    (void)column.children().add<Patch>();
    scene.show(document);
    first.dab(Area{{0, 0}, {10, 3}});
    (void)document.frame();

    SAME(document.tally().painted, std::uint64_t{3});
}


// A new size keeps the view inside the rows and the current row in it, and tells the bar, which
// shows the new window though its own size stayed.
TRIAL("List: a new size keeps the view in the rows and the current row in the view") {
    Shown shown{20};
    List& list = *shown.list;

    shown.scene.press(Key{Code::end});
    shown.scene.resize(Size{12, 10});

    SAME(list.top(), std::size_t{10});
    SAME(shown.scene.row(0), std::string{"r10         "});

    shown.scene.press(Key{Code::home});
    shown.scene.wheel({1, 1}, -2);

    SAME(list.top(), std::size_t{6});

    shown.scene.resize(Size{12, 8});

    SAME(list.top(), std::size_t{0});

    list.current = 7;
    shown.scene.resize(Size{12, 4});

    SAME(list.top(), std::size_t{4});

    Scene scene{{.screen = {8, 10}}};
    Document document{scene.time()};
    Row& row = document.children().add<Row>();
    tal::Column& column = row.children().add<tal::Column>();
    tal::Label& label = column.children().add<tal::Label>("top");
    List& other = column.children().add<List>(std::make_shared<Rows>(18));

    column.share(Share{1, 0});
    other.share(Share{1, 0});
    (void)row.children().add<Scroll>(other);
    scene.show(document);

    SAME(scene.at({7, 4}).cluster, std::string{"\xe2\x96\x88"});
    SAME(scene.at({7, 5}).cluster, std::string{"\xe2\x94\x82"});

    label.text = "top\nmore\nmore\nmore\nmore";
    scene.wait(0ms);

    SAME(scene.at({7, 1}).cluster, std::string{"\xe2\x96\x88"});
    SAME(scene.at({7, 2}).cluster, std::string{"\xe2\x94\x82"});
}

// A new size of the bar takes where the thumb stands then: rows coming later that bring the thumb
// back to where it stood before the new size paint it there.
TRIAL("Scroll: a new size, then rows bringing the thumb back") {
    Scene scene{{.screen = {8, 9}}};
    Document document{scene.time()};
    auto rows = std::make_shared<Rows>(40);
    Row& row = document.children().add<Row>();
    List& list = row.children().add<List>(rows);

    list.share(Share{1, 0});
    (void)row.children().add<Scroll>(list);
    scene.show(document);
    scene.resize(Size{8, 11});

    SAME(scene.at({7, 2}).cluster, std::string{"\xe2\x96\x88"});

    rows->insert(40, 20);
    scene.wait(0ms);

    SAME(scene.at({7, 1}).cluster, std::string{"\xe2\x96\x88"});
    SAME(scene.at({7, 2}).cluster, std::string{"\xe2\x94\x82"});
}

}

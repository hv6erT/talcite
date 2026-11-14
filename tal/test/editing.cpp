#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/model.hpp"
#include "tal/undo.hpp"

#include "tal/testing.hpp"

// Text in lines that tells its views what changed, searching in a line, and the history of steps
// that undoes and redoes them.

namespace {

using tal::Changes;
using tal::Comb;
using tal::Edited;
using tal::Hit;
using tal::Place;
using tal::Sift;
using tal::Step;
using tal::Text;
using tal::Undo;

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

// What a text tells its views, as (kind, from, count).
struct Heard {
    std::vector<std::tuple<Changes::Kind, std::size_t, std::size_t>> told;
    tal::Hook hook;

    // Listens to `text`.
    explicit Heard(Text& text) : hook(text.on<Changes>([this](Changes& what) { told.emplace_back(what.kind, what.from, what.count); })) {}
};

// A text splits at \n, and at \r\n as at \n; an empty one is one empty line. Its lines are a
// model's rows, and whole joins them again.
TRIAL("Text: lines from the whole, as rows of a model") {
    const Text text{"one\r\ntwo\n\nfour\r"};

    SAME(text.lines(), std::size_t{4});
    SAME(text.line(0), std::string_view{"one"});
    SAME(text.line(2), std::string_view{});
    SAME(text.line(3), std::string_view{"four\r"});
    SAME(text.whole(), std::string{"one\ntwo\n\nfour\r"});
    SAME(text.rows(), std::size_t{4});
    SAME(text.entry(1, 5).text, std::string{"two"});
    OUGHT(throws<std::out_of_range>([&text] { (void)text.line(4); }));

    const Text empty;

    SAME(empty.lines(), std::size_t{1});
    SAME(empty.whole(), std::string{});
}

// Text within a line, and across lines joined by \n; a place outside the text, inside a character
// or an end before its start throws.
TRIAL("Text: what lies between two places") {
    const Text text{"ab\xc5\xbc\ncd\nef"};

    SAME(text.between(Place{0, 1}, Place{0, 4}), std::string{"b\xc5\xbc"});
    SAME(text.between(Place{0, 0}, Place{0, 1}), std::string{"a"});
    SAME(text.between(Place{0, 2}, Place{2, 1}), std::string{"\xc5\xbc\ncd\ne"});
    SAME(text.between(Place{1, 2}, Place{2, 0}), std::string{"\n"});
    OUGHT(throws<std::out_of_range>([&text] { (void)text.between(Place{0, 3}, Place{1, 0}); }));
    OUGHT(throws<std::out_of_range>([&text] { (void)text.between(Place{0, 0}, Place{3, 0}); }));
    OUGHT(throws<std::out_of_range>([&text] { (void)text.between(Place{0, 0}, Place{1, 3}); }));
    OUGHT(throws<std::invalid_argument>([&text] { (void)text.between(Place{1, 1}, Place{1, 0}); }));
}

// Inserting within a line changes that line; text with line ends breaks it, and the lines after it
// are told as inserted. The place where the insertion ends comes back.
TRIAL("Text: inserting changes a line and inserts the lines after it") {
    Text text{"hello\nworld"};
    Heard heard{text};

    OUGHT(text.insert(Place{0, 5}, ",") == (Place{0, 6}));
    OUGHT(text.insert(Place{1, 0}, "big\r\nnew\n") == (Place{3, 0}));
    SAME(text.whole(), std::string{"hello,\nbig\nnew\nworld"});
    OUGHT(text.insert(Place{3, 5}, "") == (Place{3, 5}));
    OUGHT(text.insert(Place{3, 5}, "\nnew") == (Place{4, 3}));
    OUGHT(heard.told == (std::vector<std::tuple<Changes::Kind, std::size_t, std::size_t>>{
                            {Changes::Kind::changed, 0, 1}, {Changes::Kind::changed, 1, 1}, {Changes::Kind::inserted, 2, 2}, {Changes::Kind::changed, 3, 1},
                            {Changes::Kind::changed, 3, 1}, {Changes::Kind::inserted, 4, 1}}));
    OUGHT(throws<std::out_of_range>([&text] { (void)text.insert(Place{0, 7}, "x"); }));
    OUGHT(throws<std::out_of_range>([&text] { (void)text.insert(Place{5, 0}, "x"); }));
}

// Erasing within a line changes it; across lines it joins the first and the last and the lines
// between are told as removed. An empty stretch changes nothing.
TRIAL("Text: erasing joins lines and removes those between") {
    Text text{"one\ntwo\nthree\nfour"};
    Heard heard{text};

    text.erase(Place{1, 1}, Place{1, 2});
    text.erase(Place{0, 2}, Place{2, 3});
    text.erase(Place{0, 1}, Place{0, 1});

    SAME(text.whole(), std::string{"onee\nfour"});
    OUGHT(heard.told == (std::vector<std::tuple<Changes::Kind, std::size_t, std::size_t>>{
                            {Changes::Kind::changed, 1, 1}, {Changes::Kind::changed, 0, 1}, {Changes::Kind::removed, 1, 2}}));
    OUGHT(throws<std::invalid_argument>([&text] { text.erase(Place{1, 2}, Place{0, 0}); }));
    OUGHT(throws<std::out_of_range>([&text] { text.erase(Place{0, 0}, Place{1, 9}); }));

    text.erase(Place{0, 4}, Place{1, 0});

    SAME(text.whole(), std::string{"oneefour"});
    SAME(text.lines(), std::size_t{1});
}

// After the views, a change tells where it was: the stretch it replaced and where what came ends,
// once, the lines it inserted or removed told already. A hook that went hears nothing, nor does an
// empty erasing tell anything; an empty insertion tells where it was.
TRIAL("Text: a change tells where it was") {
    Text text{"one\ntwo"};
    std::vector<std::tuple<Place, Place, Place, std::size_t>> heard;
    tal::Hook hook = text.on<Edited>([&heard, &text](Edited& what) { heard.emplace_back(what.from, what.upto, what.end, text.lines()); });
    tal::Hook other = text.on<Changes>([](Changes& what) { (void)what; });

    (void)text.insert(Place{0, 3}, "!");
    (void)text.insert(Place{1, 1}, "x\ny\nz");
    text.erase(Place{0, 2}, Place{2, 1});
    text.erase(Place{0, 1}, Place{0, 1});
    (void)text.insert(Place{0, 0}, "");

    SAME(text.whole(), std::string{"on\nzwo"});
    OUGHT(heard == (std::vector<std::tuple<Place, Place, Place, std::size_t>>{{Place{0, 3}, Place{0, 3}, Place{0, 4}, 2},
                                                                            {Place{1, 1}, Place{1, 1}, Place{3, 1}, 4},
                                                                            {Place{0, 2}, Place{2, 1}, Place{0, 2}, 2},
                                                                            {Place{0, 0}, Place{0, 0}, Place{0, 0}, 2}}));

    hook = tal::Hook{};
    (void)text.insert(Place{0, 0}, "a");

    SAME(heard.size(), std::size_t{4});
}

// Plain text is found character by character, with letter case or folded (Unicode too), as a whole
// word when asked; hits stand on the borders of characters.
TRIAL("Comb: plain text, letter case and whole words") {
    const Comb cased{"Ab", Sift{false, true, false}};
    const Comb folded{"ab", Sift{}};
    const Comb words{"cat", Sift{false, false, true}};
    const Comb polish{"\xc5\xbc\xc3\xb3\xc5\x82w", Sift{}};

    OUGHT(cased.forth("xab Ab", 0) == (std::optional<Hit>{Hit{4, 6}}));
    OUGHT(folded.forth("xAB", 0) == (std::optional<Hit>{Hit{1, 3}}));
    OUGHT(!folded.forth("xAB", 2).has_value());
    OUGHT(words.forth("concat cat", 0) == (std::optional<Hit>{Hit{7, 10}}));
    OUGHT(!words.forth("cats", 0).has_value());
    OUGHT(!words.forth("\xc5\xbc" "cat", 0).has_value());
    OUGHT(words.forth("(cat)", 0) == (std::optional<Hit>{Hit{1, 4}}));
    OUGHT(polish.forth("Ma \xc5\xbb\xc3\x93\xc5\x81W!", 0) == (std::optional<Hit>{Hit{3, 10}}));
    OUGHT(!folded.forth("a", 0).has_value());
    OUGHT(!folded.forth("ab", 9).has_value());
}

// An expression finds its first non-empty match from a place, with "^" only at the start of the
// line; hits that cut a character are passed over. Backwards it is the last hit that begins before
// a place, sought from every start, so that a shorter one inside a longer may win; all hits do not
// overlap.
TRIAL("Comb: expressions, backwards and all hits") {
    const Comb digits{"[0-9]+", Sift{true, false, false}};
    const Comb start{"^a", Sift{true, false, false}};
    const Comb starred{"x*", Sift{true, false, false}};
    const Comb any{".", Sift{true, true, false}};
    const Comb upper{"B", Sift{true, false, false}};

    OUGHT(digits.forth("ab 12 345", 0) == (std::optional<Hit>{Hit{3, 5}}));
    OUGHT(digits.forth("ab 12 345", 4) == (std::optional<Hit>{Hit{4, 5}}));
    OUGHT(!start.forth("ba", 1).has_value());
    OUGHT(start.forth("ab", 0) == (std::optional<Hit>{Hit{0, 1}}));
    OUGHT(starred.forth("abxx", 0) == (std::optional<Hit>{Hit{2, 4}}));
    OUGHT(any.forth("\xc5\xbc" "a", 0) == (std::optional<Hit>{Hit{2, 3}}));
    OUGHT(upper.forth("ab", 0) == (std::optional<Hit>{Hit{1, 2}}));
    OUGHT(digits.back("1 22 333", 5) == (std::optional<Hit>{Hit{3, 4}}));
    OUGHT(digits.back("1 22 333", 3) == (std::optional<Hit>{Hit{2, 4}}));
    OUGHT(!digits.back("1 22", 0).has_value());
    OUGHT(digits.all("1 22 333") == (std::vector<Hit>{{0, 1}, {2, 4}, {5, 8}}));
    OUGHT(Comb("aa", Sift{}).all("aaaaa") == (std::vector<Hit>{{0, 2}, {2, 4}}));
}

// An empty pattern and a broken expression make a comb that finds nothing and says why.
TRIAL("Comb: an empty pattern and a broken expression") {
    const Comb empty{"", Sift{}};
    const Comb broken{"[", Sift{true, false, false}};
    const Comb fine{"[", Sift{}};

    OUGHT(!empty.ready());
    OUGHT(!empty.wrong().empty());
    OUGHT(!broken.ready());
    OUGHT(!broken.forth("[", 0).has_value());
    OUGHT(broken.all("[").empty());
    OUGHT(fine.ready());
    OUGHT(fine.forth("a[", 0) == (std::optional<Hit>{Hit{1, 2}}));
    SAME(fine.what(), std::string{"["});
    OUGHT(fine.how() == Sift{});
}

// A step that appends a letter to a string, joining the next one unless that is a space.
class Typed : public Step {
public:
    Typed(std::string& into, char one) : into_(into), letters_(1, one) { into_ += one; }

    // Takes its letters away.
    void undo() override { into_.resize(into_.size() - letters_.size()); }

    // Appends them again.
    void redo() override { into_ += letters_; }

    // What it did.
    std::string said() const override { return "Typing " + letters_; }

    // Joins a later letter that is not a space.
    bool merged(const Step& later) override {
        const auto* typed = dynamic_cast<const Typed*>(&later);

        if (typed == nullptr || typed->letters_ == " ") return false;

        letters_ += typed->letters_;

        return true;
    }

private:
    std::string& into_;
    std::string letters_;
};

// A step of another kind, which joins nothing.
class Plain : public Step {
public:
    // Nothing.
    void undo() override {}

    // Nothing.
    void redo() override {}

    // Its name.
    std::string said() const override { return "Plain"; }
};

// Steps done one after another join while the last takes them; undo and redo go back and forth, and
// a new step cuts off what waited to be redone. After an undo, a redo or a seal the next step
// stands alone. The last step is the one undo would take back, and it lives on after it.
TRIAL("Undo: steps joined, undone, redone and cut off") {
    std::string text;
    Undo undo;

    OUGHT(!undo.undo());
    OUGHT(!undo.redo());
    OUGHT(!undo.undoing().has_value());
    OUGHT(undo.last() == nullptr);

    undo.push(std::make_unique<Typed>(text, 'a'));
    undo.push(std::make_unique<Typed>(text, 'b'));
    undo.push(std::make_unique<Typed>(text, ' '));
    undo.push(std::make_unique<Typed>(text, 'c'));

    SAME(text, std::string{"ab c"});
    SAME(*undo.undoing(), std::string{"Typing  c"});

    const tal::Step* top = undo.last();

    OUGHT(top != nullptr);
    SAME(top->said(), std::string{"Typing  c"});
    OUGHT(undo.undo());

    SAME(text, std::string{"ab"});
    SAME(top->said(), std::string{"Typing  c"});
    SAME(undo.last()->said(), std::string{"Typing ab"});
    SAME(*undo.redoing(), std::string{"Typing  c"});

    OUGHT(undo.redo());
    OUGHT(undo.undo());
    OUGHT(undo.undo());

    SAME(text, std::string{});
    OUGHT(!undo.undo());

    OUGHT(undo.redo());

    undo.push(std::make_unique<Typed>(text, 'x'));

    SAME(text, std::string{"abx"});
    OUGHT(!undo.redo());
    OUGHT(undo.undo());
    SAME(text, std::string{"ab"});

    undo.push(std::make_unique<Typed>(text, 'y'));
    undo.seal();
    undo.push(std::make_unique<Typed>(text, 'z'));
    undo.push(std::make_unique<Plain>());

    OUGHT(undo.undo());
    OUGHT(undo.undo());

    SAME(text, std::string{"aby"});

    undo.push(std::make_unique<Typed>(text, 'w'));

    OUGHT(undo.undo());
    SAME(text, std::string{"aby"});

    undo.clear();

    OUGHT(!undo.undo());
    OUGHT(!undo.redo());
    OUGHT(throws<std::invalid_argument>([&undo] { undo.push(nullptr); }));
}

// The history keeps at most the steps it was made for, the oldest going first; a step of its own
// kind joins nothing.
TRIAL("Undo: a limit on the steps kept") {
    std::string text;
    Undo undo{2};

    for (const char one : std::string{"abc"}) {
        undo.push(std::make_unique<Typed>(text, one));
        undo.seal();
    }

    OUGHT(undo.undo());
    OUGHT(undo.undo());
    OUGHT(!undo.undo());
    SAME(text, std::string{"a"});

    Plain plain;
    Plain other;

    OUGHT(!plain.merged(other));

    Undo one{0};

    one.push(std::make_unique<Plain>());
    one.push(std::make_unique<Plain>());

    OUGHT(one.undo());
    OUGHT(!one.undo());
}

}

// A new history counts as saved. A step makes it changed and undoing it saved again; a save marks
// the place and seals it, so that typing on stands apart. A save undone past and replaced by a new
// step can no longer be reached; the oldest steps falling off keep a save after them, but one
// before them goes with them. Clearing saves.
TRIAL("Undo: the saved place") {
    std::string text;
    Undo undo{3};

    OUGHT(!undo.changed());

    undo.push(std::make_unique<Typed>(text, 'a'));

    OUGHT(undo.changed());
    OUGHT(undo.undo());
    OUGHT(!undo.changed());
    OUGHT(undo.redo());
    OUGHT(undo.changed());

    undo.saved();

    OUGHT(!undo.changed());

    undo.push(std::make_unique<Typed>(text, 'b'));

    SAME(*undo.undoing(), std::string{"Typing b"});
    OUGHT(undo.changed());
    OUGHT(undo.undo());
    OUGHT(!undo.changed());
    SAME(text, std::string{"a"});

    OUGHT(undo.undo());

    undo.push(std::make_unique<Typed>(text, 'c'));
    undo.push(std::make_unique<Typed>(text, ' '));

    OUGHT(undo.changed());
    OUGHT(undo.undo());
    OUGHT(undo.changed());

    undo.saved();

    for (const char one : std::string{"def"}) {
        undo.push(std::make_unique<Typed>(text, one));
        undo.seal();
    }

    OUGHT(undo.undo());
    OUGHT(undo.undo());
    OUGHT(undo.changed());
    OUGHT(undo.undo());
    OUGHT(!undo.changed());
    OUGHT(!undo.undo());

    undo.clear();

    for (const char one : std::string{"ghij"}) {
        undo.push(std::make_unique<Typed>(text, one));
        undo.seal();
    }

    OUGHT(undo.undo());
    OUGHT(undo.undo());
    OUGHT(undo.undo());
    OUGHT(!undo.undo());
    OUGHT(undo.changed());

    undo.clear();

    OUGHT(!undo.changed());

    undo.push(std::make_unique<Typed>(text, 'k'));
    undo.saved();
    undo.push(std::make_unique<Typed>(text, 'l'));

    OUGHT(undo.changed());
    OUGHT(undo.undo());
    OUGHT(!undo.changed());
}

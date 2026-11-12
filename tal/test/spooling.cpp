#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/editing.hpp"

#include "spooling.hpp"
#include "tal/testing.hpp"

// The spool: a byte document as lines, given whole or read in windows that its owner fetches when
// it orders them; here a tape plays the owner. And the lines of a text, following its changes.

namespace {

using tal::Bytes;
using tal::Edited;
using tal::Place;
using tal::Text;
using tal::spooling::Lines;
using tal::spooling::Offset;
using tal::spooling::Reel;
using tal::spooling::Spool;

// A document read in windows: it fetches what the spool orders, `give` bytes at most at a time, and
// counts the windows; past `refusing` it gives nothing, as a source that failed.
class Tape {
public:
    explicit Tape(std::string what, std::size_t give = SIZE_MAX) : raw_(tal::bytes(what)), give_(give) {}

    // How many bytes the document has.
    [[nodiscard]] Offset whole() const { return raw_.size(); }

    // How many windows were read.
    [[nodiscard]] int calls() const { return calls_; }

    // How many bytes were read.
    [[nodiscard]] Offset read() const { return read_; }

    // Gives nothing for windows from `from` on.
    void refuse(Offset from) { refusing_ = from; }

    // Reads the window the spool ordered, if it ordered one, and gives it back; tells whether it
    // did.
    bool served(Spool& one) {
        const std::optional<Spool::Order> order = one.ordered();

        if (!order) return false;

        ++calls_;

        Bytes made;
        const std::size_t much = std::min(order->most, give_);

        for (Offset at = order->from; order->from < refusing_ && at < raw_.size() && at < order->from + much; ++at)
            made.push_back(raw_[static_cast<std::size_t>(at)]);

        read_ += made.size();
        one.arrived(order->from, std::move(made));

        return true;
    }

private:
    Bytes raw_;
    std::size_t give_;
    Offset refusing_ = UINT64_MAX;
    int calls_ = 0;
    Offset read_ = 0;
};

// Up to `most` lines from `which`, read as a page reads them: the extent of each, then its bytes in
// pieces. None while a window is on its way; fewer at the end of the document.
std::optional<std::vector<std::string>> texts(Spool& one, std::size_t which, std::size_t most) {
    std::vector<std::string> made;

    for (std::size_t line = which; line < which + most; ++line) {
        const std::optional<Spool::Line> got = one.line(line);

        if (!got) {
            if (one.stalled()) return std::nullopt;

            break;
        }

        std::string text;

        for (Offset at = got->at; at < got->upto;) {
            const std::optional<std::string_view> piece = one.read(at, static_cast<std::size_t>(got->upto - at));

            if (!piece) return std::nullopt;

            text += *piece;
            at += piece->size();
        }

        made.push_back(std::move(text));
    }

    return made;
}

// Asks `ask` until the spool stops waiting, the tape serving the windows between; none when the
// spool waits with nothing ordered.
template <typename Ask>
auto settled(Tape& tape, Spool& one, Ask ask) {
    auto got = ask();

    while (one.stalled() && tape.served(one)) got = ask();

    return got;
}

// The lines from `which`, `most` at most, once the spool has them.
std::vector<std::string> rolled(Tape& tape, Spool& one, std::size_t which, std::size_t most) {
    return settled(tape, one, [&] { return texts(one, which, most); }).value_or(std::vector<std::string>{});
}

// The byte where the line `which` starts, once the spool knows it.
std::optional<Offset> placed(Tape& tape, Spool& one, std::size_t which) {
    return settled(tape, one, [&] { return one.at(which); });
}

// A document of `much` numbered lines, each `said` and its number.
std::string many(int much, const std::string& said = "line ") {
    std::string made;

    for (int one = 0; one < much; ++one) made += said + std::to_string(one) + "\n";

    return made;
}

// The byte where the line `line` of `doc` starts.
Offset started(const std::string& doc, std::size_t line) {
    std::size_t at = 0;

    for (std::size_t one = 0; one < line; ++one) at = doc.find('\n', at) + 1;

    return at;
}

}

// A document given whole is counted at once: its lines without their ends (\n, \r\n), no empty line
// after a final \n, none in an empty document.
TRIAL("Spool: a document at hand, its lines and their starts") {
    Reel one{tal::bytes("a\nbb\r\nccc")};

    SAME(one.counted(), std::optional<std::size_t>{3});
    SAME(one.whole(), Offset{9});
    OUGHT(*texts(one, 0, 10) == (std::vector<std::string>{"a", "bb", "ccc"}));
    SAME(one.at(1), std::optional<Offset>{2});
    SAME(one.at(2), std::optional<Offset>{6});
    OUGHT(!one.at(3));
    OUGHT(one.line(1) == (Spool::Line{2, 4, 6}));
    OUGHT(!one.ordered());

    Reel ended{tal::bytes("a\nb\n")};

    SAME(ended.counted(), std::optional<std::size_t>{2});

    Reel empty{Bytes{}};

    SAME(empty.counted(), std::optional<std::size_t>{0});
    OUGHT(texts(empty, 0, 5)->empty());
    OUGHT(!empty.at(0));
    SAME(empty.found(5), std::size_t{0});
}

// A document without a line end is one line however long, and any piece of it reads without the
// rest.
TRIAL("Spool: one line of megabytes") {
    const std::string said(3U * 1024 * 1024 + 7, 'z');
    Reel one{tal::bytes(said)};

    SAME(one.counted(), std::optional<std::size_t>{1});
    OUGHT(one.line(0) == (Spool::Line{0, said.size(), said.size()}));
    SAME(*one.read(2U * 1024 * 1024, 5), std::string_view{"zzzzz"});
    SAME(one.read(said.size() - 3, 100)->size(), std::size_t{3});
    SAME(one.read(said.size(), 100)->size(), std::size_t{0});
    SAME(one.read(0, 1000000)->size(), std::size_t{64 * 1024});
    SAME(one.found(2U * 1024 * 1024), std::size_t{0});
}

// The line holding a byte, the last line past the end; both ways agree along the document.
TRIAL("Spool: the line of a byte") {
    Reel one{tal::bytes("a\nbb\nccc")};

    SAME(one.found(0), std::size_t{0});
    SAME(one.found(1), std::size_t{0});
    SAME(one.found(2), std::size_t{1});
    SAME(one.found(4), std::size_t{1});
    SAME(one.found(5), std::size_t{2});
    SAME(one.found(1000), std::size_t{2});

    Reel long_one{tal::bytes(many(5000))};

    for (std::size_t which = 0; which < 5000; which += 137) SAME(long_one.found(*long_one.at(which)), which);
}

// A document read in windows does not know its count of lines before its end. The first question
// orders a window and waits; one window goes at a time, and a second question orders nothing more.
TRIAL("Spool: the first question orders a window") {
    Tape tape{many(50)};
    Reel one{tape.whole(), {}};

    OUGHT(!one.counted());
    OUGHT(!texts(one, 0, 5));
    OUGHT(one.stalled());
    OUGHT(one.ordered() == (Spool::Order{0, 64 * 1024}));
    OUGHT(!one.ordered());
    OUGHT(!texts(one, 0, 5));
    OUGHT(!one.ordered());

    one.arrived(0, tal::bytes(many(50)));

    const std::vector<std::string> got = *texts(one, 0, 5);

    SAME(got.front(), std::string{"line 0"});
    OUGHT(!one.stalled());
    SAME(one.counted(), std::optional<std::size_t>{});
    SAME(rolled(tape, one, 49, 2), (std::vector<std::string>{"line 49"}));
    SAME(one.counted(), std::optional<std::size_t>{50});
}

// The head already read answers at once, and the rest is ordered ahead from its end; a head longer
// than the document is cut to it.
TRIAL("Spool: a head at hand") {
    const std::string doc = many(5000);
    Tape tape{doc};
    Reel one{tape.whole(), tal::bytes(doc.substr(0, 100))};

    SAME(*one.read(0, 10, 1), std::string_view{"line 0\nlin"});
    SAME(*one.read(95, 1000, 1), std::string_view{doc.substr(95, 5)});
    OUGHT(!one.stalled());
    OUGHT(one.ordered() == (Spool::Order{100, 64 * 1024}));
    OUGHT(!one.read(95, 1000));
    OUGHT(one.stalled());
    OUGHT(!one.ordered());

    Reel cut{3, tal::bytes("abcdef")};

    SAME(*cut.read(0, 10), std::string_view{"abc"});
    SAME(cut.counted(), std::optional<std::size_t>{});
}

// Counting walks to the end window by window, each window taking it further; then the last line is
// at hand without a window more.
TRIAL("Spool: counting to the end") {
    Tape tape{many(30000)};
    Reel one{tape.whole(), {}};

    one.counting();

    while (!one.counted() && tape.served(one)) {
    }

    SAME(one.counted(), std::optional<std::size_t>{30000});
    OUGHT(tape.calls() > 1);

    const int calls = tape.calls();

    SAME(rolled(tape, one, 29999, 1), (std::vector<std::string>{"line 29999"}));
    SAME(tape.calls(), calls);

    one.counting();
    one.counted(5);

    SAME(one.counted(), std::optional<std::size_t>{30000});
}

// Jumps forward and back give what the document at hand gives; stepping back finds every line.
TRIAL("Spool: jumps and steps back as in a document at hand") {
    const std::string said = many(20000);
    Tape tape{said};
    Reel one{tape.whole(), {}};
    Reel known{tal::bytes(said)};

    one.screen(40);

    for (const std::size_t which : {0U, 15000U, 14999U, 1U, 19999U, 19000U}) {
        SAME(rolled(tape, one, which, 2), *texts(known, which, 2));
        SAME(*placed(tape, one, which), *known.at(which));
    }

    Tape short_tape{many(3000, "")};
    Reel steps{short_tape.whole(), {}};

    (void)rolled(short_tape, steps, 2999, 1);

    for (int which = 2999; which >= 0; --which) SAME(rolled(short_tape, steps, static_cast<std::size_t>(which), 1), (std::vector<std::string>{std::to_string(which)}));
}

// Stepping through what one window holds reads it once; a row near the end of the window orders the
// next one before anyone asks, so that a row in it then answers at once.
TRIAL("Spool: the window serves the lines around, and the next one comes ahead") {
    Tape near{many(100)};
    Reel few{near.whole(), {}};

    for (std::size_t which = 0; which < 100; ++which) (void)rolled(near, few, which, 1);

    SAME(near.calls(), 1);

    Tape tape{many(20000)};
    Reel one{tape.whole(), {}};

    (void)rolled(tape, one, 0, 40);

    SAME(tape.calls(), 1);
    OUGHT(!tape.served(one));

    (void)rolled(tape, one, 4000, 40);

    SAME(tape.calls(), 1);
    OUGHT(tape.served(one));

    const std::optional<std::vector<std::string>> got = texts(one, 6800, 40);

    OUGHT(got.has_value());
    SAME(got->front(), std::string{"line 6800"});
    OUGHT(!one.stalled());
    SAME(tape.calls(), 2);
}

// A source giving less than asked, a hundred bytes at a time, does not break stepping back.
TRIAL("Spool: a source giving little at a time") {
    Tape tape{many(4000), 100};
    Reel one{tape.whole(), {}};

    (void)rolled(tape, one, 3999, 1);

    for (int which = 3999; which >= 3900; --which) SAME(rolled(tape, one, static_cast<std::size_t>(which), 1), (std::vector<std::string>{"line " + std::to_string(which)}));
}

// A source that gives nothing ends the document where it got, and nothing is ordered after.
TRIAL("Spool: a source that fails ends the document") {
    Tape tape{many(20000)};
    Reel one{tape.whole(), {}};

    tape.refuse(1);

    SAME(rolled(tape, one, 0, 5).size(), std::size_t{5});
    OUGHT(rolled(tape, one, 19000, 5).empty());
    OUGHT(one.counted().has_value());
    OUGHT(*one.counted() < 20000);
    OUGHT(one.whole() < tape.whole());

    const int calls = tape.calls();

    OUGHT(rolled(tape, one, 19000, 5).empty());
    (void)rolled(tape, one, 2000, 5);

    SAME(tape.calls(), calls);
}

// A jump far from where the spool stands starts from the nearest anchor, by line and by byte.
TRIAL("Spool: a far jump from the nearest anchor") {
    const std::string doc = many(60000);
    Tape tape{doc};
    Reel one{tape.whole(), {}};

    OUGHT(placed(tape, one, 59000).has_value());

    int calls = tape.calls();

    SAME(placed(tape, one, 35000), std::optional<Offset>{started(doc, 35000)});
    OUGHT(tape.calls() - calls <= 2);

    OUGHT(placed(tape, one, 59000).has_value());

    calls = tape.calls();

    SAME(settled(tape, one, [&] { return one.found(started(doc, 30000) + 3); }), std::size_t{30000});
    OUGHT(tape.calls() - calls <= 2);
}

// A line far longer than the window comes through once, window after window; stepping back over it
// and finding the line of a byte deep in it read only what they need.
TRIAL("Spool: lines longer than the window") {
    const std::size_t mega = 2U * 1024 * 1024;
    Tape tape{std::string(mega, 'x') + "\nend\n"};
    Reel one{tape.whole(), {}};
    const std::optional<Spool::Line> first = settled(tape, one, [&] { return one.line(0); });

    OUGHT(first == (Spool::Line{0, mega, mega + 1}));
    OUGHT(tape.calls() <= 2 * 1024 / 64 + 4);
    SAME(rolled(tape, one, 1, 1), (std::vector<std::string>{"end"}));

    Tape back{"a\n" + std::string(mega, 'y') + "\nb\n"};
    Reel behind{back.whole(), {}};

    SAME(*placed(back, behind, 2), Offset{mega + 3});

    const int was = back.calls();

    behind.screen(1);

    SAME(*placed(back, behind, 1), Offset{2});
    OUGHT(back.calls() - was <= 2 * 1024 / 64 + 4);

    Tape deep{"a\n" + std::string(2 * mega, 'y') + "\nb\n"};
    Reel far{deep.whole(), {}};

    SAME(settled(deep, far, [&] { return far.found(2 + mega / 2); }), std::size_t{1});
    OUGHT(deep.calls() <= 1024 / 64 + 3);

    Tape piece{std::string(3 * mega / 2, 'q') + "!"};
    Reel apart{piece.whole(), {}};
    const std::optional<std::string_view> got = settled(piece, apart, [&] { return apart.read(3 * mega / 2 - 2, 10); });

    SAME(std::string{*got}, std::string{"qq!"});
    SAME(piece.calls(), 1);
}

namespace {

// Documents of every kind of line: short and empty ones, \r\n and a lone \r, lines longer than a
// window, with and without a final line end.
std::vector<std::string> kinds() {
    std::mt19937 random{7};
    std::string mixed;

    for (int line = 0; line < 400; ++line) {
        const auto much = static_cast<std::size_t>(random() % 7 == 0 ? 70000 + random() % 70000 : random() % 90);

        for (std::size_t at = 0; at < much; ++at) mixed.push_back(random() % 50 == 0 ? '\r' : static_cast<char>('a' + random() % 26));

        mixed += random() % 3 == 0 ? "\r\n" : "\n";
    }

    return {many(3000), mixed, mixed + "tail", std::string(300000, 'w'), "", "\n", "\r\n", "\r", "a\r", "\n\n\nx"};
}

}

// What a spool should answer for a document, worked out plainly: where its lines start, where their
// content ends (before \n, \r\n, or a \r ending the document), the line of a byte, its bytes.
struct Reference {
    explicit Reference(const std::string& doc) : text(doc) {
        for (std::size_t at = 0; at < doc.size();) {
            const std::size_t end = std::min(doc.find('\n', at), doc.size());
            const std::size_t upto = end > at && doc[end - 1] == '\r' ? end - 1 : end;
            const std::size_t next = std::min(end + 1, doc.size());

            lines.push_back(Spool::Line{at, upto, next});
            at = next;
        }
    }

    // The start of the line `which`.
    [[nodiscard]] std::optional<Offset> at(std::size_t which) const {
        if (which >= lines.size()) return std::nullopt;

        return lines[which].at;
    }

    // The extent of the line `which`.
    [[nodiscard]] std::optional<Spool::Line> line(std::size_t which) const {
        if (which >= lines.size()) return std::nullopt;

        return lines[which];
    }

    // The line holding the byte `byte`, the last past the end.
    [[nodiscard]] std::size_t found(Offset byte) const {
        if (text.empty()) return 0;

        const auto want = static_cast<std::ptrdiff_t>(std::min<Offset>(byte, text.size() - 1));

        return static_cast<std::size_t>(std::count(text.begin(), text.begin() + want, '\n'));
    }

    // The bytes from `at`, a window at most.
    [[nodiscard]] std::string read(Offset at, std::size_t much) const {
        if (at >= text.size()) return {};

        return text.substr(static_cast<std::size_t>(at), std::min({much, std::size_t{64 * 1024}, text.size() - static_cast<std::size_t>(at)}));
    }

    std::string text;
    std::vector<Spool::Line> lines;
};

// At hand or read in windows of any size, the spool answers every question as the document is:
// where lines start, their extents, the line of a byte, the bytes from a place, the count of lines.
TRIAL("Spool: the document as it is, whatever is asked in whatever order") {
    std::mt19937 random{11};

    for (const std::string& doc : kinds()) {
        const Reference known{doc};

        for (const std::size_t give : {std::size_t{0}, SIZE_MAX, std::size_t{100}, std::size_t{5000}}) {
            Tape tape{doc, give};
            Reel held{tal::bytes(doc)};
            Reel read{tape.whole(), {}};
            Spool& one = give == 0 ? held : read;

            SAME(held.counted(), std::optional<std::size_t>{known.lines.size()});

            for (int step = 0; step < 300; ++step) {
                const auto line = static_cast<std::size_t>(random() % (known.lines.size() + 3));
                const Offset byte = random() % (doc.size() + 5);

                switch (random() % 7) {
                    case 0: SAME(settled(tape, one, [&] { return one.at(line); }), known.at(line)); break;
                    case 1: OUGHT(settled(tape, one, [&] { return one.line(line); }) == known.line(line)); break;
                    case 2: SAME(settled(tape, one, [&] { return one.found(byte); }), known.found(byte)); break;
                    case 3: {
                        const std::size_t much = 1 + random() % 70000;

                        SAME(std::string{*settled(tape, one, [&] { return one.read(byte, much); })}, known.read(byte, much));

                        break;
                    }
                    case 4: {
                        const std::string have{*settled(tape, one, [&] { return one.read(byte, 70000, 1); })};
                        const std::string whole = known.read(byte, 70000);

                        OUGHT(whole.starts_with(have) && have.empty() == whole.empty());

                        break;
                    }
                    case 5:
                        one.counting();

                        while (!one.counted() && tape.served(one)) {
                        }

                        SAME(one.counted(), std::optional<std::size_t>{known.lines.size()});

                        break;
                    default:
                        one.screen(static_cast<int>(random() % 100));

                        (void)tape.served(one);
                }

                if (one.counted()) SAME(*one.counted(), known.lines.size());
            }
        }
    }
}

// A walk through a document reads what it needs: forward and back line by line (their text read
// whole), jumps near and far, the line of a byte, the last line. Of a document of short lines that
// is each window once; the windows and bytes read are kept here, so that a change reading more
// shows.
TRIAL("Spool: the windows a walk reads") {
    std::vector<std::pair<int, Offset>> made;

    for (const std::string& doc : {many(20000), kinds()[1]}) {
        const Reference known{doc};
        const std::size_t lines = known.lines.size();
        Tape tape{doc};
        Reel one{tape.whole(), {}};

        one.screen(40);

        for (std::size_t which = 0; which < 300; ++which) (void)rolled(tape, one, which, 1);

        for (std::size_t which = 300; which-- > 0;) (void)rolled(tape, one, which, 1);

        (void)placed(tape, one, lines / 2);
        (void)placed(tape, one, lines / 2 - 3);
        (void)placed(tape, one, lines / 3);
        (void)settled(tape, one, [&] { return one.found(doc.size() / 4); });
        (void)settled(tape, one, [&] { return one.line(lines - 1); });

        for (std::size_t which = lines; which-- > lines - 50;) (void)rolled(tape, one, which, 1);

        (void)placed(tape, one, lines / 2 + 1);

        made.emplace_back(tape.calls(), tape.read());
    }

    OUGHT(made == (std::vector<std::pair<int, Offset>>{{4, 208890}, {374, 24451140}}));
}

// A \r ending one window and its \n starting the next end the line together; a last line of
// megabytes is walked once, and the lines are counted from it.
TRIAL("Spool: a line end across windows, and a long last line kept") {
    Tape tape{std::string(64 * 1024 - 1, 'a') + "\r\nb"};
    Reel one{tape.whole(), {}};

    OUGHT(settled(tape, one, [&] { return one.line(0); }) == (Spool::Line{0, 64 * 1024 - 1, 64 * 1024 + 1}));

    const std::size_t mega = 4U * 1024 * 1024;
    Tape last{"a\n" + std::string(mega, 'z')};
    Reel longer{last.whole(), {}};

    OUGHT(settled(last, longer, [&] { return longer.line(1); }) == (Spool::Line{2, mega + 2, mega + 2}));
    SAME(longer.counted(), std::optional<std::size_t>{2});

    const int calls = last.calls();

    OUGHT(longer.line(1).has_value());
    OUGHT(longer.line(0).has_value());
    SAME(last.calls(), calls);
}

// Stepping back over a line longer than the window reads it once: from what was walked already,
// nothing, also on the way to a line no longer kept; from far below, window by window backwards,
// the search going on where it stopped.
TRIAL("Spool: stepping back over a long line") {
    const std::size_t mega = 2U * 1024 * 1024;
    const std::string doc = "a\n" + std::string(mega, 'y') + "\n" + many(5000);
    const Reference known{doc};
    Tape tape{doc};
    Reel one{tape.whole(), {}};

    SAME(*placed(tape, one, 2), Offset{mega + 3});

    int calls = tape.calls();

    SAME(*placed(tape, one, 1), Offset{2});
    SAME(tape.calls(), calls);

    (void)placed(tape, one, 5001);

    for (std::size_t which = 5001; which-- > 2;) SAME(placed(tape, one, which), known.at(which));

    calls = tape.calls();

    SAME(*placed(tape, one, 1), Offset{2});
    OUGHT(tape.calls() - calls <= static_cast<int>(mega / (64 * 1024)) + 2);

    const std::string far = many(5000) + std::string(mega, 'q') + "\nend\n";
    const Reference known_far{far};
    Tape back{far};
    Reel steps{back.whole(), {}};

    steps.screen(10000);

    SAME(placed(back, steps, 5001), known_far.at(5001));

    calls = back.calls();

    SAME(placed(back, steps, 2900), known_far.at(2900));
    OUGHT(back.calls() - calls <= 3);

    const std::string middle = many(5000) + std::string(mega, 'm') + "\n" + many(5000);
    const Reference known_middle{middle};
    Tape up{middle};
    Reel over{up.whole(), {}};

    over.screen(1000000);

    SAME(placed(up, over, 10001), known_middle.at(10001));

    for (std::size_t which = 10001; which-- > 5001;) SAME(placed(up, over, which), known_middle.at(which));

    calls = up.calls();

    SAME(placed(up, over, 5000), known_middle.at(5000));
    OUGHT(up.calls() - calls <= static_cast<int>(mega / (64 * 1024)) + 3);
}

// A walk through a document larger than the windows kept: far jumps and near ones, steps back, the
// line of a byte, the last line and the count, with anchors as far apart as the screen asks. The
// windows and the bytes read are kept here, so that a change reading more shows.
TRIAL("Spool: the windows a walk through a large document reads") {
    std::vector<std::pair<int, Offset>> made;

    for (const std::string& doc : {many(400000), kinds()[1]}) {
        const Reference known{doc};
        const std::size_t lines = known.lines.size();
        Tape tape{doc};
        Reel one{tape.whole(), {}};

        one.screen(lines > 1000 ? 1000 : 3);

        for (const std::size_t which : {lines * 3 / 4, lines / 4, lines * 3 / 4 - 5, lines / 2, lines / 4 + 7, lines - 1, std::size_t{3}})
            SAME(placed(tape, one, which), known.at(which));

        for (std::size_t which = lines / 2; which-- > lines / 2 - 30;) SAME(placed(tape, one, which), known.at(which));

        SAME(settled(tape, one, [&] { return one.found(doc.size() / 3); }), known.found(doc.size() / 3));
        SAME(settled(tape, one, [&] { return one.found(doc.size() / 3 + 1000); }), known.found(doc.size() / 3 + 1000));
        OUGHT(settled(tape, one, [&] { return one.line(lines - 2); }) == known.line(lines - 2));
        SAME(placed(tape, one, lines / 3), known.at(lines / 3));

        made.emplace_back(tape.calls(), tape.read());
    }

    OUGHT(made == (std::vector<std::pair<int, Offset>>{{79, 5078624}, {86, 5621852}}));
}

// With anchors far apart and the lines walked long forgotten, the spool goes from where it got last
// and from the lines it keeps: back up a little from there, to the line it reached, to the line
// after, and the line of a byte there, without reading from the start again; far up it walks from
// the anchor instead of stepping back all the way.
TRIAL("Spool: going on from where it got") {
    const std::string doc = many(100000) + std::string(2U * 1024 * 1024, 'q') + "\nend\n";
    const Reference known{doc};
    Tape tape{doc};
    Reel one{tape.whole(), {}};

    one.screen(1000000);

    SAME(placed(tape, one, 100001), known.at(100001));

    int calls = tape.calls();

    SAME(placed(tape, one, 97000), known.at(97000));
    OUGHT(tape.calls() - calls <= 2);

    calls = tape.calls();

    SAME(placed(tape, one, 97000), known.at(97000));
    SAME(placed(tape, one, 97001), known.at(97001));
    SAME(settled(tape, one, [&] { return one.found(*known.at(97001) + 2); }), std::size_t{97001});
    SAME(tape.calls(), calls);

    SAME(placed(tape, one, 90000), known.at(90000));

    calls = tape.calls();

    SAME(settled(tape, one, [&] { return one.found(*known.at(100001) + 1); }), std::size_t{100001});
    SAME(tape.calls(), calls + 1);

    calls = tape.calls();

    SAME(placed(tape, one, 5000), known.at(5000));
    OUGHT(tape.calls() - calls <= 2);
}

// The lines of a text each end with a \n, the last one too, so that an empty last line is a line;
// the bytes are read across the ends of lines. Everything is at hand: nothing is ordered, nothing
// stalls, and the count of lines is the text's.
TRIAL("Lines: the lines of a text, each with its end") {
    const auto text = std::make_shared<Text>("a\nbb\n");
    Lines one{text};

    SAME(one.whole(), Offset{6});
    SAME(one.counted(), std::optional<std::size_t>{3});
    SAME(one.at(0), std::optional<Offset>{0});
    SAME(one.at(2), std::optional<Offset>{5});
    OUGHT(!one.at(3).has_value());
    OUGHT(one.line(1) == (Spool::Line{2, 4, 5}));
    OUGHT(one.line(2) == (Spool::Line{5, 5, 6}));
    OUGHT(!one.line(3).has_value());
    SAME(one.found(0), std::size_t{0});
    SAME(one.found(1), std::size_t{0});
    SAME(one.found(2), std::size_t{1});
    SAME(one.found(5), std::size_t{2});
    SAME(one.found(99), std::size_t{2});
    SAME(std::string{*one.read(0, 100)}, std::string{"a\nbb\n\n"});
    SAME(std::string{*one.read(3, 2, 1)}, std::string{"b\n"});
    SAME(std::string{*one.read(6, 5)}, std::string{});
    SAME(std::string{*one.read(9, 5)}, std::string{});
    OUGHT(!one.stalled());
    OUGHT(!one.ordered().has_value());

    one.counted(7);
    one.counting();
    one.screen(50);
    one.arrived(0, tal::bytes("x"));

    SAME(one.counted(), std::optional<std::size_t>{3});
    SAME(one.whole(), Offset{6});

    Lines empty{std::make_shared<Text>()};

    SAME(empty.whole(), Offset{1});
    SAME(empty.found(0), std::size_t{0});
    OUGHT(empty.line(0) == (Spool::Line{0, 0, 1}));
}

// Changes inside lines and changes of their number, at random, leave the lines telling what a
// document of the text's lines, each with its \n, tells.
TRIAL("Lines: following the changes of a text") {
    const auto text = std::make_shared<Text>("first\nsecond line\n\nlast");
    Lines one{text};
    tal::Hook hook = text->on<Edited>([&one](Edited& what) { one.edited(what.from.line, what.upto.line, what.end.line); });
    std::mt19937 random{7};
    const std::vector<std::string> pieces{"x", "yz", "\n", "a\nb", "", "\n\n", "long piece of text"};

    for (int step = 0; step < 300; ++step) {
        const auto pick = [&random](std::size_t much) { return static_cast<std::size_t>(random() % much); };
        const std::size_t line = pick(text->lines());
        const Place from{line, pick(text->line(line).size() + 1)};

        if (random() % 2 == 0) {
            (void)text->insert(from, pieces[pick(pieces.size())]);
        } else {
            const std::size_t last = std::min(text->lines() - 1, line + pick(3));
            Place upto{last, pick(text->line(last).size() + 1)};

            if (upto < from) upto = from;

            text->erase(from, upto);
        }

        const std::string whole = text->whole() + "\n";
        Reel known{tal::bytes(whole)};

        SAME(one.whole(), Offset{whole.size()});

        for (std::size_t which = 0; which < text->lines(); ++which) {
            OUGHT(one.line(which) == known.line(which));
            SAME(one.at(which), known.at(which));
        }

        for (Offset byte = 0; byte < whole.size(); byte += 3) SAME(one.found(byte), known.found(byte));

        SAME(std::string{*one.read(0, whole.size())}, whole);
    }
}

#include "tal/testing.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/loop.hpp"
#include "talcite/promise.hpp"

#include "quilt.hpp"
#include "spool.hpp"

using namespace talcite;

namespace {


// An asynchronous source in memory: it returns a promise that is fulfilled on the pool, like
// `File::read` from talcite::fs, only without a disk. `give` limits a single answer, because a
// source is allowed to return less than was asked for.
Feed fed(const Bytes& raw, std::size_t give = SIZE_MAX) {
    return [raw, give](std::size_t at, std::size_t most) {
        Bytes made;

        const std::size_t much = most < give ? most : give;

        for (std::size_t one = at; one < raw.size() && one < at + much; ++one) made.push_back(raw[one]);

        return ready(made);
    };
}

// A source that counts its calls. It shows whether the byte window really saves work: scrolling
// through the neighborhood must not go down to it on every row. It counts when the request is
// made, on the main thread, so the counter can be read without a lock.
class Taped {
public:
    explicit Taped(std::string what) : raw_(bytes(what)) {}

    // The size of the whole document in bytes.
    [[nodiscard]] std::size_t whole() const { return raw_.size(); }

    // How many requests the spool has made so far.
    [[nodiscard]] int calls() const { return calls_; }

    // A feed over the stored bytes that counts each request.
    [[nodiscard]] Feed feed() {
        return [this](std::size_t at, std::size_t most) {
            ++calls_;

            return fed(raw_)(at, most);
        };
    }

private:
    Bytes raw_;
    int calls_ = 0;
};

// A spool on an asynchronous source has to live in a `shared_ptr`, because the window returns to it
// through a weak handle.
std::shared_ptr<Spool> spooled(const Feed& feed, std::size_t whole) { return std::make_shared<Spool>(feed, whole); }

// Up to `most` lines from `which`, read the way a page reads them: the extent of each line, then its
// bytes in pieces. Nothing while a window is on its way; fewer lines at the end of the document.
std::optional<std::vector<std::string>> texts(Spool& one, std::size_t which, std::size_t most) {
    std::vector<std::string> made;

    for (std::size_t line = which; line < which + most; ++line) {
        const std::optional<Spool::Line> got = one.line(line);

        if (!got.has_value()) {
            if (one.stalled()) return {};

            break;
        }

        std::string text;

        for (std::size_t at = got->at; at < got->upto;) {
            const std::optional<std::string_view> piece = one.read(at, got->upto - at);

            if (!piece.has_value()) return {};

            text += *piece;
            at += piece->size();
        }

        made.push_back(std::move(text));
    }

    return made;
}

// Asks until the spool stops waiting for a window; between the questions it turns the loop, as
// `Term` would. Throws if the spool does not settle within 4000 turns.
template <typename Fn>
auto settled(Loop& loop, Spool& one, Fn fn) {
    for (int tries = 0; tries < 4000; ++tries) {
        auto got = fn();

        if (!one.stalled()) return got;

        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    throw std::runtime_error{"The spool never settled."};
}

// Rows from `which` on, at most `most` of them, once the spool has them; empty if it has none.
std::vector<std::string> rolled(Loop& loop, Spool& one, std::size_t which, std::size_t most) {
    return settled(loop, one, [&] { return texts(one, which, most); }).value_or(std::vector<std::string>{});
}

// The starting byte of row `which`, once the spool knows it.
std::optional<std::size_t> placed(Loop& loop, Spool& one, std::size_t which) {
    return settled(loop, one, [&] { return one.at(which); });
}

// A document of `much` numbered rows, each one `said` followed by its number and a line end.
std::string many(int much, const char* said = "wiersz ") {
    std::string made;

    for (int one = 0; one < much; ++one) made += said + std::to_string(one) + "\n";

    return made;
}

}

// --- A document in memory ------------------------------------------------------------------------

TRIAL("Spool: counts the rows when given") {
    Spool one{bytes("a\nbb\nccc")};

    OUGHT(one.counted().has_value());
    SAME(*one.counted(), std::size_t{3});
    SAME(one.whole(), std::size_t{8});
}

TRIAL("Spool: gives the content without the line end") {
    Spool one{bytes("a\nbb\nccc")};

    const std::vector<std::string> got = *texts(one, 0, 10);

    SAME(got.size(), std::size_t{3});
    SAME(got[0], std::string{"a"});
    SAME(got[1], std::string{"bb"});
    SAME(got[2], std::string{"ccc"});
}

TRIAL("Spool: knows the starting byte of every row") {
    Spool one{bytes("a\nbb\nccc")};

    SAME(*one.at(0), std::size_t{0});
    SAME(*one.at(1), std::size_t{2});
    SAME(*one.at(2), std::size_t{5});
    OUGHT(!one.at(3).has_value());
}

// A line end at the end of the file does not make an empty row after it. Otherwise every properly
// terminated file, which is nearly every file, would have one row too many.
TRIAL("Spool: a line end at the end adds no empty row") {
    Spool one{bytes("a\nb\n")};

    SAME(*one.counted(), std::size_t{2});
}

TRIAL("Spool: an empty document has zero rows") {
    Spool one{bytes("")};

    SAME(*one.counted(), std::size_t{0});
    SAME(texts(one, 0, 5)->size(), std::size_t{0});
    OUGHT(!one.at(0).has_value());
}

TRIAL("Spool: a carriage return disappears from the end of a row") {
    Spool one{bytes("x\r\ny\r\n")};

    const std::vector<std::string> got = *texts(one, 0, 5);

    SAME(got.size(), std::size_t{2});
    SAME(got[0], std::string{"x"});
    SAME(got[1], std::string{"y"});
}

// A binary file has not a single `\n`, and the spool has no limit on a line: it is one line, however
// long, and any piece of it can be read without the rest.
TRIAL("Spool: a document without line ends is one line, however long") {
    const std::string said(3u * 1024 * 1024 + 7, 'z');

    Spool one{bytes(said)};

    SAME(*one.counted(), std::size_t{1});

    const std::optional<Spool::Line> got = one.line(0);

    OUGHT(got.has_value());
    SAME(got->at, std::size_t{0});
    SAME(got->upto, said.size());
    SAME(got->next, said.size());

    SAME(*one.read(2u * 1024 * 1024, 5), std::string_view{"zzzzz"});
    SAME(one.read(said.size() - 3, 100)->size(), std::size_t{3});
    SAME(one.found(2u * 1024 * 1024), std::size_t{0});
}

// --- A streaming source --------------------------------------------------------------------------

TRIAL("Spool: a stream does not know its row count before reaching the end") {
    Loop loop;
    Taped tape{many(2000)};

    const auto one = spooled(tape.feed(), tape.whole());

    OUGHT(!one->counted().has_value());

    (void)rolled(loop, *one, 0, 5);

    OUGHT(!one->counted().has_value());
}

// The first question has nothing to answer with: the window has only just been requested. The
// answer is an empty optional instead of a vector, and `stalled()`; after a turn of the loop the
// window is there and so are the rows. Nothing here waited.
TRIAL("Spool: the first question is answered with a request, and after a turn of the loop with rows") {
    Loop loop;
    Taped tape{many(50)};

    const auto one = spooled(tape.feed(), tape.whole());

    int woken = 0;

    one->awoken([&woken] { ++woken; });

    const std::optional<std::vector<std::string>> first = texts(*one, 0, 5);

    OUGHT(!first.has_value());
    OUGHT(one->stalled());
    SAME(tape.calls(), 1);
    SAME(woken, 0);

    // A second question before the turn does not request again: one window in flight at a time.
    OUGHT(!texts(*one, 0, 5).has_value());
    SAME(tape.calls(), 1);

    const std::vector<std::string> got = rolled(loop, *one, 0, 5);

    SAME(got.size(), std::size_t{5});
    SAME(got[0], std::string{"wiersz 0"});
    OUGHT(!one->stalled());
    SAME(woken, 1);
}

// This is the moment when the `+` disappears in Midnight Commander.
TRIAL("Spool: the row count appears after reaching the end") {
    Loop loop;
    Taped tape{many(2000)};

    const auto one = spooled(tape.feed(), tape.whole());

    (void)rolled(loop, *one, 1999, 5);

    OUGHT(one->counted().has_value());
    SAME(*one->counted(), std::size_t{2000});
}

// `counting()` goes to the end by itself, window by window, with nobody asking in the meantime; when
// it is done, it wakes the handle so that `End` can finish.
TRIAL("Spool: counting reaches the end window by window and wakes the handle") {
    Loop loop;
    Taped tape{many(30000)};

    const auto one = spooled(tape.feed(), tape.whole());

    int woken = 0;

    one->awoken([&woken] { ++woken; });
    one->counting();

    OUGHT(!one->counted().has_value());

    for (int tries = 0; tries < 4000 && !one->counted().has_value(); ++tries) {
        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    SAME(*one->counted(), std::size_t{30000});
    OUGHT(woken > 1);
    OUGHT(tape.calls() > 1);

    // Once counted, the end is at hand: the last row needs no new request.
    const int calls = tape.calls();

    const std::vector<std::string> got = rolled(loop, *one, 29999, 1);

    SAME(got.size(), std::size_t{1});
    SAME(got[0], std::string{"wiersz 29999"});
    SAME(tape.calls(), calls);
}

TRIAL("Spool: a jump forward and back gives the same as a document in memory") {
    Loop loop;

    const std::string said = many(20000);

    Taped tape{said};

    const auto one = spooled(tape.feed(), tape.whole());
    Spool wzor{bytes(said)};

    one->screen(40);

    for (const std::size_t which : {std::size_t{0}, std::size_t{15000}, std::size_t{14999},
                                    std::size_t{1}, std::size_t{19999}}) {
        const std::vector<std::string> got = rolled(loop, *one, which, 2);
        const std::vector<std::string> want = *texts(wzor, which, 2);

        SAME(got, want);
        SAME(*placed(loop, *one, which), *wzor.at(which));
    }
}

// Stepping back is the only hard operation, because the previous row start cannot be computed; it
// has to be found. So we walk the whole document backward, row by row.
TRIAL("Spool: stepping back through the whole document does not lose a single row") {
    Loop loop;
    Taped tape{many(3000, "")};

    const auto one = spooled(tape.feed(), tape.whole());

    (void)rolled(loop, *one, 2999, 1);

    for (int which = 2999; which >= 0; --which) {
        const std::vector<std::string> got = rolled(loop, *one, static_cast<std::size_t>(which), 1);

        SAME(got.size(), std::size_t{1});
        SAME(got[0], std::to_string(which));
    }
}

// The byte window makes sense only if scrolling through the neighborhood uses it. A hundred rows
// of plain text fit in one window, so there should be exactly one request.
TRIAL("Spool: scrolling through the neighborhood does not go to the source for every row") {
    Loop loop;
    Taped tape{many(100)};

    const auto one = spooled(tape.feed(), tape.whole());

    for (std::size_t which = 0; which < 100; ++which) (void)rolled(loop, *one, which, 1);

    SAME(tape.calls(), 1);
}

// With a document longer than the window, the next window is requested before anyone asks for it:
// when we hand out rows from the end of what we have, the request is already running. A row from
// the second window then comes out without an empty answer, so scrolling down has no stops.
TRIAL("Spool: the window is on its way before scrolling reaches it") {
    Loop loop;
    Taped tape{many(20000)};

    const auto one = spooled(tape.feed(), tape.whole());

    int woken = 0;

    one->awoken([&woken] { ++woken; });

    (void)rolled(loop, *one, 0, 40);

    SAME(tape.calls(), 1);

    // Row 3100 lies about 40 KB from the start: in the first window, but closer to its end than half
    // a window, which is enough for the second window to set off without being asked.
    (void)rolled(loop, *one, 3100, 40);

    SAME(tape.calls(), 2);

    for (int tries = 0; tries < 4000 && woken < 2; ++tries) {
        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    SAME(woken, 2);

    // Row 5000 is already in the second window: answered at once, without a request.
    const std::optional<std::vector<std::string>> got = texts(*one, 5000, 40);

    OUGHT(got.has_value());
    SAME(got->size(), std::size_t{40});
    SAME(got->front(), std::string{"wiersz 5000"});
    OUGHT(!one->stalled());
    SAME(tape.calls(), 2);
}

// A source is allowed to return less than was asked for: a file read in pieces, a socket, anything.
// Stepping back scans a whole window backward, so a single short answer must not throw it back to
// the very start of the document, which would yield row zero instead of the previous row.
TRIAL("Spool: a source returning a hundred bytes at a time does not break stepping back") {
    Loop loop;

    const std::string said = many(4000);

    const Bytes raw = bytes(said);

    const auto one = spooled(fed(raw, 100), raw.size());

    (void)rolled(loop, *one, 3999, 1);

    for (int which = 3999; which >= 3900; --which) {
        const std::vector<std::string> got = rolled(loop, *one, static_cast<std::size_t>(which), 1);

        SAME(got.size(), std::size_t{1});
        SAME(got[0], "wiersz " + std::to_string(which));
    }
}

// A source that refuses does not lock the program in a loop of requests: the document ends where
// we got to, and from then on nobody asks for anything.
TRIAL("Spool: a refusal by the source ends the document where it got to") {
    Loop loop;

    const Bytes raw = bytes(many(20000));

    int calls = 0;

    const Feed feed = [&calls, raw](std::size_t at, std::size_t most) {
        ++calls;

        if (at > 0) return failed<Bytes>(std::make_exception_ptr(std::runtime_error{"gone"}));

        return fed(raw)(at, most);
    };

    const auto one = spooled(feed, raw.size());

    const std::vector<std::string> first = rolled(loop, *one, 0, 5);

    SAME(first.size(), std::size_t{5});

    const std::vector<std::string> far = rolled(loop, *one, 19000, 5);

    OUGHT(far.empty());
    OUGHT(one->counted().has_value());
    OUGHT(*one->counted() < 20000);

    const int was = calls;

    (void)rolled(loop, *one, 19000, 5);
    (void)rolled(loop, *one, 5000, 5);

    SAME(calls, was);
}

TRIAL("Spool: tells which row contains a given byte") {
    Spool one{bytes("a\nbb\nccc")};

    SAME(one.found(0), std::size_t{0});
    SAME(one.found(1), std::size_t{0});
    SAME(one.found(2), std::size_t{1});
    SAME(one.found(4), std::size_t{1});
    SAME(one.found(5), std::size_t{2});
    SAME(one.found(7), std::size_t{2});
    SAME(one.found(1000), std::size_t{2});
}

TRIAL("Spool: byte and row agree both ways along the whole length") {
    const std::string said = many(5000);

    Spool one{bytes(said)};

    for (std::size_t which = 0; which < 5000; which += 137)
        SAME(one.found(*one.at(which)), which);
}

// After a change the anchors move instead of vanishing. The spool once reached a far row and knows
// the anchors along the way. A change at the start (an insertion with new rows, then deleting a line
// end) moves them, so jumping back to the same place reads the neighborhood of an anchor, not
// everything from the place of the change. The result agrees with counting from zero.
TRIAL("Spool: after a change the anchors move, and a far jump does not read again") {
    std::string doc;

    for (int at = 0; at < 60000; ++at) doc += "wiersz " + std::to_string(at) + "\n";

    auto quilt = std::make_shared<Quilt>(bytes(doc));
    auto read = std::make_shared<std::size_t>(0);

    const auto spool = std::make_shared<Spool>(
        [quilt, read](std::size_t at, std::size_t most) {
            Bytes got = quilt->bytes(at, most);

            *read += got.size();

            return got;
        },
        quilt->whole());

    OUGHT(spool->at(50000).has_value());

    const auto started = [&doc](std::size_t line) {
        std::size_t at = 0;

        for (std::size_t one = 0; one < line; ++one) at = doc.find('\n', at) + 1;

        return at;
    };

    // A line end right before an anchor (one every 64 rows): the anchor lies in the deleted stretch
    // and vanishes, because its row now starts elsewhere.
    const std::size_t joined = started(192) - 1;
    doc.erase(joined, 1);
    quilt->erase(joined, 1);
    spool->changed(joined, 1, 0, -1);

    doc.insert(5, "ab\ncd\n");
    quilt->insert(5, "ab\ncd\n");
    spool->changed(5, 0, 6, 2);

    const std::size_t gone = doc.find('\n', 20);
    doc.erase(gone, 1);
    quilt->erase(gone, 1);
    spool->changed(gone, 1, 0, -1);

    // The place where the spool stood moved along with the anchors: asking for it reads nothing.
    *read = 0;

    SAME(spool->at(50000), std::optional<std::size_t>{started(50000)});
    SAME(*read, std::size_t{0});

    SAME(spool->at(50001), std::optional<std::size_t>{started(50001)});
    OUGHT(*read < 200000);

    for (const std::size_t line : {0U, 1U, 2U, 3U, 4U, 100U, 190U, 191U, 192U, 25000U, 50000U})
        SAME(spool->at(line), std::optional<std::size_t>{started(line)});

    SAME(spool->found(started(40000) + 3), std::size_t{40000});
}

// A jump far from where the spool stands starts from the nearest anchor, both ways: a row by number
// and a row by byte. There are several thousand anchors and they are found by bisection; if the
// search returned anchor zero, a jump into the middle would read the document from the start or
// step back row by row from where the spool stands.
TRIAL("Spool: a far jump starts from the nearest anchor") {
    std::string doc;

    for (int at = 0; at < 60000; ++at) doc += "wiersz " + std::to_string(at) + "\n";

    auto quilt = std::make_shared<Quilt>(bytes(doc));
    auto read = std::make_shared<std::size_t>(0);

    const auto spool = std::make_shared<Spool>(
        [quilt, read](std::size_t at, std::size_t most) {
            Bytes got = quilt->bytes(at, most);

            *read += got.size();

            return got;
        },
        quilt->whole());

    OUGHT(spool->at(59000).has_value());

    const auto started = [&doc](std::size_t line) {
        std::size_t at = 0;

        for (std::size_t one = 0; one < line; ++one) at = doc.find('\n', at) + 1;

        return at;
    };

    *read = 0;

    SAME(spool->at(35000), std::optional<std::size_t>{started(35000)});
    OUGHT(*read < 100000);

    OUGHT(spool->at(59000).has_value());

    *read = 0;

    SAME(spool->found(started(30000) + 3), std::size_t{30000});
    OUGHT(*read < 100000);
}

// Many changes at once (`rewritten`): everything after the first change is discarded, including
// the anchor right after it, because with its line end deleted that row starts elsewhere.
TRIAL("Spool: rewriting from a place throws away the anchors right after it") {
    std::string doc;

    for (int at = 0; at < 1000; ++at) doc += "w" + std::to_string(at) + "\n";

    auto quilt = std::make_shared<Quilt>(bytes(doc));

    const auto spool = std::make_shared<Spool>([quilt](std::size_t at, std::size_t most) { return quilt->bytes(at, most); },
                                               quilt->whole());

    OUGHT(spool->at(500).has_value());

    std::size_t joined = 0;

    for (int line = 0; line < 128; ++line) joined = doc.find('\n', joined) + 1;

    --joined;

    doc.erase(joined, 1);
    quilt->erase(joined, 1);
    spool->rewritten(joined, -1, -1);

    std::size_t at = 0;

    for (int line = 0; line < 127; ++line) at = doc.find('\n', at) + 1;

    SAME(spool->at(127), std::optional<std::size_t>{at});
    SAME(spool->found(at + 2), std::size_t{127});
    SAME(spool->found(joined + 1), std::size_t{127});
    SAME(spool->at(128), std::optional<std::size_t>{doc.find('\n', joined) + 1});
}

// --- Long lines through a stream -----------------------------------------------------------------

// A line far longer than the window comes through: the walk to its end goes window after window and
// never starts again from the start of the line, so the source is asked about every window once.
// Before, a line of a megabyte or more was never answered, and the spool kept reading it forever.
TRIAL("Spool: a line far longer than the window comes through a stream") {
    Loop loop;
    Taped tape{std::string(2u * 1024 * 1024, 'x') + "\nkoniec\n"};

    const auto one = spooled(tape.feed(), tape.whole());

    const std::optional<Spool::Line> first = settled(loop, *one, [&] { return one->line(0); });

    OUGHT(first.has_value());
    SAME(first->upto, std::size_t{2u * 1024 * 1024});
    SAME(first->next, std::size_t{2u * 1024 * 1024 + 1});
    OUGHT(tape.calls() <= 2 * 1024 / 64 + 4);

    SAME(rolled(loop, *one, 1, 1), std::vector<std::string>{"koniec"});
}

// Stepping back over a long line searches backward window by window and goes on from where it
// stopped while a window was on its way.
TRIAL("Spool: stepping back over a line longer than the window finds its start") {
    Loop loop;
    Taped tape{"a\n" + std::string(2u * 1024 * 1024, 'y') + "\nb\n"};

    const auto one = spooled(tape.feed(), tape.whole());

    SAME(*placed(loop, *one, 2), std::size_t{2u * 1024 * 1024 + 3});

    const int was = tape.calls();

    one->screen(1);

    SAME(*placed(loop, *one, 1), std::size_t{2});
    OUGHT(tape.calls() - was <= 2 * 1024 / 64 + 4);
}

// The line holding a byte deep in a long line is known as soon as there is no line end between the
// line's start and the byte: what lies after the byte is never read.
TRIAL("Spool: the line of a byte in a long line is found without reading the rest of it") {
    Loop loop;
    Taped tape{"a\n" + std::string(4u * 1024 * 1024, 'y') + "\nb\n"};

    const auto one = spooled(tape.feed(), tape.whole());

    SAME(settled(loop, *one, [&] { return one->found(2 + 1024 * 1024); }), std::size_t{1});
    OUGHT(tape.calls() <= 1024 / 64 + 3);
}

// A piece far into the document is read on its own, without walking to it.
TRIAL("Spool: a piece far into the document is read without walking there") {
    Loop loop;
    Taped tape{std::string(3u * 1024 * 1024, 'q') + "!"};

    const auto one = spooled(tape.feed(), tape.whole());

    const std::optional<std::string_view> got =
        settled(loop, *one, [&] { return one->read(3u * 1024 * 1024 - 2, 10); });

    OUGHT(got.has_value());
    SAME(std::string{*got}, std::string{"qq!"});
    SAME(tape.calls(), 1);
}

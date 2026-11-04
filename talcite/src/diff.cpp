#include "talcite/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

// --- String difference ---------------------------------------------------------------------------

namespace {

// A word is a run of letters and digits, and every other character is a piece of its own.
// Comparing by characters would scatter `.1080p.BluRay` into letters that happen to match
// something on the other side; by words, it comes out as one removed whole. A byte outside ASCII
// counts as a letter, because it belongs to a multibyte letter, so a Polish name does not fall
// apart at a diacritic.
bool wordy(unsigned char sign) {
    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z') ||
           sign >= 0x80;
}

// The text cut into pieces: whole words, and every other character on its own.
std::vector<std::string_view> tokenized(std::string_view text) {
    std::vector<std::string_view> made;

    std::size_t at = 0;

    while (at < text.size()) {
        std::size_t end = at + 1;

        if (wordy(static_cast<unsigned char>(text[at])))
            while (end < text.size() && wordy(static_cast<unsigned char>(text[end]))) ++end;

        made.push_back(text.substr(at, end - at));

        at = end;
    }

    return made;
}

// How many edit steps may be searched before the strings count as simply different. For file
// names that is far beyond need; for lines of a file it is the limit past which highlighting the
// differences stops saying anything, while the memory of the trace grows with its square.
constexpr int patience = 256;

// How many pieces are compared at all. A line longer than this many words is marked as a whole.
constexpr std::size_t ample = 4096;

// The longest common subsequence by Myers' algorithm, for the middle without the common start: a
// pass over the diagonals with a remembered trace, then the way back from the end. The cost grows
// with the length times the number of differences, so two nearly equal strings cost as much as
// reading them. Returns false when there are more than `patience` differences; then the caller
// marks the whole.
bool matched(const std::vector<std::string_view>& left, std::size_t from_left, std::size_t upto_left,
             const std::vector<std::string_view>& right, std::size_t from_right, std::size_t upto_right,
             std::vector<bool>& kept_left, std::vector<bool>& kept_right) {
    const int n = static_cast<int>(upto_left - from_left);
    const int m = static_cast<int>(upto_right - from_right);

    if (n == 0 || m == 0) return true;

    const auto same = [&](int x, int y) {
        return left[from_left + static_cast<std::size_t>(x)] == right[from_right + static_cast<std::size_t>(y)];
    };

    const int most = n + m;
    const int off = most + 1;

    std::vector<int> reach(static_cast<std::size_t>(2 * most + 3), 0);

    // The trace: for every `d`, the diagonal values from `-d` to `d`, after step `d`.
    std::vector<std::vector<int>> trace;

    const int limit = std::min(most, patience);

    for (int d = 0; d <= limit; ++d) {
        for (int k = -d; k <= d; k += 2) {
            const std::size_t here = static_cast<std::size_t>(off + k);

            int x = (k == -d || (k != d && reach[here - 1] < reach[here + 1])) ? reach[here + 1] : reach[here - 1] + 1;
            int y = x - k;

            while (x < n && y < m && same(x, y)) {
                ++x;
                ++y;
            }

            reach[here] = x;

            if (x < n || y < m) continue;

            // The way back: every step leaves one edit and the diagonal before it, and the
            // diagonals are exactly the common pieces.
            int bx = n;
            int by = m;

            for (int back = d; back > 0; --back) {
                const std::vector<int>& before = trace[static_cast<std::size_t>(back - 1)];

                const auto at = [&before, back](int j) { return before[static_cast<std::size_t>(j + back - 1)]; };

                const int kk = bx - by;
                const bool down = kk == -back || (kk != back && at(kk - 1) < at(kk + 1));
                const int pk = down ? kk + 1 : kk - 1;
                const int px = at(pk);
                const int py = px - pk;

                while (bx > px && by > py) {
                    kept_left[from_left + static_cast<std::size_t>(bx - 1)] = true;
                    kept_right[from_right + static_cast<std::size_t>(by - 1)] = true;
                    --bx;
                    --by;
                }

                bx = px;
                by = py;
            }

            // There is no diagonal before the first edit: the caller has already cut off the common
            // start, so the first pieces of both sides differ.
            return true;
        }

        trace.emplace_back(reach.begin() + off - d, reach.begin() + off + d + 1);
    }

    return false;
}

// The byte ranges of the pieces not kept, merged where they touch.
std::vector<DiffSpan> spanned(const std::vector<std::string_view>& tokens, const std::vector<bool>& kept,
                              std::string_view text) {
    std::vector<DiffSpan> made;

    for (std::size_t one = 0; one < tokens.size(); ++one) {
        if (kept[one]) continue;

        const auto from = static_cast<std::size_t>(tokens[one].data() - text.data());
        const std::size_t upto = from + tokens[one].size();

        if (!made.empty() && made.back().upto == from)
            made.back().upto = upto;
        else
            made.push_back(DiffSpan{from, upto});
    }

    return made;
}

}

// The difference of two strings by words. The common start and end are cut off at once (file
// names and lines after a fix usually differ in the middle), and Myers compares the middle. When
// there are too many differences or the string is too long, the whole middle is the difference:
// highlighting half of the characters would say nothing.
std::pair<DiffSide, DiffSide> diffed(std::string_view left, std::string_view right) {
    std::pair<DiffSide, DiffSide> made;

    made.first.text = std::string{left};
    made.second.text = std::string{right};

    const std::vector<std::string_view> ones = tokenized(made.first.text);
    const std::vector<std::string_view> twos = tokenized(made.second.text);

    std::vector<bool> kept_one(ones.size(), false);
    std::vector<bool> kept_two(twos.size(), false);

    std::size_t head = 0;

    while (head < ones.size() && head < twos.size() && ones[head] == twos[head]) {
        kept_one[head] = true;
        kept_two[head] = true;
        ++head;
    }

    std::size_t tail = 0;

    while (tail < ones.size() - head && tail < twos.size() - head &&
           ones[ones.size() - 1 - tail] == twos[twos.size() - 1 - tail]) {
        kept_one[ones.size() - 1 - tail] = true;
        kept_two[twos.size() - 1 - tail] = true;
        ++tail;
    }

    const std::size_t upto_one = ones.size() - tail;
    const std::size_t upto_two = twos.size() - tail;

    if (upto_one - head + upto_two - head <= ample)
        (void)matched(ones, head, upto_one, twos, head, upto_two, kept_one, kept_two);

    made.first.marked = spanned(ones, kept_one, made.first.text);
    made.second.marked = spanned(twos, kept_two, made.second.text);

    return made;
}

// --- Widget --------------------------------------------------------------------------------------

namespace {

// How many hunks are taken from the source at once and kept at hand.
constexpr std::size_t batch = 128;

// How many rows the wheel skips. The same constant as in the list, the table and the scroller.
constexpr int stepping = 3;

// The checkbox: `[x] `. Four cells, as in `Tick`.
constexpr int ticking = 4;

// The separator between the sides: space, bar, space.
constexpr int parting = 3;

// Below this many cells per side the view switches to one column on its own.
constexpr int narrow = 24;

// A screen row within a hunk: the title or a difference row; in the one-column view one of its
// sides, or both at once when they are equal.
enum class Face : std::uint8_t { title, both, left, right };

struct Stripe {
    Face face = Face::both;
    std::size_t row = 0;
};

// Whether both sides of the row are present and equal, with nothing marked.
bool same(const DiffRow& one) {
    return one.left.has_value() && one.right.has_value() && one.left->marked.empty() &&
           one.right->marked.empty() && one.left->text == one.right->text;
}

// The screen rows of a hunk. A folded one shows the title and the first row; the rest is a "+N"
// next to it.
std::vector<Stripe> lined(const DiffHunk& hunk, bool open, bool unify) {
    std::vector<Stripe> made;

    if (!hunk.title.empty()) made.push_back(Stripe{Face::title, 0});

    const std::size_t shown = open ? hunk.rows.size() : std::min<std::size_t>(hunk.rows.size(), 1);

    for (std::size_t row = 0; row < shown; ++row) {
        const DiffRow& one = hunk.rows[row];

        if (!unify || same(one)) {
            made.push_back(Stripe{Face::both, row});

            continue;
        }

        if (one.left.has_value()) made.push_back(Stripe{Face::left, row});
        if (one.right.has_value()) made.push_back(Stripe{Face::right, row});
    }

    if (made.empty()) made.push_back(Stripe{Face::title, 0});

    return made;
}

// A character to draw: its bytes, its width and whether it lies in a marked difference.
struct Glyph {
    std::string text;
    int wide = 0;
    bool marked = false;
};

// The characters of one side, cluster by cluster (a letter with its accent is one), with tabs
// expanded to spaces and each character marked when it starts in a marked range. A cluster that
// shows nothing (a lone format character) is left out.
std::vector<Glyph> glyphed(const DiffSide& side) {
    std::vector<Glyph> made;

    std::size_t span = 0;
    int column = 0;

    for (std::size_t at = 0; at < side.text.size();) {
        const std::size_t was = at;

        at = cluster_after(side.text, at);

        while (span < side.marked.size() && side.marked[span].upto <= was) ++span;

        const bool marked = span < side.marked.size() && side.marked[span].from <= was;

        if (side.text[was] == '\t') {
            const int upto = ((column / tabbing) + 1) * tabbing;

            for (; column < upto; ++column) made.push_back(Glyph{" ", 1, marked});

            continue;
        }

        const std::string_view one = std::string_view{side.text}.substr(was, at - was);
        const int wide = width(one);

        if (wide == 0) continue;

        made.push_back(Glyph{std::string{one}, wide, marked});

        column += wide;
    }

    return made;
}

// Cutting the characters to a width, with an ellipsis where the shape asks for one. Characters are
// cut, not the string, so that marks stay with their letters even after the middle is shortened.
std::vector<Glyph> fitted(std::vector<Glyph> all, int room, Elide how, const std::string& dots) {
    int whole = 0;

    for (const Glyph& one : all) whole += one.wide;

    if (whole <= room) return all;

    if (room <= 0) return {};

    const auto head = [&all](int much) {
        std::vector<Glyph> made;
        int used = 0;

        for (const Glyph& one : all) {
            if (used + one.wide > much) break;

            made.push_back(one);
            used += one.wide;
        }

        return made;
    };

    const auto tail = [&all](int much) {
        std::vector<Glyph> made;
        int used = 0;

        for (auto one = all.rbegin(); one != all.rend(); ++one) {
            if (used + one->wide > much) break;

            made.push_back(*one);
            used += one->wide;
        }

        std::reverse(made.begin(), made.end());

        return made;
    };

    if (how == Elide::none) return head(room);

    const Glyph mark{dots, 1, false};

    if (room == 1) return {mark};

    std::vector<Glyph> made;

    if (how == Elide::right) {
        made = head(room - 1);
        made.push_back(mark);

        return made;
    }

    if (how == Elide::left) {
        made.push_back(mark);

        for (Glyph& one : tail(room - 1)) made.push_back(std::move(one));

        return made;
    }

    const int half = (room - 1) / 2;

    made = head(half);
    made.push_back(mark);

    for (Glyph& one : tail(room - 1 - half)) made.push_back(std::move(one));

    return made;
}

// The number of decimal digits of the number.
int digits(std::size_t number) {
    int made = 1;

    for (; number >= 10; number /= 10) ++made;

    return made;
}

}

struct DiffViewer::Kept {
    DiffSource source;
    std::size_t hunks = 0;

    std::string left_title;
    std::string right_title;

    // The window of hunks at hand: from `from`, as many as the source returned. The source is
    // asked only when a needed hunk falls outside the window. Each hunk is shared, so that the
    // visible rows can hold on to theirs after the window moves without copying it.
    std::size_t from = 0;
    std::vector<std::shared_ptr<const DiffHunk>> cached;

    std::vector<bool> open;

    // The first screen row: the hunk and the row within it.
    std::size_t top = 0;
    int part = 0;

    std::optional<std::size_t> which;

    // Whether the checkbox column is visible. Once on, it stays until a new source; otherwise
    // scrolling to a place without checkboxes would shift the content sideways.
    bool ticks = false;

    std::optional<bool> unify;

    // What is visible: the screen rows, each holding its hunk, shared by all the rows of that hunk.
    // Computed only after a change.
    struct Shown {
        std::size_t hunk = 0;
        Stripe line;
        bool first = false;
        std::shared_ptr<const DiffHunk> data;
    };

    std::vector<Shown> shown;

    // Whether there is anything past the last shown row: the rest of the hunk or the next hunk.
    // Without it the bar has nothing to show: the whole plan stands on the screen.
    bool more = false;

    bool fresh = true;
    Size sized;
    bool unified_now = false;

    DiffShape shape;

    std::string scrollbar_role = "talcite.scroll";

    // The bar as the layout settled it, for the mouse and for painting.
    Grip grip;
    std::optional<Track> bar;

    // The hunk with the given number, or nothing past the end or when the source says to wait.
    // Whoever keeps the pointer keeps the hunk, also when a new window replaces the old one.
    std::shared_ptr<const DiffHunk> held(std::size_t which) {
        if (which >= hunks || !source) return nullptr;

        if (which >= from && which < from + cached.size()) return cached[which - from];

        const std::size_t start = which >= batch / 2 ? which - batch / 2 : 0;

        std::optional<std::vector<DiffHunk>> got = source(start, batch);

        if (!got.has_value()) return nullptr;

        from = start;
        cached.clear();
        cached.reserve(got->size());

        for (DiffHunk& one : *got) {
            if (one.tickable) ticks = true;

            cached.push_back(std::make_shared<const DiffHunk>(std::move(one)));
        }

        if (which >= from + cached.size()) return nullptr;

        return cached[which - from];
    }

    // The hunk with the given number, for a look that ends before the next question: a new window
    // may drop it.
    const DiffHunk* fetched(std::size_t which) { return held(which).get(); }

    // Whether the hunk is unfolded.
    [[nodiscard]] bool opened(std::size_t hunk) const { return hunk < open.size() && open[hunk]; }

    // The number of screen rows of the hunk; nothing when it is not available.
    std::optional<int> height(std::size_t hunk) {
        const DiffHunk* one = fetched(hunk);

        if (one == nullptr) return {};

        return static_cast<int>(lined(*one, opened(hunk), unified_now).size());
    }

    // The cursor stops only on hunks with rows. A title alone is a section header: it separates,
    // it is not something to choose.
    bool steady(std::size_t hunk) {
        const DiffHunk* one = fetched(hunk);

        return one != nullptr && !one->rows.empty();
    }

    // The rows available for hunks: the height without the title row, when there is one.
    [[nodiscard]] int high() const { return sized.high - (left_title.empty() && right_title.empty() ? 0 : 1); }

    // The screen does not go past the end: when fewer rows than the height remain from the first
    // row to the end, the view moves back until the last row stands at the bottom. A hunk that is
    // waiting stops the moving back, because its number of rows is unknown.
    void settle() {
        const int room = high();

        if (room <= 0) return;

        int rows = -part;

        for (std::size_t one = top; one < hunks && rows < room; ++one) {
            const std::optional<int> got = height(one);

            if (!got.has_value()) return;

            rows += *got;
        }

        while ((top > 0 || part > 0) && rows < room) {
            if (part > 0) {
                --part;
            } else {
                const std::optional<int> got = height(top - 1);

                if (!got.has_value()) break;

                --top;
                part = *got - 1;
            }

            ++rows;
        }
    }

    // Brings the view to the first row of the hunk: one above lands at the top, one below at the
    // bottom.
    void reveal(std::size_t hunk) {
        const int room = high();

        if (hunk < top || (hunk == top && part > 0)) {
            top = hunk;
            part = 0;
        } else if (room > 0) {
            int before = -part;

            for (std::size_t one = top; one < hunk && before < room; ++one) before += height(one).value_or(1);

            if (before >= room) {
                top = hunk;
                part = 0;

                for (int need = room - 1; need > 0 && (top > 0 || part > 0); --need) {
                    if (part > 0) {
                        --part;
                    } else {
                        --top;
                        part = height(top).value_or(1) - 1;
                    }
                }
            }
        }

        settle();

        fresh = true;
    }
};

// An empty diff viewer with the default shape.
DiffViewer::DiffViewer() : kept_(std::make_unique<Kept>()) { role("talcite.diff"); }

// An empty diff viewer with the given shape.
DiffViewer::DiffViewer(DiffShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.diff");

    kept_->shape = shape;
}

DiffViewer::~DiffViewer() = default;

// The number of hunks is given by the caller, as with the table. The cursor lands on the first
// hunk with rows, if the source already has it; otherwise at the first paint.
void DiffViewer::source(DiffSource given, std::size_t hunks) {
    Kept& kept = *kept_;

    kept.source = std::move(given);
    kept.hunks = hunks;

    kept.from = 0;
    kept.cached.clear();
    kept.open.assign(hunks, false);

    kept.top = 0;
    kept.part = 0;
    kept.which.reset();
    kept.ticks = false;

    chosen().clear();

    for (std::size_t one = 0; one < hunks && one < batch; ++one)
        if (kept.steady(one)) {
            kept.which = one;

            break;
        }

    kept.shown.clear();
    kept.fresh = true;

    Node::changed();
}

// Drops the cached hunks, so that the next paint asks the source again.
void DiffViewer::refresh() {
    kept_->cached.clear();
    kept_->shown.clear();
    kept_->fresh = true;

    Node::changed();
}

// Sets the titles of the two sides, shown in the head row.
void DiffViewer::titles(std::string left, std::string right) {
    kept_->left_title = std::move(left);
    kept_->right_title = std::move(right);
    kept_->fresh = true;

    Node::changed();
}

// The number of hunks given with the source.
std::size_t DiffViewer::hunks() const { return kept_->hunks; }

// The hunk under the cursor, if any.
std::optional<std::size_t> DiffViewer::which() const { return kept_->which; }

// Moves the cursor to the hunk (clamped to the last one), brings it into view and triggers a
// `ScrollEvent`, saying how the move came: the viewer's own keys, wheel and mouse pass
// `Moved::by_hand`, a call from the program leaves the default.
void DiffViewer::which(std::size_t given, Moved how) {
    Kept& kept = *kept_;

    if (kept.hunks == 0) return;

    const std::size_t want = std::min(given, kept.hunks - 1);

    if (kept.which == want) return;

    kept.which = want;

    kept.reveal(want);

    Node::changed();

    (void)trigger(ScrollEvent{how});
}

// Whether the hunk is unfolded.
bool DiffViewer::opened(std::size_t hunk) const { return kept_->opened(hunk); }

// Unfolds or folds the hunk, keeping the cursor's hunk in view.
void DiffViewer::opened(std::size_t hunk, bool given) {
    Kept& kept = *kept_;

    if (hunk >= kept.open.size() || kept.open[hunk] == given) return;

    kept.open[hunk] = given;

    if (kept.which.has_value()) kept.reveal(*kept.which);

    kept.settle();
    kept.fresh = true;

    Node::changed();
}

// The forced layout: one column, two columns, or nothing when the width decides.
std::optional<bool> DiffViewer::unified() const { return kept_->unify; }

// Forces one column (true) or two (false); nothing lets the width decide.
void DiffViewer::unified(std::optional<bool> given) {
    kept_->unify = given;
    kept_->fresh = true;

    Node::changed();
}

// Whether the shape asks for a scroll bar.
bool DiffViewer::scrollbar() const { return kept_->shape.scrollbar; }

// Turns the scroll bar on or off.
void DiffViewer::scrollbar(bool given) {
    kept_->shape.scrollbar = given;
    kept_->fresh = true;

    Node::changed();
}

// The skin role of the scroll bar.
const std::string& DiffViewer::scrollbar_role() const { return kept_->scrollbar_role; }

// Sets the skin role of the scroll bar.
void DiffViewer::scrollbar_role(std::string given) {
    kept_->scrollbar_role = std::move(given);

    Node::changed();
}

// The viewer takes the focus.
bool DiffViewer::focusable() const { return true; }

// The viewer has no children.
int DiffViewer::capacity() const { return 0; }

// The viewer takes all the room it is given.
Size DiffViewer::wants(Size room) const { return room; }

// The screen rows from the first visible one, each holding its hunk: the source may return a new
// window halfway, and what already stands on the screen must not change.
void DiffViewer::spread(Size given) {
    Kept& kept = *kept_;

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int ticks = kept.ticks ? ticking : 0;
    const int side = (given.wide - aside - ticks - parting) / 2;
    const bool unify = kept.unify.value_or(side < narrow);

    if (!kept.fresh && kept.sized == given && kept.unified_now == unify) return;

    kept.sized = given;
    kept.unified_now = unify;
    kept.fresh = false;
    kept.shown.clear();
    kept.more = kept.top > 0 || kept.part > 0;

    const int room = kept.high();

    if (room <= 0 || given.wide <= 0) return;

    kept.settle();

    int skip = kept.part;

    std::size_t one = kept.top;

    for (; one < kept.hunks && static_cast<int>(kept.shown.size()) < room; ++one) {
        std::shared_ptr<const DiffHunk> data = kept.held(one);

        // A hunk the source says to wait for stops the loop before the end; the condition after
        // the loop says that there is still something to show.
        if (data == nullptr) break;

        const std::vector<Stripe> lines = lined(*data, kept.opened(one), unify);

        if (static_cast<int>(kept.shown.size() + lines.size()) - skip > room) kept.more = true;

        for (std::size_t at = 0; at < lines.size() && static_cast<int>(kept.shown.size()) < room; ++at) {
            if (skip > 0) {
                --skip;

                continue;
            }

            kept.shown.push_back(Kept::Shown{one, lines[at], at == 0, data});
        }

        skip = 0;
    }

    if (one < kept.hunks) kept.more = true;
}

// Settles what the viewer shows: the selection within the hunks, the first steady hunk as the
// current one, the visible rows (`spread`) and the scroll bar when there is something to scroll;
// its column stays, so that the content does not jump.
void DiffViewer::layout(Size given) {
    Kept& kept = *kept_;

    chosen_.keep(kept.hunks);

    if (!kept.which.has_value())
        for (std::size_t one = 0; one < kept.hunks && one < batch; ++one)
            if (kept.steady(one)) {
                kept.which = one;

                break;
            }

    spread(given);

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;

    kept.bar.reset();

    if (aside != 0 && given.high > 0 && kept.more) {
        std::size_t shown = 0;
        std::optional<std::size_t> last;

        for (const Kept::Shown& one : kept.shown)
            if (last != one.hunk) {
                ++shown;
                last = one.hunk;
            }

        kept.bar = Track{Scrollbar::edge(given, {{0, 0}, given}, true), true, given.high, kept.top, kept.hunks,
                         std::max<std::size_t>(shown, 1)};
    }

    kept.grip.laid(kept.bar);
}

// Draws the head row with the titles, the visible rows (checkboxes, line numbers, both sides or
// one column with "-" and "+") and the scroll bar when there is something to scroll.
void DiffViewer::paint(Canvas& onto) const {
    const Kept& kept = *kept_;

    const Size given = onto.size();

    const Skin& how = skin();
    const std::string base = role();
    const std::string dots = utf8::encode(how.glyph("talcite.elide"));

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int wide = given.wide - aside;
    const int ticks = kept.ticks ? ticking : 0;
    const bool unify = kept.unified_now;

    // Line numbers: as many digits as the largest visible one has, and when no side carries a
    // number, no column at all.
    int number = 0;

    for (const Kept::Shown& one : kept.shown) {
        if (one.line.face == Face::title) continue;

        const DiffRow& row = one.data->rows[one.line.row];

        for (const std::optional<DiffSide>& side : {row.left, row.right})
            if (side.has_value() && side->number.has_value()) number = std::max(number, digits(*side->number));
    }

    const int gutter = number > 0 ? number + 1 : 0;

    const int side = unify ? wide - ticks - 2 * gutter - 2 : (wide - ticks - parting) / 2 - gutter;
    const int right_at = unify ? 0 : ticks + gutter + side + parting;

    const auto written = [&onto](int x, int y, const std::vector<Glyph>& glyphs, const Style& look,
                                 const Style& marked) {
        for (const Glyph& one : glyphs) {
            onto.put({x, y}, one.text, one.marked ? marked : look);

            x += one.wide;
        }
    };

    const auto numbered = [&onto, number](int x, int y, const std::optional<DiffSide>& one, const Style& look) {
        if (number == 0 || !one.has_value() || !one->number.has_value()) return;

        std::string said = std::to_string(*one->number);

        said.insert(0, static_cast<std::size_t>(number) - said.size(), ' ');

        onto.put({x, y}, said, look);
    };

    int y = 0;

    if (!kept.left_title.empty() || !kept.right_title.empty()) {
        const Style look = how.style(base + ".head");

        onto.fill({{0, y}, {wide, 1}}, look);

        if (unify) {
            const std::string both = kept.left_title + " → " + kept.right_title;

            onto.put({ticks, y}, elided(both, wide - ticks, Elide::right, how.glyph("talcite.elide")),
                     look);
        } else {
            onto.put({ticks + gutter, y},
                     elided(kept.left_title, side, Elide::right, how.glyph("talcite.elide")), look);
            onto.put({right_at, y},
                     elided(kept.right_title, side, Elide::right, how.glyph("talcite.elide")), look);
        }

        ++y;
    }

    const char32_t upright = how.lines(Border::thin).upright;

    for (const Kept::Shown& one : kept.shown) {
        const bool here = one.first && kept.which == one.hunk;

        Style look = how.style(here ? base + ".focus" : base);

        if (!one.data->role.empty()) look = Style::merge(look, how.style(one.data->role));

        const int x = 0;

        onto.fill({{x, y}, {wide, 1}}, look);

        if (one.first && one.data->tickable)
            onto.put({x, y}, chosen_.contains(one.hunk) ? "[x] " : "[ ] ", look);

        if (one.line.face == Face::title) {
            const Style title = Style::merge(look, how.style(base + ".title"));

            onto.put({x + ticks, y}, elided(one.data->title, wide - ticks, Elide::right, how.glyph("talcite.elide")),
                     title);

            ++y;

            continue;
        }

        const DiffRow& row = one.data->rows[one.line.row];
        const bool even = same(row);

        const Style gone = even ? look : Style::merge(look, how.style(base + ".gone"));
        const Style came = even ? look : Style::merge(look, how.style(base + ".came"));
        const Style gone_mark = Style::merge(gone, how.style(base + ".gone.mark"));
        const Style came_mark = Style::merge(came, how.style(base + ".came.mark"));
        const Style faint = Style::merge(look, how.style(base + ".number"));

        // The fold sign at the first row of a hunk with more rows: closed with "+N", how many rows
        // wait behind unfolding, and open once unfolded, the way a tree shows its branches, so a
        // hunk tells that it unfolds before anyone tries.
        std::string folded;

        if (one.line.row == 0 && one.data->rows.size() > 1)
            folded = kept.opened(one.hunk)
                         ? " " + utf8::encode(how.glyph("talcite.diff.open"))
                         : " +" + std::to_string(one.data->rows.size() - 1) + " " + utf8::encode(how.glyph("talcite.diff.closed"));

        const int tag = measured(folded).wide;

        if (unify) {
            const int text_at = x + ticks + 2 * gutter;

            if (one.line.face != Face::right) numbered(x + ticks, y, row.left, faint);
            if (one.line.face != Face::left) numbered(x + ticks + gutter, y, row.right, faint);

            const bool left = one.line.face == Face::left || (one.line.face == Face::both && !row.right.has_value());
            const DiffSide& shown_side = left ? *row.left : *row.right;

            const char* sign = even ? "  " : left ? "- " : "+ ";

            onto.put({text_at, y}, sign, even ? look : left ? gone : came);

            written(text_at + 2, y, fitted(glyphed(shown_side), side - tag, kept.shape.elide, dots),
                    even ? look : left ? gone : came, left ? gone_mark : came_mark);

            if (tag > 0 && one.line.face != Face::right) onto.put({text_at + 2 + side - tag, y}, folded, faint);
        } else {
            numbered(x + ticks, y, row.left, faint);

            if (row.left.has_value())
                written(x + ticks + gutter, y, fitted(glyphed(*row.left), side - tag, kept.shape.elide, dots), gone,
                        gone_mark);

            if (tag > 0) onto.put({x + ticks + gutter + side - tag, y}, folded, faint);

            onto.put({x + ticks + gutter + side + 1, y}, upright, look);

            numbered(x + right_at, y, row.right, faint);

            if (row.right.has_value())
                written(x + right_at + gutter, y, fitted(glyphed(*row.right), side, kept.shape.elide, dots), came,
                        came_mark);
        }

        ++y;
    }

    if (!kept.bar.has_value()) return;

    const Track& track = *kept.bar;

    const Scrollbar bar{.at = track.at,
                        .much = track.much,
                        .shift = track.shift,
                        .whole = track.whole,
                        .window = track.window,
                        .focused = Scrollbar::focused_in(*this),
                        .skin = how,
                        .role = kept.scrollbar_role};

    bar.draw(onto);
}

// Keys move the cursor over hunks, and Space, `+` and `-` change the checkboxes. Tab, Enter and
// letters go up: Tab belongs to the window that cycles through its controls, and Enter presses a
// button. Jumping between sections therefore takes Ctrl with an arrow, not Tab. At its ends the
// viewer keeps its keys, for the reason given at `List::handle`.
bool DiffViewer::handle(KeyEvent& what) {
    Kept& kept = *kept_;

    if (what.mods().alt || kept.hunks == 0) return false;

    const std::size_t at = kept.which.value_or(0);

    // The next hunk with rows in the given direction, `from` excluded; nothing when there is none.
    const auto next = [&kept](std::size_t from, int by) -> std::optional<std::size_t> {
        for (std::size_t one = from; by > 0 ? one + 1 < kept.hunks : one > 0;) {
            one = by > 0 ? one + 1 : one - 1;

            if (kept.steady(one)) return one;
        }

        return {};
    };

    // A section header: a hunk with a title alone.
    const auto heading = [&kept](std::size_t one) {
        const DiffHunk* got = kept.fetched(one);

        return got != nullptr && got->rows.empty();
    };

    if (what.mods().ctrl) {
        const Code code = what.key().code();

        if (code != Code::down && code != Code::up) return false;

        // Ctrl+↓: the first hunk after the next header; Ctrl+↑: the first hunk of the section, or
        // of the previous one when the cursor already stands on it.
        if (code == Code::down) {
            for (std::size_t one = at + 1; one < kept.hunks; ++one)
                if (heading(one))
                    if (const std::optional<std::size_t> got = next(one, 1); got.has_value()) {
                        which(*got, Moved::by_hand);

                        return true;
                    }

            return true;
        }

        std::size_t start = at;

        while (start > 0 && !heading(start - 1)) --start;

        if (start == at && start > 0) {
            start = start - 1;

            while (start > 0 && !heading(start - 1)) --start;
        }

        if (kept.steady(start))
            which(start, Moved::by_hand);
        else if (const std::optional<std::size_t> got = next(start, 1); got.has_value() && *got < at)
            which(*got, Moved::by_hand);

        return true;
    }

    const auto moved = [this, &next, at](int by, int times) {
        std::size_t here = at;

        for (int step = 0; step < times; ++step) {
            const std::optional<std::size_t> got = next(here, by);

            if (!got.has_value()) break;

            here = *got;
        }

        which(here, Moved::by_hand);
    };

    const int page = std::max(static_cast<int>(kept.shown.size()), 1);

    switch (what.key().code()) {
        case Code::up: moved(-1, 1); return true;

        case Code::down: moved(1, 1); return true;

        case Code::pageup: moved(-1, page); return true;

        case Code::pagedown: moved(1, page); return true;

        case Code::home:
            if (kept.steady(0))
                which(0, Moved::by_hand);
            else if (const std::optional<std::size_t> got = next(0, 1); got.has_value())
                which(*got, Moved::by_hand);

            return true;

        case Code::end:
            if (kept.steady(kept.hunks - 1))
                which(kept.hunks - 1, Moved::by_hand);
            else if (const std::optional<std::size_t> got = next(kept.hunks - 1, -1); got.has_value())
                which(*got, Moved::by_hand);

            return true;

        case Code::right: opened(at, true); return true;

        case Code::left: opened(at, false); return true;

        default: break;
    }

    const char32_t letter = what.key().letter();

    if (letter == U' ') {
        const DiffHunk* got = kept.fetched(at);

        if (got == nullptr || !got->tickable) return true;

        if (chosen_.contains(at))
            chosen_.remove(at);
        else
            chosen_.add(at);

        kept.fresh = true;

        Node::changed();

        (void)trigger(PickEvent{});

        return true;
    }

    // `+` and `-` set the whole section of the cursor: from the header before it to the next header.
    if (letter == U'+' || letter == U'-') {
        std::size_t start = at;

        while (start > 0 && !heading(start - 1)) --start;

        bool changed = false;

        for (std::size_t one = start; one < kept.hunks && !(one > at && heading(one)); ++one) {
            const DiffHunk* got = kept.fetched(one);

            if (got == nullptr || !got->tickable || chosen_.contains(one) == (letter == U'+')) continue;

            if (letter == U'+')
                chosen_.add(one);
            else
                chosen_.remove(one);

            changed = true;
        }

        if (changed) {
            kept.fresh = true;

            Node::changed();

            (void)trigger(PickEvent{});
        }

        return true;
    }

    return false;
}

// The wheel moves the view itself and leaves the cursor, as in the table. The view does not scroll
// sideways, so the tilted wheel goes on up.
bool DiffViewer::handle(WheelEvent& what) {
    if (what.axis() != Axis::vertical) return false;

    return slid(-what.steps() * stepping, Moved::by_hand);
}

// The mouse on the bar: the track scrolls a page, the thumb jumps to a hunk. The
// bar counts hunks, so a dragged thumb puts the view at the start of a hunk, as Home does.
bool DiffViewer::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    const Asked asked = kept.grip.mouse(what, local(what.where()), *this);

    const int head = kept.left_title.empty() && kept.right_title.empty() ? 0 : 1;
    const int page = std::max(area().size.high - head, 1);

    switch (asked.part) {
        case Part::none: return false;
        case Part::before: (void)slid(-page, Moved::by_hand); break;
        case Part::after: (void)slid(page, Moved::by_hand); break;
        case Part::thumb:
            if (asked.to.has_value() && kept.hunks > 0) {
                const std::size_t want = std::min(*asked.to, kept.hunks - 1);

                if (want != kept.top || kept.part != 0) {
                    kept.top = want;
                    kept.part = 0;

                    kept.settle();

                    kept.fresh = true;

                    Node::changed();

                    (void)trigger(ScrollEvent{Moved::by_hand});
                }
            }
            break;
    }

    return true;
}

// Moves the view by the given number of rows (positive is down), over hunks and their rows; the
// cursor does not move. False when the view stayed. Only the viewer's own wheel and bar move it
// so, so the event says by hand.
bool DiffViewer::slid(int lines, Moved how) {
    Kept& kept = *kept_;

    const std::size_t was = kept.top;
    const int part = kept.part;

    for (; lines > 0; --lines) {
        const std::optional<int> got = kept.height(kept.top);

        if (!got.has_value()) break;

        if (kept.part + 1 < *got) {
            ++kept.part;
        } else {
            if (kept.top + 1 >= kept.hunks) break;

            ++kept.top;
            kept.part = 0;
        }
    }

    for (; lines < 0; ++lines) {
        if (kept.part > 0) {
            --kept.part;
        } else {
            if (kept.top == 0) break;

            const std::optional<int> got = kept.height(kept.top - 1);

            if (!got.has_value()) break;

            --kept.top;
            kept.part = *got - 1;
        }
    }

    kept.settle();

    if (kept.top == was && kept.part == part) return false;

    kept.fresh = true;

    Node::changed();

    (void)trigger(ScrollEvent{how});

    return true;
}

// A click puts the cursor on the hunk under it; a click in the checkbox toggles it.
bool DiffViewer::handle(ClickEvent& what) {
    Kept& kept = *kept_;

    // A click on the bar did its job on the press; it does not move the cursor.
    if (kept.grip.over(local(what.where()))) return true;

    if (root() != nullptr) focus();

    const int head = kept.left_title.empty() && kept.right_title.empty() ? 0 : 1;
    const Spot at = local(what.where());
    const int y = at.y - head;

    if (y < 0 || static_cast<std::size_t>(y) >= kept.shown.size()) return true;

    const Kept::Shown& one = kept.shown[static_cast<std::size_t>(y)];
    const std::size_t hunk = one.hunk;
    const bool box = one.first && one.data->tickable && at.x < ticking;

    if (one.data->rows.empty()) return true;

    which(hunk, Moved::by_hand);

    if (box) {
        if (chosen_.contains(hunk))
            chosen_.remove(hunk);
        else
            chosen_.add(hunk);

        kept.fresh = true;

        Node::changed();

        (void)trigger(PickEvent{});
    }

    return true;
}

}

#include "tal/diff.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

#include "tal/key.hpp"
#include "tal/text.hpp"

#include "controlling.hpp"

// Differences: of two strings by words, of two texts by lines, and the view that shows them in
// hunks, side by side or in one column. Both comparisons use Myers' algorithm with a bound on the
// differences it follows, so that neither time nor memory depends on what someone hands in: past
// the bound the rest counts as different as a whole.
namespace tal {

namespace {

// --- Comparing ---------------------------------------------------------------------------------

// How many edit steps the comparison of words follows before the strings count as simply
// different: for names far beyond need, for lines the point past which highlighting says nothing.
constexpr int word_patience = 256;

// How many words are compared at all; a longer string is marked as a whole.
constexpr std::size_t word_most = 4096;

// How many edit steps the comparison of lines follows; the memory of the trace grows with its
// square, about four bytes for each step squared.
constexpr int line_patience = 1024;

// The part of what is gone, on the left, and of what came, on the right.
constexpr std::string_view gone_part = "gone";
constexpr std::string_view came_part = "came";

// Whether `sign` belongs to a word: letters and digits, and every byte outside ASCII, which belongs
// to a letter of more bytes, so that a Polish name does not fall apart at a diacritic.
bool wordy(unsigned char sign) { return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z') || sign >= 0x80; }

// `text` cut into pieces: whole words, and every other character on its own. Comparing by
// characters would scatter `.1080p.BluRay` into letters that happen to match the other side.
std::vector<std::string_view> tokenized(std::string_view text) {
    std::vector<std::string_view> made;

    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = at + 1;

        if (wordy(static_cast<unsigned char>(text[at])))
            while (end < text.size() && wordy(static_cast<unsigned char>(text[end]))) ++end;

        made.push_back(text.substr(at, end - at));
        at = end;
    }

    return made;
}

// `text` cut into lines at '\n', each without its '\r' before it; a last '\n' ends the last line
// and starts none.
std::vector<std::string_view> lined(std::string_view text) {
    std::vector<std::string_view> made;

    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end = text.find('\n', at);
        std::string_view one = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);

        if (one.ends_with('\r')) one.remove_suffix(1);

        made.push_back(one);

        if (end == std::string_view::npos) break;

        at = end + 1;
    }

    return made;
}

// The longest common subsequence of `left` and `right` from `from_*` to `upto_*`, by Myers'
// algorithm: a pass over the diagonals with a remembered trace, then the way back from the end,
// marking the common pieces in `kept_*`. The cost grows with the length times the number of
// differences. With more than `patience` differences nothing is marked.
void matched(const std::vector<std::string_view>& left, std::size_t from_left, std::size_t upto_left, const std::vector<std::string_view>& right,
             std::size_t from_right, std::size_t upto_right, std::vector<bool>& kept_left, std::vector<bool>& kept_right, int patience) {
    const long long n = static_cast<long long>(upto_left - from_left);
    const long long m = static_cast<long long>(upto_right - from_right);

    if (n == 0 || m == 0) return;

    const auto same = [&](long long x, long long y) { return left[from_left + static_cast<std::size_t>(x)] == right[from_right + static_cast<std::size_t>(y)]; };
    const long long limit = std::min<long long>(n + m, patience);
    const long long off = limit + 1;
    std::vector<long long> reach(static_cast<std::size_t>(2 * limit + 3), 0);
    std::vector<std::vector<long long>> trace;

    for (long long d = 0; d <= limit; ++d) {
        for (long long k = -d; k <= d; k += 2) {
            const std::size_t here = static_cast<std::size_t>(off + k);
            long long x = (k == -d || (k != d && reach[here - 1] < reach[here + 1])) ? reach[here + 1] : reach[here - 1] + 1;
            long long y = x - k;

            while (x < n && y < m && same(x, y)) {
                ++x;
                ++y;
            }

            reach[here] = x;

            if (x < n || y < m) continue;

            long long bx = n;
            long long by = m;

            for (long long back = d; back > 0; --back) {
                const std::vector<long long>& before = trace[static_cast<std::size_t>(back - 1)];
                const auto at = [&before, back](long long j) { return before[static_cast<std::size_t>(j + back - 1)]; };
                const long long kk = bx - by;
                const bool down = kk == -back || (kk != back && at(kk - 1) < at(kk + 1));
                const long long pk = down ? kk + 1 : kk - 1;
                const long long px = at(pk);
                const long long py = px - pk;

                while (bx > px && by > py) {
                    kept_left[from_left + static_cast<std::size_t>(bx - 1)] = true;
                    kept_right[from_right + static_cast<std::size_t>(by - 1)] = true;
                    --bx;
                    --by;
                }

                bx = px;
                by = py;
            }

            // There is no diagonal before the first edit: the caller has cut off the common start,
            // so the first pieces of both sides differ.
            return;
        }

        trace.emplace_back(reach.begin() + off - d, reach.begin() + off + d + 1);
    }
}

// Which pieces of `left` and of `right` are common: the common start and end at once, the middle by
// Myers when it has at most `most` pieces and at most `patience` differences; otherwise the whole
// middle differs, since highlighting half of it would say nothing.
std::pair<std::vector<bool>, std::vector<bool>> kept(const std::vector<std::string_view>& left, const std::vector<std::string_view>& right, int patience,
                                                     std::size_t most) {
    std::vector<bool> ones(left.size(), false);
    std::vector<bool> twos(right.size(), false);
    std::size_t head = 0;

    while (head < left.size() && head < right.size() && left[head] == right[head]) {
        ones[head] = true;
        twos[head] = true;
        ++head;
    }

    std::size_t tail = 0;

    while (tail < left.size() - head && tail < right.size() - head && left[left.size() - 1 - tail] == right[right.size() - 1 - tail]) {
        ones[left.size() - 1 - tail] = true;
        twos[right.size() - 1 - tail] = true;
        ++tail;
    }

    const std::size_t upto_one = left.size() - tail;
    const std::size_t upto_two = right.size() - tail;

    if (upto_one - head + upto_two - head <= most) matched(left, head, upto_one, right, head, upto_two, ones, twos, patience);

    return {std::move(ones), std::move(twos)};
}

// The bytes of the pieces of `text` that are not kept, merged where they touch, as marks of the
// part `part`.
std::vector<Mark> spanned(const std::vector<std::string_view>& pieces, const std::vector<bool>& common, std::string_view text, std::string_view part) {
    std::vector<Mark> made;

    for (std::size_t one = 0; one < pieces.size(); ++one) {
        if (common[one]) continue;

        const auto from = static_cast<std::size_t>(pieces[one].data() - text.data());
        const std::size_t upto = from + pieces[one].size();

        if (!made.empty() && made.back().upto == from) made.back().upto = upto;
        else made.push_back(Mark{from, upto, std::string{part}, {}});
    }

    return made;
}

// A side of a line as it stands, with its number.
Side numbered(std::string_view text, std::size_t number) { return Side{std::string{text}, {}, number}; }

}

// The difference of two strings by words: the marks of what is gone on the left side (part `gone`)
// and of what came on the right (part `came`), whole words, and every other character alone.
std::pair<Side, Side> diffed(std::string_view left, std::string_view right) {
    std::pair<Side, Side> made{Side{std::string{left}, {}, {}}, Side{std::string{right}, {}, {}}};
    const std::vector<std::string_view> ones = tokenized(made.first.text);
    const std::vector<std::string_view> twos = tokenized(made.second.text);
    const auto [one, two] = kept(ones, twos, word_patience, word_most);

    made.first.marks = spanned(ones, one, made.first.text, gone_part);
    made.second.marks = spanned(twos, two, made.second.text, came_part);

    return made;
}

// The difference of two texts by lines, in hunks: each change with `context` common lines around
// it, changes whose contexts meet in one hunk, every line with its number from 1. A run of lines
// gone and a run that came in their place stand side by side, row by row, their words compared
// (`diffed`); what is left over of the longer run stands alone. Lines end at '\n', a '\r' before it
// is not part of the line, and a missing '\n' at the end makes no difference. Equal texts give no
// hunks.
std::vector<Hunk> compared(std::string_view left, std::string_view right, int context) {
    const std::vector<std::string_view> ones = lined(left);
    const std::vector<std::string_view> twos = lined(right);
    const auto [one, two] = kept(ones, twos, line_patience, std::numeric_limits<std::size_t>::max());

    enum class Fate { same, gone, came };

    struct Op {
        Fate step;
        std::size_t left;
        std::size_t right;
    };

    std::vector<Op> ops;

    for (std::size_t i = 0, j = 0; i < ones.size() || j < twos.size();) {
        if (i < ones.size() && !one[i]) ops.push_back(Op{Fate::gone, i++, j});
        else if (j < twos.size() && !two[j]) ops.push_back(Op{Fate::came, i, j++});
        else ops.push_back(Op{Fate::same, i++, j++});
    }

    const std::size_t around = static_cast<std::size_t>(std::max(context, 0));
    std::vector<Hunk> made;

    for (std::size_t at = 0; at < ops.size();) {
        if (ops[at].step == Fate::same) {
            ++at;

            continue;
        }

        std::size_t start = at >= around ? at - around : 0;
        std::size_t end = at;

        // The hunk goes on while the next change lies within twice the context.
        for (;;) {
            while (end < ops.size() && ops[end].step != Fate::same) ++end;

            std::size_t gap = end;

            while (gap < ops.size() && ops[gap].step == Fate::same && gap - end <= 2 * around) ++gap;

            if (gap < ops.size() && ops[gap].step != Fate::same && gap - end <= 2 * around) end = gap;
            else break;
        }

        const std::size_t stop = std::min(ops.size(), end + std::min(around, ops.size() - end));
        Hunk hunk;

        for (std::size_t here = start; here < stop;) {
            if (ops[here].step == Fate::same) {
                hunk.rows.push_back(Pair{numbered(ones[ops[here].left], ops[here].left + 1), numbered(twos[ops[here].right], ops[here].right + 1)});
                ++here;

                continue;
            }

            std::vector<std::size_t> gones;
            std::vector<std::size_t> cames;

            for (; here < stop && ops[here].step != Fate::same; ++here) {
                if (ops[here].step == Fate::gone) gones.push_back(ops[here].left);
                else cames.push_back(ops[here].right);
            }

            for (std::size_t row = 0; row < std::max(gones.size(), cames.size()); ++row) {
                Pair pair;

                if (row < gones.size() && row < cames.size()) {
                    auto [was, now] = diffed(ones[gones[row]], twos[cames[row]]);

                    was.number = gones[row] + 1;
                    now.number = cames[row] + 1;
                    pair = Pair{std::move(was), std::move(now)};
                } else if (row < gones.size()) {
                    Side was = numbered(ones[gones[row]], gones[row] + 1);

                    if (!was.text.empty()) was.marks.push_back(Mark{0, was.text.size(), std::string{gone_part}, {}});

                    pair.left = std::move(was);
                } else {
                    Side now = numbered(twos[cames[row]], cames[row] + 1);

                    if (!now.text.empty()) now.marks.push_back(Mark{0, now.text.size(), std::string{came_part}, {}});

                    pair.right = std::move(now);
                }

                hunk.rows.push_back(std::move(pair));
            }
        }

        made.push_back(std::move(hunk));
        at = stop;
    }

    return made;
}

// A model of hunks is told about by the hooks of its views; nothing to let go of but them.
Hunks::~Hunks() = default;

// Tells every view what changed, in the order they started listening; a handler whose hook went
// meanwhile is skipped.
void Hunks::told(Changes what) {
    const std::vector<std::shared_ptr<detail::Slot>> called = slots_;

    for (const std::shared_ptr<detail::Slot>& slot : called)
        if (slot->live) slot->called(what);
}

// Keeps the handler `slot`; the hook returned removes it.
Hook Hunks::hooked(std::shared_ptr<detail::Slot> slot) {
    std::erase_if(slots_, [](const std::shared_ptr<detail::Slot>& one) { return !one->live; });

    std::weak_ptr<detail::Slot> held = slot;

    slots_.push_back(std::move(slot));

    return Hook{std::move(held)};
}

namespace {

// --- The view ------------------------------------------------------------------------------------

// How many rows a step of the wheel moves the view, as in a list.
constexpr long long wheeled = 3;

// The box of a hunk that can be ticked: `[x] `, four cells, as in `Tick`.
constexpr int ticking = 4;

// The separator between the sides: space, line, space.
constexpr int parting = 3;

// Below this many cells a side, the view takes one column by itself.
constexpr int narrow = 24;

// The columns a tab stands for.
constexpr int tabbing = 8;

// What a row of the screen shows of its hunk: the title, a row with both sides, or in one column
// one side of a row whose sides differ.
enum class Face { title, both, left, right };

// The border and padding of a node of style `style` on each side.
Sides boxed(const Style& style) {
    const int line = style.border == Border::none ? 0 : 1;

    return Sides{style.padding.top + line, style.padding.right + line, style.padding.bottom + line, style.padding.left + line};
}

// Whether both sides of `one` stand and are equal, with nothing marked: such a row shows once.
bool same(const Pair& one) { return one.left && one.right && one.left->marks.empty() && one.right->marks.empty() && one.left->text == one.right->text; }

// `one` with its tabs as spaces to the next stop, its marks moved along.
Side expanded(const Side& one) {
    if (one.text.find('\t') == std::string::npos) return one;

    Side made{{}, one.marks, one.number};
    std::vector<std::size_t> moved(one.text.size() + 1, 0);
    int column = 0;

    for (std::size_t at = 0; at < one.text.size(); ++at) {
        moved[at] = made.text.size();

        if (one.text[at] == '\t') {
            const int stop = (column / tabbing + 1) * tabbing;

            made.text.append(static_cast<std::size_t>(stop - column), ' ');
            column = stop;
        } else {
            made.text += one.text[at];

            if ((static_cast<unsigned char>(one.text[at]) & 0xC0) != 0x80) ++column;
        }
    }

    moved[one.text.size()] = made.text.size();

    for (Mark& mark : made.marks) {
        mark.from = moved[std::min(mark.from, one.text.size())];
        mark.upto = moved[std::min(mark.upto, one.text.size())];
    }

    return made;
}

// How many decimal digits `number` has.
int digits(std::size_t number) {
    int made = 1;

    for (; number >= 10; number /= 10) ++made;

    return made;
}

}

// A row of the screen: the hunk it belongs to, what it shows and which row of the hunk, and
// whether it is the first row of its hunk.
struct Diff::Stripe {
    std::size_t hunk = 0;
    Face face = Face::both;
    std::size_t row = 0;
    bool first = false;
};

// A view of the hunks of `hunks`, in the class "diff": a row of titles over the hunks, which show
// side by side or, where a side would be narrower than 24 cells, in one column. The arrows, the
// page keys, Home and End move the current hunk over those with rows, Ctrl with an arrow over
// sections (a hunk with a title alone heads one); the right arrow opens a hunk and the left closes
// it; Space ticks a hunk that can be ticked, `+` and `-` the whole section. A click makes a hunk
// current, a click in its box ticks it, a click on its sign of folding opens or closes it, and the
// wheel moves the view. Enter and Tab go on to the window.
Diff::Diff(std::shared_ptr<Hunks> hunks) : hunks_(std::move(hunks)) {
    if (hunks_ == nullptr) throw std::invalid_argument{"tal::Diff: no hunks"};

    classes({"diff"});
    focusable(true);

    told_ = hunks_->on<Changes>([this](Changes& what) { changed(what); });

    steadied();

    hooks_.add(on<Changed>([this](Changed& what) {
        if (what.of(current)) {
            reveal(*current);
            repaint();
        }
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const std::size_t many = extent();

        if (many == 0 || what.key.mods().alt || what.key.mods().shift) return;

        const std::size_t here = std::min(*current, many - 1);

        if (what.key.mods().ctrl) {
            const Code code = what.key.code();

            if (code != Code::down && code != Code::up) return;

            what.take();

            if (code == Code::down) {
                for (std::size_t one = here + 1; one < many; ++one)
                    if (heading(one))
                        if (const std::optional<std::size_t> got = next(one, true)) {
                            current = *got;

                            return;
                        }

                return;
            }

            std::size_t start = here;

            while (start > 0 && !heading(start - 1)) --start;

            if (start == here && start > 0) {
                --start;

                while (start > 0 && !heading(start - 1)) --start;
            }

            if (steady(start)) current = start;
            else if (const std::optional<std::size_t> got = next(start, true); got && *got < here) current = *got;

            return;
        }

        const auto moved = [this, here](bool down, std::size_t times) {
            std::size_t at = here;

            for (std::size_t step = 0; step < times; ++step) {
                const std::optional<std::size_t> got = next(at, down);

                if (!got) break;

                at = *got;
            }

            current = at;
        };

        const std::size_t page = std::max<std::size_t>(static_cast<std::size_t>(std::max(rows_, 1)), 1);

        switch (what.key.code()) {
            case Code::up: moved(false, 1); break;
            case Code::down: moved(true, 1); break;
            case Code::pageup: moved(false, page); break;
            case Code::pagedown: moved(true, page); break;
            case Code::home: current = steady(0) ? 0 : next(0, true).value_or(here); break;
            case Code::end: current = steady(many - 1) ? many - 1 : next(many - 1, false).value_or(here); break;
            case Code::right: open(here, true); break;
            case Code::left: open(here, false); break;
            default: {
                const char32_t letter = what.key.letter();

                if (letter == U' ') {
                    if (hunks_->hunk(here).tickable) ticked(here, !marked(here));
                } else if (letter == U'+' || letter == U'-') {
                    section(letter == U'+');
                } else {
                    return;
                }
            }
        }

        what.take();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press == Press::wheel) {
            if (what.axis != Axis::vertical) return;

            const std::size_t was = top_;
            const int part = part_;

            slide(-wheeled * what.steps);

            if (top_ != was || part_ != part) what.take();

            return;
        }

        if (what.button != MouseButton::left || what.press != Press::click) return;

        const Sides in = inset();
        const int head = left->empty() && right->empty() ? 0 : 1;
        const int y = what.at.y - in.top - head;
        const int x = what.at.x - in.left;
        const std::vector<Stripe> shown = laid();

        if (y < 0 || static_cast<std::size_t>(y) >= shown.size() || x < 0 || x >= wide_) return;

        what.take();

        const Stripe& one = shown[static_cast<std::size_t>(y)];
        const Hunk hunk = hunks_->hunk(one.hunk);

        if (hunk.rows.empty()) return;

        current = one.hunk;

        if (one.first && hunk.tickable && ticks_ && x < ticking) {
            ticked(one.hunk, !marked(one.hunk));

            return;
        }

        // The sign of folding stands at the end of the first row's left side, or of the one
        // column: a click there opens or closes the hunk.
        if (one.face != Face::title && one.face != Face::right && one.row == 0 && hunk.rows.size() > 1) {
            const auto [from, upto] = folding(shown, one.hunk);

            if (x >= from && x < upto) open(one.hunk, !opened(one.hunk));
        }
    }));
}

// Opens the hunk `hunk` (`on`) to show all its rows, or closes it to its first row; a hunk past the
// last throws std::out_of_range. An opened hunk comes into the view with as many rows as fit.
void Diff::open(std::size_t hunk, bool on) {
    if (hunk >= extent()) throw std::out_of_range{"tal::Diff::open: no such hunk"};

    const auto at = std::lower_bound(opened_.begin(), opened_.end(), hunk);
    const bool was = at != opened_.end() && *at == hunk;

    if (was == on) return;

    if (on) opened_.insert(at, hunk);
    else opened_.erase(at);

    reveal(hunk);

    if (on && hunk >= top_) {
        int before = -part_;

        for (std::size_t one = top_; one < hunk; ++one) before += static_cast<int>(stripes(one).size());

        const int over = before + static_cast<int>(stripes(hunk).size()) - rows_;

        if (over > 0 && before > 0) slide(std::min(over, before));
    }

    repaint();
    scrolled();
}

// Whether the hunk `hunk` shows all its rows.
bool Diff::opened(std::size_t hunk) const { return std::binary_search(opened_.begin(), opened_.end(), hunk); }

// Ticks the hunk `hunk` (`on`) or takes its tick away; a hunk past the last throws
// std::out_of_range. The program's own ticks tell nothing.
void Diff::mark(std::size_t hunk, bool on) {
    if (hunk >= extent()) throw std::out_of_range{"tal::Diff::mark: no such hunk"};

    const auto at = std::lower_bound(marks_.begin(), marks_.end(), hunk);
    const bool was = at != marks_.end() && *at == hunk;

    if (was == on) return;

    if (on) marks_.insert(at, hunk);
    else marks_.erase(at);

    repaint();
}

// Whether the hunk `hunk` is ticked.
bool Diff::marked(std::size_t hunk) const { return std::binary_search(marks_.begin(), marks_.end(), hunk); }

// Takes every tick away.
void Diff::unmark() {
    if (marks_.empty()) return;

    marks_.clear();
    repaint();
}

// How many hunks the model has: the view scrolls by hunks.
std::size_t Diff::extent() const { return hunks_->count(); }

// Shows the hunks from `hunk` on, from its first row, as far as the end lets the view stay full;
// the view tells `Scrolled`.
void Diff::top(std::size_t hunk) {
    const std::size_t many = extent();
    const std::size_t was = top_;
    const int part = part_;

    top_ = many == 0 ? 0 : std::min(hunk, many - 1);
    part_ = 0;
    settle();

    if (top_ == was && part_ == part) return;

    repaint();
    scrolled();
}

// The least room: the layout gives the view its size, as to a list.
Size Diff::measure(Limits room) const { return room.least; }

// Takes the inside: its width decides between two columns and one, its height the rows for hunks
// under the titles. The box column appears once a hunk that can be ticked was seen, and stays
// until a reset, so that the text does not jump sideways while scrolling. The current hunk moves
// to one with rows, the view keeps inside the hunks, and `Scrolled` goes when the window moved.
void Diff::arrange(Area inside) {
    const std::size_t many = extent();
    const std::size_t was = window_;
    const std::size_t from = top_;

    wide_ = std::max(inside.size.wide, 0);
    rows_ = std::max(inside.size.high - (left->empty() && right->empty() ? 0 : 1), 0);

    for (std::size_t one = top_; one < many && one < top_ + static_cast<std::size_t>(rows_) + 1 && !ticks_; ++one)
        if (hunks_->hunk(one).tickable) ticks_ = true;

    const int ticks = ticks_ ? ticking : 0;

    unify_ = unified->value_or((wide_ - ticks - parting) / 2 < narrow);

    settle();

    std::size_t shown = 0;
    std::optional<std::size_t> last;

    for (const Stripe& one : laid())
        if (last != one.hunk) {
            ++shown;
            last = one.hunk;
        }

    window_ = shown;

    if (window_ != was || top_ != from) scrolled();
}

// The titles, then the rows of the hunks from the view's first: the box, the line numbers, both
// sides with a line between them, or one column with "-" and "+" where the sides differ; what is
// gone and what came in the looks of their parts, the words that differ in the state `mark`; the
// first row of the current hunk in the state `focus` while the view has the focus; at the end of a
// hunk's first row its sign of folding, with "+N" for the rows it hides.
void Diff::paint(Canvas& onto) const {
    const Area seen = onto.seen();
    const bool lit = states().has(State::focus);
    const std::vector<Stripe> shown = laid();
    std::map<std::size_t, Hunk> held;
    States ground;

    ground.set(State::disabled, states().has(State::disabled));
    onto.fill(seen, look({}, ground));

    for (const Stripe& one : shown)
        if (!held.contains(one.hunk)) held.emplace(one.hunk, hunks_->hunk(one.hunk));

    const int number = numbers(shown);
    const int ticks = ticks_ ? ticking : 0;
    const int gutter = number > 0 ? number + 1 : 0;
    const int side = unify_ ? wide_ - ticks - 2 * gutter - 2 : (wide_ - ticks - parting) / 2 - gutter;
    const int right_at = ticks + gutter + side + parting;
    const std::string_view dots = glyph("elide");
    const auto write = [&](Canvas& line, int x, int room, const Side& said, std::string_view part, States states) {
        if (room <= 0) return;

        const Side one = expanded(said);
        std::vector<Look> marked;
        States lit_mark = states;

        lit_mark.set(State::mark, true);

        for (std::size_t at = 0; at < one.marks.size(); ++at) marked.push_back(look(one.marks[at].part.empty() ? part : one.marks[at].part, lit_mark));

        (void)controlling::line(line, {x, 0}, room, one.text, 0, one.text.size(), one.marks, marked, Elide::end, Edge::start, look(part, states), dots,
                                rendering());
    };
    const auto numbered = [&](Canvas& line, int x, const std::optional<Side>& one, States states) {
        if (number == 0 || !one || !one->number) return;

        std::string said = std::to_string(*one->number);

        said.insert(0, static_cast<std::size_t>(number) - said.size(), ' ');
        (void)line.put({x, 0}, said, look("number", states));
    };

    int y = 0;

    if (!left->empty() || !right->empty()) {
        Canvas line = onto.within(Area{{0, y}, {wide_, 1}});
        const Look head = look("head", ground);

        line.fill(Area{{0, 0}, line.size()}, head);

        if (unify_) {
            const std::string both = *left + " → " + *right;

            (void)controlling::line(line, {ticks, 0}, wide_ - ticks, both, 0, both.size(), {}, {}, Elide::end, Edge::start, head, dots, rendering());
        } else {
            (void)controlling::line(line, {ticks + gutter, 0}, side, *left, 0, left->size(), {}, {}, Elide::end, Edge::start, head, dots, rendering());
            (void)controlling::line(line, {right_at + gutter, 0}, side, *right, 0, right->size(), {}, {}, Elide::end, Edge::start, head, dots, rendering());
        }

        ++y;
    }

    for (const Stripe& one : shown) {
        const Hunk& hunk = held.at(one.hunk);
        Canvas line = onto.within(Area{{0, y}, {wide_, 1}});
        States states = ground;

        states.set(State::focus, lit && one.first && one.hunk == *current);
        line.fill(Area{{0, 0}, line.size()}, look({}, states));

        if (one.first && hunk.tickable && ticks_) (void)line.put({0, 0}, marked(one.hunk) ? "[x] " : "[ ] ", look({}, states));

        if (one.face == Face::title) {
            (void)controlling::line(line, {ticks, 0}, wide_ - ticks, hunk.title, 0, hunk.title.size(), {}, {}, Elide::end, Edge::start,
                                    look("title", states), dots, rendering());
            ++y;

            continue;
        }

        const Pair& row = hunk.rows[one.row];
        const bool even = same(row);
        std::string folded;

        if (one.row == 0 && hunk.rows.size() > 1)
            folded = opened(one.hunk) ? " " + std::string{glyph("diff.open")}
                                      : " +" + std::to_string(hunk.rows.size() - 1) + " " + std::string{glyph("diff.closed")};

        const int tag = text::width(folded, rendering());

        if (unify_) {
            const int text_at = ticks + 2 * gutter;
            const bool gone = one.face == Face::left || (one.face == Face::both && !row.right);
            const Side& said = gone ? *row.left : *row.right;
            const std::string_view part = even ? std::string_view{} : gone ? gone_part : came_part;

            if (one.face != Face::right) numbered(line, ticks, row.left, states);
            if (one.face != Face::left) numbered(line, ticks + gutter, row.right, states);

            (void)line.put({text_at, 0}, even ? "  " : gone ? "- " : "+ ", look(part, states));
            write(line, text_at + 2, side - tag, said, part, states);

            if (tag > 0 && one.face != Face::right) (void)line.put({text_at + 2 + side - tag, 0}, folded, look("number", states));
        } else {
            numbered(line, ticks, row.left, states);

            if (row.left) write(line, ticks + gutter, side - tag, *row.left, even ? std::string_view{} : gone_part, states);

            if (tag > 0) (void)line.put({ticks + gutter + side - tag, 0}, folded, look("number", states));

            (void)line.put({ticks + gutter + side + 1, 0}, glyph("line.thin.upright"), look({}, states));
            numbered(line, right_at, row.right, states);

            if (row.right) write(line, right_at + gutter, side, *row.right, even ? std::string_view{} : came_part, states);
        }

        ++y;
    }
}

// The rows of the screen the hunk `hunk` takes: its title when it has one, then its first row, or
// all rows when it is open; in one column a row whose sides differ takes a row for each side. A
// hunk with neither title nor rows still takes a row, so that it can be seen.
std::vector<Diff::Stripe> Diff::stripes(std::size_t hunk) const {
    const Hunk one = hunks_->hunk(hunk);
    std::vector<Stripe> made;

    if (!one.title.empty()) made.push_back(Stripe{hunk, Face::title, 0, true});

    const std::size_t shown = opened(hunk) ? one.rows.size() : std::min<std::size_t>(one.rows.size(), 1);

    for (std::size_t row = 0; row < shown; ++row) {
        const Pair& pair = one.rows[row];

        if (!unify_ || same(pair)) {
            made.push_back(Stripe{hunk, Face::both, row, made.empty()});

            continue;
        }

        if (pair.left) made.push_back(Stripe{hunk, Face::left, row, made.empty()});
        if (pair.right) made.push_back(Stripe{hunk, Face::right, row, made.empty()});
    }

    if (made.empty()) made.push_back(Stripe{hunk, Face::title, 0, true});

    return made;
}

// The rows of the screen from the view's first, as many as the view has.
std::vector<Diff::Stripe> Diff::laid() const {
    std::vector<Stripe> made;
    const std::size_t many = extent();
    int skip = part_;

    for (std::size_t one = top_; one < many && static_cast<int>(made.size()) < rows_; ++one) {
        for (const Stripe& each : stripes(one)) {
            if (skip > 0) {
                --skip;

                continue;
            }

            if (static_cast<int>(made.size()) >= rows_) break;

            made.push_back(each);
        }

        skip = 0;
    }

    return made;
}

// Whether the cursor may stand on the hunk `hunk`: one with rows; a title alone heads a section.
bool Diff::steady(std::size_t hunk) const { return hunk < extent() && !hunks_->hunk(hunk).rows.empty(); }

// The next hunk with rows after `from` (`down`) or before it, `from` excluded, or none.
std::optional<std::size_t> Diff::next(std::size_t from, bool down) const {
    const std::size_t many = extent();

    for (std::size_t one = from; down ? one + 1 < many : one > 0;) {
        one = down ? one + 1 : one - 1;

        if (steady(one)) return one;
    }

    return std::nullopt;
}

// Whether the hunk `hunk` heads a section: a title without rows.
bool Diff::heading(std::size_t hunk) const {
    if (hunk >= extent()) return false;

    const Hunk one = hunks_->hunk(hunk);

    return one.rows.empty() && !one.title.empty();
}

// How many digits the line numbers of the rows `shown` take: those of the largest, or none when no
// side of them has a number.
int Diff::numbers(const std::vector<Stripe>& shown) const {
    int made = 0;
    std::map<std::size_t, Hunk> held;

    for (const Stripe& one : shown) {
        if (one.face == Face::title) continue;

        if (!held.contains(one.hunk)) held.emplace(one.hunk, hunks_->hunk(one.hunk));

        const Pair& row = held.at(one.hunk).rows[one.row];

        for (const std::optional<Side>& side : {row.left, row.right})
            if (side && side->number) made = std::max(made, digits(*side->number));
    }

    return made;
}

// The columns, from and past, of the sign of folding of the hunk `hunk` among the rows `shown`, as
// `paint` draws it; none when the hunk has fewer than two rows.
std::pair<int, int> Diff::folding(const std::vector<Stripe>& shown, std::size_t hunk) const {
    const Hunk one = hunks_->hunk(hunk);

    if (one.rows.size() < 2) return {0, 0};

    const std::string folded = opened(hunk) ? " " + std::string{glyph("diff.open")}
                                            : " +" + std::to_string(one.rows.size() - 1) + " " + std::string{glyph("diff.closed")};
    const int tag = text::width(folded, rendering());
    const int number = numbers(shown);
    const int ticks = ticks_ ? ticking : 0;
    const int gutter = number > 0 ? number + 1 : 0;
    const int side = unify_ ? wide_ - ticks - 2 * gutter - 2 : (wide_ - ticks - parting) / 2 - gutter;
    const int end = unify_ ? ticks + 2 * gutter + 2 + side : ticks + gutter + side;

    return {end - tag, end};
}

// Moves the current hunk to one with rows when it stands on a heading or past the end: the next
// one, or the one before when none follows.
void Diff::steadied() {
    const std::size_t many = extent();

    if (many == 0) return;

    const std::size_t here = std::min(*current, many - 1);

    if (steady(here)) {
        if (here != *current) current = here;

        return;
    }

    if (const std::optional<std::size_t> found = next(here, true)) current = *found;
    else if (const std::optional<std::size_t> back = next(here, false)) current = *back;
}

// The border and padding: how far the inside lies from the edges of the box.
Sides Diff::inset() const { return boxed(style()); }

// Keeps the view from going past the end: when fewer rows than the view has stand from its first
// row to the end, it moves back until the last row stands at the bottom.
void Diff::settle() {
    const std::size_t many = extent();

    if (many == 0) {
        top_ = 0;
        part_ = 0;

        return;
    }

    top_ = std::min(top_, many - 1);
    part_ = std::min(part_, std::max(static_cast<int>(stripes(top_).size()) - 1, 0));

    if (rows_ <= 0) return;

    int rows = -part_;

    for (std::size_t one = top_; one < many && rows < rows_; ++one) rows += static_cast<int>(stripes(one).size());

    while ((top_ > 0 || part_ > 0) && rows < rows_) {
        if (part_ > 0) {
            --part_;
        } else {
            --top_;
            part_ = static_cast<int>(stripes(top_).size()) - 1;
        }

        ++rows;
    }
}

// Brings the first row of the hunk `hunk` into the view: one above lands at the top, one below at
// the bottom, with as much of it as fits.
void Diff::reveal(std::size_t hunk) {
    const std::size_t many = extent();

    if (many == 0 || rows_ <= 0) return;

    hunk = std::min(hunk, many - 1);

    const std::size_t was = top_;
    const int part = part_;

    if (hunk < top_ || (hunk == top_ && part_ > 0)) {
        top_ = hunk;
        part_ = 0;
    } else {
        int before = -part_;

        for (std::size_t one = top_; one < hunk && before < rows_; ++one) before += static_cast<int>(stripes(one).size());

        if (before >= rows_) {
            top_ = hunk;
            part_ = 0;

            for (int need = rows_ - static_cast<int>(stripes(hunk).size()); need > 0 && (top_ > 0 || part_ > 0); --need) {
                if (part_ > 0) {
                    --part_;
                } else {
                    --top_;
                    part_ = static_cast<int>(stripes(top_).size()) - 1;
                }
            }
        }
    }

    settle();

    if (top_ != was || part_ != part) scrolled();
}

// Moves the view by `rows` rows of the screen (down when positive) over the hunks and their rows;
// the current hunk stays.
void Diff::slide(long long rows) {
    const std::size_t many = extent();

    if (many == 0) return;

    const std::size_t was = top_;
    const int part = part_;

    for (; rows > 0; --rows) {
        if (part_ + 1 < static_cast<int>(stripes(top_).size())) {
            ++part_;
        } else {
            if (top_ + 1 >= many) break;

            ++top_;
            part_ = 0;
        }
    }

    for (; rows < 0; ++rows) {
        if (part_ > 0) {
            --part_;
        } else {
            if (top_ == 0) break;

            --top_;
            part_ = static_cast<int>(stripes(top_).size()) - 1;
        }
    }

    settle();

    if (top_ == was && part_ == part) return;

    repaint();
    scrolled();
}

// The person ticked the hunk `hunk` (`on`) or took its tick away: marked, painted and told.
void Diff::ticked(std::size_t hunk, bool on) {
    mark(hunk, on);

    Ticked one{hunk, on};

    notified(one);
}

// Ticks (`on`) or unticks every hunk of the current one's section that can be ticked: from the
// heading before it to the next heading.
void Diff::section(bool on) {
    const std::size_t many = extent();
    const std::size_t here = std::min(*current, many - 1);
    std::size_t start = here;

    while (start > 0 && !heading(start - 1)) --start;

    for (std::size_t one = start; one < many && !(one > here && heading(one)); ++one)
        if (hunks_->hunk(one).tickable && marked(one) != on) ticked(one, on);
}

// Follows what the model changed. Hunks that come or go move the view, the current hunk, the open
// ones and the ticks along, so that the screen stays; a reset takes the ticks and the open hunks
// away and forgets the box column.
void Diff::changed(const Changes& what) {
    const std::size_t from = what.from;
    const std::size_t count = what.count;
    const auto shifted = [&](std::vector<std::size_t>& some) {
        if (what.kind == Changes::Kind::inserted) {
            for (std::size_t& one : some)
                if (one >= from) one += count;
        } else if (what.kind == Changes::Kind::removed) {
            std::erase_if(some, [from, count](std::size_t one) { return one >= from && one < from + count; });

            for (std::size_t& one : some)
                if (one >= from + count) one -= count;
        }
    };

    switch (what.kind) {
        case Changes::Kind::inserted:
            shifted(marks_);
            shifted(opened_);

            if (from <= top_ && top_ + count < extent()) top_ += count;

            if (from <= *current && *current + count < extent()) current = *current + count;

            break;

        case Changes::Kind::removed:
            shifted(marks_);
            shifted(opened_);

            if (top_ >= from + count) {
                top_ -= count;
            } else if (top_ >= from) {
                top_ = from;
                part_ = 0;
            }

            if (*current >= from + count) current = *current - count;
            else if (*current >= from) current = from;

            break;

        case Changes::Kind::changed: break;

        case Changes::Kind::reset:
            marks_.clear();
            opened_.clear();
            ticks_ = false;

            break;
    }

    steadied();
    settle();
    repaint();
    scrolled();
}

// Tells the view's bar that the hunks, the window or the view moved.
void Diff::scrolled() {
    Scrolled one;

    notified(one);
}

}

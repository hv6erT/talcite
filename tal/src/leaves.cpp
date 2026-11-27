#include "tal/leaves.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

#include "tal/document.hpp"
#include "tal/key.hpp"
#include "tal/trouble.hpp"

#include "mosaicking.hpp"

// The pages of a document in the tree, as viewers of PDF show them: one under another with a row
// between them (`Layout::continuous`), or one at a time (`Layout::single`). A page is as wide as
// the view (continuous) or fits the view whole (single), times the zoom.
//
// What the engine renders is tiles: squares of 512 pixels of a page at its scale, each asked for
// once and kept, the least used going first when they hold more than `most` bytes, never those the
// view shows now. The part of a page in the view is composed from its tiles and shown by an
// `Image` child, which draws it in pixels (sixel or kitty) or in characters as it draws any
// picture; moving the view composes it anew from the tiles it has, and asks for those it lacks.
// At most two tiles are asked for at a time, the visible first, so that scrolling through a long
// document does not queue the pages it passed; after them, while they fit beside what the cache
// holds, the next view's tiles are asked for ahead.
//
// A page is measured in pixels of a cell where the terminal tells its size, and otherwise in
// pixels of a cell of 1 to 2 at the grain of the characters, as `Image` takes a cell it does not
// know, so that a picture fills its cells exactly. A tile the engine fails to render shows its
// trouble in place of the page, until the view moves and asks again.
//
// Lights (`lights`) mark parts of pages, as found text is marked: they are tinted into the pictures
// as they are composed, so they show in pixels and in characters alike.
namespace tal {

namespace {

constexpr std::array<double, 6> scales{1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
constexpr std::uint32_t side = 512;
constexpr std::size_t flights = 2;
constexpr std::size_t wheeled = 3;
constexpr std::size_t panned = 4;

// `at` moved by `by`, not below zero.
std::size_t stepped(std::size_t at, long long by) {
    if (by < 0) return at > static_cast<std::size_t>(-by) ? at - static_cast<std::size_t>(-by) : 0;

    return at + static_cast<std::size_t>(by);
}

// The pixels of a cell: its size where the terminal tells it, else 1 to 2 at the grain of the
// characters.
Size step_of(const Display& shown) {
    if (shown.cell.wide > 0 && shown.cell.high > 0) return shown.cell;

    const Size grain = mosaicking::grain(shown.blocks, shown.unicode);

    return {grain.wide, 2 * grain.wide};
}

}

// The pages of `document` in the class "leaves", the first in the view. Throws
// `std::invalid_argument` for no document.
Leaves::Leaves(std::shared_ptr<docs::Document> document) : document_(std::move(document)) {
    if (document_ == nullptr) throw std::invalid_argument{"tal::Leaves: no document"};

    pages_ = document_->pages();
    classes({"leaves"});
    focusable(true);

    hooks_.add(on<Changed>([this](Changed& what) {
        if (what.of(zoom) && *zoom >= scales.size()) {
            zoom = scales.size() - 1;

            return;
        }

        if (what.of(zoom) || what.of(layout)) anchored(laid_ == Layout::single);

        if (what.of(most)) trimmed(pinned_);
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const char32_t letter = what.key.letter();
        const bool single = *layout == Layout::single;
        const std::size_t last = pages_.empty() ? 0 : pages_.size() - 1;

        if (letter == U'+' || letter == U'=') {
            zoom = std::min(*zoom + 1, scales.size() - 1);
        } else if (letter == U'-') {
            zoom = *zoom > 0 ? *zoom - 1 : 0;
        } else if (letter == U'0') {
            zoom = 0;
        } else if (letter == U'v') {
            layout = single ? Layout::continuous : Layout::single;
        } else if (letter != 0 || what.key.mods() != Mods{}) {
            return;
        } else {
            const std::size_t page_rows = std::max<std::size_t>(window_ > 1 ? window_ - 1 : 1, 1);

            switch (what.key.code()) {
                case Code::up: top(stepped(top_, -1)); break;
                case Code::down: top(top_ + 1); break;
                case Code::left: left(stepped(left_, -static_cast<long long>(panned))); break;
                case Code::right: left(left_ + panned); break;
                case Code::pageup: single ? page(stepped(current_, -1)) : top(stepped(top_, -static_cast<long long>(page_rows))); break;
                case Code::pagedown: single ? page(std::min(current_ + 1, last)) : top(top_ + page_rows); break;
                case Code::home: page(0); break;
                case Code::end: single ? page(last) : top(extent_); break;
                default: return;
            }
        }

        what.take();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press != Press::wheel) return;

        const long long by = -static_cast<long long>(wheeled) * what.steps;

        if (what.axis == Axis::vertical) {
            top(stepped(top_, by));
        } else {
            left(stepped(left_, by));
        }

        what.take();
    }));
}

// Its work on the pool ends with it: the scope cancels the tiles still asked for.
Leaves::~Leaves() = default;

// How many steps of zoom there are: 1, 1.25, 1.5, 2, 3 and 4 times the page that fits.
std::size_t Leaves::zooms() { return scales.size(); }

// Whether a tile is still being rendered.
bool Leaves::busy() const { return scope_.has_value() && scope_->busy(); }

// Lights the parts `given` of the pages, the current one most: the pictures in the view are
// composed again with them, under a colour that tints what is there as a marker would.
void Leaves::lights(std::vector<Light> given) {
    if (given == lights_) return;

    lights_ = std::move(given);

    for (auto& [page, shown] : shown_) shown.held = {};

    scheduled();
}

// Shows page `which` (the last when there are fewer) from its top, telling `Scrolled` when the
// view moved and `Turned` when it is another page than before.
void Leaves::page(std::size_t which) {
    if (pages_.empty()) return;

    which = std::min(which, pages_.size() - 1);

    if (*layout == Layout::continuous && which < sheets_.size()) return moved(static_cast<std::size_t>(sheets_[which].from));

    const bool other = which != current_;
    const bool up = top_ != 0;

    if (!other && !up && !sheets_.empty()) return;

    current_ = which;
    top_ = 0;
    failed_.clear();

    if (*layout == Layout::continuous) anchor_ = std::pair<std::size_t, double>{which, 0.0};

    repaint();
    scheduled();

    if (other || up) {
        Scrolled one;

        notified(one);
    }

    if (other) {
        Turned one{which};

        notified(one);
    }
}

// The view `column` cells from the left edge of a page wider than it, at most as far as its right
// edge comes into the view: of the widest page continuously, of the page in the view one at a time.
void Leaves::left(std::size_t column) {
    int widest = 0;

    for (std::size_t at = 0; at < sheets_.size(); ++at)
        if (*layout == Layout::continuous || at == current_) widest = std::max(widest, sheets_[at].columns);

    const std::size_t room = widest > view_.wide ? static_cast<std::size_t>(widest - view_.wide) : 0;
    const std::size_t made = std::min(column, room);

    if (made == left_) return;

    left_ = made;
    failed_.clear();
    repaint();
    scheduled();
}

// The view from row `row`: of the document laid out continuously, of the page in the view one at
// a time; at most as far as the end comes into the view.
void Leaves::top(std::size_t row) {
    if (*layout == Layout::continuous) return moved(row);

    const std::size_t made = std::min(row, extent_ > window_ ? extent_ - window_ : 0);

    if (made == top_) return;

    top_ = made;
    failed_.clear();
    repaint();
    scheduled();

    Scrolled one;

    notified(one);
}

// The least room: the layout gives a view its size, not the number of its rows.
Size Leaves::measure(Limits room) const { return room.least; }

// The pages laid out in the room `inside`, kept at the place on the page in the view when the room
// or the pixels of a cell changed; the images of the parts of them in the view placed where they
// show what the view wants, and kept out of sight where they do not yet. The tree cannot change
// while it is laid out, so the images are made, filled and let go after it (`refreshed`, at once
// on the document's clock), and the tiles they lack asked for now. `Scrolled` tells a change of
// the view, the window or the extent.
void Leaves::arrange(Area inside) {
    const std::size_t was_window = window_;
    const std::size_t was_top = top_;
    const std::size_t was_extent = extent_;

    if (!sheets_.empty() && !anchor_ && (inside.size != view_ || step_of(display()) != step_)) anchored(laid_ == Layout::single);

    inside_ = inside;
    sized(inside.size);
    slices_ = slicing();

    bool stale = false;
    std::set<std::size_t> seen;

    for (const Slice& slice : slices_) {
        seen.insert(slice.page);

        const auto shown = shown_.find(slice.page);

        if (shown == shown_.end()) {
            stale = true;
            continue;
        }

        if (showing(slice)) {
            shown->second.image->placed(slice.area);
            continue;
        }

        shown->second.image->placed(Area{slice.area.at, {0, 0}});
        stale = stale || shown->second.image->held() || complete(slice);
    }

    for (auto& [page, shown] : shown_) {
        if (seen.contains(page)) continue;

        shown.image->placed(Area{inside.at, {0, 0}});
        stale = true;
    }

    if (stale) scheduled();

    asked();

    if (window_ != was_window || top_ != was_top || extent_ != was_extent) {
        Scrolled one;

        notified(one);
    }
}

// The parts of the pages in the view as it stands: of each page the cells it shows and where they
// stand in the room.
std::vector<Leaves::Slice> Leaves::slicing() const {
    std::vector<Slice> made;
    const bool single = *layout == Layout::single;

    for (std::size_t page = 0; page < sheets_.size(); ++page) {
        if (single && page != current_) continue;

        if (const std::optional<Slice> one = sliced(page, single ? 0 : sheets_[page].from, top_)) made.push_back(*one);
    }

    return made;
}

// The part of page `page`, which begins at row `from` of the layout, in a view of the room's rows
// from row `top`; nothing when the view does not reach it.
std::optional<Leaves::Slice> Leaves::sliced(std::size_t page, int from, std::size_t top) const {
    const Sheet& sheet = sheets_[page];
    const long long first_row = static_cast<long long>(top) - from;
    const int start = static_cast<int>(std::max<long long>(0, first_row));
    const int end = static_cast<int>(std::min<long long>(sheet.rows, first_row + view_.high));

    if (end <= start) return std::nullopt;

    const bool narrow = sheet.columns <= view_.wide;
    const int first = narrow ? 0 : static_cast<int>(left_);
    const int beyond = narrow ? sheet.columns : std::min(sheet.columns, first + view_.wide);
    const int x = narrow ? (view_.wide - sheet.columns) / 2 : 0;
    const int y = static_cast<int>(from + start - static_cast<long long>(top));
    Slice made;

    made.page = page;
    made.area = Area{{inside_.at.x + x, inside_.at.y + y}, {beyond - first, end - start}};
    made.part = Area{{first, start}, {beyond - first, end - start}};

    return made;
}

// What the image of `slice` must hold to show it: the page, its size in cells, the part of it, the
// pixels of a cell it was composed at.
std::tuple<std::size_t, Size, Area, Size> Leaves::wanting(const Slice& slice) const {
    const Sheet& sheet = sheets_[slice.page];

    return {slice.page, Size{sheet.columns, sheet.rows}, slice.part, step_};
}

// Whether the image of `slice`'s page holds just what the slice shows.
bool Leaves::showing(const Slice& slice) const {
    const auto shown = shown_.find(slice.page);

    return shown != shown_.end() && shown->second.image->held() && shown->second.held == wanting(slice);
}

// Whether the tiles hold all of `slice`.
bool Leaves::complete(const Slice& slice) const {
    const std::vector<Key> keys = covering(slice.page, slice.part);

    return !keys.empty() && std::all_of(keys.begin(), keys.end(), [this](const Key& key) { return tiles_.contains(key); });
}

// The images made and let go for the parts of pages in the view as it stands, each filled with
// what its tiles hold, or emptied while they lack some; the view laid out again with them. Outside
// layout: after it, or when the view or the tiles changed.
void Leaves::refreshed() {
    slices_ = slicing();

    std::set<std::size_t> seen;

    for (const Slice& slice : slices_) {
        seen.insert(slice.page);

        Shown& shown = shown_[slice.page];

        if (shown.image == nullptr) {
            shown.image = std::make_shared<Image>();
            children().add(shown.image);
        }

        if (shown.held == wanting(slice) && shown.image->held()) continue;

        if (std::optional<media::Pixels> made = composed(slice.page, slice.part)) {
            shown.image->hold(std::move(*made));
            shown.held = wanting(slice);
        } else if (shown.image->held()) {
            shown.image->clear();
            shown.held = {};
        }
    }

    for (auto at = shown_.begin(); at != shown_.end();) {
        if (seen.contains(at->first)) {
            ++at;
            continue;
        }

        (void)children().remove(*at->second.image);
        at = shown_.erase(at);
    }

    relayout();
    asked();
}

// A refresh appointed at once on the document's clock, unless one is appointed already; none for
// pages in no document, which refresh when they are laid out in one.
void Leaves::scheduled() {
    if (refresh_.waiting() || document() == nullptr) return;

    refresh_ = document()->after(std::chrono::milliseconds{0}, [this](Timer&) { refreshed(); });
}

// The tiles of the view as it stands and of the view after it (the next rows, or the next page
// one at a time), to be asked for.
void Leaves::asked() {
    std::vector<Key> visible;
    std::vector<Key> ahead;

    for (const Slice& slice : slices_)
        for (const Key& one : covering(slice.page, slice.part)) visible.push_back(one);

    if (*layout == Layout::continuous) {
        for (std::size_t page = 0; page < sheets_.size(); ++page) {
            if (const std::optional<Slice> next = sliced(page, sheets_[page].from, top_ + static_cast<std::size_t>(view_.high))) {
                for (const Key& one : covering(page, next->part)) ahead.push_back(one);
            }
        }
    } else if (current_ + 1 < sheets_.size()) {
        if (const std::optional<Slice> next = sliced(current_ + 1, 0, 0)) {
            for (const Key& one : covering(current_ + 1, next->part)) ahead.push_back(one);
        }
    }

    wanted(visible, ahead);
}

// The view under the pages: its background, and in place of a page whose picture has not come its
// number, or the trouble that kept it from coming.
void Leaves::paint(Canvas& onto) const {
    onto.fill(Area{{0, 0}, onto.size()}, look());

    for (const Slice& slice : slices_) {
        if (showing(slice)) continue;

        const Look paper = look("page");
        const Area inner = slice.area;
        const auto failed = failed_.find(slice.page);
        const std::string said = failed != failed_.end() ? failed->second : std::to_string(slice.page + 1);
        const int wide = static_cast<int>(std::min<std::size_t>(said.size(), static_cast<std::size_t>(std::max(inner.size.wide, 0))));

        onto.fill(inner, paper);
        (void)onto.put(Spot{inner.at.x + (inner.size.wide - wide) / 2, inner.at.y + inner.size.high / 2}, said.substr(0, static_cast<std::size_t>(wide)),
                       paper);
    }
}

// The pages laid out for the view `view`: the pixels of a cell, each page's columns and rows at the
// zoom, where it begins in the continuous layout, the rows of the whole, and the view kept within
// them; a view anchored to a page before a change of zoom, layout or room comes back to it. A page
// of no area, or of a size that is no number, is laid out as US Letter; a page is at most 2^20
// rows, and one at a time at most as high as the view at the zoom.
void Leaves::sized(Size view) {
    view_ = Size{std::max(view.wide, 0), std::max(view.high, 0)};
    step_ = step_of(display());
    sheets_.assign(pages_.size(), Sheet{});
    window_ = static_cast<std::size_t>(view_.high);
    laid_ = *layout;

    const double scale = scales[std::min(*zoom, scales.size() - 1)];
    const bool single = *layout == Layout::single;
    const int tallest = 1 << 20;
    int from = 0;

    for (std::size_t at = 0; at < pages_.size(); ++at) {
        const docs::Page& given = pages_[at];
        const bool sound = given.width > 0 && given.height > 0 && std::isfinite(given.width) && std::isfinite(given.height);
        const double wide = (sound ? given.width : 612.0) * step_.high;
        const double high = (sound ? given.height : 792.0) * step_.wide;
        double fit = view_.wide;

        if (single) fit = std::min(fit, view_.high * wide / high);

        Sheet& sheet = sheets_[at];

        sheet.columns = std::max(1, static_cast<int>(std::lround(fit * scale)));
        sheet.rows = static_cast<int>(std::clamp(std::lround(sheet.columns * high / wide), 1L, static_cast<long>(tallest)));

        if (single) sheet.rows = std::min(sheet.rows, std::max(1, static_cast<int>(std::lround(view_.high * scale))));

        sheet.from = from;
        from += sheet.rows + 1;
    }

    if (pages_.empty()) {
        extent_ = 0;
    } else if (single) {
        current_ = std::min(current_, pages_.size() - 1);
        extent_ = static_cast<std::size_t>(sheets_[current_].rows);
    } else {
        extent_ = static_cast<std::size_t>(from - 1);
    }

    if (anchor_ && anchor_->first < sheets_.size()) {
        const Sheet& sheet = sheets_[anchor_->first];
        const std::size_t within = static_cast<std::size_t>(std::lround(anchor_->second * sheet.rows));

        top_ = (single ? 0 : static_cast<std::size_t>(sheet.from)) + within;
    }

    anchor_.reset();
    top_ = std::min(top_, extent_ > window_ ? extent_ - window_ : 0);

    int widest = 0;

    for (std::size_t at = 0; at < sheets_.size(); ++at)
        if (!single || at == current_) widest = std::max(widest, sheets_[at].columns);

    left_ = std::min(left_, widest > view_.wide ? static_cast<std::size_t>(widest - view_.wide) : 0);
}

// Keeps the page in the view and how far down it the view stands, laid out one page at a time
// before when `single`, for a change of zoom or layout to come back to; the troubles of the old
// tiles are forgotten.
void Leaves::anchored(bool single) {
    failed_.clear();

    if (current_ >= sheets_.size()) return;

    const Sheet& sheet = sheets_[current_];
    const std::size_t from = single ? 0 : static_cast<std::size_t>(sheet.from);
    const double down = top_ > from ? static_cast<double>(top_ - from) / sheet.rows : 0.0;

    anchor_ = std::pair<std::size_t, double>{current_, std::min(down, 1.0)};
}

// The continuous view from row `row`, at most as far as the end comes into it, telling `Scrolled`
// when it moved and `Turned` when another page is now the one it shows most of.
void Leaves::moved(std::size_t row) {
    const std::size_t made = std::min(row, extent_ > window_ ? extent_ - window_ : 0);

    if (made == top_) return;

    top_ = made;
    failed_.clear();
    repaint();
    scheduled();

    Scrolled scrolled;

    notified(scrolled);

    const std::size_t now = under(top_);

    if (now == current_) return;

    current_ = now;

    Turned turned{now};

    notified(turned);
}

// The page of the continuous layout that a view from row `row` shows the most rows of, the
// earlier of two that it shows as much of: the first page at the top, the last at the end.
std::size_t Leaves::under(std::size_t row) const {
    if (sheets_.empty()) return 0;

    const long long top = static_cast<long long>(row);
    const long long bottom = top + std::max(view_.high, 1);
    const auto after = std::upper_bound(sheets_.begin(), sheets_.end(), top, [](long long at, const Sheet& one) { return at < one.from; });
    std::size_t page = after == sheets_.begin() ? 0 : static_cast<std::size_t>(after - sheets_.begin()) - 1;
    std::size_t best = page;
    long long most = -1;

    for (; page < sheets_.size() && sheets_[page].from < bottom; ++page) {
        const long long shown = std::min<long long>(bottom, sheets_[page].from + sheets_[page].rows) - std::max<long long>(top, sheets_[page].from);

        if (shown > most) {
            most = shown;
            best = page;
        }
    }

    return best;
}

// The tiles of page `page` at its scale that the cells `part` of it cover.
std::vector<Leaves::Key> Leaves::covering(std::size_t page, Area part) const {
    const Sheet& sheet = sheets_[page];
    const auto wide = static_cast<std::uint32_t>(sheet.columns * step_.wide);
    const auto high = static_cast<std::uint32_t>(sheet.rows * step_.high);
    const auto left = static_cast<std::uint32_t>(part.at.x * step_.wide);
    const auto top = static_cast<std::uint32_t>(part.at.y * step_.high);
    const auto right = static_cast<std::uint32_t>((part.at.x + part.size.wide) * step_.wide);
    const auto bottom = static_cast<std::uint32_t>((part.at.y + part.size.high) * step_.high);
    std::vector<Key> made;

    if (right <= left || bottom <= top) return made;

    for (std::uint32_t down = top / side; down <= (bottom - 1) / side; ++down)
        for (std::uint32_t across = left / side; across <= (right - 1) / side; ++across) made.push_back(Key{page, wide, high, across, down});

    return made;
}

// The pixels of the cells `part` of page `page`, composed from its tiles, which count as used now;
// nothing while a tile is missing.
std::optional<media::Pixels> Leaves::composed(std::size_t page, Area part) {
    const std::vector<Key> keys = covering(page, part);

    for (const Key& key : keys)
        if (!tiles_.contains(key)) return std::nullopt;

    if (keys.empty()) return std::nullopt;

    const auto left = static_cast<std::uint32_t>(part.at.x * step_.wide);
    const auto top = static_cast<std::uint32_t>(part.at.y * step_.high);
    const auto wide = static_cast<std::uint32_t>(part.size.wide * step_.wide);
    const auto high = static_cast<std::uint32_t>(part.size.high * step_.high);
    Bytes rgba(std::size_t{wide} * high * 4);

    for (const Key& key : keys) {
        Tile& tile = tiles_.at(key);
        const media::Size size = tile.pixels.size();
        const std::uint32_t x0 = key.across * side;
        const std::uint32_t y0 = key.down * side;
        const std::uint32_t from_x = std::max(x0, left);
        const std::uint32_t to_x = std::min(x0 + size.width, left + wide);
        const std::uint32_t from_y = std::max(y0, top);
        const std::uint32_t to_y = std::min(y0 + size.height, top + high);
        const Bytes& source = tile.pixels.stills().front().rgba;

        tile.used = ++uses_;

        for (std::uint32_t y = from_y; y < to_y; ++y) {
            const std::size_t from = (std::size_t{y - y0} * size.width + (from_x - x0)) * 4;
            const std::size_t into = (std::size_t{y - top} * wide + (from_x - left)) * 4;

            std::memcpy(rgba.data() + into, source.data() + from, std::size_t{to_x - from_x} * 4);
        }
    }

    lit(page, keys.front().wide, keys.front().high, part, rgba);

    std::vector<media::Still> stills(1);

    stills.front().rgba = std::move(rgba);

    return media::Pixels{media::Size{wide, high}, std::move(stills)};
}

// Tints in `rgba`, the pixels of the cells `part` of page `page` of `wide` by `high` pixels, the
// lights on that page: each pixel times the colour of the part "light" of the sheet (the current
// light's in the state current), and where the sheet gives none, a marker's yellow and orange.
void Leaves::lit(std::size_t page, std::uint32_t wide, std::uint32_t high, Area part, Bytes& rgba) const {
    const auto left = static_cast<std::int64_t>(part.at.x) * step_.wide;
    const auto top = static_cast<std::int64_t>(part.at.y) * step_.high;
    const auto across = static_cast<std::int64_t>(part.size.wide) * step_.wide;
    const auto down = static_cast<std::int64_t>(part.size.high) * step_.high;
    const Color marker = look("light").paper.value_or(Color{255, 224, 102});
    const Color current = look("light", States{State::current}).paper.value_or(Color{255, 153, 51});

    for (const Light& light : lights_) {
        if (light.page != page) continue;

        const Color tint = light.current ? current : marker;
        const auto from_x = std::max(static_cast<std::int64_t>(light.region.left * wide), left);
        const auto to_x = std::min(static_cast<std::int64_t>(std::ceil((light.region.left + light.region.width) * wide)), left + across);
        const auto from_y = std::max(static_cast<std::int64_t>(light.region.top * high), top);
        const auto to_y = std::min(static_cast<std::int64_t>(std::ceil((light.region.top + light.region.height) * high)), top + down);

        for (std::int64_t y = from_y; y < to_y; ++y)
            for (std::int64_t x = from_x; x < to_x; ++x) {
                std::byte* const at = rgba.data() + ((y - top) * across + (x - left)) * 4;

                at[0] = std::byte{static_cast<std::uint8_t>(std::to_integer<unsigned>(at[0]) * tint.red / 255)};
                at[1] = std::byte{static_cast<std::uint8_t>(std::to_integer<unsigned>(at[1]) * tint.green / 255)};
                at[2] = std::byte{static_cast<std::uint8_t>(std::to_integer<unsigned>(at[2]) * tint.blue / 255)};
            }
    }
}

// The tiles to ask for: those of the view the cache lacks first, then those of the next view while
// they fit beside what the cache holds; none of a page whose trouble shows. The tiles of the view
// are kept from going.
void Leaves::wanted(const std::vector<Key>& visible, const std::vector<Key>& ahead) {
    wanted_.clear();
    pinned_ = visible;

    const auto lacking = [this](const Key& key) { return !tiles_.contains(key) && !flying_.contains(key) && !failed_.contains(key.page); };
    std::uint64_t room = held_;

    for (const Key& key : visible)
        if (lacking(key)) wanted_.push_back(key);

    for (const Key& key : ahead) {
        const std::uint64_t much = std::uint64_t{std::min(side, key.wide - key.across * side)} * std::min(side, key.high - key.down * side) * 4;

        if (!lacking(key) || room + much > *most) continue;

        room += much;
        wanted_.push_back(key);
    }

    pump();
}

// Asks for the next tiles wanted while fewer than two are being rendered.
void Leaves::pump() {
    for (const Key& key : wanted_) {
        if (flying_.size() >= flights) return;

        if (tiles_.contains(key) || flying_.contains(key)) continue;

        if (!scope_) {
            try {
                scope_.emplace();
            } catch (const Trouble&) {
                return;
            }
        }

        flying_.insert(key);
        scope_->run(fetched(key));
    }
}

// Keeps the tile `key` of `pixels`, and lets the least used go while the tiles hold more than
// `most` bytes.
void Leaves::kept(const Key& key, media::Pixels pixels) {
    const media::Size size = pixels.size();
    Tile& tile = tiles_[key];

    held_ -= tile.bytes;
    tile.bytes = std::uint64_t{size.width} * size.height * 4;
    tile.pixels = std::move(pixels);
    tile.used = ++uses_;
    held_ += tile.bytes;

    trimmed(pinned_);
}

// Lets the least used tiles go, but not those in `pinned`, while they hold more than `most` bytes.
void Leaves::trimmed(const std::vector<Key>& pinned) {
    const std::set<Key> kept_now(pinned.begin(), pinned.end());

    while (held_ > *most) {
        auto oldest = tiles_.end();

        for (auto at = tiles_.begin(); at != tiles_.end(); ++at) {
            if (kept_now.contains(at->first)) continue;

            if (oldest == tiles_.end() || at->second.used < oldest->second.used) oldest = at;
        }

        if (oldest == tiles_.end()) return;

        held_ -= oldest->second.bytes;
        tiles_.erase(oldest);
    }
}

// The tile `key` rendered by the engine: its region of the page in fractions, its size in pixels.
// It goes on from the loop's next turn, never from within the layout that asked for it, and comes
// back to the loop for what the tile brings, wherever the document answered from, since that
// changes the tree. Kept when it comes; its trouble, or any exception's words, shown in place of
// its page when it fails; the view refreshed either way, which asks for the next tiles. Nothing
// for a view that is gone, or a tile whose work was cancelled.
Task<void> Leaves::fetched(Key key) {
    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& here = co_await detail::Here{};

    co_await to(here);

    if (alive.expired()) co_return;

    const std::shared_ptr<docs::Document> document = document_;
    const std::uint32_t x0 = key.across * side;
    const std::uint32_t y0 = key.down * side;
    const std::uint32_t wide = std::min(side, key.wide - x0);
    const std::uint32_t high = std::min(side, key.high - y0);
    docs::Region region;

    region.left = static_cast<double>(x0) / key.wide;
    region.top = static_cast<double>(y0) / key.high;
    region.width = static_cast<double>(wide) / key.wide;
    region.height = static_cast<double>(high) / key.high;

    while (region.left + region.width > 1) region.width = std::nextafter(region.width, 0.0);

    while (region.top + region.height > 1) region.height = std::nextafter(region.height, 0.0);

    const media::Size size{wide, high};
    media::Pixels made;
    std::optional<std::string> trouble;
    bool cancelled = false;

    try {
        made = co_await document->rendered(key.page, size, region);
    } catch (const Cancelled&) {
        cancelled = true;
    } catch (const Trouble& why) {
        trouble = why.what();
    } catch (const std::exception& why) {
        trouble = why.what();
    }

    if (cancelled) co_return;

    co_await to(here);

    if (alive.expired()) co_return;

    flying_.erase(key);

    if (trouble) {
        failed_[key.page] = *trouble;
    } else {
        kept(key, std::move(made));
    }

    refreshed();
}

}

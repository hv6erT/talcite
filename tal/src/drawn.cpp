#include "tal/drawn.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <utility>

#include "tal/crypto.hpp"
#include "tal/executor.hpp"

#include "deflating.hpp"
#include "marking.hpp"
#include "mosaicking.hpp"
#include "sixeling.hpp"

// A drawing in the tree (E7c): pixels a painter draws with a pen, shown as pictures where the
// terminal takes them and as a mosaic of characters elsewhere, again and again at a rate or on
// demand. Its size is its room: the room's cells times the cell's pixels where the terminal shows
// pixels (kitty or sixel, with a cell of known size, or as forced), the cells times the grain of
// the characters otherwise. The painter runs on the program's loop, on a clean raster of that size,
// with the time since the first frame by the document's clock.
//
// A frame is cut into bands of four rows of cells, each a picture of its own. A sixel is six pixels
// high, so a sixel band goes on down past its cells to whole sixels with the pixels of the band
// below (painted again by that band as it stands, and part of this band's hash, so that it is sent
// anew when they change), and the last band, with none below, with clear pixels. For sixel the last
// row of the drawing is a band of its own: a sixel on the screen's last row would scroll it, so the
// program shows none there, and only that row then shows the mosaic. A band whose pixels did not
// change (by a hash of them) keeps its picture and its mark, so the terminal is not sent it again:
// of an animation where only a part moves, only that part travels. Bands are coded on the pool,
// side by side, for kitty as zlib (o=z) in base64, for sixel by our encoder, with the mosaic
// beneath them for whenever the pictures are not seen whole. The first frame of a new size shows
// its mosaic at once, made on the loop, so that a window being resized shows the drawing in
// characters rather than nothing until the pictures come. While a frame is being coded the next one
// is not made but owed, so that a slow terminal gets fewer frames, never a queue of old ones. In
// characters a frame goes to the cells at once, and the output sends only cells that changed.
//
// A painter's Trouble is shown in place of the drawing, and the next frame tries again.
namespace tal {

namespace {

constexpr int band_rows = 4;

// The pixels of a terminal that shows them: graphics and a cell of known size; none otherwise.
Graphics sharp(const Display& shown, std::optional<Graphics> forced) {
    if (shown.cell.wide <= 0 || shown.cell.high <= 0) return Graphics::none;

    return forced.value_or(shown.graphics);
}

// The rows [`top`, `top` + `high`) of the still of `frame`, those past its bottom clear.
media::Pixels cut(const media::Pixels& frame, std::uint32_t top, std::uint32_t high) {
    const std::size_t row = std::size_t{frame.size().width} * 4;
    const Bytes& rgba = frame.stills().front().rgba;
    const std::uint32_t there = std::min(high, frame.size().height - std::min(top, frame.size().height));
    Bytes part(rgba.begin() + static_cast<std::ptrdiff_t>(row * top), rgba.begin() + static_cast<std::ptrdiff_t>(row * (top + there)));

    part.resize(row * high, std::byte{0});

    std::vector<media::Still> stills;

    stills.push_back(media::Still{std::move(part), std::chrono::milliseconds{0}});

    return media::Pixels{media::Size{frame.size().width, high}, std::move(stills)};
}

// The jobs 0 to `count` - 1 done by `job`, on this thread and on as many threads of the pool as
// take part: each takes the next job not yet taken, until none is left. This thread does all of
// them when the pool has no thread to spare, so it never waits on work nobody runs; it waits
// only for the jobs other threads have taken, which run. The first trouble of a job is thrown
// once all are done; the jobs after it are not begun.
template <typename Job>
void shared(std::size_t count, const Job& job) {
    struct Sharing {
        std::mutex guard;
        std::condition_variable done;
        std::size_t next = 0;
        std::size_t finished = 0;
        std::size_t count = 0;
        std::exception_ptr trouble;
        std::function<void(std::size_t)> job;
    };

    const auto sharing = std::make_shared<Sharing>();

    sharing->count = count;
    sharing->job = [&job](std::size_t at) { job(at); };

    const auto working = [](Sharing& one) {
        for (;;) {
            std::size_t at = 0;

            {
                const std::lock_guard<std::mutex> kept{one.guard};

                if (one.next >= one.count || one.trouble) return;

                at = one.next++;
            }

            std::exception_ptr why;

            try {
                one.job(at);
            } catch (...) {
                why = std::current_exception();
            }

            const std::lock_guard<std::mutex> kept{one.guard};

            if (why && !one.trouble) one.trouble = why;

            ++one.finished;
            one.done.notify_all();
        }
    };

    const std::size_t helpers = std::min<std::size_t>(count > 0 ? count - 1 : 0, std::max(1U, std::thread::hardware_concurrency()) - 1);

    for (std::size_t one = 0; one < helpers; ++one) pool().post([sharing, working] { working(*sharing); });

    working(*sharing);

    std::unique_lock<std::mutex> held{sharing->guard};

    sharing->done.wait(held, [&] { return sharing->finished == sharing->next; });

    if (sharing->trouble) std::rethrow_exception(sharing->trouble);
}

// A hash of the bytes of `frame` (FNV-1a), to tell a band that changed from one that did not.
std::uint64_t fingerprint(const media::Pixels& frame) {
    std::uint64_t made = 14695981039346656037ULL;

    for (const std::byte one : frame.stills().front().rgba) {
        made ^= std::to_integer<std::uint64_t>(one);
        made *= 1099511628211ULL;
    }

    return made;
}

}

// A drawing of `painter` blending in `blending`, drawn on demand until a rate is given.
Drawing::Drawing(Painter painter, drawing::Blending blending) : painter_(std::move(painter)), blending_(blending) { classes({"drawing"}); }

// Its work on the pool ends with it.
Drawing::~Drawing() = default;

// Frames every 1/`frames` seconds; 0 draws only on `redraw` and on a new size. Throws Trouble
// malformed for a rate that is negative or not finite.
void Drawing::rate(double frames) {
    if (!std::isfinite(frames) || frames < 0) throw Trouble{Kind::malformed, "A rate of frames that is negative or not a finite number."};

    rate_ = frames;
    timer_ = Timer{};
    ticking();
}

// A new frame as soon as can be: now, or once the frame being coded is done.
void Drawing::redraw() { drawn(); }

// The way of showing the pixels forced (`Graphics::none` for characters), or left to the terminal
// when none.
void Drawing::force(std::optional<Graphics> graphics) {
    if (forced_ == graphics) return;

    forced_ = graphics;
    relayout();
}

// The point of the drawing (in its pixels) under `event`: the very pixel where the terminal tells
// pixels and the drawing shows them, the middle of the cell otherwise; none before the drawing has
// a size. The point may lie outside the drawing, as a drag goes on beyond it.
std::optional<drawing::Point> Drawing::pointed(const MouseEvent& event) const {
    if (size_.width == 0 || size_.height == 0 || room_.wide <= 0 || room_.high <= 0) return std::nullopt;

    const Spot origin{event.screen.x - event.at.x, event.screen.y - event.at.y};

    if (event.pixel && fine_ != Graphics::none && cell_.wide > 0 && cell_.high > 0)
        return drawing::Point{event.pixel->x - origin.x * cell_.wide + 0.5, event.pixel->y - origin.y * cell_.high + 0.5};

    const double wide = static_cast<double>(size_.width) / room_.wide;
    const double high = static_cast<double>(size_.height) / room_.high;

    return drawing::Point{(event.at.x + 0.5) * wide, (event.at.y + 0.5) * high};
}

// Busy while a frame is being coded on the pool.
bool Drawing::busy() const { return encoding_ || (scope_.has_value() && scope_->busy()); }

// All the room given; an unbounded room gives forty cells by twelve.
Size Drawing::measure(Limits room) const {
    const int wide = room.most.wide == endless ? 40 : room.most.wide;
    const int high = room.most.high == endless ? 12 : room.most.high;

    return Size{std::max(room.least.wide, wide), std::max(room.least.high, high)};
}

// The size in pixels from the room and the terminal; a new size (or a new way of showing) starts
// anew and draws at once.
void Drawing::arrange(Area inside) {
    const Display& shown = display();
    const Graphics fine = sharp(shown, forced_);
    const Size step = fine != Graphics::none ? shown.cell : mosaicking::grain(shown.blocks, shown.unicode);
    const media::Size size{static_cast<std::uint32_t>(std::max(0, inside.size.wide * step.wide)), static_cast<std::uint32_t>(std::max(0, inside.size.high * step.high))};

    ticking();

    if (room_ == inside.size && fine_ == fine && blocks_ == shown.blocks && unicode_ == shown.unicode && cell_ == shown.cell && size_ == size) return;

    room_ = inside.size;
    fine_ = fine;
    blocks_ = shown.blocks;
    unicode_ = shown.unicode;
    cell_ = shown.cell;
    size_ = size;
    ++orders_;
    bands_.clear();
    coarse_ = {};
    frame_ = {};
    encoding_ = false;
    pending_ = false;

    if (scope_) scope_->cancel();

    drawn();
}

// The trouble of the painter in place of the drawing; or the frame in characters; or, where the
// terminal shows pixels, the mosaic and the pictures of the bands over it.
void Drawing::paint(Canvas& onto) const {
    if (trouble_) {
        (void)onto.put(Spot{0, 0}, trouble_->what(), look());

        return;
    }

    const Display& shown = display();

    if (fine_ == Graphics::none) {
        if (frame_.held()) mosaicking::drawn(onto, Spot{0, 0}, frame_, 0, blocks_, look(), shown.colors, unicode_);

        return;
    }

    if (coarse_.held()) mosaicking::drawn(onto, Spot{0, 0}, coarse_, 0, blocks_, look(), shown.colors, unicode_);

    int row = 0;

    for (const Band& band : bands_) {
        onto.picture(Area{Spot{0, row}, Size{room_.wide, band.rows}}, band.picture);
        row += band.rows;
    }
}

// One frame of the painter recorded at `size` (the drawing's own, without one), at the time of the
// last frame drawn, so that it shows what the screen shows: for SVG, or to be played on a raster of
// any scale. Throws Trouble other for no size (none given and the drawing not laid out yet), as a
// recording does, and whatever the painter throws.
drawing::Recording Drawing::recorded(std::optional<media::Size> size) const {
    drawing::Recording made{size.value_or(size_), blending_};
    drawing::Pen pen{made};

    painter_(pen, made.size(), shown_);

    return made;
}

// A frame made now, unless one is being coded: then it is owed and made when that one is done.
void Drawing::drawn() {
    if (document() == nullptr || size_.width == 0 || size_.height == 0) return;

    if (encoding_) {
        pending_ = true;

        return;
    }

    const std::chrono::steady_clock::time_point now = document()->now();

    if (!started_) started_ = now;

    media::Pixels frame;

    try {
        drawing::Raster raster{size_, blending_};
        drawing::Pen pen{raster};

        shown_ = now - *started_;
        painter_(pen, size_, shown_);
        frame = raster.pixels();
    } catch (const Trouble& why) {
        trouble_ = why;
        repaint();

        return;
    }

    trouble_.reset();
    ++frames_;

    if (fine_ == Graphics::none) {
        frame_ = std::move(frame);
        repaint();

        return;
    }

    // The first frame of a size has no pictures yet: its mosaic stands at once while they are
    // coded.
    if (bands_.empty()) {
        const Size grain = mosaicking::grain(blocks_, unicode_);

        coarse_ = frame.scaled(media::Size{static_cast<std::uint32_t>(room_.wide * grain.wide), static_cast<std::uint32_t>(room_.high * grain.high)});
        repaint();
    }

    if (!scope_) {
        try {
            scope_.emplace();
        } catch (const Trouble&) {
            return;
        }
    }

    encoding_ = true;
    scope_->run(encoding(orders_, std::move(frame)));
}

// The next frame appointed at the rate on the document's clock; nothing at a rate of zero, outside
// a document or when one is appointed already.
void Drawing::ticking() {
    if (rate_ <= 0 || document() == nullptr) {
        timer_ = Timer{};

        return;
    }

    if (timer_.waiting()) return;

    const std::chrono::milliseconds every{std::max<long long>(1, std::llround(1000.0 / rate_))};

    timer_ = document()->after(every, [this, every](Timer& one) {
        drawn();
        one.again(every);
    });
}

// The bands of `frame` coded on the pool and taken in; dropped when the drawing changed size
// meanwhile (`order`) or is gone. A frame owed meanwhile is made then.
Task<void> Drawing::encoding(std::uint64_t order, media::Pixels frame) {
    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& back = co_await detail::Here{};
    const Graphics fine = fine_;
    const Size cell = cell_;
    const Size room = room_;
    const Size grain = mosaicking::grain(blocks_, unicode_);
    const std::vector<Band> before = bands_;

    co_await to(pool());

    Encoded made = encoded(frame, fine, cell, room, grain, before);

    co_await to(back);

    if (alive.expired() || order != orders_) co_return;

    bands_ = std::move(made.bands);
    coarse_ = std::move(made.coarse);
    encoding_ = false;
    repaint();

    if (pending_) {
        pending_ = false;
        drawn();
    }
}

// The bands of `frame` (a room of `room` cells of `cell` pixels) coded for `fine` (for sixel down
// to whole sixels, the last row alone), those whose pixels match the bands `before` kept as they
// were, and the mosaic of the frame at `grain`. Each band and the mosaic is a job of its own,
// shared with the pool (`shared`); the new bands take their marks afterwards, in order.
Drawing::Encoded Drawing::encoded(const media::Pixels& frame, Graphics fine, Size cell, Size room, Size grain, const std::vector<Band>& before) {
    const bool sixel = fine == Graphics::sixel;
    const int body = sixel && room.high > 1 ? room.high - 1 : room.high;
    std::vector<std::pair<int, int>> spans;

    for (int top = 0; top < room.high;) {
        const int rows = top < body ? std::min(band_rows, body - top) : room.high - top;

        spans.emplace_back(top, rows);
        top += rows;
    }

    std::vector<Band> bands(spans.size());
    std::vector<char> kept(spans.size(), 0);
    Encoded made;

    shared(spans.size() + 1, [&](std::size_t at) {
        if (at == spans.size()) {
            made.coarse = frame.scaled(media::Size{static_cast<std::uint32_t>(room.wide * grain.wide), static_cast<std::uint32_t>(room.high * grain.high)});

            return;
        }

        const auto [top, rows] = spans[at];
        const int high = rows * cell.high;
        const media::Pixels part = cut(frame, static_cast<std::uint32_t>(top * cell.high), static_cast<std::uint32_t>(sixel ? (high + 5) / 6 * 6 : high));
        const std::uint64_t hash = fingerprint(part);

        if (at < before.size() && before[at].hash == hash && before[at].rows == rows) {
            bands[at] = before[at];
            kept[at] = 1;

            return;
        }

        Picture picture{0, Size{room.wide, rows}, nullptr, nullptr, 0};

        if (fine == Graphics::kitty) {
            picture.pixels = std::make_shared<const media::Pixels>(part);
            picture.coded = std::make_shared<const std::string>(crypto::base64(deflating::deflated(part.stills().front().rgba)));
        } else {
            picture.sixel = std::make_shared<const std::string>(sixeling::sixel(part, 0));
        }

        bands[at] = Band{hash, rows, std::move(picture)};
    });

    for (std::size_t at = 0; at < bands.size(); ++at)
        if (kept[at] == 0) bands[at].picture.mark = marked();

    made.bands = std::move(bands);

    return made;
}
}

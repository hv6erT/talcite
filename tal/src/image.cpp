#include "tal/image.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "tal/executor.hpp"
#include "tal/port.hpp"
#include "tal/trouble.hpp"

#include "mosaicking.hpp"
#include "sixeling.hpp"

// An image in the tree, ported from talcite's image.cpp. It holds its source, not the picture:
// bytes of a file, decoded on the pool already at the size of its room and never at full
// resolution, or pixels made in code, scaled at once. Where the terminal shows pixels (sixel or
// kitty, with a cell of known size) the frames come at the room's full resolution and their
// pictures go into the cells, with characters drawn from them beneath for whenever the picture is
// not seen whole; elsewhere the frames come at the grain of the characters and are drawn as a
// mosaic.
namespace tal {

namespace {

// A picture's mark: unique in the whole program, since the cells of different images meet in one
// frame; zero means no picture.
std::uint32_t marked() {
    static std::atomic<std::uint32_t> next{0};

    return ++next;
}

// Whether the terminal takes pixels: it speaks sixel or kitty, and the cell has a known size,
// without which there is no telling how many pixels to order. Otherwise characters.
Graphics sharp(const Display& shown) {
    if (shown.cell.wide <= 0 || shown.cell.high <= 0) return Graphics::none;

    return shown.graphics;
}

// How many cells the picture `image` takes in `room` so that it keeps its proportions. A cell is
// not a square: on some terminals it is 10 by 20 pixels, on others 9 by 21, so without its size a
// circle comes out as an egg; a cell of unknown size (a zero) is taken as 1:2, as most are. The
// whole width first and the height from it; when that does not fit, the height and the width from
// it. Always at least one cell, since a picture of no cells is a picture nobody sees.
Size fitting(media::Size image, Size room, Size cell) {
    if (image.width == 0 || image.height == 0 || room.wide <= 0 || room.high <= 0) return {};

    const bool known = cell.wide > 0 && cell.high > 0;
    const std::int64_t wide = known ? cell.wide : 1;
    const std::int64_t high = known ? cell.high : 2;
    const std::int64_t across = image.width;
    const std::int64_t down = image.height;
    int columns = room.wide;
    int rows = static_cast<int>(std::clamp<std::int64_t>((std::int64_t{columns} * wide * down + high * across / 2) / (high * across), 1, endless));

    if (rows > room.high) {
        rows = room.high;
        columns = static_cast<int>(std::clamp<std::int64_t>((std::int64_t{rows} * high * across + wide * down / 2) / (wide * down), 1, room.wide));
    }

    return {columns, rows};
}

// The cells the picture `image` takes at its own resolution: a cell's pixels each, or a cell's
// grain of characters when the cell's size is not known. An unbounded room is bounded by it, so
// that a picture is not blown up past its pixels just because nothing stops it.
Size natural(media::Size image, Size cell, Size grain) {
    const Size step = cell.wide > 0 && cell.high > 0 ? cell : grain;

    return {static_cast<int>((image.width + static_cast<std::uint32_t>(step.wide) - 1) / static_cast<std::uint32_t>(step.wide)),
            static_cast<int>((image.height + static_cast<std::uint32_t>(step.high) - 1) / static_cast<std::uint32_t>(step.high))};
}

// The pixels of `room` cells of `step` pixels each.
media::Size pixelled(Size room, Size step) {
    return {static_cast<std::uint32_t>(room.wide * step.wide), static_cast<std::uint32_t>(room.high * step.high)};
}

}

// An empty image in the class "image", playing once it has frames.
Image::Image() { classes({"image"}); }

// Its work on the pool ends with it: the scope cancels what is still under way.
Image::~Image() = default;

// The image of the bytes `given`: the format and the size come from the header at once, so that
// layout can ask for room before anything is decoded; the frames come once a room is known. With
// no decoder at all (`media::Decoding`, whose default is ours) a format other than GIF leaves the
// image without a size, and so does a header without one; a format the decoder refuses empties
// the image when it is decoded. The number of frames is known after the first
// decoding.
void Image::hold(Bytes given) {
    reset();

    format_ = media::format_of(given);

    if (format_ == media::Format::gif || (format_ != media::Format::none && defaults().get<media::Decoding>() != nullptr))
        size_ = media::size_of(given).value_or(media::Size{});

    bytes_ = std::move(given);

    if (!held()) bytes_.clear();

    relayout();
}

// The image of pixels made in code, for which there is no file: scaled at once when its room is
// known.
void Image::hold(media::Pixels given) {
    reset();

    whole_ = std::move(given);
    size_ = whole_.held() ? whole_.size() : media::Size{};
    frames_ = whole_.stills().size();

    relayout();
}

// Lets go of everything: no source, no frames, no format. A page being closed must hold neither a
// file nor frames.
void Image::clear() {
    reset();
    relayout();
}

// Whether the image has a size, that is, something to show.
bool Image::held() const { return size_.width > 0 && size_.height > 0; }

// Busy while any of its work on the pool has not come back: frames or the sixel of frames, also
// those of an order already replaced, which come back only to be dropped.
bool Image::busy() const { return scope_.has_value() && scope_->busy(); }

// Pausing freezes the frame on show, not the first one, so that the picture does not jump; playing
// again goes on from it, its whole delay ahead of it.
void Image::play(bool on) {
    if (play_ == on) return;

    play_ = on;
    frame_ = Timer{};

    beating();
}

// Shows frame `which` (the last when there are fewer), seeking: a playing animation goes on from
// it, its whole delay ahead of it.
void Image::still(std::size_t which) {
    const std::size_t many = fit_.stills().empty() ? frames_ : fit_.stills().size();

    shown_ = many == 0 ? 0 : std::min(which, many - 1);
    frame_ = Timer{};

    repaint();
    beating();
}

// The cells the picture takes in `room` keeping its proportions (`fitting`), at most those of its
// own resolution where the room has no bound; nothing without a picture.
Size Image::measure(Limits room) const {
    const Display& shown = display();
    const Size most = natural(size_, shown.cell, mosaicking::grain(shown.blocks, shown.unicode));
    const Size bounded{room.most.wide == endless ? most.wide : room.most.wide, room.most.high == endless ? most.high : room.most.high};

    return fitting(size_, bounded, shown.cell);
}

// The frames fitted to the room (`spread`), and the next frame of an animation appointed.
void Image::arrange(Area inside) {
    spread(inside.size);
    beating();
}

// Frames are made again only when the room, the characters or the cell changed: a photo is not to
// go through the filter at every frame of the screen. A change of the terminal
// (`Document::display`) lays every node out again, and the comparison sees it with nobody telling
// anyone.
//
// Bytes are decoded on the pool, one order at a time: a new one leaves the old's result unused,
// known by its number. Until the frames come the image paints what it had, the old frames at
// their old size, without flicker, or nothing. Pixels made in code are scaled at once, since they
// are in memory and small.
void Image::spread(Size given) {
    const Display& shown = display();
    const Blocks kind = shown.blocks;
    const bool unicode = shown.unicode;
    const Graphics fine = sharp(shown);
    const Size room = fitting(size_, given, shown.cell);

    if (shaped_ == given && made_ == kind && unicode_ == unicode && fine_ == fine && cell_ == shown.cell) return;

    shaped_ = given;
    made_ = kind;
    unicode_ = unicode;
    cell_ = shown.cell;

    if (!held() || room.wide <= 0 || room.high <= 0) {
        fit_ = {};
        placed_ = {};
        fine_ = fine;
        ordered_ = false;
        ++orders_;
        sharpened();

        return;
    }

    const Size step = fine != Graphics::none ? shown.cell : mosaicking::grain(kind, unicode);
    const media::Size want = pixelled(room, step);

    if (!bytes_.empty()) {
        if (ordered_ && ordering_ == want) return;

        ordering_ = want;
        ordered_ = started(decoding(++orders_, bytes_, want, room, fine));

        return;
    }

    fit_ = whole_.scaled(want);
    placed_ = room;
    fine_ = fine;
    frames_ = fit_.stills().size();

    sharpened();
}

// The frames in pixels and what stands for them in characters: with pixels `fit_` has the room's
// full resolution, so the characters, drawn while the picture is not seen whole and before its
// sixel is ready, are made from it here, once. Sixel is encoded on the pool, every frame at once;
// until then the picture stands in characters. Kitty encodes nothing here: the terminal gets the
// raw pixels, which the output sends once for every frame, so the pictures are ready at once.
void Image::sharpened() {
    pictures_.clear();
    coarse_ = {};
    if (fine_ == Graphics::none || fit_.stills().empty()) return;

    const Size step = mosaicking::grain(made_, unicode_);

    coarse_ = fit_.scaled(pixelled(placed_, step));

    if (fine_ == Graphics::kitty) {
        const auto shared = std::make_shared<const media::Pixels>(fit_);

        for (std::size_t at = 0; at < fit_.stills().size(); ++at) pictures_.push_back(Picture{marked(), placed_, nullptr, shared, at});

        return;
    }

    (void)started(encoding(++orders_, fit_, placed_));
}

// Keeps the next frame of a playing animation appointed on the document's clock, each frame shown
// for its own delay; a still picture, a paused one, one whose frames add up to no time and one
// outside a document appoint nothing.
void Image::beating() {
    const std::vector<media::Still>& stills = fit_.stills();
    const bool moving = play_ && stills.size() >= 2 && document() != nullptr &&
                        std::any_of(stills.begin(), stills.end(), [](const media::Still& one) { return one.after.count() > 0; });

    if (!moving) {
        frame_ = Timer{};

        return;
    }

    if (frame_.waiting()) return;

    frame_ = document()->after(stills[std::min(shown_, stills.size() - 1)].after, [this](Timer& one) {
        const std::vector<media::Still>& now = fit_.stills();

        if (now.size() < 2) return;

        shown_ = (shown_ + 1) % now.size();

        repaint();
        one.again(now[shown_].after);
    });
}

// The frames of `given` at `want` pixels for `room` cells, decoded on the pool; the result of an
// order a later one replaced (`order`) is dropped, and so is one for an image that is gone. A
// refusal empties the image as a header without a size would: the image shows what it can, and
// whoever wants the reason decodes the bytes themselves.
Task<void> Image::decoding(std::uint64_t order, Bytes given, media::Size want, Size room, Graphics fine) {
    const std::weak_ptr<Node> alive = weak_from_this();
    media::Pixels made;

    try {
        made = co_await media::decoded(std::move(given), want);
    } catch (const Trouble&) {
        made = {};
    }

    if (alive.expired() || order != orders_) co_return;

    ordered_ = false;

    if (!made.held()) {
        bytes_.clear();
        size_ = {};
        fit_ = {};
        placed_ = {};
        frames_ = 0;
        sharpened();
        relayout();

        co_return;
    }

    fit_ = std::move(made);
    placed_ = room;
    fine_ = fine;
    frames_ = fit_.stills().size();
    shown_ = std::min(shown_, frames_ - 1);

    sharpened();
    repaint();
    beating();
}

// The sixel of every frame of `given`, encoded on the pool, as pictures of `room` cells; dropped
// when the frames changed meanwhile (`order`) or the image is gone.
Task<void> Image::encoding(std::uint64_t order, media::Pixels given, Size room) {
    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& back = co_await detail::Here{};
    std::vector<std::shared_ptr<const std::string>> made;

    co_await to(pool());

    made.reserve(given.stills().size());

    for (std::size_t at = 0; at < given.stills().size(); ++at) made.push_back(std::make_shared<const std::string>(sixeling::sixel(given, at)));

    co_await to(back);

    if (alive.expired() || order != orders_) co_return;

    for (std::shared_ptr<const std::string>& one : made) pictures_.push_back(Picture{marked(), room, std::move(one), nullptr, 0});

    repaint();
}

// Runs `work` in the image's scope on the program's loop; false when there is no loop to come back
// to, and then the image stays as it is.
bool Image::started(Task<void> work) {
    if (!scope_) {
        try {
            scope_.emplace();
        } catch (const Trouble&) {
            return false;
        }
    }

    scope_->run(std::move(work));

    return true;
}

// Forgets the source and everything made of it, and the work still under way for it.
void Image::reset() {
    if (scope_) scope_->cancel();

    ++orders_;
    bytes_.clear();
    whole_ = {};
    format_ = media::Format::none;
    size_ = {};
    frames_ = 0;
    ordered_ = false;
    ordering_ = {};
    fit_ = {};
    coarse_ = {};
    pictures_.clear();
    shaped_ = {};
    placed_ = {};
    fine_ = Graphics::none;
    shown_ = 0;
    frame_ = Timer{};
}

// The picture in the middle of the room, as the preview of Midnight Commander stands; the rest of
// the room is left undrawn, so the parent's background shows through it. The frame on show in
// characters, and its picture over them where the terminal takes pixels.
void Image::paint(Canvas& onto) const {
    if (fit_.stills().empty()) return;

    const Size given = onto.size();
    const Spot at{(given.wide - placed_.wide) / 2, (given.high - placed_.high) / 2};
    const Display& shown = display();

    mosaicking::drawn(onto, at, fine_ != Graphics::none ? coarse_ : fit_, shown_, made_, look(), shown.colors, unicode_);

    if (shown_ < pictures_.size()) onto.picture({at, placed_}, pictures_[shown_]);
}

}

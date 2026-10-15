#include "talcite/image.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "talcite/root.hpp"
#include "talcite/sixel.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

// An empty image in the `talcite.image` role.
Image::Image() { role("talcite.image"); }

namespace {

// The grid density and the cell size do not belong to the widget. They are facts about the surface
// we draw on, so they live on the root, and `Image` reaches for them the same way it reaches for
// the skin. Outside a tree we get what a silent terminal gives: block characters and "unknown".
[[nodiscard]] Blocks grained(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->blocks() : Blocks::basic;
}

// Color depth and characters are facts about the surface too: monochrome draws only the silhouette,
// and without Unicode there are no block characters. Outside a tree: full color and Unicode, as
// with a silent terminal.
[[nodiscard]] Colors colored(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->colors() : Colors::rgb;
}

// Whether the surface shows Unicode; true outside a tree.
[[nodiscard]] bool lettered(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->unicode() : true;
}

// The size of one cell in pixels, as the root knows it; zero means unknown.
[[nodiscard]] Size celled(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->cell() : Size{};
}

// Pixels, when the surface accepts them: the terminal speaks sixel or kitty, and the cell has a
// known size; without it there is no telling how many pixels to order. `none` means characters.
[[nodiscard]] Graphics sharp(const Node& one) {
    const Root* top = one.root();

    if (top == nullptr || top->cell().wide <= 0 || top->cell().high <= 0) return Graphics::none;

    return top->graphics();
}

// An image mark: unique in the whole program, because cells of different images meet in one frame.
// Zero means "no image".
[[nodiscard]] std::uint32_t marked() {
    static std::atomic<std::uint32_t> next{0};

    return ++next;
}

// Fills `upto` with the running end time of every frame and returns the total length. A frame
// without a delay is not an animation but a still picture. The GIF decoder never produces those (it
// raises the shortest to a hundred milliseconds), but a picture made in code may consist of frames
// with zero time, and then "which frame now" has no answer, so such frames count as zero.
[[nodiscard]] std::chrono::milliseconds summed(const Pixels& one,
                                               std::vector<std::chrono::milliseconds>& upto) {
    upto.clear();
    upto.reserve(one.stills().size());

    std::chrono::milliseconds much{0};

    for (const Still& said : one.stills()) {
        much += said.after > std::chrono::milliseconds{0} ? said.after
                                                          : std::chrono::milliseconds{0};

        upto.push_back(much);
    }

    return much;
}

}

// We hold the source, not the picture. Frames are kept in memory already at cell resolution, and
// the full resolution never is: the source (`Figure::fetch`, from `figured` over bytes and a seam,
// or anything that can produce pixels in a given size) decodes on the pool and hands over frames
// already scaled down. The widget knows neither the bytes nor the seam: it gets the format and the
// shape ready, at once (layout asks for them before painting), and orders pixels once `paint` tells
// it the size. A source without a shape (without a provider) leaves the widget empty at once; a
// refusal by the source empties it when the order returns. The widget knows the number of frames
// once the first order returns.
void Image::hold(Figure given) {
    fetch_ = std::move(given.fetch);
    whole_ = {};
    format_ = given.format;
    size_ = fetch_ ? given.size : Size{};
    frames_ = 0;

    ordered_.reset();
    ordering_ = {};
    fit_ = {};
    shaped_ = {};
    placed_ = {};
    fine_ = Graphics::none;
    sharpened();
    upto_.clear();
    total_ = std::chrono::milliseconds{0};
    still_ = 0;
    began_ = std::chrono::steady_clock::now();
}

// The other way: a picture made in code, for which there is no file. Setting one source clears the
// other, so that there is no state in which both mean something.
void Image::hold(Pixels given) {
    fetch_ = {};
    ordered_.reset();
    ordering_ = {};
    whole_ = std::move(given);
    format_ = Format::none;
    size_ = whole_.size();
    frames_ = whole_.stills().size();

    fit_ = {};
    shaped_ = {};
    fine_ = Graphics::none;
    sharpened();
    upto_.clear();
    total_ = std::chrono::milliseconds{0};
    still_ = 0;
    began_ = std::chrono::steady_clock::now();
}

// Releases everything: no source, no frames, no format. A page being closed must hold neither a
// file in the source's closure nor frames.
void Image::clear() { hold(Pixels{}); }

// Whether the image has a size, that is, something to show.
bool Image::held() const { return size_.wide > 0 && size_.high > 0; }

// Busy while waiting for anything from the pool: for frames or, with pixels, for their sixel.
bool Image::busy() const { return ordered_.has_value() || encoded_.has_value(); }

// An image holds no children.
int Image::capacity() const { return 0; }

// Pausing freezes the frame that is visible, not frame zero; otherwise a pause would make the
// picture jump. Resuming starts from the frozen frame: it winds the clock as if the animation had
// just reached it, so that after a pause the picture does not jump by as many frames as time has
// passed since it was shown. A pause also takes back the appointment of the next frame.
void Image::play(bool given) {
    if (play_ == given) return;

    if (!given) {
        still_ = chosen();

        frame_.stop();
    }

    play_ = given;

    if (given) still(still_);
}

// Setting the frame is also seeking: the start of the count is moved as if the animation had just
// reached that frame. One call thus works the same for a paused and a running animation, and tests
// can check the time arithmetic without waiting.
void Image::still(std::size_t given) {
    still_ = given;

    if (upto_.empty()) return;

    still_ = std::min(still_, upto_.size() - 1);

    const std::chrono::milliseconds gone =
        still_ == 0 ? std::chrono::milliseconds{0} : upto_[still_ - 1];

    began_ = std::chrono::steady_clock::now() - gone;
}

// The frame shown now.
std::size_t Image::still() const { return chosen(); }

// Without the cell size a circle comes out as an egg, so `fitting` takes it into account; zero
// means "unknown", and then 1:2 is assumed.
Size Image::wants(Size room) const {
    if (!held()) return {0, 0};

    return fitting(size_, room, celled(*this));
}

// How long until the next frame is due; nothing for a still picture or a paused animation.
std::optional<std::chrono::milliseconds> Image::until_next() const {
    if (!play_ || upto_.size() < 2 || total_ <= std::chrono::milliseconds{0}) return {};

    const std::chrono::milliseconds gone =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              began_);

    const std::chrono::milliseconds at{gone.count() % total_.count()};

    const auto found = std::upper_bound(upto_.begin(), upto_.end(), at);

    if (found == upto_.end()) return total_ - at;

    return *found - at;
}

// The frame to show at this moment: the set frame when paused or still, otherwise the one the clock
// points at within the loop.
std::size_t Image::chosen() const {
    if (!play_ || upto_.size() < 2 || total_ <= std::chrono::milliseconds{0})
        return upto_.empty() ? still_ : std::min(still_, upto_.size() - 1);

    const std::chrono::milliseconds gone =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              began_);

    const std::chrono::milliseconds at{gone.count() % total_.count()};

    const auto found = std::upper_bound(upto_.begin(), upto_.end(), at);

    if (found == upto_.end()) return upto_.size() - 1;

    return static_cast<std::size_t>(found - upto_.begin());
}

// Frames are recomputed only when the allotted size, the grid or the cell has changed. Without
// that, a photo from disk would go through the filter ten times a second.
//
// The same comparison also serves as notification of a settings change. The first frame after
// `reconfigure` reads a different density or cell from the root, the condition breaks and the
// frames recompute themselves. Nobody has to report anything to anyone.
//
// A file is not recomputed here but ordered, in the manner of a spool: in `paint` the widget knows
// how much room it has, so here it computes the target size and calls the source with it; the
// result comes back through the loop and is taken if the size is still the same, otherwise the next
// `paint` orders again. Until then the widget paints what it had (old frames when the size changes,
// without flicker) or nothing. One order at a time; a new one replaces the old, and an old result
// is recognized by its size. The pointer to ourselves is weak: a result for a widget that has died
// has nowhere to go. A picture from code (`hold(Pixels)`) is recomputed at once, because it is in
// memory and small.
void Image::spread(Size given) {
    const Blocks kind = grained(*this);
    const bool unicode = lettered(*this);
    const Graphics fine = sharp(*this);
    const Size room = fitting(size_, given, celled(*this));

    if (shaped_ == given && made_ == kind && unicode_ == unicode && placed_ == room && fine_ == fine &&
        !fit_.stills().empty())
        return;

    if (!held() || given.wide <= 0 || given.high <= 0) {
        shaped_ = given;
        made_ = kind;
        unicode_ = unicode;
        fit_ = {};
        placed_ = {};
        fine_ = fine;
        sharpened();
        upto_.clear();
        total_ = std::chrono::milliseconds{0};
        ordered_.reset();

        return;
    }

    // With pixels we order the full resolution of the area (cells times cell pixels), and the
    // characters shown while the picture is not fully visible are computed from it in `sharpened`.
    const Size step = fine != Graphics::none ? celled(*this) : grain(kind, unicode);
    const Size want{room.wide * step.wide, room.high * step.high};

    if (fetch_) {
        if (ordered_.has_value() && ordering_ == want) return;

        shaped_ = given;
        made_ = kind;
        unicode_ = unicode;
        ordering_ = want;

        const std::weak_ptr<Image> weak = std::static_pointer_cast<Image>(weak_from_this().lock());

        // A refusal by the source (a rejected promise) and an empty result end the same way: the
        // widget is empty and the format stays. The widget shows what it can; whoever wants the
        // refusal message decodes through the seam themselves.
        const auto emptied = [](Image& self) {
            self.size_ = {};
            self.fit_ = {};
            self.placed_ = {};
            self.frames_ = 0;
            self.sharpened();
        };

        ordered_.emplace(fetch_(want)
                             .here()
                             .then([weak, want, room, fine, emptied](Pixels made) {
                                 const auto self = weak.lock();

                                 if (!self || self->ordering_ != want) return;

                                 self->ordered_.reset();

                                 if (!made.held()) {
                                     emptied(*self);

                                     return;
                                 }

                                 self->fit_ = std::move(made);
                                 self->placed_ = room;
                                 self->fine_ = fine;
                                 self->frames_ = self->fit_.stills().size();
                                 self->total_ = summed(self->fit_, self->upto_);
                                 self->still_ = std::min(self->still_, self->upto_.size() - 1);
                                 self->sharpened();
                             })
                             .recover([weak, want, emptied](const std::exception&) {
                                 const auto self = weak.lock();

                                 if (!self || self->ordering_ != want) return;

                                 self->ordered_.reset();
                                 emptied(*self);
                             }));

        return;
    }

    shaped_ = given;
    made_ = kind;
    unicode_ = unicode;
    fit_ = {};
    placed_ = {};
    fine_ = fine;
    upto_.clear();
    total_ = std::chrono::milliseconds{0};

    const Pixels one = whole_.held() ? whole_.scaled(want) : Pixels{};

    if (!one.stills().empty()) {
        placed_ = room;
        fit_ = one;
        frames_ = fit_.stills().size();
        total_ = summed(fit_, upto_);

        still_ = std::min(still_, upto_.size() - 1);
    }

    sharpened();
}

// Frames in pixels and their stand-in in characters. With pixels `fit_` has the full resolution of
// the area, so the characters (drawn while the picture is not fully visible, and before the sixel
// is ready) are computed from it here, once. Sixel is encoded on the pool, on the `cpu` lane, all
// frames at once; until then the picture stands in characters. A result for frames that no longer
// exist does not need to be recognized: an abandoned promise (`encoded_.reset()`) is cancelled and
// its `then` never arrives.
//
// Kitty encodes nothing here: the terminal gets raw pixels, and `Term` sends them, once per frame
// of the image. The pictures are therefore ready at once; all frames share one set of pixels, and
// as long as someone holds them, the terminal may keep them on its side.
void Image::sharpened() {
    encoded_.reset();
    pictures_.clear();
    coarse_ = {};

    if (fine_ == Graphics::none || fit_.stills().empty()) return;

    const Size step = grain(made_, unicode_);

    coarse_ = fit_.scaled({placed_.wide * step.wide, placed_.high * step.high});

    if (fine_ == Graphics::kitty) {
        const auto shared = std::make_shared<const Pixels>(fit_);

        for (std::size_t at = 0; at < fit_.stills().size(); ++at)
            pictures_.push_back(Picture{marked(), placed_, nullptr, shared, at});

        return;
    }

    const std::weak_ptr<Image> weak = std::static_pointer_cast<Image>(weak_from_this().lock());
    const Size room = placed_;

    using Sixels = std::vector<std::shared_ptr<const std::string>>;

    encoded_.emplace(run(cpu(), [source = fit_]() -> Sixels {
                                         Sixels made;

                                         made.reserve(source.stills().size());

                                         for (std::size_t at = 0; at < source.stills().size(); ++at)
                                             made.push_back(std::make_shared<const std::string>(sixel(source, at)));

                                         return made;
                                     })
                         .here()
                         .then([weak, room](Sixels made) {
                             const auto self = weak.lock();

                             if (!self) return;

                             self->encoded_.reset();

                             for (std::shared_ptr<const std::string>& one : made)
                                 self->pictures_.push_back(Picture{marked(), room, std::move(one)});
                         }));
}

// Placement in the area: centered, like the preview in Midnight Commander. The rest of the area is
// left undrawn, so the parent's background shows through it, as with every container.
View Image::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    spread(given);

    if (fit_.stills().empty()) return made;

    const Style look = skin().style(role());

    const Spot at{(given.wide - placed_.wide) / 2, (given.high - placed_.high) / 2};

    const Spot from{at.x - seen.at.x, at.y - seen.at.y};
    const std::size_t which = chosen();

    drawn(made, from, fine_ != Graphics::none ? coarse_ : fit_, which, made_, look, colored(*this), unicode_);

    if (fine_ != Graphics::none && which < pictures_.size()) made.picture({from, placed_}, pictures_[which]);

    // The next frame of an animation is an appointment with the root's clock, made anew at every
    // frame; it only wakes the loop, since the frame is chosen by the time when it is drawn. A still
    // picture and a paused animation make none, and neither does a picture off the tree, which is
    // not drawn.
    if (Root* const top = root(); top != nullptr) {
        if (const std::optional<std::chrono::milliseconds> left = until_next()) frame_ = top->clock().after(*left, {});
        else frame_.stop();
    }

    return made;
}

}

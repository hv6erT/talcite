#include "tal/pixels.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "tal/meta.hpp"
#include "tal/trouble.hpp"

// The preview of an image file, as cheap as it can be had: a picture the file carries already (the
// thumbnail of EXIF, the previews of a raw file) when it is large enough for what is wanted and of
// the image's proportions (cameras put black bars around a thumbnail of 4:3 for a photo of 3:2),
// the smallest such first, and the image itself decoded otherwise, reduced while it decodes as far
// as it may be (`decoded`). Either way the result fits what is wanted, keeps its proportions, is
// never enlarged, and is turned as the orientation of EXIF says. A file that does not start as an
// image tal knows, or of more than `most_file` bytes, is not read whole. The head of the file is
// read once, for the description, the picture and the format alike.
namespace tal::media {

namespace {

constexpr std::uint64_t most_file = std::uint64_t{256} << 20;
constexpr std::size_t head_size = std::size_t{64} << 10;

// A reader of the file that keeps its head (`head_size` bytes) once read: a read that lies in it
// comes from there, any other from the file.
class Headed final : public meta::Reader {
public:
    // A reader of `file`, which it keeps open while it lives.
    explicit Headed(fs::File file) : file_(std::move(file)) {}

    // The size, as the file says it now.
    Task<std::uint64_t> size() override { co_return co_await file_.size(); }

    // Up to `much` bytes from `at`, from the head when they lie in it.
    Task<Bytes> read(std::uint64_t at, std::size_t much) override {
        co_await loaded();

        if (at <= head_.size() && much <= head_.size() - at) {
            const auto from = static_cast<std::ptrdiff_t>(at);

            co_return Bytes(head_.begin() + from, head_.begin() + from + static_cast<std::ptrdiff_t>(much));
        }

        co_return co_await file_.read(at, much);
    }

    // The head of the file, read the first time it is asked for.
    [[nodiscard]] Task<Bytes> head() {
        co_await loaded();
        co_return head_;
    }

private:
    // Reads the head, once.
    Task<void> loaded() {
        if (read_) co_return;

        head_ = co_await file_.read(0, head_size);
        read_ = true;
    }

    fs::File file_;
    Bytes head_;
    bool read_ = false;
};

// Whether `one` has the proportions of `whole`, within two in a hundred.
[[nodiscard]] bool alike(Size one, Size whole) {
    const std::uint64_t across = std::uint64_t{one.width} * whole.height;
    const std::uint64_t down = std::uint64_t{one.height} * whole.width;

    return (across > down ? across - down : down - across) * 50 <= down;
}

// The size of `source` brought within `most`, its proportions kept, never larger than it is and
// at least a pixel each way; `source` itself for a `most` without size.
[[nodiscard]] Size fitted(Size source, Size most) {
    if (most.width == 0 || most.height == 0 || source.width == 0 || source.height == 0) return source;

    const double scale = std::min({static_cast<double>(most.width) / source.width, static_cast<double>(most.height) / source.height, 1.0});

    return Size{std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(source.width * scale))),
                std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(source.height * scale)))};
}

}

// The preview of the image at `where` within `most` (see above): the pictures in JPEG said
// "thumbnail" or "preview" are tried from the smallest, and the first of the image's proportions
// that is at least as large as the image brought within `most` is taken; otherwise the file is
// decoded whole, its first frame. Throws `Trouble`: unsupported for a file that is no image tal
// knows, full for one over `most_file`, and what reading the file and decoding throw.
Task<Pixels> previewed(Path where, Size most, Port<fs::Files> files) {
    fs::File file = co_await files->open(where);
    Headed from{std::move(file)};
    const meta::Description told = co_await meta::described(from);
    const std::uint8_t orientation = told.photo ? told.photo->orientation : 1;
    const Size box = orientation >= 5 && orientation <= 8 ? Size{most.height, most.width} : most;
    const std::optional<Size> whole = told.photo && told.photo->width != 0 && told.photo->height != 0 ? std::optional<Size>{Size{told.photo->width, told.photo->height}} : std::nullopt;
    std::vector<std::size_t> order;

    for (std::size_t one = 0; one < told.pictures.size(); ++one)
        if (told.pictures[one].type == "image/jpeg" && (told.pictures[one].said == "thumbnail" || told.pictures[one].said == "preview")) order.push_back(one);

    std::ranges::stable_sort(order, {}, [&told](std::size_t one) { return told.pictures[one].size; });

    for (const std::size_t one : order) {
        std::optional<Bytes> bytes;

        try {
            bytes = co_await meta::pictured(from, one);
        } catch (const Trouble&) {
            continue;
        }

        const std::optional<Size> size = size_of(*bytes);

        if (!size) continue;

        const Size needed = fitted(whole.value_or(*size), box);

        if (size->width < needed.width || size->height < needed.height || (whole && !alike(*size, *whole))) continue;

        const Pixels made = co_await decoded(std::move(*bytes), fitted(*size, box), Optional<Decoding>{defaults().get<Decoding>()}, 1);

        co_return made.oriented(orientation);
    }

    const Bytes head = co_await from.head();

    if (format_of(head) == Format::none) throw Trouble{Kind::unsupported, "Not an image tal knows."};

    const fs::Entry entry = co_await files->look(where);

    if (entry.size > most_file) throw Trouble{Kind::full, "The image is over the ceiling."};

    Bytes all = co_await files->read(where);
    const std::optional<Size> size = size_of(all);
    const Pixels made = co_await decoded(std::move(all), size ? fitted(*size, box) : Size{}, Optional<Decoding>{defaults().get<Decoding>()}, 1);

    co_return (size ? made : made.scaled(fitted(made.size(), box))).oriented(orientation);
}

}

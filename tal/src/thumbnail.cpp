#include "tal/pixels.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#include "pnging.hpp"

// Thumbnails kept for every program on tal, in the folder of caches: <cache>/tal/thumbnails, a
// folder for each side asked, a PNG of ours in it for each file, named by the SHA-256 of what
// makes the file what it is (its canonical path, its size, its time of change to the nanosecond,
// the side), so that a file changed or moved gets a new one and an old one is never shown for it.
// The thumbnails are the user's alone: the files 0600, the folders 0700. The store is a help, not
// a duty: without a folder of caches, without SHA-256 (macOS without `TAL_OWN_SUMS`), or when
// reading or writing it fails, the preview is computed and given all the same. Old thumbnails stay
// until the program trims the store (`trimmed`), which it does now and then, not at every writing,
// so as not to list thousands of files each time.
namespace tal::media {

namespace {

constexpr std::uint16_t file_rights = 0600;
constexpr std::uint16_t folder_rights = 0700;

// The folder of the thumbnails, or none when there is no folder of caches.
[[nodiscard]] Task<std::optional<Path>> store_of(fs::Files& files) {
    const std::optional<Path> cache = co_await files.known(fs::Known::cache);

    if (!cache) co_return std::nullopt;

    co_return *cache / "tal" / "thumbnails";
}

// The name of the thumbnail of the file `real` as `entry` describes it at `side`: the SHA-256 in
// hexadecimal of the path, the size, the time of change in nanoseconds and the side, each ended by
// a zero byte. Throws `Trouble` unsupported where there is no SHA-256.
[[nodiscard]] std::string named(const Path& real, const fs::Entry& entry, std::uint32_t side) {
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(entry.modified.time_since_epoch()).count();
    const std::string told = std::string{real.stored()} + '\0' + std::to_string(entry.size) + '\0' + std::to_string(nanoseconds) + '\0' + std::to_string(side) + '\0';
    const Bytes sum = crypto::summed(crypto::Sum::sha256, std::as_bytes(std::span{told}));
    std::string made;

    for (const std::byte one : sum) {
        made += "0123456789abcdef"[std::to_integer<unsigned>(one) >> 4];
        made += "0123456789abcdef"[std::to_integer<unsigned>(one) & 15];
    }

    return made + ".png";
}

// Makes the folder `where` and those above it up to `top`, each the user's alone.
[[nodiscard]] Task<void> made_private(fs::Files& files, const Path& top, const Path& where) {
    co_await files.make(where);

    for (Path one = where;; one = one.parent()) {
        co_await files.change(one, fs::Changing{.rights = folder_rights});

        if (one == top || one.parent() == one) break;
    }
}

}

// The thumbnail of the image at `where` within a square of `side`: the one kept for it, or the
// preview (`previewed`) computed and kept. Throws `Trouble`: what finding the file and computing
// the preview throw; the store's own troubles are left out.
Task<Pixels> thumbnailed(Path where, std::uint32_t side, Port<fs::Files> files) {
    const Path real = co_await files->canonical(where);
    const fs::Entry entry = co_await files->look(real);
    const std::optional<Path> store = co_await store_of(*files);
    std::optional<Path> kept;

    try {
        if (store) kept = *store / std::to_string(side) / named(real, entry, side);
    } catch (const Trouble&) {
        kept.reset();
    }

    if (kept) {
        try {
            Bytes bytes = co_await files->read(*kept);
            Pixels found = co_await decoded(std::move(bytes), Size{}, Optional<Decoding>{Decoding::builtin()}, 1);

            if (found.held()) co_return found;
        } catch (const Trouble&) {
        }
    }

    Pixels made = co_await previewed(real, Size{side, side}, files);

    if (kept && made.held()) {
        try {
            co_await made_private(*files, store->parent(), kept->parent());
            co_await files->write(*kept, pnging::encoded(made.stills().front().rgba, made.size()), fs::Writing{.rights = file_rights});
        } catch (const Trouble&) {
        }
    }

    co_return made;
}

// Trims the store of thumbnails to `most` bytes: when it holds more, the thumbnails least recently
// written go until three quarters of `most` are left, so that the next trimming is not at once.
// A store that is not there, or a thumbnail that will not go, is left as it is.
Task<void> trimmed(std::uint64_t most, Port<fs::Files> files) {
    const std::optional<Path> store = co_await store_of(*files);

    if (!store) co_return;

    std::vector<fs::Entry> all;

    try {
        all = co_await files->walk(*store);
    } catch (const Trouble&) {
        co_return;
    }

    std::erase_if(all, [](const fs::Entry& one) { return one.type != fs::Type::file; });

    std::uint64_t total = 0;

    for (const fs::Entry& one : all) total += one.size;

    if (total <= most) co_return;

    std::ranges::sort(all, {}, &fs::Entry::modified);

    for (const fs::Entry& one : all) {
        if (total <= most / 4 * 3) break;

        try {
            co_await files->remove(one.where);
            total -= one.size;
        } catch (const Trouble&) {
            continue;
        }
    }
}

}

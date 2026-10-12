#include "talcite/meta.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "fscore.hpp"

// Metadata embedded in files: the core's door. Parsing the formats (ID3, Vorbis comments, MP4
// atoms) lives behind the seam, in talcite-adapters, like the image decoders: the core provides
// the types, the provider interface and this file, which opens the file, runs the provider on the
// pool and turns its refusal sentence into `Refused`. The provider recognizes the container; the
// caller asks "what does this file say about itself" without knowing whether it reads FLAC or MP4.
//
// The provider gets a `meta::Reader`, not a lump of bytes: a FLAC file weighs tens of megabytes,
// ID3v1 sits at the very end of the file, and the MP4 moov atom often does too, so parsing jumps
// around the file instead of reading it whole. Reading is synchronous, because all the work
// already runs on the pool, on the `io` lane: parsing tags is a handful of byte comparisons, and
// the waiting is for the disk.
namespace talcite {

namespace {

// A file opened for reading, with its size remembered at opening. A read past the end returns
// fewer bytes or nothing, never an error: the provider asks about the end of the file through
// `size()`, and a short read is its sign of a truncated file. A system error (a share that
// vanished) comes out as `fs::Trouble` and rejects the promise, as in every `fs` read.
class Opened final : public meta::Reader {
public:
    explicit Opened(const fs::path& where)
        : held_(fs::core::open_at(where, fs::Mode::read)), size_(fs::core::sized(*held_)) {}

    // The size of the file at the moment it was opened.
    [[nodiscard]] std::uintmax_t size() const override { return size_; }

    // Up to `much` bytes from offset `at`, cut at the end of the file.
    [[nodiscard]] Bytes read(std::uintmax_t at, std::size_t much) override {
        if (at >= size_ || much == 0) return {};

        const std::uintmax_t left = size_ - at;

        return fs::core::read_at(*held_, at, static_cast<std::size_t>(std::min<std::uintmax_t>(much, left)));
    }

private:
    std::shared_ptr<fs::File::Held> held_;
    std::uintmax_t size_;
};

}

// The bound metadata provider; throws `Unbound` when none is bound.
MetaProvider& Seam<MetaProvider>::provider() const {
    if (provider_ == nullptr) throw Unbound{MetaProvider::kind()};

    return *provider_;
}

// Promises the description of the file's embedded metadata. A seam without a provider rejects the
// promise (`Unbound`) rather than throwing at once: the caller of a promise has one error path,
// not two. A provider's refusal rejects it with `Refused`.
Promise<meta::Description> Seam<MetaProvider>::described(fs::path where) const {
    return run(io(), [held = provider_, where = std::move(where)]() -> meta::Description {
        if (held == nullptr) throw Unbound{MetaProvider::kind()};

        Opened from{where};
        meta::Description made;

        if (const std::string said = held->describe(from, made); !said.empty())
            throw Refused{MetaProvider::kind(), said};

        return made;
    });
}

// Promises the bytes of the embedded picture with the given number, with the same errors as
// `described`.
Promise<Bytes> Seam<MetaProvider>::pictured(fs::path where, std::size_t which) const {
    return run(io(), [held = provider_, where = std::move(where), which]() -> Bytes {
        if (held == nullptr) throw Unbound{MetaProvider::kind()};

        Opened from{where};
        Bytes made;

        if (const std::string said = held->picture(from, which, made); !said.empty())
            throw Refused{MetaProvider::kind(), said};

        return made;
    });
}

}

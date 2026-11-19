#include "tal/meta.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "tal/executor.hpp"
#include "tal/trouble.hpp"

#include "tagging.hpp"

// Embedded metadata, ported from talcite-adapters' tags: the readers are our own, in tal-media,
// and run on the pool as coroutines that wait only where they reach into the file (`Walk::need`,
// and the windows the PDF reader reads by itself).
// This file recognizes the container by its magic bytes (never by the file name: a file manager
// looks at other people's disks, where the extension may not match the content) and sets the order
// of the readers, which is the order in the file: ID3v2 at the start (MP3, sometimes FLAC), then
// the container, and APE and ID3v1 at the end. A PDF document is recognized by "%PDF-" among its
// first bytes, where readers look for it, since writers may put junk before it.
//
// Trailing tags are read only for the families in which they occur: MPEG audio, WavPack, Monkey's
// Audio, Musepack. They are not looked for in FLAC, Ogg and MP4: "TAG" in the last 128 bytes of a
// FLAC stream is a coincidence, not a tag.
//
// An unknown file gives an empty description, not a refusal: "I do not know what this is" is an
// answer, not an error. The exception: when an ID3v2 tag stood before the unknown content, its
// fields stay, because the tag was real even though the stream behind it was not recognized.
namespace tal::meta {

namespace {

// A file opened through the port of files, as a reader.
class Opened final : public Reader {
public:
    // A reader of `file`, which it keeps open while it lives.
    explicit Opened(fs::File file) : file_(std::move(file)) {}

    // The size, as the file says it now.
    Task<std::uint64_t> size() override { co_return co_await file_.size(); }

    // Up to `much` bytes from `at`.
    Task<Bytes> read(std::uint64_t at, std::size_t much) override { co_return co_await file_.read(at, much); }

private:
    fs::File file_;
};

// A file opened elsewhere, read through a reference, so that its owner can close it.
class Borrowed final : public Reader {
public:
    // A reader of `file`, which must outlive it.
    explicit Borrowed(const fs::File& file) : file_(file) {}

    // The size, as the file says it now.
    Task<std::uint64_t> size() override { co_return co_await file_.size(); }

    // Up to `much` bytes from `at`.
    Task<Bytes> read(std::uint64_t at, std::size_t much) override { co_return co_await file_.read(at, much); }

private:
    const fs::File& file_;
};

// How many bytes of the file are copied at a time when it is written again.
constexpr std::size_t copied_piece = std::size_t{1} << 20;

// Whether `head` holds the magic bytes at offset `at`.
bool starts(const Bytes& head, std::string_view magic, std::size_t at = 0) {
    return head.size() >= at + magic.size() && std::memcmp(head.data() + at, magic.data(), magic.size()) == 0;
}

// APE and ID3v1 from the end, in file order: first APE, then ID3v1, although they have to be read
// from the back, because only the end of ID3v1 tells where APE ends. Returns the end of the stream
// before the tags. ID3v1 is collected aside, within the budgets of the whole file (`Spent`), and
// the damage of what did not fit in them comes along.
Task<std::uint64_t> trailing(Reader& from, std::uint64_t size, tagging::Gathered& out) {
    Description last;
    tagging::Gathered aside{last, out};
    const std::uint64_t before = co_await tagging::id3v1(from, size, size, aside);
    const std::uint64_t made = co_await tagging::ape(from, size, before, out);

    for (Field& one : last.fields) out.into.fields.push_back(std::move(one));

    if (last.damaged) out.damaged();

    co_return made;
}

// The description with what a file of one sound lacks: it has no list of tracks, so its one stream
// is made from the codec the format names and the properties. Containers list their tracks
// themselves and name no codec here.
Description completed(Description made, const std::string& codec) {
    if (codec.empty()) return made;

    Stream sound;

    sound.kind = Stream::Kind::audio;
    sound.codec = codec;
    sound.channels = made.properties.channels;
    sound.rate = made.properties.rate;
    sound.depth = made.properties.depth;
    sound.bitrate = made.properties.bitrate;

    made.streams.push_back(std::move(sound));

    return made;
}

}

// The number of bytes.
Task<std::uint64_t> Held::size() { co_return bytes_.size(); }

// Up to `much` bytes from position `at`: fewer near the end, none past it.
Task<Bytes> Held::read(std::uint64_t at, std::size_t much) {
    if (at >= bytes_.size()) co_return Bytes{};

    const auto from = static_cast<std::size_t>(at);
    const std::size_t taken = std::min(much, bytes_.size() - from);

    co_return Bytes(bytes_.begin() + static_cast<std::ptrdiff_t>(from), bytes_.begin() + static_cast<std::ptrdiff_t>(from + taken));
}

// Reads the whole file into `out`: ID3v2 and a leading APE tag first, then the container recognized
// by its magic bytes, which decides whether the trailing tags are read and where the stream ends.
// Describing a file and handing out a picture both go through here. The size of the file sets the
// budget of unpacking (`unpackable`).
Task<void> tagging::gathered(Reader& from, Gathered& out) {
    Walk walk = co_await Walk::over(from);
    const std::uint64_t size = walk.size();

    out.spent->size = size;

    const bool tagged = co_await id3v2_at(walk);
    std::uint64_t audio = co_await id3v2(walk, out);

    if (out.done()) co_return;

    audio = co_await ape_ahead(walk, out);

    if (out.done()) co_return;

    const Bytes head = co_await from.read(audio, 12);

    if (starts(head, "fLaC")) {
        Walk stream{from, size, audio, size};

        co_await flac(stream, out);

        co_return;
    }

    if (!tagged && starts(head, "OggS")) {
        co_await ogg(from, size, out);

        co_return;
    }

    if (!tagged && starts(head, "\xFF\xD8\xFF")) {
        co_await jpeg(from, size, out);

        co_return;
    }

    if (!tagged && starts(head, "\x89PNG\r\n\x1A\n")) {
        co_await png(from, size, out);

        co_return;
    }

    if (!tagged && starts(head, "RIFF") && starts(head, "WEBP", 8)) {
        co_await webp(from, size, out);

        co_return;
    }

    if (!tagged && (starts(head, std::string_view{"II*\0", 4}) || starts(head, std::string_view{"MM\0*", 4}) || starts(head, "IIRO") ||
                    starts(head, "IIRS") || starts(head, "MMOR") || starts(head, std::string_view{"IIU\0", 4}))) {
        co_await tiff(from, size, out);

        co_return;
    }

    if (!tagged && starts(head, "ftyp", 4)) {
        const std::string_view brand = head.size() >= 12 ? std::string_view{reinterpret_cast<const char*>(head.data()) + 8, 4} : std::string_view{};
        const bool avif = brand == "avif" || brand == "avis";

        if (avif || brand == "heic" || brand == "heix" || brand == "heim" || brand == "heis" || brand == "hevc" || brand == "hevx" || brand == "mif1" ||
            brand == "msf1") {
            co_await heif(from, size, avif, out);

            co_return;
        }

        co_await mp4(from, size, out);

        co_return;
    }

    if (!tagged && starts(head, "\x1A\x45\xDF\xA3")) {
        co_await matroska(from, size, out);

        co_return;
    }

    if (!tagged && starts(head, "RIFF") && starts(head, "AVI ", 8)) {
        co_await avi(from, size, out);

        co_return;
    }

    if (starts(head, "wvpk")) {
        const std::uint64_t end = co_await trailing(from, size, out);
        Walk stream{from, size, audio, end};

        co_await wavpack(stream, out);

        co_return;
    }

    if (starts(head, "MAC ")) {
        out.into.type = "audio/x-ape";
        out.codec = "ape";
        (void)co_await trailing(from, size, out);

        co_return;
    }

    if (starts(head, "MPCK") || starts(head, "MP+")) {
        out.into.type = "audio/x-musepack";
        out.codec = "musepack";
        (void)co_await trailing(from, size, out);

        co_return;
    }

    if (!tagged) {
        const bool document = co_await pdf_at(from, size);

        if (document) {
            co_await pdf_document(from, size, out);

            co_return;
        }
    }

    // MPEG audio has no signature, so we look for frames, and only when there are some (or an ID3v2
    // tag stood before the content) do the trailing tags belong to this file. They have to be read
    // earlier, though, because they mark the end of the stream, so they are collected aside and
    // joined only after recognition. The aside picture list starts with the pictures already
    // gathered, so that picture numbers from APE continue after those from ID3v2.
    Description trail;
    Gathered after{trail, out};

    trail.pictures = out.into.pictures;

    const std::uint64_t end = co_await trailing(from, size, after);
    Walk stream{from, size, audio, end};
    const bool heard = co_await mpeg(stream, out);

    if (!heard && !tagged) co_return;

    for (Field& one : trail.fields) out.into.fields.push_back(std::move(one));

    for (std::size_t at = out.into.pictures.size(); at < trail.pictures.size(); ++at) out.into.pictures.push_back(std::move(trail.pictures[at]));

    out.found = out.found || after.found;
    out.refused = out.refused.empty() ? after.refused : out.refused;

    if (trail.damaged) out.damaged();
}

// The description of what `from` holds, read on the pool. Never refuses what it reads: an unknown
// or damaged file still gives a description, possibly empty or marked `damaged`; the reader's own
// trouble (a file that cannot be read) passes on.
Task<Description> described(Reader& from) {
    Executor& back = co_await detail::Here{};
    Description made;
    std::string codec;
    std::exception_ptr failed;

    co_await to(pool());

    try {
        tagging::Gathered out{made};

        co_await tagging::gathered(from, out);

        codec = out.codec;
    } catch (...) {
        failed = std::current_exception();
    }

    co_await to(back);

    if (failed) std::rethrow_exception(failed);

    co_return completed(std::move(made), codec);
}

// The description of the file `where`, opened through `files`.
Task<Description> described(Path where, Port<fs::Files> files) {
    fs::File file = co_await files->open(where);
    Opened from{std::move(file)};
    Description made = co_await described(from);

    co_return made;
}

// The bytes of picture number `which`, counted as in the description's list of pictures, read on
// the pool. Throws `Trouble`: missing when there is no such picture, full for a picture over the
// limit, and the reader's own.
Task<Bytes> pictured(Reader& from, std::size_t which) {
    Executor& back = co_await detail::Here{};
    Description seen;
    Bytes made;
    std::string refused;
    bool found = false;
    std::exception_ptr failed;

    co_await to(pool());

    try {
        tagging::Gathered out{seen, which, &made};

        co_await tagging::gathered(from, out);

        refused = out.refused;
        found = out.found;
    } catch (...) {
        failed = std::current_exception();
    }

    co_await to(back);

    if (failed) std::rethrow_exception(failed);

    if (!refused.empty()) throw Trouble{Kind::full, refused};

    if (!found) throw Trouble{Kind::missing, "There is no such picture."};

    co_return made;
}

// The bytes of picture number `which` of the file `where`, opened through `files`.
Task<Bytes> pictured(Path where, std::size_t which, Port<fs::Files> files) {
    fs::File file = co_await files->open(where);
    Opened from{std::move(file)};
    Bytes made = co_await pictured(from, which);

    co_return made;
}

// Writes the edit and the changes into the file `where` (JPEG, PNG or WebP), through `files`: a
// link is followed to the file it leads to, which is written; the new file is written beside that
// one under a hidden name, the pieces the plan keeps copied from the file, the rest written anew;
// then, when the file has kept its size and its time of change, the new one takes its rights and
// its place. What goes wrong on the way removes the new file and leaves the old one as it was.
// Throws `Trouble`: the plan's (unsupported, malformed), unsupported for what is not a file, busy
// when the file changed meanwhile, and the files' own.
Task<void> written(Path where, Edit edit, std::vector<Change> changes, Port<fs::Files> files) {
    where = co_await files->canonical(where);

    const fs::Entry before = co_await files->look(where);

    if (before.type != fs::Type::file) throw Trouble{Kind::unsupported, "Only a file is written: " + where.text()};

    fs::File source = co_await files->open(where);
    Borrowed from{source};
    const std::vector<tagging::Piece> plan = co_await tagging::planned(from, edit, changes);
    std::optional<Path> temporary;
    fs::File out;

    for (int attempt = 0; attempt < 16 && !temporary; ++attempt) {
        const Path candidate = where.parent() / ("." + std::string{where.name()} + ".tal-" + std::to_string(attempt));

        try {
            out = co_await files->open(candidate, fs::Mode::exclusive);
            temporary = candidate;
        } catch (const Trouble& why) {
            if (why.kind() != Kind::exists) throw;
        }
    }

    if (!temporary) throw Trouble{Kind::exists, "No free name for the new file beside " + where.text() + "."};

    std::exception_ptr failed;

    try {
        std::uint64_t at = 0;

        for (const tagging::Piece& one : plan) {
            if (!one.made.empty()) {
                co_await out.write(at, one.made);
                at += one.made.size();

                continue;
            }

            for (std::uint64_t done = 0; done < one.much;) {
                const auto much = static_cast<std::size_t>(std::min<std::uint64_t>(copied_piece, one.much - done));
                Bytes part = co_await source.read(one.from + done, much);

                if (part.size() < much) throw Trouble{Kind::busy, "The file shrank while it was being written."};

                co_await out.write(at, std::move(part));
                at += much;
                done += much;
            }
        }

        co_await out.flush();
        out.close();
        source.close();

        const fs::Entry now = co_await files->look(where);

        if (now.size != before.size || now.modified != before.modified) throw Trouble{Kind::busy, "The file changed while it was being written."};

        if (before.rights) co_await files->change(*temporary, fs::Changing{.rights = before.rights});

        co_await files->move(*temporary, where, fs::Conflict::replace);
    } catch (...) {
        failed = std::current_exception();
    }

    if (!failed) co_return;

    out.close();

    try {
        co_await files->remove(*temporary);
    } catch (const Trouble&) {
    }

    std::rethrow_exception(failed);
}

}

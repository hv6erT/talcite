#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "tal/trouble.hpp"

#include "inflating.hpp"
#include "pdfing.hpp"
#include "tagging.hpp"

// The structure of a PDF file (ISO 32000-2, 7.5 and 7.7), ported from talcite-adapters' tags, read
// for the description of a document: the version, the pages and their sizes, which pages carry
// text, the fonts and whether they are embedded, links, bookmarks, a form and the fields of the
// document information dictionary, or that the file is encrypted. Nothing is rendered: content
// streams are never interpreted, fonts and images never decoded, scripts never looked at. That is
// where the dangerous parts of PDF readers are, and none of it is needed to say what a document is.
// One object is read by the parser of objects (`Syntax`); this file knows the file around them.
//
// The way through the file: the header gives the version and where the file starts (writers put
// junk before it, and then offsets may count from the header); "startxref" at the end gives the
// newest cross-reference section, a table or a stream (PDF 1.5), and each section points to the
// one before it (`/Prev`), so that an incrementally updated file is read newest first and older
// entries never cover newer ones. A hybrid file has a table for old readers and a stream for new
// ones (`/XRefStm`); the stream's entries come first, because the table marks the objects in
// object streams as free. Objects live either at an offset or inside an object stream, which is
// one compressed stream holding many objects.
//
// The file is reached only through the reader, a window at a time, and every method that reads
// waits for it as a coroutine; what only computes stays synchronous. An object is read once and
// kept: `numbered`, `resolved` and `unpacked` hand out pointers into caches that never move or
// drop what they hold while the file is read (the nodes of a map, the elements of a deque), so a
// pointer stays good across every later wait.
//
// A damaged file is read as far as it goes, and the description says it is `damaged`: when the
// cross-reference is missing or lies, the whole file is scanned for "N G obj" and "trailer"
// (`rebuild`), the way every reader repairs files, and the object streams found are indexed too.
// A place that lies once the walk has begun is not repaired, but the file is damaged all the same.
//
// Limits, because the file is foreign: one object's text at most `most_block`, a decoded stream at
// most `most_packet`, `most_resolved` objects per file, `most_sections` cross-reference sections
// (and as many trailers and cross-reference streams found by a scan), at most `most_numbers`
// entries in one section, `most_packs` object streams indexed by a scan, a page tree `tree_depth`
// deep, and `read_times` the size of the file and `read_extra` more read from it in reads larger
// than a first window (`fetched`); past that no such read is made, and the file is damaged. What
// all its streams unpack to together is counted against the file's budget of unpacking
// (`unpackable`), since the objects of every object stream stay in memory and a small stream can
// unpack to megabytes. A damaged file larger than `most_scanned` is not scanned at all. Every
// reference is followed with a guard against cycles, and a dictionary or an array that pages share
// is looked at once. DEFLATE goes through our own `Inflater` in pieces of `piece` bytes, so a small
// stream that unpacks to gigabytes stops at the limit, after at most one piece's worth of output
// past it; bytes after a whole stream are left unread, since some writers count an end of line
// into the length.
//
// An encrypted file says that it is encrypted, and its version, and nothing more: its strings and
// streams are encrypted, so whatever could still be read of it would be a part, different from
// file to file. It is a decision: encrypted files are not opened.
namespace tal::meta::tagging {

namespace {

using pdf::Array;
using pdf::Dict;
using pdf::Entry;
using pdf::Name;
using pdf::Null;
using pdf::Object;
using pdf::Ref;
using pdf::Stream;
using pdf::Syntax;

// The first read for one object; a larger one follows when the object goes on past it.
constexpr std::size_t first_window = 4096;

// How many objects one file may make us read.
constexpr std::size_t most_resolved = std::size_t{1} << 18;

// The steps of work of every file read so far (`pdf_looks`), each file's added once it is done.
std::atomic<std::uint64_t> looks_made{0};

// How many cross-reference sections we follow through `/Prev`.
constexpr std::size_t most_sections = 512;

// The highest object number we accept; a file of more objects than this is not a document.
constexpr std::uint64_t most_numbers = std::uint64_t{1} << 24;

// The largest damaged file the scan reads; a larger one is not scanned.
constexpr std::uint64_t most_scanned = std::uint64_t{1} << 30;

// How much one file may make us read in reads larger than a first window: `read_times` its size and
// `read_extra` more, several times what reading any file needs; a budget that would not fit in 64
// bits is `unbounded`.
constexpr std::uint64_t read_times = 4;
constexpr std::uint64_t read_extra = std::uint64_t{256} << 20;
constexpr std::uint64_t unbounded = std::numeric_limits<std::uint64_t>::max();

// The keys of a trailer that the reading needs; the others are not kept.
constexpr std::array<std::string_view, 3> trailer_keys{"Root", "Info", "Encrypt"};

// How many pages we count, how many page sizes we tell apart, how deep the page tree may go.
constexpr std::uint32_t most_pages = std::uint32_t{1} << 20;
constexpr std::size_t most_sheets = 64;
constexpr std::size_t tree_depth = 256;

// How many annotations of one page we look at, how many object streams a scan indexes.
constexpr std::size_t most_annotations = std::size_t{1} << 14;
constexpr std::size_t most_packs = 10000;

// How deep forms inside forms are followed when looking for fonts.
constexpr std::size_t form_depth = 4;

// How many bytes go to the inflater at once.
constexpr std::size_t piece = 1024;

// Where the header may stand and how far from the end "startxref" is looked for.
constexpr std::size_t head_room = 1024;
constexpr std::size_t tail_room = 4096;

// A larger read: the first one of a cross-reference table, that of a trailer the scan found, and
// each piece of a stream searched for its end.
constexpr std::size_t large_window = std::size_t{64} << 10;

// The chunk of a scan and how much of it is read again with the next one. A word belongs to the
// chunk it starts in, up to the middle of the overlap, so every word is seen by exactly one
// chunk, with half the overlap before it and half after it in sight: the look ahead into an
// object's dictionary (`scan_head`) and the look back for its number and generation.
constexpr std::size_t scan_chunk = std::size_t{1} << 20;
constexpr std::size_t scan_overlap = 512;
constexpr std::size_t scan_head = 256;

static_assert(scan_overlap / 2 >= scan_head && scan_overlap < scan_chunk);

// How many filters one stream may chain, and how long a row of a predictor may be.
constexpr std::size_t most_filters = 8;
constexpr std::size_t most_row = std::size_t{1} << 20;

// Where an object lives: nowhere (free), at an offset of the file, or inside an object stream as
// its `index`-th object.
struct Place {
    enum class Kind : std::uint8_t { free, direct, packed };

    Kind kind = Kind::free;
    std::uint64_t at = 0;
    std::uint32_t index = 0;
};

// One object of an object stream: its number and where its text lies, from `at` up to `end`,
// counted from the start of the objects.
struct Slot {
    std::uint32_t number = 0;
    std::uint32_t at = 0;
    std::uint32_t end = 0;
};

// A decoded object stream: its bytes, its objects in the order of its table, the objects' indexes
// in `table` sorted by number (`numbers`), and where the objects start (`first`).
struct Packed {
    Bytes data;
    std::vector<Slot> table;
    std::vector<std::uint32_t> numbers;
    std::size_t first = 0;
};

// One cross-reference section: its trailer and its entries in file order.
struct Section {
    Dict trailer;
    std::vector<std::pair<std::uint32_t, Place>> entries;
};

// What the scan of a damaged file notes besides the places: where the newest `most_sections`
// trailers start, which objects look like cross-reference streams, which like object streams, and
// which like the catalog.
struct Scan {
    std::set<std::uint64_t> trailers;
    std::set<std::uint32_t> streams;
    std::set<std::uint32_t> packs;
    std::set<std::uint32_t> catalogs;
};

// An object read from where the cross-reference places it, or none. A missing one was free or
// unknown, which makes it the null object; one that is none without being missing was not where
// its place said.
struct Loaded {
    std::optional<Object> object;
    bool missing = false;
};

// A rectangle as the four numbers PDF writes, lower left and upper right.
using Box = std::array<double, 4>;

// What a page takes from the nodes above it in the tree (ISO 32000-2, 7.7.3.4).
struct Inherited {
    const Object* resources = nullptr;
    Box media{0, 0, 612, 792};
    std::optional<Box> crop;
    std::int64_t rotate = 0;
};

// A view of bytes as a span.
[[nodiscard]] std::span<const std::byte> spanned(const Bytes& bytes) { return {bytes.data(), bytes.size()}; }

// Where the text occurs in the bytes from `from` on, or nothing. The search of the standard library
// looks for the first character with memchr, which matters for the scan and the searches for an
// end of a stream, the only reads of a file that go through all of it.
[[nodiscard]] std::optional<std::size_t> found(const Bytes& bytes, std::string_view text, std::size_t from = 0) {
    const std::size_t at = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()}.find(text, from);

    if (at == std::string_view::npos) return std::nullopt;

    return at;
}

// The byte at `at` as a number, or -1 outside the bytes.
[[nodiscard]] int byte_at(const Bytes& bytes, std::size_t at) { return at < bytes.size() ? std::to_integer<int>(bytes[at]) : -1; }

// Undoes the predictor of a Flate stream (ISO 32000-2, 7.4.4.4): the PNG filters, one per row with
// the filter's number before the row, and TIFF predictor 2 for eight-bit samples. Cross-reference
// streams use PNG Up almost always. Says nothing for a predictor that does not exist or parameters
// out of all proportion (a row over `most_row`); a short last row is kept as far as it goes.
[[nodiscard]] std::optional<Bytes> predicted(Bytes data, const Dict* parms) {
    if (parms == nullptr) return data;

    const auto value = [parms](std::string_view key, std::int64_t fallback) {
        const Object* one = parms->find(key);
        const std::optional<std::int64_t> got = one == nullptr ? std::nullopt : pdf::integer_of(*one);

        return got.value_or(fallback);
    };

    const std::int64_t predictor = value("Predictor", 1);

    if (predictor == 1) return data;

    const std::int64_t colors = value("Colors", 1);
    const std::int64_t bits = value("BitsPerComponent", 8);
    const std::int64_t columns = value("Columns", 1);

    if (colors < 1 || colors > 32 || columns < 1 || columns > (std::int64_t{1} << 20)) return std::nullopt;

    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) return std::nullopt;

    const auto step = static_cast<std::size_t>(std::max<std::int64_t>(1, colors * bits / 8));
    const auto row = static_cast<std::size_t>((colors * bits * columns + 7) / 8);

    if (row > most_row) return std::nullopt;

    if (predictor == 2) {
        if (bits != 8) return std::nullopt;

        for (std::size_t start = 0; start < data.size(); start += row)
            for (std::size_t at = start + step; at < std::min(start + row, data.size()); ++at)
                data[at] = static_cast<std::byte>(std::to_integer<int>(data[at]) + std::to_integer<int>(data[at - step]));

        return data;
    }

    if (predictor < 10 || predictor > 15) return std::nullopt;

    Bytes made;
    std::vector<int> prior(row, 0);
    std::vector<int> current(row, 0);

    made.reserve(data.size());

    for (std::size_t at = 0; at < data.size(); at += row + 1) {
        const int filter = std::to_integer<int>(data[at]);
        const std::size_t much = std::min(row, data.size() - at - 1);

        for (std::size_t one = 0; one < row; ++one) {
            const int raw = one < much ? std::to_integer<int>(data[at + 1 + one]) : 0;
            const int left = one >= step ? current[one - step] : 0;
            const int up = prior[one];
            const int corner = one >= step ? prior[one - step] : 0;

            int guess = 0;

            switch (filter) {
                case 0: break;
                case 1: guess = left; break;
                case 2: guess = up; break;
                case 3: guess = (left + up) / 2; break;
                case 4: {
                    const int all = left + up - corner;
                    const int to_left = std::abs(all - left);
                    const int to_up = std::abs(all - up);
                    const int to_corner = std::abs(all - corner);

                    guess = to_left <= to_up && to_left <= to_corner ? left : to_up <= to_corner ? up : corner;

                    break;
                }
                default: return std::nullopt;
            }

            current[one] = (raw + guess) & 0xFF;
        }

        for (std::size_t one = 0; one < much; ++one) made.push_back(static_cast<std::byte>(current[one]));

        std::swap(prior, current);
    }

    return made;
}

// A rectangle from an array of four numbers, its corners put in order; nothing for anything else.
[[nodiscard]] std::optional<Box> box_of(const Array* numbers) {
    if (numbers == nullptr || numbers->size() != 4) return std::nullopt;

    Box made{};

    for (std::size_t at = 0; at < 4; ++at) {
        const std::optional<double> one = pdf::number_of((*numbers)[at]);

        if (!one || !std::isfinite(*one)) return std::nullopt;

        made[at] = *one;
    }

    return Box{std::min(made[0], made[2]), std::min(made[1], made[3]), std::max(made[0], made[2]), std::max(made[1], made[3])};
}

// A name's bytes as UTF-8: names are bytes, almost always ASCII, and a broken sequence becomes the
// replacement character instead of passing on.
[[nodiscard]] std::string cleaned(std::string_view name) {
    return utf8(std::span<const std::byte>{reinterpret_cast<const std::byte*>(name.data()), name.size()});
}

// Whether the character can be part of a keyword or a number (neither white space nor a delimiter).
[[nodiscard]] bool regular(int one) { return one >= 0 && !pdf::spacing(one) && !pdf::delimiting(one); }

// Whether the character is a decimal digit.
[[nodiscard]] bool digit(int one) { return one >= '0' && one <= '9'; }

// The reader of one file. It lives for one description and holds what was learned on the way: the
// places of objects, the objects already read, the decoded object streams.
class File {
public:
    // A reader of `size` bytes of `from`, describing into `out`; what it may read in all is counted
    // from the size (`read_times`, `read_extra`), without overflow for any size.
    File(Reader& from, std::uint64_t size, Gathered& out)
        : from_(from), size_(size), out_(out),
          budget_(size > (unbounded - read_extra) / read_times ? unbounded : size * read_times + read_extra) {}

    // Adds the steps this file took to those of every file (`pdf_looks`).
    ~File() { looks_made.fetch_add(looks_, std::memory_order_relaxed); }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    [[nodiscard]] Task<void> read();

private:
    [[nodiscard]] Task<Bytes> fetched(std::uint64_t at, std::size_t much);

    [[nodiscard]] std::optional<std::uint64_t> shifted(std::uint64_t at) const;

    [[nodiscard]] Task<void> headed();

    [[nodiscard]] Task<std::optional<std::uint64_t>> started();

    [[nodiscard]] Task<bool> chained(std::uint64_t first);

    [[nodiscard]] Task<std::optional<Section>> section(std::uint64_t at);

    [[nodiscard]] Task<std::optional<Section>> tabled(std::uint64_t at);

    [[nodiscard]] Task<std::optional<Section>> streamed(std::uint64_t at);

    void placed(const std::vector<std::pair<std::uint32_t, Place>>& entries);

    void merged(const Dict& older);

    [[nodiscard]] Task<void> rebuild();

    [[nodiscard]] Task<bool> catalogued(std::uint32_t number);

    void scanned(const Bytes& text, std::uint64_t at, std::size_t to, Scan& into);

    [[nodiscard]] Task<std::optional<Object>> indirect(std::uint64_t at, std::optional<std::uint32_t> number);

    [[nodiscard]] Task<std::optional<Object>> located(std::uint64_t at, std::uint32_t number);

    [[nodiscard]] Task<Loaded> loaded(std::uint32_t number);

    [[nodiscard]] Task<const Object*> numbered(std::uint32_t number);

    [[nodiscard]] Task<const Object*> resolved(const Object* one);

    [[nodiscard]] Task<const Packed*> unpacked(std::uint32_t number);

    [[nodiscard]] Task<std::optional<Bytes>> data_of(const Stream& one);

    [[nodiscard]] Task<std::optional<Bytes>> ended(std::uint64_t at);

    [[nodiscard]] Task<std::optional<Bytes>> decoded(const Stream& one);

    [[nodiscard]] std::optional<Bytes> inflated(const Bytes& packed);

    // Whether the file is encrypted: its trailer names an encryption dictionary. The trailer holds
    // only `trailer_keys`, so the look is short however many keys the file gave.
    [[nodiscard]] bool sealed() const { return trailer_.find("Encrypt") != nullptr; }

    [[nodiscard]] Task<void> paged(const Object& root);

    [[nodiscard]] Task<void> page(const Dict& leaf, const Inherited& from);

    void sheet(double width, double height);

    [[nodiscard]] Task<bool> lettered(const Object* resources, std::size_t depth);

    [[nodiscard]] Task<bool> drew(const Dict* objects, std::size_t depth);

    [[nodiscard]] Task<bool> formed(const Object& value, std::size_t depth);

    [[nodiscard]] Task<void> font(const Object& value);

    [[nodiscard]] Task<bool> embedded(const Dict* font);

    [[nodiscard]] Task<void> bookmarked(const Dict& catalog);

    [[nodiscard]] Task<void> informed();

    Reader& from_;
    std::uint64_t size_;
    Gathered& out_;
    std::uint64_t budget_;
    std::uint64_t fetched_ = 0;
    Document document_;
    std::uint64_t shift_ = 0;
    std::string version_;
    Dict trailer_;
    std::map<std::uint32_t, Place> places_;
    std::map<std::uint32_t, Object> cache_;
    std::deque<Packed> packs_;
    std::map<std::uint32_t, const Packed*> packed_;
    std::set<std::uint32_t> resolving_;
    std::map<const Dict*, bool> drawn_;
    std::map<std::uint32_t, bool> forms_;
    std::set<const Dict*> named_;
    std::set<std::uint32_t> fonts_;
    std::set<const Array*> annotated_;
    std::size_t spent_ = 0;
    std::uint64_t looks_ = 0;
    bool rebuilt_ = false;
    bool settled_ = false;
    const Object none_{Null{}};
};

// The bytes from the offset, at most `much` and none past the size the file claims. Every read of
// the file goes through here, and every read larger than a first window is counted, so that no
// file, whatever it says, makes us read more than `budget_` bytes in such reads: one that would go
// past it gives nothing, and the file is damaged. The smaller reads are the first windows of
// objects, the starts of sections and the bytes after a stream, and their number has limits of
// its own (`most_resolved`, `most_sections`); counted, they would cut short a large file of many
// small objects that breaks no rule.
Task<Bytes> File::fetched(std::uint64_t at, std::size_t much) {
    if (at >= size_) co_return Bytes{};

    const auto clamped = static_cast<std::size_t>(std::min<std::uint64_t>(much, size_ - at));

    if (clamped > first_window) {
        if (clamped > budget_ - fetched_) {
            out_.damaged();

            co_return Bytes{};
        }

        fetched_ += clamped;
    }

    co_return co_await from_.read(at, clamped);
}

// The offset moved by the junk before the header, for a second try at an offset that may count
// from the header; nothing without junk, or when the moved offset would leave the file (an offset
// near 2^64 must not wrap around to the start).
std::optional<std::uint64_t> File::shifted(std::uint64_t at) const {
    if (shift_ == 0 || at >= size_ || shift_ > size_ - at) return std::nullopt;

    return at + shift_;
}

// The header: where "%PDF-" stands among the first bytes, which is where the file starts, and the
// version after it.
Task<void> File::headed() {
    const Bytes head = co_await fetched(0, head_room);
    const std::optional<std::size_t> at = found(head, "%PDF-");

    if (!at) co_return;

    shift_ = *at;

    const std::size_t from = *at + 5;

    if (digit(byte_at(head, from)) && byte_at(head, from + 1) == '.' && digit(byte_at(head, from + 2)))
        version_ = {static_cast<char>(byte_at(head, from)), '.', static_cast<char>(byte_at(head, from + 2))};
}

// The offset after the last "startxref" near the end of the file, or nothing when there is none or
// it points past the file.
Task<std::optional<std::uint64_t>> File::started() {
    const auto much = static_cast<std::size_t>(std::min<std::uint64_t>(size_, tail_room));
    const Bytes tail = co_await fetched(size_ - much, much);

    std::optional<std::size_t> last;

    for (std::optional<std::size_t> at = found(tail, "startxref"); at; at = found(tail, "startxref", *at + 1)) last = at;

    if (!last) co_return std::nullopt;

    Syntax syntax{spanned(tail), *last + 9};

    const std::optional<std::int64_t> offset = syntax.integer();

    if (!offset || *offset < 0 || static_cast<std::uint64_t>(*offset) >= size_) co_return std::nullopt;

    co_return static_cast<std::uint64_t>(*offset);
}

// Follows the cross-reference sections from the newest one through `/Prev`, newest entries first,
// and says whether a trailer with a catalog came of it. Every offset is tried as it is and then
// through the junk before the header (`shifted`), the stream of a hybrid file too. A section that
// cannot be read after the first ends the chain and marks the file damaged, and so does the stream
// of a hybrid file that cannot be read; a loop in the chain ends it quietly, and the stream of a
// hybrid file that sections name again is read once.
Task<bool> File::chained(std::uint64_t first) {
    std::set<std::uint64_t> seen;
    std::set<std::uint64_t> hybrids;
    std::optional<std::uint64_t> next = first;
    std::size_t sections = 0;
    bool any = false;

    while (next && sections++ < most_sections) {
        const std::uint64_t at = *next;

        next.reset();

        if (!seen.insert(at).second) break;

        std::optional<Section> got = co_await section(at);
        const std::optional<std::uint64_t> moved = shifted(at);

        if (!got && moved) got = co_await section(*moved);

        if (!got) {
            if (any) out_.damaged();

            break;
        }

        const Object* hybrid = got->trailer.find("XRefStm");
        const std::optional<std::int64_t> stream_at = hybrid == nullptr ? std::nullopt : pdf::integer_of(*hybrid);

        if (stream_at && *stream_at >= 0 && hybrids.insert(static_cast<std::uint64_t>(*stream_at)).second) {
            const auto from = static_cast<std::uint64_t>(*stream_at);

            std::optional<Section> stream = co_await streamed(from);
            const std::optional<std::uint64_t> stream_moved = shifted(from);

            if (!stream && stream_moved) stream = co_await streamed(*stream_moved);

            if (stream) placed(stream->entries);
            else out_.damaged();
        }

        placed(got->entries);
        merged(got->trailer);

        any = true;

        if (const Object* prev = got->trailer.find("Prev")) {
            const std::optional<std::int64_t> where = pdf::integer_of(*prev);

            if (where && *where >= 0) next = static_cast<std::uint64_t>(*where);
        }
    }

    co_return any && trailer_.find("Root") != nullptr;
}

// The cross-reference section at the offset: a table, or else a stream.
Task<std::optional<Section>> File::section(std::uint64_t at) {
    std::optional<Section> table = co_await tabled(at);

    if (table) co_return table;

    co_return co_await streamed(at);
}

// A cross-reference table (ISO 32000-2, 7.5.4) with its trailer. Entries are read as words, not as
// the twenty bytes the standard fixes, because writers end the lines in every way. An entry in use
// at offset zero is a lie and is left out. While the table goes on past the window, a window four
// times larger follows, up to `most_packet`.
Task<std::optional<Section>> File::tabled(std::uint64_t at) {
    if (at >= size_) co_return std::nullopt;

    for (std::size_t reach = large_window;; reach *= 4) {
        const auto much = static_cast<std::size_t>(std::min<std::uint64_t>(reach, size_ - at));
        const Bytes text = co_await fetched(at, much);
        const bool whole = text.size() < much || at + text.size() >= size_ || reach >= most_packet;

        Syntax syntax{spanned(text)};

        if (!syntax.keyword("xref")) co_return std::nullopt;

        Section made;
        bool broken = false;

        for (;;) {
            if (syntax.keyword("trailer")) {
                std::optional<Object> trailer = syntax.object();

                if (trailer && std::holds_alternative<Dict>(trailer->value)) {
                    made.trailer = std::get<Dict>(std::move(trailer->value));

                    co_return made;
                }

                broken = true;

                break;
            }

            const std::optional<std::int64_t> start = syntax.integer();
            const std::optional<std::int64_t> count = start ? syntax.integer() : std::nullopt;

            if (!count || *start < 0 || *count < 0 || static_cast<std::uint64_t>(*start) + static_cast<std::uint64_t>(*count) > most_numbers) {
                broken = true;

                break;
            }

            for (std::int64_t one = 0; one < *count && !broken; ++one) {
                ++looks_;

                const std::optional<std::int64_t> offset = syntax.integer();
                const std::optional<std::int64_t> generation = offset ? syntax.integer() : std::nullopt;

                if (!generation) {
                    broken = true;

                    break;
                }

                const auto number = static_cast<std::uint32_t>(*start + one);

                if (syntax.keyword("n")) {
                    if (*offset > 0) made.entries.emplace_back(number, Place{Place::Kind::direct, static_cast<std::uint64_t>(*offset), 0});
                } else if (syntax.keyword("f")) {
                    made.entries.emplace_back(number, Place{});
                } else {
                    broken = true;
                }
            }

            if (broken) break;
        }

        if (!syntax.cut() || whole) co_return std::nullopt;
    }
}

// A cross-reference stream (ISO 32000-2, 7.5.8): three fields per entry as wide as `/W` says, for
// the object numbers `/Index` gives (by default all of them from zero); type 0 is free, 1 at an
// offset, 2 inside an object stream. The stream's dictionary is the trailer. These streams are
// never encrypted. Data shorter than the entries promise ends them and marks the file damaged. The
// ranges of `/Index` may come in any order, but an object number has at most one entry in a
// section, as the standard says: a range that overlaps one before it ends the entries and marks the
// file damaged, so that one section holds at most `most_numbers` entries however its data repeats.
Task<std::optional<Section>> File::streamed(std::uint64_t at) {
    const std::optional<Object> made = co_await indirect(at, std::nullopt);

    if (!made) co_return std::nullopt;

    const Stream* stream = pdf::stream_of(*made);

    if (stream == nullptr || !pdf::named(stream->dict.find("Type"), "XRef")) co_return std::nullopt;

    const Object* widths = stream->dict.find("W");
    const Array* list = widths == nullptr ? nullptr : pdf::array_of(*widths);

    if (list == nullptr || list->size() != 3) co_return std::nullopt;

    std::array<std::size_t, 3> wide{};

    for (std::size_t one = 0; one < 3; ++one) {
        const std::optional<std::int64_t> width = pdf::integer_of((*list)[one]);

        if (!width || *width < 0 || *width > 8) co_return std::nullopt;

        wide[one] = static_cast<std::size_t>(*width);
    }

    const std::size_t entry = wide[0] + wide[1] + wide[2];

    if (entry == 0) co_return std::nullopt;

    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;

    if (const Object* index = stream->dict.find("Index")) {
        const Array* pairs = pdf::array_of(*index);

        if (pairs == nullptr) co_return std::nullopt;

        for (std::size_t one = 0; one + 1 < pairs->size(); one += 2) {
            ++looks_;

            const std::optional<std::int64_t> start = pdf::integer_of((*pairs)[one]);
            const std::optional<std::int64_t> count = pdf::integer_of((*pairs)[one + 1]);

            if (!start || !count) co_return std::nullopt;

            ranges.emplace_back(*start, *count);
        }
    } else {
        const Object* size = stream->dict.find("Size");
        const std::optional<std::int64_t> count = size == nullptr ? std::nullopt : pdf::integer_of(*size);

        if (!count) co_return std::nullopt;

        ranges.emplace_back(0, *count);
    }

    const std::optional<Bytes> data = co_await decoded(*stream);

    if (!data) co_return std::nullopt;

    Section parsed;
    std::map<std::uint64_t, std::uint64_t> given;
    std::size_t used = 0;

    const auto field = [&data, &used](std::size_t width) {
        std::uint64_t value = 0;

        for (std::size_t one = 0; one < width; ++one) value = (value << 8) | std::to_integer<std::uint64_t>((*data)[used++]);

        return value;
    };

    for (const auto& [start, count] : ranges) {
        if (start < 0 || count < 0 || static_cast<std::uint64_t>(start) + static_cast<std::uint64_t>(count) > most_numbers) {
            out_.damaged();

            break;
        }

        if (count == 0) continue;

        const auto from = static_cast<std::uint64_t>(start);
        const std::uint64_t to = from + static_cast<std::uint64_t>(count);
        const auto after = given.lower_bound(from);

        if ((after != given.end() && after->first < to) || (after != given.begin() && std::prev(after)->second > from)) {
            out_.damaged();

            break;
        }

        given.emplace(from, to);

        bool short_of = false;

        for (std::int64_t one = 0; one < count; ++one) {
            ++looks_;

            if (used + entry > data->size()) {
                short_of = true;

                break;
            }

            const std::uint64_t type = wide[0] == 0 ? 1 : field(wide[0]);
            const std::uint64_t second = field(wide[1]);
            const std::uint64_t third = field(wide[2]);
            const auto number = static_cast<std::uint32_t>(start + one);

            if (type == 0) parsed.entries.emplace_back(number, Place{});
            else if (type == 1 && second > 0) parsed.entries.emplace_back(number, Place{Place::Kind::direct, second, 0});
            else if (type == 2 && second < most_numbers && third <= 0xFFFFFFFF)
                parsed.entries.emplace_back(number, Place{Place::Kind::packed, second, static_cast<std::uint32_t>(third)});
        }

        if (short_of) {
            out_.damaged();

            break;
        }
    }

    parsed.trailer = stream->dict;

    co_return parsed;
}

// Takes the entries of a section for every object not yet placed: sections come newest first, so
// what is already there is newer.
void File::placed(const std::vector<std::pair<std::uint32_t, Place>>& entries) {
    for (const auto& [number, place] : entries) places_.try_emplace(number, place);
}

// Takes the keys of an older trailer that the newer ones did not have, of `trailer_keys` only: a
// trailer of a quarter of a million keys, merged key by key into one that keeps them all, would
// make tens of billions of comparisons. Each key is a step, and so is each comparison with a key
// kept.
void File::merged(const Dict& older) {
    for (const Entry& one : older.entries) {
        ++looks_;

        if (std::ranges::find(trailer_keys, one.key) == trailer_keys.end()) continue;

        looks_ += trailer_.entries.size();

        if (trailer_.find(one.key) == nullptr) trailer_.entries.push_back(one);
    }
}

// Scans the whole file for objects and trailers, the way readers repair a file whose
// cross-reference is missing or lies: every "N G obj" places object N there (a later one wins,
// because an update is appended), and every "trailer" and every cross-reference stream gives a
// dictionary; those are taken in the order they stand in the file, the newest first, so that an
// update wins over what it updates whether its writer made a table or a stream. Object streams
// found on the way are indexed for the objects not found directly. Without a catalog in any
// trailer, the last object that is one becomes the root: among those whose dictionary starts with
// "/Catalog" in sight, then among the packed ones, which are in memory already.
// Runs at most once per file, only before the walk begins, and marks it damaged. Object streams
// that could not be read before are tried again, since their places were the ones that lied. Of
// the objects that look like cross-reference streams the newest `most_sections` are read, as many
// as the trailers kept, and at most `most_packs` that look like object streams are indexed.
Task<void> File::rebuild() {
    if (rebuilt_) co_return;

    rebuilt_ = true;
    out_.damaged();

    if (size_ > most_scanned) co_return;

    places_.clear();

    std::erase_if(packed_, [](const auto& one) { return one.second == nullptr; });

    Scan scan;

    for (std::uint64_t at = 0; at < size_; at += scan_chunk - scan_overlap) {
        const Bytes text = co_await fetched(at, scan_chunk);
        const bool last = text.size() < scan_chunk;

        scanned(text, at, last ? text.size() : scan_chunk - scan_overlap / 2, scan);

        if (last) break;
    }

    std::vector<std::pair<std::uint64_t, std::optional<std::uint32_t>>> heads;

    for (const std::uint64_t at : scan.trailers) heads.emplace_back(at, std::nullopt);

    for (const std::uint32_t number : scan.streams) heads.emplace_back(places_.at(number).at, number);

    std::sort(heads.begin(), heads.end(), [](const auto& one, const auto& other) { return one.first > other.first; });

    Dict before = std::move(trailer_);
    std::size_t looked = 0;

    trailer_ = {};

    for (const auto& head : heads) {
        if (!head.second) {
            const Bytes text = co_await fetched(head.first, large_window);

            Syntax syntax{spanned(text)};

            std::optional<Object> one = syntax.object();

            if (one && std::holds_alternative<Dict>(one->value)) merged(std::get<Dict>(one->value));

            continue;
        }

        if (looked++ >= most_sections) continue;

        const std::optional<Object> one = co_await indirect(head.first, *head.second);
        const Stream* stream = one ? pdf::stream_of(*one) : nullptr;

        if (stream != nullptr && pdf::named(stream->dict.find("Type"), "XRef")) merged(stream->dict);
    }

    merged(before);

    std::size_t indexed = 0;

    for (const std::uint32_t number : scan.packs) {
        if (indexed++ >= most_packs) break;

        const Packed* pack = co_await unpacked(number);

        if (pack == nullptr) continue;

        for (std::size_t one = 0; one < pack->table.size(); ++one) {
            const std::uint32_t inside = pack->table[one].number;

            places_.try_emplace(inside, Place{Place::Kind::packed, number, static_cast<std::uint32_t>(one)});
        }
    }

    const Object* root = trailer_.find("Root");

    if (root != nullptr) {
        const Object* target = co_await resolved(root);

        if (pdf::dict_of(*target) != nullptr) co_return;
    }

    std::optional<std::uint32_t> catalog;

    for (auto at = scan.catalogs.rbegin(); !catalog && at != scan.catalogs.rend(); ++at) {
        const bool one = co_await catalogued(*at);

        if (one) catalog = *at;
    }

    for (auto at = places_.rbegin(); !catalog && at != places_.rend() && spent_ < most_resolved; ++at) {
        if (at->second.kind != Place::Kind::packed) continue;

        const bool one = co_await catalogued(at->first);

        if (one) catalog = at->first;
    }

    if (!catalog) co_return;

    std::erase_if(trailer_.entries, [](const Entry& one) { return one.key == "Root"; });

    trailer_.entries.push_back(Entry{"Root", Object{Ref{*catalog, 0}}});
}

// Whether the object of the number is a catalog: a dictionary of the type "Catalog".
Task<bool> File::catalogued(std::uint32_t number) {
    const Object* one = co_await numbered(number);
    const Dict* dict = pdf::dict_of(*one);

    co_return dict != nullptr && pdf::named(dict->find("Type"), "Catalog");
}

// Looks through one chunk of the scan, which starts at the offset `at` of the file, for the words
// that start in it before `to`: "N G obj" places object N, with a look at the first `scan_head`
// bytes of its dictionary for a cross-reference stream, an object stream or the catalog;
// "trailer" notes where a trailer dictionary starts, and only the newest `most_sections` are kept.
// Words in the first half of the overlap of a chunk after the first belong to the chunk before.
// The number and generation before "obj" are looked for at most half the overlap back, wherever
// the word lies, so that every word is judged on bytes its own chunk holds.
void File::scanned(const Bytes& text, std::uint64_t at, std::size_t to, Scan& into) {
    const std::size_t from = at == 0 ? 0 : scan_overlap / 2;

    for (std::optional<std::size_t> word = found(text, "trailer", from); word && *word < to; word = found(text, "trailer", *word + 1)) {
        ++looks_;

        if (regular(byte_at(text, *word + 7)) || (*word > 0 && regular(byte_at(text, *word - 1)))) continue;

        into.trailers.insert(at + *word + 7);

        if (into.trailers.size() > most_sections) into.trailers.erase(into.trailers.begin());
    }

    for (std::optional<std::size_t> word = found(text, "obj", from); word && *word < to; word = found(text, "obj", *word + 1)) {
        ++looks_;

        if (regular(byte_at(text, *word + 3))) continue;

        const std::size_t floor = *word - std::min(*word, scan_overlap / 2);

        std::size_t back = *word;

        const auto spaces = [&text, &back, floor] {
            std::size_t many = 0;

            while (back > floor && pdf::spacing(byte_at(text, back - 1))) {
                --back;
                ++many;
            }

            return many;
        };

        const auto digits = [&text, &back, floor] {
            std::size_t many = 0;

            while (back > floor && digit(byte_at(text, back - 1)) && many < 10) {
                --back;
                ++many;
            }

            return many;
        };

        if (spaces() == 0 || digits() == 0 || spaces() == 0) continue;

        const std::size_t number_end = back;

        if (digits() == 0 || (back == floor ? at + floor > 0 : regular(byte_at(text, back - 1)))) continue;

        std::uint64_t number = 0;

        for (std::size_t one = back; one < number_end; ++one) number = number * 10 + static_cast<std::uint64_t>(byte_at(text, one) - '0');

        if (number >= most_numbers) continue;

        places_[static_cast<std::uint32_t>(number)] = Place{Place::Kind::direct, at + back, 0};

        const Bytes head(text.begin() + static_cast<std::ptrdiff_t>(*word), text.begin() + static_cast<std::ptrdiff_t>(std::min(text.size(), *word + scan_head)));

        if (found(head, "/XRef")) into.streams.insert(static_cast<std::uint32_t>(number));

        if (found(head, "/ObjStm")) into.packs.insert(static_cast<std::uint32_t>(number));

        if (found(head, "/Catalog")) into.catalogs.insert(static_cast<std::uint32_t>(number));
    }
}

// The object at the offset: "N G obj", the object, and for a dictionary followed by "stream" the
// stream, whose data starts after the end of that line. The first read is small; while the object
// goes on past it, a larger one follows, up to `most_block`, and so it does when the read ends
// right after the CR of "stream", since an LF may follow. With a number given, an object of another
// number is no answer: the offset lies.
Task<std::optional<Object>> File::indirect(std::uint64_t at, std::optional<std::uint32_t> number) {
    if (at >= size_) co_return std::nullopt;

    for (std::size_t reach = first_window;; reach *= 4) {
        const auto much = static_cast<std::size_t>(std::min<std::uint64_t>(reach, size_ - at));
        const Bytes text = co_await fetched(at, much);
        const bool whole = text.size() < much || at + text.size() >= size_ || reach >= most_block;

        Syntax syntax{spanned(text)};

        const std::optional<std::int64_t> said = syntax.integer();
        const std::optional<std::int64_t> generation = said ? syntax.integer() : std::nullopt;

        if (generation && syntax.keyword("obj")) {
            if (number && *said != static_cast<std::int64_t>(*number)) co_return std::nullopt;

            std::optional<Object> made = syntax.object();

            const bool streaming = made && std::holds_alternative<Dict>(made->value) && syntax.keyword("stream");

            if (streaming) (void)syntax.eol();

            if (!syntax.cut() || whole) {
                if (!made) co_return std::nullopt;

                if (!streaming) co_return made;

                co_return Object{Stream{std::get<Dict>(std::move(made->value)), at + syntax.at()}};
            }
        } else if (!syntax.cut() || whole) {
            co_return std::nullopt;
        }
    }
}

// The object of the number at the offset a cross-reference gives, tried as it is and then through
// the junk before the header (`shifted`), since writers that prepend junk count from the header.
Task<std::optional<Object>> File::located(std::uint64_t at, std::uint32_t number) {
    std::optional<Object> made = co_await indirect(at, number);
    const std::optional<std::uint64_t> moved = shifted(at);

    if (!made && moved) made = co_await indirect(*moved, number);

    co_return made;
}

// The object of the number from where the cross-reference places it, or none: `missing` when the
// number is free or unknown, which makes it the null object, as the standard says; not missing
// when the place was there but held no such object. In an encrypted file a packed object is
// missing, since its object stream is encrypted. A packed object is the one at the index its place
// gives, or else the first of its number in the stream, and is read from its slot only.
Task<Loaded> File::loaded(std::uint32_t number) {
    const auto place = places_.find(number);

    if (place == places_.end() || place->second.kind == Place::Kind::free) co_return Loaded{std::nullopt, true};

    const Place where = place->second;

    if (where.kind == Place::Kind::direct) {
        std::optional<Object> made = co_await located(where.at, number);

        co_return Loaded{std::move(made), false};
    }

    if (sealed()) co_return Loaded{std::nullopt, true};

    const Packed* pack = nullptr;

    if (where.at < most_numbers) pack = co_await unpacked(static_cast<std::uint32_t>(where.at));

    if (pack == nullptr) co_return Loaded{};

    const Slot* slot = nullptr;

    if (where.index < pack->table.size() && pack->table[where.index].number == number) {
        slot = &pack->table[where.index];
    } else {
        const auto at = std::ranges::lower_bound(pack->numbers, number, {}, [pack](std::uint32_t one) { return pack->table[one].number; });

        if (at != pack->numbers.end() && pack->table[*at].number == number) slot = &pack->table[*at];
    }

    if (slot == nullptr) co_return Loaded{};

    Syntax syntax{std::span<const std::byte>{pack->data}.first(pack->first + slot->end), pack->first + slot->at};

    co_return Loaded{syntax.object(), false};
}

// The object of the number, read once and kept; what is kept is never removed or moved (the nodes
// of a map), so a pointer to it stays good for the whole reading. A number being read already (a
// reference to itself along the way), one that is not there and a file over its budget of objects
// give the null object, which is not kept: after a repair the number may be found. A place that
// holds no such object marks the file damaged; while the structure is still being read
// (`settled_` is not yet set) it sends the file to `rebuild` once, and the object is looked for
// again. Later the place is not repaired: the trailer that the walk holds parts of must not change.
Task<const Object*> File::numbered(std::uint32_t number) {
    if (const auto kept = cache_.find(number); kept != cache_.end()) co_return &kept->second;

    if (resolving_.contains(number) || spent_ >= most_resolved) co_return &none_;

    ++spent_;
    ++looks_;
    resolving_.insert(number);

    Loaded made = co_await loaded(number);

    if (!made.object && !made.missing) {
        out_.damaged();

        if (!rebuilt_ && !settled_) {
            co_await rebuild();

            made = co_await loaded(number);
        }
    }

    resolving_.erase(number);

    if (!made.object) co_return &none_;

    co_return &cache_.emplace(number, std::move(*made.object)).first->second;
}

// The object a reference points to, and any other object as it is; a missing one (null) is the
// null object. A chain of references is followed a few steps; longer chains do not occur in real
// files.
Task<const Object*> File::resolved(const Object* one) {
    if (one == nullptr) co_return &none_;

    const Object* at = one;

    for (int step = 0; step < 8; ++step) {
        const std::optional<Ref> ref = pdf::ref_of(*at);

        if (!ref) co_return at;

        at = co_await numbered(ref->number);
    }

    co_return &none_;
}

// The decoded object stream of the number (ISO 32000-2, 7.5.7), read once and kept: the pairs of
// object number and offset in its first `/First` bytes, then the objects. The objects are stored
// in the order of their offsets, as the standard says, so each one's slot ends where the next
// begins, whatever order the table lists them in. An offset that another object has already, or
// one at or past the end of the objects, gives no object, so that no text is read as two objects,
// and the table is damage even when that object is never wanted. An object stream lives at an
// offset (`located`), never in another object stream. In an encrypted file its data is encrypted,
// so there is none. Every stream decoded stays in `packs_` until the file is read, so a pointer
// handed out stays good even when a repair reads the same number again.
Task<const Packed*> File::unpacked(std::uint32_t number) {
    if (sealed()) co_return nullptr;

    if (const auto kept = packed_.find(number); kept != packed_.end()) co_return kept->second;

    packed_.emplace(number, nullptr);

    const auto place = places_.find(number);

    if (place == places_.end() || place->second.kind != Place::Kind::direct) co_return nullptr;

    const std::uint64_t at = place->second.at;
    const std::optional<Object> made = co_await located(at, number);
    const Stream* stream = made ? pdf::stream_of(*made) : nullptr;

    if (stream == nullptr || !pdf::named(stream->dict.find("Type"), "ObjStm")) co_return nullptr;

    const Object* counted = co_await resolved(stream->dict.find("N"));
    const Object* begun = co_await resolved(stream->dict.find("First"));
    const std::optional<std::int64_t> many = pdf::integer_of(*counted);
    const std::optional<std::int64_t> first = pdf::integer_of(*begun);

    if (!many || !first || *many < 0 || *many > static_cast<std::int64_t>(pdf::most_items) || *first < 0) co_return nullptr;

    std::optional<Bytes> data = co_await decoded(*stream);

    if (!data || static_cast<std::uint64_t>(*first) > data->size()) co_return nullptr;

    Packed pack;

    pack.first = static_cast<std::size_t>(*first);

    const auto objects = static_cast<std::uint32_t>(data->size() - pack.first);

    Syntax syntax{std::span<const std::byte>{data->data(), pack.first}};

    for (std::int64_t one = 0; one < *many; ++one) {
        const std::optional<std::int64_t> inside = syntax.integer();
        const std::optional<std::int64_t> offset = inside ? syntax.integer() : std::nullopt;

        if (!offset || *inside < 0 || static_cast<std::uint64_t>(*inside) >= most_numbers || *offset < 0) {
            out_.damaged();

            break;
        }

        if (*offset >= objects) out_.damaged();

        const auto begins = static_cast<std::uint32_t>(std::min<std::int64_t>(*offset, objects));

        pack.table.push_back(Slot{static_cast<std::uint32_t>(*inside), begins, begins});
    }

    std::vector<std::uint32_t> order(pack.table.size());

    for (std::uint32_t one = 0; one < order.size(); ++one) order[one] = one;

    std::ranges::stable_sort(order, {}, [&pack](std::uint32_t one) { return pack.table[one].at; });

    std::uint32_t next = objects;

    for (std::size_t one = order.size(); one-- > 0;) {
        Slot& slot = pack.table[order[one]];

        if (one > 0 && pack.table[order[one - 1]].at == slot.at) {
            out_.damaged();

            continue;
        }

        slot.end = next;
        next = slot.at;
    }

    for (std::uint32_t one = 0; one < order.size(); ++one) order[one] = one;

    std::ranges::stable_sort(order, {}, [&pack](std::uint32_t one) { return pack.table[one].number; });

    pack.numbers = std::move(order);

    pack.data = std::move(*data);

    const Packed* kept = &packs_.emplace_back(std::move(pack));

    packed_[number] = kept;

    co_return kept;
}

// The raw data of a stream: `/Length` bytes when "endstream" follows them, as it should; otherwise
// (a length missing, wrong, or too large) up to the "endstream" that ends it.
Task<std::optional<Bytes>> File::data_of(const Stream& one) {
    if (one.at > size_) co_return std::nullopt;

    const Object* length = co_await resolved(one.dict.find("Length"));
    const std::optional<std::int64_t> said = pdf::integer_of(*length);

    if (said && *said >= 0 && static_cast<std::uint64_t>(*said) <= size_ - one.at && static_cast<std::uint64_t>(*said) <= most_packet) {
        const auto much = static_cast<std::size_t>(*said);

        Bytes made = co_await fetched(one.at, much);

        const Bytes after = co_await fetched(one.at + much, 32);

        Syntax syntax{spanned(after)};

        if (made.size() == much && syntax.keyword("endstream")) co_return made;
    }

    co_return co_await ended(one.at);
}

// The data from the offset to the next "endstream", without the end of line before it, at most
// `most_packet` bytes; a stream found this way had a wrong length, so the file is damaged. Every
// search reads through `fetched`, so that searches for streams without an end cannot add up to
// more than the budget of the file.
Task<std::optional<Bytes>> File::ended(std::uint64_t at) {
    constexpr std::string_view mark = "endstream";

    Bytes made;

    while (made.size() < most_packet) {
        const Bytes chunk = co_await fetched(at + made.size(), large_window);

        if (chunk.empty()) co_return std::nullopt;

        const std::size_t before = made.size();

        made.insert(made.end(), chunk.begin(), chunk.end());

        const std::optional<std::size_t> end = found(made, mark, before >= mark.size() ? before - mark.size() : 0);

        if (end) {
            made.resize(*end);

            if (!made.empty() && made.back() == std::byte{'\n'}) made.pop_back();
            if (!made.empty() && made.back() == std::byte{'\r'}) made.pop_back();

            out_.damaged();

            co_return made;
        }
    }

    co_return std::nullopt;
}

// The decoded data of a stream through its filters. Only FlateDecode is needed for the streams we
// read (cross-reference and object streams), so any other filter means there is no data for us.
Task<std::optional<Bytes>> File::decoded(const Stream& one) {
    std::optional<Bytes> data = co_await data_of(one);

    if (!data) co_return std::nullopt;

    const Object* filter = co_await resolved(one.dict.find("Filter"));
    const Object* parms = co_await resolved(one.dict.find("DecodeParms"));
    const Name* single = pdf::name_of(*filter);
    const Array* chain = pdf::array_of(*filter);

    std::vector<std::pair<std::string, const Dict*>> filters;

    if (single != nullptr) {
        filters.emplace_back(single->text, pdf::dict_of(*parms));
    } else if (chain != nullptr) {
        const Array* each = pdf::array_of(*parms);

        for (std::size_t at = 0; at < chain->size(); ++at) {
            const Object* link = co_await resolved(&(*chain)[at]);
            const Name* listed = pdf::name_of(*link);

            if (listed == nullptr) co_return std::nullopt;

            const Dict* given = nullptr;

            if (each != nullptr && at < each->size()) {
                const Object* parm = co_await resolved(&(*each)[at]);

                given = pdf::dict_of(*parm);
            }

            filters.emplace_back(listed->text, given);
        }
    }

    if (filters.size() > most_filters) co_return std::nullopt;

    for (const auto& [name, given] : filters) {
        if (name != "FlateDecode" && name != "Fl") co_return std::nullopt;

        data = inflated(*data);

        if (!data) co_return std::nullopt;

        data = predicted(std::move(*data), given);

        if (!data) co_return std::nullopt;
    }

    co_return data;
}

// The data unpacked from the zlib format by our `Inflater`, in pieces of `piece` bytes until the
// stream ends; past `most_packet` there is no data at all. A damaged stream, or one cut short,
// gives what came out before the damage or the cut and marks the file; when nothing came out, no
// data. Bytes after a whole stream are left unread (`Rest::left`): some writers count an end of
// line into the length. What every stream unpacks to is counted against the file's budget of
// unpacking (`unpackable`): a stream that would go past what is left of it gives no data, which
// makes the file damaged where it is wanted.
std::optional<Bytes> File::inflated(const Bytes& packed) {
    inflating::Inflater inflater{inflating::Wrapping::zlib, inflating::Rest::left};
    Bytes made;
    bool whole = true;

    const std::uint64_t left = out_.unpackable();
    const std::uint64_t most = std::min<std::uint64_t>(most_packet, left);

    try {
        for (std::size_t at = 0; at < packed.size() && !inflater.ended() && made.size() <= most; at += piece)
            inflater.fed(std::span<const std::byte>{packed}.subspan(at, std::min(piece, packed.size() - at)), made);
    } catch (const Trouble&) {
        whole = false;
    }

    out_.unpacked(made.size());

    if (made.size() > most_packet || made.size() > left) return std::nullopt;

    if (!whole || !inflater.ended()) {
        out_.damaged();

        if (made.empty()) return std::nullopt;
    }

    return made;
}

// Walks the page tree from its root, depth first in document order, carrying down what pages
// inherit. A node met twice is a loop and marks the file damaged, and so does an array of kids met
// twice (nodes sharing it): its kids are not taken again, and a kid already met is not taken at
// all, so that the walk costs no more than the arrays it reads. A node with neither kids nor a
// type is taken as a page, as readers do.
Task<void> File::paged(const Object& root) {
    struct Step {
        const Object* node;
        Inherited from;
        std::size_t depth;
    };

    std::vector<Step> steps{Step{&root, Inherited{}, 0}};
    std::set<std::uint32_t> visited;
    std::set<const Array*> walked;

    while (!steps.empty() && document_.pages < most_pages) {
        ++looks_;

        const Step step = steps.back();

        steps.pop_back();

        const Object* node = step.node;
        const std::optional<Ref> ref = pdf::ref_of(*node);

        if (ref) {
            if (!visited.insert(ref->number).second) {
                out_.damaged();

                continue;
            }

            node = co_await numbered(ref->number);
        }

        const Dict* dict = pdf::dict_of(*node);

        if (dict == nullptr) continue;

        Inherited here = step.from;

        if (const Object* resources = dict->find("Resources")) here.resources = resources;

        const Object* media = co_await resolved(dict->find("MediaBox"));

        if (const std::optional<Box> box = box_of(pdf::array_of(*media))) here.media = *box;

        const Object* crop = co_await resolved(dict->find("CropBox"));

        if (const std::optional<Box> box = box_of(pdf::array_of(*crop))) here.crop = box;

        const Object* rotate = co_await resolved(dict->find("Rotate"));

        if (const std::optional<std::int64_t> turn = pdf::integer_of(*rotate)) here.rotate = *turn;

        const Object* kids = dict->find("Kids");
        const Object* type = dict->find("Type");

        if (pdf::named(type, "Page") || (kids == nullptr && !pdf::named(type, "Pages"))) {
            co_await page(*dict, here);

            continue;
        }

        const Object* listed = co_await resolved(kids);
        const Array* list = pdf::array_of(*listed);

        if (list == nullptr) continue;

        if (step.depth >= tree_depth || !walked.insert(list).second) {
            out_.damaged();

            continue;
        }

        for (auto kid = list->rbegin(); kid != list->rend(); ++kid) {
            ++looks_;

            const std::optional<Ref> child = pdf::ref_of(*kid);

            if (child && visited.contains(child->number)) out_.damaged();
            else steps.push_back(Step{&*kid, here, step.depth + 1});
        }
    }
}

// One page: its size as shown (the crop box within the media box, times the user unit, turned by
// the rotation), whether it carries text, and its links. A size that overflows is no size; the page
// still counts. An array of annotations that pages share (a broken file) is counted once.
Task<void> File::page(const Dict& leaf, const Inherited& from) {
    Box shown = from.media;

    if (from.crop) {
        const Box cut{std::max(shown[0], (*from.crop)[0]), std::max(shown[1], (*from.crop)[1]), std::min(shown[2], (*from.crop)[2]),
                      std::min(shown[3], (*from.crop)[3])};

        if (cut[2] > cut[0] && cut[3] > cut[1]) shown = cut;
    }

    double unit = 1;

    const Object* user = co_await resolved(leaf.find("UserUnit"));

    if (const std::optional<double> said = pdf::number_of(*user); said && std::isfinite(*said) && *said > 0) unit = *said;

    double width = (shown[2] - shown[0]) * unit;
    double height = (shown[3] - shown[1]) * unit;

    const std::int64_t turned = ((from.rotate % 360) + 360) % 360;

    if (turned == 90 || turned == 270) std::swap(width, height);

    if (std::isfinite(width) && std::isfinite(height)) sheet(width, height);

    ++document_.pages;

    const bool lettering = co_await lettered(from.resources, 0);

    if (lettering) ++document_.with_text;

    const Object* listed = co_await resolved(leaf.find("Annots"));
    const Array* annotations = pdf::array_of(*listed);

    if (annotations == nullptr || !annotated_.insert(annotations).second) co_return;

    for (std::size_t at = 0; at < std::min(annotations->size(), most_annotations); ++at) {
        ++looks_;

        const Object* one = co_await resolved(&(*annotations)[at]);
        const Dict* annotation = pdf::dict_of(*one);

        if (annotation != nullptr && pdf::named(annotation->find("Subtype"), "Link")) ++document_.links;
    }
}

// Counts a page of the size into its sheet: sizes within a point of each other are one size,
// because writers round differently (595.276 and 595.28 are both A4).
void File::sheet(double width, double height) {
    for (Sheet& one : document_.sheets) {
        if (std::abs(one.width - width) < 1 && std::abs(one.height - height) < 1) {
            ++one.pages;

            return;
        }
    }

    if (document_.sheets.size() < most_sheets) document_.sheets.push_back(Sheet{width, height, 1});
}

// Whether resources name a font, their own or those of the forms they draw, followed `form_depth`
// deep; the fonts found are counted. A page that names a font has text: its content is never
// interpreted, so a font that is named and not used counts too, and a scan with a text layer laid
// over it (OCR) has text, which is the truth. Resources that many pages share, by reference or
// inherited, cost a few lookups each time: the dictionary of fonts is gone through once and kept
// in `named_` by its address, which stays good while the file is read (`numbered`), and so is the
// dictionary of XObjects (`drew`) and every form (`formed`).
Task<bool> File::lettered(const Object* resources, std::size_t depth) {
    const Object* held = co_await resolved(resources);
    const Dict* dict = pdf::dict_of(*held);

    if (dict == nullptr) co_return false;

    const Object* typed = co_await resolved(dict->find("Font"));
    const Dict* fonts = pdf::dict_of(*typed);

    bool made = fonts != nullptr && !fonts->entries.empty();

    if (fonts != nullptr && named_.insert(fonts).second)
        for (const Entry& one : fonts->entries) {
            ++looks_;

            co_await font(one.value);
        }

    if (depth < form_depth) {
        const Object* listed = co_await resolved(dict->find("XObject"));
        const bool lettering = co_await drew(pdf::dict_of(*listed), depth + 1);

        if (lettering) made = true;
    }

    co_return made;
}

// Whether a dictionary of XObjects draws a form that has text, its forms at the depth given. The
// answer is kept in `drawn_` by the dictionary's address, so that one which many resources share
// is looked at once.
Task<bool> File::drew(const Dict* objects, std::size_t depth) {
    if (objects == nullptr) co_return false;

    if (const auto kept = drawn_.find(objects); kept != drawn_.end()) co_return kept->second;

    drawn_.emplace(objects, false);

    bool made = false;

    for (const Entry& one : objects->entries) {
        ++looks_;

        const bool lettering = co_await formed(one.value, depth);

        if (lettering) made = true;
    }

    drawn_[objects] = made;

    co_return made;
}

// Whether a form XObject has text, by its resources; any other XObject (an image) has none. A
// form is a stream, so it is always a reference, and the answer is kept in `forms_` by its number:
// a form that draws itself, or forms drawing each other a thousand times over, cost one look each.
Task<bool> File::formed(const Object& value, std::size_t depth) {
    const std::optional<Ref> ref = pdf::ref_of(value);

    if (!ref) co_return false;

    if (const auto kept = forms_.find(ref->number); kept != forms_.end()) co_return kept->second;

    forms_.emplace(ref->number, false);

    const Object* drawn = co_await resolved(&value);
    const Stream* form = pdf::stream_of(*drawn);

    bool made = false;

    if (form != nullptr && pdf::named(form->dict.find("Subtype"), "Form")) made = co_await lettered(form->dict.find("Resources"), depth);

    forms_[ref->number] = made;

    co_return made;
}

// Counts a font once, and whether it is embedded. One by reference is known by its number; one
// written directly is met only from the dictionary of fonts that holds it, which `lettered` goes
// through once.
Task<void> File::font(const Object& value) {
    const std::optional<Ref> ref = pdf::ref_of(value);

    if (ref && !fonts_.insert(ref->number).second) co_return;

    ++document_.fonts;

    const Object* target = co_await resolved(&value);
    const bool inside = co_await embedded(pdf::dict_of(*target));

    if (inside) ++document_.embedded;
}

// Whether a font is embedded: a Type 3 font is drawn by the document itself; a composite font
// (Type 0) is embedded when its descendant is; any other when its descriptor holds a font file.
Task<bool> File::embedded(const Dict* font) {
    if (font == nullptr) co_return false;

    const Object* subtype = font->find("Subtype");

    if (pdf::named(subtype, "Type3")) co_return true;

    const Dict* real = font;

    if (pdf::named(subtype, "Type0")) {
        const Object* listed = co_await resolved(font->find("DescendantFonts"));
        const Array* descendants = pdf::array_of(*listed);

        real = nullptr;

        if (descendants != nullptr && !descendants->empty()) {
            const Object* first = co_await resolved(&descendants->front());

            real = pdf::dict_of(*first);
        }
    }

    if (real == nullptr) co_return false;

    const Object* described = co_await resolved(real->find("FontDescriptor"));
    const Dict* descriptor = pdf::dict_of(*described);

    co_return descriptor != nullptr &&
        (descriptor->find("FontFile") != nullptr || descriptor->find("FontFile2") != nullptr || descriptor->find("FontFile3") != nullptr);
}

// Counts the bookmarks (the outline items, ISO 32000-2, 12.3.3) by walking them, children and
// siblings, because `/Count` says how many are open, not how many there are. An item met twice is a
// loop and marks the file damaged.
Task<void> File::bookmarked(const Dict& catalog) {
    const Object* listed = co_await resolved(catalog.find("Outlines"));
    const Dict* outlines = pdf::dict_of(*listed);

    if (outlines == nullptr) co_return;

    std::vector<const Object*> items;
    std::set<std::uint32_t> seen;

    if (const Object* first = outlines->find("First")) items.push_back(first);

    while (!items.empty() && document_.bookmarks < most_pages) {
        ++looks_;

        const Object* item = items.back();

        items.pop_back();

        const std::optional<Ref> ref = pdf::ref_of(*item);

        if (ref && !seen.insert(ref->number).second) {
            out_.damaged();

            continue;
        }

        const Object* target = co_await resolved(item);
        const Dict* dict = pdf::dict_of(*target);

        if (dict == nullptr) continue;

        ++document_.bookmarks;

        if (const Object* next = dict->find("Next")) items.push_back(next);
        if (const Object* first = dict->find("First")) items.push_back(first);
    }
}

// The fields of the document information dictionary, in file order, as text: dates stay as PDF
// writes them ("D:20260302141137+01'00'"), because the meaning is the caller's.
Task<void> File::informed() {
    const Object* listed = co_await resolved(trailer_.find("Info"));
    const Dict* info = pdf::dict_of(*listed);

    if (info == nullptr) co_return;

    for (const Entry& one : info->entries) {
        ++looks_;

        const Object* value = co_await resolved(&one.value);

        if (const std::string* text = pdf::string_of(*value)) out_.field(Scheme::pdf, cleaned(one.key), pdf::text_of(*text));
        else if (const Name* name = pdf::name_of(*value)) out_.field(Scheme::pdf, cleaned(one.key), cleaned(name->text));
    }
}

// The whole reading: the header, the cross-reference (repaired when it fails), then for an
// encrypted file only that it is, otherwise the catalog, the pages, the bookmarks, the form and the
// information dictionary. A catalog that cannot be read sends the file to `rebuild`, unless it has
// been repaired already, and then the root is looked for again: also when the repair began while
// the root itself was being read, which found the catalog that the trailer now names.
Task<void> File::read() {
    out_.into.type = "application/pdf";

    co_await headed();

    const std::optional<std::uint64_t> start = co_await started();

    bool whole = false;

    if (start) whole = co_await chained(*start);

    if (!whole) co_await rebuild();

    document_.version = version_;

    if (sealed()) {
        document_.encrypted = true;
        out_.into.document = std::move(document_);

        co_return;
    }

    const Object* root = co_await resolved(trailer_.find("Root"));
    const Dict* catalog = pdf::dict_of(*root);

    if (catalog == nullptr) {
        co_await rebuild();

        const Object* repaired = co_await resolved(trailer_.find("Root"));

        catalog = pdf::dict_of(*repaired);
    }

    settled_ = true;

    if (catalog != nullptr) {
        const Object* versioned = co_await resolved(catalog->find("Version"));

        const Name* said = pdf::name_of(*versioned);

        if (said != nullptr && said->text.size() == 3 && digit(said->text[0]) && said->text[1] == '.' && digit(said->text[2]) && said->text > document_.version)
            document_.version = said->text;

        const Object* pages = catalog->find("Pages");

        if (pages != nullptr) co_await paged(*pages);

        if (document_.pages == 0) {
            const Object* tree = co_await resolved(catalog->find("Pages"));
            const Dict* top = pdf::dict_of(*tree);
            std::optional<std::int64_t> count;

            if (top != nullptr) {
                const Object* counted = co_await resolved(top->find("Count"));

                count = pdf::integer_of(*counted);
            }

            if (count && *count > 0) document_.pages = static_cast<std::uint32_t>(std::min<std::int64_t>(*count, most_pages));
        }

        co_await bookmarked(*catalog);

        const Object* acro = co_await resolved(catalog->find("AcroForm"));
        const Dict* form = pdf::dict_of(*acro);

        if (form != nullptr) {
            const Object* listed = co_await resolved(form->find("Fields"));
            const Array* fields = pdf::array_of(*listed);

            document_.form = (fields != nullptr && !fields->empty()) || form->find("XFA") != nullptr;
        }
    }

    co_await informed();

    out_.into.document = std::move(document_);
}

}

// The steps of work every PDF read so far took: an object read, an entry of a table, a dictionary
// or an array gone through, a word found in a scan, a node of a tree. It grows by each file's
// steps once the file is done, and tells the trials that a file built to multiply the work does
// not.
std::uint64_t pdf_looks() { return looks_made.load(std::memory_order_relaxed); }

// Whether the file of `size` bytes is PDF: "%PDF-" among its first bytes, where readers look for
// it.
Task<bool> pdf_at(Reader& from, std::uint64_t size) {
    const Bytes head = co_await from.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size, head_room)));

    co_return found(head, "%PDF-").has_value();
}

// Describes a PDF document of `size` bytes; a request for a picture gets nothing, because a
// document's pictures are not embedded metadata.
Task<void> pdf_document(Reader& from, std::uint64_t size, Gathered& out) {
    if (out.wanted) co_return;

    File file{from, size, out};

    co_await file.read();
}

}

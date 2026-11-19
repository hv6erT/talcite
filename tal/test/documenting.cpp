#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/meta.hpp"

#include "tal/testing.hpp"

#include "pdfing.hpp"
#include "tagging.hpp"

// Trials of our PDF reader, ported from talcite-adapters. The syntax of objects and the decoding of
// text strings need no file: the parser (`pdf::Syntax`) is handed bytes in memory, and a text
// string goes straight to `pdf::text_of`. The inputs are the forms writers really make, not only
// the ones the standard allows.
//
// The files: three samples were written by other people's tools and are kept here as bytes: a
// document of two pages from pdfTeX (PDF 1.5, object streams and a cross-reference stream with the
// PNG predictor, fonts named but not embedded, two bookmarks, a link), one of three pages from
// ReportLab (a classic table, three page sizes, a title in UTF-16) and the pdfTeX one encrypted by
// qpdf with AES-256. Their pages, sizes, fonts, bookmarks, links, encryption and fields were
// matched against pdfinfo, pdffonts and pypdf once, outside the trials.
//
// The structures a trial needs one at a time (an incremental update, a hybrid file, a lying
// cross-reference, junk before the header, loops, inherited attributes, every font kind, the
// encodings of text strings) are assembled here, object by object, with their cross-reference
// computed from where the objects landed (`Assembly`). The trials hand the files to
// `meta::described` as bytes held in memory (`meta::Held`), as any caller does, so they go
// through the recognition of the container and our own inflater.

namespace {

namespace pdf = tal::meta::tagging::pdf;

using tal::Bytes;

// The bytes of a text, as they are.
[[nodiscard]] Bytes bytes_of(std::string_view text) {
    return Bytes(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

}

// The syntax: numbers the standard does not allow but writers make, names with #, literal strings
// with every escape, hexadecimal strings with spaces and an odd digit, references among numbers.
TRIAL("PDF: the syntax of objects as writers really make it") {
    const Bytes text = bytes_of("[--3 +4 1.2.3 .5 -.25 4. - 99999999999999999999 /A#20B /C#2 (a\\(b\\)c\\\nd\\101\\7\\n) <61 6 > 1 0 R 2 true null]");

    pdf::Syntax syntax{std::span<const std::byte>{text.data(), text.size()}};

    const std::optional<pdf::Object> made = syntax.object();

    OUGHT(made.has_value());

    const pdf::Array* list = made ? pdf::array_of(*made) : nullptr;

    OUGHT(list != nullptr && list->size() == 16);

    SAME(pdf::integer_of((*list)[0]), std::optional<std::int64_t>{-3});
    SAME(pdf::integer_of((*list)[1]), std::optional<std::int64_t>{4});
    SAME(pdf::number_of((*list)[2]), std::optional<double>{1.2});
    SAME(pdf::number_of((*list)[3]), std::optional<double>{0.5});
    SAME(pdf::number_of((*list)[4]), std::optional<double>{-0.25});
    SAME(pdf::number_of((*list)[5]), std::optional<double>{4});
    SAME(pdf::integer_of((*list)[6]), std::optional<std::int64_t>{0});
    OUGHT(std::holds_alternative<double>((*list)[7].value));
    SAME(pdf::name_of((*list)[8])->text, std::string{"A B"});
    SAME(pdf::name_of((*list)[9])->text, std::string{"C#2"});
    SAME(*pdf::string_of((*list)[10]), std::string{"a(b)cdA\x07\n"});
    SAME(*pdf::string_of((*list)[11]), std::string{"a`"});
    SAME(pdf::ref_of((*list)[12]), (std::optional<pdf::Ref>{pdf::Ref{1, 0}}));
    SAME(pdf::integer_of((*list)[13]), std::optional<std::int64_t>{2});
    OUGHT(std::holds_alternative<bool>((*list)[14].value));
    OUGHT(std::holds_alternative<pdf::Null>((*list)[15].value));
}

// A window of the file that ends inside an object says so (`cut`), so that the caller reads a
// larger one: after an integer that may begin a reference, and between the two ">" that end a
// dictionary. The smallest 64-bit integer is an integer; a real too large for a double is
// infinite, which every size check refuses, and one too small is zero.
TRIAL("PDF: an object cut by the end of its window, and numbers at the edges of their types") {
    const auto parsed = [](std::string_view text) {
        const Bytes held = bytes_of(text);
        pdf::Syntax syntax{std::span<const std::byte>{held.data(), held.size()}};
        std::optional<pdf::Object> made = syntax.object();

        return std::pair{std::move(made), syntax.cut()};
    };

    const auto [five, five_cut] = parsed("5 ");

    SAME(pdf::integer_of(*five), std::optional<std::int64_t>{5});
    OUGHT(five_cut);

    const auto [whole, whole_cut] = parsed("5 0 R ");

    OUGHT(pdf::ref_of(*whole) == (std::optional<pdf::Ref>{pdf::Ref{5, 0}}));
    OUGHT(!whole_cut);

    for (const std::string_view text : {"<</A 1>", "<</A>", "<</A 1", "<</A"}) {
        const auto [made, cut] = parsed(text);

        OUGHT(!made.has_value());
        OUGHT(cut);
    }

    const auto [broken, broken_cut] = parsed("<</A 1]>>");

    OUGHT(!broken.has_value());
    OUGHT(!broken_cut);

    SAME(pdf::integer_of(*parsed("-9223372036854775808 ").first), std::optional<std::int64_t>{std::numeric_limits<std::int64_t>::min()});

    const std::optional<double> huge = pdf::number_of(*parsed("1" + std::string(400, '0') + " ").first);
    const std::optional<double> tiny = pdf::number_of(*parsed("-0." + std::string(400, '0') + "1 ").first);

    OUGHT(huge && std::isinf(*huge) && *huge > 0);
    OUGHT(tiny && *tiny == 0);
}

// Text strings: PDFDocEncoding with its own characters, UTF-16 in both orders after their marks,
// with a language code taken out, UTF-8 after its mark (PDF 2.0), an undefined code, and a zero,
// which ends the text.
TRIAL("PDF: text strings in PDFDocEncoding, UTF-16 and UTF-8") {
    SAME(pdf::text_of("\x80\x84\x93\x95\x9B\xA0 \x18"), std::string{"•—ﬁŁł€ ˘"});
    SAME(pdf::text_of(std::string_view{"\xFE\xFF\x00\x41\x01\x42", 6}), std::string{"Ał"});
    SAME(pdf::text_of(std::string_view{"\xFF\xFE\x41\x00\x42\x01", 6}), std::string{"Ał"});
    SAME(pdf::text_of(std::string_view{"\xFE\xFF\x00\x1B\x70\x6C\x00\x1B\x00\x41", 10}), std::string{"A"});
    SAME(pdf::text_of("\xEF\xBB\xBFza\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87"), std::string{"zażółć"});
    SAME(pdf::text_of("a\x7F"), std::string{"a\xEF\xBF\xBD"});
    SAME(pdf::text_of(std::string_view{"a\0b", 3}), std::string{"a"});
}

namespace {

namespace meta = tal::meta;

// Bytes from a string of lower-case hexadecimal digits, the form in which the samples are kept.
[[nodiscard]] Bytes lump(std::string_view hex) {
    Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) -> int {
        if (one >= '0' && one <= '9') return one - '0';

        return one - 'a' + 10;
    };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(std::byte{static_cast<std::uint8_t>(value(hex[at]) * 16 + value(hex[at + 1]))});

    return made;
}

// Describes a file held in memory, on a loop of its own. The readers never refuse a file, so
// anything thrown here fails the trial.
[[nodiscard]] meta::Description described(const Bytes& given) {
    tal::Loop loop;
    meta::Held from{given};

    return tal::wait(loop, meta::described(from));
}

// Bytes held in memory, those read counted.
class Tallied final : public meta::Reader {
public:
    // Over `given`.
    explicit Tallied(Bytes given) : held_(std::move(given)) {}

    // The size of the bytes.
    tal::Task<std::uint64_t> size() override { return held_.size(); }

    // What the bytes have of `much` from `at`, its length counted.
    tal::Task<Bytes> read(std::uint64_t at, std::size_t much) override {
        Bytes made = co_await held_.read(at, much);

        read_ += made.size();
        co_return made;
    }

    // The bytes read so far.
    [[nodiscard]] std::uint64_t bytes_read() const { return read_; }

private:
    meta::Held held_;
    std::uint64_t read_ = 0;
};

// What describing a file cost: the steps of work of the PDF reader (`pdf_looks`), the bytes read,
// and whether it hung (two minutes). A file made to blow up the costs of a reader is held to
// bounds of work, which no busy machine stretches, not to a time.
struct Cost {
    meta::Description made;
    std::uint64_t looks = 0;
    std::uint64_t read = 0;
    bool hung = false;
};

// A file described, with what that cost.
[[nodiscard]] Cost costed(const Bytes& given) {
    tal::Loop loop;
    Tallied from{given};
    const std::uint64_t before = meta::tagging::pdf_looks();
    const auto began = std::chrono::steady_clock::now();
    meta::Description made = tal::wait(loop, meta::described(from));

    return {std::move(made), meta::tagging::pdf_looks() - before, from.bytes_read(), std::chrono::steady_clock::now() - began > std::chrono::minutes{2}};
}

// The least a `need` loads set to `least` bytes while it lives.
struct Narrowed {
    // Sets the window.
    explicit Narrowed(std::size_t least) { meta::tagging::windowed(least); }

    // Gives the window its size back.
    ~Narrowed() { meta::tagging::windowed(meta::tagging::window); }

    Narrowed(const Narrowed&) = delete;
    Narrowed& operator=(const Narrowed&) = delete;
};

// The document of a description, or an empty one when there is none (the trial then fails on it).
[[nodiscard]] meta::Document document_of(const meta::Description& one) {
    OUGHT(one.document.has_value());

    return one.document.value_or(meta::Document{});
}

// An expected field of the information dictionary.
[[nodiscard]] meta::Field info(std::string key, std::string value) { return {meta::Scheme::pdf, std::move(key), std::move(value)}; }

// The value of a field by its key, or nothing.
[[nodiscard]] std::optional<std::string> field_of(const meta::Description& one, std::string_view key) {
    for (const meta::Field& field : one.fields)
        if (field.key == key) return std::get<std::string>(field.value);

    return std::nullopt;
}

// The bytes in the zlib format, made of stored (uncompressed) DEFLATE blocks, with a correct
// Adler-32: what a writer that does not compress would make, and enough for the inflater.
[[nodiscard]] std::string zlibbed(std::string_view given) {
    std::string made = "\x78\x01";
    std::size_t at = 0;

    do {
        const std::size_t much = std::min<std::size_t>(65535, given.size() - at);
        const bool last = at + much == given.size();

        made.push_back(static_cast<char>(last ? 1 : 0));
        made.push_back(static_cast<char>(much & 0xFF));
        made.push_back(static_cast<char>(much >> 8));
        made.push_back(static_cast<char>(~much & 0xFF));
        made.push_back(static_cast<char>((~much >> 8) & 0xFF));
        made.append(given.substr(at, much));

        at += much;
    } while (at < given.size());

    std::uint32_t low = 1;
    std::uint32_t high = 0;

    for (const char one : given) {
        low = (low + static_cast<unsigned char>(one)) % 65521;
        high = (high + low) % 65521;
    }

    const std::uint32_t adler = (high << 16) | low;

    for (int shift = 24; shift >= 0; shift -= 8) made.push_back(static_cast<char>((adler >> shift) & 0xFF));

    return made;
}

// The bytes in the zlib format with a wrong Adler-32: the data whole, the check damaged.
[[nodiscard]] std::string wrongly_summed(std::string_view given) {
    std::string made = zlibbed(given);

    made.back() = static_cast<char>(made.back() ^ 0x55);

    return made;
}

// A zlib stream that unpacks to `given` and then to a space and `matches` times 258 spaces more,
// in the fixed code: a few bytes for each 258 of output, as a bomb is made. Its Adler-32 follows
// when `summed`, and the stream is whole; otherwise it is cut before it.
[[nodiscard]] std::string bomb(std::string_view given, std::size_t matches, bool summed = false) {
    std::string made = zlibbed(given);

    made.resize(made.size() - 4);
    made[2] = 0;

    std::uint32_t pending = 0;
    int count = 0;

    const auto put = [&made, &pending, &count](std::uint32_t value, int many) {
        for (int one = 0; one < many; ++one) {
            pending |= ((value >> one) & 1U) << count;

            if (++count == 8) {
                made.push_back(static_cast<char>(pending));
                pending = 0;
                count = 0;
            }
        }
    };
    const auto code = [&put](std::uint32_t value, int many) {
        for (int one = many - 1; one >= 0; --one) put((value >> one) & 1U, 1);
    };

    put(1, 1);
    put(1, 2);
    code(0x30 + ' ', 8);

    for (std::size_t one = 0; one < matches; ++one) {
        code(0xC5, 8);
        code(0, 5);
    }

    code(0, 7);

    if (count > 0) made.push_back(static_cast<char>(pending));

    if (!summed) return made;

    std::uint32_t low = 1;
    std::uint32_t high = 0;

    const auto added = [&low, &high](unsigned char one) {
        low = (low + one) % 65521;
        high = (high + low) % 65521;
    };

    for (const char one : given) added(static_cast<unsigned char>(one));

    for (std::size_t one = 0; one <= matches * 258; ++one) added(' ');

    for (int shift = 24; shift >= 0; shift -= 8) made.push_back(static_cast<char>((((high << 16) | low) >> shift) & 0xFF));

    return made;
}

// The rows of a cross-reference stream: the three fields of every entry, high byte first, each as
// wide as `widths` says.
[[nodiscard]] std::string fielded(const std::vector<std::array<std::uint64_t, 3>>& entries, const std::array<int, 3>& widths) {
    std::string made;

    for (const std::array<std::uint64_t, 3>& entry : entries)
        for (std::size_t field = 0; field < 3; ++field)
            for (int byte = widths[field] - 1; byte >= 0; --byte) made.push_back(static_cast<char>((entry[field] >> (8 * byte)) & 0xFF));

    return made;
}

// The text in rows of `columns` bytes, each behind the PNG filter of its turn in `filters` and
// its number (ISO 32000-2, 7.4.4.4, as PNG defines them): None, Sub, Up, Average and Paeth.
[[nodiscard]] std::string png(std::string_view text, std::size_t columns, const std::vector<int>& filters) {
    std::string made;
    std::vector<int> prior(columns, 0);

    for (std::size_t at = 0, row = 0; at < text.size(); at += columns, ++row) {
        const std::size_t much = std::min(columns, text.size() - at);
        const int filter = filters[row % filters.size()];
        std::vector<int> current(columns, 0);

        for (std::size_t one = 0; one < much; ++one) current[one] = static_cast<unsigned char>(text[at + one]);

        made.push_back(static_cast<char>(filter));

        for (std::size_t one = 0; one < much; ++one) {
            const int left = one > 0 ? current[one - 1] : 0;
            const int up = prior[one];
            const int corner = one > 0 ? prior[one - 1] : 0;
            const int all = left + up - corner;
            const int to_left = std::abs(all - left);
            const int to_up = std::abs(all - up);
            const int to_corner = std::abs(all - corner);
            const int paeth = to_left <= to_up && to_left <= to_corner ? left : to_up <= to_corner ? up : corner;
            const std::array<int, 5> guesses{0, left, up, (left + up) / 2, paeth};

            made.push_back(static_cast<char>((current[one] - guesses[static_cast<std::size_t>(filter)]) & 0xFF));
        }

        prior = current;
    }

    return made;
}

// The text in rows of `columns` bytes behind TIFF predictor 2 for one eight-bit component: every
// byte of a row after the first less the one before it.
[[nodiscard]] std::string tiff(std::string_view text, std::size_t columns) {
    std::string made{text};

    for (std::size_t at = 0; at < text.size(); ++at)
        if (at % columns != 0) made[at] = static_cast<char>(text[at] - text[at - 1]);

    return made;
}

// The data of an object stream (ISO 32000-2, 7.5.7) and where its objects begin (`/First`).
struct Packing {
    std::string data;
    std::size_t first = 0;
};

// The objects as an object stream holds them: after `lead` (white space or a comment), the pairs
// of number and offset, then the objects, each on a line of its own.
[[nodiscard]] Packing packing(const std::vector<std::pair<std::uint32_t, std::string>>& objects, std::string_view lead = {}) {
    std::string head{lead};
    std::string body;

    for (const auto& [number, text] : objects) {
        head += std::to_string(number) + " " + std::to_string(body.size()) + " ";
        body += text + "\n";
    }

    return {head + body, head.size()};
}

// The objects of a document of one page, 300 by 400 points, as an object stream holds them.
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::string>> single() {
    return {{80, "<< /Type /Catalog /Pages 81 0 R >>"},
            {81, "<< /Type /Pages /Kids [82 0 R] /Count 1 >>"},
            {82, "<< /Type /Page /Parent 81 0 R /MediaBox [0 0 300 400] >>"}};
}

// A PDF file assembled for a trial: objects one after another, a cross-reference table or stream
// for the objects written since the last one, a trailer pointing to the section before. Offsets
// count from the header, so junk before it shifts them as writers that prepend it do. Objects in
// an object stream are remembered for the cross-reference stream.
class Assembly {
public:
    // A file with the header of the version, after the junk.
    explicit Assembly(std::string_view version = "1.4", std::string_view junk = {}) : base_(junk.size()) {
        text_ = std::string{junk} + "%PDF-" + std::string{version} + "\n%\xE2\xE3\xCF\xD3\n";
    }

    // Appends an object.
    void object(std::uint32_t number, std::string_view body) {
        offsets_[number] = text_.size() - base_;
        fresh_.push_back(number);
        text_ += std::to_string(number) + " 0 obj\n" + std::string{body} + "\nendobj\n";
    }

    // Appends a stream object, its length counted.
    void stream(std::uint32_t number, std::string_view dict, std::string_view data) {
        object(number, "<<" + std::string{dict} + " /Length " + std::to_string(data.size()) + ">>\nstream\n" + std::string{data} + "\nendstream");
    }

    // Appends an object stream holding the objects after `lead`, its data made by `encode` (by
    // default stored in the zlib format) and its dictionary holding `filter` besides its type,
    // count and start.
    void packed(std::uint32_t number, const std::vector<std::pair<std::uint32_t, std::string>>& objects,
                const std::function<std::string(std::string_view)>& encode = zlibbed, std::string_view filter = " /Filter /FlateDecode",
                std::string_view lead = {}) {
        const Packing made = packing(objects, lead);

        for (std::size_t at = 0; at < objects.size(); ++at) inside(objects[at].first, number, static_cast<std::uint32_t>(at));

        stream(number, "/Type /ObjStm" + std::string{filter} + " /N " + std::to_string(objects.size()) + " /First " + std::to_string(made.first),
               encode(made.data));
    }

    // Notes that object `number` is the `index`-th of object stream `pack`, for the cross-reference
    // streams after.
    void inside(std::uint32_t number, std::uint32_t pack, std::uint32_t index) { inside_[number] = {pack, index}; }

    // The entries of a cross-reference stream about to be written as object `number`, which is
    // placed where the file ends now: for every number from zero, type 1 and the offset of an
    // object written directly, type 2 with the object stream and the index of a packed one, and
    // type 0 (free) for the numbers between.
    [[nodiscard]] std::vector<std::array<std::uint64_t, 3>> entries(std::uint32_t number) {
        offsets_[number] = text_.size() - base_;

        std::uint32_t top = number;

        for (const auto& [one, place] : offsets_) top = std::max(top, one);
        for (const auto& [one, place] : inside_) top = std::max(top, one);

        std::vector<std::array<std::uint64_t, 3>> made(top + 1, std::array<std::uint64_t, 3>{0, 0, 0});

        for (std::uint32_t one = 0; one <= top; ++one) {
            if (const auto direct = offsets_.find(one); direct != offsets_.end()) made[one] = {1, direct->second, 0};
            else if (const auto packed = inside_.find(one); packed != inside_.end()) made[one] = {2, packed->second.first, packed->second.second};
        }

        return made;
    }

    // Appends a cross-reference stream as object `number` with the data as given, its dictionary
    // `dict` after its type, with `/Prev` to the section before and the length; then "startxref"
    // pointing to it. Returns its offset.
    std::size_t xref_given(std::uint32_t number, std::string_view dict, std::string_view data) {
        const std::size_t at = text_.size() - base_;

        offsets_[number] = at;
        text_ += std::to_string(number) + " 0 obj\n<</Type /XRef" + std::string{dict} +
                 (previous_ == 0 ? std::string{} : " /Prev " + std::to_string(previous_)) + " /Length " + std::to_string(data.size()) +
                 ">>\nstream\n" + std::string{data} + "\nendstream\nendobj\n";
        text_ += "startxref\n" + std::to_string(at) + "\n%%EOF\n";

        fresh_.clear();
        previous_ = at;

        return at;
    }

    // Appends a cross-reference table for the objects written since the last section, in runs of
    // consecutive numbers, then the trailer with `/Size` and `/Prev`, and the end of the file.
    // Returns the offset of the table.
    std::size_t table(std::string_view trailer) {
        const std::size_t at = text_.size() - base_;

        std::sort(fresh_.begin(), fresh_.end());

        text_ += "xref\n";

        if (previous_ == 0) text_ += "0 1\n0000000000 65535 f\r\n";

        for (std::size_t from = 0; from < fresh_.size();) {
            std::size_t to = from + 1;

            while (to < fresh_.size() && fresh_[to] == fresh_[to - 1] + 1) ++to;

            text_ += std::to_string(fresh_[from]) + " " + std::to_string(to - from) + "\n";

            for (std::size_t one = from; one < to; ++one) text_ += padded(offsets_[fresh_[one]], 10) + " 00000 n\r\n";

            from = to;
        }

        closed(at, "trailer\n<<" + std::string{trailer} + sized() + ">>\n");

        return at;
    }

    // Appends a cross-reference stream as object `number` for every object so far: offsets, the
    // objects inside object streams, and itself; fields 1, 4 and 2 bytes wide, with the PNG Up
    // predictor when `predicted`.
    std::size_t xref_stream(std::uint32_t number, std::string_view trailer, bool predicted) {
        const std::vector<std::array<std::uint64_t, 3>> rows = entries(number);
        const std::string fields = fielded(rows, {1, 4, 2});
        const std::string parms = predicted ? " /DecodeParms << /Predictor 12 /Columns 7 >>" : "";

        return xref_given(number, " /W [1 4 2] /Size " + std::to_string(rows.size()) + " /Filter /FlateDecode" + parms + std::string{trailer},
                          zlibbed(predicted ? png(fields, 7, {2}) : fields));
    }

    // The offset an object was written at, counted from the header.
    [[nodiscard]] std::size_t offset(std::uint32_t number) const { return offsets_.at(number); }

    // The file as it is so far.
    [[nodiscard]] std::string& text() { return text_; }

    // The file as bytes.
    [[nodiscard]] Bytes bytes() const { return bytes_of(text_); }

private:
    // A number written with leading zeros to the width.
    [[nodiscard]] static std::string padded(std::size_t value, std::size_t width) {
        std::string made = std::to_string(value);

        return std::string(width > made.size() ? width - made.size() : 0, '0') + made;
    }

    // The `/Size` and `/Prev` of the next trailer.
    [[nodiscard]] std::string sized() const {
        std::uint32_t top = 0;

        for (const auto& [one, place] : offsets_) top = std::max(top, one);

        return " /Size " + std::to_string(top + 1) + (previous_ == 0 ? std::string{} : " /Prev " + std::to_string(previous_));
    }

    // Ends a section: the trailer, "startxref" with the section's offset, the end-of-file mark.
    void closed(std::size_t at, const std::string& trailer) {
        text_ += trailer + "startxref\n" + std::to_string(at) + "\n%%EOF\n";

        fresh_.clear();
        previous_ = at;
    }

    std::string text_;
    std::size_t base_ = 0;
    std::size_t previous_ = 0;
    std::map<std::uint32_t, std::size_t> offsets_;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> inside_;
    std::vector<std::uint32_t> fresh_;
};

// The objects of a document of four pages with everything a description reports: pages under two
// nodes, inherited media boxes, resources and rotation, a crop box, a user unit, an image page
// without text, a form that brings a font, fonts of three kinds (a standard one, a TrueType font
// with a file, a composite one whose descendant has a file), three bookmarks in two levels, two
// links among other annotations, a form field, and the information dictionary in every encoding.
void filled(Assembly& one) {
    one.object(1, "<< /Type /Catalog /Pages 2 0 R /Outlines 20 0 R /AcroForm << /Fields [30 0 R] >> >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 10 0 R] /Count 4 /MediaBox [0 0 595.276 841.89] /Resources 5 0 R >>");
    one.object(3, "<< /Type /Page /Parent 2 0 R /Annots [40 0 R 41 0 R] >>");
    one.object(4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources 6 0 R >>");
    one.object(5, "<< /Font << /F1 7 0 R >> >>");
    one.object(6, "<< /Font << /F1 7 0 R /F2 8 0 R /F3 9 0 R >> >>");
    one.object(7, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    one.object(8, "<< /Type /Font /Subtype /TrueType /BaseFont /Lato /FontDescriptor 15 0 R >>");
    one.object(9, "<< /Type /Font /Subtype /Type0 /BaseFont /Noto /DescendantFonts [17 0 R] >>");
    one.object(10, "<< /Type /Pages /Parent 2 0 R /Kids [11 0 R 12 0 R] /Count 2 /Rotate 90 /MediaBox [0 0 612 792] >>");
    one.object(11, "<< /Type /Page /Parent 10 0 R /CropBox [10 10 602 782] /Resources << /XObject << /I 13 0 R >> >> >>");
    one.object(12, "<< /Type /Page /Parent 10 0 R /Rotate 0 /UserUnit 2 /Resources << /XObject << /X 14 0 R >> >> /Annots [42 0 R] >>");
    one.stream(13, "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8", "\x80");
    one.stream(14, "/Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /Font << /F1 7 0 R >> >>", "BT /F1 9 Tf (x) Tj ET");
    one.object(15, "<< /Type /FontDescriptor /FontName /Lato /FontFile2 16 0 R >>");
    one.stream(16, "", "font");
    one.object(17, "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /Noto /FontDescriptor 18 0 R >>");
    one.object(18, "<< /Type /FontDescriptor /FontName /Noto /FontFile3 19 0 R >>");
    one.stream(19, "/Subtype /CIDFontType0C", "font");
    one.object(20, "<< /Type /Outlines /First 21 0 R /Last 22 0 R /Count 3 >>");
    one.object(21, "<< /Title (Jeden) /Parent 20 0 R /Next 22 0 R /First 23 0 R /Last 23 0 R >>");
    one.object(22, "<< /Title (Dwa) /Parent 20 0 R /Prev 21 0 R >>");
    one.object(23, "<< /Title (Pod) /Parent 21 0 R >>");
    one.object(30, "<< /FT /Tx /T (name) >>");
    one.object(40, "<< /Type /Annot /Subtype /Link /Rect [0 0 1 1] >>");
    one.object(41, "<< /Type /Annot /Subtype /Text /Rect [0 0 1 1] >>");
    one.object(42, "<< /Type /Annot /Subtype /Link /Rect [0 0 1 1] >>");
    one.object(50, "<< /Title (Metody \\(numeryczne\\) \\205 wyk\\233ad) /Author <FEFF001B706C001B004D00690063006800610142> "
                   "/Producer (pdfTeX\\0551.40) /CreationDate (D:20260302141137+01'00') /Trapped /False /Keywords <41 42\n43> "
                   "/Custom <41424> >>");
}

// What `filled` describes.
[[nodiscard]] meta::Document filled_document() {
    meta::Document made;

    made.version = "1.4";
    made.pages = 4;
    made.sheets = {meta::Sheet{595.276, 841.89, 2}, meta::Sheet{772, 592, 1}, meta::Sheet{1224, 1584, 1}};
    made.with_text = 3;
    made.fonts = 3;
    made.embedded = 2;
    made.bookmarks = 3;
    made.links = 2;
    made.form = true;

    return made;
}

// The fields of `filled`, in file order.
[[nodiscard]] std::vector<meta::Field> filled_fields() {
    return {info("Title", "Metody (numeryczne) – wykład"), info("Author", "Michał"),           info("Producer", "pdfTeX-1.40"),
            info("CreationDate", "D:20260302141137+01'00'"), info("Trapped", "False"), info("Keywords", "ABC"),
            info("Custom", "AB@")};
}

// What `filled` describes in a file of PDF 1.5.
[[nodiscard]] meta::Document filled_later() {
    meta::Document made = filled_document();

    made.version = "1.5";

    return made;
}

// Object stream 70 holding `single`, from "<<" to "endstream": its data `data` as given (encoded
// already), its length said to be `length`, `count` objects promised, and `filter` the rest of
// its dictionary.
[[nodiscard]] std::string object_stream(std::string_view data, std::size_t length, std::string_view filter = " /Filter /FlateDecode",
                                        std::size_t count = 3) {
    return "<< /Type /ObjStm /N " + std::to_string(count) + " /First " + std::to_string(packing(single()).first) + std::string{filter} +
           " /Length " + std::to_string(length) + " >>\nstream\n" + std::string{data} + "\nendstream";
}

// A file of PDF 1.5 whose document of one page lies in object stream 70 written as `stream`
// (`object_stream`), described: with a cross-reference stream when `indexed`, otherwise with none,
// so that the file is scanned.
[[nodiscard]] meta::Description single_packed(std::string_view stream, bool indexed) {
    Assembly one{"1.5"};

    for (std::uint32_t at = 0; at < 3; ++at) one.inside(80 + at, 70, at);

    one.object(70, stream);

    if (!indexed) return described(bytes_of(one.text() + "%%EOF\n"));

    const std::vector<std::array<std::uint64_t, 3>> rows = one.entries(90);

    (void)one.xref_given(90, " /W [1 4 2] /Size " + std::to_string(rows.size()) + " /Root 80 0 R /Filter /FlateDecode",
                         zlibbed(fielded(rows, {1, 4, 2})));

    return described(one.bytes());
}

// A number written with leading zeros to the ten digits of a cross-reference table's offset.
[[nodiscard]] std::string ten(std::size_t value) {
    const std::string made = std::to_string(value);

    return std::string(made.size() < 10 ? 10 - made.size() : 0, '0') + made;
}

// pdfTeX 1.40.25: two A4 pages, PDF 1.5 with object streams and a cross-reference stream.
constexpr std::string_view from_pdftex =
    "255044462d312e350a25d0d4c5d80a31342030206f626a0a3c3c0a2f4c656e67746820383720202020202020200a2f46696c746572202f466c61"
    "74654465636f64650a3e3e0a73747265616d0a78da25cba11280200c00d0ce572c42706e138754ef3490d73c1b18cd7ebe72f6f75673e31e0938"
    "e21455c02e6089b890402245960c56e1f025f0ec5b6d7738ad7c83216356d11e080611144d3f7d3a719bb917aa5414bc0a656e6473747265616d"
    "0a656e646f626a0a32332030206f626a0a3c3c0a2f4c656e67746820353920202020202020200a2f46696c746572202f466c6174654465636f64"
    "650a3e3e0a73747265616d0a78da730ae1d2773331503034d13336313352084953303432d1b3303052303730d33334b25408495188d670294fd4"
    "8c0df1e2720de1020017410b440a656e6473747265616d0a656e646f626a0a33312030206f626a0a3c3c0a2f4c656e6774682036393920202020"
    "2020200a2f46696c746572202f466c6174654465636f64650a3e3e0a73747265616d0a78da6d54c16ea33010bdf315de43a5f690c686404c1545"
    "2210a41cb6ad9a6ab5d7049c2e5203889043ff7ee7cd3869b75a2941e3e799796f1ec6373f9eb793aceef66e12dd6bf5e24edd79a8dc24ffb9eb"
    "839b9ba2abce47d78e8fced5aebeec9e1ed4f3d0555b37aadb7c536cda66bca3e44d5bbd9f6b77c9fa7fd2cabd35ed670a78d4edabfb3de9c7e3"
    "de0e13fa69e4bd36e33bed7fdf52b456d7b5e2d45f6e38355dfba0ccbdd69a80755be7dd11ba4fc1d473abe945cda169ebc10b507bc8094ca8ea"
    "a61afd8a9fd5910c40f1f6e334bae3a63d74c162a1a62fb4791a870f5676174c9f86da0d4dfba66eafaa08dd9efbfedd4181d2c172a96a77a066"
    "34ebe3eee8d4f4fb50d7edd78fdea990d746d4545ded4efdae72c3ae7d73c142eba55a94e532706dfd6dcfc452b23f5c7213cad5733ccccc2c09"
    "58232e19981704184bb149e91186c98c8010ed43aed31413808cb814c01290188ae7dc4827686a916dc1124a491a519cce1830390064a4926122"
    "0232f4c872ee91a147867ec55c8035010568d7925180765d606e11b686f412d9e58a018a09407629c22886411727627d71a6fab31bc807f0ebb0"
    "48e14928a324f4e775f4b986782d83cce08e8ea50efcda4abc429c4a9c215e490cd13a67674d08ab35e339a41ae637114637cc1f468c7fe14eb0"
    "9e7d597b7d26fe17834693c81b84fb662e9c70d1641c6be65f09ce9ca28b4e02c530564739d7fad7cab17fe7d015fa7707fe444b8cf9acc41934"
    "58c33d43f86143c9811f9667322934587f28a0cdc6726230a7152f738ef934ea081a6cc6b5acc7ae048706cb07c3c4dc13fac962ae5dcb8ccc5b"
    "723c83ced473213ff5ef8d0f662ab58c7b2ef8937a2ef4493d17ce6d5a886fe04a992bce39c77f52e0ca0af18dcfa878e83f577ffaf0e1e272b9"
    "5e07d57918e8a6e01b886f017cff4debae9754dff5a8e23fdf6e97fb13aba732f80b25f472670a656e6473747265616d0a656e646f626a0a3332"
    "2030206f626a0a3c3c0a2f4c656e67746820363939202020202020200a2f46696c746572202f466c6174654465636f64650a3e3e0a7374726561"
    "6d0a78da6d54c16ea33010bdf315de43a5f690c686404c15452210a41cb6ad9a68b5d7149c2e5203889043ff7ee7cd3869b75a2941e3e799796f"
    "1ec6373f9eb793acee5edd24bad7eac59dbaf350b949fe73df07373745579d8fae1d1f9dab5d7dd93d3da8e7a1abb66e54b7f9a6d8b4cd7847c9"
    "9bb67a3fd7ee92f5ffa4957b6bdacf14f0a8db9dfb3de9c7e3608709fd34f276cdf84efbdfb714add575ad38f5971b4e4dd73e2873afb52660dd"
    "d6797784ee5330f5dc6a7a517368da7af002d42be404265475538d7ec5cfea4806a078fb711add71d31eba60b150d317da3c8dc3072bbb0ba64f"
    "43ed86a67d53b75755846ecf7dffeea040e960b954b53b50339af5717f746afa7da8ebf6eea3772ae4b511355557bb53bfafdcb06fdf5cb0d07a"
    "a91665b90c5c5b7fdb33b194bc1e2eb909e5ea391e66669604ac11970ccc0b028ca5d8a4f408c364464088f621d7698a0940465c0a6009480cc5"
    "736ea41334b5c8b66009a5248d284e670c981c003252c9301101197a6439f7c8d02343bf622ec09a8002b46bc92840bb2e30b7085b437a89ec72"
    "c500c50420bb146114c3a08b13b1be3853fdd90fe403f87558a4f024945112faf33afa5c43bc9641667047c752077e6d255e214e25ce10af2486"
    "689db3b32684d59af11c520df39b08a31be60f23c6bf702758cfbeacbd3e13ff8b41a349e40dc27d33174eb868328e35f3af04674ed145278162"
    "18aba39c6bfd6be5d8bf73e80afdbb037fa225c67c56e20c1aace19e21fcb0a1e4c00fcb3399141aac3f14d06663393198d38a9739c77c1a7504"
    "0d36e35ad66357824383e5836162ee09fd6431d7ae6546e62d399e4167eab9909ffaf7c60733955ac63d17fc493d17faa49e0be7362dc43770a5"
    "cc15e79ce33f29706585f8c667543cf49fab3f7df87071b95caf83ea3c0c7453f00dc4b700beffa675d74baaef7a54f19f6fb7cbfd89d55319fc"
    "05a81372b70a656e6473747265616d0a656e646f626a0a33392030206f626a0a3c3c0a2f50726f647563657220287064665465582d312e34302e"
    "3235290a2f417574686f72285c3337365c3337375c303030745c303030775c30303061292f5469746c65285c3337365c3337375c303030505c30"
    "3030725c3030305c3336335c303030625c3030306b5c30303061292f5375626a65637428292f43726561746f72285c3337365c3337375c303030"
    "745c303030775c30303061292f4b6579776f72647328290a2f4372656174696f6e446174652028443a32303236313032303230323435385a290a"
    "2f4d6f64446174652028443a32303236313032303230323435385a290a2f54726170706564202f46616c73650a2f505445582e46756c6c62616e"
    "6e6572202854686973206973207064665465582c2056657273696f6e20332e3134313539323635332d322e362d312e34302e3235202854655820"
    "4c69766520323032332f44656269616e29206b706174687365612076657273696f6e20362e332e35290a3e3e0a656e646f626a0a322030206f62"
    "6a0a3c3c0a2f54797065202f4f626a53746d0a2f4e2033330a2f4669727374203233320a2f4c656e67746820313039302020202020200a2f4669"
    "6c746572202f466c6174654465636f64650a3e3e0a73747265616d0a78da9d56db72db36107de757eca3d519133712978c2633b25d374e73f1d8"
    "729ad6d103232132a712a921e9c6fdfbee82a444c94ee2f48124c0bd1d9c5d2c2080430a32010d9a83039b80e020c203229120244894881412ee"
    "4068485205c240e24ca4204d500b3f167d48d0a9205726e52053300e8760950529c05af463c0290dd28243c712bd619c48a173851a52633c8701"
    "2d06d33841b9910a24cac95a6158a724a0d8590b090a12058abece822200898ac8bf120928835f6380a22b843a1e03bb06f65b392d819dc15116"
    "f311bc7c191d7d5246e3633e71ce5fd3cbd36bb11d15a347b69f9fb23da3d7577a658f2c6e914b0e57c0cef3664696e371c4a6ff6e3cb0cb6ce9"
    "23765a168d2f9a1a1920c5885df9babcafe6be2676c39fb77e916727e503dc724a994b636934a64bc4d6cd22f453a103222b684f8aa24477b794"
    "3e8a3c8bf6a20671c44eca6ae1abe050ccd82b76c14e7182199d11827983c0a58a2dc7dab032b69414c5636371ea62e524aa5ddf7f6ed0257b93"
    "177fb3c9781c02b0c9bcc9cb825db39bab0b7a8eee9a6653bf60cc3f64ebcdcac765b51c21a20ed480a18f7ffe056d508975a46287d556dcaf56"
    "b36fea26adaed031c7ea79962e7eb97e9eae76499c68b7af7b8ee902caf17942c5d9e596b6820b635463975539bff64821e6f8ec1cd8d43f3487"
    "693848be5487c997e27f247fb72029f7176470a37c8bd443ddef11f594ae46501afbc20f89fa3e39b729e7b30145e4e1ccd7f32adf3465d57a7c"
    "97ad5132cdd7be3ebe2ad75981bf57d9b2c666d02a9c74541d4bec5702b724686391a7493d279e7082ac679b573e5fde35d4b57055be959149c4"
    "2e9a6c95cf27c572e5816391377efd013b59c43e763649ca035a435d4f1bd0583b0637063dca3ac0558431c95c827d10f5c2bcd7c3799aea200f"
    "327c02d07e4202852d4f9243451d5604a7618c5f324ed071ff1dfedfda7463db8d7bf9d60681aa810ce3fe0cf527e56ad1332fb514c4ea3efb74"
    "6004f61d1fb0eff81efb34ddb1cfdd73d8d7627f23fd5acccb455e2cd153fee58bc7dd40bbe7565bac5693b0d7e00ccb080b5b304fa71b2bf0ed"
    "d857f670b0256901dbae06e12756c74956fb50cbfb4bdf230854d77bcff3aa6e4eefb20adb64c4de64dd04e345ec8f7cd1dcd574f605d51e371d"
    "7ee1c7b4bc2972fce74189e166fe1970fd96d84727ed213a21f9101ecd7a78e6c7f0e46378d4ce6aea67f7d48c22f67bbea8b75db5ed19076cbf"
    "bf6f56794146011674a71f81820e6fefadb7cb1bac8a8ebe0974b8ba06a8ba067a59f97f3a5f7b66696f269e347b475dc80ecda8dc71094767e5"
    "fcf8bac9aa664437205a4e7b8150ed385c084c3bde2009b118d18569379723ba178579edc3d9f84bd0312d27787aaef3e620d050737608682b23"
    "cfe9a197a17438d9ba6933a35a482a7d168a81399615ea29fdb8044e33dcbbe5326a6b617b1de9f3bca33aac43993e0f4bff16ab8addd47e5712"
    "ef37be682f127425ed42fd07cfa5af6e0a656e6473747265616d0a656e646f626a0a34302030206f626a0a3c3c0a2f54797065202f585265660a"
    "2f496e646578205b302034315d0a2f53697a652034310a2f57205b31203220315d0a2f526f6f74203338203020520a2f496e666f203339203020"
    "520a2f4944205b3c33413635343432433742433745443937454130454644353536423636304139313e203c334136353434324337424337454439"
    "37454130454644353536423636304139313e5d0a2f4c656e67746820393820202020202020200a2f46696c746572202f466c6174654465636f64"
    "650a3e3e0a73747265616d0a78da15cab90182400005d1ff571450113cb8448556ccc96d87aaacc38e703678138da43528c8d98f863d06180125"
    "eed820c1163b1cac32fe2932e468d0a24385a3f58d4f81136a9c71c11537fb2d27737c7a3cf0c40b2326a71fb958f40726e606b50a656e647374"
    "7265616d0a656e646f626a0a7374617274787265660a333435360a2525454f460a";

// ReportLab 4: three pages (A4, Letter landscape, 200 by 300 points), a classic table.
constexpr std::string_view from_reportlab =
    "255044462d312e330a25938c8b9e205265706f72744c61622047656e6572617465642050444620646f63756d656e7420286f70656e736f757263"
    "65290a312030206f626a0a3c3c0a2f46312032203020520a3e3e0a656e646f626a0a322030206f626a0a3c3c0a2f42617365466f6e74202f4865"
    "6c766574696361202f456e636f64696e67202f57696e416e7369456e636f64696e67202f4e616d65202f4631202f53756274797065202f547970"
    "6531202f54797065202f466f6e740a3e3e0a656e646f626a0a332030206f626a0a3c3c0a2f41203c3c0a2f53202f555249202f54797065202f41"
    "6374696f6e202f555249202868747470733a2f2f6578616d706c652e6f7267290a3e3e202f426f72646572205b203020302030205d202f526563"
    "74205b20313030203639302032303020373130205d202f53756274797065202f4c696e6b202f54797065202f416e6e6f740a3e3e0a656e646f62"
    "6a0a342030206f626a0a3c3c0a2f416e6e6f7473205b203320302052205d202f436f6e74656e747320313220302052202f4d65646961426f7820"
    "5b20302030203539352e32373536203834312e38383938205d202f506172656e7420313120302052202f5265736f7572636573203c3c0a2f466f"
    "6e74203120302052202f50726f63536574205b202f504446202f54657874202f496d61676542202f496d61676543202f496d61676549205d0a3e"
    "3e202f526f746174652030200a20202f5472616e73203c3c0a0a3e3e202f54797065202f506167650a3e3e0a656e646f626a0a352030206f626a"
    "0a3c3c0a2f436f6e74656e747320313320302052202f4d65646961426f78205b203020302037393220363132205d202f506172656e7420313120"
    "302052202f5265736f7572636573203c3c0a2f466f6e74203120302052202f50726f63536574205b202f504446202f54657874202f496d616765"
    "42202f496d61676543202f496d61676549205d0a3e3e202f526f746174652030202f5472616e73203c3c0a0a3e3e200a20202f54797065202f50"
    "6167650a3e3e0a656e646f626a0a362030206f626a0a3c3c0a2f436f6e74656e747320313420302052202f4d65646961426f78205b2030203020"
    "32303020333030205d202f506172656e7420313120302052202f5265736f7572636573203c3c0a2f466f6e74203120302052202f50726f635365"
    "74205b202f504446202f54657874202f496d61676542202f496d61676543202f496d61676549205d0a3e3e202f526f746174652030202f547261"
    "6e73203c3c0a0a3e3e200a20202f54797065202f506167650a3e3e0a656e646f626a0a372030206f626a0a3c3c0a2f4f75746c696e6573203920"
    "302052202f506167654d6f6465202f5573654e6f6e65202f506167657320313120302052202f54797065202f436174616c6f670a3e3e0a656e64"
    "6f626a0a382030206f626a0a3c3c0a2f417574686f722028616e6f6e796d6f757329202f4372656174696f6e446174652028443a323032363130"
    "32303230323234302b30302730302729202f43726561746f722028616e6f6e796d6f757329202f4b6579776f726473202829202f4d6f64446174"
    "652028443a32303236313032303230323234302b30302730302729202f50726f647563657220285265706f72744c616220504446204c69627261"
    "7279202d205c286f70656e736f757263655c2929200a20202f5375626a6563742028756e73706563696669656429202f5469746c6520285c3337"
    "365c3337375c303030525c303030655c303030705c3030306f5c303030725c303030745c3030304c5c303030615c303030625c303030205c3030"
    "315c3030355c3030315c30333129202f54726170706564202f46616c73650a3e3e0a656e646f626a0a392030206f626a0a3c3c0a2f436f756e74"
    "2031202f466972737420313020302052202f4c61737420313020302052202f54797065202f4f75746c696e65730a3e3e0a656e646f626a0a3130"
    "2030206f626a0a3c3c0a2f44657374205b203420302052202f466974205d202f506172656e74203920302052202f5469746c6520284a6564656e"
    "290a3e3e0a656e646f626a0a31312030206f626a0a3c3c0a2f436f756e742033202f4b696473205b203420302052203520302052203620302052"
    "205d202f54797065202f50616765730a3e3e0a656e646f626a0a31322030206f626a0a3c3c0a2f46696c746572205b202f415343494938354465"
    "636f6465202f466c6174654465636f6465205d202f4c656e6774682039350a3e3e0a73747265616d0a476170516830453d462c30555c4833545c"
    "704e59545e514b6b3f74633e49502c3b572355315e3233696850454d5f3f4357344b49536939304d6a472e696649434b253f4b232f533a222562"
    "4f3d5a28732f575f5e6b2f635f51572648727e3e656e6473747265616d0a656e646f626a0a31332030206f626a0a3c3c0a2f46696c746572205b"
    "202f415343494938354465636f6465202f466c6174654465636f6465205d202f4c656e6774682035390a3e3e0a73747265616d0a476170516830"
    "453d462c30555c4833545c704e59545e514b6b3f74633e49502c3b572355315e3233696850454d5f5050244f21335e2c4335517e3e656e647374"
    "7265616d0a656e646f626a0a31342030206f626a0a3c3c0a2f46696c746572205b202f415343494938354465636f6465202f466c617465446563"
    "6f6465205d202f4c656e6774682037350a3e3e0a73747265616d0a476170516830453d462c30555c4833545c704e59545e514b6b3f74633e4950"
    "2c3b572355315e3233696850454d5f4e68242c42453141723f7257416a26427075642254576346246e5f7e3e656e6473747265616d0a656e646f"
    "626a0a787265660a302031350a303030303030303030302036353533352066200a30303030303030303631203030303030206e200a3030303030"
    "3030303932203030303030206e200a30303030303030313939203030303030206e200a30303030303030333530203030303030206e200a303030"
    "30303030353733203030303030206e200a30303030303030373638203030303030206e200a30303030303030393633203030303030206e200a30"
    "303030303031303438203030303030206e200a30303030303031333735203030303030206e200a30303030303031343438203030303030206e20"
    "0a30303030303031353230203030303030206e200a30303030303031353932203030303030206e200a3030303030303137373720303030303020"
    "6e200a30303030303031393236203030303030206e200a747261696c65720a3c3c0a2f4944200a5b3c3366313637373762393230643538316635"
    "3162303331613462336632323631323e3c33663136373737623932306435383166353162303331613462336632323631323e5d0a25205265706f"
    "72744c61622067656e6572617465642050444620646f63756d656e74202d2d2064696765737420286f70656e736f75726365290a0a2f496e666f"
    "2038203020520a2f526f6f742037203020520a2f53697a652031350a3e3e0a7374617274787265660a323039310a2525454f460a";

// The pdfTeX sample encrypted by qpdf 11 with AES-256 and an empty user password, object streams
// kept.
constexpr std::string_view from_qpdf =
    "255044462d312e370a25bff7a2fe0a312030206f626a0a3c3c202f457874656e73696f6e73203c3c202f41444245203c3c202f42617365566572"
    "73696f6e202f312e37202f457874656e73696f6e4c6576656c2038203e3e203e3e202f4e616d657320333520302052202f4f70656e416374696f"
    "6e20313220302052202f4f75746c696e657320333120302052202f506167654d6f6465202f5573654f75746c696e6573202f5061676573203231"
    "20302052202f54797065202f436174616c6f67203e3e0a656e646f626a0a322030206f626a0a3c3c202f417574686f72203c6437396335653230"
    "34663662303866633865656661323438663738303033663831653537306432633337646561653962656662353365613365323638656539393e20"
    "2f4372656174696f6e44617465203c37346630656439336365393766633361646333376638343566626564313534393530383765653039626639"
    "36663637313635663536626232366531336639636665623438656366353761336235323333666266383465303131333831623937393e202f4372"
    "6561746f72203c366366383033313235386530373966346261333530366437306231653137333633613764363439373536386666643830343364"
    "636565383531643837633063353e202f4b6579776f726473203c6536333834346632376165613331356464326164623431656166653832666531"
    "31306430666139626563353234636231393334336635326161333163343236373e202f4d6f6444617465203c3732333431623263356461393934"
    "39353662393236333439323937613733613533393530333037383138356438386534363366636531336664323961373933636632356133616131"
    "3232343862313536656636616264636464383135366362653e202f505445582e46756c6c62616e6e6572203c6434653961653138373362343666"
    "37643161663738376431646532333134333631623831333639323935623730356162643833313566653861366166326231303763656232613232"
    "34383861396337393566626636653038393061383862646239316531316464363733303130313135383139336636393363333439363962323632"
    "39346631666538313136386233663034373532396566343939663931663833313939383230323465303439326135646566613739306462366232"
    "3864666533643833343364383231316230316563303836383935393530366663646666623e202f50726f6475636572203c366264323064643036"
    "326338613336306462666533613861663332656434336664393231363934363338613463373862393961303930616261376362616333393e202f"
    "5375626a656374203c61646464393434623531366163346166353764623633663137336138643466386431306330643334326236383237636231"
    "6131353836323463376165313037313e202f5469746c65203c303334643636656433353631356431376236336530386262623131306131356566"
    "383434326232663862616231636634383062663538313462396161303463643e202f54726170706564202f46616c7365203e3e0a656e646f626a"
    "0a332030206f626a0a3c3c202f54797065202f4f626a53746d202f4c656e6774682031303732202f46696c746572202f466c6174654465636f64"
    "65202f4e203332202f466972737420323239203e3e0a73747265616d0a312ff529a0661b8427106eecaaab18cb6374a5254d512bd6a9874b0cd0"
    "4ed44df7acf15f50c22b327e9d35a2ed9c9f557ba009d08a040a73eb98798e2ad6faeae5034c1812720dadee96f08436a06d8df3c8e8812e461e"
    "c2344b75485f76684b24d69546d7af507a4cd98890293432c0f9f25cde1ce66445e5b98f89c6945ed3717defd3a94da0ebcbeabcac85cd74f564"
    "44ac35cf2650b396c8f61e5545da41c169fd11f4ae7c14c897f80fddf436a463f66611414822d7c010847bf713118132770c4fcd4c22f11cab14"
    "7056fde0b0adbceaad08e552fb5109872c5729825466f75cacbe1776587d14bd3e1af42394ba144952a9c2163b62ef139a8998c024dc16386578"
    "bd758c3bc6fc7d965ac57f104dee0cf11f6d915a8a6ac92125de4338833baf6c0c8ac2f878832c97979c8b5e54e22fe81c9918cc4b489a65c76d"
    "d7a6722391c70cba7a790d529385076824e8fd5540a03290f6f97f1b5d41593b2daf2b1e27d357b4d1eac150c0f204a13969fa621fb70192ca71"
    "1d1dff017f2c89e1e1bda6cc362f4083d3212b0c69dfb6b4830c082bc96d9dbb36ba991ac289c3f1531a6f6ec51a74d9f6a579b52949154cfa21"
    "1ac7b762fe7f58da9c80eaa884ed07cddded165659e01c9c6133e9a1b9568e32fcaceea10211e9a211aa3081d5efab0d5ada1655942bf0bbbebf"
    "920ee20452e4d755ef9764157a3133d365dbe614e1dedc08f1b63b394dd66a8b7c557ddf427addefb151901a988b7e06de44a9cadb8ab0b175ce"
    "935deff1e17f18661d9495e75f27b895a7e2d8987092f97dac7dc02ebd0366b0411c673279b3df4cd395d7aa6d3e8c0d511947cc34003cb71e2e"
    "d722580e4554ae1d934aef2946022c8b6245fb6c39c0556c7b55580b3dc011207b77f93681ff09fe54f0b00db09885fb0af6faa2b4cc7ea955ce"
    "980b0eb5be4033c69bc209df1196dc44823f8a5725cf31cf4cabcf8b9d7fdae8817d84b957776b9458509ca0ceffc75cfd22bd792414868f8ce9"
    "80266584ef5ba7455ca453e2f9e9c01a8dadb14b324353c44979a725232c1f772af7739f7d460b9a847dbcc2d9d1b2b484a5d07d43fcbcd289f5"
    "b3b20bdc9d9e9680115c63a68d02df0d9ae7b70cbb46325171cae5d808758463ce521ff1458f58ee131aa8dd73aa0ed6783ca5a0023cbe6be8ff"
    "31cee5eabef2ca954d3e446b393a95f97b6dd948833b7b33ee514394b3f1c0bd9155fe488f5c08218b9c8a4785b3754ca4903e7d3ae0d2f22005"
    "001ec7588ac2c77fbb005da3c3bc6e9e476e98aca198c1df8565fb3b94e54dbad0bd4b2c53667e3ba753bb0779cacc088c6ad83fae209084ae65"
    "b7152bbd56245f0553de155531c94e6bedbf32dda1ed915b8d912481b8d740536ffac96d53cf0c6cabac1b63a809ed61171d4ef6a632f6333864"
    "294fd59ae103d00ad4e5b1a0120cc5e55edf5b62f9c59b6e7e1d753a9715c175fef8b290ee4e54f1cee2043b5feb30fe5f1806119b9b43288365"
    "6e6473747265616d0a656e646f626a0a33362030206f626a0a3c3c202f46696c746572202f466c6174654465636f6465202f4c656e6774682031"
    "3132203e3e0a73747265616d0a891431eb8bdb0ecee9f1deea6fc87c5701708fd44b85a7eef201f4c71bc6f44cb1f237d8673851939b37de4971"
    "d9c9fa98c1676f4a0082e68664c11a9eca5059a3639cc734a638a719fad85e26e9d0db0774f5921f90b54b31a62a08aeeff3e8e9e9e5ac8a4598"
    "97a094e1826eff2ef8656e6473747265616d0a656e646f626a0a33372030206f626a0a3c3c202f46696c746572202f466c6174654465636f6465"
    "202f4c656e67746820373230203e3e0a73747265616d0a29737852812bda7e10012c9ab1255e4a9b94cc9d669619a61cc5385ed0d50568770aa4"
    "f18c9fe5fe9841ec2acc191cb8958c8a718f0cfdad6d5fb38e58e669f3b77214a2c5ac47dc15d595b60a1575d443af3788c98c2c6a64133bbbea"
    "664563b1097426f3b501fb3a34d628f8c1b67f4afe481084ca33604d895e2ecafa46c945e95b4a39367c797a697b703490c02c76fb6c82e8e6a8"
    "8c6a52a5f5e6748623fc5d44b455ef66e7fe0273a84be4b4b4abf0475fa06f172ea8b4bf144242335bcde2aff49ff6408aa003c744da3c739dfa"
    "6516fc1f00991394feee8853210c0432d655d88b88653e1823a397bfe3d7b371be3f401e27cf15aab0d800abd9638ffef1fb48b005d67cb24a8b"
    "9b62ffe98d0c3d6d8b6f87a3a18dac584541bd46af488ab44d78aa8d31e856a078231a433bbcb8de58041e70f5ccd9d699fad686e2095a2d7146"
    "57e4a2be21d95c77064ca490d5d23fe3a6b46908b22000b23d0dbd4f1cada8b25a74ba8c3e1561565f185bd47414ee655eabce41f5472b2d1913"
    "7f736346447500d8eb6f5215da19ebd1618e5f1ed485f15e317485775e2c1bb72fd0d480242f76618dd3aecaa31df19b8ac7a8c4953811b5c506"
    "26613ea6c1243be0e318feda1a7938363113fa67eb3782a190c2d2f8020cb6b073054677c9f3f62a0a223a2e9dfa56351198557fc3a393ef7a18"
    "a52dd0a8427fbd4bfa0ce95f999c85b5a01c735f2bccb11a614eaeba027e29de94c46507c6f43adbabcf388dddefa2a94162c06f9b8cbe4044a6"
    "bb3d7b156fa41c9d243e71fa9c9874e0555be5eb74236e8b2a4fa24b42975f105f4e8201f0c5fe726d362564571e1fb091bee132c586a1a85d4a"
    "eec9bc60c688f2c802ddb4d881d7a291c48249ca2e302c02b56099bdc97d7f8d2606da45bcec3e055d065fcc6ab7c703ffaf6f00cbcdbceed2ab"
    "d2ce36f29a40ffbbc5dca13e6f3c094d95be27a3c1bad3732e17ebb0d6487b11d58a78599b23a4ea98740abffb0f0d656e6473747265616d0a65"
    "6e646f626a0a33382030206f626a0a3c3c202f46696c746572202f466c6174654465636f6465202f4c656e67746820373230203e3e0a73747265"
    "616d0a277296e8b0c9d78117b27f0ac516200990f24a1a1057cc2e9a1d5dfbc2f556e52c3c0ae10cb91f94d11ca3c9bee480e5452951ec34ecb7"
    "6a7c1bd22491652c3b3e8fdf592095bfa7bf6d44fcca246f06b65e7b4788b3da3d7326762ab794fc5f225a9afb567f51d6ee2bd313fb9d44920e"
    "08f758cde6c26273f9c6b72f1bc87e97993f58a3dddb9ed98becf924f3e8519d219a31c819d28b43e304111f8c7183f8a2b4a32588e43e17e6e8"
    "aaf5942dad3df99dd2d8478771ca73e6d4af9912b16aa2a3d813d910fb0ffdebd088731d8e03db84647d4c5798f4459c04131342c426796070d4"
    "58801ccd91f772410973e6e45f9279dfa5ee051e2b59c677fe3ab9ebc75ce2da1deab810fe2c5ef99df9928400a91a33f5ab2a5021fc54514cb5"
    "696b1743bede89df6601b3be5dbd3dc7030daef0804ed1dba6c353a4e16b89e9deba3859285073dc16382ef16719853e0f1ba2fdf58d142c32c5"
    "dc7f7b8e6bba39b2828808142fec264dbc5c7b2625679a29b6a186154a2994a2d04d1db295e084b4e9c407b98db1e165e85711e3da897e80536f"
    "2ca741b437ec221b375802f73e920c82cc5771b14d03bdca1779fcb7997b36d79d23f90267c24e771e1280d7a2a6f674fb3af008e18a1959cc5e"
    "f16518968ad3b777e3fa6fff57836e31d9ed4b8acf6d924c84021ef3d995c24a034a8303ecfbbbcd499cd784d3960c372fe9911b9a1d7e24b0eb"
    "6556ba26327a1d5b51d810603763845cebd10cb5f4ef2e708857e028d4f74ff5991fd0aea740e04c090749b69eaa4af8773a29aea913d0d980b8"
    "ab755c1f3aaa3e07d539a23e01b0f841a4be92edcd11292c14df16ee9032bf314ef7af93b83cf73fc82edda35a7f04d771f2ea715d9406b2c785"
    "7d64805835f144ffa9748de0c81cccb0ebac9908681314e04f8b8307b473af6afc5123c2485d7f3b4c902501edf03eb948b80d72e682e6c9a9f1"
    "22f7f994089073c3f0a99234987d586a0f14fc4e57a7898f2a2dad656e6473747265616d0a656e646f626a0a33392030206f626a0a3c3c202f46"
    "696c746572202f466c6174654465636f6465202f4c656e677468203830203e3e0a73747265616d0aaabed97bf711660cd8e1cc7454c0c740f186"
    "1c6bf1192a58b10497d79cac07f9b8792579305902ca8d96aea72a9c50abde8561f1a0e98c374f5e1a8610fe8de56b03eeb12293ca5b1f7d16e0"
    "199e6c49656e6473747265616d0a656e646f626a0a34302030206f626a0a3c3c202f4346203c3c202f5374644346203c3c202f41757468457665"
    "6e74202f446f634f70656e202f43464d202f4145535633202f4c656e677468203332203e3e203e3e202f46696c746572202f5374616e64617264"
    "202f4c656e67746820323536202f4f203c3032353636613434646661353939623164323238646138613638643265366432666637353736653562"
    "633832353234326633613466346462323836633334643436363166623165663061343936663339396261643437613565356636613732643e202f"
    "4f45203c336636376563353131643730623239336231303763313166626335363165383965653333643964366265633664303036356361313535"
    "633063626665663239623e202f50202d33333838202f5065726d73203c3935656363393563393133353339323964363337336566316238356338"
    "3162363e202f522036202f53746d46202f5374644346202f53747246202f5374644346202f55203c306263653264316164303230303232333131"
    "39313135653538303437313833643732653961666662336630303764616436303763346166653430356538626233393539366135653263653932"
    "64666437643332323739656535383862393337663e202f5545203c37663462666564306531363763353937336132623761396639353266303939"
    "3564386238626562373735333466313031613438653533383663313265326630393e202f562035203e3e0a656e646f626a0a34312030206f626a"
    "0a3c3c202f54797065202f58526566202f4c656e677468203536202f46696c746572202f466c6174654465636f6465202f4465636f6465506172"
    "6d73203c3c202f436f6c756d6e732034202f507265646963746f72203132203e3e202f57205b203120322031205d202f496e666f203220302052"
    "202f526f6f74203120302052202f53697a65203432202f4944205b3c336136353434326337626337656439376561306566643535366236363061"
    "39313e3c37353030333464376163623836656565623735623931396232353636373933373e5d202f456e637279707420343020302052203e3e0a"
    "73747265616d0a789c636200022646067e06260686bd4082054830fea9027119180729f19f53f62190b503e8486609909b4104c37420c1a4ce00"
    "00c1fd072e0a656e6473747265616d0a656e646f626a0a7374617274787265660a343830360a2525454f460a";

}

// The pdfTeX sample: what pdfinfo, pdffonts and pypdf say about it, from object streams through a
// predicted cross-reference stream.
TRIAL("PDF: a pdfTeX file — object streams, a cross-reference stream, bookmarks, a link") {
    const meta::Description made = described(lump(from_pdftex));
    const meta::Document document = document_of(made);

    SAME(made.type, std::string{"application/pdf"});
    OUGHT(!made.damaged);
    SAME(document.version, std::string{"1.5"});
    SAME(document.pages, std::uint32_t{2});
    SAME(document.sheets, (std::vector<meta::Sheet>{{595.276, 841.89, 2}}));
    SAME(document.with_text, std::uint32_t{2});
    SAME(document.fonts, std::uint32_t{2});
    SAME(document.embedded, std::uint32_t{0});
    SAME(document.bookmarks, std::uint32_t{2});
    SAME(document.links, std::uint32_t{1});
    OUGHT(!document.form);
    OUGHT(!document.encrypted);
    SAME(field_of(made, "Title"), std::optional<std::string>{"Próbka"});
    SAME(field_of(made, "Producer"), std::optional<std::string>{"pdfTeX-1.40.25"});
    SAME(field_of(made, "Trapped"), std::optional<std::string>{"False"});
    SAME(made.fields.size(), std::size_t{10});
}

// The ReportLab sample: a page size per page, a title in UTF-16, a font in the resources of every
// page (ReportLab shares them), so every page counts as carrying text.
TRIAL("PDF: a ReportLab file — a classic table, three page sizes, a title in UTF-16") {
    const meta::Description made = described(lump(from_reportlab));
    const meta::Document document = document_of(made);

    OUGHT(!made.damaged);
    SAME(document.version, std::string{"1.3"});
    SAME(document.pages, std::uint32_t{3});
    SAME(document.sheets, (std::vector<meta::Sheet>{{595.2756, 841.8898, 1}, {792, 612, 1}, {200, 300, 1}}));
    SAME(document.with_text, std::uint32_t{3});
    SAME(document.fonts, std::uint32_t{1});
    SAME(document.embedded, std::uint32_t{0});
    SAME(document.bookmarks, std::uint32_t{1});
    SAME(document.links, std::uint32_t{1});
    SAME(field_of(made, "Title"), std::optional<std::string>{"ReportLab ąę"});
    SAME(field_of(made, "Author"), std::optional<std::string>{"anonymous"});
    SAME(made.fields.size(), std::size_t{9});
}

// The encrypted sample says that it is encrypted, and its version, and nothing more.
TRIAL("PDF: an encrypted file says only that it is encrypted") {
    const meta::Description made = described(lump(from_qpdf));

    meta::Document wanted;

    wanted.version = "1.7";
    wanted.encrypted = true;

    OUGHT(!made.damaged);
    SAME(document_of(made), wanted);
    OUGHT(made.fields.empty());
}

// The assembled document of four pages with a classic table: inheritance down the page tree, the
// shown size of a page, every font kind, text through a form, bookmarks counted by walking them,
// and the text strings in PDFDocEncoding, UTF-16 with a language mark, and hexadecimal.
TRIAL("PDF: a classic file — inherited sizes, rotation, crop, user unit, fonts, links, bookmarks") {
    Assembly one;

    filled(one);
    (void)one.table(" /Root 1 0 R /Info 50 0 R");

    const meta::Description made = described(one.bytes());

    OUGHT(!made.damaged);
    SAME(document_of(made), filled_document());
    SAME(made.fields, filled_fields());
}

// An incremental update: a new information dictionary and a page added under the root; the
// newer section's entries win, and the older trailer gives what the newer one leaves out.
TRIAL("PDF: an incremental update — the newer section wins, the older trailer fills in") {
    Assembly one;

    filled(one);
    (void)one.table(" /Root 1 0 R /Info 50 0 R /ID [<00><00>]");

    one.object(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 10 0 R 60 0 R] /Count 5 /MediaBox [0 0 595.276 841.89] /Resources 5 0 R >>");
    one.object(60, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 420 595] >>");
    one.object(61, "<< /Title (Nowy) >>");
    (void)one.table(" /Info 61 0 R /Root 1 0 R");

    const meta::Description made = described(one.bytes());
    const meta::Document document = document_of(made);

    OUGHT(!made.damaged);
    SAME(document.pages, std::uint32_t{5});
    SAME(document.sheets.size(), std::size_t{4});
    SAME(made.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));
}

// PDF 1.5 the way writers make it: the pages, fonts and bookmarks inside an object stream, and a
// cross-reference stream with the PNG Up predictor; then the same without the predictor.
TRIAL("PDF: an object stream and a cross-reference stream, with and without the predictor") {
    for (const bool predicted : {true, false}) {
        Assembly one{"1.5"};

        filled(one);

        one.packed(70, {{80, "<< /Type /Catalog /Pages 81 0 R /Outlines 84 0 R >>"},
                        {81, "<< /Type /Pages /Kids [82 0 R 83 0 R] /Count 2 >>"},
                        {82, "<< /Type /Page /Parent 81 0 R /MediaBox [0 0 100 200] /Resources 5 0 R >>"},
                        {83, "<< /Type /Page /Parent 81 0 R /MediaBox [0 0 100 200] >>"},
                        {84, "<< /Type /Outlines /First 85 0 R >>"},
                        {85, "<< /Title (Tylko) >>"}});

        (void)one.xref_stream(90, " /Root 80 0 R /Info 50 0 R", predicted);

        const meta::Description made = described(one.bytes());
        const meta::Document document = document_of(made);

        OUGHT(!made.damaged);
        SAME(document.version, std::string{"1.5"});
        SAME(document.pages, std::uint32_t{2});
        SAME(document.sheets, (std::vector<meta::Sheet>{{100, 200, 2}}));
        SAME(document.with_text, std::uint32_t{1});
        SAME(document.fonts, std::uint32_t{1});
        SAME(document.bookmarks, std::uint32_t{1});
        SAME(made.fields, filled_fields());
    }
}

// A hybrid file: the table marks the packed objects free for old readers, and its `/XRefStm`
// points to a stream that places them; the stream's entries come first.
TRIAL("PDF: a hybrid file — the table marks packed objects free, /XRefStm places them") {
    Assembly one{"1.5"};

    filled(one);

    one.packed(70, {{80, "<< /Type /Catalog /Pages 81 0 R >>"},
                    {81, "<< /Type /Pages /Kids [82 0 R] /Count 1 >>"},
                    {82, "<< /Type /Page /Parent 81 0 R /MediaBox [0 0 300 400] >>"}});

    std::string& text = one.text();
    const std::size_t stream = one.xref_stream(90, "", false);
    const std::size_t cut = text.rfind("startxref");

    text.resize(cut);

    std::string table = "xref\n0 1\n0000000000 65535 f\r\n80 3\n";

    for (int free = 0; free < 3; ++free) table += "0000000000 00000 f\r\n";

    const std::size_t at = text.size();

    text += table + "trailer\n<< /Size 91 /Root 80 0 R /XRefStm " + std::to_string(stream) + " >>\nstartxref\n" + std::to_string(at) + "\n%%EOF\n";

    const meta::Description made = described(bytes_of(text));
    const meta::Document document = document_of(made);

    OUGHT(!made.damaged);
    SAME(document.pages, std::uint32_t{1});
    SAME(document.sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
}

// A hybrid file with junk before its header, every offset counted from the header: the stream that
// `/XRefStm` points to is found through the shift, as the table is, and the file is whole. A
// hybrid file whose `/XRefStm` points to where no stream is is damaged, and its table still gives
// the whole document.
TRIAL("PDF: a hybrid file with junk before the header, and one whose /XRefStm points nowhere") {
    const std::string junk = "Content-Type: application/pdf\r\n\r\n";

    Assembly one{"1.5", junk};

    filled(one);

    one.packed(70, single());

    std::string& text = one.text();
    const std::size_t stream = one.xref_stream(90, "", false);

    text.resize(text.rfind("startxref"));

    const std::size_t at = text.size() - junk.size();

    text += "xref\n0 1\n0000000000 65535 f\r\n80 3\n0000000000 00000 f\r\n0000000000 00000 f\r\n0000000000 00000 f\r\ntrailer\n<< /Size 91 "
            "/Root 80 0 R /XRefStm " +
            std::to_string(stream) + " >>\nstartxref\n" + std::to_string(at) + "\n%%EOF\n";

    const meta::Description shifted = described(one.bytes());

    OUGHT(!shifted.damaged);
    SAME(document_of(shifted).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));

    Assembly nowhere;

    filled(nowhere);
    (void)nowhere.table(" /Root 1 0 R /Info 50 0 R /XRefStm 7");

    const meta::Description lost = described(nowhere.bytes());

    OUGHT(lost.damaged);
    SAME(document_of(lost), filled_document());
    SAME(lost.fields, filled_fields());
}

// An offset near 2^64 in a cross-reference stream (fields eight bytes wide), in a file with junk
// before its header: the offset is past the file, and the shift must not wrap it around to the
// start, where the junk holds an object of the same number, a catalog without pages. The place
// lies, the scan repairs it, and the catalog written after the header gives the page.
TRIAL("PDF: an offset near 2^64 does not wrap around through the shift of the header") {
    const std::string junk = "1 0 obj << /Type /Catalog /Pages 9 0 R >> endobj\n";

    Assembly one{"1.5", junk};

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    one.object(3, "<< /Type /Page /MediaBox [0 0 300 400] >>");

    std::vector<std::array<std::uint64_t, 3>> rows = one.entries(90);

    rows[1][1] = std::numeric_limits<std::uint64_t>::max() - junk.size() + 1;

    (void)one.xref_given(90, " /W [1 8 2] /Size " + std::to_string(rows.size()) + " /Root 1 0 R /Filter /FlateDecode", zlibbed(fielded(rows, {1, 8, 2})));

    const meta::Description made = described(one.bytes());

    OUGHT(made.damaged);
    SAME(document_of(made).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
}

// A cross-reference that lies, and none at all: the file is scanned, the same document comes out,
// and the description says the file is damaged. An object stream is found by the scan too.
TRIAL("PDF: a lying or missing cross-reference is rebuilt from a scan, and the file is damaged") {
    Assembly lying;

    filled(lying);
    (void)lying.table(" /Root 1 0 R /Info 50 0 R");

    std::string& text = lying.text();
    const std::size_t table = text.rfind("xref\n0 1");

    for (std::size_t at = text.find(" 00000 n", table); at != std::string::npos; at = text.find(" 00000 n", at + 1)) text[at - 1] = '3';

    const meta::Description wrong = described(bytes_of(text));

    OUGHT(wrong.damaged);
    SAME(document_of(wrong), filled_document());
    SAME(wrong.fields, filled_fields());

    std::string cut = text.substr(0, table);
    const meta::Description missing = described(bytes_of(cut + "trailer\n<< /Root 1 0 R /Info 50 0 R >>\n%%EOF\n"));

    OUGHT(missing.damaged);
    SAME(document_of(missing), filled_document());
    SAME(missing.fields, filled_fields());

    Assembly packed{"1.5"};

    packed.packed(70, {{80, "<< /Type /Catalog /Pages 81 0 R >>"},
                       {81, "<< /Type /Pages /Kids [82 0 R] /Count 1 >>"},
                       {82, "<< /Type /Page /Parent 81 0 R /MediaBox [0 0 300 400] >>"}});

    std::string streamless = packed.text() + "%%EOF\n";
    const meta::Description scanned = described(bytes_of(streamless));

    OUGHT(scanned.damaged);
    SAME(document_of(scanned).pages, std::uint32_t{1});
}

// Junk before the header, with the offsets counted from the header, as some writers make it: the
// file is read through the shift and is not damaged.
TRIAL("PDF: junk before the header shifts the offsets, and the file is whole") {
    Assembly one{"1.4", "Content-Type: application/pdf\r\n\r\n"};

    filled(one);
    (void)one.table(" /Root 1 0 R /Info 50 0 R");

    const meta::Description made = described(one.bytes());

    SAME(made.type, std::string{"application/pdf"});
    OUGHT(!made.damaged);
    SAME(document_of(made), filled_document());
}

// An encrypted file without object streams, whose pages could be read, says no more than one with
// them: that it is encrypted, and its version.
TRIAL("PDF: an encrypted file without object streams says only that it is encrypted too") {
    Assembly one;

    filled(one);
    one.object(51, "<< /Filter /Standard /V 2 /R 3 /Length 128 /O <00> /U <00> /P -1340 >>");
    (void)one.table(" /Root 1 0 R /Info 50 0 R /Encrypt 51 0 R");

    const meta::Description made = described(one.bytes());

    meta::Document wanted;

    wanted.version = "1.4";
    wanted.encrypted = true;

    SAME(document_of(made), wanted);
    OUGHT(made.fields.empty());
}

// Loops, each in a file of its own: a page tree whose node is its own kid and whose kid is its
// parent, a node that lists one page twice, and bookmarks in a ring. Each ends, every page and
// bookmark counted once, and each loop marks its file damaged.
TRIAL("PDF: loops in the page tree and the bookmarks end, and the file is damaged") {
    Assembly tree;

    tree.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    tree.object(2, "<< /Type /Pages /Kids [3 0 R 2 0 R 4 0 R] /Count 3 >>");
    tree.object(3, "<< /Type /Page /MediaBox [0 0 10 10] >>");
    tree.object(4, "<< /Type /Pages /Kids [2 0 R 3 0 R] >>");
    (void)tree.table(" /Root 1 0 R");

    const meta::Description looped = described(tree.bytes());

    OUGHT(looped.damaged);
    SAME(document_of(looped).pages, std::uint32_t{1});

    Assembly twice;

    twice.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    twice.object(2, "<< /Type /Pages /Kids [3 0 R 3 0 R] /Count 2 >>");
    twice.object(3, "<< /Type /Page /MediaBox [0 0 10 10] >>");
    (void)twice.table(" /Root 1 0 R");

    const meta::Description doubled = described(twice.bytes());

    OUGHT(doubled.damaged);
    SAME(document_of(doubled).pages, std::uint32_t{1});

    Assembly ring;

    ring.object(1, "<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>");
    ring.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    ring.object(3, "<< /Type /Page /MediaBox [0 0 10 10] >>");
    ring.object(5, "<< /First 6 0 R >>");
    ring.object(6, "<< /Title (a) /Next 7 0 R >>");
    ring.object(7, "<< /Title (b) /Next 6 0 R /First 7 0 R >>");
    (void)ring.table(" /Root 1 0 R");

    const meta::Description ringed = described(ring.bytes());
    const meta::Document document = document_of(ringed);

    OUGHT(ringed.damaged);
    SAME(document.pages, std::uint32_t{1});
    SAME(document.bookmarks, std::uint32_t{2});
}

// The limits hold against a file made to break a reader, which has no cross-reference, so all
// of it goes through the scan: arrays nested ten thousand deep, a page tree a thousand nodes deep,
// an object stream that promises a billion objects, one that says it is compressed and is not,
// and a length far past the end of the file.
TRIAL("PDF: deep nesting, a deep tree, huge counts, bad streams and lying lengths do not break the reader") {
    Assembly one{"1.5"};

    one.object(1, "<< /Type /Catalog /Pages 2 0 R /Outlines 1010 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R] >>");

    for (std::uint32_t node = 3; node < 1003; ++node) one.object(node, "<< /Type /Pages /Kids [" + std::to_string(node + 1) + " 0 R] >>");

    one.object(1003, "<< /Type /Page /MediaBox [0 0 1 1] >>");
    one.object(1004, "<< /Type /ObjStm /N 1000000000 /First 0 /Length 2 >>\nstream\nxx\nendstream");
    one.stream(1005, "/Type /ObjStm /N 1 /First 4 /Filter /FlateDecode", "1 0 not compressed at all");
    one.object(1006, "<< /Type /ObjStm /N 1 /First 4 /Length 99999999 >>\nstream\n1 0 <<>>\nendstream");
    one.object(1010, "<< /First " + std::string(10000, '[') + std::string(10000, ']') + " >>");

    const meta::Description made = described(bytes_of(one.text() + "%%EOF\n"));
    const meta::Document document = document_of(made);

    OUGHT(made.damaged);
    SAME(document.pages, std::uint32_t{0});
    SAME(document.bookmarks, std::uint32_t{0});
}

// Files made to blow up the costs of a repair, none with a cross-reference, each read within
// bounds of bytes. A
// hundred thousand words "trailer", each 64 KiB before the end, where reading them all would be six
// gigabytes, then two catalogs and a trailer that names the one of one page: the scan keeps the
// newest `most_sections` trailers (in `pdf.cpp`), the real one among them. A hundred thousand
// objects that look like cross-reference streams after a real one that names an information
// dictionary: the repair reads the newest `most_sections`, the catalog still comes, and the real
// stream, older than those, is not taken. Three hundred object streams with a wrong length and no
// "endstream", each searched for its end through eight megabytes to the end of the file: the
// searches stop at the budget of reading (`read_times`, `read_extra`), where they would read two
// and a half gigabytes, and the file is damaged.
TRIAL("PDF: repairs of files made to blow them up — trailers, streams, searches for an end") {
    const auto document = [](Assembly& one) {
        one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        one.object(3, "<< /Type /Page /MediaBox [0 0 300 400] >>");
    };

    Assembly trailers;
    std::string& words = trailers.text();

    for (int one = 0; one < 100000; ++one) words += "trailer\n";

    words += "%" + std::string(std::size_t{64} << 10, 'x') + "\n";
    document(trailers);
    trailers.object(9, "<< /Type /Catalog /Pages 10 0 R >>");
    trailers.object(10, "<< /Type /Pages /Kids [3 0 R 11 0 R] >>");
    trailers.object(11, "<< /Type /Page /MediaBox [0 0 300 400] >>");

    const Cost newest = costed(bytes_of(trailers.text() + "trailer\n<< /Root 1 0 R >>\n%%EOF\n"));

    OUGHT(!newest.hung);
    OUGHT(newest.read <= std::uint64_t{64} << 20);
    OUGHT(newest.made.damaged);
    SAME(document_of(newest.made).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));

    Assembly streams;

    document(streams);
    streams.object(4, "<< /Title (Stary) >>");
    streams.object(5, "<< /Type /XRef /Info 4 0 R /W [1 1 1] /Size 1 /Length 0 >>\nstream\n\nendstream");

    for (std::uint32_t number = 10; number < 100010; ++number) streams.object(number, "<< /Type /XRef >>");

    const Cost looked = costed(bytes_of(streams.text() + "%%EOF\n"));

    OUGHT(!looked.hung);
    OUGHT(looked.read <= std::uint64_t{64} << 20);
    OUGHT(looked.made.damaged);
    SAME(document_of(looked.made).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
    OUGHT(looked.made.fields.empty());

    Assembly endless;

    for (std::uint32_t number = 10; number < 310; ++number) endless.object(number, "<< /Type /ObjStm /N 1 /First 0 /Length 1 >>\nstream\nx");

    endless.text() += "%" + std::string(std::size_t{8} << 20, 'x') + "\n";

    const Bytes endless_bytes = endless.bytes();
    const Cost searched = costed(endless_bytes);

    OUGHT(!searched.hung);
    OUGHT(searched.read <= 5 * endless_bytes.size() + (std::uint64_t{320} << 20));
    OUGHT(searched.made.damaged);
}

// A cross-reference stream whose `/Index` names numbers twice, as an update of the classic file:
// two ranges over the same numbers, and a thousand numbers given again sixty-five thousand times
// over eight megabytes of entries, which would make two million of them. A range that overlaps
// one before it ends the entries and the file is damaged; the first range counts, the document is
// whole, and its steps stay far below those two million; a range that starts inside the one before
// it is damage too. Ranges in descending order that do not overlap are taken, and so is an empty
// range inside another.
TRIAL("PDF: cross-reference streams — ranges of /Index that overlap are damage, in any order they are not") {
    const auto updated = [](std::string_view index, const std::vector<std::array<std::uint64_t, 3>>& entries, std::size_t padding) {
        Assembly one{"1.5"};

        filled(one);
        (void)one.table(" /Root 1 0 R /Info 50 0 R");
        one.object(61, "<< /Title (Nowy) >>");

        std::vector<std::array<std::uint64_t, 3>> rows;

        for (const std::array<std::uint64_t, 3>& entry : entries) rows.push_back({entry[0], entry[1] == 61 ? one.offset(61) : one.text().size(), 0});

        (void)one.xref_given(62, " /W [0 4 0] /Index [" + std::string{index} + "] /Size 63 /Root 1 0 R /Info 61 0 R /Filter /FlateDecode",
                             zlibbed(fielded(rows, {0, 4, 0}) + std::string(padding, ' ')));

        return costed(one.bytes());
    };

    const Cost twice = updated("61 2 61 2", {{0, 61, 0}, {0, 62, 0}, {0, 61, 0}, {0, 62, 0}}, 0);

    OUGHT(twice.made.damaged);
    SAME(document_of(twice.made), filled_later());
    SAME(twice.made.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));

    std::string many = "61 2";

    for (int one = 0; one < 65535; ++one) many += " 100000 1000";

    const Cost repeated = updated(many, {{0, 61, 0}, {0, 62, 0}}, std::size_t{8} << 20);

    OUGHT(!repeated.hung);
    OUGHT(repeated.looks <= 300000);
    OUGHT(repeated.made.damaged);
    SAME(document_of(repeated.made), filled_later());

    const Cost inside = updated("61 2 62 1", {{0, 61, 0}, {0, 62, 0}, {0, 62, 0}}, 0);

    OUGHT(inside.made.damaged);
    SAME(document_of(inside.made), filled_later());

    const Cost empty = updated("61 2 62 0", {{0, 61, 0}, {0, 62, 0}}, 0);

    OUGHT(!empty.made.damaged);
    SAME(empty.made.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));

    const Cost descending = updated("62 1 61 1", {{0, 62, 0}, {0, 61, 0}}, 0);

    OUGHT(!descending.made.damaged);
    SAME(document_of(descending.made), filled_later());
    SAME(descending.made.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));
}

// Pages that share what they should not, each read once. Three hundred nodes of a page tree share
// one array of kids that lists them all eight hundred times over: the array is walked once, a node
// met again is damage, and the one page counts; walked again under every node, as deep as the tree
// may go, it would make sixty million steps. Two hundred pages share one array of five hundred
// links: the array is counted once. A hundred of them inherit resources written directly in their
// root, whose dictionary of fonts names one font four thousand times: it is gone through once,
// where once for every page would be four hundred thousand looks. The other hundred have resources
// of their own, which share by reference a dictionary of two thousand forms, all one form with a
// font: it is gone through once, where once for every page would be two hundred thousand. The
// file breaks no rule but the shared array, and is whole.
TRIAL("PDF: shared kids, shared resources and shared annotations are looked at once") {
    Assembly kids;
    std::string listed = "[";

    for (int round = 0; round < 800; ++round)
        for (std::uint32_t node = 10; node < 310; ++node) listed += std::to_string(node) + " 0 R ";

    kids.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    kids.object(2, "<< /Type /Pages /Kids 5 0 R >>");
    kids.object(5, listed + "]");
    kids.object(10, "<< /Type /Page /MediaBox [0 0 100 100] >>");

    for (std::uint32_t node = 11; node < 310; ++node) kids.object(node, "<< /Type /Pages /Kids 5 0 R >>");

    (void)kids.table(" /Root 1 0 R");

    const Cost walked = costed(kids.bytes());

    OUGHT(!walked.hung);
    OUGHT(walked.looks <= 1000000);
    OUGHT(walked.made.damaged);
    SAME(document_of(walked.made).pages, std::uint32_t{1});

    constexpr std::uint32_t many = 200;

    Assembly shared;
    std::string pages = "[";
    std::string fonts = "<<";
    std::string forms = "<<";
    std::string links = "[";

    for (std::uint32_t page = 100; page < 100 + many; ++page) pages += std::to_string(page) + " 0 R ";

    for (int font = 0; font < 4000; ++font) fonts += " /F" + std::to_string(font) + " 7 0 R";

    for (int form = 0; form < 2000; ++form) forms += " /X" + std::to_string(form) + " 13 0 R";

    for (int link = 0; link < 500; ++link) links += "8 0 R ";

    shared.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    shared.object(2, "<< /Type /Pages /Kids " + pages + "] /Count " + std::to_string(many) + " /MediaBox [0 0 100 100] /Resources << /Font " + fonts +
                         " >> >> >>");
    shared.object(7, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    shared.object(8, "<< /Type /Annot /Subtype /Link /Rect [0 0 1 1] >>");
    shared.object(9, links + "]");
    shared.object(12, forms + " >>");
    shared.stream(13, "/Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /Font << /F1 7 0 R >> >>", "BT /F1 9 Tf (x) Tj ET");

    for (std::uint32_t page = 100; page < 100 + many / 2; ++page) shared.object(page, "<< /Type /Page /Parent 2 0 R /Annots 9 0 R >>");

    for (std::uint32_t page = 100 + many / 2; page < 100 + many; ++page)
        shared.object(page, "<< /Type /Page /Parent 2 0 R /Annots 9 0 R /Resources << /XObject 12 0 R >> >>");

    (void)shared.table(" /Root 1 0 R");

    const Cost once = costed(shared.bytes());
    const meta::Document document = document_of(once.made);

    OUGHT(!once.hung);
    OUGHT(once.looks <= 20000);
    OUGHT(!once.made.damaged);
    SAME(document.pages, many);
    SAME(document.sheets, (std::vector<meta::Sheet>{{100, 100, many}}));
    SAME(document.with_text, many);
    SAME(document.fonts, std::uint32_t{1});
    SAME(document.links, std::uint32_t{500});
}

// The corners of the parser. A window that ends inside what could still become a keyword says
// `cut`, so that a larger one is read; a word that cannot become the keyword does not. An object
// number past 32 bits is no reference, and two integers without R stay two values. Containers
// nest `pdf::deepest` levels deep and no deeper, a name of 5000 bytes is too long to keep, a
// hexadecimal string with a character that is no digit is no string, a key right before ">>" has
// the null object, NUL is white space, and an escaped CR LF joins two lines of a literal string.
TRIAL("PDF: the corners of the syntax — cut keywords, references at their limits, nesting, names") {
    const auto parsed = [](std::string_view text) {
        const Bytes held = bytes_of(text);
        pdf::Syntax syntax{std::span<const std::byte>{held.data(), held.size()}};

        return syntax.object();
    };
    const auto valued = [&parsed](std::string_view text) { return parsed(text).value_or(pdf::Object{}); };
    const auto counted = [&valued](std::string_view text) {
        const pdf::Object made = valued(text);
        const pdf::Array* list = pdf::array_of(made);

        return list == nullptr ? std::size_t{0} : list->size();
    };
    const auto keyed = [](std::string_view text) {
        const Bytes held = bytes_of(text);
        pdf::Syntax syntax{std::span<const std::byte>{held.data(), held.size()}};
        const bool taken = syntax.keyword("trailer");

        return std::pair{taken, syntax.cut()};
    };

    SAME(keyed("tra"), (std::pair{false, true}));
    SAME(keyed("trx"), (std::pair{false, false}));
    SAME(keyed(" trailer "), (std::pair{true, false}));

    SAME(pdf::integer_of(valued("4294967296 0 R ")), std::optional<std::int64_t>{4294967296});
    SAME(pdf::ref_of(valued("4294967295 0 R ")), (std::optional<pdf::Ref>{pdf::Ref{4294967295U, 0}}));
    SAME(counted("[1 2 3]"), std::size_t{3});

    const std::string deep = std::string(pdf::deepest + 1, '[') + std::string(pdf::deepest + 1, ']');

    OUGHT(parsed(deep).has_value());
    OUGHT(!parsed("[" + deep + "]").has_value());
    OUGHT(!parsed("/" + std::string(5000, 'a') + " ").has_value());
    OUGHT(!parsed("<6g>").has_value());

    const std::optional<pdf::Object> keyless = parsed("<</A>>");
    const pdf::Dict* dict = keyless ? pdf::dict_of(*keyless) : nullptr;

    OUGHT(dict != nullptr && dict->entries.size() == 1 && std::holds_alternative<pdf::Null>(dict->entries[0].value.value));

    SAME(counted(std::string_view{"[1\0002]", 5}), std::size_t{2});

    const pdf::Object joined = valued("(a\\\r\nb)");

    OUGHT(pdf::string_of(joined) != nullptr && *pdf::string_of(joined) == "ab");
}

// Cross-reference tables at their edges. A table longer than its first window is read through a
// larger one. A table naming an object past the highest number accepted (`most_numbers` in
// `pdf.cpp`) is refused, and the scan repairs the file. A free entry of a newer section hides the
// older place of its object, which is then missing (the null object), and that is no damage. A
// `/Prev` to where no section is marks the file damaged, and the newer section is read all the
// same. An entry in use at offset zero is a lie: the object is missing, so the catalog it names
// is looked for by the scan.
TRIAL("PDF: cross-reference tables — longer than a window, numbers past the limit, free, /Prev, zero") {
    Assembly large;

    filled(large);

    for (std::uint32_t number = 100; number < 3700; ++number) large.object(number, "null");

    (void)large.table(" /Root 1 0 R /Info 50 0 R");

    const meta::Description longer = described(large.bytes());

    OUGHT(!longer.damaged);
    SAME(document_of(longer), filled_document());

    Assembly beyond;

    filled(beyond);
    (void)beyond.table(" /Root 1 0 R /Info 50 0 R");
    beyond.text().insert(beyond.text().rfind("trailer"), "16777300 1\n0000000015 00000 n\r\n");

    const meta::Description refused = described(beyond.bytes());

    OUGHT(refused.damaged);
    SAME(document_of(refused), filled_document());
    SAME(refused.fields, filled_fields());

    Assembly freed;

    filled(freed);

    const std::size_t older = freed.table(" /Root 1 0 R /Info 50 0 R");
    std::string& text = freed.text();
    const std::size_t newer = text.size();

    text += "xref\n50 1\n0000000000 00001 f\r\ntrailer\n<< /Size 51 /Root 1 0 R /Info 50 0 R /Prev " + std::to_string(older) + " >>\nstartxref\n" +
            std::to_string(newer) + "\n%%EOF\n";

    const meta::Description hidden = described(freed.bytes());

    OUGHT(!hidden.damaged);
    SAME(document_of(hidden), filled_document());
    OUGHT(hidden.fields.empty());

    Assembly lost;

    filled(lost);
    (void)lost.table(" /Root 1 0 R /Info 50 0 R /Prev 7");

    const meta::Description chained = described(lost.bytes());

    OUGHT(chained.damaged);
    SAME(document_of(chained), filled_document());
    SAME(chained.fields, filled_fields());

    Assembly zero;

    filled(zero);

    const std::size_t first = zero.table(" /Root 1 0 R /Info 50 0 R");
    std::string& more = zero.text();
    const std::size_t second = more.size();

    more += "xref\n63 1\n0000000000 00000 n\r\ntrailer\n<< /Size 64 /Root 63 0 R /Prev " + std::to_string(first) + " >>\nstartxref\n" +
            std::to_string(second) + "\n%%EOF\n";

    const meta::Description repaired = described(zero.bytes());

    OUGHT(repaired.damaged);
    SAME(document_of(repaired), filled_document());
    SAME(repaired.fields, filled_fields());
}

// Cross-reference streams at their edges, each an update of a classic file. One names its objects
// by `/Index` and leaves the type out of its entries (`/W` [0 4 0]: every entry in use at an
// offset); the same promising more entries than its data holds is damaged and read as far as it
// goes. One places the catalog that the trailer names at offset zero, which is a lie: the object is
// missing, and the scan finds the catalog.
TRIAL("PDF: cross-reference streams — /Index, a type of width zero, short data, offset zero") {
    const auto updated = [](std::size_t promised) {
        Assembly one{"1.5"};

        filled(one);
        (void)one.table(" /Root 1 0 R /Info 50 0 R");
        one.object(61, "<< /Title (Nowy) >>");

        const std::string rows = fielded({{0, one.offset(61), 0}, {0, one.text().size(), 0}}, {0, 4, 0});

        (void)one.xref_given(62, " /W [0 4 0] /Index [61 " + std::to_string(promised) + "] /Size 63 /Root 1 0 R /Info 61 0 R /Filter /FlateDecode",
                             zlibbed(rows));

        return described(one.bytes());
    };

    const meta::Description whole = updated(2);

    OUGHT(!whole.damaged);
    SAME(document_of(whole), filled_later());
    SAME(whole.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));

    const meta::Description short_of = updated(3);

    OUGHT(short_of.damaged);
    SAME(document_of(short_of), filled_later());
    SAME(short_of.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));

    Assembly zero{"1.5"};

    filled(zero);
    (void)zero.table(" /Root 1 0 R /Info 50 0 R");
    (void)zero.xref_given(62, " /W [1 4 2] /Index [63 1] /Size 64 /Root 63 0 R /Filter /FlateDecode", zlibbed(fielded({{1, 0, 0}}, {1, 4, 2})));

    const meta::Description repaired = described(zero.bytes());

    OUGHT(repaired.damaged);
    SAME(document_of(repaired), filled_later());
    SAME(repaired.fields, filled_fields());
}

// Object and cross-reference streams the way some writers vary them: the object stream's filter
// abbreviated (/Fl), "stream" ended by CR LF, and a length that counts the end of line before
// "endstream"; the cross-reference stream's filter and its parameters in arrays, and the index of
// every packed object but the first wrong, so that the object is looked for in its stream.
TRIAL("PDF: streams as writers vary them — /Fl, CR LF, a length with its end of line, wrong indexes") {
    Assembly one{"1.5"};

    filled(one);

    const Packing pack = packing(single());
    const std::string zipped = zlibbed(pack.data);

    one.object(70, "<< /Type /ObjStm /N 3 /First " + std::to_string(pack.first) + " /Filter /Fl /Length " + std::to_string(zipped.size() + 2) +
                       " >>\nstream\r\n" + zipped + "\r\nendstream");

    for (const std::uint32_t number : {80U, 81U, 82U}) one.inside(number, 70, 0);

    const std::vector<std::array<std::uint64_t, 3>> rows = one.entries(90);

    (void)one.xref_given(90,
                         " /W [1 4 2] /Size " + std::to_string(rows.size()) +
                             " /Root 80 0 R /Info 50 0 R /Filter [/FlateDecode] /DecodeParms [<< /Predictor 12 /Columns 7 >>]",
                         zlibbed(png(fielded(rows, {1, 4, 2}), 7, {2})));

    const meta::Description made = described(one.bytes());
    const meta::Document document = document_of(made);

    OUGHT(!made.damaged);
    SAME(document.pages, std::uint32_t{1});
    SAME(document.sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
    SAME(made.fields, filled_fields());
}

// A window of the file that ends right after the CR of "stream" says `cut`, since the LF may
// follow, and so does one that ends where the end of the line should be. An object stream whose
// dictionary is padded so that its first window (`first_window` in `pdf.cpp`, 4096 bytes from
// "70 0 obj") ends there, with the LF of CR LF beyond it: the object is read again through a
// larger window, its data starts after the LF, and the file is whole.
TRIAL("PDF: a window that ends after the CR of \"stream\" is read again for the LF") {
    const Bytes held = bytes_of("<<>> stream\r");
    pdf::Syntax syntax{std::span<const std::byte>{held.data(), held.size()}};

    OUGHT(syntax.object().has_value() && syntax.keyword("stream") && !syntax.cut());
    OUGHT(syntax.eol() && syntax.cut());

    pdf::Syntax ended{std::span<const std::byte>{held.data(), held.size()}, held.size()};

    OUGHT(!ended.eol() && ended.cut());

    const std::string zipped = zlibbed(packing(single()).data);
    const std::string before = "<< /Type /ObjStm /N 3 /First " + std::to_string(packing(single()).first) + " /Filter /FlateDecode /Length " +
                               std::to_string(zipped.size()) + " /Pad (";
    const std::string after = ") >>\nstream\r";
    const std::string dict = before + std::string(4096 - std::string{"70 0 obj\n"}.size() - before.size() - after.size(), 'x') + after;
    const meta::Description made = single_packed(dict + "\n" + zipped + "\nendstream", true);

    OUGHT(!made.damaged);
    SAME(document_of(made).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
}

// The predictors a stream may carry (ISO 32000-2, 7.4.4.4). The object stream is in rows of two
// bytes behind the PNG filters by turns (Sub, Paeth, Average, Up, None), after a comment whose
// bytes make Paeth's distances to up and to the corner equal, where up wins; the cross-reference
// stream is behind TIFF predictor 2.
TRIAL("PDF: the predictors — PNG filters by rows, a tie in Paeth's guess, and TIFF's") {
    Assembly one{"1.5"};

    filled(one);

    const auto filtered = [](std::string_view data) { return zlibbed(png(data, 2, {1, 4, 3, 2, 0})); };

    one.packed(70, single(), filtered, " /Filter /FlateDecode /DecodeParms << /Predictor 15 /Columns 2 >>", "%#&x\n");

    const std::vector<std::array<std::uint64_t, 3>> rows = one.entries(90);

    (void)one.xref_given(
        90, " /W [1 4 2] /Size " + std::to_string(rows.size()) + " /Root 80 0 R /Info 50 0 R /Filter /FlateDecode /DecodeParms << /Predictor 2 /Columns 7 >>",
        zlibbed(tiff(fielded(rows, {1, 4, 2}), 7)));

    const meta::Description made = described(one.bytes());
    const meta::Document document = document_of(made);

    OUGHT(!made.damaged);
    SAME(document.pages, std::uint32_t{1});
    SAME(document.sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
    SAME(made.fields, filled_fields());
}

// Object streams whose length lies or whose data is damaged, each holding a document of one page.
// A length too short is found out by the "endstream" missing after it, and the data is read up to
// the "endstream" that ends it, which is damage; also when that word falls across two reads of
// the search. A wrong Adler-32 gives the data before the check, and the file is damaged; so does
// a table of objects shorter than `/N` promises.
TRIAL("PDF: object streams whose length lies, whose check is wrong, whose table is short") {
    const std::string zipped = zlibbed(packing(single()).data);

    const meta::Description shorter = single_packed(object_stream(zipped, 20), true);

    OUGHT(shorter.damaged);
    SAME(document_of(shorter).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));

    std::string padded = packing(single()).data;

    padded.resize(65520, ' ');

    const std::string across = zlibbed(padded);

    OUGHT(across.size() + 5 == std::size_t{64} << 10);

    const meta::Description straddled = single_packed(object_stream(across, 1), false);

    OUGHT(straddled.damaged);
    SAME(document_of(straddled).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));

    const meta::Description summed = single_packed(object_stream(wrongly_summed(packing(single()).data), zipped.size()), true);

    OUGHT(summed.damaged);
    SAME(document_of(summed).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));

    const meta::Description promised = single_packed(object_stream(zipped, zipped.size(), " /Filter /FlateDecode", 4), true);

    OUGHT(promised.damaged);
    SAME(document_of(promised).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
}

// An object stream whose zlib data is cut in half, in the middle of its fourth object, a long
// string after the document of one page: what came out before the cut is kept, the three objects
// of the page are read from it, and the file is damaged.
TRIAL("PDF: an object stream cut in half gives the objects before the cut") {
    std::vector<std::pair<std::uint32_t, std::string>> objects = single();

    objects.emplace_back(83, "(" + std::string(300, 'x') + ")");

    const Packing pack = packing(objects);
    const std::string zipped = zlibbed(pack.data);
    const std::string half = zipped.substr(0, zipped.size() / 2);

    OUGHT(half.size() > 7 + pack.data.find("(xxx") && half.size() < 7 + pack.data.size());

    const meta::Description made = single_packed("<< /Type /ObjStm /N 4 /First " + std::to_string(pack.first) + " /Filter /FlateDecode /Length " +
                                                     std::to_string(half.size()) + " >>\nstream\n" + half + "\nendstream",
                                                 true);

    OUGHT(made.damaged);
    SAME(document_of(made).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
}

// The limits of decoding hold: eight filters in a chain are undone, nine are not
// (`most_filters`), and a stream that unpacks past `most_packet` is no data at all, while one
// made the same way that stays below it is read. Without its object stream a document has no
// pages.
TRIAL("PDF: a chain of nine filters and a stream that unpacks past the limit are no data") {
    const auto layered = [](std::size_t many) {
        std::string data = packing(single()).data;
        std::string filter = " /Filter [";

        for (std::size_t one = 0; one < many; ++one) {
            data = zlibbed(data);
            filter += "/FlateDecode ";
        }

        return single_packed(object_stream(data, data.size(), filter + "]"), true);
    };

    const meta::Description eight = layered(8);

    OUGHT(!eight.damaged);
    SAME(document_of(eight).pages, std::uint32_t{1});
    SAME(document_of(layered(9)).pages, std::uint32_t{0});

    const auto bombed = [](std::size_t matches) {
        const std::string data = bomb(packing(single()).data, matches);

        return document_of(single_packed(object_stream(data, data.size()), false));
    };

    SAME(bombed(1000).pages, std::uint32_t{1});
    SAME(bombed(meta::tagging::most_packet / 258 + 1).pages, std::uint32_t{0});
}

// What the streams of a file unpack to together has a budget (`unpack_times` and `unpack_extra`,
// four times the file and `most_packet` more), since the objects of every object stream stay in
// memory. A catalog and its pages node written directly, and each of three pages in
// an object stream of its own that unpacks to thirty megabytes, whole and checked: two such
// streams fit in the budget, and the file of two pages is whole; of three the last is no data, its
// page is missing, and the file is damaged, where a thousand such streams in a file of a third of
// a megabyte each would be sixty-four gigabytes.
TRIAL("PDF: what the streams of a file unpack to together has a budget, and a stream past it is no data") {
    const std::size_t matches = (std::size_t{30} << 20) / 258;

    const auto paged = [matches](std::uint32_t many) {
        Assembly one{"1.5"};
        std::string kids;

        for (std::uint32_t page = 0; page < many; ++page) kids += std::to_string(80 + page) + " 0 R ";

        one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        one.object(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(many) + " >>");

        for (std::uint32_t page = 0; page < many; ++page)
            one.packed(70 + page, {{80 + page, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>"}},
                       [matches](std::string_view data) { return bomb(data, matches, true); });

        (void)one.xref_stream(90, " /Root 1 0 R", false);

        return described(one.bytes());
    };

    const meta::Description two = paged(2);

    OUGHT(!two.damaged);
    SAME(document_of(two).pages, std::uint32_t{2});

    const meta::Description three = paged(3);

    OUGHT(three.damaged);
    SAME(document_of(three).pages, std::uint32_t{2});
}

// The slots of an object stream, objects 84 and 85 in it and the information dictionary one of
// them. A table that lists them backwards, offsets falling, is read as they lie, each up to the
// one after it: the dictionary 84 gives its title through 85, and the file is whole. The second of
// two objects at one offset is no object, where without slots it would be the first read again,
// as many times as the table names that offset; the table is damage even when only the first is
// wanted, and so is an offset at the end of the objects. An object whose offset falls inside
// the one before cuts that one short, which is then no object either, where without slots the same
// text would be read as two objects.
TRIAL("PDF: object streams — a table backwards is read, an offset shared or inside another object is damage") {
    const auto tabled = [](std::string_view table, std::string_view body, std::uint32_t info) {
        Assembly one{"1.5"};
        const std::string head = std::string{table} + "\n";

        one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        one.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>");
        one.stream(70, "/Type /ObjStm /N 2 /First " + std::to_string(head.size()) + " /Filter /FlateDecode", zlibbed(head + std::string{body}));
        one.inside(84, 70, 0);
        one.inside(85, 70, 1);

        (void)one.xref_stream(90, " /Root 1 0 R /Info " + std::to_string(info) + " 0 R", false);

        return described(one.bytes());
    };

    const std::string referring = "<< /Title 85 0 R >>\n";
    const meta::Description backwards = tabled("85 " + std::to_string(referring.size()) + " 84 0", referring + "(Jeden)\n", 84);

    OUGHT(!backwards.damaged);
    SAME(backwards.fields, (std::vector<meta::Field>{info("Title", "Jeden")}));
    SAME(document_of(backwards).pages, std::uint32_t{1});

    const std::string title = "<< /Title (Jeden) >>\n";
    const meta::Description shared = tabled("84 0 85 0", title, 85);

    OUGHT(shared.damaged);
    OUGHT(shared.fields.empty());

    const meta::Description unwanted = tabled("84 0 85 0", title, 84);

    OUGHT(unwanted.damaged);
    SAME(unwanted.fields, (std::vector<meta::Field>{info("Title", "Jeden")}));

    const meta::Description past = tabled("84 0 85 " + std::to_string(title.size()), title, 84);

    OUGHT(past.damaged);
    SAME(past.fields, (std::vector<meta::Field>{info("Title", "Jeden")}));

    const std::string both = "<< /Title (Jeden) /Subject (Dwa) >>\n";
    const meta::Description inside = tabled("84 0 85 " + std::to_string(both.find("(Dwa)")), both, 84);

    OUGHT(inside.damaged);
    OUGHT(inside.fields.empty());
    SAME(document_of(inside).pages, std::uint32_t{1});
}

// A hybrid file of three sections, each naming the same `/XRefStm`, a stream whose entries are
// followed by thirty megabytes of padding, whole and checked: the stream is read once, and the file
// is whole. Read for every section, three times thirty megabytes would go past the budget of what
// a file's streams unpack to, and five hundred sections would be fifteen gigabytes.
TRIAL("PDF: the /XRefStm that sections name again is read once") {
    Assembly one{"1.5"};

    filled(one);

    const std::vector<std::array<std::uint64_t, 3>> rows = one.entries(90);

    one.stream(90, "/Type /XRef /W [1 4 2] /Size " + std::to_string(rows.size()) + " /Filter /FlateDecode",
               bomb(fielded(rows, {1, 4, 2}), (std::size_t{30} << 20) / 258, true));

    const std::string trailer = " /Root 1 0 R /Info 50 0 R /XRefStm " + std::to_string(one.offset(90));

    for (int section = 0; section < 3; ++section) (void)one.table(trailer);

    const meta::Description made = described(one.bytes());

    OUGHT(!made.damaged);
    SAME(document_of(made), filled_later());
    SAME(made.fields, filled_fields());
}

// A trailer of a hundred and twenty thousand keys besides its catalog and information dictionary,
// and an update after it: only the keys the reading needs are kept, so merging the trailers takes
// a step for each key and for each comparison with a key kept, where keeping them all would make
// seven billion comparisons; the file is whole.
TRIAL("PDF: a trailer of a hundred and twenty thousand keys is merged in linear steps") {
    Assembly one;
    std::string keys;

    filled(one);

    for (int key = 0; key < 120000; ++key) keys += " /k" + std::to_string(1000000 + key) + " 0";

    (void)one.table(" /Root 1 0 R /Info 50 0 R" + keys);
    one.object(60, "<< /Title (Nowy) >>");
    (void)one.table(" /Info 60 0 R");

    const Cost merged = costed(one.bytes());

    OUGHT(!merged.hung);
    OUGHT(merged.looks <= 250000);
    OUGHT(!merged.made.damaged);
    SAME(document_of(merged.made), filled_document());
    SAME(merged.made.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));
}

// Repairs by the scan. An incremental update whose last "startxref" lies: the newer trailer wins.
// PDF 1.5 whose "startxref" lies: the cross-reference stream is the trailer, and an object written
// directly after it wins over the same number in an object stream. A table entry that sends the
// catalog to another object, and one that leaves the catalog out: the scan finds it. A trailer
// too long for the scan's window is still the trailer that the chain read.
TRIAL("PDF: repairs — the newer trailer, a stream as trailer, direct before packed, the chain's trailer") {
    Assembly update;

    filled(update);
    (void)update.table(" /Root 1 0 R /Info 50 0 R");
    update.object(61, "<< /Title (Nowy) >>");
    (void)update.table(" /Root 1 0 R /Info 61 0 R");

    std::string& text = update.text();
    const std::size_t last = text.rfind("startxref\n") + 10;

    text.replace(last, text.find('\n', last) - last, "1");

    const meta::Description newest = described(update.bytes());

    OUGHT(newest.damaged);
    SAME(document_of(newest), filled_document());
    SAME(newest.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));

    Assembly streamed{"1.5"};

    filled(streamed);
    streamed.packed(70, single());
    (void)streamed.xref_stream(90, " /Root 80 0 R /Info 50 0 R", true);
    streamed.object(81, "<< /Type /Pages /Kids [82 0 R 3 0 R] /Count 2 >>");

    std::string& later = streamed.text();
    const std::size_t pointed = later.rfind("startxref\n") + 10;

    later.replace(pointed, later.find('\n', pointed) - pointed, "1");

    const meta::Description direct = described(bytes_of(later + "%%EOF\n"));

    OUGHT(direct.damaged);
    SAME(document_of(direct).pages, std::uint32_t{2});
    SAME(direct.fields, filled_fields());

    const auto entry = [](Assembly& one, std::uint32_t number, std::size_t at) {
        std::string& all = one.text();
        const std::string was = ten(one.offset(number)) + " 00000 n";

        all.replace(all.find(was, all.rfind("xref\n0 1")), was.size(), ten(at) + " 00000 n");
    };

    Assembly other;

    filled(other);
    (void)other.table(" /Root 1 0 R /Info 50 0 R");
    entry(other, 1, other.offset(9));

    const meta::Description elsewhere = described(other.bytes());

    OUGHT(elsewhere.damaged);
    SAME(document_of(elsewhere), filled_document());
    SAME(elsewhere.fields, filled_fields());

    Assembly left_out;

    filled(left_out);
    (void)left_out.table(" /Root 1 0 R /Info 50 0 R");

    std::string& table = left_out.text();
    const std::size_t run = table.find("\n1 23\n");

    table.replace(run, 6 + 20, "\n2 22\n");

    const meta::Description missing = described(left_out.bytes());

    OUGHT(missing.damaged);
    SAME(document_of(missing), filled_document());
    SAME(missing.fields, filled_fields());

    Assembly padded;

    filled(padded);
    (void)padded.table(" /Root 1 0 R /Info 50 0 R /Pad (" + std::string(70000, 'x') + ")");
    entry(padded, 1, padded.offset(1) - 2);

    const meta::Description kept = described(padded.bytes());

    OUGHT(kept.damaged);
    SAME(document_of(kept), filled_document());
    SAME(kept.fields, filled_fields());
}

// Repairs that choose a catalog. A file without a cross-reference holds two catalogs, of one page
// and of two: a trailer whose root is a catalog keeps it; a root that is not there, or no trailer
// at all, gives the catalog of the highest number.
TRIAL("PDF: repairs — the trailer's catalog first, otherwise the catalog of the highest number") {
    const auto catalogs = [](std::string_view trailer) {
        Assembly one;

        one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R] >>");
        one.object(3, "<< /Type /Page /MediaBox [0 0 100 100] >>");
        one.object(9, "<< /Type /Catalog /Pages 10 0 R >>");
        one.object(10, "<< /Type /Pages /Kids [3 0 R 11 0 R] >>");
        one.object(11, "<< /Type /Page /MediaBox [0 0 100 100] >>");

        return document_of(described(bytes_of(one.text() + std::string{trailer} + "%%EOF\n")));
    };

    SAME(catalogs("trailer\n<< /Root 1 0 R >>\n").pages, std::uint32_t{1});
    SAME(catalogs("trailer\n<< /Root 77 0 R >>\n").pages, std::uint32_t{2});
    SAME(catalogs("").pages, std::uint32_t{2});
}

// A repair takes the trailers and the cross-reference streams in the order they stand in the
// file, the newest first: a file of PDF 1.4 updated by a writer of PDF 1.5, with a new information
// dictionary named by the update's cross-reference stream, whose "startxref" lies. The stream's
// dictionary, the newer one, wins over the older classic trailer.
TRIAL("PDF: repairs — a cross-reference stream newer than a classic trailer wins") {
    Assembly one;

    filled(one);
    (void)one.table(" /Root 1 0 R /Info 50 0 R");
    one.object(61, "<< /Title (Nowy) >>");

    const std::string rows = fielded({{0, one.offset(61), 0}, {0, one.text().size(), 0}}, {0, 4, 0});

    (void)one.xref_given(62, " /W [0 4 0] /Index [61 2] /Size 63 /Root 1 0 R /Info 61 0 R /Filter /FlateDecode", zlibbed(rows));

    std::string& text = one.text();
    const std::size_t pointed = text.rfind("startxref\n") + 10;

    text.replace(pointed, text.find('\n', pointed) - pointed, "1");

    const meta::Description made = described(one.bytes());

    OUGHT(made.damaged);
    SAME(document_of(made), filled_document());
    SAME(made.fields, (std::vector<meta::Field>{info("Title", "Nowy")}));
}

// A repair that begins while the root itself is being read: an update names as the root object 63,
// which it places where object 2 stands. Reading the root finds the lie and repairs the file, which
// names the catalog the scan found; the root is then looked for again, and the document is whole.
TRIAL("PDF: repairs — a repair begun while reading the root gives the catalog it found") {
    Assembly one;

    filled(one);

    const std::size_t first = one.table(" /Root 1 0 R /Info 50 0 R");
    std::string& text = one.text();
    const std::size_t second = text.size();

    text += "xref\n63 1\n" + ten(one.offset(2)) + " 00000 n\r\ntrailer\n<< /Size 64 /Root 63 0 R /Prev " + std::to_string(first) + " >>\nstartxref\n" +
            std::to_string(second) + "\n%%EOF\n";

    const meta::Description made = described(one.bytes());

    OUGHT(made.damaged);
    SAME(document_of(made), filled_document());
}

// Repairs of places that lie about object streams. A table sends an object kept in an object stream
// to where there is nothing: after the scan, the stream's index places it. A cross-reference
// stream places the object stream itself a byte off: it is read again after the scan.
TRIAL("PDF: repairs — a packed object placed where nothing is, an object stream placed a byte off") {
    Assembly stale{"1.5"};

    stale.packed(70, single());

    const std::size_t older = stale.table(" /Root 80 0 R");
    std::string& text = stale.text();
    const std::size_t newer = text.size();

    text += "xref\n80 1\n0000099999 00000 n\r\ntrailer\n<< /Size 83 /Root 80 0 R /Prev " + std::to_string(older) + " >>\nstartxref\n" +
            std::to_string(newer) + "\n%%EOF\n";

    const meta::Description placed = described(stale.bytes());

    OUGHT(placed.damaged);
    SAME(document_of(placed).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));

    Assembly off{"1.5"};

    off.packed(70, single());

    std::vector<std::array<std::uint64_t, 3>> rows = off.entries(90);

    ++rows[70][1];

    (void)off.xref_given(90, " /W [1 4 2] /Size " + std::to_string(rows.size()) + " /Root 80 0 R /Filter /FlateDecode", zlibbed(fielded(rows, {1, 4, 2})));

    const meta::Description again = described(off.bytes());

    OUGHT(again.damaged);
    SAME(document_of(again).sheets, (std::vector<meta::Sheet>{{300, 400, 1}}));
}

// The scan reads a large file in chunks of a megabyte that overlap, and finds every word once
// wherever it falls: "obj" and "trailer" starting at every offset where a chunk could lose them,
// or lose the number before "obj", around the middle of the overlap and the end of the first
// chunk (`scan_chunk` and `scan_overlap` in `pdf.cpp`). Every file has no cross-reference. One
// whose catalog's "obj" is found has a page; one whose trailer is found takes the catalog it names,
// of one page, rather than the one of the higher number, of two. A word that the end of the first
// chunk cuts is judged with the byte after it: "1 0 objx" there is no object, and does not take
// the place of the catalog.
TRIAL("PDF: the scan finds every word at every offset around the border of its chunks") {
    constexpr std::size_t border = std::size_t{1} << 20;

    const auto padded = [](Assembly& one, std::size_t to) {
        std::string& text = one.text();

        text += "%" + std::string(to - text.size() - 2, 'x') + "\n";
    };

    for (const std::size_t at : {border - 513, border - 512, border - 258, border - 257, border - 256, border - 255, border - 4, border - 3,
                                 border - 2, border - 1, border}) {
        Assembly one;

        padded(one, at - 4);
        one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        one.object(3, "<< /Type /Page /MediaBox [0 0 100 100] >>");

        OUGHT(one.text().compare(at, 3, "obj") == 0);

        const meta::Description made = described(bytes_of(one.text() + "%%EOF\n"));

        OUGHT(made.damaged);
        SAME(document_of(made).pages, std::uint32_t{1});
    }

    for (const std::size_t at : {border - 258, border - 257, border - 256, border - 255, border - 8, border - 7, border - 6, border - 1, border}) {
        Assembly one;

        one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R] >>");
        one.object(3, "<< /Type /Page /MediaBox [0 0 100 100] >>");
        one.object(9, "<< /Type /Catalog /Pages 10 0 R >>");
        one.object(10, "<< /Type /Pages /Kids [3 0 R 11 0 R] >>");
        one.object(11, "<< /Type /Page /MediaBox [0 0 100 100] >>");
        padded(one, at);

        const meta::Description made = described(bytes_of(one.text() + "trailer\n<< /Root 1 0 R >>\n%%EOF\n"));

        OUGHT(made.damaged);
        SAME(document_of(made).pages, std::uint32_t{1});
    }

    Assembly cut;

    cut.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    cut.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    cut.object(3, "<< /Type /Page /MediaBox [0 0 100 100] >>");
    padded(cut, border - 3 - 4);
    cut.text() += "1 0 objx\n%%EOF\n";

    OUGHT(cut.text().compare(border - 3, 4, "objx") == 0);

    SAME(document_of(described(cut.bytes())).pages, std::uint32_t{1});
}

// The number before "obj" is looked for at most half the overlap back (`scan_overlap` in
// `pdf.cpp`), wherever the word lies, so that no chunk takes a number cut by its start: "123456 0",
// 251 spaces and "obj", the number across the start of the second chunk. Taken from there, "456"
// would place the catalog of the file, object 456, where that other object stands.
TRIAL("PDF: the scan does not take a number cut by the start of a chunk") {
    constexpr std::size_t border = std::size_t{1} << 20;
    constexpr std::size_t second = border - 512;

    Assembly one;

    one.object(456, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    one.object(3, "<< /Type /Page /MediaBox [0 0 100 100] >>");

    std::string& text = one.text();

    text += "%" + std::string(second - 3 - text.size() - 2, 'x') + "\n";
    text += "123456 0" + std::string(251, ' ') + "obj\n<< >>\nendobj\n%" + std::string(600, 'x') + "\n%%EOF\n";

    OUGHT(text.compare(second - 3, 6, "123456") == 0 && text.compare(border - 256, 3, "obj") == 0);

    const meta::Description made = described(one.bytes());

    OUGHT(made.damaged);
    SAME(document_of(made).pages, std::uint32_t{1});
}

// The walk never repairs: once the structure is read, a page whose place lies is left out, and the
// trailer and the places stay as they were; the file is damaged all the same.
TRIAL("PDF: a place that lies about a page is not repaired during the walk") {
    Assembly one;

    filled(one);
    (void)one.table(" /Root 1 0 R /Info 50 0 R");

    std::string& text = one.text();
    const std::string was = ten(one.offset(4)) + " 00000 n";

    text.replace(text.find(was, text.rfind("xref\n0 1")), was.size(), ten(one.offset(5)) + " 00000 n");

    const meta::Description made = described(one.bytes());

    OUGHT(made.damaged);
    SAME(document_of(made).pages, std::uint32_t{3});
}

// One object longer than the first window of a read, a reference that leads to another, and a
// value of the information dictionary that is a reference: the object is read through a larger
// window, the chain is followed, and the value is resolved.
TRIAL("PDF: an object past its first window, a reference to a reference, a value by reference") {
    Assembly one;

    filled(one);
    one.object(60, "<< /Title (" + std::string(10000, 'a') + ") /Author 62 0 R >>");
    one.object(61, "60 0 R");
    one.object(62, "(Ktos)");
    (void)one.table(" /Root 1 0 R /Info 61 0 R");

    const meta::Description made = described(one.bytes());

    OUGHT(!made.damaged);
    SAME(made.fields, (std::vector<meta::Field>{info("Title", std::string(10000, 'a')), info("Author", "Ktos")}));
}

// The page tree as writers bend it: six pages under a root that gives them its resources, a font
// written directly in them. A page with neither a type nor kids; a crop box outside the media box
// (the media box counts); one reaching past it (cut to it); a user unit of zero (ignored) with a
// rotation of -90 degrees (a quarter turn); a size that overflows (no sheet, the page counts); a
// page whose font dictionary is empty (no text). The font shared by five pages counts once. A
// catalog's `/Version` above the header's wins, one below does not.
TRIAL("PDF: the page tree as writers bend it — untyped pages, odd boxes, units and turns") {
    const auto made = [](std::string_view version) {
        Assembly one;

        one.object(1, "<< /Type /Catalog /Pages 2 0 R /Version /" + std::string{version} + " >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R 8 0 R] /Count 6 "
                      "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> >>");
        one.object(3, "<< /MediaBox [0 0 100 100] >>");
        one.object(4, "<< /Type /Page /MediaBox [0 0 100 200] /CropBox [500 500 600 600] >>");
        one.object(5, "<< /Type /Page /MediaBox [0 0 100 300] /CropBox [-50 -50 50 400] >>");
        one.object(6, "<< /Type /Page /MediaBox [0 0 100 400] /UserUnit 0 /Rotate -90 >>");
        one.object(7, "<< /Type /Page /MediaBox [0 0 1" + std::string(200, '0') + " 1] /UserUnit 1" + std::string(200, '0') + " >>");
        one.object(8, "<< /Type /Page /MediaBox [0 0 100 500] /Resources << /Font << >> >> >>");
        (void)one.table(" /Root 1 0 R");

        return described(one.bytes());
    };

    meta::Document wanted;

    wanted.version = "1.4";
    wanted.pages = 6;
    wanted.sheets = {{100, 100, 1}, {100, 200, 1}, {50, 300, 1}, {400, 100, 1}, {100, 500, 1}};
    wanted.with_text = 5;
    wanted.fonts = 1;

    const meta::Description lower = made("1.3");

    OUGHT(!lower.damaged);
    SAME(document_of(lower), wanted);
    SAME(document_of(made("1.7")).version, std::string{"1.7"});
}

// A page tree deeper than the walk goes (`tree_depth` in `pdf.cpp`) is damage, and its page is
// not reached; a tree whose kids cannot be read gives the count its root says.
TRIAL("PDF: a page tree too deep is damage, and one that cannot be walked gives its /Count") {
    Assembly deep;

    deep.object(1, "<< /Type /Catalog /Pages 2 0 R >>");

    for (std::uint32_t node = 2; node < 302; ++node) deep.object(node, "<< /Type /Pages /Kids [" + std::to_string(node + 1) + " 0 R] >>");

    deep.object(302, "<< /Type /Page >>");
    (void)deep.table(" /Root 1 0 R");

    const meta::Description deepest = described(deep.bytes());

    OUGHT(deepest.damaged);
    SAME(document_of(deepest).pages, std::uint32_t{0});

    Assembly counted;

    counted.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    counted.object(2, "<< /Type /Pages /Kids [9 0 R] /Count 5 >>");
    (void)counted.table(" /Root 1 0 R");

    const meta::Description told = described(counted.bytes());

    OUGHT(!told.damaged);
    SAME(document_of(told).pages, std::uint32_t{5});
}

// The font kinds the classic file lacks and forms at their edges: a Type 3 font is drawn by the
// document, so it is embedded, and a Type 1 font with `/FontFile` is embedded; an interactive form
// whose `/Fields` are empty is no form, and one of XFA alone is one.
TRIAL("PDF: Type 3 fonts and /FontFile are embedded; empty fields are no form, XFA is one") {
    const auto made = [](std::string_view form) {
        Assembly one;

        one.object(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm " + std::string{form} + " >>");
        one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        one.object(3, "<< /Type /Page /Resources << /Font << /F1 4 0 R /F2 5 0 R >> >> >>");
        one.object(4, "<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1 1] /FontMatrix [1 0 0 1 0 0] /CharProcs << >> /Encoding << >> >>");
        one.object(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Lato /FontDescriptor 6 0 R >>");
        one.object(6, "<< /Type /FontDescriptor /FontName /Lato /FontFile 7 0 R >>");
        one.stream(7, "", "font");
        (void)one.table(" /Root 1 0 R");

        return document_of(described(one.bytes()));
    };

    const meta::Document empty = made("<< /Fields [] >>");

    SAME(empty.fonts, std::uint32_t{2});
    SAME(empty.embedded, std::uint32_t{2});
    OUGHT(!empty.form);
    OUGHT(made("<< /XFA [(template) 8 0 R] >>").form);
}

// A cut or a flipped byte anywhere in the samples gives a description, never a crash or a hang;
// the pdfTeX sample cut in the middle is still seen as PDF.
TRIAL("PDF: every truncation and every flipped byte of the samples") {
    for (const std::string_view hex : {from_pdftex, from_reportlab, from_qpdf}) {
        const Bytes whole = lump(hex);

        for (std::size_t length = 0; length < whole.size(); length += 7) (void)described(Bytes(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length)));

        for (std::size_t at = 0; at < whole.size(); at += 3) {
            Bytes flipped = whole;

            flipped[at] ^= std::byte{0xFF};

            (void)described(flipped);
        }
    }

    const Bytes half = lump(from_pdftex.substr(0, from_pdftex.size() / 2));

    SAME(described(half).type, std::string{"application/pdf"});
}

// PDF reads its windows by itself, through the reader, not through `Walk`; the least a `need`
// loads is shrunk to a byte all the same, because the recognition before it (ID3v2, APE) goes
// through `Walk`, so that a read no `need` covered stops a debug build. Every sample, the
// assembled classic file among them, whole and cut short, must read alike either way.
TRIAL("PDF: with a window of a byte every sample reads alike") {
    Assembly classic;

    filled(classic);
    (void)classic.table(" /Root 1 0 R /Info 50 0 R");

    std::size_t tried = 0;

    for (const Bytes& whole : {lump(from_pdftex), lump(from_reportlab), lump(from_qpdf), classic.bytes()}) {
        for (std::size_t much = whole.size(); much > 0; much = much > 64 ? much - much / 8 : much - 1) {
            const Bytes one(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(much));
            const meta::Description wide = described(one);
            const Narrowed narrow{1};

            OUGHT(described(one) == wide);

            ++tried;
        }
    }

    OUGHT(tried > 300);
}

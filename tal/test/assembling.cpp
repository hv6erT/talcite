#include "assembling.hpp"

#include <algorithm>
#include <cstdlib>

namespace tal::testing {

// Bytes from a string of lower-case hexadecimal digits, the form in which the samples are kept.
Bytes lump(std::string_view hex) {
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

// The bytes in the zlib format, made of stored (uncompressed) DEFLATE blocks, with a correct
// Adler-32: what a writer that does not compress would make, and enough for the inflater.
std::string zlibbed(std::string_view given) {
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

// The rows of a cross-reference stream: the three fields of every entry, high byte first, each as
// wide as `widths` says.
std::string fielded(const std::vector<std::array<std::uint64_t, 3>>& entries, const std::array<int, 3>& widths) {
    std::string made;

    for (const std::array<std::uint64_t, 3>& entry : entries)
        for (std::size_t field = 0; field < 3; ++field)
            for (int byte = widths[field] - 1; byte >= 0; --byte) made.push_back(static_cast<char>((entry[field] >> (8 * byte)) & 0xFF));

    return made;
}

// The text in rows of `columns` bytes, each behind the PNG filter of its turn in `filters` and
// its number (ISO 32000-2, 7.4.4.4, as PNG defines them): None, Sub, Up, Average and Paeth.
std::string png(std::string_view text, std::size_t columns, const std::vector<int>& filters) {
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

// The objects as an object stream holds them (ISO 32000-2, 7.5.7): after `lead` (white space or a
// comment), the pairs of number and offset, then the objects, each on a line of its own; and where
// the objects begin (`/First`).
Packing packing(const std::vector<std::pair<std::uint32_t, std::string>>& objects, std::string_view lead) {
    std::string head{lead};
    std::string body;

    for (const auto& [number, text] : objects) {
        head += std::to_string(number) + " " + std::to_string(body.size()) + " ";
        body += text + "\n";
    }

    return {head + body, head.size()};
}

// A PDF file assembled for a trial: objects one after another, a cross-reference table or stream
// for the objects written since the last one, a trailer pointing to the section before. Offsets
// count from the header, so junk before it shifts them as writers that prepend it do. Objects in
// an object stream are remembered for the cross-reference stream. It begins with the header of the
// version, after the junk.
Assembly::Assembly(std::string_view version, std::string_view junk) : base_(junk.size()) {
    text_ = std::string{junk} + "%PDF-" + std::string{version} + "\n%\xE2\xE3\xCF\xD3\n";
}

// Appends an object.
void Assembly::object(std::uint32_t number, std::string_view body) {
    offsets_[number] = text_.size() - base_;
    fresh_.push_back(number);
    text_ += std::to_string(number) + " 0 obj\n" + std::string{body} + "\nendobj\n";
}

// Appends a stream object, its length counted.
void Assembly::stream(std::uint32_t number, std::string_view dict, std::string_view data) {
    object(number, "<<" + std::string{dict} + " /Length " + std::to_string(data.size()) + ">>\nstream\n" + std::string{data} + "\nendstream");
}

// Appends an object stream holding the objects after `lead`, its data made by `encode` (by
// default stored in the zlib format) and its dictionary holding `filter` besides its type,
// count and start.
void Assembly::packed(std::uint32_t number, const std::vector<std::pair<std::uint32_t, std::string>>& objects,
                      const std::function<std::string(std::string_view)>& encode, std::string_view filter, std::string_view lead) {
    const Packing made = packing(objects, lead);

    for (std::size_t at = 0; at < objects.size(); ++at) inside(objects[at].first, number, static_cast<std::uint32_t>(at));

    stream(number, "/Type /ObjStm" + std::string{filter} + " /N " + std::to_string(objects.size()) + " /First " + std::to_string(made.first),
           encode(made.data));
}

// Notes that object `number` is the `index`-th of object stream `pack`, for the cross-reference
// streams after.
void Assembly::inside(std::uint32_t number, std::uint32_t pack, std::uint32_t index) {
    inside_[number] = {pack, index};
}

// The entries of a cross-reference stream about to be written as object `number`, which is
// placed where the file ends now: for every number from zero, type 1 and the offset of an
// object written directly, type 2 with the object stream and the index of a packed one, and
// type 0 (free) for the numbers between.
std::vector<std::array<std::uint64_t, 3>> Assembly::entries(std::uint32_t number) {
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
std::size_t Assembly::xref_given(std::uint32_t number, std::string_view dict, std::string_view data) {
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
std::size_t Assembly::table(std::string_view trailer) {
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
std::size_t Assembly::xref_stream(std::uint32_t number, std::string_view trailer, bool predicted) {
    const std::vector<std::array<std::uint64_t, 3>> rows = entries(number);
    const std::string fields = fielded(rows, {1, 4, 2});
    const std::string parms = predicted ? " /DecodeParms << /Predictor 12 /Columns 7 >>" : "";

    return xref_given(number, " /W [1 4 2] /Size " + std::to_string(rows.size()) + " /Filter /FlateDecode" + parms + std::string{trailer},
                      zlibbed(predicted ? png(fields, 7, {2}) : fields));
}

// The file as bytes.
Bytes Assembly::bytes() const {
    return Bytes(reinterpret_cast<const std::byte*>(text_.data()), reinterpret_cast<const std::byte*>(text_.data()) + text_.size());
}

// A number written with leading zeros to the width.
std::string Assembly::padded(std::size_t value, std::size_t width) {
    std::string made = std::to_string(value);

    return std::string(width > made.size() ? width - made.size() : 0, '0') + made;
}

// The `/Size` and `/Prev` of the next trailer.
std::string Assembly::sized() const {
    std::uint32_t top = 0;

    for (const auto& [one, place] : offsets_) top = std::max(top, one);

    return " /Size " + std::to_string(top + 1) + (previous_ == 0 ? std::string{} : " /Prev " + std::to_string(previous_));
}

// Ends a section: the trailer, "startxref" with the section's offset, the end-of-file mark.
void Assembly::closed(std::size_t at, const std::string& trailer) {
    text_ += trailer + "startxref\n" + std::to_string(at) + "\n%%EOF\n";

    fresh_.clear();
    previous_ = at;
}

}

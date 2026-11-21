#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "tal/docs.hpp"
#include "tal/executor.hpp"
#include "tal/meta.hpp"
#include "tal/pages.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "assembling.hpp"
#include "documents_of.hpp"
#include "expected_of.hpp"
#include "tal/testing.hpp"

// Text versions (E6f): PDF read page by page by our own reader, against pages made here for each
// operator, font and filter the reading knows, against three articles set by pdfLaTeX (lmodern,
// the old OT1 fonts, and EC fonts that came out as bitmaps; test/documents/article.tex), and
// against what is no document, an encrypted one, damaged content and two readings at once.

namespace {

using tal::Bytes;
using tal::docs::Block;
using tal::docs::Plain;
using tal::testing::Assembly;
using tal::testing::zlibbed;

// The bytes of `text`.
Bytes bytes_of(std::string_view text) {
    return Bytes{reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

// What `work` threw: the kind and message of its trouble, or "none".
template <typename Work>
std::string refusal(Work work) {
    try {
        work();
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

// A font of the standard 14 in WinAnsiEncoding, every glyph from 32 to 255 half an em wide.
std::string helvetica(std::string_view encoding = "/WinAnsiEncoding") {
    std::string widths;

    for (int one = 32; one < 256; ++one) widths += " 500";

    return "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding " + std::string{encoding} + " /FirstChar 32 /LastChar 255 /Widths [" + widths +
           " ] >>";
}

// A stream object for a PDF being made: its number, the entries of its dictionary and its data.
struct Part {
    std::uint32_t number = 0;
    std::string dict;
    std::string data;
};

// A document of one page drawn by `content` (whose dictionary holds `filter`), with `resources`
// and the objects `objects` and streams `streams` besides; its trailer holds `trailer`.
Bytes page_of(std::string_view content, std::string_view resources, const std::vector<std::pair<std::uint32_t, std::string>>& objects = {},
              const std::vector<Part>& streams = {}, std::string_view filter = {}, std::string_view trailer = " /Root 1 0 R") {
    Assembly one{"1.7"};

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    one.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources " + std::string{resources} + " /Contents 4 0 R >>");
    one.stream(4, filter, content);

    for (const auto& [number, body] : objects) one.object(number, body);

    for (const Part& part : streams) one.stream(part.number, part.dict, part.data);

    one.table(trailer);

    return one.bytes();
}

// The text of the page `which` of the document `pdf`.
Plain text_of(Bytes pdf, std::size_t which = 0) {
    tal::Loop loop;
    const auto prose = tal::wait(loop, tal::docs::prose(std::make_shared<tal::meta::Held>(std::move(pdf))));

    return tal::wait(loop, prose->page(which));
}

// The blocks of a page's text after its number, joined by " / ".
std::string body(const Plain& plain) {
    std::string made;

    for (const Block& one : plain.blocks) {
        if (one.kind == Block::Kind::page) continue;

        if (!made.empty()) made += " / ";

        made += one.text;
    }

    return made;
}

// The text of one page drawn by `content` in Helvetica as /F1.
std::string drawn(std::string_view content) { return body(text_of(page_of(content, "<< /Font << /F1 5 0 R >> >>", {{5, helvetica()}}))); }

// `data` as ASCIIHexDecode writes it.
std::string hexed(std::string_view data) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string made;

    for (const char one : data) {
        made += digits[static_cast<unsigned char>(one) >> 4U];
        made += digits[static_cast<unsigned char>(one) & 15U];
        made += ' ';
    }

    return made + ">";
}

// `data` as ASCII85Decode writes it, with 'z' for four zeros.
std::string based(std::string_view data) {
    std::string made;

    for (std::size_t at = 0; at < data.size(); at += 4) {
        const std::size_t much = std::min<std::size_t>(4, data.size() - at);
        std::uint32_t group = 0;

        for (std::size_t one = 0; one < 4; ++one) group = (group << 8U) | (one < much ? static_cast<unsigned char>(data[at + one]) : 0U);

        if (group == 0 && much == 4) {
            made += 'z';

            continue;
        }

        char digits[5];

        for (int one = 4; one >= 0; --one) {
            digits[one] = static_cast<char>('!' + group % 85);
            group /= 85;
        }

        made.append(digits, much + 1);
    }

    return made + "~>";
}

// `data` as RunLengthDecode writes it: runs of three or more as repeats, the rest as literals.
std::string run_length(std::string_view data) {
    std::string made;
    std::size_t at = 0;

    while (at < data.size()) {
        std::size_t same = 1;

        while (at + same < data.size() && same < 128 && data[at + same] == data[at]) ++same;

        if (same >= 3) {
            made += static_cast<char>(257 - same);
            made += data[at];
            at += same;

            continue;
        }

        const std::size_t much = std::min<std::size_t>(128, data.size() - at);

        made += static_cast<char>(much - 1);
        made.append(data.substr(at, much));
        at += much;
    }

    return made + '\x80';
}

// `data` as LZWDecode writes it, with EarlyChange 1 when `early`: a clear code first, codes from 9
// to 12 bits, the table cleared when full, the end code last. Its output was checked against
// qpdf's decoder when the trial was written, with either EarlyChange, for texts that reach every
// width and clear the table.
std::string woven(std::string_view data, bool early = true) {
    std::map<std::string, std::uint32_t> table;
    std::uint32_t next = 258;
    std::uint32_t width = 9;
    std::uint64_t bits = 0;
    std::uint32_t held = 0;
    std::string made;

    const auto reset = [&] {
        table.clear();

        for (std::uint32_t code = 0; code < 256; ++code) table[std::string(1, static_cast<char>(code))] = code;

        next = 258;
        width = 9;
    };
    const auto put = [&](std::uint32_t code) {
        bits = (bits << width) | code;
        held += width;

        while (held >= 8) {
            made += static_cast<char>((bits >> (held - 8)) & 0xFFU);
            held -= 8;
        }
    };

    reset();
    put(256);

    std::string word;

    for (const char one : data) {
        std::string longer = word + one;

        if (table.contains(longer)) {
            word = std::move(longer);

            continue;
        }

        put(table[word]);
        table[longer] = next++;

        if (next + (early ? 1 : 0) > (1U << width)) {
            if (width < 12) {
                ++width;
            } else {
                put(256);
                reset();
            }
        }

        word = std::string(1, one);
    }

    if (!word.empty()) put(table[word]);

    put(257);

    if (held > 0) made += static_cast<char>((bits << (8 - held)) & 0xFFU);

    return made;
}

// Words of made-up syllables, `size` characters in all, the same for the same `seed`: text
// varied enough that LZW fills its table to every width and clears it.
std::string babble(std::size_t size, std::uint32_t seed) {
    constexpr std::string_view syllables[] = {"ka", "lo", "mi", "ne", "ru", "sta", "pre", "wo", "zy", "bi", "dok", "tal", "cite", "ent"};
    std::string made;

    const auto next = [&seed] {
        seed = seed * 1664525U + 1013904223U;

        return seed >> 16U;
    };

    while (made.size() < size) {
        if (!made.empty()) made += ' ';

        for (std::uint32_t one = next() % 4 + 1; one > 0; --one) made += syllables[next() % std::size(syllables)];
    }

    made.resize(size);

    while (made.back() == ' ') made.pop_back();

    return made;
}

// A big-endian number of `size` bytes.
std::string big(std::uint32_t value, int size) {
    std::string made;

    for (int shift = 8 * (size - 1); shift >= 0; shift -= 8) made += static_cast<char>((value >> shift) & 0xFFU);

    return made;
}

// A cmap subtable of format 4 mapping the one code `code` to the glyph `glyph`.
std::string format4(std::uint32_t code, std::uint32_t glyph) {
    const std::uint32_t delta = (glyph - code) & 0xFFFFU;

    return big(4, 2) + big(32, 2) + big(0, 2) + big(4, 2) + big(4, 2) + big(1, 2) + big(0, 2) + big(code, 2) + big(0xFFFF, 2) + big(0, 2) + big(code, 2) +
           big(0xFFFF, 2) + big(delta, 2) + big(1, 2) + big(0, 2) + big(0, 2);
}

// The program of a TrueType font that holds only a 'cmap': the symbol subtable (3,0) maps 0xF041
// to glyph 5 and the Unicode one (3,1) maps U+0105 to it.
std::string truetype() {
    const std::string symbol = format4(0xF041, 5);
    const std::string unicode = format4(0x0105, 5);
    const std::string cmap = big(0, 2) + big(2, 2) + big(3, 2) + big(0, 2) + big(20, 4) + big(3, 2) + big(1, 2) + big(20 + static_cast<std::uint32_t>(symbol.size()), 4) +
                             symbol + unicode;

    return big(0x00010000, 4) + big(1, 2) + big(16, 2) + big(0, 2) + big(0, 2) + "cmap" + big(0, 4) + big(28, 4) + big(static_cast<std::uint32_t>(cmap.size()), 4) + cmap;
}

// A reader of `bytes` that waits a moment before every read, so that a reading takes turns.
class Slow final : public tal::meta::Reader {
public:
    // Over `given`.
    explicit Slow(Bytes given) : held_(std::move(given)) {}

    // The size of the bytes.
    tal::Task<std::uint64_t> size() override { return held_.size(); }

    // What the bytes have of `much` from `at`, a millisecond later.
    tal::Task<Bytes> read(std::uint64_t at, std::size_t much) override {
        co_await tal::after(std::chrono::milliseconds{1});

        co_return co_await held_.read(at, much);
    }

private:
    tal::meta::Held held_;
};

}

TRIAL("PDF text: lines and paragraphs from where glyphs land, spaces from gaps, a superscript on its line") {
    SAME(drawn("BT /F1 10 Tf 72 700 Td (Hello) Tj ( World) Tj 0 -12 Td (second line) Tj 0 -40 Td (new paragraph) Tj 5 Ts (2) Tj 0 Ts "
               "[(a) -400 (b) -100 (c)] TJ ET"),
         std::string{"Hello World\nsecond line / new paragraph2a bc"});
    SAME(drawn("BT /F1 10 Tf 14 TL 72 700 Td (one) Tj T* (two) Tj (three) ' 3 3 (four) \" ET"), std::string{"one\ntwo\nthree\nfour"});
    SAME(drawn("BT /F1 10 Tf 5 Tw 72 700 Td (a b) Tj 0 -12 Td 200 Tz (cd) Tj ET"), std::string{"a b\ncd"});
    SAME(drawn("BT /F1 10 Tf 300 700 Td (right) Tj -200 0 Td (left) Tj ET"), std::string{"right / left"});
    SAME(drawn("BT /F1 10 Tf 72 700 Td (first) Tj 0 300 Td (above) Tj ET"), std::string{"first / above"});
    SAME(drawn("BT /F1 10 Tf 72 700 Td (a) Tj 0 -12 Td (b) Tj 0 -25 Td (c) Tj ET"), std::string{"a\nb / c"});
    SAME(drawn("BT /F1 10 Tf 72 700 Td (a) Tj 0 -14 TD (b) Tj T* (c) Tj ET"), std::string{"a\nb\nc"});
    SAME(drawn("BT /F1 10 Tf 14 TL 72 700 Td 10 0 (a b) \" 1 0 0 1 97 686 Tm (c) Tj ET"), std::string{"a bc"});
    SAME(drawn("BT /F1 10 Tf 72 700 Td 20 Tw (ab) Tj 1 0 0 1 82 700 Tm (c) Tj ET"), std::string{"abc"});
    SAME(drawn("q 2 0 0 2 0 0 cm BT /F1 10 Tf 36 350 Td (scaled) Tj ET Q BT /F1 10 Tf 72 680 Td (plain) Tj ET"), std::string{"scaled\nplain"});
}

TRIAL("PDF text: turned text read along its direction, a vertical font down its column") {
    SAME(drawn("BT /F1 10 Tf 0 1 -1 0 300 300 Tm (Turned) Tj ( text) Tj ET"), std::string{"Turned text"});
    SAME(drawn("BT /F1 10 Tf -1 0 0 -1 500 500 Tm (Upside) Tj ( down) Tj ET"), std::string{"Upside down"});
}

TRIAL("PDF text: simple fonts by their Differences through the glyph list, uniXXXX and uXXXX, ligatures as letters") {
    const std::string font = helvetica("<< /BaseEncoding /WinAnsiEncoding /Differences [65 /aogonek /fi /uni017C /u1F600 /notaname /a_b /ff.sc /uni00410042] >>");

    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td (ABCDEFGH) Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, font}}))),
         std::string{"\xC4\x85" "fi\xC5\xBC\xF0\x9F\x98\x80\xEF\xBF\xBD" "abffAB"});
    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td (\\200\\223\\256) Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, helvetica("/MacRomanEncoding")}}))),
         std::string{"\xC3\x84\xC3\xAC\xC3\x86"});
    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td (\\261\\341) Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, helvetica("/StandardEncoding")}}))),
         std::string{"\xE2\x80\x93\xC3\x86"});
    SAME(drawn("BT /F1 10 Tf 72 700 Td (\\200\\201\\225x) Tj ET"), std::string{"\xE2\x82\xAC\xE2\x80\xA2\xE2\x80\xA2x"});
}

TRIAL("PDF text: a composite font: ToUnicode's codespace, bfchar and bfrange (counted up and listed), widths from W") {
    const std::string cmap = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                             "2 beginbfchar <0001> <0041> <0002> <D83DDE00> endbfchar\n1 beginbfrange <000A> <000C> <0061> endbfrange\n"
                             "1 beginbfrange <0010> <0011> [<0105> <00660069>] endbfrange\nendcmap CMapName currentdict /CMap defineresource pop end end";
    const std::string font = "<< /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 7 0 R >>";
    const std::string cid = "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /X /DW 1000 /W [1 [500 500] 10 20 600] >>";
    const Plain one = text_of(page_of("BT /F1 10 Tf 72 700 Td <00010002000A000B000C001000110003> Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, font}, {6, cid}},
                                      {Part{7, "", cmap}}));

    SAME(body(one), std::string{"A\xF0\x9F\x98\x80" "abc\xC4\x85" "fi\xEF\xBF\xBD"});

    const std::string spaced = "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /X /DW 500 >>";

    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td [<0001> -1000 <000A>] TJ ET", "<< /Font << /F1 5 0 R >> >>", {{5, font}, {6, spaced}},
                              {Part{7, "", cmap}}))),
         std::string{"A a"});
}

TRIAL("PDF text: content in every filter, chained, and split into an array of streams") {
    const std::string content = "BT /F1 10 Tf 72 700 Td (Filtered text in every way: " + std::string(600, 'w') + ") Tj ET";
    const std::string wanted = "Filtered text in every way: " + std::string(600, 'w');
    const std::string resources = "<< /Font << /F1 5 0 R >> >>";
    const std::vector<std::pair<std::uint32_t, std::string>> font{{5, helvetica()}};

    SAME(body(text_of(page_of(hexed(content), resources, font, {}, " /Filter /ASCIIHexDecode"))), wanted);
    SAME(body(text_of(page_of(based(content), resources, font, {}, " /Filter /A85"))), wanted);
    SAME(body(text_of(page_of(run_length(content), resources, font, {}, " /Filter /RunLengthDecode"))), wanted);
    SAME(body(text_of(page_of(woven(content), resources, font, {}, " /Filter /LZWDecode"))), wanted);

    const std::string long_text = babble(60000, 4);

    SAME(body(text_of(page_of(woven("BT /F1 10 Tf 72 700 Td (" + long_text + ") Tj ET"), resources, font, {}, " /Filter /LZWDecode"))), long_text);
    SAME(body(text_of(page_of(woven("BT /F1 10 Tf 72 700 Td (" + long_text + ") Tj ET"), resources, font, {}, " /Filter /LZWDecode /DecodeParms << /EarlyChange 1 >>"))),
         long_text);
    SAME(body(text_of(page_of(woven("BT /F1 10 Tf 72 700 Td (" + long_text + ") Tj ET", false), resources, font, {},
                              " /Filter /LZWDecode /DecodeParms << /EarlyChange 0 >>"))),
         long_text);
    SAME(body(text_of(page_of(based(zlibbed(content)), resources, font, {}, " /Filter [/ASCII85Decode /FlateDecode]"))), wanted);
    SAME(body(text_of(page_of(based(std::string(8, '\0') + "abc"), resources, font, {}, " /Filter /ASCII85Decode"))), std::string{});

    std::string ragged = "BT /F1 10 Tf 72 700 Td (ab)";

    while ((ragged.size() + 3) % 4 != 2) ragged += ' ';

    ragged += " Tj";
    SAME(body(text_of(page_of(based(ragged), resources, font, {}, " /Filter /A85"))), std::string{"ab"});
    SAME(body(text_of(page_of(run_length("BT /F1 10 Tf 72 700 Td (ab) Tj") + "\x07 (zz) Tj", resources, font, {}, " /Filter /RL"))), std::string{"ab"});
    SAME(refusal([&] { (void)text_of(page_of("!!z!!~>", resources, font, {}, " /Filter /A85")); }), std::string{"broken: The content of the page could not be read."});
    SAME(refusal([&] { (void)text_of(page_of("!!!!!!~>", resources, font, {}, " /Filter /A85")); }), std::string{"broken: The content of the page could not be read."});
    SAME(refusal([&] { (void)text_of(page_of("41 4G>", resources, font, {}, " /Filter /AHx")); }), std::string{"broken: The content of the page could not be read."});

    Assembly split{"1.7"};

    split.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    split.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    split.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources " + resources + " /Contents [4 0 R 6 0 R] >>");
    split.stream(4, "", "BT /F1 10 Tf 72 700 Td");
    split.object(5, helvetica());
    split.stream(6, " /Filter /FlateDecode", zlibbed("(Split) Tj ET"));
    split.table(" /Root 1 0 R");
    SAME(body(text_of(split.bytes())), std::string{"Split"});
}

TRIAL("PDF text: the LZW decoder against the example of the standard") {
    const std::string example{"\x80\x0B\x60\x50\x22\x0C\x0C\x85\x01", 9};

    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td <" + hexed("-----A---B").substr(0, 29) + "> Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, helvetica()}}))),
         std::string{"-----A---B"});
    SAME(woven("-----A---B"), example);
}

TRIAL("PDF text: forms drawn by forms within their depth, with their matrices and their own resources") {
    const std::string form = "BT /F2 10 Tf 72 700 Td (In form) Tj ET /Me Do";
    const Plain one = text_of(page_of("BT /F1 10 Tf 72 750 Td (Page) Tj ET /Fm Do", "<< /Font << /F1 5 0 R >> /XObject << /Fm 6 0 R >> >>", {{5, helvetica()}},
                                      {Part{6, " /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Matrix [1 0 0 1 0 -60] /Resources << /Font << /F2 5 0 R >> "
                                               "/XObject << /Me 6 0 R >> >>",
                                            form}}));

    SAME(one.blocks.size(), std::size_t{10});
    SAME(one.blocks[1].text, std::string{"Page"});
    SAME(one.blocks[9].text, std::string{"In form"});
    std::string often = "BT /F1 10 Tf 72 700 Td (Drawn) Tj ET ";

    for (int one = 0; one < 2000; ++one) often += "/Fm Do ";

    const std::string big = "BT /F1 10 Tf 72 600 Td (x) Tj ET" + std::string(100000, ' ');
    const Plain cached = text_of(page_of(often, "<< /Font << /F1 5 0 R >> /XObject << /Fm 6 0 R >> >>", {{5, helvetica()}},
                                         {Part{6, " /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Filter /FlateDecode", zlibbed(big)}}));

    OUGHT(!cached.cut);
    SAME(cached.blocks.at(1).text, std::string{"Drawn"});
    SAME(cached.blocks.at(2).text, std::string(2000, 'x'));

    std::string endless = "BT /F1 10 Tf 72 700 Td (Drawn) Tj ET ";

    for (int one = 0; one < 70000; ++one) endless += "/Fm Do ";

    const Plain cut = text_of(page_of(endless, "<< /Font << /F1 5 0 R >> /XObject << /Fm 6 0 R >> >>", {{5, helvetica()}},
                                      {Part{6, " /Type /XObject /Subtype /Form /BBox [0 0 612 792]", ""}}));

    OUGHT(cut.cut);
    SAME(cut.blocks.at(1).text, std::string{"Drawn"});
}

TRIAL("PDF text: an accent over a letter joins it, an unknown glyph over a letter goes, ActualText stands for glyphs, inline pictures are skipped") {
    const std::string font = helvetica("<< /BaseEncoding /WinAnsiEncoding /Differences [1 /notaname 2 /suppress] >>");
    const std::string content = "BT /F1 10 Tf 72 700 Td [(\\264) 500 (e) (\\001) 500 (x) (z) (\\264)] TJ ( ) Tj [(l) 500 (\\002) ( ) (\\264)] TJ ET "
                                "BT /F1 10 Tf 72 688 Td /Span << /ActualText (fi) >> BDC (XY) Tj EMC (!) Tj ET "
                                "BI /W 1 /H 1 /CS /G /BPC 8 ID \x01 ET (no) Tj BT EI BT /F1 10 Tf 72 676 Td (after) Tj ET "
                                "BT /F1 10 Tf 72 664 Td /Span << /ActualText (one) >> BDC (O) Tj /Span << /ActualText () >> BDC (N) Tj EMC (E) Tj EMC (.) Tj ET";

    SAME(body(text_of(page_of(content, "<< /Font << /F1 5 0 R >> >>", {{5, font}}))),
         std::string{"\xC3\xA9xz\xC2\xB4 \xC5\x82 \xC2\xB4\nfi!\nafter\none."});

    const std::string wide = helvetica("<< /BaseEncoding /WinAnsiEncoding /Differences [200 /notaname /dotlessi] >>");
    const auto shown = [&](std::string_view drawing) {
        return body(text_of(page_of("BT /F1 10 Tf 72 700 Td " + std::string{drawing} + " ET", "<< /Font << /F1 5 0 R >> >>", {{5, wide}})));
    };

    SAME(shown("[(\\264) (e)] TJ"), std::string{"\xC2\xB4" "e"});
    SAME(shown("[(\\264) 500 (\\311)] TJ"), std::string{"\xC3\xAD"});
    SAME(shown("(\\264) Tj 0 -20 Td (e) Tj"), std::string{"\xC2\xB4 / e"});
    SAME(shown("[(a) (\\310) 500 (x)] TJ"), std::string{"ax"});
    SAME(shown("[(e) 500 (\\310) (z)] TJ"), std::string{"ez"});
    SAME(shown("(a\\310b) Tj"), std::string{"a\xEF\xBF\xBD" "b"});
    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td /Span << /ActualText (outer) >> BDC (A) Tj /Span << /ActualText (inner) >> BDC (B) Tj EMC (C) Tj EMC (!) Tj ET",
                              "<< /Font << /F1 5 0 R >> >>", {{5, font}}))),
         std::string{"outer!"});

    std::string deep = "BT /F1 10 Tf 72 700 Td /Span << /ActualText (fi) >> BDC ";

    for (int one = 0; one < 300; ++one) deep += "/X BMC ";

    for (int one = 0; one < 300; ++one) deep += "EMC ";

    SAME(body(text_of(page_of(deep + "(XY) Tj EMC (!) Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, font}}))), std::string{"fi!"});
}

TRIAL("PDF text: an embedded Type 1 font's own encoding, a symbolic TrueType font's cmaps, bitmap fonts of pdfTeX by their codes") {
    const std::string program = "%!PS-AdobeFont-1.0: Test 001\n/FontName /Test def\n/Encoding 256 array\n0 1 255 {1 index exch /.notdef put} for\n"
                                "dup 65 /aogonek put\ndup 66 /zdotaccent put\nreadonly def\ncurrentfile eexec\n\x01\x02\x03";
    const std::string type1 = "<< /Type /Font /Subtype /Type1 /BaseFont /Test /FirstChar 65 /LastChar 66 /Widths [500 500] /FontDescriptor 6 0 R >>";

    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td (AB) Tj ET", "<< /Font << /F1 5 0 R >> >>",
                              {{5, type1}, {6, "<< /Type /FontDescriptor /FontName /Test /Flags 32 /FontFile 7 0 R >>"}},
                              {Part{7, " /Length1 " + std::to_string(program.size() - 3) + " /Length2 3 /Length3 0", program}}))),
         std::string{"\xC4\x85\xC5\xBC"});

    const std::string symbolic = "<< /Type /Font /Subtype /TrueType /BaseFont /Sym /FirstChar 65 /LastChar 66 /Widths [500 500] /FontDescriptor 6 0 R >>";

    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td (AB) Tj ET", "<< /Font << /F1 5 0 R >> >>",
                              {{5, symbolic}, {6, "<< /Type /FontDescriptor /FontName /Sym /Flags 4 /FontFile2 7 0 R >>"}}, {Part{7, "", truetype()}}))),
         std::string{"\xC4\x85\xEF\xBF\xBD"});

    const std::string bitmap = "<< /Type /Font /Subtype /Type3 /FontMatrix [0.001 0 0 0.001 0 0] /FontBBox [0 0 1 1] /CharProcs << >> /FirstChar 65 "
                               "/LastChar 66 /Widths [500 500] /Encoding << /Type /Encoding /Differences [65 /a65 /a161] >> >>";
    const std::string symbols = "<< /Type /Font /Subtype /Type3 /FontMatrix [0.001 0 0 0.001 0 0] /FontBBox [0 0 1 1] /CharProcs << >> /FirstChar 15 "
                                "/LastChar 15 /Widths [500] /Encoding << /Type /Encoding /Differences [15 /a15] >> >>";

    SAME(body(text_of(page_of("BT /F1 10 Tf 72 700 Td (AB) Tj /F2 10 Tf (\\017) Tj ET", "<< /Font << /F1 5 0 R /F2 6 0 R >> >>", {{5, bitmap}, {6, symbols}}))),
         std::string{"A\xC4\x85\xEF\xBF\xBD"});
}

TRIAL("PDF text: links of the page to addresses on the web are anchors over the text that lies in them, across lines too") {
    const std::string annotations = " /Annots [6 0 R 7 0 R 8 0 R 9 0 R 10 0 R]";
    Assembly one{"1.7"};

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    one.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R" + annotations + " >>");
    one.stream(4, "", "BT /F1 10 Tf 72 700 Td (see the docs and) Tj 0 -12 Td (the next line too) Tj 0 -12 Td (inside nothing) Tj ET");
    one.object(5, helvetica());
    one.object(6, "<< /Type /Annot /Subtype /Link /Rect [132 701 92 710] /A << /S /URI /URI (https://example.test/docs) >> >>");
    one.object(7, "<< /Type /Annot /Subtype /Link /Rect [137 698 300 710] /A 11 0 R >>");
    one.object(8, "<< /Type /Annot /Subtype /Link /Rect [72 686 87 698] /A 11 0 R >>");
    one.object(9, "<< /Type /Annot /Subtype /Link /Rect [72 674 300 686] /Dest [3 0 R /Fit] >>");
    one.object(10, "<< /Type /Annot /Subtype /Link /Rect [72 674 300 686] /A << /S /URI /URI (javascript:\001x) >> >>");
    one.object(11, "<< /S /URI /URI (https://example.test/next) >>");
    one.table(" /Root 1 0 R");

    const Plain made = text_of(one.bytes());

    SAME(made.blocks.size(), std::size_t{2});
    SAME(made.blocks[1].text, std::string{"see the docs and\nthe next line too\ninside nothing"});
    SAME(made.blocks[1].anchors.size(), std::size_t{2});
    SAME(made.blocks[1].anchors[0], (tal::docs::Anchor{0, 4, 12, "https://example.test/docs"}));
    SAME(made.blocks[1].anchors[1], (tal::docs::Anchor{0, 13, 20, "https://example.test/next"}));

    const std::string broken = "<< /S /URI /URI (https://example.test/a/b) >>";
    Assembly hyper{"1.7"};

    hyper.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    hyper.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    hyper.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R 7 0 R] >>");
    hyper.stream(4, "", "BT /F1 10 Tf 72 700 Td (go to x/a/) Tj 0 -12 Td (b now) Tj ET");
    hyper.object(5, helvetica());
    hyper.object(6, "<< /Subtype /Link /Rect [102 698 118 710] /A " + broken + " >>");
    hyper.object(7, "<< /Subtype /Link /Rect [72 686 77 698] /A " + broken + " >>");
    hyper.table(" /Root 1 0 R");

    const Plain joined = text_of(hyper.bytes());

    SAME(joined.blocks[1].text, std::string{"go to x/a/\nb now"});
    SAME(joined.blocks[1].anchors, (std::vector<tal::docs::Anchor>{{0, 6, 12, "https://example.test/a/b"}}));
    Assembly many{"1.7"};

    many.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    many.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    many.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R 7 0 R 8 0 R 9 0 R 10 0 R 11 0 R] >>");
    many.stream(4, "", "BT /F1 10 Tf 12 TL 72 700 Td [(ab) -400 (cd) ( ef)] TJ T* [(e) 500 (\\264) ( z)] TJ T* ( gh ij) Tj T* (kl mn) Tj T* (op qr) Tj T* (st) Tj ET");
    many.object(5, helvetica());
    many.object(6, "<< /Subtype /Link /Rect [72 698 97 710] /A << /S /URI /URI (https://example.test/one) >> >>");
    many.object(7, "<< /Subtype /Link /Rect [72 686 77 698] /A << /S /URI /URI (https://example.test/two) >> >>");
    many.object(8, "<< /Subtype /Link /Rect [77 674 92 686] /A << /S /URI /URI (https://example.test/three) >> >>");
    many.object(9, "<< /Subtype /Link /Rect [72 662 82 674] /A << /S /URI /URI (https://example.test/three) >> >>");
    many.object(10, "<< /Subtype /Link /Rect [72 650 82 662] /A << /S /URI /URI (https://example.test/four) >> >>");
    many.object(11, "<< /Subtype /Link /Rect [72 638 82 650] /A << /S /URI /URI (https://example.test/four) >> >>");
    many.table(" /Root 1 0 R");

    const Plain spans = text_of(many.bytes());

    SAME(spans.blocks[1].text, std::string{"ab cd ef\n\xC3\xA9 z\ngh ij\nkl mn\nop qr\nst"});
    SAME(spans.blocks[1].anchors, (std::vector<tal::docs::Anchor>{{0, 0, 5, "https://example.test/one"},
                                                                  {0, 9, 11, "https://example.test/two"},
                                                                  {0, 14, 16, "https://example.test/three"},
                                                                  {0, 20, 22, "https://example.test/three"},
                                                                  {0, 26, 28, "https://example.test/four"},
                                                                  {0, 32, 34, "https://example.test/four"}}));

    Assembly other{"1.7"};

    other.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    other.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    other.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R] >>");
    other.stream(4, "", "BT /F1 10 Tf 72 700 Td (here) Tj ET");
    other.object(5, helvetica());
    other.object(6, "<< /Subtype /Link /Rect [72 698 92 710] /A << /S /GoTo /URI (https://example.test/no) /D [3 0 R /Fit] >> >>");
    other.table(" /Root 1 0 R");
    OUGHT(text_of(other.bytes()).blocks[1].anchors.empty());

    Assembly led{"1.7"};

    led.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    led.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    led.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R] >>");
    led.stream(4, "", "BT /F1 10 Tf 72 700 Td /Span << /ActualText ( x) >> BDC (XY) Tj EMC (y) Tj ET");
    led.object(5, helvetica());
    led.object(6, "<< /Subtype /Link /Rect [82 698 87 710] /A << /S /URI /URI (https://example.test/y) >> >>");
    led.table(" /Root 1 0 R");

    const Plain trimmed = text_of(led.bytes());

    SAME(trimmed.blocks[1].text, std::string{"xy"});
    SAME(trimmed.blocks[1].anchors, (std::vector<tal::docs::Anchor>{{0, 1, 2, "https://example.test/y"}}));
}

TRIAL("PDF text: three articles set by pdfLaTeX read as the texts they are known to hold") {
    const std::vector<std::tuple<std::string_view, std::string_view, std::string_view>> samples{
        {"lmodern", documents_of::lmodern, expected_of::lmodern}, {"ot1", documents_of::ot1, expected_of::ot1}, {"bitmap", documents_of::bitmap, expected_of::bitmap}};

    for (const auto& [name, document, expected] : samples) {
        tal::Loop loop;
        const auto prose = tal::wait(loop, tal::docs::prose(std::make_shared<tal::meta::Held>(bytes_of(document))));

        SAME(prose->pages(), std::size_t{1});

        const std::string made = tal::written(tal::wait(loop, prose->page(0))).text;

        if (made != expected) tal::testing::fell(__FILE__, __LINE__, ("the text of " + std::string{name} + " differs:\n" + made).c_str());
    }
}

TRIAL("PDF text: five hundred pages listed, each read alone, in any order") {
    Assembly one{"1.7"};
    std::string kids;

    for (std::uint32_t page = 0; page < 500; ++page) kids += std::to_string(10 + 2 * page) + " 0 R ";

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [" + kids + "] /Count 500 /Resources << /Font << /F1 5 0 R >> >> >>");
    one.object(5, helvetica());

    for (std::uint32_t page = 0; page < 500; ++page) {
        one.object(10 + 2 * page, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents " + std::to_string(11 + 2 * page) + " 0 R >>");
        one.stream(11 + 2 * page, "", "BT /F1 10 Tf 72 700 Td (Page number " + std::to_string(page + 1) + ") Tj ET");
    }

    one.table(" /Root 1 0 R");

    tal::Loop loop;
    const auto prose = tal::wait(loop, tal::docs::prose(std::make_shared<tal::meta::Held>(one.bytes())));

    SAME(prose->pages(), std::size_t{500});

    for (const std::size_t page : {std::size_t{499}, std::size_t{0}, std::size_t{250}}) {
        const Plain made = tal::wait(loop, prose->page(page));

        SAME(made.blocks.at(0).text, std::to_string(page + 1));
        SAME(body(made), "Page number " + std::to_string(page + 1));
    }

    SAME(refusal([&] { (void)tal::wait(loop, prose->page(500)); }), std::string{"other: No such page."});
}

TRIAL("PDF text: no PDF, an encrypted one, one without a catalog, damaged content") {
    tal::Loop loop;
    const auto opened = [&](Bytes given) { (void)tal::wait(loop, tal::docs::prose(std::make_shared<tal::meta::Held>(std::move(given)))); };

    SAME(refusal([&] { opened(bytes_of("<html>no</html>")); }), std::string{"unsupported: This is not a PDF document."});
    SAME(refusal([&] { opened(page_of("BT ET", "<< >>", {}, {}, {}, " /Root 1 0 R /Encrypt << /Filter /Standard /V 1 >>")); }),
         std::string{"denied: The document is encrypted, so it has no text version."});
    SAME(refusal([&] { opened(page_of("BT ET", "<< >>", {}, {}, {}, " /Info 1 0 R")); }), std::string{"none"});

    Assembly bare{"1.7"};

    bare.object(1, "<< /Producer (x) >>");
    bare.table(" /Info 1 0 R");
    SAME(refusal([&] { opened(bare.bytes()); }), std::string{"malformed: The document has no catalog."});
    SAME(refusal([&] { (void)text_of(page_of("garbage, not zlib", "<< >>", {}, {}, " /Filter /FlateDecode")); }),
         std::string{"broken: The content of the page could not be read."});
}

TRIAL("PDF text: a page asked for while another is read is busy; a reading cancelled half way leaves the next one whole") {
    tal::Loop loop;
    const Bytes pdf = page_of("BT /F1 10 Tf 72 700 Td (Slow) Tj ET", "<< /Font << /F1 5 0 R >> >>", {{5, helvetica()}});
    const auto prose = tal::wait(loop, tal::docs::prose(std::make_shared<Slow>(pdf)));

    SAME(refusal([&] { (void)tal::wait(loop, tal::all(prose->page(0), prose->page(0))); }), std::string{"busy: Another page of the text is being read."});
    SAME(body(tal::wait(loop, prose->page(0))), std::string{"Slow"});

    const auto quick = []() -> tal::Task<Plain> { co_return Plain{}; };
    std::vector<tal::Task<Plain>> race;

    race.push_back(prose->page(0));
    race.push_back(quick());
    (void)tal::wait(loop, tal::first(std::move(race)));
    SAME(body(tal::wait(loop, prose->page(0))), std::string{"Slow"});
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/docs.hpp"
#include "tal/document.hpp"
#include "tal/executor.hpp"
#include "tal/leaves.hpp"
#include "tal/meta.hpp"
#include "tal/pixels.hpp"
#include "tal/process.hpp"
#include "tal/scene.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "assembling.hpp"
#include "rendering.hpp"

// Trials of the renderers built into tal. Without PDFium (TAL_PDFIUM) the built-in chain takes PDF
// only to refuse it with the sentence that says why; with it, the chain takes PDF and gives it to
// tal-render-pdfium, the program beside the trials, which renders through PDFium in a process of
// its own.
//
// The documents are assembled in the trials, object by object (`Assembly`): pages of known sizes
// and turns, each red on its left half and blue on its right, with a green square in a known
// place, so that a picture says where it was cut and how it was turned. One sample was written by
// another tool and is kept as bytes: such a page encrypted by qpdf 11 with AES-256 and the user
// password "tal".
namespace {

using tal::Bytes;
using tal::Kind;
using tal::docs::Page;
using tal::docs::Region;
using tal::docs::Renderer;
using tal::media::Pixels;
using tal::media::Size;

// The kind and the message of the trouble `call` throws, or "none".
template <typename Call>
std::string refusal(Call call) {
    try {
        call();
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

// The bytes of the text `text`.
Bytes bytes_of(std::string_view text) { return Bytes(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()); }

}

// The built-in chain is called "tal"; it takes PDF by its header (within the first 1024 bytes,
// after junk too), and when tal is built without PDFium it refuses every one, saying so.
TRIAL("Renderers built in: PDF by its header; without PDFium refused with the reason") {
    const std::shared_ptr<Renderer> builtin = Renderer::builtin();

    SAME(builtin->name(), std::string_view{"tal"});
    OUGHT(builtin->takes(bytes_of("%PDF-1.7\n")));
    OUGHT(builtin->takes(bytes_of(std::string(1000, ' ') + "%PDF-1.4")));
    OUGHT(!builtin->takes(bytes_of(std::string(1024, ' ') + "%PDF-1.4")));
    OUGHT(!builtin->takes(bytes_of("PK\x03\x04")));
    SAME(tal::docs::pdfium()->name(), std::string_view{"pdfium"});

#ifndef TAL_PDFIUM
    tal::Loop loop;

    SAME(refusal([&] { (void)tal::wait(loop, builtin->opened(std::make_shared<tal::meta::Held>(bytes_of("%PDF-1.7\n")), "secret")); }),
         std::string{"unsupported: This program was built without a PDF engine."});
#endif
}

#ifdef TAL_PDFIUM

namespace {

using tal::testing::Assembly;

// A page of `width` by `height` points turned by `turn` degrees: red on its left half, blue on its
// right, both but for a white strip of 10 points at the top, and a green square of 20 points whose
// lower left corner is at three quarters of the width and 10 points up; when `noted`, with an
// annotation whose appearance is a yellow square of 20 points, its lower left corner at a quarter
// of the width less 10 and 10 points up.
struct Sheet {
    int width = 200;
    int height = 100;
    int turn = 0;
    bool noted = false;
};

// A document of the pages `sheets`.
Bytes paper(const std::vector<Sheet>& sheets) {
    Assembly one{"1.7"};
    std::string kids;

    for (std::size_t at = 0; at < sheets.size(); ++at) kids += std::to_string(10 + 2 * at) + " 0 R ";

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(sheets.size()) + " >>");

    for (std::size_t at = 0; at < sheets.size(); ++at) {
        const Sheet& sheet = sheets[at];
        const std::string half = std::to_string(sheet.width / 2);
        const std::string painted = std::to_string(sheet.height - 10);
        const auto number = static_cast<std::uint32_t>(10 + 2 * at);
        const std::string notes = sheet.noted ? " /Annots [" + std::to_string(5000 + 2 * at) + " 0 R]" : "";

        one.object(number, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(sheet.width) + " " + std::to_string(sheet.height) +
                               "] /Rotate " + std::to_string(sheet.turn) + " /Contents " + std::to_string(number + 1) + " 0 R" + notes + " >>");
        one.stream(number + 1, "",
                   "1 0 0 rg 0 0 " + half + " " + painted + " re f 0 0 1 rg " + half + " 0 " + half + " " + painted + " re f 0 1 0 rg " +
                       std::to_string(sheet.width * 3 / 4) + " 10 20 20 re f");

        if (sheet.noted) {
            const std::string left = std::to_string(sheet.width / 4 - 10);
            const std::string right = std::to_string(sheet.width / 4 + 10);

            one.object(static_cast<std::uint32_t>(5000 + 2 * at), "<< /Type /Annot /Subtype /Square /Rect [" + left + " 10 " + right + " 30] /AP << /N " +
                                                                    std::to_string(5001 + 2 * at) + " 0 R >> >>");
            one.stream(static_cast<std::uint32_t>(5001 + 2 * at), "/Type /XObject /Subtype /Form /BBox [0 0 20 20]", "1 1 0 rg 0 0 20 20 re f");
        }
    }

    one.table(" /Root 1 0 R");

    return one.bytes();
}

// A document of one page whose content is a comment of three million bytes before the red and
// blue halves: PDFium asks for the whole stream at once, more than a block.
Bytes bulky() {
    Assembly one{"1.7"};

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    one.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R >>");
    one.stream(4, "", "% " + std::string(3000000, 'x') + "\n1 0 0 rg 0 0 100 100 re f 0 0 1 rg 100 0 100 100 re f");
    one.table(" /Root 1 0 R");

    return one.bytes();
}

// The page of `paper` encrypted by qpdf 11 with AES-256 and the user password "tal".
constexpr std::string_view locked_by_qpdf =
    "255044462d312e370a25bff7a2fe0a312030206f626a0a3c3c202f457874656e73696f6e73203c3c202f41444245203c3c202f42617365566572"
    "73696f6e202f312e37202f457874656e73696f6e4c6576656c2038203e3e203e3e202f5061676573203220302052202f54797065202f43617461"
    "6c6f67203e3e0a656e646f626a0a322030206f626a0a3c3c202f436f756e742031202f4b696473205b203320302052205d202f54797065202f50"
    "61676573203e3e0a656e646f626a0a332030206f626a0a3c3c202f436f6e74656e7473203420302052202f4d65646961426f78205b2030203020"
    "32303020313030205d202f506172656e74203220302052202f54797065202f50616765203e3e0a656e646f626a0a342030206f626a0a3c3c202f"
    "4c656e677468203634202f46696c746572202f466c6174654465636f6465203e3e0a73747265616d0a1457a03296f19d2b4f13af5b8b4c70a1a0"
    "00300ef80c4585bf1eb4ec9749932abdfccb6a6b2c0b6214ff67a9b75997d4f3ec1b69eb5a231553eb12d969f4a0df656e6473747265616d0a65"
    "6e646f626a0a352030206f626a0a3c3c202f4346203c3c202f5374644346203c3c202f417574684576656e74202f446f634f70656e202f43464d"
    "202f4145535633202f4c656e677468203332203e3e203e3e202f46696c746572202f5374616e64617264202f4c656e67746820323536202f4f20"
    "3c326536336461666232626665353363353265383630646431313231613765323161623462633237306631313363303163663464333538386664"
    "3864623435613837313130323966323131396532626461643638383665303531346166333636633e202f4f45203c656338393936663735386537"
    "643739656463393334303162316565633831616435343631633435323830393365373166363336636131323335633665353166343e202f50202d"
    "34202f5065726d73203c65636362373538336539613732373161636630633363653437303334313964643e202f522036202f53746d46202f5374"
    "644346202f53747246202f5374644346202f55203c36616434626135343434353532316531613236653835346433363730316134313135333061"
    "34643832353630303264653239656333383039363862623438373931653465653366333636346434323238363934633435623163643661393837"
    "623e202f5545203c3531333738386532616333643333353766393435323732383839666161663865636465396433373833383834363637646666"
    "30623836353263313066373162623e202f562035203e3e0a656e646f626a0a787265660a3020360a303030303030303030302036353533352066"
    "200a30303030303030303135203030303030206e200a30303030303030313330203030303030206e200a30303030303030313839203030303030"
    "206e200a30303030303030323738203030303030206e200a30303030303030343132203030303030206e200a747261696c6572203c3c202f526f"
    "6f74203120302052202f53697a652036202f4944205b3c33383932383663343536346266646266616361323635643536643130316138383e3c33"
    "383932383663343536346266646266616361323635643536643130316138383e5d202f456e6372797074203520302052203e3e0a737461727478"
    "7265660a3935390a2525454f460a";

// The bands of the pixel at `x`, `y` of `pixels`.
std::vector<int> pixel(const Pixels& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * pixels.size().width + x) * 4;
    std::vector<int> made;

    for (std::size_t band = 0; band < 4; ++band) made.push_back(std::to_integer<int>(pixels.stills().front().rgba[at + band]));

    return made;
}

const std::vector<int> red{255, 0, 0, 255};
const std::vector<int> blue{0, 0, 255, 255};
const std::vector<int> green{0, 255, 0, 255};
const std::vector<int> yellow{255, 255, 0, 255};
const std::vector<int> white{255, 255, 255, 255};

// PDFium as the trials use it: as `tal::docs::pdfium` gives it, except under the sanitizers, whose
// own reservations of memory its limit would count; there the same program, without the limit.
std::shared_ptr<Renderer> engine() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    tal::Command program;

    program.program = tal::testing::mine().parent() / "tal-render-pdfium";

    return tal::docs::renderer(program, [](std::span<const std::byte>) { return true; });
#else
    return tal::docs::pdfium();
#endif
}

// The document `bytes` opened by `renderer` with `password`.
std::shared_ptr<tal::docs::Document> opened(tal::Loop& loop, const std::shared_ptr<Renderer>& renderer, Bytes bytes, std::string password = {}) {
    return tal::wait(loop, renderer->opened(std::make_shared<tal::meta::Held>(std::move(bytes)), std::move(password)));
}

// tal-render-pdfium spoken to directly, frame by frame, as any program may speak to it: what it
// does with questions tal's own adapter never asks.
class Raw {
public:
    // The program started.
    explicit Raw(tal::Loop& loop) : loop_(loop) {
        tal::Command program;

        program.program = tal::testing::mine().parent() / "tal-render-pdfium";
        process_ = tal::wait(loop_, tal::Process::start(program));
    }

    // Kills the program.
    ~Raw() { process_.kill(); }

    Raw(const Raw&) = delete;
    Raw& operator=(const Raw&) = delete;

    // Asks the question `id` of `sort` with `payload`.
    void ask(tal::rendering::Sort sort, std::uint32_t id, Bytes payload = {}) {
        tal::rendering::Frame frame;

        frame.sort = sort;
        frame.id = id;
        frame.payload = std::move(payload);
        tal::wait(loop_, process_.input().write(tal::rendering::framed(frame)));
    }

    // The next frame the program says. Throws `Trouble` broken when it has ended.
    tal::rendering::Frame heard() {
        for (;;) {
            if (std::optional<tal::rendering::Frame> got = unframing_.next()) return *got;

            const Bytes more = tal::wait(loop_, process_.output().read(std::size_t{64} << 10));

            if (more.empty()) throw tal::Trouble{Kind::broken, "The program ended."};

            unframing_.add(more);
        }
    }

    // The next frame the program says besides its reads, which are answered from `document`; the
    // most any read asked for is kept in `most`.
    tal::rendering::Frame served(const Bytes& document) {
        for (;;) {
            tal::rendering::Frame got = heard();

            if (got.sort != tal::rendering::Sort::read) return got;

            tal::rendering::Reading from{got.payload};

            (void)from.u32();

            const std::uint64_t at = from.u64();
            const std::uint32_t much = from.u32();

            most = std::max(most, much);
            ask(tal::rendering::Sort::block, got.id,
                Bytes(document.begin() + static_cast<std::ptrdiff_t>(at), document.begin() + static_cast<std::ptrdiff_t>(at + much)));
        }
    }

    // The process's end, once its input is closed.
    tal::Exit closed() {
        process_.input().close();

        return tal::wait(loop_, process_.ended());
    }

    std::uint32_t most = 0;

private:
    tal::Loop& loop_;
    tal::Process process_;
    tal::rendering::Unframing unframing_;
};

// The kind and message of the trouble a failed frame carries, or "none" for another frame.
std::string troubled(const tal::rendering::Frame& frame) {
    if (frame.sort != tal::rendering::Sort::failed) return "none";

    const tal::Trouble said = tal::rendering::failure(frame.payload);

    return std::string{tal::spoken(said.kind())} + ": " + said.what();
}

// The payload of a question to open document `number` of `size` bytes with `password`.
Bytes opening(std::uint32_t number, std::uint64_t size, std::string_view password = {}) {
    return tal::rendering::Writing().u32(number).u64(size).text(password).done();
}

// The payload of a question to render `page` of document `number` at `width` by `height`, of the
// region `region`.
Bytes rendering_of(std::uint32_t number, std::uint32_t page, std::uint32_t width, std::uint32_t height, Region region = {}) {
    return tal::rendering::Writing()
        .u32(number)
        .u32(page)
        .u32(width)
        .u32(height)
        .f64(region.left)
        .f64(region.top)
        .f64(region.width)
        .f64(region.height)
        .done();
}

// A reader of `bytes` that fails at any byte from `from` to `to`.
class Torn final : public tal::meta::Reader {
public:
    // The bytes `given`, a hole from `from` to `to`.
    Torn(Bytes given, std::uint64_t from, std::uint64_t to) : bytes_(std::move(given)), from_(from), to_(to) {}

    // The size of all the bytes.
    tal::Task<std::uint64_t> size() override { co_return bytes_.size(); }

    // The bytes asked for. Throws `Trouble` missing for any in the hole.
    tal::Task<Bytes> read(std::uint64_t at, std::size_t much) override {
        if (at < to_ && at + much > from_) throw tal::Trouble{Kind::missing, "The file went away."};

        co_return Bytes(bytes_.begin() + static_cast<std::ptrdiff_t>(at), bytes_.begin() + static_cast<std::ptrdiff_t>(at + much));
    }

private:
    Bytes bytes_;
    std::uint64_t from_;
    std::uint64_t to_;
};

}

// Pages as they are shown: on white paper, with their annotations; a page turned by a quarter is as
// high as it was wide, and its picture is turned (its red left half now on top). A region of a page
// is the same part of the page rendered whole at the same scale, pixel for pixel.
TRIAL("PDFium: pages as they are shown, whole and by region") {
    tal::Loop loop;
    const auto pdf = engine();
    const auto document = opened(loop, pdf, paper({{200, 100, 0, true}, {200, 100, 90}, {300, 400, 0}}));

    OUGHT(document->pages() == (std::vector<Page>{{200, 100}, {100, 200}, {300, 400}}));

    const Pixels whole = tal::wait(loop, document->rendered(0, Size{400, 200}, Region{}));

    OUGHT(whole.size() == (Size{400, 200}));
    OUGHT(pixel(whole, 100, 100) == red);
    OUGHT(pixel(whole, 250, 50) == blue);
    OUGHT(pixel(whole, 320, 160) == green);
    OUGHT(pixel(whole, 100, 160) == yellow);
    OUGHT(pixel(whole, 100, 5) == white);

    const Pixels turned = tal::wait(loop, document->rendered(1, Size{10, 20}, Region{}));

    OUGHT(pixel(turned, 5, 2) == red);
    OUGHT(pixel(turned, 5, 17) == blue);

    const Pixels corner = tal::wait(loop, document->rendered(0, Size{200, 100}, Region{0.5, 0.5, 0.5, 0.5}));
    bool alike = true;

    for (std::uint32_t y = 0; y < 100; ++y)
        for (std::uint32_t x = 0; x < 200; ++x) alike = alike && pixel(corner, x, y) == pixel(whole, x + 200, y + 100);

    OUGHT(alike);
    OUGHT(pixel(corner, 120, 60) == green);
}

// A password: none or another one is denied in PDFium's words, the right one opens the document.
// What is no PDF is malformed; a page that cannot be read takes the size of the one before it and
// fails as malformed alone. A document whose reader fails, at its end or amid its pages, is broken
// with the reader's words, and the process lives on to open the next one.
TRIAL("PDFium: passwords, what is no PDF, a damaged page, a reader that fails") {
    tal::Loop loop;
    const auto pdf = engine();
    const Bytes locked = tal::testing::lump(locked_by_qpdf);

    SAME(refusal([&] { (void)opened(loop, pdf, locked); }), std::string{"denied: The document needs a password."});
    SAME(refusal([&] { (void)opened(loop, pdf, locked, "guess"); }), std::string{"denied: The password does not open the document."});

    const auto open = opened(loop, pdf, locked, "tal");

    OUGHT(open->pages() == (std::vector<Page>{{200, 100}}));
    OUGHT(pixel(tal::wait(loop, open->rendered(0, Size{20, 10}, Region{})), 2, 5) == red);

    SAME(refusal([&] { (void)opened(loop, pdf, bytes_of("%PDF-1.7\nnothing here\n")); }), std::string{"malformed: This is no PDF, or a damaged one."});

    Assembly gap{"1.7"};

    gap.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    gap.object(2, "<< /Type /Pages /Kids [3 0 R 99 0 R] /Count 2 >>");
    gap.object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>");
    gap.table(" /Root 1 0 R");

    const auto damaged = opened(loop, pdf, gap.bytes());

    OUGHT(damaged->pages() == (std::vector<Page>{{300, 400}, {300, 400}}));
    OUGHT(pixel(tal::wait(loop, damaged->rendered(0, Size{3, 4}, Region{})), 1, 1) == white);
    SAME(refusal([&] { (void)tal::wait(loop, damaged->rendered(1, Size{3, 4}, Region{})); }), std::string{"malformed: The page could not be read."});

    const Bytes whole = paper({{}});
    const auto torn = std::make_shared<Torn>(whole, whole.size() / 2, whole.size());

    SAME(refusal([&] { (void)tal::wait(loop, pdf->opened(torn, {})); }), std::string{"broken: The document could not be read: The file went away."});

    const Bytes book = paper(std::vector<Sheet>(500));
    const auto holed = std::make_shared<Torn>(book, book.size() * 45 / 100, book.size() * 55 / 100);

    SAME(refusal([&] { (void)tal::wait(loop, pdf->opened(holed, {})); }), std::string{"broken: The document could not be read: The file went away."});
    SAME(opened(loop, pdf, whole)->pages().size(), std::size_t{1});
}

// A stream larger than a block is read a block at a time, and its page renders.
TRIAL("PDFium: a stream larger than a block") {
    tal::Loop loop;
    const auto document = opened(loop, engine(), bulky());

    OUGHT(pixel(tal::wait(loop, document->rendered(0, Size{20, 10}, Region{})), 2, 5) == red);
}

// The program as any program may speak to it: it greets as PDFium that renders documents; reads a
// document only by asking for blocks of at most 1 MiB, putting off every other frame meanwhile,
// and taking a block only by the number of its read; answers a refused read or a block of another
// length as broken, while opening or while rendering (and renders the page when asked again);
// refuses a document number in use, a document it does not have, a page, a size or a region out of
// range, a question of a shape it should not have, and pages; closes what it is asked to, known or
// not; ends when its input does.
TRIAL("PDFium: the program spoken to directly") {
    using tal::rendering::Sort;

    tal::Loop loop;
    Raw raw{loop};
    const tal::rendering::Frame hello = raw.heard();

    OUGHT(hello.sort == Sort::hello);
    SAME(hello.id, std::uint32_t{0});

    const tal::rendering::Hello greeting = tal::rendering::greeting(hello.payload);

    SAME(greeting.version, std::uint32_t{1});
    OUGHT(greeting.engine.starts_with("PDFium 160."));
    OUGHT(greeting.abilities == tal::rendering::documents);

    const Bytes document = bulky();

    raw.ask(Sort::open, 1, opening(1, document.size()));

    const tal::rendering::Frame first = raw.heard();

    OUGHT(first.sort == Sort::read);
    raw.ask(Sort::render, 2, rendering_of(9, 0, 1, 1));

    tal::rendering::Reading asked{first.payload};

    (void)asked.u32();

    const std::uint64_t at = asked.u64();
    const std::uint32_t much = asked.u32();

    raw.ask(Sort::block, first.id + 100, Bytes(much));
    raw.ask(Sort::block, first.id, Bytes(document.begin() + static_cast<std::ptrdiff_t>(at), document.begin() + static_cast<std::ptrdiff_t>(at + much)));

    const tal::rendering::Frame opened_one = raw.served(document);

    SAME(opened_one.id, std::uint32_t{1});
    OUGHT(opened_one.sort == Sort::done);
    OUGHT(tal::rendering::paged(opened_one.payload) == (std::vector<Page>{{200, 100}}));

    const tal::rendering::Frame put_off = raw.heard();

    SAME(put_off.id, std::uint32_t{2});
    SAME(troubled(put_off), std::string{"missing: No such document."});

    raw.ask(Sort::render, 3, rendering_of(1, 0, 20, 10));

    const tal::rendering::Frame picture = raw.served(document);
    tal::rendering::Reading drawn{picture.payload};

    OUGHT(std::to_integer<int>(tal::rendering::pictured(drawn, Size{20, 10}).stills().front().rgba[(5 * 20 + 2) * 4]) == 255);
    OUGHT(raw.most > 0 && raw.most <= tal::rendering::widest_block);

    raw.ask(Sort::open, 15, opening(5, document.size()));
    OUGHT(raw.served(document).sort == Sort::done);
    raw.ask(Sort::render, 16, rendering_of(5, 0, 20, 10));

    const tal::rendering::Frame lazy = raw.heard();

    OUGHT(lazy.sort == Sort::read);
    raw.ask(Sort::unread, lazy.id, tal::rendering::failed_with(tal::Trouble{Kind::missing, "Gone for now."}));
    SAME(troubled(raw.served(document)), std::string{"broken: The document could not be read: Gone for now."});
    raw.ask(Sort::render, 17, rendering_of(5, 0, 20, 10));
    OUGHT(raw.served(document).sort == Sort::done);
    raw.ask(Sort::render, 18, Bytes(3));
    OUGHT(troubled(raw.heard()).starts_with("malformed: "));

    raw.ask(Sort::open, 4, opening(1, document.size()));
    SAME(troubled(raw.heard()), std::string{"other: A document of that number is open."});
    raw.ask(Sort::render, 5, rendering_of(1, 1, 20, 10));
    SAME(troubled(raw.heard()), std::string{"other: No such page."});
    raw.ask(Sort::render, 6, rendering_of(1, 0, 0, 10));
    SAME(troubled(raw.heard()), std::string{"other: A size of no pixels or over the ceiling."});
    raw.ask(Sort::render, 7, rendering_of(1, 0, 4000, 4000));
    SAME(troubled(raw.heard()), std::string{"other: A size of no pixels or over the ceiling."});
    raw.ask(Sort::render, 8, rendering_of(1, 0, 20, 10, Region{0.6, 0, 0.5, 1}));
    SAME(troubled(raw.heard()), std::string{"other: A region not within the page."});
    raw.ask(Sort::navigate, 9, tal::rendering::Writing().u32(1).text("file:///a").u8(0).u8(0).u32(1).u32(1).done());
    SAME(troubled(raw.heard()), std::string{"unsupported: PDFium shows no pages."});

    raw.ask(Sort::open, 10, opening(2, document.size()));

    const tal::rendering::Frame refused = raw.heard();

    OUGHT(refused.sort == Sort::read);
    raw.ask(Sort::unread, refused.id, tal::rendering::failed_with(tal::Trouble{Kind::denied, "Not for you."}));
    SAME(troubled(raw.heard()), std::string{"broken: The document could not be read: Not for you."});

    raw.ask(Sort::open, 11, opening(3, document.size()));

    const tal::rendering::Frame cut = raw.heard();

    OUGHT(cut.sort == Sort::read);
    raw.ask(Sort::block, cut.id, Bytes(1));
    SAME(troubled(raw.heard()), std::string{"broken: The document could not be read: A block of another length than asked."});

    raw.ask(Sort::close, 12, tal::rendering::Writing().u32(77).done());
    OUGHT(raw.heard().sort == Sort::done);
    raw.ask(Sort::close, 13, tal::rendering::Writing().u32(1).done());
    OUGHT(raw.heard().sort == Sort::done);
    raw.ask(Sort::render, 14, rendering_of(1, 0, 20, 10));
    SAME(troubled(raw.heard()), std::string{"missing: No such document."});
    OUGHT(raw.closed() == (tal::Exit{tal::Exit::Ended::normal, 0, 0}));
}

// Five hundred pages: all told, the last rendered. Documents open side by side in one process, and
// one let go leaves the other as it was.
TRIAL("PDFium: five hundred pages, documents side by side") {
    tal::Loop loop;
    const auto pdf = engine();
    const auto book = opened(loop, pdf, paper(std::vector<Sheet>(500)));

    SAME(book->pages().size(), std::size_t{500});
    OUGHT(pixel(tal::wait(loop, book->rendered(499, Size{20, 10}, Region{})), 2, 5) == red);

    auto first = opened(loop, pdf, paper({{}}));
    const auto second = opened(loop, pdf, paper({{200, 100, 180}}));

    OUGHT(pixel(tal::wait(loop, first->rendered(0, Size{20, 10}, Region{})), 2, 5) == red);
    OUGHT(pixel(tal::wait(loop, second->rendered(0, Size{20, 10}, Region{})), 2, 5) == blue);
    first.reset();
    OUGHT(pixel(tal::wait(loop, second->rendered(0, Size{20, 10}, Region{})), 17, 5) == red);
    OUGHT(pixel(tal::wait(loop, book->rendered(0, Size{20, 10}, Region{})), 17, 5) == blue);
}

// Five hundred pages leafed through in tal-ui on a terminal that takes pixels: the first page in
// the view as PDFium renders it (its white strip above, its red half), the last one after the end
// key, the tiles kept within a limit of 8 MiB on the way.
TRIAL("PDFium: five hundred pages leafed through in tal-ui, within the limit") {
    tal::testing::Scene scene{tal::Display{.screen = {80, 24}, .graphics = tal::Graphics::kitty, .blocks = tal::Blocks::sextants, .cell = {10, 20}}};
    tal::Loop loop{scene.time()};
    tal::Document document{scene.time()};
    const auto book = opened(loop, engine(), paper(std::vector<Sheet>(500)));
    tal::Leaves& leaves = document.children().add<tal::Leaves>(book);
    const auto settled = [&] {
        for (int round = 0; round < 50; ++round) {
            scene.wait(std::chrono::milliseconds{0});
            UNTIL(loop, !leaves.busy());
            scene.wait(std::chrono::milliseconds{0});

            if (!leaves.busy()) break;
        }
    };
    const auto picture = [&scene](tal::Spot at) -> const Pixels* {
        const std::uint32_t mark = scene.at(at).picture;
        const tal::Picture* shown = mark == 0 ? nullptr : scene.screen().picture(mark);

        return shown == nullptr || shown->pixels == nullptr ? nullptr : shown->pixels.get();
    };

    leaves.most = std::uint64_t{8} << 20;
    scene.show(document);
    leaves.focus();
    settled();

    const Pixels* first = picture({0, 0});

    OUGHT(first != nullptr);
    OUGHT(pixel(*first, 10, 10) == white);
    OUGHT(pixel(*first, 10, 200) == red);
    OUGHT(pixel(*first, 790, 200) == blue);

    scene.press(tal::Key{tal::Code::end});
    settled();

    SAME(leaves.page(), std::size_t{499});
    OUGHT(picture({0, 23}) != nullptr);
    OUGHT(leaves.held() <= *leaves.most);
    UNTIL(loop, !leaves.busy());
}

#endif


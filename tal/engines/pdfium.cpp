#include <fpdfview.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "rendering.hpp"

// tal-render-pdfium: PDF rendered by PDFium in a process of its own, speaking tal's protocol of
// renderers (RENDERING.md) over its standard streams. It reads a document only by asking the
// program for blocks of it, and touches no file. One thread: PDFium is not safe for more.
//
// PDFium asks for bytes from within its own calls (`FPDF_LoadCustomDocument`, and later while it
// renders a page whose content it has not read yet), so a question for a block is asked and its
// answer awaited right there, while the frames the program sends meanwhile wait their turn in a
// queue. A document whose reading the program refused fails the question it was asked in as broken,
// with the program's own words.
//
// A page is rendered through a matrix from the page as it is shown (PDFium turns it by its /Rotate
// itself) onto the picture: the region asked, scaled to the size asked, on white paper, with the
// annotations, straight into RGBA. The build of PDFium is the one without V8, so no PDF runs
// JavaScript or XFA here.
namespace {

namespace rendering = tal::rendering;

using rendering::Frame;
using rendering::Sort;
using tal::Bytes;
using tal::Kind;
using tal::Trouble;

// What the process keeps of the wire: the frames put off while it waited for a block, and the
// number of its last read.
struct Wire {
    std::deque<Frame> later;
    std::uint32_t asked = 0;
};

Wire wire;

// Ends the process at once with `code`, without the destructors of what it holds: it may be inside
// a call of PDFium, which the closing of its documents would pull from under it.
[[noreturn]] void gone(int code) { std::_Exit(code); }

// Reads exactly `much` bytes into `into`; ends the process when the program has closed its end.
void read_exactly(std::byte* into, std::size_t much) {
    if (much > 0 && std::fread(into, 1, much, stdin) != much) gone(0);
}

// The next frame from the program, as it comes; what came of a frame that opens a document, which
// carries a password, wiped. Ends the process when the program has closed its end or sends a frame
// no frame can be.
Frame heard() {
    std::array<std::byte, 4> length_bytes{};

    read_exactly(length_bytes.data(), length_bytes.size());

    std::uint64_t length = 0;

    for (std::size_t one = 4; one > 0; --one) length = (length << 8) | std::to_integer<std::uint64_t>(length_bytes[one - 1]);

    if (length < 5 || length > rendering::widest - 4) gone(2);

    Bytes rest(static_cast<std::size_t>(length));

    read_exactly(rest.data(), rest.size());

    Frame made;

    made.sort = static_cast<Sort>(std::to_integer<std::uint8_t>(rest[0]));
    made.id = static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(rest[1]) | std::to_integer<std::uint32_t>(rest[2]) << 8 |
                                         std::to_integer<std::uint32_t>(rest[3]) << 16 | std::to_integer<std::uint32_t>(rest[4]) << 24);
    made.payload.assign(rest.begin() + 5, rest.end());

    if (made.sort == Sort::open) tal::crypto::wiped(rest);

    return made;
}

// The next frame to handle: one put off before, or the next that comes.
Frame next() {
    if (!wire.later.empty()) {
        Frame made = std::move(wire.later.front());

        wire.later.pop_front();

        return made;
    }

    return heard();
}

// Writes the frame of `sort` for `id` with `payload`.
void said(Sort sort, std::uint32_t id, Bytes payload = {}) {
    Frame frame;

    frame.sort = sort;
    frame.id = id;
    frame.payload = std::move(payload);

    const Bytes bytes = rendering::framed(frame);

    if (std::fwrite(bytes.data(), 1, bytes.size(), stdout) != bytes.size() || std::fflush(stdout) != 0) gone(0);
}

// A password wiped when it goes out of scope, however the scope ends.
struct Forgotten {
    std::string& password;

    // Wipes the password.
    ~Forgotten() { tal::crypto::wiped(password); }
};

// Answers the question `id` with `trouble`.
void failed(std::uint32_t id, const Trouble& trouble) { said(Sort::failed, id, rendering::failed_with(trouble)); }

// A document open in PDFium: its number and size as the program gave them, how PDFium reads it,
// and what the program said when it refused a block of it, since PDFium only says that reading
// failed.
struct Opened {
    std::uint32_t number = 0;
    std::uint64_t size = 0;
    FPDF_FILEACCESS access{};
    FPDF_DOCUMENT document = nullptr;
    std::optional<Trouble> refused;

    Opened() = default;

    // Closes the document in PDFium.
    ~Opened() {
        if (document != nullptr) FPDF_CloseDocument(document);
    }

    Opened(const Opened&) = delete;
    Opened& operator=(const Opened&) = delete;
};

// Fills `into` with `much` bytes at `at` of the document `param` holds, asking the program for them
// a block at a time and putting off every other frame. Returns 0, as PDFium wants a failure, when
// the program refuses a block (kept in the document's `refused`) or sends one of another length.
int block(void* param, unsigned long at, unsigned char* into, unsigned long much) {
    Opened& opened = *static_cast<Opened*>(param);

    while (much > 0) {
        const std::uint32_t part = static_cast<std::uint32_t>(std::min<unsigned long>(much, rendering::widest_block));
        const std::uint32_t id = ++wire.asked;

        said(Sort::read, id, rendering::Writing().u32(opened.number).u64(at).u32(part).done());

        for (;;) {
            Frame got = heard();

            if (got.id != id || (got.sort != Sort::block && got.sort != Sort::unread)) {
                wire.later.push_back(std::move(got));
                continue;
            }

            if (got.sort == Sort::unread) {
                try {
                    opened.refused = rendering::failure(got.payload);
                } catch (const Trouble& why) {
                    opened.refused = why;
                }

                return 0;
            }

            if (got.payload.size() != part) {
                opened.refused = Trouble{Kind::broken, "A block of another length than asked."};

                return 0;
            }

            std::memcpy(into, got.payload.data(), part);

            break;
        }

        into += part;
        at += part;
        much -= part;
    }

    return 1;
}

std::map<std::uint32_t, std::unique_ptr<Opened>> documents;

// The trouble of a document PDFium could not open with `password` or read: the program's own
// words when it refused a block.
Trouble unopened(const Opened& opened, const std::string& password) {
    if (opened.refused) return Trouble{Kind::broken, "The document could not be read: " + std::string{opened.refused->what()}};

    switch (FPDF_GetLastError()) {
        case FPDF_ERR_FILE: return Trouble{Kind::broken, "The document could not be read."};
        case FPDF_ERR_FORMAT: return Trouble{Kind::malformed, "This is no PDF, or a damaged one."};
        case FPDF_ERR_PASSWORD: return rendering::locked(password.empty());
        case FPDF_ERR_SECURITY: return Trouble{Kind::unsupported, "The document is locked in a way PDFium does not know."};
        case FPDF_ERR_PAGE: return Trouble{Kind::malformed, "A page of the document is damaged."};
        default: return Trouble{Kind::other, "PDFium could not open the document."};
    }
}

// Opens the document the question `frame` asks for: its number, its size and its password, which
// is wiped once PDFium has had it. Answers with its pages, each as it is shown (turned), in points;
// a page PDFium cannot read takes the size of the page before it (US Letter for the first), and
// rendering it fails as malformed, so that one damaged page does not take the document with it.
void open(Frame& frame) {
    rendering::Reading from{frame.payload};
    auto opened = std::make_unique<Opened>();

    opened->number = from.u32();
    opened->size = from.u64();

    std::string password = from.text();
    const Forgotten forgotten{password};

    from.ended();
    tal::crypto::wiped(frame.payload);

    if (documents.contains(opened->number)) return failed(frame.id, Trouble{Kind::other, "A document of that number is open."});

    if (opened->size > std::numeric_limits<unsigned long>::max()) {
        return failed(frame.id, Trouble{Kind::unsupported, "The document is too large for PDFium on this system."});
    }

    opened->access.m_FileLen = static_cast<unsigned long>(opened->size);
    opened->access.m_GetBlock = block;
    opened->access.m_Param = opened.get();
    opened->document = FPDF_LoadCustomDocument(&opened->access, password.empty() ? nullptr : password.c_str());

    if (opened->document == nullptr) return failed(frame.id, unopened(*opened, password));

    tal::crypto::wiped(password);

    const int count = FPDF_GetPageCount(opened->document);

    if (count < 0 || 4 + std::uint64_t(count) * 16 > rendering::widest - rendering::head) {
        return failed(frame.id, Trouble{Kind::unsupported, "The document has more pages than tal can take."});
    }

    rendering::Writing pages;

    pages.u32(static_cast<std::uint32_t>(count));

    FS_SIZEF before{612, 792};

    for (int one = 0; one < count; ++one) {
        FS_SIZEF size{};
        const bool told = FPDF_GetPageSizeByIndexF(opened->document, one, &size) && size.width > 0 && size.height > 0 && std::isfinite(size.width) &&
                          std::isfinite(size.height);

        if (!told && opened->refused) return failed(frame.id, unopened(*opened, {}));

        if (!told) size = before;

        pages.f64(size.width).f64(size.height);
        before = size;
    }

    const std::uint32_t number = opened->number;

    documents.emplace(number, std::move(opened));
    said(Sort::done, frame.id, pages.done());
}

// A page of PDFium, closed when it goes out of scope.
struct Loaded {
    FPDF_PAGE page = nullptr;

    // Closes the page.
    ~Loaded() {
        if (page != nullptr) FPDF_ClosePage(page);
    }
};

// A bitmap of PDFium, destroyed when it goes out of scope.
struct Drawn {
    FPDF_BITMAP bitmap = nullptr;

    // Destroys the bitmap.
    ~Drawn() {
        if (bitmap != nullptr) FPDFBitmap_Destroy(bitmap);
    }
};

// Renders the page the question `frame` asks for: the document, the page, the size of the picture
// and the region of the page in fractions of it. Answers with the picture in RGBA.
void render(const Frame& frame) {
    rendering::Reading from{frame.payload};
    const std::uint32_t number = from.u32();
    const std::uint32_t index = from.u32();
    const std::uint32_t width = from.u32();
    const std::uint32_t height = from.u32();
    const double left = from.f64();
    const double top = from.f64();
    const double wide = from.f64();
    const double high = from.f64();

    from.ended();

    const auto found = documents.find(number);

    if (found == documents.end()) return failed(frame.id, Trouble{Kind::missing, "No such document."});

    Opened& opened = *found->second;

    if (index >= static_cast<std::uint32_t>(FPDF_GetPageCount(opened.document))) return failed(frame.id, Trouble{Kind::other, "No such page."});

    const std::uint64_t pixels = std::uint64_t{width} * height;

    if (pixels == 0 || pixels > tal::media::ceiling) return failed(frame.id, Trouble{Kind::other, "A size of no pixels or over the ceiling."});

    if (!(left >= 0 && top >= 0 && wide > 0 && high > 0 && left + wide <= 1 && top + high <= 1)) {
        return failed(frame.id, Trouble{Kind::other, "A region not within the page."});
    }

    opened.refused.reset();

    const Loaded loaded{FPDF_LoadPage(opened.document, static_cast<int>(index))};

    if (loaded.page == nullptr) {
        return failed(frame.id, opened.refused ? unopened(opened, {}) : Trouble{Kind::malformed, "The page could not be read."});
    }

    const double page_width = FPDF_GetPageWidthF(loaded.page);
    const double page_height = FPDF_GetPageHeightF(loaded.page);
    const Drawn drawn{FPDFBitmap_Create(static_cast<int>(width), static_cast<int>(height), 1)};

    if (drawn.bitmap == nullptr) return failed(frame.id, Trouble{Kind::full, "No memory for the picture."});

    FPDFBitmap_FillRect(drawn.bitmap, 0, 0, static_cast<int>(width), static_cast<int>(height), 0xFFFFFFFF);

    const double across = width / (wide * page_width);
    const double down = height / (high * page_height);
    const FS_MATRIX matrix{static_cast<float>(across), 0, 0, static_cast<float>(down), static_cast<float>(-left * page_width * across),
                           static_cast<float>(-top * page_height * down)};
    const FS_RECTF clip{0, 0, static_cast<float>(width), static_cast<float>(height)};

    FPDF_RenderPageBitmapWithMatrix(drawn.bitmap, loaded.page, &matrix, &clip, FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER);

    if (opened.refused) return failed(frame.id, unopened(opened, {}));

    const auto* rows = static_cast<const std::byte*>(FPDFBitmap_GetBuffer(drawn.bitmap));
    const std::size_t stride = static_cast<std::size_t>(FPDFBitmap_GetStride(drawn.bitmap));
    rendering::Writing picture;

    picture.u32(width).u32(height);

    for (std::uint32_t row = 0; row < height; ++row) picture.bytes(std::span<const std::byte>{rows + row * stride, std::size_t{width} * 4});

    said(Sort::done, frame.id, picture.done());
}

// Closes the document the question `frame` names, if it is open.
void close(const Frame& frame) {
    rendering::Reading from{frame.payload};

    documents.erase(from.u32());
    said(Sort::done, frame.id);
}

// Handles one question of the program. A question of a shape it should not have fails as
// malformed; pages PDFium does not show.
void handled(Frame& frame) {
    try {
        switch (frame.sort) {
            case Sort::open: return open(frame);
            case Sort::render: return render(frame);
            case Sort::close: return close(frame);
            case Sort::navigate:
            case Sort::shot:
            case Sort::resize:
            case Sort::dismiss: return failed(frame.id, Trouble{Kind::unsupported, "PDFium shows no pages."});
            default: return;
        }
    } catch (const Trouble& why) {
        failed(frame.id, why);
    }
}

}

// Greets the program and answers its questions until it closes the standard input.
int main() {
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif

    FPDF_LIBRARY_CONFIG config{};

    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);

    rendering::Hello hello;

    hello.version = rendering::version;
    hello.engine = "PDFium " TAL_PDFIUM_VERSION;
    hello.abilities = rendering::documents;
    said(Sort::hello, 0, rendering::greeted(hello));

    for (;;) {
        Frame frame = next();

        handled(frame);
    }
}

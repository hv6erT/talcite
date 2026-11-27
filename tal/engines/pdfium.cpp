#include <fpdf_text.h>
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
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <windows.h>
#endif

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "licenses_of.hpp"
#include "rendering.hpp"

// tal-render-pdfium: PDF rendered by PDFium in a process of its own, speaking tal's protocol of
// renderers (RENDERING.md) over its standard streams. It reads a document only by asking the
// program for blocks of it, and touches no file of the program's. One thread: PDFium is not safe
// for more. `--licenses` writes the licenses of PDFium and of what it holds, which it carries.
//
// It calls PDFium through a table of its functions (`Library`): linked in, or on Windows taken
// from the library it carries as a resource. There it unpacks the library once into the user's
// local data, into a folder named by the library's SHA-256 (tal\pdfium\<sum>\pdfium.dll), clears
// the folders of other builds, and loads it from there by its full path. Each start reads the file
// back through a handle that lets no one write it, checks its sum, and loads it while that handle
// is held, so that what it checked is what it loads; a file that differs is unpacked again, and one
// that still differs is refused.
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

// The functions of PDFium the process calls.
struct Library {
    decltype(&::FPDFBitmap_Create) FPDFBitmap_Create = nullptr;
    decltype(&::FPDFBitmap_Destroy) FPDFBitmap_Destroy = nullptr;
    decltype(&::FPDFBitmap_FillRect) FPDFBitmap_FillRect = nullptr;
    decltype(&::FPDFBitmap_GetBuffer) FPDFBitmap_GetBuffer = nullptr;
    decltype(&::FPDFBitmap_GetStride) FPDFBitmap_GetStride = nullptr;
    decltype(&::FPDF_CloseDocument) FPDF_CloseDocument = nullptr;
    decltype(&::FPDF_ClosePage) FPDF_ClosePage = nullptr;
    decltype(&::FPDF_GetLastError) FPDF_GetLastError = nullptr;
    decltype(&::FPDF_GetPageCount) FPDF_GetPageCount = nullptr;
    decltype(&::FPDF_GetPageHeightF) FPDF_GetPageHeightF = nullptr;
    decltype(&::FPDF_GetPageSizeByIndexF) FPDF_GetPageSizeByIndexF = nullptr;
    decltype(&::FPDF_GetPageWidthF) FPDF_GetPageWidthF = nullptr;
    decltype(&::FPDF_InitLibraryWithConfig) FPDF_InitLibraryWithConfig = nullptr;
    decltype(&::FPDF_LoadCustomDocument) FPDF_LoadCustomDocument = nullptr;
    decltype(&::FPDF_LoadPage) FPDF_LoadPage = nullptr;
    decltype(&::FPDF_RenderPageBitmapWithMatrix) FPDF_RenderPageBitmapWithMatrix = nullptr;
    decltype(&::FPDFText_LoadPage) FPDFText_LoadPage = nullptr;
    decltype(&::FPDFText_ClosePage) FPDFText_ClosePage = nullptr;
    decltype(&::FPDFText_CountChars) FPDFText_CountChars = nullptr;
    decltype(&::FPDFText_GetUnicode) FPDFText_GetUnicode = nullptr;
    decltype(&::FPDFText_GetLooseCharBox) FPDFText_GetLooseCharBox = nullptr;
    decltype(&::FPDF_PageToDevice) FPDF_PageToDevice = nullptr;
};

Library pdfium;

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
        if (document != nullptr) pdfium.FPDF_CloseDocument(document);
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

    switch (pdfium.FPDF_GetLastError()) {
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
    opened->document = pdfium.FPDF_LoadCustomDocument(&opened->access, password.empty() ? nullptr : password.c_str());

    if (opened->document == nullptr) return failed(frame.id, unopened(*opened, password));

    tal::crypto::wiped(password);

    const int count = pdfium.FPDF_GetPageCount(opened->document);

    if (count < 0 || 4 + std::uint64_t(count) * 16 > rendering::widest - rendering::head) {
        return failed(frame.id, Trouble{Kind::unsupported, "The document has more pages than tal can take."});
    }

    rendering::Writing pages;

    pages.u32(static_cast<std::uint32_t>(count));

    FS_SIZEF before{612, 792};

    for (int one = 0; one < count; ++one) {
        FS_SIZEF size{};
        const bool told = pdfium.FPDF_GetPageSizeByIndexF(opened->document, one, &size) && size.width > 0 && size.height > 0 && std::isfinite(size.width) &&
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
        if (page != nullptr) pdfium.FPDF_ClosePage(page);
    }
};

// A bitmap of PDFium, destroyed when it goes out of scope.
struct Drawn {
    FPDF_BITMAP bitmap = nullptr;

    // Destroys the bitmap.
    ~Drawn() {
        if (bitmap != nullptr) pdfium.FPDFBitmap_Destroy(bitmap);
    }
};

// The text of a page of PDFium, closed when it goes out of scope.
struct Lettering {
    FPDF_TEXTPAGE text = nullptr;

    // Closes the text.
    ~Lettering() {
        if (text != nullptr) pdfium.FPDFText_ClosePage(text);
    }
};

// Where the box `box` of `page`, in its user space, stands on the page as it is shown (turned by
// its rotation), in fractions of it within [0, 1]: its corners carried through PDFium's own
// transform onto a display of 2^24 by 2^24.
tal::docs::Region placed(FPDF_PAGE page, const FS_RECTF& box) {
    constexpr int display = 1 << 24;
    std::array<int, 4> corners{};

    (void)pdfium.FPDF_PageToDevice(page, 0, 0, display, display, 0, box.left, box.top, &corners[0], &corners[1]);
    (void)pdfium.FPDF_PageToDevice(page, 0, 0, display, display, 0, box.right, box.bottom, &corners[2], &corners[3]);

    const auto fraction = [](int at) { return std::clamp(static_cast<double>(at) / display, 0.0, 1.0); };
    const double left = fraction(std::min(corners[0], corners[2]));
    const double top = fraction(std::min(corners[1], corners[3]));

    return {left, top, fraction(std::max(corners[0], corners[2])) - left, fraction(std::max(corners[1], corners[3])) - top};
}

// The box that holds both `one` and `other`.
tal::docs::Region joined(const tal::docs::Region& one, const tal::docs::Region& other) {
    const double left = std::min(one.left, other.left);
    const double top = std::min(one.top, other.top);

    return {left, top, std::max(one.left + one.width, other.left + other.width) - left, std::max(one.top + one.height, other.top + other.height) - top};
}

// Answers the question `frame` for the letters of a page: the document and the page. Each letter is
// a character of the page's text as PDFium reads it, with its loose box (the whole height of its
// font) on the page as it is shown; a pair of UTF-16 halves, as PDFium gives a character beyond
// them on Windows, is one letter over both boxes, and a half alone is U+FFFD.
void letters(const Frame& frame) {
    rendering::Reading from{frame.payload};
    const std::uint32_t number = from.u32();
    const std::uint32_t index = from.u32();

    from.ended();

    const auto found = documents.find(number);

    if (found == documents.end()) return failed(frame.id, Trouble{Kind::missing, "No such document."});

    Opened& opened = *found->second;

    if (index >= static_cast<std::uint32_t>(pdfium.FPDF_GetPageCount(opened.document))) return failed(frame.id, Trouble{Kind::other, "No such page."});

    opened.refused.reset();

    const Loaded loaded{pdfium.FPDF_LoadPage(opened.document, static_cast<int>(index))};

    if (loaded.page == nullptr) {
        return failed(frame.id, opened.refused ? unopened(opened, {}) : Trouble{Kind::malformed, "The page could not be read."});
    }

    const Lettering lettering{pdfium.FPDFText_LoadPage(loaded.page)};

    if (lettering.text == nullptr) return failed(frame.id, opened.refused ? unopened(opened, {}) : Trouble{Kind::malformed, "The text of the page could not be read."});

    const int count = pdfium.FPDFText_CountChars(lettering.text);
    std::vector<tal::docs::Letter> made;

    for (int at = 0; at < count; ++at) {
        const unsigned code = pdfium.FPDFText_GetUnicode(lettering.text, at);
        FS_RECTF box{};
        tal::docs::Letter letter;

        letter.code = static_cast<char32_t>(code);

        if (pdfium.FPDFText_GetLooseCharBox(lettering.text, at, &box)) letter.box = placed(loaded.page, box);

        if (!made.empty() && code >= 0xDC00 && code <= 0xDFFF && made.back().code >= 0xD800 && made.back().code <= 0xDBFF) {
            tal::docs::Letter& high = made.back();

            high.code = static_cast<char32_t>(0x10000 + ((high.code - 0xD800) << 10U) + (code - 0xDC00));
            high.box = joined(high.box, letter.box);

            continue;
        }

        if (!made.empty() && made.back().code >= 0xD800 && made.back().code <= 0xDBFF) made.back().code = U'\uFFFD';

        if (code > 0x10FFFF || (code >= 0xDC00 && code <= 0xDFFF)) letter.code = U'\uFFFD';

        made.push_back(letter);
    }

    if (!made.empty() && made.back().code >= 0xD800 && made.back().code <= 0xDBFF) made.back().code = U'\uFFFD';

    if (opened.refused) return failed(frame.id, unopened(opened, {}));

    said(Sort::done, frame.id, rendering::letters_with(made));
}

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

    if (index >= static_cast<std::uint32_t>(pdfium.FPDF_GetPageCount(opened.document))) return failed(frame.id, Trouble{Kind::other, "No such page."});

    const std::uint64_t pixels = std::uint64_t{width} * height;

    if (pixels == 0 || pixels > tal::media::ceiling) return failed(frame.id, Trouble{Kind::other, "A size of no pixels or over the ceiling."});

    if (!(left >= 0 && top >= 0 && wide > 0 && high > 0 && left + wide <= 1 && top + high <= 1)) {
        return failed(frame.id, Trouble{Kind::other, "A region not within the page."});
    }

    opened.refused.reset();

    const Loaded loaded{pdfium.FPDF_LoadPage(opened.document, static_cast<int>(index))};

    if (loaded.page == nullptr) {
        return failed(frame.id, opened.refused ? unopened(opened, {}) : Trouble{Kind::malformed, "The page could not be read."});
    }

    const double page_width = pdfium.FPDF_GetPageWidthF(loaded.page);
    const double page_height = pdfium.FPDF_GetPageHeightF(loaded.page);
    const Drawn drawn{pdfium.FPDFBitmap_Create(static_cast<int>(width), static_cast<int>(height), 1)};

    if (drawn.bitmap == nullptr) return failed(frame.id, Trouble{Kind::full, "No memory for the picture."});

    pdfium.FPDFBitmap_FillRect(drawn.bitmap, 0, 0, static_cast<int>(width), static_cast<int>(height), 0xFFFFFFFF);

    const double across = width / (wide * page_width);
    const double down = height / (high * page_height);
    const FS_MATRIX matrix{static_cast<float>(across), 0, 0, static_cast<float>(down), static_cast<float>(-left * page_width * across),
                           static_cast<float>(-top * page_height * down)};
    const FS_RECTF clip{0, 0, static_cast<float>(width), static_cast<float>(height)};

    pdfium.FPDF_RenderPageBitmapWithMatrix(drawn.bitmap, loaded.page, &matrix, &clip, FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER);

    if (opened.refused) return failed(frame.id, unopened(opened, {}));

    const auto* rows = static_cast<const std::byte*>(pdfium.FPDFBitmap_GetBuffer(drawn.bitmap));
    const std::size_t stride = static_cast<std::size_t>(pdfium.FPDFBitmap_GetStride(drawn.bitmap));
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
            case Sort::letters: return letters(frame);
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

#ifndef _WIN32
// PDFium's functions as the program was linked with them.
Library taken() {
    Library made;

    made.FPDFBitmap_Create = &::FPDFBitmap_Create;
    made.FPDFBitmap_Destroy = &::FPDFBitmap_Destroy;
    made.FPDFBitmap_FillRect = &::FPDFBitmap_FillRect;
    made.FPDFBitmap_GetBuffer = &::FPDFBitmap_GetBuffer;
    made.FPDFBitmap_GetStride = &::FPDFBitmap_GetStride;
    made.FPDF_CloseDocument = &::FPDF_CloseDocument;
    made.FPDF_ClosePage = &::FPDF_ClosePage;
    made.FPDF_GetLastError = &::FPDF_GetLastError;
    made.FPDF_GetPageCount = &::FPDF_GetPageCount;
    made.FPDF_GetPageHeightF = &::FPDF_GetPageHeightF;
    made.FPDF_GetPageSizeByIndexF = &::FPDF_GetPageSizeByIndexF;
    made.FPDF_GetPageWidthF = &::FPDF_GetPageWidthF;
    made.FPDF_InitLibraryWithConfig = &::FPDF_InitLibraryWithConfig;
    made.FPDF_LoadCustomDocument = &::FPDF_LoadCustomDocument;
    made.FPDF_LoadPage = &::FPDF_LoadPage;
    made.FPDF_RenderPageBitmapWithMatrix = &::FPDF_RenderPageBitmapWithMatrix;
    made.FPDFText_LoadPage = &::FPDFText_LoadPage;
    made.FPDFText_ClosePage = &::FPDFText_ClosePage;
    made.FPDFText_CountChars = &::FPDFText_CountChars;
    made.FPDFText_GetUnicode = &::FPDFText_GetUnicode;
    made.FPDFText_GetLooseCharBox = &::FPDFText_GetLooseCharBox;
    made.FPDF_PageToDevice = &::FPDF_PageToDevice;

    return made;
}
#else
// The bytes of `sum` written in lowercase hexadecimal, as SHA-256 sums are written.
std::string hexadecimal(std::span<const std::byte> sum) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string made;

    for (const std::byte one : sum) {
        made.push_back(digits[std::to_integer<unsigned>(one) >> 4U]);
        made.push_back(digits[std::to_integer<unsigned>(one) & 0xFU]);
    }

    return made;
}

// The library the program carries as its resource PDFIUM, of the type RCDATA (10).
std::span<const std::byte> carried() {
    HRSRC const found = FindResourceW(nullptr, L"PDFIUM", MAKEINTRESOURCEW(10));
    HGLOBAL const held = found != nullptr ? LoadResource(nullptr, found) : nullptr;
    const void* const bytes = held != nullptr ? LockResource(held) : nullptr;

    if (bytes == nullptr) throw Trouble{Kind::missing, "The program carries no PDFium."};

    return {static_cast<const std::byte*>(bytes), SizeofResource(nullptr, found)};
}

// The folder of PDFium's builds in the user's local data (tal\pdfium), made when missing.
std::wstring builds() {
    PWSTR given = nullptr;

    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &given))) {
        CoTaskMemFree(given);

        throw Trouble{Kind::missing, "There is no folder for the user's local data."};
    }

    std::wstring made = std::wstring{given} + L"\\tal\\pdfium";

    CoTaskMemFree(given);

    return made;
}

// A file of Windows, closed when it goes out of scope.
struct Handle {
    HANDLE held = INVALID_HANDLE_VALUE;

    // Holds `given`.
    explicit Handle(HANDLE given) : held(given) {}

    // Closes the file.
    ~Handle() { close(); }

    // Closes the file now, once.
    void close() {
        if (held != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(held, INVALID_HANDLE_VALUE));
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// Writes `library` as the file `file`: into a file of its own beside it first, which then takes
// the name, over a file there when `over`, so that no one ever sees half of it. A file another
// start put there meanwhile is left as it is.
void unpacked(const std::wstring& file, std::span<const std::byte> library, bool over) {
    const std::wstring part = file + L"." + std::to_wstring(GetCurrentProcessId()) + L".part";

    {
        const Handle written{CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        DWORD wrote = 0;

        if (written.held == INVALID_HANDLE_VALUE || !WriteFile(written.held, library.data(), static_cast<DWORD>(library.size()), &wrote, nullptr) || wrote != library.size() ||
            !FlushFileBuffers(written.held))
            throw Trouble{Kind::denied, "PDFium could not be unpacked."};
    }

    if (!MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH | (over ? MOVEFILE_REPLACE_EXISTING : 0))) (void)DeleteFileW(part.c_str());
}

// The file `file` opened so that no one can write it or take its name while it is open, with the
// SHA-256 of what it holds; an invalid handle when it cannot be opened.
std::pair<HANDLE, std::string> read_back(const std::wstring& file) {
    HANDLE const held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (held == INVALID_HANDLE_VALUE) return {held, {}};

    tal::crypto::Summing summing{tal::crypto::Sum::sha256};
    std::vector<std::byte> block(1U << 16U);
    DWORD got = 0;

    while (ReadFile(held, block.data(), static_cast<DWORD>(block.size()), &got, nullptr) && got > 0) summing.feed(std::span{block.data(), got});

    return {held, hexadecimal(summing.done())};
}

// Clears from `folder` the builds of PDFium other than `kept`; one still in use stays.
void cleared(const std::wstring& folder, const std::wstring& kept) {
    WIN32_FIND_DATAW found{};
    HANDLE const walking = FindFirstFileW((folder + L"\\*").c_str(), &found);

    if (walking == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring name{found.cFileName};

        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || name == L"." || name == L".." || name == kept) continue;

        const std::wstring build = folder + L"\\" + name;
        WIN32_FIND_DATAW inside{};
        HANDLE const files = FindFirstFileW((build + L"\\*").c_str(), &inside);

        if (files != INVALID_HANDLE_VALUE) {
            do {
                if ((inside.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) (void)DeleteFileW((build + L"\\" + inside.cFileName).c_str());
            } while (FindNextFileW(files, &inside));

            FindClose(files);
        }

        (void)RemoveDirectoryW(build.c_str());
    } while (FindNextFileW(walking, &found));

    FindClose(walking);
}

// PDFium's functions taken from the library the program carries: unpacked when its file is missing
// or differs from it, checked by its SHA-256 and loaded while no one can change it, by its full
// path, its own dependencies only from its folder and the system's. Fails as broken when the
// library carried is not the one built in, or the file still differs once unpacked again.
Library taken() {
    const std::span<const std::byte> library = carried();
    const std::string sum = TAL_PDFIUM_LIBRARY_SHA256;

    if (hexadecimal(tal::crypto::summed(tal::crypto::Sum::sha256, library)) != sum) throw Trouble{Kind::broken, "The PDFium the program carries is damaged."};

    const std::wstring folder = builds();
    const std::wstring wide{sum.begin(), sum.end()};
    const std::wstring build = folder + L"\\" + wide;
    const std::wstring file = build + L"\\pdfium.dll";

    cleared(folder, wide);

    const int made_folder = SHCreateDirectoryExW(nullptr, build.c_str(), nullptr);

    if (made_folder != ERROR_SUCCESS && made_folder != ERROR_ALREADY_EXISTS && made_folder != ERROR_FILE_EXISTS)
        throw Trouble{Kind::denied, "PDFium has no folder to be unpacked into."};

    HMODULE module = nullptr;

    for (int round = 0; module == nullptr; ++round) {
        auto [held, found] = read_back(file);
        Handle checked{held};

        if (found == sum) {
            module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

            if (module == nullptr) throw Trouble{Kind::broken, "PDFium could not be loaded."};
        } else if (round > 0) {
            throw Trouble{Kind::broken, "The unpacked PDFium differs from the one the program carries."};
        } else {
            const bool there = checked.held != INVALID_HANDLE_VALUE;

            checked.close();
            unpacked(file, library, there);
        }
    }

    Library made;

    made.FPDFBitmap_Create = reinterpret_cast<decltype(made.FPDFBitmap_Create)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFBitmap_Create")));
    made.FPDFBitmap_Destroy = reinterpret_cast<decltype(made.FPDFBitmap_Destroy)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFBitmap_Destroy")));
    made.FPDFBitmap_FillRect = reinterpret_cast<decltype(made.FPDFBitmap_FillRect)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFBitmap_FillRect")));
    made.FPDFBitmap_GetBuffer = reinterpret_cast<decltype(made.FPDFBitmap_GetBuffer)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFBitmap_GetBuffer")));
    made.FPDFBitmap_GetStride = reinterpret_cast<decltype(made.FPDFBitmap_GetStride)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFBitmap_GetStride")));
    made.FPDF_CloseDocument = reinterpret_cast<decltype(made.FPDF_CloseDocument)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_CloseDocument")));
    made.FPDF_ClosePage = reinterpret_cast<decltype(made.FPDF_ClosePage)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_ClosePage")));
    made.FPDF_GetLastError = reinterpret_cast<decltype(made.FPDF_GetLastError)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_GetLastError")));
    made.FPDF_GetPageCount = reinterpret_cast<decltype(made.FPDF_GetPageCount)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_GetPageCount")));
    made.FPDF_GetPageHeightF = reinterpret_cast<decltype(made.FPDF_GetPageHeightF)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_GetPageHeightF")));
    made.FPDF_GetPageSizeByIndexF = reinterpret_cast<decltype(made.FPDF_GetPageSizeByIndexF)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_GetPageSizeByIndexF")));
    made.FPDF_GetPageWidthF = reinterpret_cast<decltype(made.FPDF_GetPageWidthF)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_GetPageWidthF")));
    made.FPDF_InitLibraryWithConfig = reinterpret_cast<decltype(made.FPDF_InitLibraryWithConfig)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_InitLibraryWithConfig")));
    made.FPDF_LoadCustomDocument = reinterpret_cast<decltype(made.FPDF_LoadCustomDocument)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_LoadCustomDocument")));
    made.FPDF_LoadPage = reinterpret_cast<decltype(made.FPDF_LoadPage)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_LoadPage")));
    made.FPDF_RenderPageBitmapWithMatrix = reinterpret_cast<decltype(made.FPDF_RenderPageBitmapWithMatrix)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_RenderPageBitmapWithMatrix")));
    made.FPDFText_LoadPage = reinterpret_cast<decltype(made.FPDFText_LoadPage)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFText_LoadPage")));
    made.FPDFText_ClosePage = reinterpret_cast<decltype(made.FPDFText_ClosePage)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFText_ClosePage")));
    made.FPDFText_CountChars = reinterpret_cast<decltype(made.FPDFText_CountChars)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFText_CountChars")));
    made.FPDFText_GetUnicode = reinterpret_cast<decltype(made.FPDFText_GetUnicode)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFText_GetUnicode")));
    made.FPDFText_GetLooseCharBox = reinterpret_cast<decltype(made.FPDFText_GetLooseCharBox)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDFText_GetLooseCharBox")));
    made.FPDF_PageToDevice = reinterpret_cast<decltype(made.FPDF_PageToDevice)>(reinterpret_cast<void*>(GetProcAddress(module, "FPDF_PageToDevice")));

    const bool whole = made.FPDFBitmap_Create != nullptr && made.FPDFBitmap_Destroy != nullptr && made.FPDFBitmap_FillRect != nullptr && made.FPDFBitmap_GetBuffer != nullptr && made.FPDFBitmap_GetStride != nullptr && made.FPDF_CloseDocument != nullptr && made.FPDF_ClosePage != nullptr && made.FPDF_GetLastError != nullptr && made.FPDF_GetPageCount != nullptr && made.FPDF_GetPageHeightF != nullptr && made.FPDF_GetPageSizeByIndexF != nullptr && made.FPDF_GetPageWidthF != nullptr && made.FPDF_InitLibraryWithConfig != nullptr && made.FPDF_LoadCustomDocument != nullptr && made.FPDF_LoadPage != nullptr && made.FPDF_RenderPageBitmapWithMatrix != nullptr && made.FPDFText_LoadPage != nullptr && made.FPDFText_ClosePage != nullptr && made.FPDFText_CountChars != nullptr && made.FPDFText_GetUnicode != nullptr && made.FPDFText_GetLooseCharBox != nullptr && made.FPDF_PageToDevice != nullptr;

    if (!whole) throw Trouble{Kind::broken, "PDFium lacks a function it should have."};

    return made;
}
#endif

// With `--licenses`, writes the licenses it carries. Otherwise takes PDFium, greets the program and
// answers its questions until it closes the standard input; a PDFium it cannot take ends it with
// the reason on the standard error and the code 1.
int main(int count, char** given) {
    if (count == 2 && std::string_view{given[1]} == "--licenses") {
        (void)std::fwrite(licenses_of::licenses.data(), 1, licenses_of::licenses.size(), stdout);

        return 0;
    }

    try {
        pdfium = taken();
    } catch (const Trouble& why) {
        (void)std::fprintf(stderr, "tal-render-pdfium: %s\n", why.what());

        return 1;
    }

#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif

    FPDF_LIBRARY_CONFIG config{};

    config.version = 2;
    pdfium.FPDF_InitLibraryWithConfig(&config);

    rendering::Hello hello;

    hello.version = rendering::version;
    hello.engine = "PDFium " TAL_PDFIUM_VERSION;
    hello.abilities = static_cast<std::uint8_t>(rendering::documents | rendering::letters);
    said(Sort::hello, 0, rendering::greeted(hello));

    for (;;) {
        Frame frame = next();

        handled(frame);
    }
}

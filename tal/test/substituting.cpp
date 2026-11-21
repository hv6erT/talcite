#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/docs.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/geometry.hpp"
#include "tal/leaves.hpp"
#include "tal/meta.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "assembling.hpp"
#include "quieting.hpp"
#include "tal/testing.hpp"

// What a viewer shows of a document (E6f): its pages through a renderer that opens it, otherwise
// its text version under the reason, a PDF's pages added as they are read; the text version of
// a page of HTML on its own; the troubles that leave nothing to show.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::Document;
using tal::Followed;
using tal::Hooks;
using tal::Node;
using tal::Size;
using tal::docs::Block;
using tal::docs::Plain;
using tal::testing::Assembly;
using tal::testing::Scene;

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

// The bytes of `text`.
Bytes bytes_of(std::string_view text) {
    return Bytes{reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

// A renderer that takes everything and opens nothing: it throws a trouble of `kind` saying
// `said`, as a chain does whose engine is not there.
class Refusing final : public tal::docs::Renderer {
public:
    // One that refuses with `said`, of the kind `kind`.
    Refusing(tal::Kind kind, std::string said) : kind_(kind), said_(std::move(said)) {}

    // Its name.
    [[nodiscard]] std::string_view name() const override { return "refusing"; }

    // Whether it takes `head`: always.
    [[nodiscard]] bool takes(std::span<const std::byte> head) const override {
        (void)head;

        return true;
    }

    // Throws its trouble when awaited.
    tal::Task<std::shared_ptr<tal::docs::Document>> opened(std::shared_ptr<tal::meta::Reader> from, std::string password) override {
        (void)from;
        (void)password;

        throw tal::Trouble{kind_, said_};

        co_return nullptr;
    }

private:
    tal::Kind kind_;
    std::string said_;
};

// A PDF of the pages `contents`, each drawn in Helvetica by its content, as it is given.
Bytes pages_of(const std::vector<std::string>& contents) {
    Assembly one{"1.7"};
    std::string kids;

    for (std::size_t at = 0; at < contents.size(); ++at) kids += std::to_string(10 + 2 * at) + " 0 R ";

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(contents.size()) + " /Resources << /Font << /F1 5 0 R >> >> >>");
    one.object(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

    for (std::size_t at = 0; at < contents.size(); ++at) {
        const auto number = static_cast<std::uint32_t>(10 + 2 * at);
        const bool broken = contents[at] == "broken";

        one.object(number, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents " + std::to_string(number + 1) + " 0 R >>");
        one.stream(number + 1, broken ? " /Filter /FlateDecode" : "", broken ? "not zlib at all" : contents[at]);
    }

    one.table(" /Root 1 0 R");

    return one.bytes();
}

// A node shown on a scene of `size`, with the links it followed; the reading of pages runs on the
// program's loop.
struct Shown {
    Scene scene;
    Document document{scene.time()};
    std::vector<std::string> followed;
    Hooks hooks;

    // `node` shown filling a scene of `size`.
    Shown(const std::shared_ptr<Node>& node, Size size) : scene({.screen = size}) {
        document.children().add(node);
        hooks.add(node->on<Followed>([this](Followed& what) { followed.push_back(what.address); }));
        scene.show(document);
        settled();
    }

    // Lets every page come, and draws.
    void settled() {
        QUIETED(tal::loop());

        scene.wait(0ms);
    }

    // The rows of the screen joined by |, their ends trimmed.
    std::string rows() {
        std::string made;

        for (int y = 0; y < scene.screen().size().high; ++y) {
            std::string row = scene.row(y);

            while (!row.empty() && row.back() == ' ') row.pop_back();

            made += (y > 0 ? "|" : "") + row;
        }

        return made;
    }
};

}

TRIAL("Preview: the text version under the line of its reason, wrapped, its links followed") {
    tal::Loop loop;
    Block heading;
    Block body;

    heading.kind = Block::Kind::heading;
    heading.level = 1;
    heading.text = "Title";
    body.text = "see the docs";
    body.anchors.push_back(tal::docs::Anchor{0, 4, 12, "https://example.test/"});

    Shown shown{tal::transcript(Plain{"page", {heading, body}, false}, "No web engine is there to show the page."), {24, 6}};

    SAME(shown.rows(), std::string{"No web engine is there|to show the page.|# Title||see the docs|"});

    shown.scene.click({6, 4});

    OUGHT(shown.followed == std::vector<std::string>{"https://example.test/"});
}

TRIAL("Preview: a document no engine opens is its text version, pages added as they come, a broken page told in its place") {
    tal::Loop loop;
    const auto made = tal::wait(loop, tal::previewed(std::make_shared<Refusing>(tal::Kind::unsupported, "No engine here."),
                                                     std::make_shared<tal::meta::Held>(pages_of({"BT /F1 10 Tf 72 700 Td (First page) Tj ET", "broken",
                                                                                                 "BT /F1 10 Tf 72 700 Td (Third page) Tj ET"}))));

    Shown shown{made, {44, 12}};

    SAME(shown.rows(), std::string{"No engine here.|\xE2\x80\x94 1 \xE2\x80\x94||First page||\xE2\x80\x94 2 \xE2\x80\x94||The content of the page could not be read.||"
                                   "\xE2\x80\x94 3 \xE2\x80\x94||Third page"});
}

TRIAL("Preview: a link on a later page of the text version is marked where its page landed, and followed") {
    tal::Loop loop;
    Assembly one{"1.7"};

    one.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    one.object(2, "<< /Type /Pages /Kids [10 0 R 12 0 R] /Count 2 /Resources << /Font << /F1 5 0 R >> >> >>");
    one.object(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 126 /Widths [" +
                      [] {
                          std::string widths;

                          for (int at = 32; at <= 126; ++at) widths += " 500";

                          return widths;
                      }() +
                      "] >>");
    one.object(10, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 11 0 R >>");
    one.stream(11, "", "BT /F1 10 Tf 72 700 Td (First page) Tj ET");
    one.object(12, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 13 0 R /Annots [<< /Subtype /Link /Rect [82 695 102 710] "
                   "/A << /S /URI /URI (https://example.test/l) >> >>] >>");
    one.stream(13, "", "BT /F1 10 Tf 72 700 Td (a link here) Tj ET");
    one.table(" /Root 1 0 R");

    Shown shown{tal::wait(loop, tal::previewed(std::make_shared<Refusing>(tal::Kind::unsupported, "No engine here."), std::make_shared<tal::meta::Held>(one.bytes()))),
                {20, 9}};

    SAME(shown.rows(), std::string{"No engine here.|\xE2\x80\x94 1 \xE2\x80\x94||First page||\xE2\x80\x94 2 \xE2\x80\x94||a link here|"});

    shown.scene.click({1, 7});

    OUGHT(shown.followed.empty());

    shown.scene.click({3, 7});

    OUGHT(shown.followed == std::vector<std::string>{"https://example.test/l"});
}

TRIAL("Preview: a document an engine opens is its pages") {
    tal::Loop loop;
    const auto made = tal::wait(loop, tal::previewed(std::make_shared<tal::docs::ScriptedRenderer>(), std::make_shared<tal::meta::Held>(bytes_of("%PDF-1.7\n"))));

    OUGHT(std::dynamic_pointer_cast<tal::Leaves>(made) != nullptr);
}

TRIAL("Preview: no text version either is the trouble with both reasons; other refusals of the engine stand") {
    tal::Loop loop;
    const auto refusing = std::make_shared<Refusing>(tal::Kind::unsupported, "No engine here.");

    SAME(refusal([&] { (void)tal::wait(loop, tal::previewed(refusing, std::make_shared<tal::meta::Held>(bytes_of("PK\x03\x04")))); }),
         std::string{"unsupported: No engine here. This is not a PDF document."});

    Assembly locked{"1.7"};

    locked.object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    locked.object(2, "<< /Type /Pages /Kids [] /Count 0 >>");
    locked.table(" /Root 1 0 R /Encrypt << /Filter /Standard /V 1 >>");
    SAME(refusal([&] { (void)tal::wait(loop, tal::previewed(refusing, std::make_shared<tal::meta::Held>(locked.bytes()))); }),
         std::string{"denied: No engine here. The document is encrypted, so it has no text version."});
    SAME(refusal([&] {
             (void)tal::wait(loop, tal::previewed(std::make_shared<Refusing>(tal::Kind::denied, "The password is wrong."),
                                                  std::make_shared<tal::meta::Held>(pages_of({"BT ET"}))));
         }),
         std::string{"denied: The password is wrong."});
}

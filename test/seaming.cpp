#include "talcite/json.hpp"
#include "talcite/picture.hpp"
#include "talcite/seam.hpp"
#include "talcite/xml.hpp"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "tal/testing.hpp"

using namespace talcite;

namespace {



// A fake codec: it does not parse XML and does not pretend to. It turns the bytes `<a/>` into a
// fixed tree with one element `a`, `refuse` into a refusal, and anything else into an empty
// document; it encodes the root's name, or refuses when there is no root. That is enough to check
// the seam without any library: no provider, a refusal, a round trip.
class Fake : public Codec<xml::Document> {
public:
    // The provider name that the seam reports.
    [[nodiscard]] std::string_view name() const override { return "fake 0.1"; }

    // Turns `<a/>` into a document with one element `a`, refuses `refuse`, and leaves anything else
    // empty; returns the refusal reason, or an empty string on success.
    [[nodiscard]] std::string decode(const Bytes& given, xml::Document& into) override {
        const std::string said = text(given);

        if (said == "refuse") return "the fake refuses";

        if (said == "<a/>") {
            xml::Node made;

            made.name = "a";

            into.nodes.push_back(std::move(made));
        }

        return {};
    }

    // Writes the root element as `<name/>`, or returns "no root" when the document has none.
    [[nodiscard]] std::string encode(const xml::Document& given, Bytes& into) override {
        const xml::Node* top = xml::root(given);

        if (top == nullptr) return "no root";

        into = bytes("<" + top->name + "/>");

        return {};
    }
};

}

// --- Codec seam -------------------------------------------------------------------------------

TRIAL("Seam: an empty codec seam refuses with Unbound on input and output") {
    const Seam<Codec<xml::Document>> none;

    OUGHT(!none.bound());

    bool amiss = false;

    try {
        (void)none.decoded(bytes("<a/>"));
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for XML.";
    }

    OUGHT(amiss);

    amiss = false;

    try {
        (void)none.encoded(xml::Document{});
    } catch (const Unbound&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Seam: the codec goes both ways through the provider") {
    const Seam<Codec<xml::Document>> codec{std::make_shared<Fake>()};

    OUGHT(codec.bound());
    SAME(std::string{codec.provider().name()}, std::string{"fake 0.1"});

    const xml::Document made = codec.decoded(bytes("<a/>"));

    OUGHT(xml::root(made) != nullptr);
    SAME(xml::root(made)->name, std::string{"a"});
    SAME(text(codec.encoded(made)), std::string{"<a/>"});
    OUGHT(codec.decoded(bytes("")).nodes.empty());
}

// A provider's refusal comes back through the seam as a reason and leaves talcite as an exception,
// with the kind's name in front, so that `what()` says which seam refused before it says why.
TRIAL("Seam: a provider's refusal comes out as Refused with the reason") {
    const Seam<Codec<xml::Document>> codec{std::make_shared<Fake>()};

    bool amiss = false;

    try {
        (void)codec.decoded(bytes("refuse"));
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "XML provider refused: the fake refuses";
    }

    OUGHT(amiss);

    amiss = false;

    try {
        (void)codec.encoded(xml::Document{});
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "XML provider refused: no root";
    }

    OUGHT(amiss);

    amiss = false;

    try {
        (void)codec.decoded(bytes("refuse"));
    } catch (const std::runtime_error&) {
        amiss = true;
    }

    OUGHT(amiss);
}

// --- XML tree ---------------------------------------------------------------------------------

namespace {

// An element with the given name and, when `text` is not empty, a single text child.
xml::Node element(std::string name, std::string text = {}) {
    xml::Node made;

    made.name = std::move(name);

    if (!text.empty()) xml::text(made, std::move(text));

    return made;
}

}

TRIAL("XML: the root is the first element among the document's nodes") {
    xml::Document one;

    OUGHT(xml::root(one) == nullptr);

    xml::Node declaration;

    declaration.kind = xml::Kind::declaration;
    declaration.name = "xml";

    xml::Node comment;

    comment.kind = xml::Kind::comment;
    comment.value = "made by";

    one.nodes.push_back(declaration);
    one.nodes.push_back(comment);

    OUGHT(xml::root(one) == nullptr);

    one.nodes.push_back(element("movie"));
    one.nodes.push_back(element("other"));

    SAME(xml::root(one)->name, std::string{"movie"});
    SAME(xml::root(std::as_const(one))->name, std::string{"movie"});
}

TRIAL("XML: child and children take only elements with the name, in order") {
    xml::Node top = element("movie");

    top.children.push_back(element("genre", "Crime"));

    xml::Node stray;

    stray.kind = xml::Kind::comment;
    stray.value = "genre";

    top.children.push_back(stray);
    top.children.push_back(element("plot", "P"));
    top.children.push_back(element("genre", "Drama"));

    OUGHT(xml::child(top, "plot") != nullptr);
    SAME(xml::text(*xml::child(top, "plot")), std::string{"P"});
    SAME(xml::text(*xml::child(top, "genre")), std::string{"Crime"});
    OUGHT(xml::child(top, "actor") == nullptr);
    OUGHT(xml::child(std::as_const(top), "plot") != nullptr);

    const std::vector<xml::Node*> genres = xml::children(top, "genre");

    SAME(genres.size(), std::size_t{2});
    SAME(xml::text(*genres[1]), std::string{"Drama"});
    OUGHT(xml::children(top, "none").empty());
}

// The text is the text and CDATA children joined in order; setting it removes them and puts one
// text at the start, leaving element children in place; an empty text adds nothing.
TRIAL("XML: text joins text with CDATA, and setting it keeps the elements") {
    xml::Node top = element("plot");

    xml::Node a;

    a.kind = xml::Kind::text;
    a.value = "a";

    xml::Node b;

    b.kind = xml::Kind::cdata;
    b.value = "b";

    top.children.push_back(a);
    top.children.push_back(element("inner", "x"));
    top.children.push_back(b);

    SAME(xml::text(top), std::string{"ab"});

    xml::text(top, "new");

    SAME(xml::text(top), std::string{"new"});
    SAME(top.children.size(), std::size_t{2});
    SAME(top.children.front().kind, xml::Kind::text);
    SAME(top.children.back().name, std::string{"inner"});

    xml::text(top, "");

    SAME(top.children.size(), std::size_t{1});
    SAME(xml::text(top), std::string{});
}

TRIAL("XML: attribute by name, setting replaces or adds") {
    xml::Node top = element("uniqueid");

    OUGHT(xml::attribute(top, "type") == nullptr);

    xml::attribute(top, "type", "tmdb");
    xml::attribute(top, "default", "true");

    SAME(*xml::attribute(top, "type"), std::string{"tmdb"});
    SAME(top.attributes.size(), std::size_t{2});

    xml::attribute(top, "type", "imdb");

    SAME(*xml::attribute(top, "type"), std::string{"imdb"});
    SAME(top.attributes.size(), std::size_t{2});
    SAME(top.attributes.front().name, std::string{"type"});
}

TRIAL("XML: documents and nodes compare by content") {
    xml::Document left;
    xml::Document right;

    left.nodes.push_back(element("a", "x"));
    right.nodes.push_back(element("a", "x"));

    OUGHT(left == right);

    xml::attribute(right.nodes.front(), "k", "v");

    OUGHT(!(left == right));
}

// --- JSON value -------------------------------------------------------------------------------

namespace {

// A fake as above: it does not parse JSON. It turns the bytes `{}` into an empty object, `1` into
// the integer one, `refuse` into a refusal; it encodes the value's type as one word.
class FakeJson : public Codec<json::Value> {
public:
    // The provider name that the seam reports.
    [[nodiscard]] std::string_view name() const override { return "fake json"; }

    // Turns `{}` into an empty object and `1` into the integer one, refuses `refuse`, and leaves
    // anything else null; returns the refusal reason, or an empty string on success.
    [[nodiscard]] std::string decode(const Bytes& given, json::Value& into) override {
        const std::string said = text(given);

        if (said == "refuse") return "the fake refuses";

        if (said == "{}") into = json::Object{};

        if (said == "1") into = 1;

        return {};
    }

    // Writes the name of the value's type for null, object and integer, and refuses any other type.
    [[nodiscard]] std::string encode(const json::Value& given, Bytes& into) override {
        switch (given.type()) {
        case json::Type::null: into = bytes("null"); break;
        case json::Type::object: into = bytes("object"); break;
        case json::Type::integer: into = bytes("integer"); break;
        default: return "unsupported";
        }

        return {};
    }
};

}

TRIAL("JSON: an empty seam refuses with Unbound naming the kind, a bound one goes both ways") {
    const Seam<Codec<json::Value>> none;

    bool amiss = false;

    try {
        (void)none.decoded(bytes("{}"));
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for JSON.";
    }

    OUGHT(amiss);

    const Seam<Codec<json::Value>> codec{std::make_shared<FakeJson>()};

    SAME(codec.decoded(bytes("{}")).type(), json::Type::object);
    SAME(codec.decoded(bytes("1")), json::Value{1});
    SAME(codec.decoded(bytes("")).type(), json::Type::null);
    SAME(text(codec.encoded(json::Value{7})), std::string{"integer"});

    amiss = false;

    try {
        (void)codec.encoded(json::Value{"x"});
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "JSON provider refused: unsupported";
    }

    OUGHT(amiss);

    amiss = false;

    try {
        (void)codec.decoded(bytes("refuse"));
    } catch (const Refused&) {
        amiss = true;
    }

    OUGHT(amiss);
}

// Each constructor lands in its own type: also `"x"`, which without a separate constructor from
// `const char*` would go to bool, and every width of integer, which without a concept would be
// ambiguous between int64, double and bool.
TRIAL("JSON: constructors pick the type unambiguously") {
    SAME(json::Value{}.type(), json::Type::null);
    SAME(json::Value{nullptr}.type(), json::Type::null);
    SAME(json::Value{true}.type(), json::Type::boolean);
    SAME(json::Value{2}.type(), json::Type::integer);
    SAME(json::Value{2L}.type(), json::Type::integer);
    SAME(json::Value{2LL}.type(), json::Type::integer);
    SAME(json::Value{std::uint8_t{2}}.type(), json::Type::integer);
    SAME(json::Value{2.5}.type(), json::Type::real);
    SAME(json::Value{2.5F}.type(), json::Type::real);
    SAME(json::Value{"x"}.type(), json::Type::string);
    SAME(json::Value{std::string{"x"}}.type(), json::Type::string);
    SAME(json::Value{json::Array{}}.type(), json::Type::array);
    SAME(json::Value{json::Object{}}.type(), json::Type::object);

    SAME(*json::Value{"x"}.string(), std::string{"x"});
    SAME(*json::Value{false}.boolean(), false);
    SAME(*json::Value{-3}.integer(), std::int64_t{-3});
    SAME(*json::Value{0.5}.real(), 0.5);
}

// Typed access: a pointer to its own type, `nullptr` for any other; `number()` combines both kinds
// of number.
TRIAL("JSON: access gives nullptr for another type, and number combines the numbers") {
    const json::Value one{2};

    OUGHT(one.integer() != nullptr);
    OUGHT(one.real() == nullptr);
    OUGHT(one.boolean() == nullptr);
    OUGHT(one.string() == nullptr);
    OUGHT(one.array() == nullptr);
    OUGHT(one.object() == nullptr);

    SAME(one.number(), std::optional<double>{2.0});
    SAME(json::Value{2.5}.number(), std::optional<double>{2.5});
    SAME(json::Value{"2"}.number(), std::optional<double>{});

    json::Value text{"a"};

    *text.string() += "b";

    SAME(*text.string(), std::string{"ab"});

    json::Value list{json::Array{}};

    list.array()->push_back(1);

    SAME(list.array()->size(), std::size_t{1});
}

// An object keeps its keys in insertion order: `insert` appends at the end and returns an existing
// entry without moving it; `erase` removes; on another type `member` and `erase` say "not there",
// and `insert` throws, because that is a programmer's mistake.
TRIAL("JSON: an object in insertion order, insert returns the existing entry") {
    json::Value one{json::Object{}};

    json::insert(one, "b") = 1;
    json::insert(one, "a") = "x";
    json::insert(one, "b") = 2;

    SAME(one.object()->size(), std::size_t{2});
    SAME(one.object()->front().name, std::string{"b"});
    SAME(*json::member(one, "b"), json::Value{2});
    SAME(*json::member(std::as_const(one), "a"), json::Value{"x"});
    OUGHT(json::member(one, "c") == nullptr);

    OUGHT(json::erase(one, "b"));
    OUGHT(!json::erase(one, "b"));
    SAME(one.object()->size(), std::size_t{1});

    json::Value number{1};

    OUGHT(json::member(number, "a") == nullptr);
    OUGHT(!json::erase(number, "a"));

    bool amiss = false;

    try {
        json::insert(number, "a");
    } catch (const std::logic_error&) {
        amiss = true;
    }

    OUGHT(amiss);
}

// Equality by content, deep; the order of keys is content, because the value preserves it.
TRIAL("JSON: values compare by content, including the order of keys") {
    json::Value left{json::Object{}};
    json::Value right{json::Object{}};

    json::insert(left, "a") = json::Array{1, "x", nullptr};
    json::insert(left, "b") = 2.5;
    json::insert(right, "a") = json::Array{1, "x", nullptr};
    json::insert(right, "b") = 2.5;

    OUGHT(left == right);

    json::insert(right, "b") = 2;

    OUGHT(!(left == right));

    json::Value swapped{json::Object{}};

    json::insert(swapped, "b") = 2.5;
    json::insert(swapped, "a") = json::Array{1, "x", nullptr};

    OUGHT(!(left == swapped));
    OUGHT(json::Value{1} == json::Value{1L});
    OUGHT(!(json::Value{1} == json::Value{1.0}));
}

// --- Third shape: a streaming decoder ------------------------------------------------------

// `Seam<ImageProvider>` has the same input and output in talcite's language as the codec seam
// (`Unbound`, `Refused`), but instead of `Made` it hands frames to a sink. The mechanics of the
// stream, the ceiling and collecting into `Pixels` are in picturing.cpp; here only what it shares
// with the standard.
TRIAL("Seam: the streaming decoder speaks the same language: Unbound, Refused, name") {
    const Seam<ImageProvider> none;

    bool amiss = false;

    try {
        (void)none.provider();
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for Image.";
    }

    OUGHT(amiss);

    const Seam<ImageProvider> images = faking::images();

    OUGHT(images.bound());
    SAME(std::string{images.provider().name()}, std::string{"fake images"});

    amiss = false;

    try {
        images.decoded(bytes("nothing"), [](const Bytes&, ui::Size, std::chrono::milliseconds) { return true; });
    } catch (const Refused& why) {
        amiss = std::string{why.what()}.starts_with("Image provider refused: ");
    }

    OUGHT(amiss);
}

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
#include "proving.hpp"

using namespace talcite;

namespace {



// Atrapa przekształcenia: **nie parsuje XML-a** i nie udaje, że to robi. Bajty `<a/>` oddaje
// jako ustalone drzewo z jednym elementem `a`, `refuse` jako odmowę, resztę jako pusty dokument;
// koduje nazwę korzenia albo odmawia, gdy korzenia nie ma. Tyle wystarcza, żeby szew sprawdził
// się bez żadnej biblioteki: brak dostawcy, odmowa, przejście w obie strony.
class Fake : public Codec<xml::Document> {
public:
    [[nodiscard]] std::string_view name() const override { return "fake 0.1"; }

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

    [[nodiscard]] std::string encode(const xml::Document& given, Bytes& into) override {
        const xml::Node* top = xml::root(given);

        if (top == nullptr) return "no root";

        into = bytes("<" + top->name + "/>");

        return {};
    }
};

}

// --- Szew przekształcenia ---------------------------------------------------------------------

TRIAL("Szew: pusty szew przekształcenia odmawia Unbound na wejściu i wyjściu") {
    const Seam<Codec<xml::Document>> none;

    ought(!none.bound());

    bool amiss = false;

    try {
        (void)none.decoded(bytes("<a/>"));
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for XML.";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)none.encoded(xml::Document{});
    } catch (const Unbound&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Szew: przekształcenie idzie w obie strony przez dostawcę") {
    const Seam<Codec<xml::Document>> codec{std::make_shared<Fake>()};

    ought(codec.bound());
    same(std::string{codec.provider().name()}, std::string{"fake 0.1"});

    const xml::Document made = codec.decoded(bytes("<a/>"));

    ought(xml::root(made) != nullptr);
    same(xml::root(made)->name, std::string{"a"});
    same(text(codec.encoded(made)), std::string{"<a/>"});
    ought(codec.decoded(bytes("")).nodes.empty());
}

// Odmowa dostawcy wraca zdaniem przez szew i wychodzi z talcite wyjątkiem — z nazwą rodzaju
// z przodu, żeby `what()` mówił, który szew odmówił, zanim powie dlaczego.
TRIAL("Szew: odmowa dostawcy wychodzi jako Refused ze zdaniem") {
    const Seam<Codec<xml::Document>> codec{std::make_shared<Fake>()};

    bool amiss = false;

    try {
        (void)codec.decoded(bytes("refuse"));
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "XML provider refused: the fake refuses";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)codec.encoded(xml::Document{});
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "XML provider refused: no root";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)codec.decoded(bytes("refuse"));
    } catch (const std::runtime_error&) {
        amiss = true;
    }

    ought(amiss);
}

// --- Drzewo XML -------------------------------------------------------------------------------

namespace {

xml::Node element(std::string name, std::string text = {}) {
    xml::Node made;

    made.name = std::move(name);

    if (!text.empty()) xml::text(made, std::move(text));

    return made;
}

}

TRIAL("XML: korzeń to pierwszy element wśród węzłów dokumentu") {
    xml::Document one;

    ought(xml::root(one) == nullptr);

    xml::Node declaration;

    declaration.kind = xml::Kind::declaration;
    declaration.name = "xml";

    xml::Node comment;

    comment.kind = xml::Kind::comment;
    comment.value = "made by";

    one.nodes.push_back(declaration);
    one.nodes.push_back(comment);

    ought(xml::root(one) == nullptr);

    one.nodes.push_back(element("movie"));
    one.nodes.push_back(element("other"));

    same(xml::root(one)->name, std::string{"movie"});
    same(xml::root(std::as_const(one))->name, std::string{"movie"});
}

TRIAL("XML: child i children biorą tylko elementy o nazwie, w kolejności") {
    xml::Node top = element("movie");

    top.children.push_back(element("genre", "Crime"));

    xml::Node stray;

    stray.kind = xml::Kind::comment;
    stray.value = "genre";

    top.children.push_back(stray);
    top.children.push_back(element("plot", "P"));
    top.children.push_back(element("genre", "Drama"));

    ought(xml::child(top, "plot") != nullptr);
    same(xml::text(*xml::child(top, "plot")), std::string{"P"});
    same(xml::text(*xml::child(top, "genre")), std::string{"Crime"});
    ought(xml::child(top, "actor") == nullptr);
    ought(xml::child(std::as_const(top), "plot") != nullptr);

    const std::vector<xml::Node*> genres = xml::children(top, "genre");

    same(genres.size(), std::size_t{2});
    same(xml::text(*genres[1]), std::string{"Drama"});
    ought(xml::children(top, "none").empty());
}

// Tekst to dzieci tekstowe i CDATA sklejone po kolei; ustawienie zdejmuje je i stawia jeden
// tekst na początku, dzieci-elementy zostawiając na miejscu; pusty tekst niczego nie dokłada.
TRIAL("XML: text skleja tekst z CDATA, a ustawienie zostawia elementy") {
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

    same(xml::text(top), std::string{"ab"});

    xml::text(top, "new");

    same(xml::text(top), std::string{"new"});
    same(top.children.size(), std::size_t{2});
    same(top.children.front().kind, xml::Kind::text);
    same(top.children.back().name, std::string{"inner"});

    xml::text(top, "");

    same(top.children.size(), std::size_t{1});
    same(xml::text(top), std::string{});
}

TRIAL("XML: atrybut po nazwie, ustawienie zastępuje albo dokłada") {
    xml::Node top = element("uniqueid");

    ought(xml::attribute(top, "type") == nullptr);

    xml::attribute(top, "type", "tmdb");
    xml::attribute(top, "default", "true");

    same(*xml::attribute(top, "type"), std::string{"tmdb"});
    same(top.attributes.size(), std::size_t{2});

    xml::attribute(top, "type", "imdb");

    same(*xml::attribute(top, "type"), std::string{"imdb"});
    same(top.attributes.size(), std::size_t{2});
    same(top.attributes.front().name, std::string{"type"});
}

TRIAL("XML: dokumenty i węzły porównują się po treści") {
    xml::Document left;
    xml::Document right;

    left.nodes.push_back(element("a", "x"));
    right.nodes.push_back(element("a", "x"));

    ought(left == right);

    xml::attribute(right.nodes.front(), "k", "v");

    ought(!(left == right));
}

// --- Wartość JSON -----------------------------------------------------------------------------

namespace {

// Atrapa jak wyżej: **nie parsuje JSON-a**. Bajty `{}` oddaje jako pusty obiekt, `1` jako
// całkowitą jedynkę, `refuse` jako odmowę; koduje typ wartości jednym słowem.
class FakeJson : public Codec<json::Value> {
public:
    [[nodiscard]] std::string_view name() const override { return "fake json"; }

    [[nodiscard]] std::string decode(const Bytes& given, json::Value& into) override {
        const std::string said = text(given);

        if (said == "refuse") return "the fake refuses";

        if (said == "{}") into = json::Object{};

        if (said == "1") into = 1;

        return {};
    }

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

TRIAL("JSON: pusty szew odmawia Unbound z nazwą rodzaju, związany idzie w obie strony") {
    const Seam<Codec<json::Value>> none;

    bool amiss = false;

    try {
        (void)none.decoded(bytes("{}"));
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for JSON.";
    }

    ought(amiss);

    const Seam<Codec<json::Value>> codec{std::make_shared<FakeJson>()};

    same(codec.decoded(bytes("{}")).type(), json::Type::object);
    same(codec.decoded(bytes("1")), json::Value{1});
    same(codec.decoded(bytes("")).type(), json::Type::null);
    same(text(codec.encoded(json::Value{7})), std::string{"integer"});

    amiss = false;

    try {
        (void)codec.encoded(json::Value{"x"});
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "JSON provider refused: unsupported";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)codec.decoded(bytes("refuse"));
    } catch (const Refused&) {
        amiss = true;
    }

    ought(amiss);
}

// Każdy konstruktor trafia do swojego typu — także `"x"`, które bez osobnego konstruktora od
// `const char*` poszłoby do bool, i każda szerokość liczby całkowitej, która bez konceptu byłaby
// dwuznaczna między int64, double i bool.
TRIAL("JSON: konstruktory wybierają typ jednoznacznie") {
    same(json::Value{}.type(), json::Type::null);
    same(json::Value{nullptr}.type(), json::Type::null);
    same(json::Value{true}.type(), json::Type::boolean);
    same(json::Value{2}.type(), json::Type::integer);
    same(json::Value{2L}.type(), json::Type::integer);
    same(json::Value{2LL}.type(), json::Type::integer);
    same(json::Value{std::uint8_t{2}}.type(), json::Type::integer);
    same(json::Value{2.5}.type(), json::Type::real);
    same(json::Value{2.5F}.type(), json::Type::real);
    same(json::Value{"x"}.type(), json::Type::string);
    same(json::Value{std::string{"x"}}.type(), json::Type::string);
    same(json::Value{json::Array{}}.type(), json::Type::array);
    same(json::Value{json::Object{}}.type(), json::Type::object);

    same(*json::Value{"x"}.string(), std::string{"x"});
    same(*json::Value{false}.boolean(), false);
    same(*json::Value{-3}.integer(), std::int64_t{-3});
    same(*json::Value{0.5}.real(), 0.5);
}

// Dostęp typowany: wskaźnik na swój typ, `nullptr` na każdy inny; `number()` łączy obie liczby.
TRIAL("JSON: dostęp oddaje nullptr na cudzy typ, a number łączy liczby") {
    const json::Value one{2};

    ought(one.integer() != nullptr);
    ought(one.real() == nullptr);
    ought(one.boolean() == nullptr);
    ought(one.string() == nullptr);
    ought(one.array() == nullptr);
    ought(one.object() == nullptr);

    same(one.number(), std::optional<double>{2.0});
    same(json::Value{2.5}.number(), std::optional<double>{2.5});
    same(json::Value{"2"}.number(), std::optional<double>{});

    json::Value text{"a"};

    *text.string() += "b";

    same(*text.string(), std::string{"ab"});

    json::Value list{json::Array{}};

    list.array()->push_back(1);

    same(list.array()->size(), std::size_t{1});
}

// Obiekt trzyma klucze w kolejności wstawienia: `insert` dopisuje na końcu i oddaje istniejący
// wpis bez zmiany miejsca; `erase` usuwa; na innym typie `member` i `erase` mówią „nie ma",
// a `insert` rzuca, bo to pomyłka programisty.
TRIAL("JSON: obiekt w porządku wstawienia, insert oddaje istniejący wpis") {
    json::Value one{json::Object{}};

    json::insert(one, "b") = 1;
    json::insert(one, "a") = "x";
    json::insert(one, "b") = 2;

    same(one.object()->size(), std::size_t{2});
    same(one.object()->front().name, std::string{"b"});
    same(*json::member(one, "b"), json::Value{2});
    same(*json::member(std::as_const(one), "a"), json::Value{"x"});
    ought(json::member(one, "c") == nullptr);

    ought(json::erase(one, "b"));
    ought(!json::erase(one, "b"));
    same(one.object()->size(), std::size_t{1});

    json::Value number{1};

    ought(json::member(number, "a") == nullptr);
    ought(!json::erase(number, "a"));

    bool amiss = false;

    try {
        json::insert(number, "a");
    } catch (const std::logic_error&) {
        amiss = true;
    }

    ought(amiss);
}

// Równość po treści, w głąb; porządek kluczy jest treścią, bo wartość go zachowuje.
TRIAL("JSON: wartości porównują się po treści, z porządkiem kluczy") {
    json::Value left{json::Object{}};
    json::Value right{json::Object{}};

    json::insert(left, "a") = json::Array{1, "x", nullptr};
    json::insert(left, "b") = 2.5;
    json::insert(right, "a") = json::Array{1, "x", nullptr};
    json::insert(right, "b") = 2.5;

    ought(left == right);

    json::insert(right, "b") = 2;

    ought(!(left == right));

    json::Value swapped{json::Object{}};

    json::insert(swapped, "b") = 2.5;
    json::insert(swapped, "a") = json::Array{1, "x", nullptr};

    ought(!(left == swapped));
    ought(json::Value{1} == json::Value{1L});
    ought(!(json::Value{1} == json::Value{1.0}));
}

// --- Trzeci kształt: dekoder strumieniowy ------------------------------------------------------

// `Seam<ImageProvider>` ma to samo wejście i wyjście w języku talcite, co szew przekształcenia
// (`Unbound`, `Refused`), ale zamiast `Made` oddaje klatki do ujścia. Mechanika strumienia,
// sufitu i zbierania w `Pixels` jest w `picturing.cpp`; tu tylko to, co wspólne ze standardem.
TRIAL("Szew: dekoder strumieniowy mówi tym samym językiem — Unbound, Refused, name") {
    const Seam<ImageProvider> none;

    bool amiss = false;

    try {
        (void)none.provider();
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for Image.";
    }

    ought(amiss);

    const Seam<ImageProvider> images = faking::images();

    ought(images.bound());
    same(std::string{images.provider().name()}, std::string{"fake images"});

    amiss = false;

    try {
        images.decoded(bytes("nothing"), [](const Bytes&, ui::Size, std::chrono::milliseconds) { return true; });
    } catch (const Refused& why) {
        amiss = std::string{why.what()}.starts_with("Image provider refused: ");
    }

    ought(amiss);
}

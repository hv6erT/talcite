#include <string>
#include <vector>

#include "tal/trouble.hpp"
#include "tal/xml.hpp"

#include "tal/testing.hpp"

// XML: the reader against what the files people keep hold and against hostile text, the writer
// against the reader, and the walking of the tree.

namespace {

using tal::xml::Document;
using tal::xml::Kind;
using tal::xml::Node;

// What reading `text` within `limits` threw: its kind and message, or "none".
std::string refusal(std::string_view text, tal::xml::Limits limits = {}) {
    try {
        (void)tal::xml::parsed(text, limits);
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

// The kind of what reading `text` threw, or "none".
std::string kind_of(std::string_view text, tal::xml::Limits limits = {}) {
    try {
        (void)tal::xml::parsed(text, limits);
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())};
    }

    return "none";
}

// A node of the kind `kind` holding `value`.
Node made(Kind kind, std::string value, std::string name = {}) {
    Node one;

    one.kind = kind;
    one.value = std::move(value);
    one.name = std::move(name);

    return one;
}

// An element `name` with `children`.
Node element(std::string name, std::vector<Node> children = {}) {
    Node one;

    one.name = std::move(name);
    one.children = std::move(children);

    return one;
}

}

TRIAL("XML: a file of Kodi is read whole: the declaration, comments, attributes, text and CDATA") {
    const Document one = tal::xml::parsed("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" standalone='yes'?>\r\n"
                                          "<!-- made by twa -->\r\n"
                                          "<movie>\r\n"
                                          "  <title>Poirot &amp; Hastings</title>\r\n"
                                          "  <plot>a<![CDATA[<b>]]>c</plot>\r\n"
                                          "  <uniqueid type=\"tmdb\" default=\"true\">123</uniqueid>\r\n"
                                          "  <empty/>\r\n"
                                          "</movie>\r\n");

    SAME(one.nodes.size(), std::size_t{3});
    OUGHT(one.nodes[0].kind == Kind::declaration);
    SAME(one.nodes[0].name, std::string{"xml"});
    SAME(one.nodes[0].attributes.size(), std::size_t{3});
    SAME(one.nodes[0].attributes[2].value, std::string{"yes"});
    OUGHT(one.nodes[1].kind == Kind::comment);
    SAME(one.nodes[1].value, std::string{" made by twa "});

    const Node* movie = tal::xml::root(one);

    OUGHT(movie != nullptr);
    SAME(movie->name, std::string{"movie"});
    SAME(movie->children.size(), std::size_t{4});
    SAME(tal::xml::text(*tal::xml::child(*movie, "title")), std::string{"Poirot & Hastings"});
    SAME(tal::xml::text(*tal::xml::child(*movie, "plot")), std::string{"a<b>c"});
    SAME(tal::xml::child(*movie, "plot")->children.size(), std::size_t{3});
    OUGHT(tal::xml::child(*movie, "plot")->children[1].kind == Kind::cdata);
    SAME(*tal::xml::attribute(*tal::xml::child(*movie, "uniqueid"), "type"), std::string{"tmdb"});
    OUGHT(tal::xml::attribute(*tal::xml::child(*movie, "uniqueid"), "lang") == nullptr);
    OUGHT(tal::xml::child(*movie, "empty")->children.empty());
    OUGHT(tal::xml::child(*movie, "none") == nullptr);
}

TRIAL("XML: references, line ends and white space in values as the standard reads them") {
    const Document one = tal::xml::parsed("<a b=\"x&#9;y\r\nz\tw &lt;&#x41;&#66;&quot;&apos;\">1&#10;2\r3\r\n4 &gt; &nbsp; &amp &#x1F600;</a>");
    const Node& a = one.nodes[0];

    SAME(a.attributes[0].value, std::string{"x\ty z w <AB\"'"});
    SAME(tal::xml::text(a), std::string{"1\n2\n3\n4 > &nbsp; &amp \xF0\x9F\x98\x80"});
    SAME(tal::xml::text(tal::xml::parsed("<a>&#00000000065;&x;&</a>").nodes[0]), std::string{"A&x;&"});
    SAME(tal::xml::text(tal::xml::parsed("<a>&#x1f600;&#xa;</a>").nodes[0]), std::string{"\xF0\x9F\x98\x80\n"});

    const Document lined = tal::xml::parsed("<a><!--x\r\ny\rz--><![CDATA[p\r\nq\rr]]><?pi s\r\nt?></a>");

    SAME(lined.nodes[0].children[0].value, std::string{"x\ny\nz"});
    SAME(lined.nodes[0].children[1].value, std::string{"p\nq\nr"});
    SAME(lined.nodes[0].children[2].value, std::string{"s\nt"});

    const Document spaced = tal::xml::parsed("<a>\n  <b> </b>\n  <c>&#32;</c>\n  x <d/> y\n</a>");

    SAME(spaced.nodes[0].children.size(), std::size_t{5});
    OUGHT(spaced.nodes[0].children[0].kind == Kind::element);
    OUGHT(spaced.nodes[0].children[0].children.empty());
    SAME(tal::xml::text(spaced.nodes[0].children[1]), std::string{" "});
    SAME(spaced.nodes[0].children[2].value, std::string{"\n  x "});
    SAME(spaced.nodes[0].children[4].value, std::string{" y\n"});
}

TRIAL("XML: instructions, a doctype with its subset raw, and what pugixml took at the top level") {
    const Document one = tal::xml::parsed("<?xml version='1.0'?><!DOCTYPE note [ <!ENTITY a \"b>c\"> ]>"
                                          "<?style  href=\"x\" ?><note>&a;</note><extra/>https://example.org/1\n<!--end-->");

    SAME(one.nodes.size(), std::size_t{7});
    OUGHT(one.nodes[1].kind == Kind::doctype);
    SAME(one.nodes[1].value, std::string{"note [ <!ENTITY a \"b>c\"> ]"});
    OUGHT(one.nodes[2].kind == Kind::instruction);
    SAME(one.nodes[2].name, std::string{"style"});
    SAME(one.nodes[2].value, std::string{"href=\"x\" "});
    SAME(tal::xml::text(one.nodes[3]), std::string{"&a;"});
    SAME(one.nodes[4].name, std::string{"extra"});
    OUGHT(one.nodes[5].kind == Kind::text);
    SAME(one.nodes[5].value, std::string{"https://example.org/1"});
    OUGHT(one.nodes[6].kind == Kind::comment);

    SAME(tal::xml::parsed("<?pi?><a/>").nodes[0].value, std::string{});
    OUGHT(tal::xml::parsed("<?xml?><a/>").nodes[0].kind == Kind::declaration);
    SAME(tal::xml::parsed("<!DOCTYPE html><a/>").nodes[0].value, std::string{"html"});
}

TRIAL("XML: mistakes are malformed and say the line and the column in characters") {
    SAME(refusal("<a>\n  <ż>x</b>\n</a>"), std::string{"malformed: line 2, column 10: the closing tag is not of \"ż\""});
    SAME(refusal(""), std::string{"malformed: line 1, column 1: the document has no element"});
    SAME(refusal("  text <!--c-->"), std::string{"malformed: line 1, column 16: the document has no element"});

    for (const char* bad : {"<a>", "<a></a", "<a b></a>", "<a b=c></a>", "<a b=\"c></a>", "<a b=\"<\"/>", "<a b='1' b='2'/>", "<a b='1'c='2'/>",
                            "<1a/>", "<a/></b>", "<a><![CDATA[x</a>", "<![CDATA[x]]><a/>", "<a><!--x</a>", "<a><?pi x</a>", "<a/><?xml version='1.0'?>",
                            "<a/><?XmL x?>", "<?pi\"x\"?><a/>", "<a/><!DOCTYPE a>", "<!DOCTYPE a><!DOCTYPE a><a/>", "<!DOCTYPE a [<a/>", "<!DOCTYPE a \"x><a/>",
                            "<a><!ELEMENT a></a>", "<a>&#;</a>", "<a>&#x;</a>", "<a>&#12a;</a>", "<a>&#xG;</a>", "<a>&#0;</a>", "<a>&#xD800;</a>",
                            "<a>&#x110000;</a>", "<a>&#99999999999;</a>", "<a>&#4294967361;</a>", "<a>&#12A;</a>", "<a>&#xFFFE;</a>", "<a>&#1;</a>", "<a>\xC3</a>", "<a/ >", "<a b='1'?>",
                            "<?xml version='1.0'", "<a><!--a--b--></a>", "<!--a---><a/>"})
        SAME(kind_of(bad), std::string{"malformed"});

    SAME(kind_of("<a>&#9;&#xA;&#13;&#x10FFFF;&#xFFFD;</a>"), std::string{"none"});
    SAME(refusal("<![CDATA[x]]><a/>"), std::string{"malformed: line 1, column 1: CDATA stands only inside an element"});
    OUGHT(refusal("<a>&#;</a>").ends_with("a reference to a character has no digits"));
    OUGHT(refusal("<a><!ELEMENT a></a>").ends_with("an unknown declaration"));
    SAME(kind_of("<a b = \"1\" c='2' ></a >"), std::string{"none"});
    SAME(kind_of("<?xml version=\"1.0\" encoding=\"utf8\"?><a/>"), std::string{"none"});
    SAME(refusal("<?xml version=\"1.0\" encoding=\"ISO-8859-2\"?><a/>"), std::string{"unsupported: The document is in ISO-8859-2, not UTF-8."});
}

TRIAL("XML: limits of size, depth and nodes, and no quadratic time on many '&'") {
    SAME(kind_of("<a/>", {.bytes = 3}), std::string{"malformed"});
    SAME(kind_of("<a/>", {.bytes = 4}), std::string{"none"});
    SAME(kind_of("<a><b><c/></b></a>", {.depth = 2}), std::string{"malformed"});
    SAME(kind_of("<a><b><c/></b></a>", {.depth = 3}), std::string{"none"});
    SAME(kind_of("<a x='1'>t<!--c--></a>", {.nodes = 3}), std::string{"malformed"});
    SAME(kind_of("<a x='1'>t<!--c--></a>", {.nodes = 4}), std::string{"none"});
    SAME(kind_of("<?xml version='1.0'?><a/>", {.nodes = 2}), std::string{"malformed"});
    SAME(kind_of("<!DOCTYPE a><a/>", {.nodes = 1}), std::string{"malformed"});
    SAME(kind_of("<a><![CDATA[x]]></a>", {.nodes = 1}), std::string{"malformed"});
    SAME(refusal(std::string(200, '<')), std::string{"malformed: line 1, column 2: a name is expected"});

    std::string deep;

    for (int at = 0; at < 100000; ++at) deep += "<a>";

    SAME(kind_of(deep), std::string{"malformed"});

    std::string ands = "<a>" + std::string(std::size_t{4} << 20, '&') + "</a>";

    SAME(tal::xml::text(tal::xml::parsed(ands).nodes[0]).size(), std::size_t{4} << 20);
}

TRIAL("XML: written as pugixml wrote it: a declaration first, two spaces a level, text in one piece") {
    Document one;
    Node movie = element("movie", {element("title", {made(Kind::text, "A & <B> \"c\"")}), element("empty"),
                                   element("plot", {made(Kind::text, "x "), element("b", {element("i")}), made(Kind::cdata, "y]]>z")})});

    tal::xml::attribute(movie.children[1], "a", "1\t2\n3\r\"<&>'");
    one.nodes = {made(Kind::comment, " c "), movie};

    const std::string text = tal::xml::written(one);

    SAME(text, std::string{"<?xml version=\"1.0\"?>\n"
                           "<!-- c -->\n"
                           "<movie>\n"
                           "  <title>A &amp; &lt;B&gt; \"c\"</title>\n"
                           "  <empty a=\"1&#9;2&#10;3&#13;&quot;&lt;&amp;&gt;'\" />\n"
                           "  <plot>x <b><i /></b><![CDATA[y]]]]><![CDATA[>z]]></plot>\n"
                           "</movie>\n"});

    const Document back = tal::xml::parsed(text);

    SAME(back.nodes.size(), std::size_t{3});
    SAME(back.nodes[1], one.nodes[0]);
    SAME(*tal::xml::attribute(back.nodes[2].children[1], "a"), std::string{"1\t2\n3\r\"<&>'"});
    SAME(tal::xml::text(back.nodes[2].children[0]), std::string{"A & <B> \"c\""});
    SAME(tal::xml::text(back.nodes[2].children[2]), std::string{"x y]]>z"});

    Document declared;
    Node declaration = made(Kind::declaration, "");

    tal::xml::attribute(declaration, "version", "1.0");
    tal::xml::attribute(declaration, "encoding", "UTF-8");
    declared.nodes = {declaration, made(Kind::doctype, "a"), made(Kind::instruction, "x", "pi"), made(Kind::instruction, "", "pj"), element("a"),
                      made(Kind::text, "u\r\n")};

    SAME(tal::xml::written(declared), std::string{"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE a>\n<?pi x?>\n<?pj?>\n<a />\nu&#13;&#10;\n"});
    SAME(tal::xml::parsed(tal::xml::written(declared)).nodes[5].value, std::string{"u\r\n"});

    Document edges;

    edges.nodes = {element("a"), made(Kind::text, " \t\nx y\r\n "), element("b")};

    SAME(tal::xml::written(edges), std::string{"<?xml version=\"1.0\"?>\n<a />\n&#32;&#9;&#10;x y&#13;&#10;&#32;\n<b />\n"});
    SAME(tal::xml::parsed(tal::xml::written(edges)).nodes[2].value, std::string{" \t\nx y\r\n "});

    Document blank;

    blank.nodes = {element("a", {made(Kind::text, " \n"), element("b")})};

    SAME(tal::xml::written(blank), std::string{"<?xml version=\"1.0\"?>\n<a>&#32;&#10;<b /></a>\n"});

    blank.nodes = {element("p", {made(Kind::cdata, "x"), element("b")})};

    SAME(tal::xml::written(blank), std::string{"<?xml version=\"1.0\"?>\n<p><![CDATA[x]]><b /></p>\n"});
    SAME(tal::xml::parsed(tal::xml::written(blank)).nodes[1], blank.nodes[0]);
}

TRIAL("XML: what XML cannot hold is refused by the writer") {
    const auto refused = [](Node one) {
        Document held;

        held.nodes = {std::move(one)};

        try {
            (void)tal::xml::written(held);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    };

    OUGHT(refused(element("")));
    OUGHT(refused(element("1a")));
    OUGHT(refused(element("a b")));
    OUGHT(refused(element("a\xC3")));
    OUGHT(refused(made(Kind::comment, "a--b")));
    OUGHT(refused(made(Kind::comment, "a-")));
    OUGHT(refused(made(Kind::comment, "\xC3")));
    OUGHT(refused(made(Kind::instruction, "a?>", "pi")));
    OUGHT(refused(made(Kind::instruction, "a", "")));
    OUGHT(refused(made(Kind::instruction, "\xC3", "pi")));
    OUGHT(refused(made(Kind::text, "\xC3")));
    OUGHT(refused(made(Kind::cdata, "\xC3")));
    OUGHT(refused(made(Kind::doctype, "\xC3")));
    OUGHT(refused(element("a", {made(Kind::text, "\xFF")})));

    Node attributed = element("a");

    tal::xml::attribute(attributed, "b c", "1");

    OUGHT(refused(attributed));

    attributed.attributes = {{"b", "\xC3"}};

    OUGHT(refused(attributed));
    OUGHT(!refused(element("_a-1.b:c\xC5\xBC")));

    Document late;

    late.nodes = {element("a"), made(Kind::declaration, "", "xml")};

    try {
        (void)tal::xml::written(late);
        OUGHT(false);
    } catch (const std::invalid_argument&) {
    }
}

TRIAL("XML: the tree is walked by name; text set keeps child elements, attributes set replace") {
    Node actor = element("actor", {made(Kind::text, "old"), element("role"), made(Kind::cdata, "more"), element("role"), made(Kind::comment, "c")});

    SAME(tal::xml::children(actor, "role").size(), std::size_t{2});
    SAME(tal::xml::children(static_cast<const Node&>(actor), "role").size(), std::size_t{2});
    OUGHT(tal::xml::children(actor, "name").empty());
    OUGHT(tal::xml::children(static_cast<const Node&>(actor), "c").empty());

    tal::xml::text(actor, "Name");

    SAME(actor.children.size(), std::size_t{4});
    SAME(tal::xml::text(actor), std::string{"Name"});
    OUGHT(actor.children[0].kind == Kind::text);

    tal::xml::text(actor, "");

    SAME(actor.children.size(), std::size_t{3});
    SAME(tal::xml::text(actor), std::string{});

    tal::xml::attribute(actor, "a", "1");
    tal::xml::attribute(actor, "b", "2");
    tal::xml::attribute(actor, "a", "3");

    SAME(actor.attributes.size(), std::size_t{2});
    SAME(*tal::xml::attribute(actor, "a"), std::string{"3"});

    Document document;

    OUGHT(tal::xml::root(document) == nullptr);
    OUGHT(tal::xml::root(static_cast<const Document&>(document)) == nullptr);

    document.nodes = {made(Kind::comment, "c"), element("first"), element("second")};

    SAME(tal::xml::root(document)->name, std::string{"first"});
    SAME(tal::xml::root(static_cast<const Document&>(document))->name, std::string{"first"});
}

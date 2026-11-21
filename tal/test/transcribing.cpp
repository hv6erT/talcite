#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/docs.hpp"
#include "tal/pages.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// Text versions (E6f): HTML read into blocks as a browser decodes and tokenizes it, against pages
// of every kind of content and against hostile ones; the bounds.

namespace {

using tal::docs::Anchor;
using tal::docs::Block;
using tal::docs::Bounds;
using tal::docs::Encoding;
using tal::docs::Plain;

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
std::span<const std::byte> bytes_of(std::string_view text) { return {reinterpret_cast<const std::byte*>(text.data()), text.size()}; }

// The text version of `html` read at `address`.
Plain read(std::string_view html, std::string_view address = {}, Encoding fallback = Encoding::windows_1252, Bounds bounds = {}) {
    return tal::docs::plain(bytes_of(html), address, fallback, bounds);
}

// The blocks of `plain` in short: each as its kind, level and mark, then its text or its cells
// joined by '|', the blocks joined by " / ".
std::string shown(const Plain& plain) {
    std::string made;

    for (const Block& one : plain.blocks) {
        if (!made.empty()) made += " / ";

        switch (one.kind) {
            case Block::Kind::paragraph: made += "p"; break;
            case Block::Kind::heading: made += "h" + std::to_string(one.level); break;
            case Block::Kind::item: made += "li" + std::to_string(one.level) + one.mark; break;
            case Block::Kind::quote: made += "q" + std::to_string(one.level); break;
            case Block::Kind::preformatted: made += "pre"; break;
            case Block::Kind::row: made += "row"; break;
            case Block::Kind::rule: made += "hr"; break;
            case Block::Kind::page: made += "page"; break;
        }

        made += ":";

        if (one.kind == Block::Kind::row) {
            for (std::size_t at = 0; at < one.cells.size(); ++at) made += (at > 0 ? "|" : "") + one.cells[at];
        } else {
            made += one.text;
        }
    }

    return made;
}

// The texts the anchors of `block` cover, each with its address: "text>address", joined by ' '.
std::string linked(const Block& block) {
    std::string made;

    for (const Anchor& one : block.anchors) {
        const std::string& body = block.kind == Block::Kind::row ? block.cells.at(one.cell) : block.text;

        if (!made.empty()) made += " ";

        made += body.substr(one.from, one.upto - one.from) + ">" + one.address;
    }

    return made;
}

}

TRIAL("HTML text: paragraphs, headings and breaks, whitespace collapsed, the title kept apart") {
    const Plain one = read("<!DOCTYPE html><html><head><title> Strona \n domowa </title><meta charset=utf-8>"
                           "<style>p { color: red }</style><script>document.title = 'x'</script></head>"
                           "<body><h1>Nag\xC5\x82\xC3\xB3wek</h1>\n<p>Pierwszy   akapit\n ci\xC4\x85g dalszy.<p>Drugi<br>wiersz<br></p>"
                           "<div>Blok <b>pogrubiony</b> i <i>pochy\xC5\x82y</i></div>Tekst lu\xC5\xBAny<h2>Dwa</h2><h3>Trzy</h3></body></html>");

    SAME(one.title, std::string{"Strona domowa"});
    SAME(shown(one), std::string{"h1:Nag\xC5\x82\xC3\xB3wek / p:Pierwszy akapit ci\xC4\x85g dalszy. / p:Drugi\nwiersz / "
                                 "p:Blok pogrubiony i pochy\xC5\x82y / p:Tekst lu\xC5\xBAny / h2:Dwa / h3:Trzy"});
    OUGHT(!one.cut);
}

TRIAL("HTML text: lists nested and numbered from their start and value, items of many paragraphs, definitions") {
    const Plain one = read("<ul><li>raz<li>dwa<ol start=4><li>cztery<li value=9>dziewi\xC4\x99\xC4\x87<li>dziesi\xC4\x99\xC4\x87</ol>"
                           "<li><p>pierwszy akapit</p><p>drugi akapit</p></ul>"
                           "<dl><dt>Termin<dd>Opis</dl><blockquote><p>Cytat<blockquote>w cytacie</blockquote></blockquote>");

    SAME(shown(one), std::string{"li1\xE2\x80\xA2:raz / li1\xE2\x80\xA2:dwa / li2" "4.:cztery / li2" "9.:dziewi\xC4\x99\xC4\x87 / li2" "10.:dziesi\xC4\x99\xC4\x87 / "
                                 "li1\xE2\x80\xA2:pierwszy akapit / li1:drugi akapit / p:Termin / li1:Opis / q1:Cytat / q2:w cytacie"});
}

TRIAL("HTML text: character references by name (with and without the semicolon, the longest), by number, in attributes") {
    const Plain one = read("<p>&lt;a&gt; &amp;amp &copy &notit; &notin; &#65;&#x42;&#X43; &#128; &#0; &#xD800; &#x110000; &#; &bogus; &"
                           " <img alt='&not=x &notx &not; &amp'>");

    SAME(shown(one), std::string{"p:<a> &amp \xC2\xA9 \xC2\xACit; \xE2\x88\x89 ABC \xE2\x82\xAC \xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD &#; &bogus; & "
                                 "[&not=x &notx \xC2\xAC &]"});
}

TRIAL("HTML text: the encoding from a byte order mark, a declaration, the whole file as UTF-8, or the fallback") {
    SAME(shown(read("\xEF\xBB\xBF<p>\xC5\xBC\xC3\xB3\xC5\x82w")), std::string{"p:\xC5\xBC\xC3\xB3\xC5\x82w"});
    SAME(shown(read(std::string_view{"\xFF\xFE<\0p\0>\0\x7C\x01\x3D\xD8\x00\xDE", 14})), std::string{"p:\xC5\xBC\xF0\x9F\x98\x80"});
    SAME(shown(read(std::string_view{"\xFE\xFF\0<\0p\0>\x01\x7C", 10})), std::string{"p:\xC5\xBC"});
    SAME(shown(read("<meta charset=\"windows-1250\"><p>\xBF\xF3\xB3w \x9C")), std::string{"p:\xC5\xBC\xC3\xB3\xC5\x82w \xC5\x9B"});
    SAME(shown(read("<meta http-equiv=Content-Type content='text/html; charset=ISO-8859-2'><p>\xBF\xB1")), std::string{"p:\xC5\xBC\xC4\x85"});
    SAME(shown(read("<meta charset=latin1><p>\x80\xE9")), std::string{"p:\xE2\x82\xAC\xC3\xA9"});
    SAME(shown(read("<meta charset=utf-16><p>\xC3\xA9")), std::string{"p:\xC3\xA9"});
    SAME(shown(read("<!-- <meta charset=windows-1250> --><meta charset=klingon><p>\xC3\xA9")), std::string{"p:\xC3\xA9"});
    SAME(shown(read("<p>\xBF\xF3\xB3w", {}, Encoding::windows_1250)), std::string{"p:\xC5\xBC\xC3\xB3\xC5\x82w"});
    SAME(shown(read("<p>\xBF\xF3\xB3w")), std::string{"p:\xC2\xBF\xC3\xB3\xC2\xB3w"});
    SAME(shown(read("<p>\xC3\xA9 is UTF-8 throughout", {}, Encoding::windows_1250)), std::string{"p:\xC3\xA9 is UTF-8 throughout"});
}

TRIAL("HTML text: what is not shown: scripts, styles, templates, frames, comments of every form, the head, hidden") {
    const Plain one = read("<head><meta name=x><link rel=a>stray<title>T</title></head><body>"
                           "a<script>var s = '</p><p>no';</script>b<style>x{}</style>c<template><p>no</template>d"
                           "<iframe>no</iframe>e<!-- no --> f<!-->g<!--->h<!-- no --!>i<!-- unclosed");

    SAME(shown(one), std::string{"p:abcde fghi"});

    const Plain hidden = read("<p>shown<span hidden>not <b>shown</b></span> again<div hidden><p>no<ul><li>no</ul></div><p>last");

    SAME(shown(hidden), std::string{"p:shown again / p:last"});
    SAME(shown(read("<noscript><p>Scripts are off</noscript><xmp><b>no</b></xmp>")), std::string{"p:Scripts are off"});
}

TRIAL("HTML text: preformatted text keeps its spaces and lines, plaintext runs to the end") {
    SAME(shown(read("<p>before<pre>\n  a  b\n\tc\r\nd\n\n</pre>after")), std::string{"p:before / pre:  a  b\n\tc\nd / p:after"});
    SAME(shown(read("<p>x<plaintext><b>&amp;</b>\nend")), std::string{"p:x / pre:<b>&amp;</b>\nend"});
}

TRIAL("HTML text: links resolved against the address and the base, their text marked, across breaks and blocks") {
    const Plain one = read("<p>See <a href='b.html'>the next</a> and <a href=\"../up/c.html?q=1#f\">another <b>page</b></a>, "
                           "<a href='/root'>root</a>, <a href='//cdn.example/x'>cdn</a>, <a href='#top'>top</a>, "
                           "<a href='mailto:a@b'>mail</a>, <a>no address</a>.",
                           "https://example.org/dir/sub/page.html#old");

    SAME(one.blocks.size(), std::size_t{1});
    SAME(linked(one.blocks[0]), std::string{"the next>https://example.org/dir/sub/b.html another page>https://example.org/dir/up/c.html?q=1#f "
                                            "root>https://example.org/root cdn>https://cdn.example/x top>https://example.org/dir/sub/page.html#top "
                                            "mail>mailto:a@b"});

    const Plain based = read("<base href='http://other.test/a/'><base href='http://ignored/'><p><a href=' ./x/../y '>y</a>", "file:///home/me/page.html");

    SAME(linked(based.blocks.at(0)), std::string{"y>http://other.test/a/y"});

    const Plain local = read("<p><a href='pic.png'>p</a> <a href='C:/x'>c</a>", "file:///home/me/page.html");

    SAME(linked(local.blocks.at(0)), std::string{"p>file:///home/me/pic.png c>C:/x"});

    const Plain across = read("<p>a <a href=x>one<br>two</a> b<a href=y>three<p>four</a>", "http://h/");

    SAME(shown(across), std::string{"p:a one\ntwo bthree / p:four"});
    SAME(linked(across.blocks[0]), std::string{"one\ntwo>http://h/x three>http://h/y"});
    SAME(linked(across.blocks[1]), std::string{"four>http://h/y"});
}

TRIAL("HTML text: tables as rows of cells, with captions, links in cells and nested tables flattened") {
    const Plain one = read("<p>before<table><caption>Ludno\xC5\x9B\xC4\x87</caption><tr><th>Miasto<th>Ludno\xC5\x9B\xC4\x87"
                           "<tr><td><a href=g>Gda\xC5\x84sk</a><td> 486 <tr><td>Olsztyn<td><table><tr><td>171<td>tys.</table>"
                           "<tr><td><td></table>after",
                           "http://h/");

    SAME(shown(one), std::string{"p:before / p:Ludno\xC5\x9B\xC4\x87 / row:Miasto|Ludno\xC5\x9B\xC4\x87 / row:Gda\xC5\x84sk|486 / row:Olsztyn|171 tys. / p:after"});
    SAME(one.blocks[3].anchors.size(), std::size_t{1});
    SAME(one.blocks[3].anchors[0].cell, std::size_t{0});
    SAME(linked(one.blocks[3]), std::string{"Gda\xC5\x84sk>http://h/g"});
}

TRIAL("HTML text: fields, buttons, choices and pictures in brackets") {
    const Plain one = read("<form><input name=a value='Jan'> <input placeholder='Szukaj'> <input type=hidden value=no>"
                           "<input type=checkbox checked><input type=radio> <input type=submit value=Wy\xC5\x9Blij>"
                           "<select><option>pierwsza<option selected>druga</select> <select><option>jedyna</select>"
                           "<textarea>  wiele\n s\xC5\x82\xC3\xB3w </textarea><button><b>OK</b></button><button> </button>"
                           "<img src=a.png alt='Logo'><img src=b.png alt=''><img src=c.png></form>");

    SAME(shown(one), std::string{"p:[Jan] [Szukaj] [x][ ] [Wy\xC5\x9Blij][druga] [jedyna][wiele s\xC5\x82\xC3\xB3w][OK][Logo]"});
    SAME(shown(read("<select><option>a choice long enough to live on the heap<option selected>the chosen one, long as well</select>")),
         std::string{"p:[the chosen one, long as well]"});
    SAME(shown(read("<p>a<button>b<p>c</button>d")), std::string{"p:a[b / p:c] / p:d"});
}

TRIAL("HTML text: SVG and MathML: CDATA is text there, self-closing elements close") {
    SAME(shown(read("<p>a<svg><rect/><text>tekst <![CDATA[<cdata>]]></text></svg>b <math><mi>x</mi><mn>2</mn></math>")),
         std::string{"p:atekst <cdata>b x2"});
    SAME(shown(read("<p>a<![CDATA[no]]>b")), std::string{"p:ab"});
}

TRIAL("HTML text: control characters that a terminal could obey become U+FFFD, NUL goes, broken bytes are U+FFFD") {
    SAME(shown(read(std::string_view{"<p>a\x1B[31mb\x07 c\x00" "d &#x1B; &#x9B; \xC2\x9B", 32})),
         std::string{"p:a\xEF\xBF\xBD[31mb\xEF\xBF\xBD c" "d \xEF\xBF\xBD \xE2\x80\xBA \xEF\xBF\xBD"});
    SAME(shown(read("<p>\xC5 x", {}, Encoding::windows_1252)), std::string{"p:\xC3\x85 x"});
    SAME(shown(read("<meta charset=utf-8><p>\xC5 x")), std::string{"p:\xEF\xBF\xBD x"});
}

TRIAL("HTML text: hostile pages: tags cut off, stray end tags, depth over the bound, a page over its bounds cut") {
    SAME(shown(read("<p>a</div></li></table></b>b<p")), std::string{"p:ab"});
    SAME(shown(read("<p>a<b c='unclosed>no")), std::string{"p:a"});
    SAME(shown(read("</p><p>a")), std::string{"p:a"});

    std::string deep;

    for (int one = 0; one < 5000; ++one) deep += "<div><span>";

    deep += "x";

    Bounds narrow;

    narrow.depth = 64;
    SAME(shown(read(deep, {}, Encoding::windows_1252, narrow)), std::string{"p:x"});

    std::string many;

    for (int one = 0; one < 50; ++one) many += "<p>akapit " + std::to_string(one);

    Bounds few;

    few.blocks = 10;

    const Plain cut = read(many, {}, Encoding::windows_1252, few);

    SAME(cut.blocks.size(), std::size_t{10});
    OUGHT(cut.cut);

    Bounds short_text;

    short_text.text = 30;
    SAME(read(many, {}, Encoding::windows_1252, short_text).blocks.size(), std::size_t{3});

    Bounds small;

    small.bytes = 100;
    SAME(refusal([&] { (void)read(many, {}, Encoding::windows_1252, small); }), std::string{"full: The page is too large for its text version."});
}

TRIAL("HTML text written: headings, items with their marks and indents, quotes, rows, rules; links marked where they land") {
    const Plain one = read("<h2>Tytu\xC5\x82</h2><p>Zobacz <a href=a>to</a>.<ul><li>raz<li>dwa<br><a href=b>trzy</a>"
                           "<ol start=9><li>g\xC5\x82\xC4\x99" "boko<li><p>a<p><a href=c>b</a></ol></ul><blockquote><p>cyt<br>at</blockquote><hr>"
                           "<table><tr><td>x<td><a href=d>y</a><tr><td>z<td>w</table>",
                           "http://h/");
    const tal::Written made = tal::written(one);

    SAME(made.text, std::string{"## Tytu\xC5\x82\n\nZobacz to.\n\n\xE2\x80\xA2 raz\n\xE2\x80\xA2 dwa\n  trzy\n  9. g\xC5\x82\xC4\x99" "boko\n  10. a\n      b\n\n"
                                "> cyt\n> at\n\n---\n\nx | y\nz | w"});

    const std::vector<tal::Stretch> wanted{
        tal::Stretch{tal::Place{2, 7}, tal::Place{2, 9}, "link", "http://h/a"},
        tal::Stretch{tal::Place{6, 2}, tal::Place{6, 6}, "link", "http://h/b"},
        tal::Stretch{tal::Place{9, 6}, tal::Place{9, 7}, "link", "http://h/c"},
        tal::Stretch{tal::Place{16, 4}, tal::Place{16, 5}, "link", "http://h/d"}};

    SAME(made.marks, wanted);
    SAME(tal::written(Plain{}).text, std::string{});
}

TRIAL("HTML text: what closes on its own: a paragraph at the next, an option at the next, empty cells at the end of a row") {
    SAME(shown(read("<p hidden>secret<p>shown")), std::string{"p:shown"});
    SAME(shown(read("<select><option>first<option>second</select>")), std::string{"p:[first]"});
    SAME(shown(read("<table><tr><td>a<td><td></table>")), std::string{"row:a"});
    SAME(shown(read("<table><tr><td>a<br>b<td>c</table>")), std::string{"row:a b|c"});
    SAME(shown(read("<input type=image alt=Send> <input type=image src=x.png value=Go alt=Send>")), std::string{"p:[Send] [Go]"});
}

TRIAL("HTML text: a declared Latin-1 is windows-1252 whatever the fallback; elements past the depth are not read as elements") {
    SAME(shown(read("<meta charset=latin1><p>\xB3", {}, Encoding::windows_1250)), std::string{"p:\xC2\xB3"});
    SAME(shown(read("<meta charset=iso-8859-2><p>\xB3", {}, Encoding::windows_1252)), std::string{"p:\xC5\x82"});

    std::string deep;

    for (int one = 0; one < 20; ++one) deep += "<div>";

    Bounds narrow;

    narrow.depth = 8;
    SAME(shown(read(deep + "<span hidden>past the bound</span>", {}, Encoding::windows_1252, narrow)), std::string{"p:past the bound"});
    SAME(shown(read("<div><div><span hidden>within</span>shown", {}, Encoding::windows_1252, narrow)), std::string{"p:shown"});
}

TRIAL("HTML text written: a link across lines is marked from its first line to its last") {
    const tal::Written made = tal::written(read("<blockquote><p>x <a href=l>a<br>b</a></blockquote>", "http://h/"));

    SAME(made.text, std::string{"> x a\n> b"});
    SAME(made.marks.size(), std::size_t{1});
    OUGHT(made.marks[0].from == (tal::Place{0, 4}));
    OUGHT(made.marks[0].upto == (tal::Place{1, 3}));
}

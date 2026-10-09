#include "talcite/xml.hpp"

#include <algorithm>
#include <utility>

namespace talcite::xml {

// Drzewo jest danymi — struktury bez metod — a to, czego potrzebuje każdy, kto po nim chodzi,
// stoi obok jako wolne funkcje. Wskaźnik: `nullptr` znaczy „nie ma" i to jest cały język błędów
// tego pliku; drzewo nie rzuca, bo nie ma czym zawieść.

Node* root(Document& one) {
    for (Node& node : one.nodes)
        if (node.kind == Kind::element) return &node;

    return nullptr;
}

const Node* root(const Document& one) { return root(const_cast<Document&>(one)); }

Node* child(Node& one, std::string_view name) {
    for (Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) return &node;

    return nullptr;
}

const Node* child(const Node& one, std::string_view name) { return child(const_cast<Node&>(one), name); }

std::vector<Node*> children(Node& one, std::string_view name) {
    std::vector<Node*> made;

    for (Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) made.push_back(&node);

    return made;
}

// Tekst elementu to jego dzieci tekstowe i CDATA sklejone po kolei — tak jak stoją w pliku,
// bo `<plot>a<![CDATA[b]]>c</plot>` znaczy `abc`.
std::string text(const Node& one) {
    std::string made;

    for (const Node& node : one.children)
        if (node.kind == Kind::text || node.kind == Kind::cdata) made += node.value;

    return made;
}

// Ustawienie tekstu zdejmuje wszystkie dzieci tekstowe i CDATA, a dzieci-elementy zostawia na
// miejscu; nowy tekst staje na początku, jak w `<actor>Nazwisko<role>…</role></actor>`. Pusty
// tekst niczego nie dokłada — element bez tekstu jest elementem pustym, nie z pustym dzieckiem.
void text(Node& one, std::string given) {
    std::erase_if(one.children, [](const Node& node) {
        return node.kind == Kind::text || node.kind == Kind::cdata;
    });

    if (given.empty()) return;

    Node made;

    made.kind = Kind::text;
    made.value = std::move(given);

    one.children.insert(one.children.begin(), std::move(made));
}

const std::string* attribute(const Node& one, std::string_view name) {
    for (const Attribute& each : one.attributes)
        if (each.name == name) return &each.value;

    return nullptr;
}

void attribute(Node& one, std::string_view name, std::string value) {
    for (Attribute& each : one.attributes)
        if (each.name == name) {
            each.value = std::move(value);

            return;
        }

    one.attributes.push_back(Attribute{std::string{name}, std::move(value)});
}

}

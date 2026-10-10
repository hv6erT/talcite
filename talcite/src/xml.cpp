#include "talcite/xml.hpp"

#include <algorithm>
#include <utility>

namespace talcite::xml {

// The tree is data (structures without methods), and what everyone walking it needs stands
// beside it as free functions. A pointer: `nullptr` means "not there", and that is the whole
// error language of this file; the tree does not throw, because it has nothing to fail at.

// The first element of the document, or `nullptr` when it has none.
Node* root(Document& one) {
    for (Node& node : one.nodes)
        if (node.kind == Kind::element) return &node;

    return nullptr;
}

// The first element of a read-only document.
const Node* root(const Document& one) { return root(const_cast<Document&>(one)); }

// The first child element called `name`, or `nullptr`.
Node* child(Node& one, std::string_view name) {
    for (Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) return &node;

    return nullptr;
}

// The first child element called `name` of a read-only node.
const Node* child(const Node& one, std::string_view name) { return child(const_cast<Node&>(one), name); }

// All child elements called `name`, in document order.
std::vector<Node*> children(Node& one, std::string_view name) {
    std::vector<Node*> made;

    for (Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) made.push_back(&node);

    return made;
}

// The text of an element is its text and CDATA children joined in order, as they stand in the
// file, because <plot>a<![CDATA[b]]>c</plot> means abc.
std::string text(const Node& one) {
    std::string made;

    for (const Node& node : one.children)
        if (node.kind == Kind::text || node.kind == Kind::cdata) made += node.value;

    return made;
}

// Setting the text removes all text and CDATA children and leaves child elements in place; the
// new text goes first, as in <actor>Name<role>…</role></actor>. An empty text adds nothing: an
// element without text is an empty element, not one with an empty child.
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

// The value of the attribute `name`, or `nullptr` when the element does not have it.
const std::string* attribute(const Node& one, std::string_view name) {
    for (const Attribute& each : one.attributes)
        if (each.name == name) return &each.value;

    return nullptr;
}

// Sets the attribute `name`, replacing its value or adding it at the end.
void attribute(Node& one, std::string_view name, std::string value) {
    for (Attribute& each : one.attributes)
        if (each.name == name) {
            each.value = std::move(value);

            return;
        }

    one.attributes.push_back(Attribute{std::string{name}, std::move(value)});
}

}

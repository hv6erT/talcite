#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace talcite::xml {

enum class Kind : std::uint8_t { element, text, cdata, comment, declaration, instruction, doctype };

struct Attribute {
    std::string name;
    std::string value;

    friend bool operator==(const Attribute&, const Attribute&) = default;
};

struct Node {
    Kind kind = Kind::element;
    std::string name;
    std::string value;
    std::vector<Attribute> attributes;
    std::vector<Node> children;

    friend bool operator==(const Node&, const Node&) = default;
};

struct Document {
    std::vector<Node> nodes;

    [[nodiscard]] static constexpr std::string_view kind() { return "XML"; }

    friend bool operator==(const Document&, const Document&) = default;
};

[[nodiscard]] Node* root(Document& one);

[[nodiscard]] const Node* root(const Document& one);

[[nodiscard]] Node* child(Node& one, std::string_view name);

[[nodiscard]] const Node* child(const Node& one, std::string_view name);

[[nodiscard]] std::vector<Node*> children(Node& one, std::string_view name);

[[nodiscard]] std::string text(const Node& one);

void text(Node& one, std::string given);

[[nodiscard]] const std::string* attribute(const Node& one, std::string_view name);

void attribute(Node& one, std::string_view name, std::string value);

}

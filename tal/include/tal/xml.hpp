#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tal::xml {

enum class Kind { element, text, cdata, comment, declaration, instruction, doctype };

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

    friend bool operator==(const Document&, const Document&) = default;
};

struct Limits {
    std::size_t bytes = std::size_t{64} << 20;
    int depth = 128;
    std::size_t nodes = std::size_t{1} << 20;
};

[[nodiscard]] Document parsed(std::string_view text, Limits limits = {});

[[nodiscard]] std::string written(const Document& one);

[[nodiscard]] Node* root(Document& one);

[[nodiscard]] const Node* root(const Document& one);

[[nodiscard]] Node* child(Node& one, std::string_view name);

[[nodiscard]] const Node* child(const Node& one, std::string_view name);

[[nodiscard]] std::vector<Node*> children(Node& one, std::string_view name);

[[nodiscard]] std::vector<const Node*> children(const Node& one, std::string_view name);

[[nodiscard]] std::string text(const Node& one);

void text(Node& one, std::string given);

[[nodiscard]] const std::string* attribute(const Node& one, std::string_view name);

void attribute(Node& one, std::string_view name, std::string value);

}

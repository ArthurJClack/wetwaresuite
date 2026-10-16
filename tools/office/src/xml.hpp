// A small XML reader and writer: enough for OOXML (docx/xlsx/pptx) and ODF
// (odt/ods) parts. Namespaces are kept as written ("w:p"); lookups by local
// name ignore the prefix, which is how every office format is read here.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wetwaresuite::office::xml {

struct Node {
    std::string name;  // qualified element name; empty for a text node
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> children;
    std::string text;  // text nodes only (entities decoded)

    bool isText() const { return name.empty(); }
    std::string_view local() const;  // name without its prefix
    bool is(std::string_view localName) const { return !isText() && local() == localName; }

    // Attribute by qualified name or, failing that, by local name. Empty if missing.
    std::string attr(std::string_view key) const;
    bool hasAttr(std::string_view key) const;

    const Node* child(std::string_view localName) const;  // first child element with that local name
    std::vector<const Node*> all(std::string_view localName) const;  // direct children with that name
    void descendants(std::string_view localName, std::vector<const Node*>& out) const;
    const Node* find(std::string_view localName) const;  // first descendant (depth first)

    // All text below this node, concatenated.
    std::string allText() const;
};

// Parses a whole document and returns its root element. Throws std::runtime_error on broken XML.
Node parse(std::string_view text);

// Escapes text for element content or attribute values.
std::string escape(std::string_view text);

// Builds XML text: w.open("w:p").attr("w:val", "x"); w.text("hi"); w.close();
class Writer {
public:
    Writer();  // starts with the XML declaration
    Writer& open(std::string_view name);
    Writer& attr(std::string_view key, std::string_view value);
    Writer& text(std::string_view content);
    Writer& raw(std::string_view xml);
    Writer& close();  // closes the innermost open element (self-closing when empty)
    Writer& leaf(std::string_view name, std::vector<std::pair<std::string, std::string>> attrs = {});
    Writer& element(std::string_view name, std::string_view content);  // <name>content</name>
    std::string str();  // closes anything still open

private:
    std::string out_;
    std::vector<std::string> stack_;
    bool tagOpen_ = false;
    void finishTag();
};

}  // namespace wetwaresuite::office::xml

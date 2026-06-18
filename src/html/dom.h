#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>

enum class NodeType { Document, Element, Text };

struct Node {
    NodeType    type    = NodeType::Element;
    std::string tagName;                              // lowercase; "#document" for root
    std::string text;                                 // only for Text nodes
    std::map<std::string, std::string> attrs;
    std::vector<std::shared_ptr<Node>> children;
    Node*       parent  = nullptr;

    std::string attr(const std::string& key) const {
        auto it = attrs.find(key);
        return it != attrs.end() ? it->second : "";
    }

    void appendChild(std::shared_ptr<Node> child) {
        child->parent = this;
        children.push_back(std::move(child));
    }

    static std::shared_ptr<Node> makeDocument() {
        auto n = std::make_shared<Node>();
        n->type    = NodeType::Document;
        n->tagName = "#document";
        return n;
    }
    static std::shared_ptr<Node> makeElement(const std::string& tag) {
        auto n = std::make_shared<Node>();
        n->type    = NodeType::Element;
        n->tagName = tag;
        return n;
    }
    static std::shared_ptr<Node> makeText(const std::string& t) {
        auto n = std::make_shared<Node>();
        n->type = NodeType::Text;
        n->text = t;
        return n;
    }
};

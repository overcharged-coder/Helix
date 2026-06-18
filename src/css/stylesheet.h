#pragma once
#include "css/style.h"
#include "html/dom.h"
#include <vector>
#include <string>

struct CssSelectorPart {
    std::string tag;       // "" = any
    std::string cls;       // "" = any
    std::string id;        // "" = any
    std::string attrName;  // "" = no attribute selector
    std::string attrValue; // only used when attrHasValue = true
    bool attrHasValue = false;
    char combinator = 0;   // 0 = first part, ' ' = descendant, '>' = child
};

struct CssRule {
    std::string tag;       // "" = any
    std::string cls;       // "" = any  (matches class attribute)
    std::string id;        // "" = any
    std::vector<CssSelectorPart> selector;
    ComputedStyle style;

    int specificity() const;

    bool matches(const Node* node) const;
};

struct Stylesheet {
    std::vector<CssRule> rules;

    // Compute style for a node from the sheet + its inline style=""
    ComputedStyle resolve(const Node* node) const;
};

Stylesheet ParseStylesheet(const std::string& css);
ComputedStyle ParseInlineStyle(const std::string& style);
std::string SerializeStylesheet(const Stylesheet& sheet);
std::string SerializeComputedStyle(const ComputedStyle& style);

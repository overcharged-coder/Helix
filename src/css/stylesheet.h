#pragma once
#include "css/style.h"
#include "html/dom.h"
#include <vector>
#include <string>

struct CssRule {
    std::string tag;       // "" = any
    std::string cls;       // "" = any  (matches class attribute)
    std::string id;        // "" = any
    ComputedStyle style;

    int specificity() const {
        return (!id.empty() ? 100 : 0)
             + (!cls.empty() ?  10 : 0)
             + (!tag.empty() ?   1 : 0);
    }

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

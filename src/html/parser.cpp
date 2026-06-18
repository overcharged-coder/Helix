#include "html/parser.h"
#include "html/tokenizer.h"
#include <set>
#include <algorithm>
#include <cctype>

static const std::set<std::string> kVoidTags = {
    "area","base","br","col","embed","hr","img","input",
    "link","meta","param","source","track","wbr"
};

// Tags that implicitly close when a sibling of the same kind opens
static const std::set<std::string> kAutoClose = {
    "p","li","dt","dd","tr","td","th","thead","tbody","tfoot",
    "colgroup","option","optgroup"
};

std::shared_ptr<Node> ParseHtml(const std::string& html) {
    auto doc = Node::makeDocument();

    // Open-element stack: doc is always at index 0
    std::vector<std::shared_ptr<Node>> stack;
    stack.push_back(doc);

    auto current = [&]() -> Node* { return stack.back().get(); };

    HtmlTokenizer tok;
    tok.tokenize(html, [&](const HtmlToken& t) {
        switch (t.type) {

        case TokenType::StartTag: {
            // Auto-close peer elements (e.g. <p> closes previous <p>)
            if (kAutoClose.count(t.name) && stack.size() > 1) {
                if (stack.back()->tagName == t.name) {
                    stack.pop_back();
                }
            }

            auto node = Node::makeElement(t.name);
            node->attrs = t.attrs;
            current()->appendChild(node);

            if (!kVoidTags.count(t.name) && !t.selfClosing)
                stack.push_back(node);
            break;
        }

        case TokenType::EndTag: {
            // Walk stack backwards to find matching open tag
            for (int i = (int)stack.size() - 1; i >= 1; --i) {
                if (stack[i]->tagName == t.name) {
                    stack.erase(stack.begin() + i, stack.end());
                    break;
                }
            }
            break;
        }

        case TokenType::Text: {
            // Collapse runs of whitespace; drop all-whitespace nodes
            // inside block elements (they're invisible anyway)
            std::string txt = t.data;
            // Normalise whitespace: newlines/tabs → spaces, runs → single space
            for (auto& c : txt)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            // Collapse consecutive spaces
            std::string collapsed;
            bool lastSpace = false;
            for (char c : txt) {
                if (c == ' ') {
                    if (!lastSpace) collapsed += ' ';
                    lastSpace = true;
                } else {
                    collapsed += c;
                    lastSpace = false;
                }
            }
            if (!collapsed.empty() && collapsed != " ")
                current()->appendChild(Node::makeText(collapsed));
            break;
        }

        default: break;
        }
    });

    return doc;
}

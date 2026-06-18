#include "test/fixture.h"

#include "css/stylesheet.h"
#include "html/parser.h"

static const Node* FindFirstElement(const Node* node, const std::string& tag) {
    if (!node) return nullptr;
    if (node->type == NodeType::Element && node->tagName == tag) return node;
    for (const auto& child : node->children) {
        if (auto* found = FindFirstElement(child.get(), tag)) return found;
    }
    return nullptr;
}

static const Node* FindElementById(const Node* node, const std::string& id) {
    if (!node) return nullptr;
    if (node->type == NodeType::Element && node->attr("id") == id) return node;
    for (const auto& child : node->children) {
        if (auto* found = FindElementById(child.get(), id)) return found;
    }
    return nullptr;
}

TestResult RunCssTests() {
    TestResult result;
    auto root = FindRepoRoot();

    {
        auto input = ReadTextFile(root / "tests/fixtures/css/rules/basic.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/rules/basic.expected.txt");
        auto actual = SerializeStylesheet(ParseStylesheet(input));
        ExpectEqual("css/rules/basic", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/basic.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/basic.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/basic.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        auto* p = FindFirstElement(dom.get(), "p");
        std::string actual = p ? SerializeComputedStyle(sheet.resolve(p)) : "missing p\n";
        ExpectEqual("css/cascade/basic", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/ancestor-combinators.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/ancestor-combinators.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/ancestor-combinators.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : { "article-desc", "section-desc", "section-child", "loose" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/ancestor-combinators", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/attribute-selectors.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/attribute-selectors.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/attribute-selectors.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : { "ready", "busy", "div-ready", "missing" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/attribute-selectors", actual, expected, result);
    }

    return result;
}

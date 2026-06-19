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
        for (const std::string id : {
            "ready", "busy", "div-ready", "missing",
            "token-hit", "token-miss",
            "dash-hit", "dash-exact", "dash-miss",
            "prefix-hit", "prefix-miss",
            "suffix-hit", "suffix-miss",
            "substring-hit", "substring-miss",
            "escaped-class"
        }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/attribute-selectors", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/adjacent-sibling.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/adjacent-sibling.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/adjacent-sibling.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : { "lead", "later", "nested" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/adjacent-sibling", actual, expected, result);
    }

    {
        auto html = ReadTextFile(root / "tests/fixtures/css/cascade/pseudo-classes.in.html");
        auto css = ReadTextFile(root / "tests/fixtures/css/cascade/pseudo-classes.in.css");
        auto expected = ReadTextFile(root / "tests/fixtures/css/cascade/pseudo-classes.expected.txt");
        auto dom = ParseHtml(html);
        auto sheet = ParseStylesheet(css);
        std::string actual;
        for (const std::string id : {
            "first", "middle", "last", "only",
            "empty", "not-empty",
            "link", "not-link", "hover-target"
        }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/pseudo-classes", actual, expected, result);
    }

    {
        auto dom = ParseHtml("<html><body class=\"category\"><div id=\"content-main\"></div><a id=\"skip\"></a></body></html>");
        auto sheet = ParseStylesheet(
            "body.category #content-main { float: right; width: 60%; overflow: hidden; }"
            "a#skip { display: block; position: absolute; top: 0; left: 0; width: 100%; }");
        std::string actual;
        for (const std::string id : { "content-main", "skip" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/wasp-layout-primitives",
            actual,
            "content-main: widthPercent=60 float=right overflow=hidden \n"
            "skip: display=block widthPercent=100 position=absolute top=0 left=0 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"face\"></div></body></html>");
        auto sheet = ParseStylesheet(
            "#face { background: red url(data:image/png;base64,QUJDRA%3D%3D); width: 4em; height: 2em; }");
        auto* node = FindElementById(dom.get(), "face");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/data-uri-background",
            actual,
            "bg=1,0,0,1 width=64 height=32 backgroundImage=data:image/png;base64,QUJDRA%3D%3D \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><h2 id=\"top\">Hello World!</h2></body></html>");
        auto sheet = ParseStylesheet("#top { font: 2em/24px sans-serif; }");
        auto* node = FindElementById(dom.get(), "top");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/font-shorthand-absolute-line-height",
            actual,
            "fontSize=32 lineHeight=24 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div class=\"picture\"></div></body></html>");
        auto sheet = ParseStylesheet(".picture { background: red; } .picture { background: none; }");
        auto* node = FindFirstElement(dom.get(), "div");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/background-none-clears-background",
            actual,
            "\n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"band\"></div></body></html>");
        auto sheet = ParseStylesheet("#band { height: 8px; min-height: 1em; max-height: 2mm; }");
        auto* node = FindElementById(dom.get(), "band");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/min-max-height",
            actual,
            "height=8 minHeight=16 maxHeight=7.55906 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"nose\"></div></body></html>");
        auto* owner = FindElementById(dom.get(), "nose");
        auto pseudo = Node::makeElement("div");
        pseudo->attrs = owner->attrs;
        pseudo->attrs["_helix_pseudo"] = "before";
        pseudo->parent = const_cast<Node*>(owner);
        auto sheet = ParseStylesheet(
            "#nose:before { display: block; border-style: none solid solid; "
            "border-color: red yellow black yellow; border-width: 1em; content: ''; height: 0; }");
        std::string actual = SerializeComputedStyle(sheet.resolve(pseudo.get()));
        ExpectEqual("css/cascade/generated-border-box",
            actual,
            "display=block borderWidth=16 borderTopWidth=0 borderTopColor=1,0,0,1 borderRightColor=1,1,0,1 borderBottomColor=0,0,0,1 borderLeftColor=1,1,0,1 height=0 content= \n",
            result);
    }

    return result;
}

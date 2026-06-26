#include "test/fixture.h"

#include "css/stylesheet.h"
#include "html/parser.h"

#include <cstdio>

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
        auto dom = ParseHtml(
            "<html><body>"
            "<p id=\"is-hit\" class=\"target primary\"></p>"
            "<p id=\"where-hit\" class=\"target secondary\"></p>"
            "<p id=\"miss\" class=\"target\"></p>"
            "<section id=\"ancestor\"><span id=\"desc\" class=\"child\"></span></section>"
            "</body></html>");
        auto sheet = ParseStylesheet(
            ".target:is(.primary, .secondary) { color: red; }"
            ".target:where(.secondary) { margin-left: 4px; color: green; }"
            ".target { color: blue; }"
            "#ancestor :is(.child, a) { padding-left: 3px; }"
            ".target:is(.absent) { padding-right: 9px; }");
        std::string actual;
        for (const std::string id : { "is-hit", "where-hit", "miss", "desc" }) {
            auto* node = FindElementById(dom.get(), id);
            actual += id + ": ";
            actual += node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        }
        ExpectEqual("css/cascade/is-where-selectors",
            actual,
            "is-hit: color=1,0,0,1 \n"
            "where-hit: color=1,0,0,1 marginLeft=4 \n"
            "miss: color=0,0,1,1 \n"
            "desc: paddingLeft=3 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            "#target { color: red; }"
            "@supports (display: grid) { #target { color: blue; margin-left: 5px; } }"
            "@supports not (display: grid) { #target { padding-left: 9px; } }"
            "@supports (display: made-up) { #target { padding-right: 11px; } }");
        std::string actual = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        ExpectEqual("css/supports/display-feature-queries",
            actual,
            "color=0,0,1,1 marginLeft=5 \n",
            result);
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
        // CSS sprite: background-position (negative offset), -size, -repeat parse.
        auto dom = ParseHtml("<html><body><i id=\"icon\"></i></body></html>");
        auto sheet = ParseStylesheet(
            "#icon { background-image: url(sprite.png); background-position: -40px -80px;"
            " background-size: 200px 100px; background-repeat: no-repeat; }");
        auto* node = FindElementById(dom.get(), "icon");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf,
            "repeat=%d posX=%g posY=%g sizeMode=%d sizeW=%g sizeH=%g\n",
            cs.bgRepeat, cs.bgPosX, cs.bgPosY, cs.bgSizeMode, cs.bgSizeW, cs.bgSizeH);
        ExpectEqual("css/background/sprite-longhand",
            std::string(buf),
            "repeat=3 posX=-40 posY=-80 sizeMode=3 sizeW=200 sizeH=100\n",
            result);
    }

    {
        // Shorthand carries position + repeat: "url() <x> <y> no-repeat".
        auto dom = ParseHtml("<html><body><i id=\"s\"></i></body></html>");
        auto sheet = ParseStylesheet(
            "#s { background: url(sprite.png) -12px -34px no-repeat; }");
        auto* node = FindElementById(dom.get(), "s");
        auto cs = sheet.resolve(node);
        char buf[256];
        snprintf(buf, sizeof buf, "repeat=%d posX=%g posY=%g img=%d\n",
            cs.bgRepeat, cs.bgPosX, cs.bgPosY, cs.backgroundImageSet ? 1 : 0);
        ExpectEqual("css/background/sprite-shorthand",
            std::string(buf),
            "repeat=3 posX=-12 posY=-34 img=1\n",
            result);
    }

    {
        // clamp()/min()/max() resolve against the viewport. At 1000px wide:
        // clamp(200px,30vw,400px) = clamp(200,300,400) = 300.
        SetCssViewport(1000.f, 800.f);
        auto cs = ParseInlineStyle("width: clamp(200px, 30vw, 400px)");
        char buf[64]; snprintf(buf, sizeof buf, "width=%g\n", cs.width);
        ExpectEqual("css/values/clamp-resolves-against-viewport",
            std::string(buf), "width=300\n", result);
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

    {
        auto dom = ParseHtml("<html><body><div id=\"cover\"></div></body></html>");
        auto* node = FindElementById(dom.get(), "cover");
        auto sheet = ParseStylesheet("#cover { position: relative; z-index: 2; }");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/z-index",
            actual,
            "position=relative zIndex=2 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div class=\"picture\"><p id=\"scalp\"></p></div></body></html>");
        auto* node = FindElementById(dom.get(), "scalp");
        auto sheet = ParseStylesheet(
            "html { font: 12px sans-serif; } "
            ".picture p { position: fixed; top: 9em; left: 11em; }");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/acid2-fixed-em-offsets",
            actual,
            "position=fixed top=108 left=132 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><h2 id=\"top\">Hello World!</h2></body></html>");
        auto* node = FindElementById(dom.get(), "top");
        auto sheet = ParseStylesheet(
            "html { font: 12px sans-serif; } "
            "#top { margin: 100em 3em 0; font: 2em/24px sans-serif; }");
        std::string actual = node ? SerializeComputedStyle(sheet.resolve(node)) : "missing\n";
        ExpectEqual("css/cascade/em-lengths-use-element-font-size",
            actual,
            "fontSize=24 marginTop=2400 marginRight=72 marginBottom=0 marginLeft=72 lineHeight=24 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* rootNode = FindFirstElement(dom.get(), "html");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            ":root { --brand: #13579b; --gap: 12px; }"
            "#target { color: var(--brand); padding-left: var(--gap); }");
        auto rootStyle = rootNode ? sheet.resolve(rootNode) : ComputedStyle{};
        ResolveStyleVariables(rootStyle);
        auto targetStyle = target ? rootStyle.inherit(sheet.resolve(target)) : ComputedStyle{};
        ResolveStyleVariables(targetStyle);
        ExpectEqual("css/custom-properties/inherited-and-resolved",
            SerializeComputedStyle(targetStyle),
            "color=0.0745098,0.341176,0.607843,1 paddingLeft=12 \n",
            result);
    }

    {
        auto dom = ParseHtml("<html><body><div id=\"target\"></div></body></html>");
        auto* target = FindElementById(dom.get(), "target");
        auto sheet = ParseStylesheet(
            "#target { color: red; }"
            "@media screen and (min-width: 600px) { #target { color: blue; } }");
        sheet.setViewport(599.f, 800.f);
        const std::string narrow = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        sheet.setViewport(600.f, 800.f);
        const std::string wide = target ? SerializeComputedStyle(sheet.resolve(target)) : "missing\n";
        ExpectEqual("css/media/min-width",
            "narrow: " + narrow + "wide: " + wide,
            "narrow: color=1,0,0,1 \nwide: color=0,0,1,1 \n",
            result);
    }

    return result;
}

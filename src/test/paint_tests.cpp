#include "test/fixture.h"

#include "html/parser.h"
#include "layout/layout.h"
#include "paint/display_list.h"

TestResult RunPaintTests() {
    TestResult result;
    auto root = FindRepoRoot();

    auto input = ReadTextFile(root / "tests/fixtures/paint/basic.in.html");
    auto expected = ReadTextFile(root / "tests/fixtures/paint/basic.expected.txt");
    auto dom = ParseHtml(input);
    auto layout = BuildSimpleLayout(dom, 800.0f);
    auto items = BuildDisplayList(layout);
    auto actual = SerializeDisplayList(items);
    ExpectEqual("paint/basic", actual, expected, result);

    {
        auto root = FindRepoRoot();
        std::string painter = ReadTextFile(root / "src/render/box_paint.cpp");
        const bool usesStableFontKey = painter.find("const std::string fontKey = FontCacheKey(f);") != std::string::npos;
        ExpectEqual("paint/text-measurement-cache-key-uses-stable-storage",
            usesStableFontKey ? "stable\n" : "temporary\n",
            "stable\n",
            result);
    }

    return result;
}

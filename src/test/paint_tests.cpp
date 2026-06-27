#include "test/fixture.h"

#include "html/parser.h"
#include "layout/layout.h"
#include "paint/display_list.h"
#include "render/svg.h"

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

    {
        const std::string svg =
            "<svg width=\"12\" height=\"10\" viewBox=\"0 0 12 10\">"
            "<rect x=\"1\" y=\"2\" width=\"4\" height=\"3\" fill=\"red\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        bool hasRed = false;
        for (size_t i = 0; i + 3 < bmp.pixels.size(); i += 4) {
            if (bmp.pixels[i] > 200 && bmp.pixels[i + 1] < 40
             && bmp.pixels[i + 2] < 40 && bmp.pixels[i + 3] > 200) {
                hasRed = true;
                break;
            }
        }
        ExpectEqual("paint/svg-bytes-rasterize-external-image",
            (bmp.width == 12 && bmp.height == 10 && hasRed) ? "svg\n" : "empty\n",
            "svg\n",
            result);
    }

    return result;
}

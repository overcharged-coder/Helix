#include "test/fixture.h"

#include "html/parser.h"
#include "layout/layout.h"
#include "paint/display_list.h"
#include "render/svg.h"

static bool SvgPixelIs(const SvgBitmap& bmp, int x, int y, int rMin, int gMax, int bMax, int aMin) {
    if (x < 0 || x >= bmp.width || y < 0 || y >= bmp.height) return false;
    size_t i = (size_t)(y * bmp.width + x) * 4;
    return bmp.pixels[i] >= rMin
        && bmp.pixels[i + 1] <= gMax
        && bmp.pixels[i + 2] <= bMax
        && bmp.pixels[i + 3] >= aMin;
}

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

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" "
            "x1=\"0\" y1=\"0\" x2=\"10\" y2=\"0\" gradientTransform=\"rotate(90 5 5)\">"
            "<stop offset=\"0%\" stop-color=\"red\"/>"
            "<stop offset=\"100%\" stop-color=\"blue\"/>"
            "</linearGradient></defs>"
            "<rect width=\"10\" height=\"10\" fill=\"url(#g)\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        size_t top = (size_t)(1 * bmp.width + 5) * 4;
        size_t bottom = (size_t)(8 * bmp.width + 5) * 4;
        bool transformed = bmp.width == 10 && bmp.height == 10
            && bmp.pixels[top] > bmp.pixels[top + 2]
            && bmp.pixels[bottom + 2] > bmp.pixels[bottom];
        ExpectEqual("paint/svg-gradient-transform-rotates-sampling",
            transformed ? "rotated\n" : "flat\n",
            "rotated\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<path fill=\"red\" fill-rule=\"evenodd\" "
            "d=\"M1 1H9V9H1Z M3 3H7V7H3Z\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        size_t center = (size_t)(5 * bmp.width + 5) * 4;
        bool centerIsHole = bmp.width == 10 && bmp.height == 10 && bmp.pixels[center + 3] == 0;
        ExpectEqual("paint/svg-path-fill-rule-evenodd-makes-hole",
            centerIsHole ? "hole\n" : "filled\n",
            "hole\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<style>.cut{fill:red;fill-rule:evenodd}</style>"
            "<path class=\"cut\" d=\"M1 1H9V9H1Z M3 3H7V7H3Z\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        size_t center = (size_t)(5 * bmp.width + 5) * 4;
        bool centerIsHole = bmp.width == 10 && bmp.height == 10 && bmp.pixels[center + 3] == 0;
        ExpectEqual("paint/svg-css-fill-rule-evenodd-makes-hole",
            centerIsHole ? "hole\n" : "filled\n",
            "hole\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<defs><symbol id=\"mark\"><rect x=\"1\" y=\"1\" width=\"4\" height=\"4\" fill=\"red\"/></symbol></defs>"
            "<use href=\"#mark\" x=\"2\" y=\"2\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        ExpectEqual("paint/svg-use-renders-symbol-children",
            SvgPixelIs(bmp, 4, 4, 200, 40, 40, 200) ? "symbol\n" : "empty\n",
            "symbol\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
            "<style>.a.b{fill:red}</style>"
            "<rect class=\"a b\" x=\"2\" y=\"2\" width=\"4\" height=\"4\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        ExpectEqual("paint/svg-css-selector-requires-all-classes",
            SvgPixelIs(bmp, 4, 4, 200, 40, 40, 200) ? "matched\n" : "missed\n",
            "matched\n",
            result);
    }

    {
        const std::string svg =
            "<svg width=\"12\" height=\"8\" viewBox=\"0 0 12 8\">"
            "<line x1=\"4\" y1=\"4\" x2=\"8\" y2=\"4\" stroke=\"red\" stroke-width=\"4\" stroke-linecap=\"round\"/>"
            "</svg>";
        auto bmp = svg::renderSvgBytes(svg, 32);
        bool cornerClear = bmp.width == 12 && bmp.height == 8 && bmp.pixels[(2 * bmp.width + 2) * 4 + 3] == 0;
        ExpectEqual("paint/svg-stroke-round-linecap-avoids-square-corners",
            cornerClear ? "round\n" : "square\n",
            "round\n",
            result);
    }

    return result;
}

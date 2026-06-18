#include "test/fixture.h"

#include "html/parser.h"
#include "layout/layout.h"

TestResult RunLayoutTests() {
    TestResult result;
    auto root = FindRepoRoot();

    auto input = ReadTextFile(root / "tests/fixtures/layout/basic.in.html");
    auto expected = ReadTextFile(root / "tests/fixtures/layout/basic.expected.txt");
    auto dom = ParseHtml(input);
    auto layout = BuildSimpleLayout(dom, 800.0f);
    auto actual = SerializeLayout(layout);
    ExpectEqual("layout/basic", actual, expected, result);

    return result;
}

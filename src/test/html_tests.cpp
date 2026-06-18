#include "test/fixture.h"

#include "html/parser.h"
#include "html/serializer.h"
#include "html/tokenizer.h"

#include <vector>

static std::vector<HtmlToken> TokenizeForTest(const std::string& html) {
    std::vector<HtmlToken> tokens;
    HtmlTokenizer tokenizer;
    tokenizer.tokenize(html, [&](const HtmlToken& token) {
        tokens.push_back(token);
    });
    return tokens;
}

TestResult RunHtmlTests() {
    TestResult result;
    auto root = FindRepoRoot();

    {
        auto input = ReadTextFile(root / "tests/fixtures/html/tokenizer/basic.in.html");
        auto expected = ReadTextFile(root / "tests/fixtures/html/tokenizer/basic.expected.txt");
        auto actual = SerializeTokens(TokenizeForTest(input));
        ExpectEqual("html/tokenizer/basic", actual, expected, result);
    }

    {
        auto input = ReadTextFile(root / "tests/fixtures/html/dom/basic.in.html");
        auto expected = ReadTextFile(root / "tests/fixtures/html/dom/basic.expected.txt");
        auto actual = SerializeDom(ParseHtml(input));
        ExpectEqual("html/dom/basic", actual, expected, result);
    }

    return result;
}

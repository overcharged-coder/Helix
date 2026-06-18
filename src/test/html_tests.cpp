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

static void RunTokenizerFixture(const std::string& name, TestResult& result) {
    auto root = FindRepoRoot();
    auto input = ReadTextFile(root / ("tests/fixtures/html/tokenizer/" + name + ".in.html"));
    auto expected = ReadTextFile(root / ("tests/fixtures/html/tokenizer/" + name + ".expected.txt"));
    auto actual = SerializeTokens(TokenizeForTest(input));
    ExpectEqual("html/tokenizer/" + name, actual, expected, result);
}

static void RunDomFixture(const std::string& name, TestResult& result) {
    auto root = FindRepoRoot();
    auto input = ReadTextFile(root / ("tests/fixtures/html/dom/" + name + ".in.html"));
    auto expected = ReadTextFile(root / ("tests/fixtures/html/dom/" + name + ".expected.txt"));
    auto actual = SerializeDom(ParseHtml(input));
    ExpectEqual("html/dom/" + name, actual, expected, result);
}

TestResult RunHtmlTests() {
    TestResult result;

    RunTokenizerFixture("basic", result);
    RunTokenizerFixture("doctype-comment-entity", result);
    RunTokenizerFixture("rawtext", result);
    RunTokenizerFixture("invalid-numeric-entity", result);

    RunDomFixture("basic", result);
    RunDomFixture("rawtext-style", result);
    RunDomFixture("autoclose-paragraphs", result);

    return result;
}

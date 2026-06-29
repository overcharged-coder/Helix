#include "test/fixture.h"

#include "js/compiler.h"
#include "js/engine.h"
#include "js/gc.h"
#include "js/lexer.h"
#include "js/parser.h"
#include "js/runtime.h"
#include "js/vm.h"
#include "js/dom_bridge.h"
#include "html/parser.h"

#include <sstream>
#include <string>

static std::string ValueForSnapshot(const JsValue& value) {
    std::ostringstream out;
    if (value.isNumber()) out << "number: " << value.toString() << "\n";
    else if (value.isString()) out << "string: " << value.toString() << "\n";
    else if (value.isBool()) out << "boolean: " << value.toString() << "\n";
    else if (value.isNull()) out << "null\n";
    else if (value.isUndefined()) out << "undefined\n";
    else if (value.isObject()) out << "object: " << value.toString() << "\n";
    return out.str();
}

static std::string RunJsSourceSnapshot(const std::string& source) {
    GC gc;
    VM vm(gc);
    registerBuiltins(vm);

    Lexer lexer(source);
    Parser parser(lexer.tokenize());
    auto program = parser.parse();
    auto bytecode = Compiler::compile(program);
    vm.execute(bytecode.get());
    vm.drainMicrotasks();
    return ValueForSnapshot(vm.getGlobal("__result"));
}

static void ExpectJsResult(
    const std::string& name,
    const std::string& source,
    const std::string& expected,
    TestResult& result) {
    try {
        ExpectEqual("js/" + name, RunJsSourceSnapshot(source), expected, result);
    } catch (const ParseError& error) {
        ExpectEqual("js/" + name, std::string("parse error: ") + error.what() + "\n", expected, result);
    } catch (const JsException& error) {
        ExpectEqual("js/" + name, std::string("runtime error: ") + error.val.toString() + "\n", expected, result);
    } catch (const std::exception& error) {
        ExpectEqual("js/" + name, std::string("internal error: ") + error.what() + "\n", expected, result);
    }
}

static std::string RunEngineDomRegistrationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><p id=\"target\">Hello</p></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript("var el = document.getElementById('target');\n", "dom-registration");
    return ok ? "registered\n" : "script failed\n";
}

static std::string RunEngineDeepDomRegistrationSnapshot() {
    std::string html = "<html><body>";
    for (int i = 0; i < 1600; ++i) {
        html += "<div>";
    }
    html += "leaf";
    for (int i = 0; i < 1600; ++i) {
        html += "</div>";
    }
    html += "</body></html>";

    JsEngine engine;
    auto dom = ParseHtml(html);
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript("var body = document.body;\n", "deep-dom-registration");
    return ok ? "registered\n" : "script failed\n";
}

static Node* FindByTag(Node* n, const std::string& tag) {
    if (!n) return nullptr;
    if (n->tagName == tag) return n;
    for (auto& c : n->children)
        if (Node* r = FindByTag(c.get(), tag)) return r;
    return nullptr;
}

static Node* FindById(Node* n, const std::string& id) {
    if (!n) return nullptr;
    if (n->attr("id") == id) return n;
    for (auto& c : n->children)
        if (Node* r = FindById(c.get(), id)) return r;
    return nullptr;
}

// Run a script against a tiny DOM, then read back the <p> node's attributes to
// prove that className / classList / style / textContent writes mutate the real
// DOM (not just the JS wrapper).
static std::string RunDomReflectionSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><p id=\"t\" class=\"a\">Hi</p></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var el = document.getElementById('t');\n"
        "el.classList.add('b');\n"
        "el.classList.remove('a');\n"
        "el.className += ' c';\n"
        "el.style.display = 'none';\n"
        "el.textContent = 'Bye';\n", "reflection");
    if (!ok) return "script failed\n";
    Node* p = FindByTag(dom.get(), "p");
    if (!p) return "no p\n";
    std::string text;
    for (auto& c : p->children) if (c->type == NodeType::Text) text += c->text;
    return "class=" + p->attr("class") + " style=" + p->attr("style")
         + " text=" + text + "\n";
}

static std::string RunDomCssomGeometrySnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body><div id=\"box\" style=\"position:absolute; left:12px; top:7px; width:123px; height:45px; display:block; color:red\">Hi</div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "var r = el.getBoundingClientRect();\n"
        "var cs = getComputedStyle(el);\n"
        "el.setAttribute('data-result', el.offsetWidth + 'x' + el.offsetHeight + ':' + r.left + ',' + r.top + ':' + cs.getPropertyValue('width') + ':' + cs.display);\n",
        "cssom-geometry");
    if (!ok) return "script failed\n";
    Node* box = FindById(dom.get(), "box");
    return box ? box->attr("data-result") + "\n" : "missing box\n";
}

static std::string RunDomObserverSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body><div id=\"box\" style=\"width:10px; height:20px\"></div></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "var mutations = 0, intersections = 0, resizes = 0;\n"
        "var mo = new MutationObserver(function(records){ mutations += records.length; });\n"
        "mo.observe(el, { attributes: true });\n"
        "el.setAttribute('data-x', '1');\n"
        "new IntersectionObserver(function(records){ intersections = records.length; }).observe(el);\n"
        "new ResizeObserver(function(records){ resizes = records.length; }).observe(el);\n"
        "el.setAttribute('data-result', mutations + ':' + intersections + ':' + resizes);\n",
        "observers");
    if (!ok) return "script failed\n";
    Node* box = FindById(dom.get(), "box");
    return box ? box->attr("data-result") + "\n" : "missing box\n";
}

static std::string RunDomDirtyCoalescingSnapshot() {
    JsEngine engine;
    int repaintCount = 0;
    auto dom = ParseHtml("<html><body><div id=\"box\"></div></body></html>");
    resetDomDirtyCoalesce();
    engine.setDocument(dom, [&]() { repaintCount++; });
    bool ok = engine.runScript(
        "var el = document.getElementById('box');\n"
        "el.firstCustomProperty = 1;\n"
        "el.secondCustomProperty = 2;\n",
        "dom-dirty-coalesce");
    resetDomDirtyCoalesce();
    if (!ok) return "script failed\n";
    return std::to_string(repaintCount) + "\n";
}

static std::string RunDomSelectorCompatibilitySnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body class=\"skin-vector client-js\">"
        "<main id=\"content\" class=\"mw-body\">"
        "<div class=\"mw-parser-output\">"
        "<p id=\"lead\" class=\"mw-empty-elt\">Lead</p>"
        "<section class=\"vector-dropdown\">"
        "<input id=\"toc-toggle\" class=\"vector-dropdown-checkbox\" type=\"checkbox\" checked>"
        "<div class=\"vector-dropdown-content\" data-mw=\"toc\">"
        "<a class=\"mw-list-item selected\" href=\"#History\">History</a>"
        "</div>"
        "</section>"
        "</div>"
        "</main>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "var out = document.querySelector('body.skin-vector') ? 'body' : 'missing';\n"
        "out = out + '|' + document.querySelector('#content .vector-dropdown-content[data-mw=\"toc\"] a.selected').textContent;\n"
        "out = out + '|' + document.querySelectorAll('main > div.mw-parser-output, input:checked').length;\n"
        "var toggle = document.querySelector('input.vector-dropdown-checkbox:checked');\n"
        "out = out + '|' + (toggle && toggle.matches('input:is(.vector-dropdown-checkbox, .other):not([disabled])') ? 'match' : 'nomatch');\n"
        "out = out + '|' + document.querySelector('[href^=\"#Hist\"]').getAttribute('href');\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', out);\n",
        "selector-compat");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomHistoryLocationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {}, "https://en.wikipedia.org/wiki/Wikipedia?oldid=1#top");
    bool ok = engine.runScript(
        "history.pushState(null, '', '/wiki/Helix#History');\n"
        "var a = location.href + '|' + location.pathname + '|' + location.hash + '|' + history.length;\n"
        "history.replaceState(null, '', '?search=browser');\n"
        "var b = location.href + '|' + location.search + '|' + history.length;\n"
        "location.assign('https://www.wikipedia.org/');\n"
        "var c = location.href + '|' + location.hostname + '|' + history.length;\n"
        "history.back();\n"
        "var d = location.href + '|' + history.length;\n"
        "history.forward();\n"
        "var e = location.href + '|' + history.length;\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', a + '\\n' + b + '\\n' + c + '\\n' + d + '\\n' + e);\n",
        "history-location");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunDomScrollApiSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml(
        "<html><body>"
        "<div id=\"top\" style=\"height:50px\"></div>"
        "<div id=\"target\" style=\"position:absolute; top:240px; height:20px\"></div>"
        "</body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "scrollTo(0, 120);\n"
        "var a = scrollY + ':' + pageYOffset;\n"
        "scrollBy(0, 35);\n"
        "var b = scrollY + ':' + pageYOffset;\n"
        "document.getElementById('target').scrollIntoView();\n"
        "var c = scrollY + ':' + pageYOffset;\n"
        "document.getElementsByTagName('body')[0].setAttribute('data-result', a + '|' + b + '|' + c);\n",
        "scroll-apis");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunWindowEventListenerSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.windowKeyCount = 0;\n"
        "globalThis.onWindowKey = function(event) { globalThis.windowKeyCount = globalThis.windowKeyCount + event.keyCode; };\n"
        "addEventListener('keydown', globalThis.onWindowKey);\n"
        "removeEventListener('keydown', function noop() {});\n",
        "window-listener-setup");
    if (!ok) return "script failed\n";
    engine.dispatchKeyDown(7, "g");
    ok = engine.runScript("removeEventListener('keydown', globalThis.onWindowKey);\n", "window-listener-remove");
    if (!ok) return "script failed\n";
    engine.dispatchKeyDown(11, "k");
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', String(globalThis.windowKeyCount));\n",
        "window-listener-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunStartupEventListenerSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.startupEvents = 'events:';\n"
        "document.addEventListener('DOMContentLoaded', function(event) { globalThis.startupEvents = globalThis.startupEvents + event.type; });\n"
        "addEventListener('load', function(event) { globalThis.startupEvents = globalThis.startupEvents + '|' + event.type; });\n",
        "startup-listener-setup");
    if (!ok) return "script failed\n";
    engine.dispatchDocumentEvent("DOMContentLoaded");
    engine.dispatchWindowEvent("load");
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.startupEvents);\n",
        "startup-listener-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunTimerCancellationSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.fired = 0;\n"
        "var timeoutId = setTimeout(function(){ globalThis.fired = globalThis.fired + 100; }, 0);\n"
        "clearTimeout(timeoutId);\n"
        "setTimeout(function(){ globalThis.fired = globalThis.fired + 2; }, 0);\n"
        "var frameId = requestAnimationFrame(function(){ globalThis.fired = globalThis.fired + 1000; });\n"
        "cancelAnimationFrame(frameId);\n"
        "requestAnimationFrame(function(ts){ globalThis.fired = globalThis.fired + (ts >= 0 ? 3 : 3000); });\n",
        "timer-cancellation-setup");
    if (!ok) return "script failed\n";
    engine.runMacrotasks();
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', String(globalThis.fired));\n",
        "timer-cancellation-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

static std::string RunFetchPromiseShapeSnapshot() {
    JsEngine engine;
    auto dom = ParseHtml("<html><body></body></html>");
    engine.setDocument(dom, []() {});
    bool ok = engine.runScript(
        "globalThis.fetched = 'pending';\n"
        "fetch('data:text/plain,hello').then(function(response) {\n"
        "  globalThis.fetched = response.ok + ':' + response.status + ':' + response.url;\n"
        "  return response.text();\n"
        "}).then(function(text) { globalThis.fetched = globalThis.fetched + ':' + text; });\n",
        "fetch-promise-shape");
    if (!ok) return "script failed\n";
    ok = engine.runScript(
        "document.getElementsByTagName('body')[0].setAttribute('data-result', globalThis.fetched);\n",
        "fetch-promise-result");
    if (!ok) return "script failed\n";
    Node* body = FindByTag(dom.get(), "body");
    return body ? body->attr("data-result") + "\n" : "missing body\n";
}

TestResult RunJsTests() {
    TestResult result;

    ExpectEqual(
        "js/dom/selector-compatibility",
        RunDomSelectorCompatibilitySnapshot(),
        "body|History|2|match|#History\n",
        result);

    ExpectEqual(
        "js/dom/history-location-state",
        RunDomHistoryLocationSnapshot(),
        "https://en.wikipedia.org/wiki/Helix#History|/wiki/Helix|#History|2\n"
        "https://en.wikipedia.org/wiki/Helix?search=browser|?search=browser|2\n"
        "https://www.wikipedia.org/|www.wikipedia.org|3\n"
        "https://en.wikipedia.org/wiki/Helix?search=browser|3\n"
        "https://www.wikipedia.org/|3\n",
        result);

    ExpectEqual(
        "js/dom/scroll-apis-update-window-offsets",
        RunDomScrollApiSnapshot(),
        "120:120|155:155|240:240\n",
        result);

    ExpectEqual(
        "js/window/event-listeners-dispatch-and-remove",
        RunWindowEventListenerSnapshot(),
        "7\n",
        result);

    ExpectEqual(
        "js/window/startup-events-dispatch",
        RunStartupEventListenerSnapshot(),
        "events:DOMContentLoaded|load\n",
        result);

    ExpectEqual(
        "js/async/timers-and-raf-can-cancel",
        RunTimerCancellationSnapshot(),
        "5\n",
        result);

    ExpectEqual(
        "js/async/fetch-uses-real-promise-shape",
        RunFetchPromiseShapeSnapshot(),
        "true:200:data:text/plain,hello:hello\n",
        result);

    ExpectEqual(
        "js/dom/property-writes-reflect-to-node",
        RunDomReflectionSnapshot(),
        "class=b c style=display: none text=Bye\n",
        result);

    ExpectEqual(
        "js/dom/cssom-geometry",
        RunDomCssomGeometrySnapshot(),
        "123x45:12,7:123px:block\n",
        result);

    ExpectEqual(
        "js/dom/observers-fire",
        RunDomObserverSnapshot(),
        "1:1:1\n",
        result);

    ExpectEqual(
        "js/dom/direct-property-writes-coalesce-repaint",
        RunDomDirtyCoalescingSnapshot(),
        "1\n",
        result);

    ExpectJsResult(
        "object/static-property",
        "var product = { title: 'book' };\n"
        "product.price = 12;\n"
        "__result = product.title + ':' + product.price;\n",
        "string: book:12\n",
        result);

    ExpectJsResult(
        "function/direct-call",
        "function add(left, right) { return left + right; }\n"
        "__result = add(2, 3);\n",
        "number: 5\n",
        result);

    ExpectJsResult(
        "array/callback-reduce",
        "var total = 0;\n"
        "[1, 2, 3].forEach(function (value) { total += value; });\n"
        "__result = total;\n",
        "number: 6\n",
        result);

    ExpectEqual(
        "js/engine/dom-registration-does-not-recurse",
        RunEngineDomRegistrationSnapshot(),
        "registered\n",
        result);

    ExpectEqual(
        "js/engine/deep-dom-registration-does-not-overflow",
        RunEngineDeepDomRegistrationSnapshot(),
        "registered\n",
        result);

    return result;
}

#include "js/dom_bridge.h"
#include "css/stylesheet.h"
#include "html/parser.h"
#include "network/resource_cache.h"
#include "network/cookies.h"
#include "network/url.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <chrono>
#include <cstdio>

#define NATIVE(name) [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue
#define ARG(i) (args.size() > (size_t)(i) ? args[i] : JsValue::undefined())
#define ARG_STR(i) ARG(i).toString()
#define ARG_NUM(i) ARG(i).toNumber()
#define ARG_INT(i) ARG(i).toInt32()

static void addNative(VM& vm, JsObject* obj, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(std::move(fn), name);
    obj->setProp(name, JsValue::object(fnObj));
}

// ── class-token helpers (for classList) ───────────────────────────────────────
static std::vector<std::string> classTokens(const std::string& cls) {
    std::vector<std::string> out;
    std::istringstream ss(cls);
    std::string t;
    while (ss >> t) out.push_back(t);
    return out;
}
static std::string classJoin(const std::vector<std::string>& toks) {
    std::string s;
    for (size_t i = 0; i < toks.size(); ++i) { if (i) s += ' '; s += toks[i]; }
    return s;
}
static bool classHas(const std::vector<std::string>& toks, const std::string& t) {
    return std::find(toks.begin(), toks.end(), t) != toks.end();
}

// ── Node store ────────────────────────────────────────────────────────────────
// We keep shared_ptr alive by storing them in a global registry keyed by raw ptr.
// This is safe because the VM and document share the same lifetime.

static std::unordered_map<Node*, std::shared_ptr<Node>> g_nodeStore;
static std::unordered_map<Node*, JsObject*> g_wrapperStore;

// Event listeners: node -> [(eventName, callbackFn), ...]
struct EventListener { std::string event; JsValue fn; };
static std::unordered_map<Node*, std::vector<EventListener>> g_eventListeners;
static std::vector<EventListener> g_windowEventListeners;
// Persistent GC roots for cached DOM wrappers. These live at STABLE heap
// addresses so the GC can mark them safely (rooting a stack local was a
// use-after-return bug). Cleared per document in registerDom().
static std::vector<std::unique_ptr<JsValue>> g_wrapperRoots;
static std::vector<std::unique_ptr<JsValue>> g_observerRoots;

struct MutationObserverEntry {
    JsObject* observer = nullptr;
    Node* target = nullptr;
    bool active = false;
    bool subtree = false;
    bool attributes = true;
    bool childList = true;
};
static std::vector<MutationObserverEntry> g_mutationObservers;

static std::shared_ptr<Node> getShared(Node* raw) {
    auto it = g_nodeStore.find(raw);
    if (it != g_nodeStore.end()) return it->second;
    return nullptr;
}

Node* unwrapNode(JsValue val) {
    if (!val.isObject()) return nullptr;
    return static_cast<Node*>(val.asObject()->domNode);
}

static std::string textContent(Node* n);

static std::string trimCopy(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool hasAttr(const Node* n, const std::string& name) {
    return n && n->attrs.find(name) != n->attrs.end();
}

static float parseFloatOr(const std::string& s, float fallback) {
    if (s.empty()) return fallback;
    try {
        size_t consumed = 0;
        float v = std::stof(s, &consumed);
        return consumed > 0 ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

static std::map<std::string, std::string> parseStyleMap(const std::string& raw) {
    std::map<std::string, std::string> out;
    std::istringstream ss(raw);
    std::string decl;
    while (std::getline(ss, decl, ';')) {
        size_t pos = decl.find(':');
        if (pos == std::string::npos) continue;
        std::string key = lowerCopy(trimCopy(decl.substr(0, pos)));
        std::string val = trimCopy(decl.substr(pos + 1));
        if (!key.empty()) out[key] = val;
    }
    return out;
}

static std::string numberCss(float v) {
    if (std::fabs(v - std::round(v)) < 0.01f) return std::to_string((int)std::round(v));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

static std::string pxCss(float v) {
    return numberCss(v) + "px";
}

static std::string colorCss(const CssColor& c) {
    if (!c.valid) return "";
    int r = (int)std::round(std::clamp(c.r, 0.f, 1.f) * 255.f);
    int g = (int)std::round(std::clamp(c.g, 0.f, 1.f) * 255.f);
    int b = (int)std::round(std::clamp(c.b, 0.f, 1.f) * 255.f);
    if (c.a < 0.999f) return "rgba(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ", " + numberCss(c.a) + ")";
    return "rgb(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ")";
}

static std::string defaultDisplayForTag(const Node* n) {
    if (!n || n->type != NodeType::Element) return "";
    const std::string& t = n->tagName;
    if (t == "span" || t == "a" || t == "b" || t == "strong" || t == "i"
        || t == "em" || t == "small" || t == "label") return "inline";
    if (t == "script" || t == "style" || t == "template" || (t == "dialog" && !hasAttr(n, "open"))) return "none";
    return "block";
}

static std::string displayName(const ComputedStyle& s, const Node* n) {
    switch (s.display) {
        case 1: return "block";
        case 2: return "inline";
        case 3: return "none";
        case 4: return "flex";
        case 5: return "table";
        case 6: return "table-cell";
        case 7: return "inline-block";
        case 8: return "list-item";
        case 9: return "table-row";
        case 10: return "table-row-group";
        case 11: return "grid";
        case 12: return "flow-root";
        case 13: return "contents";
        default: return defaultDisplayForTag(n);
    }
}

static std::string positionName(const ComputedStyle& s) {
    switch (s.positionMode) {
        case 1: return "relative";
        case 2: return "absolute";
        case 3: return "fixed";
        default: return "static";
    }
}

static std::string overflowName(const ComputedStyle& s) {
    switch (s.overflowMode) {
        case 1: return "hidden";
        case 2: return "auto";
        case 3: return "scroll";
        default: return "visible";
    }
}

struct DomMetrics {
    float left = 0;
    float top = 0;
    float width = 0;
    float height = 0;
};

static DomMetrics computeDomMetrics(Node* n) {
    DomMetrics m;
    if (!n || n->type != NodeType::Element) return m;
    ComputedStyle s = ParseInlineStyle(n->attr("style"));
    ResolveStyleVariables(s);
    if (s.display == 3 || (n->tagName == "dialog" && !hasAttr(n, "open"))) return m;

    m.width = s.width >= 0 ? s.width : parseFloatOr(n->attr("width"), -1.f);
    m.height = s.height >= 0 ? s.height : parseFloatOr(n->attr("height"), -1.f);

    auto directTextLen = [](Node* node) {
        size_t len = 0;
        for (const auto& child : node->children) {
            if (child && child->type == NodeType::Text) {
                len += child->text.size();
                if (len > 4096) return len;
            }
        }
        return len;
    };

    if (m.width < 0) {
        size_t textLen = directTextLen(n);
        m.width = textLen == 0 ? 0.f : std::max(16.f, (float)textLen * 8.f);
        if (textLen > 0) m.width += 8.f;
    }
    if (m.height < 0) {
        m.height = directTextLen(n) > 0 ? std::max(16.f, s.lineHeight > 0 ? s.lineHeight : 16.f) : 0.f;
    }

    float padX = std::max(0.f, s.paddingLeft) + std::max(0.f, s.paddingRight);
    float padY = std::max(0.f, s.paddingTop) + std::max(0.f, s.paddingBottom);
    float borderX = std::max(0.f, s.borderLeftWidth >= 0 ? s.borderLeftWidth : s.borderWidth)
                  + std::max(0.f, s.borderRightWidth >= 0 ? s.borderRightWidth : s.borderWidth);
    float borderY = std::max(0.f, s.borderTopWidth >= 0 ? s.borderTopWidth : s.borderWidth)
                  + std::max(0.f, s.borderBottomWidth >= 0 ? s.borderBottomWidth : s.borderWidth);
    if (s.boxSizing != 1) {
        m.width += padX + borderX;
        m.height += padY + borderY;
    }
    if (s.leftSet && !s.leftPercent) m.left = s.left;
    if (s.topSet && !s.topPercent) m.top = s.top;
    if (s.transformSet) {
        if (!s.transformTxPercent) m.left += s.transformTx;
        if (!s.transformTyPercent) m.top += s.transformTy;
        if (s.transformScale > 0) {
            m.width *= s.transformScale;
            m.height *= s.transformScale;
        }
    }
    return m;
}

static void markDomDirty(VM& vm, Node* target, const std::string& type);

// ── Build the text content of a subtree ──────────────────────────────────────

static std::string textContent(Node* n) {
    if (!n) return "";
    std::string s;
    std::vector<Node*> stack;
    stack.push_back(n);
    while (!stack.empty()) {
        Node* cur = stack.back();
        stack.pop_back();
        if (!cur) continue;
        if (cur->type == NodeType::Text) {
            s += cur->text;
            continue;
        }
        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return s;
}

static std::string innerHTML(VM& vm, Node* n);
static JsValue wrapNodeInternal(VM& vm, std::shared_ptr<Node> node, bool materializeRelations);

static bool observerCoversNode(const MutationObserverEntry& entry, Node* target) {
    if (!entry.active || !entry.target || !target) return false;
    if (entry.target == target) return true;
    if (!entry.subtree) return false;
    for (Node* cur = target->parent; cur; cur = cur->parent)
        if (cur == entry.target) return true;
    return false;
}

static void notifyMutationObservers(VM& vm, Node* target, const std::string& type) {
    for (auto& entry : g_mutationObservers) {
        if (!observerCoversNode(entry, target)) continue;
        if (type == "attributes" && !entry.attributes) continue;
        if (type == "childList" && !entry.childList) continue;
        JsValue callback = entry.observer ? entry.observer->getProp("_callback") : JsValue::undefined();
        if (!callback.isCallable()) continue;

        auto* record = vm.gc().newObject(ObjKind::Plain);
        record->setProp("type", vm.str(type));
        auto shared = getShared(target);
        record->setProp("target", shared ? wrapNode(vm, shared) : JsValue::null());
        auto* records = vm.gc().newArray();
        records->arrayPush(JsValue::object(record));
        try {
            vm.call(callback, JsValue::undefined(), { JsValue::object(records), JsValue::object(entry.observer) });
        } catch (...) {
        }
    }
}

static bool g_domDirtyCoalesced = false;
static bool g_domPaintDirtyCoalesced = false;
static DomBridgeCallbacks g_domCallbacks;
static float g_windowScrollX = 0.f;
static float g_windowScrollY = 0.f;

static void syncWindowScrollGlobals(VM& vm) {
    vm.setGlobal("pageXOffset", JsValue::integer((int32_t)std::round(g_windowScrollX)));
    vm.setGlobal("pageYOffset", JsValue::integer((int32_t)std::round(g_windowScrollY)));
    vm.setGlobal("scrollX",     JsValue::integer((int32_t)std::round(g_windowScrollX)));
    vm.setGlobal("scrollY",     JsValue::integer((int32_t)std::round(g_windowScrollY)));
}

static void setWindowScrollTo(VM& vm, float x, float y) {
    g_windowScrollX = std::max(0.f, x);
    g_windowScrollY = std::max(0.f, y);
    syncWindowScrollGlobals(vm);
}

static bool IsPaintOnlyDirtyType(const std::string& type) {
    if (type.rfind("style:", 0) != 0) return false;
    std::string prop = type.substr(6);
    for (char& c : prop) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return prop == "color"
        || prop == "background"
        || prop == "background-color"
        || prop == "border-color"
        || prop == "outline-color"
        || prop == "text-decoration"
        || prop == "text-decoration-color"
        || prop == "opacity"
        || prop == "visibility";
}

void notifyDomDirtyCoalesced(VM& vm, bool affectsLayout) {
    vm.domDirty = true;
    if (!affectsLayout && vm.onDomPaintDirty) {
        if (!g_domPaintDirtyCoalesced) {
            g_domPaintDirtyCoalesced = true;
            vm.onDomPaintDirty();
        }
        return;
    }
    if (!g_domDirtyCoalesced && vm.onDomDirty) {
        g_domDirtyCoalesced = true;
        vm.onDomDirty();
    }
}

static void markDomDirty(VM& vm, Node* target, const std::string& type) {
    notifyMutationObservers(vm, target, type);
    notifyDomDirtyCoalesced(vm, !IsPaintOnlyDirtyType(type));
}

// Call this from the platform timer to reset coalescing for the next batch.
void resetDomDirtyCoalesce() {
    g_domDirtyCoalesced = false;
    g_domPaintDirtyCoalesced = false;
}

static bool CanEagerSerialize(Node* n) {
    if (!n) return false;
    struct Entry { Node* node; int depth; };
    std::vector<Entry> stack;
    stack.push_back({ n, 0 });
    size_t count = 0;
    while (!stack.empty()) {
        Entry cur = stack.back();
        stack.pop_back();
        if (!cur.node) continue;
        if (++count > 512 || cur.depth > 64) return false;
        for (auto& child : cur.node->children)
            stack.push_back({ child.get(), cur.depth + 1 });
    }
    return true;
}

static std::string outerHTML(VM& vm, Node* n) {
    if (!n) return "";
    if (n->type == NodeType::Text) {
        std::string s = n->text;
        std::string r;
        for (char c : s) {
            if      (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (c == '&') r += "&amp;";
            else r += c;
        }
        return r;
    }
    std::string html = "<" + n->tagName;
    for (auto& [k, v] : n->attrs) html += " " + k + "=\"" + v + "\"";
    html += ">" + innerHTML(vm, n) + "</" + n->tagName + ">";
    return html;
}

static std::string innerHTML(VM& vm, Node* n) {
    std::string s;
    for (auto& c : n->children) s += outerHTML(vm, c.get());
    return s;
}

static bool isIdentChar(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || c == '-' || c == '\\';
}

static std::string stripQuotes(std::string s) {
    s = trimCopy(std::move(s));
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
        || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::vector<std::string> splitSelectorTopLevel(const std::string& input, char delimiter) {
    std::vector<std::string> out;
    std::string cur;
    int bracket = 0, paren = 0;
    char quote = 0;
    for (char c : input) {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '[') bracket++;
        else if (c == ']') bracket = std::max(0, bracket - 1);
        else if (c == '(') paren++;
        else if (c == ')') paren = std::max(0, paren - 1);
        if (c == delimiter && bracket == 0 && paren == 0) {
            std::string part = trimCopy(cur);
            if (!part.empty()) out.push_back(part);
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string part = trimCopy(cur);
    if (!part.empty()) out.push_back(part);
    return out;
}

struct SelectorPart {
    std::string simple;
    char combinatorBefore = 0;
};

static std::vector<SelectorPart> parseSelectorParts(const std::string& selector) {
    std::vector<SelectorPart> parts;
    std::string cur;
    int bracket = 0, paren = 0;
    char quote = 0;
    char pendingCombinator = 0;

    auto flush = [&]() {
        std::string simple = trimCopy(cur);
        if (!simple.empty()) {
            parts.push_back({ simple, parts.empty() ? char(0) : (pendingCombinator ? pendingCombinator : ' ') });
            cur.clear();
            pendingCombinator = ' ';
        }
    };

    for (size_t i = 0; i < selector.size(); ++i) {
        char c = selector[i];
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '[') { bracket++; cur += c; continue; }
        if (c == ']') { bracket = std::max(0, bracket - 1); cur += c; continue; }
        if (c == '(') { paren++; cur += c; continue; }
        if (c == ')') { paren = std::max(0, paren - 1); cur += c; continue; }

        if (bracket == 0 && paren == 0 && (c == '>' || c == '+' || c == '~')) {
            flush();
            pendingCombinator = c;
            while (i + 1 < selector.size()
                && std::isspace(static_cast<unsigned char>(selector[i + 1]))) ++i;
            continue;
        }
        if (bracket == 0 && paren == 0 && std::isspace(static_cast<unsigned char>(c))) {
            flush();
            if (pendingCombinator == 0) pendingCombinator = ' ';
            continue;
        }
        cur += c;
    }
    flush();
    return parts;
}

static bool hasClassToken(const Node* n, const std::string& cls) {
    if (!n || cls.empty()) return false;
    return classHas(classTokens(n->attr("class")), cls);
}

static std::vector<Node*> elementSiblings(const Node* n) {
    std::vector<Node*> out;
    if (!n || !n->parent) return out;
    for (const auto& child : n->parent->children)
        if (child && child->type == NodeType::Element)
            out.push_back(child.get());
    return out;
}

static Node* previousElementSibling(const Node* n) {
    Node* prev = nullptr;
    for (Node* sibling : elementSiblings(n)) {
        if (sibling == n) return prev;
        prev = sibling;
    }
    return nullptr;
}

static bool attrMatches(const Node* n, const std::string& raw) {
    std::string expr = trimCopy(raw);
    if (expr.empty()) return false;
    std::istringstream ss(expr);
    std::string first;
    ss >> first;
    expr = first.empty() ? expr : first;

    const std::vector<std::string> ops = { "~=", "|=", "^=", "$=", "*=", "=" };
    std::string name, op, value;
    size_t pos = std::string::npos;
    for (const auto& candidate : ops) {
        pos = expr.find(candidate);
        if (pos != std::string::npos) {
            op = candidate;
            name = trimCopy(expr.substr(0, pos));
            value = stripQuotes(expr.substr(pos + candidate.size()));
            break;
        }
    }
    if (op.empty()) name = trimCopy(expr);
    if (name.empty() || !hasAttr(n, name)) return false;
    if (op.empty()) return true;

    std::string actual = n->attr(name);
    if (op == "=") return actual == value;
    if (op == "^=") return actual.rfind(value, 0) == 0;
    if (op == "$=") return actual.size() >= value.size()
        && actual.compare(actual.size() - value.size(), value.size(), value) == 0;
    if (op == "*=") return actual.find(value) != std::string::npos;
    if (op == "~=") return classHas(classTokens(actual), value);
    if (op == "|=") return actual == value || actual.rfind(value + "-", 0) == 0;
    return false;
}

static bool matchesSelector(Node* n, const std::string& selector, Node* scope);

static bool matchesPseudo(Node* n, const std::string& name, const std::string& arg, Node* scope) {
    if (!n) return false;
    if (name == "not") return !matchesSelector(n, arg, scope);
    if (name == "is" || name == "where") return matchesSelector(n, arg, scope);
    if (name == "checked") return hasAttr(n, "checked") || hasAttr(n, "selected");
    if (name == "disabled") return hasAttr(n, "disabled");
    if (name == "enabled") return !hasAttr(n, "disabled");
    if (name == "empty") {
        for (const auto& child : n->children) {
            if (!child) continue;
            if (child->type == NodeType::Element) return false;
            if (child->type == NodeType::Text && !trimCopy(child->text).empty()) return false;
        }
        return true;
    }
    if (name == "first-child" || name == "last-child" || name == "only-child") {
        auto siblings = elementSiblings(n);
        if (siblings.empty()) return false;
        if (name == "first-child") return siblings.front() == n;
        if (name == "last-child") return siblings.back() == n;
        return siblings.size() == 1 && siblings.front() == n;
    }
    if (name == "root") return n->parent && n->parent->type == NodeType::Document;
    if (name == "scope") return n == scope;
    return false;
}

static bool matchesSimpleSelector(Node* n, const std::string& simple, Node* scope) {
    if (!n || n->type != NodeType::Element) return false;
    size_t i = 0;
    bool sawAnySelector = false;
    while (i < simple.size()) {
        char c = simple[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '*') { sawAnySelector = true; ++i; continue; }
        if (c == '#') {
            ++i;
            size_t start = i;
            while (i < simple.size() && isIdentChar(simple[i])) ++i;
            if (n->attr("id") != simple.substr(start, i - start)) return false;
            sawAnySelector = true;
            continue;
        }
        if (c == '.') {
            ++i;
            size_t start = i;
            while (i < simple.size() && isIdentChar(simple[i])) ++i;
            if (!hasClassToken(n, simple.substr(start, i - start))) return false;
            sawAnySelector = true;
            continue;
        }
        if (c == '[') {
            int depth = 1;
            size_t start = ++i;
            char quote = 0;
            while (i < simple.size() && depth > 0) {
                char ac = simple[i];
                if (quote) {
                    if (ac == quote) quote = 0;
                } else if (ac == '"' || ac == '\'') quote = ac;
                else if (ac == '[') depth++;
                else if (ac == ']') {
                    if (--depth == 0) break;
                }
                ++i;
            }
            if (i >= simple.size() || !attrMatches(n, simple.substr(start, i - start))) return false;
            ++i;
            sawAnySelector = true;
            continue;
        }
        if (c == ':') {
            ++i;
            size_t start = i;
            while (i < simple.size() && isIdentChar(simple[i])) ++i;
            std::string name = lowerCopy(simple.substr(start, i - start));
            std::string arg;
            if (i < simple.size() && simple[i] == '(') {
                int depth = 1;
                size_t argStart = ++i;
                char quote = 0;
                while (i < simple.size() && depth > 0) {
                    char pc = simple[i];
                    if (quote) {
                        if (pc == quote) quote = 0;
                    } else if (pc == '"' || pc == '\'') quote = pc;
                    else if (pc == '(') depth++;
                    else if (pc == ')') {
                        if (--depth == 0) break;
                    }
                    ++i;
                }
                if (i >= simple.size()) return false;
                arg = simple.substr(argStart, i - argStart);
                ++i;
            }
            if (!matchesPseudo(n, name, arg, scope)) return false;
            sawAnySelector = true;
            continue;
        }

        size_t start = i;
        while (i < simple.size() && isIdentChar(simple[i])) ++i;
        if (start == i) return false;
        std::string tag = lowerCopy(simple.substr(start, i - start));
        if (tag != lowerCopy(n->tagName)) return false;
        sawAnySelector = true;
    }
    return sawAnySelector;
}

static bool matchesSelectorParts(Node* n, const std::vector<SelectorPart>& parts, int index, Node* scope) {
    if (index < 0) return true;
    if (!matchesSimpleSelector(n, parts[(size_t)index].simple, scope)) return false;
    if (index == 0) return true;

    char combinator = parts[(size_t)index].combinatorBefore;
    if (combinator == '>') {
        return n->parent && matchesSelectorParts(n->parent, parts, index - 1, scope);
    }
    if (combinator == '+') {
        Node* prev = previousElementSibling(n);
        return prev && matchesSelectorParts(prev, parts, index - 1, scope);
    }
    if (combinator == '~') {
        for (Node* prev = previousElementSibling(n); prev; prev = previousElementSibling(prev))
            if (matchesSelectorParts(prev, parts, index - 1, scope)) return true;
        return false;
    }

    for (Node* cur = n->parent; cur; cur = cur->parent)
        if (matchesSelectorParts(cur, parts, index - 1, scope)) return true;
    return false;
}

static bool matchesSelector(Node* n, const std::string& selector, Node* scope) {
    for (const auto& group : splitSelectorTopLevel(selector, ',')) {
        auto parts = parseSelectorParts(group);
        if (!parts.empty() && matchesSelectorParts(n, parts, (int)parts.size() - 1, scope))
            return true;
    }
    return false;
}

static std::vector<std::shared_ptr<Node>> domQueryAll(Node* root, const std::string& sel) {
    std::vector<std::shared_ptr<Node>> result;
    size_t visited = 0;
    int depth = 0;
    std::function<void(Node*)> walk = [&](Node* n) {
        if (++depth > 1000) { --depth; return; }   // guard against deep/cyclic trees
        for (auto& c : n->children) {
            if (++visited > 200000) break;          // guard against runaway traversal
            if (matchesSelector(c.get(), sel, root)) result.push_back(c);
            walk(c.get());
        }
        --depth;
    };
    walk(root);
    return result;
}

// ── wrapNode ─────────────────────────────────────────────────────────────────

static JsValue wrapNodeInternal(VM& vm, std::shared_ptr<Node> node, bool materializeRelations) {
    if (!node) return JsValue::null();

    // Register in store
    Node* raw = node.get();
    g_nodeStore[raw] = node;

    auto cached = g_wrapperStore.find(raw);
    if (cached != g_wrapperStore.end()) return JsValue::object(cached->second);

    auto* obj = vm.gc().newObject(ObjKind::DomWrapper);
    obj->domNode = raw;
    g_wrapperStore[raw] = obj;

    // Keep the wrapper alive while it stays cached in g_wrapperStore by rooting
    // it at a stable heap address. Previously this rooted the address of a local
    // (&objValue); after the function returned, the GC dereferenced that
    // dangling stack pointer during marking — a non-deterministic crash/hang
    // that worsened as more nodes were wrapped (large DOMs).
    g_wrapperRoots.push_back(std::make_unique<JsValue>(JsValue::object(obj)));
    vm.gc().addRoot(g_wrapperRoots.back().get());
    JsValue objValue = *g_wrapperRoots.back();

    // ── Core properties ──

    if (node->type == NodeType::Text) {
        obj->setProp("nodeType", JsValue::integer(3));
        obj->setProp("nodeName", vm.str("#text"));
    } else if (node->type == NodeType::Document) {
        obj->setProp("nodeType", JsValue::integer(9));
        obj->setProp("nodeName", vm.str("#document"));
    } else {
        obj->setProp("nodeType", JsValue::integer(1));
        obj->setProp("nodeName", vm.str(node->tagName));
        obj->setProp("tagName",  vm.str(node->tagName));
        obj->setProp("localName",vm.str(node->tagName));
    }

    // ── id/className ──
    auto addPropAccessor = [&](const std::string& attr) {
        std::string val = node->attr(attr);
        obj->setProp(attr, vm.str(val));
    };
    addPropAccessor("id");
    addPropAccessor("class");
    obj->setProp("className", vm.str(node->attr("class")));

    // ── textContent / innerHTML / outerHTML ──
    // Only serialize small subtrees eagerly. For large nodes (document, html,
    // body…) this is skipped: serializing every wrapped node's whole subtree is
    // O(n^2) over a page and makes DOM-heavy scripts crawl to a halt.
    if (CanEagerSerialize(raw)) {
        obj->setProp("textContent", vm.str(textContent(raw)));
        obj->setProp("innerHTML",   vm.str(innerHTML(vm, raw)));
        obj->setProp("outerHTML",   vm.str(outerHTML(vm, raw)));
    } else {
        obj->setProp("textContent", vm.str(""));
        obj->setProp("innerHTML",   vm.str(""));
        obj->setProp("outerHTML",   vm.str(""));
    }

    // ── attributes map ──
    {
        auto* attrsObj = vm.gc().newObject(ObjKind::Plain);
        for (auto& [k, v] : node->attrs) attrsObj->setProp(k, vm.str(v));
        obj->setProp("attributes", JsValue::object(attrsObj));
    }

    // ── style object ──
    {
        auto* style = vm.gc().newObject(ObjKind::Plain);
        style->domNode = raw;  // Plain+domNode marks a style obj for the set hook
        // Parse inline style
        const std::string& styleStr = node->attr("style");
        std::istringstream ss(styleStr);
        std::string decl;
        while (std::getline(ss, decl, ';')) {
            auto pos = decl.find(':');
            if (pos != std::string::npos) {
                std::string k = decl.substr(0, pos), v = decl.substr(pos+1);
                auto trim = [](std::string s) { size_t b=s.find_first_not_of(" \t"); size_t e=s.find_last_not_of(" \t"); return b==std::string::npos?"":s.substr(b,e-b+1); };
                k = trim(k); v = trim(v);
                if (!k.empty()) style->setProp(k, vm.str(v));
            }
        }
        // setProperty / removeProperty helpers
        addNative(vm, style, "setProperty", [](VM& vm2, JsValue t, std::vector<JsValue> a) -> JsValue {
            if (!t.isObject()) return JsValue::undefined();
            std::string prop = a.size()>0?a[0].toString():"";
            std::string val  = a.size()>1?a[1].toString():"";
            if (!prop.empty()) {
                t.asObject()->setProp(prop, vm2.str(val));
                if (t.asObject()->domNode && vm2.onDomPropSet)
                    vm2.onDomPropSet(t.asObject(), prop, vm2.str(val));
            }
            return JsValue::undefined();
        });
        addNative(vm, style, "removeProperty", [](VM& vm2, JsValue t, std::vector<JsValue> a) -> JsValue {
            if (!t.isObject() || a.empty()) return JsValue::undefined();
            std::string prop = a[0].toString();
            JsValue old = t.asObject()->getProp(prop);
            t.asObject()->props.erase(prop);
            if (t.asObject()->domNode && vm2.onDomPropSet)
                vm2.onDomPropSet(t.asObject(), prop, vm2.str(""));
            return old;
        });
        obj->setProp("style", JsValue::object(style));
    }

    // ── DOM traversal ──
    auto addNativeM = [&](const std::string& name, NativeFn fn) {
        obj->setProp(name, JsValue::object(vm.gc().newNativeFunction(fn, name)));
    };

    addNativeM("getAttribute", NATIVE("getAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::null();
        std::string k = args.empty() ? "" : args[0].toString();
        auto it = n->attrs.find(k);
        return it != n->attrs.end() ? vm.str(it->second) : JsValue::null();
    });
    addNativeM("setAttribute", NATIVE("setAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2) return JsValue::undefined();
        n->attrs[args[0].toString()] = args[1].toString();
        markDomDirty(vm, n, "attributes");
        return JsValue::undefined();
    });
    addNativeM("removeAttribute", NATIVE("removeAttribute") {
        Node* n = unwrapNode(thisVal);
        if (n && !args.empty()) n->attrs.erase(args[0].toString());
        markDomDirty(vm, n, "attributes");
        return JsValue::undefined();
    });
    addNativeM("hasAttribute", NATIVE("hasAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        return JsValue::boolean(n->attrs.count(args[0].toString()) > 0);
    });

    // classList — back-pointer to the owner node lets the methods mutate the
    // real class attribute and trigger relayout (used by show/hide toggles).
    {
        auto* cl = vm.gc().newObject(ObjKind::Plain);
        cl->domNode = raw;  // owner element, so unwrapNode(thisVal) resolves it
        addNative(vm, cl, "add", NATIVE("classList_add") {
            Node* n = unwrapNode(thisVal);
            if (!n) return JsValue::undefined();
            auto toks = classTokens(n->attr("class"));
            for (auto& a : args) { std::string t = a.toString();
                if (!t.empty() && !classHas(toks, t)) toks.push_back(t); }
            n->attrs["class"] = classJoin(toks);
            markDomDirty(vm, n, "attributes");
            return JsValue::undefined();
        });
        addNative(vm, cl, "remove", NATIVE("classList_remove") {
            Node* n = unwrapNode(thisVal);
            if (!n) return JsValue::undefined();
            auto toks = classTokens(n->attr("class"));
            for (auto& a : args) { std::string t = a.toString();
                toks.erase(std::remove(toks.begin(), toks.end(), t), toks.end()); }
            n->attrs["class"] = classJoin(toks);
            markDomDirty(vm, n, "attributes");
            return JsValue::undefined();
        });
        addNative(vm, cl, "toggle", NATIVE("classList_toggle") {
            Node* n = unwrapNode(thisVal);
            if (!n || args.empty()) return JsValue::boolean(false);
            std::string t = args[0].toString();
            auto toks = classTokens(n->attr("class"));
            bool has = classHas(toks, t);
            bool want = args.size() > 1 ? args[1].toBool() : !has;
            if (want && !has) toks.push_back(t);
            else if (!want && has) toks.erase(std::remove(toks.begin(), toks.end(), t), toks.end());
            n->attrs["class"] = classJoin(toks);
            markDomDirty(vm, n, "attributes");
            return JsValue::boolean(want);
        });
        addNative(vm, cl, "contains", NATIVE("classList_contains") {
            Node* n = unwrapNode(thisVal);
            if (!n || args.empty()) return JsValue::boolean(false);
            return JsValue::boolean(classHas(classTokens(n->attr("class")), args[0].toString()));
        });
        obj->setProp("classList", JsValue::object(cl));
    }

    // children / childNodes
    if (materializeRelations) {
        auto* children  = vm.gc().newArray();
        auto* childNodes = vm.gc().newArray();
        for (auto& c : node->children) {
            JsValue wrapped = wrapNodeInternal(vm, c, false);
            childNodes->arrayPush(wrapped);
            if (c->type == NodeType::Element) children->arrayPush(wrapped);
        }
        obj->setProp("children",   JsValue::object(children));
        obj->setProp("childNodes", JsValue::object(childNodes));
        obj->setProp("childElementCount", JsValue::integer((int32_t)children->arrayLength()));
    } else {
        obj->setProp("children", JsValue::object(vm.gc().newArray()));
        obj->setProp("childNodes", JsValue::object(vm.gc().newArray()));
        obj->setProp("childElementCount", JsValue::integer(0));
    }

    // firstChild / lastChild / firstElementChild / lastElementChild
    if (materializeRelations && !node->children.empty()) {
        obj->setProp("firstChild", wrapNodeInternal(vm, node->children.front(), false));
        obj->setProp("lastChild",  wrapNodeInternal(vm, node->children.back(), false));
        for (auto& c : node->children) {
            if (c->type == NodeType::Element) { obj->setProp("firstElementChild", wrapNodeInternal(vm, c, false)); break; }
        }
        for (int i = (int)node->children.size()-1; i >= 0; i--) {
            if (node->children[i]->type == NodeType::Element) { obj->setProp("lastElementChild", wrapNodeInternal(vm, node->children[i], false)); break; }
        }
    } else {
        obj->setProp("firstChild",        JsValue::null());
        obj->setProp("lastChild",         JsValue::null());
        obj->setProp("firstElementChild", JsValue::null());
        obj->setProp("lastElementChild",  JsValue::null());
    }

    // parentNode / parentElement
    if (materializeRelations && node->parent) {
        auto parentShared = getShared(node->parent);
        if (parentShared) {
            JsValue parentWrapped = wrapNodeInternal(vm, parentShared, false);
            obj->setProp("parentNode",    parentWrapped);
            obj->setProp("parentElement", parentWrapped);
        }
    } else {
        obj->setProp("parentNode",    JsValue::null());
        obj->setProp("parentElement", JsValue::null());
    }

    // Sibling traversal
    if (raw && raw->parent) {
        const auto& siblings = raw->parent->children;
        // nextElementSibling / previousElementSibling
        bool foundSelf = false;
        std::shared_ptr<Node> prevElem, nextElem;
        for (size_t si = 0; si < siblings.size(); ++si) {
            if (siblings[si].get() == raw) {
                foundSelf = true;
                continue;
            }
            if (siblings[si]->type == NodeType::Element) {
                if (!foundSelf) prevElem = siblings[si];
                else if (!nextElem) nextElem = siblings[si];
            }
        }
        obj->setProp("nextElementSibling",     nextElem ? wrapNodeInternal(vm, nextElem, false) : JsValue::null());
        obj->setProp("previousElementSibling", prevElem ? wrapNodeInternal(vm, prevElem, false) : JsValue::null());
    } else {
        obj->setProp("nextElementSibling",     JsValue::null());
        obj->setProp("previousElementSibling", JsValue::null());
    }

    // dataset — proxy for data-* attributes.
    {
        auto* ds = vm.gc().newObject(ObjKind::Plain);
        if (raw) {
            for (auto& [k, v] : raw->attrs) {
                if (k.rfind("data-", 0) == 0) {
                    // Convert "data-my-attr" to "myAttr" (camelCase).
                    std::string camel;
                    bool upper = false;
                    for (size_t ci = 5; ci < k.size(); ++ci) {
                        if (k[ci] == '-') { upper = true; continue; }
                        camel += upper ? (char)std::toupper((unsigned char)k[ci]) : k[ci];
                        upper = false;
                    }
                    ds->setProp(camel, vm.str(v));
                }
            }
        }
        obj->setProp("dataset", JsValue::object(ds));
    }

    // Layout dimensions. The JS bridge does not own the renderer's box tree,
    // but inline CSS/HTML dimensions are enough for many visibility checks.
    DomMetrics metrics = computeDomMetrics(raw);
    obj->setProp("offsetWidth",  JsValue::integer((int32_t)std::round(metrics.width)));
    obj->setProp("offsetHeight", JsValue::integer((int32_t)std::round(metrics.height)));
    obj->setProp("offsetTop",    JsValue::integer((int32_t)std::round(metrics.top)));
    obj->setProp("offsetLeft",   JsValue::integer((int32_t)std::round(metrics.left)));
    obj->setProp("offsetParent", JsValue::null());
    obj->setProp("clientWidth",  JsValue::integer((int32_t)std::round(metrics.width)));
    obj->setProp("clientHeight", JsValue::integer((int32_t)std::round(metrics.height)));
    obj->setProp("scrollWidth",  JsValue::integer((int32_t)std::round(metrics.width)));
    obj->setProp("scrollHeight", JsValue::integer((int32_t)std::round(metrics.height)));
    obj->setProp("scrollTop",    JsValue::integer(0));
    obj->setProp("scrollLeft",   JsValue::integer(0));

    // DOM mutations
    addNativeM("appendChild", NATIVE("appendChild") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return ARG(0);
        Node* child = unwrapNode(ARG(0));
        if (!child) return ARG(0);
        auto childShared = getShared(child);
        if (childShared) { n->appendChild(childShared); markDomDirty(vm, n, "childList"); }
        return ARG(0);
    });
    addNativeM("removeChild", NATIVE("removeChild") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return ARG(0);
        Node* child = unwrapNode(ARG(0));
        if (!child) return ARG(0);
        auto& ch = n->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==child; }), ch.end());
        markDomDirty(vm, n, "childList");
        return ARG(0);
    });
    addNativeM("insertBefore", NATIVE("insertBefore") {
        Node* n = unwrapNode(thisVal);
        if (!n) return ARG(0);
        Node* child = unwrapNode(ARG(0)), *ref = unwrapNode(ARG(1));
        if (!child) return ARG(0);
        auto childShared = getShared(child);
        if (!childShared) return ARG(0);
        if (!ref) { n->appendChild(childShared); }
        else {
            auto& ch = n->children;
            auto it = std::find_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==ref; });
            if (it != ch.end()) ch.insert(it, childShared);
        }
        markDomDirty(vm, n, "childList");
        return ARG(0);
    });
    addNativeM("replaceChild", NATIVE("replaceChild") {
        Node* n = unwrapNode(thisVal);
        if (!n) return ARG(0);
        Node* newNode = unwrapNode(ARG(0)), *oldNode = unwrapNode(ARG(1));
        if (!newNode || !oldNode) return ARG(0);
        auto newShared = getShared(newNode);
        for (auto& c : n->children) {
            if (c.get() == oldNode) { c = newShared; break; }
        }
        markDomDirty(vm, n, "childList");
        return ARG(1);
    });
    addNativeM("cloneNode", NATIVE("cloneNode") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::null();
        auto clone = std::make_shared<Node>();
        clone->type = n->type; clone->tagName = n->tagName;
        clone->text = n->text; clone->attrs = n->attrs;
        g_nodeStore[clone.get()] = clone;
        return wrapNode(vm, clone);
    });

    // querySelector / querySelectorAll (simplified: id/class/tag only)
    addNativeM("querySelector", NATIVE("querySelector") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        auto results = domQueryAll(n, args[0].toString());
        return results.empty() ? JsValue::null() : wrapNode(vm, results[0]);
    });
    addNativeM("querySelectorAll", NATIVE("querySelectorAll") {
        Node* n = unwrapNode(thisVal);
        auto* arr = vm.gc().newArray();
        if (!n || args.empty()) return JsValue::object(arr);
        for (auto& found : domQueryAll(n, args[0].toString())) arr->arrayPush(wrapNode(vm, found));
        return JsValue::object(arr);
    });
    addNativeM("getElementById", NATIVE("getElementById") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        auto results = domQueryAll(n, "#" + args[0].toString());
        return results.empty() ? JsValue::null() : wrapNode(vm, results[0]);
    });
    addNativeM("getElementsByClassName", NATIVE("getElementsByClassName") {
        Node* n = unwrapNode(thisVal);
        auto* arr = vm.gc().newArray();
        if (!n || args.empty()) return JsValue::object(arr);
        for (auto& found : domQueryAll(n, "." + args[0].toString())) arr->arrayPush(wrapNode(vm, found));
        return JsValue::object(arr);
    });
    addNativeM("getElementsByTagName", NATIVE("getElementsByTagName") {
        Node* n = unwrapNode(thisVal);
        auto* arr = vm.gc().newArray();
        if (!n || args.empty()) return JsValue::object(arr);
        for (auto& found : domQueryAll(n, args[0].toString())) arr->arrayPush(wrapNode(vm, found));
        return JsValue::object(arr);
    });
    addNativeM("createElement", NATIVE("createElement") {
        auto newNode = Node::makeElement(ARG_STR(0));
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createTextNode", NATIVE("createTextNode") {
        auto newNode = Node::makeText(ARG_STR(0));
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createComment", NATIVE("createComment") {
        auto newNode = Node::makeElement("!--");
        newNode->text = ARG_STR(0);
        g_nodeStore[newNode.get()] = newNode;
        return wrapNode(vm, newNode);
    });
    addNativeM("createDocumentFragment", NATIVE("createDocumentFragment") {
        auto frag = Node::makeElement("#document-fragment");
        frag->type = NodeType::Document;
        g_nodeStore[frag.get()] = frag;
        return wrapNode(vm, frag);
    });

    addNativeM("addEventListener", NATIVE("addEventListener") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2 || !ARG(1).isCallable()) return JsValue::undefined();
        g_eventListeners[n].push_back({ ARG_STR(0), ARG(1) });
        return JsValue::undefined();
    });
    addNativeM("removeEventListener", NATIVE("removeEventListener") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2) return JsValue::undefined();
        std::string ev = ARG_STR(0);
        auto it = g_eventListeners.find(n);
        if (it != g_eventListeners.end()) {
            auto& list = it->second;
            for (auto li = list.begin(); li != list.end(); ++li)
                if (li->event == ev) { list.erase(li); break; }
        }
        return JsValue::undefined();
    });
    addNativeM("dispatchEvent", NATIVE("dispatchEvent") {
        return JsValue::boolean(true);
    });
    addNativeM("matches", NATIVE("matches") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        return JsValue::boolean(matchesSelector(n, ARG_STR(0), n));
    });
    addNativeM("closest", NATIVE("closest") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        std::string sel = ARG_STR(0);
        Node* cur = n;
        while (cur) {
            if (matchesSelector(cur, sel, n)) { auto s = getShared(cur); if (s) return wrapNode(vm, s); }
            cur = cur->parent;
        }
        return JsValue::null();
    });

    addNativeM("getBoundingClientRect", NATIVE("getBoundingClientRect") {
        Node* n = unwrapNode(thisVal);
        DomMetrics rect = computeDomMetrics(n);
        auto* r = vm.gc().newObject(ObjKind::Plain);
        r->setProp("top",    JsValue::number(rect.top));
        r->setProp("left",   JsValue::number(rect.left));
        r->setProp("bottom", JsValue::number(rect.top + rect.height));
        r->setProp("right",  JsValue::number(rect.left + rect.width));
        r->setProp("width",  JsValue::number(rect.width));
        r->setProp("height", JsValue::number(rect.height));
        r->setProp("x",      JsValue::number(rect.left));
        r->setProp("y",      JsValue::number(rect.top));
        return JsValue::object(r);
    });
    addNativeM("scrollIntoView", NATIVE("scrollIntoView") {
        Node* n = unwrapNode(thisVal);
        if (!n) return JsValue::undefined();
        DomMetrics rect = computeDomMetrics(n);
        setWindowScrollTo(vm, g_windowScrollX, rect.top);
        if (g_domCallbacks.scrollIntoView) g_domCallbacks.scrollIntoView(n);
        return JsValue::undefined();
    });
    addNativeM("focus",  NATIVE("focus")  {
        if (Node* n = unwrapNode(thisVal)) {
            SetCssFocusNode(n);
            markDomDirty(vm, n, "attributes");
        }
        return JsValue::undefined();
    });
    addNativeM("blur",   NATIVE("blur")   {
        if (Node* n = unwrapNode(thisVal)) {
            SetCssFocusNode(nullptr);
            markDomDirty(vm, n, "attributes");
        }
        return JsValue::undefined();
    });
    addNativeM("click",  NATIVE("click")  { return JsValue::undefined(); });
    addNativeM("remove", NATIVE("remove") {
        Node* n = unwrapNode(thisVal);
        if (!n || !n->parent) return JsValue::undefined();
        auto& ch = n->parent->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==n; }), ch.end());
        markDomDirty(vm, n->parent, "childList");
        return JsValue::undefined();
    });

    // value / checked for form elements
    obj->setProp("value",   vm.str(node->attr("value")));
    obj->setProp("checked", JsValue::boolean(hasAttr(raw, "checked")));
    obj->setProp("disabled",JsValue::boolean(hasAttr(raw, "disabled")));
    obj->setProp("href",    vm.str(node->attr("href")));
    obj->setProp("src",     vm.str(node->attr("src")));
    obj->setProp("alt",     vm.str(node->attr("alt")));
    obj->setProp("type",    vm.str(node->attr("type")));
    obj->setProp("name",    vm.str(node->attr("name")));

    vm.gc().removeRoot(&objValue);
    return objValue;
}

JsValue wrapNode(VM& vm, std::shared_ptr<Node> node) {
    return wrapNodeInternal(vm, std::move(node), true);
}

// ── registerDom ───────────────────────────────────────────────────────────────

// camelCase CSS property → kebab-case (backgroundColor → background-color).
static std::string cssKebab(const std::string& k) {
    std::string out;
    for (char c : k) {
        if (c >= 'A' && c <= 'Z') { out += '-'; out += (char)(c - 'A' + 'a'); }
        else out += c;
    }
    return out;
}

// Re-serialize a style object's own CSS properties into the node's style attr.
static void rebuildStyleAttr(Node* n, JsObject* styleObj) {
    std::string s;
    for (const auto& key : styleObj->ownEnumKeys()) {
        JsValue v = styleObj->getProp(key);
        if (v.isObject()) continue;             // skip methods like setProperty
        std::string val = v.toString();
        if (val.empty()) continue;
        if (!s.empty()) s += "; ";
        s += cssKebab(key) + ": " + val;
    }
    n->attrs["style"] = s;
}

static std::string cssCamel(const std::string& key) {
    std::string out;
    bool upper = false;
    for (char c : key) {
        if (c == '-') { upper = true; continue; }
        out += upper ? (char)std::toupper((unsigned char)c) : c;
        upper = false;
    }
    return out;
}

static std::map<std::string, std::string> computedStyleMap(Node* n) {
    std::map<std::string, std::string> props;
    if (!n) return props;
    auto raw = parseStyleMap(n->attr("style"));
    ComputedStyle s = ParseInlineStyle(n->attr("style"));
    ResolveStyleVariables(s);

    auto set = [&](const std::string& kebab, const std::string& val) {
        props[kebab] = val;
        props[cssCamel(kebab)] = val;
    };
    auto rawOr = [&](const std::string& key, const std::string& fallback) {
        auto it = raw.find(key);
        return it != raw.end() ? it->second : fallback;
    };

    set("display", rawOr("display", displayName(s, n)));
    set("position", rawOr("position", positionName(s)));
    set("visibility", rawOr("visibility", s.visibilityHidden ? "hidden" : "visible"));
    set("overflow", rawOr("overflow", overflowName(s)));
    set("width", rawOr("width", s.width >= 0 ? pxCss(s.width) : ""));
    set("height", rawOr("height", s.height >= 0 ? pxCss(s.height) : ""));
    set("margin-top", rawOr("margin-top", s.marginTopSet() ? pxCss(s.marginTop) : "0px"));
    set("margin-right", rawOr("margin-right", s.marginRightSet() ? pxCss(s.marginRight) : "0px"));
    set("margin-bottom", rawOr("margin-bottom", s.marginBottomSet() ? pxCss(s.marginBottom) : "0px"));
    set("margin-left", rawOr("margin-left", s.marginLeftSet() ? pxCss(s.marginLeft) : "0px"));
    set("padding-top", rawOr("padding-top", s.paddingTop >= 0 ? pxCss(s.paddingTop) : "0px"));
    set("padding-right", rawOr("padding-right", s.paddingRight >= 0 ? pxCss(s.paddingRight) : "0px"));
    set("padding-bottom", rawOr("padding-bottom", s.paddingBottom >= 0 ? pxCss(s.paddingBottom) : "0px"));
    set("padding-left", rawOr("padding-left", s.paddingLeft >= 0 ? pxCss(s.paddingLeft) : "0px"));
    set("color", rawOr("color", colorCss(s.color)));
    set("background-color", rawOr("background-color", s.bgColorSet ? colorCss(s.bgColor) : "rgba(0, 0, 0, 0)"));
    set("background-image", rawOr("background-image", s.backgroundImageSet ? "url(" + s.backgroundImage + ")" : "none"));
    set("font-size", rawOr("font-size", s.fontSize > 0 ? pxCss(s.fontSize) : "16px"));
    set("font-family", rawOr("font-family", s.fontFamily));
    set("font-weight", rawOr("font-weight", s.bold ? "700" : "400"));
    set("opacity", rawOr("opacity", numberCss(s.opacity)));
    set("z-index", rawOr("z-index", s.zIndexSet ? std::to_string(s.zIndex) : "auto"));
    set("top", rawOr("top", s.topSet && !s.topPercent ? pxCss(s.top) : "auto"));
    set("left", rawOr("left", s.leftSet && !s.leftPercent ? pxCss(s.left) : "auto"));
    set("right", rawOr("right", s.rightSet && !s.rightPercent ? pxCss(s.right) : "auto"));
    set("bottom", rawOr("bottom", s.bottomSet && !s.bottomPercent ? pxCss(s.bottom) : "auto"));
    set("transform", rawOr("transform", s.transformSet ? "matrix(1, 0, 0, 1, " + numberCss(s.transformTx) + ", " + numberCss(s.transformTy) + ")" : "none"));
    set("transition", rawOr("transition", s.transitionSet ? s.transitionProperty + " " + numberCss(s.transitionDuration) + "s" : ""));

    for (const auto& [key, val] : raw) set(key, val);
    return props;
}

// Replace a node's children with the parsed contents of an HTML fragment.
static void setInnerHtml(Node* parent, const std::string& html) {
    if (!parent) return;
    auto doc = ParseHtml(html);
    std::function<Node*(Node*, const std::string&)> findTag =
        [&](Node* node, const std::string& tag) -> Node* {
            for (auto& c : node->children) {
                if (c->tagName == tag) return c.get();
                if (Node* r = findTag(c.get(), tag)) return r;
            }
            return nullptr;
        };
    Node* content = doc.get();
    if (Node* body = findTag(doc.get(), "body")) content = body;
    parent->children.clear();
    for (auto& c : content->children) {
        c->parent = parent;
        parent->children.push_back(c);
    }
}

struct ParsedDomUrl {
    std::string href;
    std::string protocol;
    std::string host;
    std::string hostname;
    std::string port;
    std::string pathname = "/";
    std::string search;
    std::string hash;
    std::string origin;
};

static std::string removeHash(const std::string& href) {
    size_t hash = href.find('#');
    return hash == std::string::npos ? href : href.substr(0, hash);
}

static std::string removeQueryAndHash(const std::string& href) {
    size_t stop = href.find_first_of("?#");
    return stop == std::string::npos ? href : href.substr(0, stop);
}

static std::string resolveDomUrl(const std::string& href, const std::string& base) {
    if (href.empty()) return base;
    if (href[0] == '#') return removeHash(base) + href;
    if (href[0] == '?') return removeQueryAndHash(base) + href;
    return ResolveUrlAgainstBase(href, base);
}

static ParsedDomUrl parseDomUrl(const std::string& href) {
    ParsedDomUrl out;
    out.href = href;
    size_t scheme = href.find("://");
    if (scheme == std::string::npos) return out;

    out.protocol = href.substr(0, scheme + 1);
    size_t hostStart = scheme + 3;
    size_t hostEnd = href.find_first_of("/?#", hostStart);
    out.host = href.substr(hostStart,
        hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
    out.hostname = out.host;
    size_t colon = out.host.rfind(':');
    if (colon != std::string::npos && out.host.find(']') == std::string::npos) {
        out.hostname = out.host.substr(0, colon);
        out.port = out.host.substr(colon + 1);
    }
    out.origin = href.substr(0, scheme + 3) + out.host;

    if (hostEnd != std::string::npos && href[hostEnd] == '/') {
        size_t qmark = href.find('?', hostEnd);
        size_t hashMark = href.find('#', hostEnd);
        size_t pathEnd = std::min(qmark, hashMark);
        out.pathname = href.substr(hostEnd,
            pathEnd != std::string::npos ? pathEnd - hostEnd : std::string::npos);
    }
    size_t qmark = href.find('?', hostEnd == std::string::npos ? hostStart : hostEnd);
    size_t hashMark = href.find('#', hostEnd == std::string::npos ? hostStart : hostEnd);
    if (qmark != std::string::npos) {
        size_t searchEnd = (hashMark != std::string::npos && hashMark > qmark)
            ? hashMark : std::string::npos;
        out.search = href.substr(qmark,
            searchEnd != std::string::npos ? searchEnd - qmark : std::string::npos);
    }
    if (hashMark != std::string::npos) out.hash = href.substr(hashMark);
    return out;
}

static void applyLocationProps(VM& vm, JsObject* loc, const std::string& href) {
    ParsedDomUrl parsed = parseDomUrl(href);
    loc->setProp("href",     vm.str(parsed.href));
    loc->setProp("protocol", vm.str(parsed.protocol));
    loc->setProp("host",     vm.str(parsed.host));
    loc->setProp("hostname", vm.str(parsed.hostname));
    loc->setProp("pathname", vm.str(parsed.pathname));
    loc->setProp("search",   vm.str(parsed.search));
    loc->setProp("hash",     vm.str(parsed.hash));
    loc->setProp("origin",   vm.str(parsed.origin));
    loc->setProp("port",     vm.str(parsed.port));
}

void registerDom(VM& vm, std::shared_ptr<Node> docNode,
                 std::function<void()> onRepaint,
                 const std::string& pageUrl,
                 DomBridgeCallbacks callbacks) {
    vm.onDomDirty = onRepaint;
    vm.onDomPaintDirty = callbacks.repaintOnly ? callbacks.repaintOnly : onRepaint;
    g_domCallbacks = std::move(callbacks);
    g_windowScrollX = 0.f;
    g_windowScrollY = 0.f;

    // Reflect live JS property writes onto the backing DOM node.
    vm.onDomPropSet = [vmPtr = &vm](JsObject* wrapper, const std::string& key, JsValue val) {
        Node* n = static_cast<Node*>(wrapper->domNode);
        if (!n) return;
        // A Plain object carrying a domNode is the style object (reflect CSS) or
        // classList (mutated via its own methods, so ignore stray writes here).
        if (wrapper->kind != ObjKind::DomWrapper) {
            if (wrapper->hasOwn("toggle")) return;   // classList, not a style obj
            rebuildStyleAttr(n, wrapper);
            markDomDirty(*vmPtr, n, "style:" + key);
            return;
        }
        if (key == "className" || key == "class") n->attrs["class"] = val.toString();
        else if (key == "id")    n->attrs["id"] = val.toString();
        else if (key == "value") n->attrs["value"] = val.toString();
        else if (key == "checked") {
            if (val.toBool()) n->attrs["checked"] = "checked";
            else n->attrs.erase("checked");
        } else if (key == "disabled") {
            if (val.toBool()) n->attrs["disabled"] = "disabled";
            else n->attrs.erase("disabled");
        }
        else if (key == "textContent" || key == "innerText") {
            n->children.clear();
            std::string t = val.toString();
            if (!t.empty()) n->appendChild(Node::makeText(t));
        } else if (key == "innerHTML") {
            setInnerHtml(n, val.toString());
        }
        markDomDirty(*vmPtr, n, (key == "textContent" || key == "innerText" || key == "innerHTML") ? "childList" : "attributes");
    };

    // Symmetric live getter: keep className/id/value reads in sync with the node
    // after classList/setAttribute mutations (a stale snapshot otherwise clobbers
    // on read-modify-write, e.g. el.className += ' x').
    vm.onDomPropGet = [vmPtr = &vm](JsObject* wrapper, const std::string& key, JsValue& out) -> bool {
        if (wrapper->kind != ObjKind::DomWrapper) return false;
        Node* n = static_cast<Node*>(wrapper->domNode);
        if (!n) return false;
        if (key == "className") { out = vmPtr->str(n->attr("class")); return true; }
        if (key == "id")        { out = vmPtr->str(n->attr("id"));    return true; }
        if (key == "value")     { out = vmPtr->str(n->attr("value")); return true; }
        if (key == "checked")   { out = JsValue::boolean(hasAttr(n, "checked")); return true; }
        if (key == "disabled")  { out = JsValue::boolean(hasAttr(n, "disabled")); return true; }
        return false;
    };

    // Release the previous document's wrapper roots before rewrapping.
    for (auto& r : g_wrapperRoots) vm.gc().removeRoot(r.get());
    g_wrapperRoots.clear();
    g_wrapperStore.clear();
    g_eventListeners.clear();
    g_windowEventListeners.clear();
    for (auto& r : g_observerRoots) vm.gc().removeRoot(r.get());
    g_observerRoots.clear();
    g_mutationObservers.clear();

    JsValue docVal = wrapNode(vm, docNode);
    vm.setGlobal("document", docVal);

    // document.body / document.head / document.documentElement
    auto findFirst = [&](const std::string& tag) -> JsValue {
        std::vector<Node*> stack;
        stack.push_back(docNode.get());
        while (!stack.empty()) {
            Node* n = stack.back();
            stack.pop_back();
            if (!n) continue;
            if (n->tagName == tag) {
                auto shared = getShared(n);
                return shared ? wrapNode(vm, shared) : JsValue::null();
            }
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(it->get());
        }
        return JsValue::null();
    };

    if (docVal.isObject()) {
        docVal.asObject()->setProp("body",            findFirst("body"));
        docVal.asObject()->setProp("head",            findFirst("head"));
        docVal.asObject()->setProp("documentElement", findFirst("html"));
        docVal.asObject()->setProp("title",           vm.str(""));
        docVal.asObject()->setProp("URL",             vm.str(""));
        docVal.asObject()->setProp("readyState",      vm.str("complete"));
        docVal.asObject()->setProp("domain",          vm.str(""));
        docVal.asObject()->setProp("cookie",          vm.str(CookieJar::instance().documentCookies(pageUrl)));
        docVal.asObject()->setProp("referrer",        vm.str(""));
        docVal.asObject()->setProp("lastModified",    vm.str(""));
        docVal.asObject()->setProp("characterSet",    vm.str("UTF-8"));
    }

    // window globals — populate location from the page URL.
    auto* winLocation = vm.gc().newObject(ObjKind::Plain);
    auto* winHistory = vm.gc().newObject(ObjKind::Plain);
    struct HistoryStack {
        std::vector<std::string> entries;
        size_t index = 0;
    };
    auto historyStack = std::make_shared<HistoryStack>();
    historyStack->entries.push_back(pageUrl);

    auto syncLocation = [winLocation, &vm](const std::string& href) {
        applyLocationProps(vm, winLocation, href);
    };
    auto syncHistoryLength = [winHistory, historyStack]() {
        winHistory->setProp("length", JsValue::integer((int32_t)historyStack->entries.size()));
    };
    syncLocation(pageUrl);
    syncHistoryLength();
    winHistory->setProp("state", JsValue::null());

    addNative(vm, winLocation, "assign",
        [historyStack, syncLocation, syncHistoryLength, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = resolveDomUrl(args.empty() ? "" : args[0].toString(), current);
            if (historyStack->index + 1 < historyStack->entries.size())
                historyStack->entries.erase(historyStack->entries.begin() + historyStack->index + 1, historyStack->entries.end());
            historyStack->entries.push_back(href);
            historyStack->index = historyStack->entries.size() - 1;
            syncLocation(href);
            syncHistoryLength();
            if (g_domCallbacks.navigate) g_domCallbacks.navigate(href, false);
            return JsValue::undefined();
        });
    addNative(vm, winLocation, "replace",
        [historyStack, syncLocation, syncHistoryLength, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = resolveDomUrl(args.empty() ? "" : args[0].toString(), current);
            if (historyStack->entries.empty()) historyStack->entries.push_back(href);
            else historyStack->entries[historyStack->index] = href;
            syncLocation(href);
            syncHistoryLength();
            if (g_domCallbacks.navigate) g_domCallbacks.navigate(href, true);
            return JsValue::undefined();
        });
    addNative(vm, winLocation, "reload",
        [historyStack, baseUrl = pageUrl](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            std::string href = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            if (g_domCallbacks.navigate) g_domCallbacks.navigate(href, true);
            return JsValue::undefined();
        });
    vm.setGlobal("location", JsValue::object(winLocation));

    addNative(vm, winHistory, "pushState",
        [historyStack, syncLocation, syncHistoryLength, winHistory, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = args.size() >= 3 && !args[2].isNullOrUndefined()
                ? resolveDomUrl(args[2].toString(), current)
                : current;
            if (historyStack->index + 1 < historyStack->entries.size())
                historyStack->entries.erase(historyStack->entries.begin() + historyStack->index + 1, historyStack->entries.end());
            historyStack->entries.push_back(href);
            historyStack->index = historyStack->entries.size() - 1;
            winHistory->setProp("state", args.empty() ? JsValue::null() : args[0]);
            syncLocation(href);
            syncHistoryLength();
            return JsValue::undefined();
        });
    addNative(vm, winHistory, "replaceState",
        [historyStack, syncLocation, syncHistoryLength, winHistory, baseUrl = pageUrl]
        (VM&, JsValue, std::vector<JsValue> args) -> JsValue {
            std::string current = historyStack->entries.empty() ? baseUrl : historyStack->entries[historyStack->index];
            std::string href = args.size() >= 3 && !args[2].isNullOrUndefined()
                ? resolveDomUrl(args[2].toString(), current)
                : current;
            if (historyStack->entries.empty()) historyStack->entries.push_back(href);
            else historyStack->entries[historyStack->index] = href;
            winHistory->setProp("state", args.empty() ? JsValue::null() : args[0]);
            syncLocation(href);
            syncHistoryLength();
            return JsValue::undefined();
        });
    addNative(vm, winHistory, "back",
        [historyStack, syncLocation](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            if (historyStack->index > 0) {
                historyStack->index--;
                syncLocation(historyStack->entries[historyStack->index]);
            }
            return JsValue::undefined();
        });
    addNative(vm, winHistory, "forward",
        [historyStack, syncLocation](VM&, JsValue, std::vector<JsValue>) -> JsValue {
            if (historyStack->index + 1 < historyStack->entries.size()) {
                historyStack->index++;
                syncLocation(historyStack->entries[historyStack->index]);
            }
            return JsValue::undefined();
        });
    vm.setGlobal("history", JsValue::object(winHistory));

    auto* winNavigator = vm.gc().newObject(ObjKind::Plain);
    winNavigator->setProp("userAgent",   vm.str("Helix/1.0"));
    winNavigator->setProp("language",    vm.str("en-US"));
    winNavigator->setProp("onLine",      JsValue::boolean(true));
    winNavigator->setProp("platform",    vm.str("Win32"));
    winNavigator->setProp("cookieEnabled",JsValue::boolean(true));
    vm.setGlobal("navigator", JsValue::object(winNavigator));

    auto* screen = vm.gc().newObject(ObjKind::Plain);
    screen->setProp("width",  JsValue::integer(1280));
    screen->setProp("height", JsValue::integer(800));
    screen->setProp("availWidth",  JsValue::integer(1280));
    screen->setProp("availHeight", JsValue::integer(800));
    screen->setProp("colorDepth",  JsValue::integer(24));
    vm.setGlobal("screen", JsValue::object(screen));

    vm.setGlobal("innerWidth",  JsValue::integer(1280));
    vm.setGlobal("innerHeight", JsValue::integer(800));
    syncWindowScrollGlobals(vm);
    vm.setGlobal("devicePixelRatio", JsValue::number(1.0));

    // getComputedStyle(element): lightweight CSSOM from inline style + defaults.
    vm.setGlobal("getComputedStyle", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("getComputedStyle") {
            Node* n = unwrapNode(ARG(0));
            auto* style = vm.gc().newObject(ObjKind::Plain);
            auto props = computedStyleMap(n);
            for (const auto& [key, val] : props) style->setProp(key, vm.str(val));
            addNative(vm, style, "getPropertyValue", NATIVE("getPropertyValue") {
                if (!thisVal.isObject() || args.empty()) return vm.str("");
                std::string key = lowerCopy(trimCopy(ARG_STR(0)));
                JsValue direct = thisVal.asObject()->getProp(key);
                if (!direct.isUndefined()) return direct;
                JsValue camel = thisVal.asObject()->getProp(cssCamel(key));
                return camel.isUndefined() ? vm.str("") : camel;
            });
            return JsValue::object(style);
        }, "getComputedStyle")));

    // matchMedia(query) — returns { matches: false, media: query }.
    vm.setGlobal("matchMedia", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("matchMedia") {
            std::string query = ARG_STR(0);
            auto* mql = vm.gc().newObject(ObjKind::Plain);
            // Simple checks for common queries.
            bool matches = false;
            if (query.find("(prefers-color-scheme: light)") != std::string::npos) matches = true;
            mql->setProp("matches", JsValue::boolean(matches));
            mql->setProp("media", vm.str(query));
            addNative(vm, mql, "addEventListener", NATIVE("mql_ael") { return JsValue::undefined(); });
            addNative(vm, mql, "removeEventListener", NATIVE("mql_rel") { return JsValue::undefined(); });
            addNative(vm, mql, "addListener", NATIVE("mql_al") { return JsValue::undefined(); });
            return JsValue::object(mql);
        }, "matchMedia")));

    // fetch(url) — makes an HTTP request and returns a Promise-like object.
    vm.setGlobal("fetch", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("fetch") {
            std::string url = ARG_STR(0);
            // Synchronous fetch (simplified — real fetch is async, but our VM
            // doesn't have a true event loop yet).
            auto res = FetchResourceCached(url, 12 * 1024 * 1024, ResourceKind::Other);
            auto* response = vm.gc().newObject(ObjKind::Plain);
            response->setProp("ok", JsValue::boolean(res.success));
            response->setProp("status", JsValue::integer(res.success ? 200 : 0));
            response->setProp("url", vm.str(url));
            std::string body = res.success ? res.body : "";
            // .text() returns a resolved Promise with the body string.
            auto bodyStr = vm.str(body);
            addNative(vm, response, "text", [bodyStr](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
                return v.promiseResolve(bodyStr);
            });
            // .json() parses the body as JSON.
            addNative(vm, response, "json", [body](VM& v, JsValue, std::vector<JsValue>) -> JsValue {
                JsValue jsonVal = v.getGlobal("JSON");
                JsValue parsed = JsValue::undefined();
                if (jsonVal.isObject()) {
                    JsValue parseFn = jsonVal.asObject()->getProp("parse");
                    if (parseFn.isCallable())
                        parsed = v.call(parseFn, jsonVal, { v.str(body) });
                }
                return v.promiseResolve(parsed);
            });
            return vm.promiseResolve(JsValue::object(response));
        }, "fetch")));

    // window.addEventListener / removeEventListener
    vm.setGlobal("addEventListener", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("win_addEventListener") {
            if (args.size() >= 2 && ARG(1).isCallable())
                g_windowEventListeners.push_back({ ARG_STR(0), ARG(1) });
            return JsValue::undefined();
        }, "addEventListener")));
    vm.setGlobal("removeEventListener", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("win_removeEventListener") {
            if (args.size() < 2) return JsValue::undefined();
            std::string eventName = ARG_STR(0);
            JsValue fn = ARG(1);
            auto it = std::find_if(g_windowEventListeners.begin(), g_windowEventListeners.end(),
                [&](const EventListener& listener) {
                    return listener.event == eventName && listener.fn.strictEq(fn);
                });
            if (it != g_windowEventListeners.end()) g_windowEventListeners.erase(it);
            return JsValue::undefined();
        }, "removeEventListener")));

    // window.scrollTo / scroll / open
    vm.setGlobal("scrollTo", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("scrollTo") {
            float x = 0.f, y = 0.f;
            if (!args.empty() && args[0].isObject()) {
                auto* options = args[0].asObject();
                JsValue left = options->getProp("left");
                JsValue top = options->getProp("top");
                x = left.isUndefined() ? g_windowScrollX : (float)left.toNumber();
                y = top.isUndefined() ? g_windowScrollY : (float)top.toNumber();
            } else {
                x = args.size() > 0 ? (float)ARG_NUM(0) : g_windowScrollX;
                y = args.size() > 1 ? (float)ARG_NUM(1) : g_windowScrollY;
            }
            setWindowScrollTo(vm, x, y);
            if (g_domCallbacks.scrollTo) g_domCallbacks.scrollTo(g_windowScrollX, g_windowScrollY);
            return JsValue::undefined();
        }, "scrollTo")));
    vm.setGlobal("scroll", vm.getGlobal("scrollTo"));
    vm.setGlobal("scrollBy", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("scrollBy") {
            float dx = 0.f, dy = 0.f;
            if (!args.empty() && args[0].isObject()) {
                auto* options = args[0].asObject();
                JsValue left = options->getProp("left");
                JsValue top = options->getProp("top");
                dx = left.isUndefined() ? 0.f : (float)left.toNumber();
                dy = top.isUndefined() ? 0.f : (float)top.toNumber();
            } else {
                dx = args.size() > 0 ? (float)ARG_NUM(0) : 0.f;
                dy = args.size() > 1 ? (float)ARG_NUM(1) : 0.f;
            }
            setWindowScrollTo(vm, g_windowScrollX + dx, g_windowScrollY + dy);
            if (g_domCallbacks.scrollBy) g_domCallbacks.scrollBy(dx, dy);
            else if (g_domCallbacks.scrollTo) g_domCallbacks.scrollTo(g_windowScrollX, g_windowScrollY);
            return JsValue::undefined();
        }, "scrollBy")));
    vm.setGlobal("open", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_open") { return JsValue::null(); }, "open")));
    vm.setGlobal("close", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_close") { return JsValue::undefined(); }, "close")));
    vm.setGlobal("focus", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_focus") { return JsValue::undefined(); }, "focus")));
    vm.setGlobal("blur", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_blur") { return JsValue::undefined(); }, "blur")));
    vm.setGlobal("print", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("window_print") { return JsValue::undefined(); }, "print")));
    vm.setGlobal("atob", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("atob") {
            // Minimal base64 decode.
            std::string input = ARG_STR(0), out;
            static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int val = 0, bits = -8;
            for (char c : input) {
                size_t p = b64.find(c);
                if (p == std::string::npos) continue;
                val = (val << 6) | (int)p; bits += 6;
                if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
            }
            return vm.str(out);
        }, "atob")));
    vm.setGlobal("btoa", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("btoa") {
            static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string input = ARG_STR(0), out;
            int val = 0, bits = 0;
            for (unsigned char c : input) {
                val = (val << 8) | c; bits += 8;
                while (bits >= 6) { bits -= 6; out += b64[(val >> bits) & 0x3F]; }
            }
            if (bits > 0) out += b64[(val << (6 - bits)) & 0x3F];
            while (out.size() % 4) out += '=';
            return vm.str(out);
        }, "btoa")));

    // window.alert / confirm / prompt
    vm.setGlobal("alert", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("alert") { fprintf(stderr, "[ALERT] %s\n", ARG_STR(0).c_str()); return JsValue::undefined(); }, "alert")));
    vm.setGlobal("confirm", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("confirm") { return JsValue::boolean(false); }, "confirm")));
    vm.setGlobal("prompt", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("prompt") { return JsValue::null(); }, "prompt")));

    // performance.now()
    auto* perf = vm.gc().newObject(ObjKind::Plain);
    addNative(vm, perf, "now", NATIVE("performance.now") {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return JsValue::number((double)std::chrono::duration_cast<std::chrono::microseconds>(t).count() / 1000.0);
    });
    vm.setGlobal("performance", JsValue::object(perf));

    // requestAnimationFrame (queued as macrotask with 0 delay)
    vm.setGlobal("requestAnimationFrame", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("requestAnimationFrame") {
            if (!ARG(0).isCallable()) return JsValue::integer(0);
            auto t = std::chrono::steady_clock::now().time_since_epoch();
            double timestamp = (double)std::chrono::duration_cast<std::chrono::microseconds>(t).count() / 1000.0;
            return JsValue::integer(vm.scheduleMacrotask(ARG(0), { JsValue::number(timestamp) }, 0, false));
        }, "requestAnimationFrame")));
    vm.setGlobal("cancelAnimationFrame", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("cancelAnimationFrame") {
            vm.cancelMacrotask(ARG_INT(0));
            return JsValue::undefined();
        }, "cancelAnimationFrame")));

    vm.setGlobal("MutationObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("MutationObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            obs->setProp("_callback", ARG(0));
            g_mutationObservers.push_back({ obs, nullptr, false, false, true, true });
            JsValue obsVal = JsValue::object(obs);
            g_observerRoots.push_back(std::make_unique<JsValue>(obsVal));
            vm.gc().addRoot(g_observerRoots.back().get());
            addNative(vm, obs, "observe", NATIVE("obs_observe") {
                if (!thisVal.isObject()) return JsValue::undefined();
                Node* target = unwrapNode(ARG(0));
                for (auto& entry : g_mutationObservers) {
                    if (entry.observer != thisVal.asObject()) continue;
                    entry.target = target;
                    entry.active = target != nullptr;
                    if (ARG(1).isObject()) {
                        auto* options = ARG(1).asObject();
                        JsValue subtree = options->getProp("subtree");
                        JsValue attributes = options->getProp("attributes");
                        JsValue childList = options->getProp("childList");
                        if (!subtree.isUndefined()) entry.subtree = subtree.toBool();
                        if (!attributes.isUndefined()) entry.attributes = attributes.toBool();
                        if (!childList.isUndefined()) entry.childList = childList.toBool();
                    }
                    break;
                }
                return JsValue::undefined();
            });
            addNative(vm, obs, "disconnect", NATIVE("obs_disconnect") {
                if (!thisVal.isObject()) return JsValue::undefined();
                for (auto& entry : g_mutationObservers)
                    if (entry.observer == thisVal.asObject()) entry.active = false;
                return JsValue::undefined();
            });
            return obsVal;
        }, "MutationObserver")));

    vm.setGlobal("IntersectionObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("IntersectionObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            obs->setProp("_callback", ARG(0));
            addNative(vm, obs, "observe", NATIVE("io_observe") {
                if (!thisVal.isObject()) return JsValue::undefined();
                JsValue callback = thisVal.asObject()->getProp("_callback");
                if (!callback.isCallable()) return JsValue::undefined();
                Node* target = unwrapNode(ARG(0));
                DomMetrics rect = computeDomMetrics(target);
                auto* bounds = vm.gc().newObject(ObjKind::Plain);
                bounds->setProp("top", JsValue::number(rect.top));
                bounds->setProp("left", JsValue::number(rect.left));
                bounds->setProp("bottom", JsValue::number(rect.top + rect.height));
                bounds->setProp("right", JsValue::number(rect.left + rect.width));
                bounds->setProp("width", JsValue::number(rect.width));
                bounds->setProp("height", JsValue::number(rect.height));
                auto* record = vm.gc().newObject(ObjKind::Plain);
                record->setProp("target", ARG(0));
                record->setProp("isIntersecting", JsValue::boolean(true));
                record->setProp("intersectionRatio", JsValue::number(1.0));
                record->setProp("boundingClientRect", JsValue::object(bounds));
                auto* records = vm.gc().newArray();
                records->arrayPush(JsValue::object(record));
                try { vm.call(callback, JsValue::undefined(), { JsValue::object(records), thisVal }); } catch (...) {}
                return JsValue::undefined();
            });
            addNative(vm, obs, "unobserve",  NATIVE("io_unobserve")  { return JsValue::undefined(); });
            addNative(vm, obs, "disconnect", NATIVE("io_disconnect") { return JsValue::undefined(); });
            return JsValue::object(obs);
        }, "IntersectionObserver")));

    vm.setGlobal("ResizeObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("ResizeObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            obs->setProp("_callback", ARG(0));
            addNative(vm, obs, "observe", NATIVE("ro_observe") {
                if (!thisVal.isObject()) return JsValue::undefined();
                JsValue callback = thisVal.asObject()->getProp("_callback");
                if (!callback.isCallable()) return JsValue::undefined();
                Node* target = unwrapNode(ARG(0));
                DomMetrics rect = computeDomMetrics(target);
                auto* size = vm.gc().newObject(ObjKind::Plain);
                size->setProp("inlineSize", JsValue::number(rect.width));
                size->setProp("blockSize", JsValue::number(rect.height));
                auto* record = vm.gc().newObject(ObjKind::Plain);
                record->setProp("target", ARG(0));
                record->setProp("contentRect", JsValue::object(size));
                auto* records = vm.gc().newArray();
                records->arrayPush(JsValue::object(record));
                try { vm.call(callback, JsValue::undefined(), { JsValue::object(records), thisVal }); } catch (...) {}
                return JsValue::undefined();
            });
            addNative(vm, obs, "unobserve",  NATIVE("ro_unobserve")  { return JsValue::undefined(); });
            addNative(vm, obs, "disconnect", NATIVE("ro_disconnect") { return JsValue::undefined(); });
            return JsValue::object(obs);
        }, "ResizeObserver")));

    // CustomEvent / Event
    auto makeEventCtor = [&](const char* name) {
        vm.setGlobal(name, JsValue::object(vm.gc().newNativeFunction(NATIVE("Event") {
            auto* ev = vm.gc().newObject(ObjKind::Plain);
            ev->setProp("type",    ARG(0));
            ev->setProp("bubbles", JsValue::boolean(false));
            ev->setProp("cancelable", JsValue::boolean(false));
            ev->setProp("target",  JsValue::null());
            ev->setProp("currentTarget", JsValue::null());
            ev->setProp("defaultPrevented", JsValue::boolean(false));
            addNative(vm, ev, "preventDefault",  NATIVE("preventDefault")  { return JsValue::undefined(); });
            addNative(vm, ev, "stopPropagation", NATIVE("stopPropagation") { return JsValue::undefined(); });
            return JsValue::object(ev);
        }, name)));
    };
    makeEventCtor("Event");
    makeEventCtor("CustomEvent");
    makeEventCtor("MouseEvent");
    makeEventCtor("KeyboardEvent");
    makeEventCtor("InputEvent");
    makeEventCtor("FocusEvent");
    makeEventCtor("PointerEvent");
    makeEventCtor("WheelEvent");
    makeEventCtor("TouchEvent");
    makeEventCtor("DragEvent");
    makeEventCtor("SubmitEvent");
}

void dispatchDomEvent(VM& vm, Node* target, const std::string& eventName) {
    if (!target) return;
    // Build a minimal event object.
    auto* ev = vm.gc().newObject(ObjKind::Plain);
    ev->setProp("type", vm.str(eventName));
    ev->setProp("target", wrapNode(vm, getShared(target) ? getShared(target)
                                       : std::shared_ptr<Node>(target, [](Node*){})));
    ev->setProp("preventDefault", JsValue::object(vm.gc().newNativeFunction(
        [](VM&, JsValue, std::vector<JsValue>) { return JsValue::undefined(); }, "preventDefault")));
    ev->setProp("stopPropagation", JsValue::object(vm.gc().newNativeFunction(
        [](VM&, JsValue, std::vector<JsValue>) { return JsValue::undefined(); }, "stopPropagation")));
    JsValue evVal = JsValue::object(ev);

    // Walk from target up through ancestors (bubble phase).
    Node* cur = target;
    while (cur) {
        auto it = g_eventListeners.find(cur);
        if (it != g_eventListeners.end()) {
            for (auto& listener : it->second) {
                if (listener.event == eventName) {
                    try { vm.call(listener.fn, JsValue::undefined(), { evVal }); }
                    catch (...) {}
                }
            }
        }
        cur = cur->parent;
    }
}

void dispatchWindowEvent(VM& vm, const std::string& eventName, JsValue eventValue) {
    JsValue evVal = eventValue;
    if (!evVal.isObject()) {
        auto* ev = vm.gc().newObject(ObjKind::Plain);
        ev->setProp("type", vm.str(eventName));
        ev->setProp("target", JsValue::object(vm.globals()));
        ev->setProp("defaultPrevented", JsValue::boolean(false));
        ev->setProp("preventDefault", JsValue::object(vm.gc().newNativeFunction(
            [](VM&, JsValue, std::vector<JsValue>) { return JsValue::undefined(); }, "preventDefault")));
        ev->setProp("stopPropagation", JsValue::object(vm.gc().newNativeFunction(
            [](VM&, JsValue, std::vector<JsValue>) { return JsValue::undefined(); }, "stopPropagation")));
        evVal = JsValue::object(ev);
    }

    JsValue propertyHandler = vm.getGlobal("on" + eventName);
    if (propertyHandler.isCallable()) {
        try { vm.call(propertyHandler, JsValue::object(vm.globals()), { evVal }); } catch (...) {}
    }
    auto listeners = g_windowEventListeners;
    for (const auto& listener : listeners) {
        if (listener.event != eventName || !listener.fn.isCallable()) continue;
        try { vm.call(listener.fn, JsValue::object(vm.globals()), { evVal }); } catch (...) {}
    }
    vm.drainMicrotasks();
}

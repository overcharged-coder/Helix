#include "js/dom_bridge.h"
#include <algorithm>
#include <sstream>
#include <chrono>
#include <windows.h>

#define NATIVE(name) [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue
#define ARG(i) (args.size() > (size_t)(i) ? args[i] : JsValue::undefined())
#define ARG_STR(i) ARG(i).toString()
#define ARG_NUM(i) ARG(i).toNumber()
#define ARG_INT(i) ARG(i).toInt32()

static void addNative(VM& vm, JsObject* obj, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(std::move(fn), name);
    obj->setProp(name, JsValue::object(fnObj));
}

// ── Node store ────────────────────────────────────────────────────────────────
// We keep shared_ptr alive by storing them in a global registry keyed by raw ptr.
// This is safe because the VM and document share the same lifetime.

static std::unordered_map<Node*, std::shared_ptr<Node>> g_nodeStore;
static std::unordered_map<Node*, JsObject*> g_wrapperStore;
// Persistent GC roots for cached DOM wrappers. These live at STABLE heap
// addresses so the GC can mark them safely (rooting a stack local was a
// use-after-return bug). Cleared per document in registerDom().
static std::vector<std::unique_ptr<JsValue>> g_wrapperRoots;

static std::shared_ptr<Node> getShared(Node* raw) {
    auto it = g_nodeStore.find(raw);
    if (it != g_nodeStore.end()) return it->second;
    return nullptr;
}

Node* unwrapNode(JsValue val) {
    if (!val.isObject()) return nullptr;
    return static_cast<Node*>(val.asObject()->domNode);
}

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

// Forward declared free function for querySelector
static std::vector<std::shared_ptr<Node>> domQueryAll(Node* root, const std::string& sel) {
    std::vector<std::shared_ptr<Node>> result;
    size_t visited = 0;
    int depth = 0;
    std::function<void(Node*)> walk = [&](Node* n) {
        if (++depth > 1000) { --depth; return; }   // guard against deep/cyclic trees
        for (auto& c : n->children) {
            if (++visited > 200000) break;          // guard against runaway traversal
            bool match = false;
            if (!sel.empty() && sel[0]=='#') match = (c->attr("id") == sel.substr(1));
            else if (!sel.empty() && sel[0]=='.') match = (c->attr("class").find(sel.substr(1)) != std::string::npos);
            else match = (c->tagName == sel);
            if (match) result.push_back(c);
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
        vm.gc().newNativeFunction([](VM& vm2, JsValue t, std::vector<JsValue> a) -> JsValue {
            if (!t.isObject()) return JsValue::undefined();
            std::string prop = a.size()>0?a[0].toString():"";
            std::string val  = a.size()>1?a[1].toString():"";
            t.asObject()->setProp(prop, vm2.str(val));
            return JsValue::undefined();
        }, "setProperty");
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
        vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty();
        return JsValue::undefined();
    });
    addNativeM("removeAttribute", NATIVE("removeAttribute") {
        Node* n = unwrapNode(thisVal);
        if (n && !args.empty()) n->attrs.erase(args[0].toString());
        vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty();
        return JsValue::undefined();
    });
    addNativeM("hasAttribute", NATIVE("hasAttribute") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        return JsValue::boolean(n->attrs.count(args[0].toString()) > 0);
    });

    // classList
    {
        auto* cl = vm.gc().newObject(ObjKind::Plain);
        addNative(vm, cl, "add", NATIVE("classList_add") {
            Node* n = unwrapNode(thisVal.isObject() ? thisVal : JsValue::undefined());
            // thisVal is classList, parent is the element — we use domNode stored on parent
            // classList is on the style obj, we need the node from owner
            return JsValue::undefined(); // simplified
        });
        addNative(vm, cl, "remove", NATIVE("classList_remove") { return JsValue::undefined(); });
        addNative(vm, cl, "toggle", NATIVE("classList_toggle") { return JsValue::boolean(false); });
        addNative(vm, cl, "contains", NATIVE("classList_contains") { return JsValue::boolean(false); });
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

    // DOM mutations
    addNativeM("appendChild", NATIVE("appendChild") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return ARG(0);
        Node* child = unwrapNode(ARG(0));
        if (!child) return ARG(0);
        auto childShared = getShared(child);
        if (childShared) { n->appendChild(childShared); vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty(); }
        return ARG(0);
    });
    addNativeM("removeChild", NATIVE("removeChild") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return ARG(0);
        Node* child = unwrapNode(ARG(0));
        if (!child) return ARG(0);
        auto& ch = n->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==child; }), ch.end());
        vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty();
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
        vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty();
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
        vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty();
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

    // Event listeners (simplified: store them, no real dispatch)
    {
        auto* listeners = vm.gc().newObject(ObjKind::Plain);
        obj->setProp("__listeners__", JsValue::object(listeners));
    }
    addNativeM("addEventListener", NATIVE("addEventListener") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.size() < 2) return JsValue::undefined();
        std::string ev = ARG_STR(0);
        // Store listener on the DOM node attrs (simplified)
        n->attrs["__on" + ev + "__"] = "1";
        return JsValue::undefined();
    });
    addNativeM("removeEventListener", NATIVE("removeEventListener") {
        return JsValue::undefined();
    });
    addNativeM("dispatchEvent", NATIVE("dispatchEvent") {
        return JsValue::boolean(true);
    });
    addNativeM("matches", NATIVE("matches") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::boolean(false);
        std::string sel = ARG_STR(0);
        bool match = false;
        if (!sel.empty() && sel[0]=='#') match = (n->attr("id") == sel.substr(1));
        else if (!sel.empty() && sel[0]=='.') match = (n->attr("class").find(sel.substr(1)) != std::string::npos);
        else match = (n->tagName == sel);
        return JsValue::boolean(match);
    });
    addNativeM("closest", NATIVE("closest") {
        Node* n = unwrapNode(thisVal);
        if (!n || args.empty()) return JsValue::null();
        std::string sel = ARG_STR(0);
        Node* cur = n;
        while (cur) {
            bool match = false;
            if (!sel.empty() && sel[0]=='#') match = (cur->attr("id") == sel.substr(1));
            else if (!sel.empty() && sel[0]=='.') match = (cur->attr("class").find(sel.substr(1)) != std::string::npos);
            else match = (cur->tagName == sel);
            if (match) { auto s = getShared(cur); if (s) return wrapNode(vm, s); }
            cur = cur->parent;
        }
        return JsValue::null();
    });

    // Geometry stubs
    addNativeM("getBoundingClientRect", NATIVE("getBoundingClientRect") {
        auto* r = vm.gc().newObject(ObjKind::Plain);
        r->setProp("top",    JsValue::integer(0));
        r->setProp("left",   JsValue::integer(0));
        r->setProp("bottom", JsValue::integer(0));
        r->setProp("right",  JsValue::integer(0));
        r->setProp("width",  JsValue::integer(0));
        r->setProp("height", JsValue::integer(0));
        r->setProp("x",      JsValue::integer(0));
        r->setProp("y",      JsValue::integer(0));
        return JsValue::object(r);
    });
    addNativeM("scrollIntoView", NATIVE("scrollIntoView") { return JsValue::undefined(); });
    addNativeM("focus",  NATIVE("focus")  { return JsValue::undefined(); });
    addNativeM("blur",   NATIVE("blur")   { return JsValue::undefined(); });
    addNativeM("click",  NATIVE("click")  { return JsValue::undefined(); });
    addNativeM("remove", NATIVE("remove") {
        Node* n = unwrapNode(thisVal);
        if (!n || !n->parent) return JsValue::undefined();
        auto& ch = n->parent->children;
        ch.erase(std::remove_if(ch.begin(), ch.end(), [&](auto& c){ return c.get()==n; }), ch.end());
        vm.domDirty = true; if (vm.onDomDirty) vm.onDomDirty();
        return JsValue::undefined();
    });

    // value / checked for form elements
    obj->setProp("value",   vm.str(node->attr("value")));
    obj->setProp("checked", JsValue::boolean(node->attr("checked") == "checked"));
    obj->setProp("disabled",JsValue::boolean(node->attr("disabled") == "disabled"));
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

void registerDom(VM& vm, std::shared_ptr<Node> docNode,
                 std::function<void()> onRepaint) {
    vm.onDomDirty = onRepaint;
    // Release the previous document's wrapper roots before rewrapping.
    for (auto& r : g_wrapperRoots) vm.gc().removeRoot(r.get());
    g_wrapperRoots.clear();
    g_wrapperStore.clear();

    JsValue docVal = wrapNode(vm, docNode);
    vm.setGlobal("document", docVal);

    // document.body / document.head / document.documentElement
    auto findFirst = [&](const std::string& tag) -> JsValue {
        std::function<JsValue(Node*)> find = [&](Node* n) -> JsValue {
            for (auto& c : n->children) {
                if (c->tagName == tag) return wrapNode(vm, c);
                JsValue r = find(c.get());
                if (!r.isNull()) return r;
            }
            return JsValue::null();
        };
        return find(docNode.get());
    };

    if (docVal.isObject()) {
        docVal.asObject()->setProp("body",            findFirst("body"));
        docVal.asObject()->setProp("head",            findFirst("head"));
        docVal.asObject()->setProp("documentElement", findFirst("html"));
        docVal.asObject()->setProp("title",           vm.str(""));
        docVal.asObject()->setProp("URL",             vm.str(""));
        docVal.asObject()->setProp("readyState",      vm.str("complete"));
        docVal.asObject()->setProp("domain",          vm.str(""));
        docVal.asObject()->setProp("cookie",          vm.str(""));
        docVal.asObject()->setProp("referrer",        vm.str(""));
        docVal.asObject()->setProp("lastModified",    vm.str(""));
        docVal.asObject()->setProp("characterSet",    vm.str("UTF-8"));
    }

    // window globals
    auto* winLocation = vm.gc().newObject(ObjKind::Plain);
    winLocation->setProp("href",     vm.str(""));
    winLocation->setProp("protocol", vm.str(""));
    winLocation->setProp("host",     vm.str(""));
    winLocation->setProp("pathname", vm.str("/"));
    winLocation->setProp("search",   vm.str(""));
    winLocation->setProp("hash",     vm.str(""));
    winLocation->setProp("origin",   vm.str(""));
    vm.gc().newNativeFunction([](VM& v, JsValue, std::vector<JsValue> a) -> JsValue {
        return JsValue::undefined();
    }, "assign");
    vm.setGlobal("location", JsValue::object(winLocation));

    auto* winHistory = vm.gc().newObject(ObjKind::Plain);
    winHistory->setProp("length", JsValue::integer(1));
    addNative(vm, winHistory, "pushState",    NATIVE("pushState")    { return JsValue::undefined(); });
    addNative(vm, winHistory, "replaceState", NATIVE("replaceState") { return JsValue::undefined(); });
    addNative(vm, winHistory, "back",         NATIVE("back")         { return JsValue::undefined(); });
    addNative(vm, winHistory, "forward",      NATIVE("forward")      { return JsValue::undefined(); });
    vm.setGlobal("history", JsValue::object(winHistory));

    auto* winNavigator = vm.gc().newObject(ObjKind::Plain);
    winNavigator->setProp("userAgent",   vm.str("Helix/1.0"));
    winNavigator->setProp("language",    vm.str("en-US"));
    winNavigator->setProp("onLine",      JsValue::boolean(true));
    winNavigator->setProp("platform",    vm.str("Win32"));
    winNavigator->setProp("cookieEnabled",JsValue::boolean(false));
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
    vm.setGlobal("pageXOffset", JsValue::integer(0));
    vm.setGlobal("pageYOffset", JsValue::integer(0));
    vm.setGlobal("scrollX",     JsValue::integer(0));
    vm.setGlobal("scrollY",     JsValue::integer(0));
    vm.setGlobal("devicePixelRatio", JsValue::number(1.0));

    // window.addEventListener / removeEventListener (no-ops for now)
    vm.setGlobal("addEventListener", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("win_addEventListener") { return JsValue::undefined(); }, "addEventListener")));
    vm.setGlobal("removeEventListener", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("win_removeEventListener") { return JsValue::undefined(); }, "removeEventListener")));

    // window.alert / confirm / prompt
    vm.setGlobal("alert", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("alert") { OutputDebugStringA(("[ALERT] " + ARG_STR(0) + "\n").c_str()); return JsValue::undefined(); }, "alert")));
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
            if (ARG(0).isCallable()) {
                VM::Macrotask task;
                task.fn    = ARG(0);
                task.delay = 0;
                vm.macrotasks().push_back(task);
            }
            return JsValue::integer((int32_t)vm.macrotasks().size());
        }, "requestAnimationFrame")));
    vm.setGlobal("cancelAnimationFrame", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("cancelAnimationFrame") { return JsValue::undefined(); }, "cancelAnimationFrame")));

    // MutationObserver (stub)
    vm.setGlobal("MutationObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("MutationObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            addNative(vm, obs, "observe",    NATIVE("obs_observe")    { return JsValue::undefined(); });
            addNative(vm, obs, "disconnect", NATIVE("obs_disconnect") { return JsValue::undefined(); });
            return JsValue::object(obs);
        }, "MutationObserver")));

    // IntersectionObserver (stub)
    vm.setGlobal("IntersectionObserver", JsValue::object(vm.gc().newNativeFunction(
        NATIVE("IntersectionObserver") {
            auto* obs = vm.gc().newObject(ObjKind::Plain);
            addNative(vm, obs, "observe",    NATIVE("io_observe")    { return JsValue::undefined(); });
            addNative(vm, obs, "unobserve",  NATIVE("io_unobserve")  { return JsValue::undefined(); });
            addNative(vm, obs, "disconnect", NATIVE("io_disconnect") { return JsValue::undefined(); });
            return JsValue::object(obs);
        }, "IntersectionObserver")));

    // ResizeObserver (stub)
    vm.setGlobal("ResizeObserver", vm.getGlobal("MutationObserver"));

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

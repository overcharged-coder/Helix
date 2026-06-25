#include "js/engine.h"
#include "js/gc.h"
#include "js/vm.h"
#include "js/runtime.h"
#include "js/dom_bridge.h"
#include "js/lexer.h"
#include "js/parser.h"
#include "js/compiler.h"
#include <cstdio>

#define NATIVE(name) [](VM& vm, JsValue thisVal, std::vector<JsValue> args) -> JsValue
#define ARG(i) (args.size() > (size_t)(i) ? args[i] : JsValue::undefined())
#define ARG_STR(i) ARG(i).toString()

static void addNative(VM& vm, JsObject* obj, const std::string& name, NativeFn fn) {
    auto* fnObj = vm.gc().newNativeFunction(std::move(fn), name);
    obj->setProp(name, JsValue::object(fnObj));
}

struct JsEngine::Impl {
    GC  gc;
    VM  vm;

    explicit Impl() : gc(), vm(gc) {
        registerBuiltins(vm);
    }
};

JsEngine::JsEngine() : m_impl(std::make_unique<Impl>()) {}
JsEngine::~JsEngine() = default;

void JsEngine::setDocument(std::shared_ptr<Node> doc, std::function<void()> onRepaint,
                           const std::string& pageUrl) {
    m_impl = std::make_unique<Impl>();
    registerDom(m_impl->vm, std::move(doc), std::move(onRepaint), pageUrl);
}

bool JsEngine::runScript(const std::string& source, const std::string& filename) {
    // Skip very large scripts — our recursive parser may stack-overflow on
    // huge minified bundles. 256KB covers most real site scripts.
    constexpr size_t kMaxScriptBytes = 256 * 1024;
    if (source.size() > kMaxScriptBytes) {
        fprintf(stderr, "%s",("[JS] Skipping large script (" + std::to_string(source.size()/1024) + "KB) in " + filename + "\n").c_str());
        return false;
    }
    try {
        Lexer lex(source);
        auto tokens = lex.tokenize();
        Parser parser(tokens);
        Program prog = parser.parse();
        auto bytecode = Compiler::compile(prog);
        m_impl->vm.execute(bytecode.get());
        m_impl->vm.drainMicrotasks();
        return true;
    } catch (ParseError& e) {
        std::string msg = "[JS Parse Error] " + std::string(e.what()) + "\n";
        fprintf(stderr, "%s",msg.c_str());
        return false;
    } catch (JsException& e) {
        std::string msg = "[JS Error] " + e.val.toString() + "\n";
        fprintf(stderr, "%s",msg.c_str());
        return false;
    } catch (std::exception& e) {
        std::string msg = "[JS Internal Error] " + std::string(e.what()) + "\n";
        fprintf(stderr, "%s",msg.c_str());
        return false;
    } catch (...) {
        fprintf(stderr, "%s",("[JS Unknown Error] in " + filename + "\n").c_str());
        return false;
    }
}

void JsEngine::dispatchClick(Node* target, int x, int y) {
    auto& vm = m_impl->vm;

    // Build a MouseEvent object
    auto* ev = vm.gc().newObject(ObjKind::Plain);
    ev->setProp("type",    vm.str("click"));
    ev->setProp("clientX", JsValue::integer(x));
    ev->setProp("clientY", JsValue::integer(y));
    ev->setProp("pageX",   JsValue::integer(x));
    ev->setProp("pageY",   JsValue::integer(y));
    ev->setProp("bubbles", JsValue::boolean(true));
    ev->setProp("cancelable", JsValue::boolean(true));
    ev->setProp("defaultPrevented", JsValue::boolean(false));
    addNative(vm, ev, "preventDefault",  NATIVE("preventDefault")  { return JsValue::undefined(); });
    addNative(vm, ev, "stopPropagation", NATIVE("stopPropagation") { return JsValue::undefined(); });
    JsValue evVal = JsValue::object(ev);

    // Walk up from target invoking onclick handlers
    Node* cur = target;
    while (cur) {
        // Check onclick attribute
        std::string handler = cur->attr("onclick");
        if (!handler.empty()) {
            runScript(handler, "onclick");
        }
        // Check any registered event listeners stored in attrs
        if (cur->attrs.count("__onclick__")) {
            JsValue listeners = vm.getGlobal("__eventListeners__");
            // Simplified — just run onclick attr
        }
        cur = cur->parent;
    }

    vm.drainMicrotasks();
}

void JsEngine::dispatchKeyDown(int keyCode, const std::string& key) {
    auto& vm = m_impl->vm;
    auto* ev = vm.gc().newObject(ObjKind::Plain);
    ev->setProp("type",    vm.str("keydown"));
    ev->setProp("key",     vm.str(key));
    ev->setProp("keyCode", JsValue::integer(keyCode));
    ev->setProp("which",   JsValue::integer(keyCode));
    addNative(vm, ev, "preventDefault",  NATIVE("preventDefault")  { return JsValue::undefined(); });
    addNative(vm, ev, "stopPropagation", NATIVE("stopPropagation") { return JsValue::undefined(); });
    // Global onkeydown
    JsValue onKD = vm.getGlobal("onkeydown");
    if (onKD.isCallable()) {
        try { vm.call(onKD, JsValue::object(vm.globals()), {JsValue::object(ev)}); } catch (...) {}
    }
    vm.drainMicrotasks();
}

void JsEngine::runMacrotasks() {
    auto& vm = m_impl->vm;
    auto tasks = std::move(vm.macrotasks());
    vm.macrotasks().clear();
    for (auto& task : tasks) {
        try {
            vm.call(task.fn, JsValue::undefined(), task.args);
            vm.drainMicrotasks();
        } catch (...) {}
    }
}

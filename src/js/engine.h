#pragma once
#include "html/dom.h"
#include <memory>
#include <functional>
#include <string>

// Top-level JS engine facade.
// Owns the GC, VM, and built-in registrations.
class JsEngine {
public:
    JsEngine();
    ~JsEngine();

    // Attach a DOM document so JS can access document/window.
    void setDocument(std::shared_ptr<Node> doc, std::function<void()> onRepaint,
                     const std::string& pageUrl = "");

    // Execute a JS source string (parses, compiles, runs).
    // Returns false and logs on parse/runtime error.
    bool runScript(const std::string& source, const std::string& filename = "<script>");

    // Dispatch a click event to a DOM node.
    void dispatchClick(Node* target, int x, int y);

    // Dispatch a keydown event.
    void dispatchKeyDown(int keyCode, const std::string& key);

    // Run pending macrotasks (called by Win32 WM_TIMER).
    void runMacrotasks();
    bool hasPendingMacrotasks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

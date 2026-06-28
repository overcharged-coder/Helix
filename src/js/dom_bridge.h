#pragma once
#include "js/vm.h"
#include "html/dom.h"
#include <memory>
#include <functional>

// Wraps a Node* into a JsObject (Element/Text/Document).
JsValue wrapNode(VM& vm, std::shared_ptr<Node> node);

// Bootstrap the document object and window globals.
// pageUrl is the current page's URL (populates window.location).
void registerDom(VM& vm, std::shared_ptr<Node> document,
                 std::function<void()> onRepaint,
                 const std::string& pageUrl = "");

// Retrieve the Node* from a DOM wrapper JsObject (nullptr if not a DOM object).
Node* unwrapNode(JsValue val);

// Dispatch an event (e.g. "click") on a DOM node, calling registered listeners.
void dispatchDomEvent(VM& vm, Node* target, const std::string& eventName);

// Reset DOM dirty coalescing — call from platform timer tick.
void resetDomDirtyCoalesce();

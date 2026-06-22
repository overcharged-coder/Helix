#pragma once
//
// browser_core.h — platform-independent browser logic.
//
// Contains: Tab management, navigation, history, URL encoding, home page HTML,
// image fetch throttling. Used by all platform shells (Win32, macOS, Linux).
//
#include "network/fetcher.h"
#include "network/url.h"
#include "network/text_decode.h"
#include "html/parser.h"
#include "html/resources.h"
#include "js/engine.h"
#include "layout/box.h"

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <cctype>

// ── semaphore (C++17 compat) ─────────────────────────────────────────────────

class Semaphore {
public:
    explicit Semaphore(int count) : m_count(count) {}
    void acquire() {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait(lk, [&] { return m_count > 0; });
        --m_count;
    }
    void release() {
        { std::lock_guard<std::mutex> lk(m_mu); ++m_count; }
        m_cv.notify_one();
    }
private:
    std::mutex m_mu;
    std::condition_variable m_cv;
    int m_count;
};

// ── data types ───────────────────────────────────────────────────────────────

struct Page {
    std::string           url;
    std::shared_ptr<Node> dom;
    std::string           error;
};

struct ImageMsg {
    std::string          url;
    std::vector<uint8_t> bytes;
};

struct PageMsg {
    int   tabIdx;
    Page* page;
};

struct Tab {
    std::string           url        = "helix://home";
    std::string           displayUrl;
    std::string           title      = "Helix";
    std::shared_ptr<Page> page;
    float                 scrollY    = 0.f;
    float                 docHeight  = 600.f;
    bool                  loading    = false;
    std::string           pendingFragment;
    bool                  fragmentScrollPending = false;
    std::vector<std::string> history;
    int                   histIdx    = -1;
};

// ── URL helpers ──────────────────────────────────────────────────────────────

inline std::string UrlEncodeQuery(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else {
            char hex[4]; snprintf(hex, sizeof hex, "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

inline bool LooksLikeUrl(const std::string& s) {
    if (s.find("://") != std::string::npos) return true;
    if (s.size() > 1 && s[0] == '/' && s[1] == '/') return true;
    // Has a dot before any space → probably a domain.
    size_t dot = s.find('.');
    size_t sp  = s.find(' ');
    return dot != std::string::npos && (sp == std::string::npos || dot < sp);
}

inline std::string UrlFragment(const std::string& url) {
    size_t hash = url.find('#');
    if (hash == std::string::npos || hash + 1 >= url.size()) return {};
    return url.substr(hash + 1);
}

inline std::string UrlWithoutFragment(const std::string& url) {
    size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

inline void TabPushHistory(Tab& tab, const std::string& url) {
    if (tab.histIdx >= 0 && tab.histIdx < (int)tab.history.size())
        tab.history.erase(tab.history.begin() + tab.histIdx + 1, tab.history.end());
    tab.history.push_back(url);
    tab.histIdx = (int)tab.history.size() - 1;
}

// ── home page ────────────────────────────────────────────────────────────────

inline const std::string& HomePageHtml() {
    static const std::string html = R"html(<!DOCTYPE html>
<html>
<head><title>Helix</title>
<style>
* { margin: 0; padding: 0; }
body {
    font-family: -apple-system, 'Segoe UI', system-ui, sans-serif;
    background: #0a0a0a;
    color: #e0e0e0;
    padding: 60px 40px;
    line-height: 1.6;
}
.hero { text-align: center; padding: 40px 0 50px; }
.hero h1 { font-size: 42px; color: #ffffff; margin-bottom: 8px; }
.hero .accent { color: #6c63ff; }
.hero p { font-size: 16px; color: #888; margin-top: 6px; }
.links { max-width: 600px; margin: 0 auto; padding: 30px 0; }
.links a {
    display: block; background: #161616; border: 1px solid #2a2a2a;
    border-radius: 10px; padding: 14px 20px; margin: 8px 0;
    text-decoration: none; color: #d0d0d0; font-size: 15px;
}
.links .url { color: #666; font-size: 12px; }
.section { max-width: 600px; margin: 30px auto 0; padding: 20px 0; border-top: 1px solid #1e1e1e; }
.section h3 { font-size: 14px; color: #6c63ff; margin-bottom: 14px; }
.key { display: block; padding: 4px 0; color: #999; font-size: 13px; }
.key strong { color: #ccc; }
.footer { max-width: 600px; margin: 40px auto 0; padding-top: 20px; border-top: 1px solid #1e1e1e; text-align: center; }
.footer p { font-size: 12px; color: #444; }
.tag { display: inline-block; background: #1a1a2e; color: #6c63ff; border-radius: 4px; padding: 2px 8px; font-size: 11px; margin-left: 6px; }
</style>
</head>
<body>
<div class="hero">
    <h1><span class="accent">&lt;</span>Helix<span class="accent">/&gt;</span></h1>
    <p>A web browser, built from scratch in C++</p>
    <p style="font-size:13px;color:#555;margin-top:4px;">No Chromium. No WebView. No shortcuts.</p>
</div>
<div class="links">
    <a href="https://en.wikipedia.org/wiki/Main_Page">Wikipedia <span class="url">en.wikipedia.org</span></a>
    <a href="https://news.ycombinator.com">Hacker News <span class="url">news.ycombinator.com</span></a>
    <a href="https://lite.cnn.com">CNN Lite <span class="url">lite.cnn.com</span></a>
    <a href="helix://history">History <span class="url">helix://history</span></a>
</div>
<div class="section">
    <h3>Shortcuts</h3>
    <span class="key"><strong>Ctrl+L</strong> &mdash; address bar</span>
    <span class="key"><strong>Ctrl+T / W</strong> &mdash; new / close tab</span>
    <span class="key"><strong>Ctrl+R</strong> &mdash; reload</span>
    <span class="key"><strong>Ctrl+F</strong> &mdash; find in page</span>
    <span class="key"><strong>Ctrl++/-</strong> &mdash; zoom</span>
    <span class="key"><strong>Alt+Left/Right</strong> &mdash; back / forward</span>
</div>
<div class="footer">
    <p>Helix v1.0 <span class="tag">cross-platform</span></p>
    <p style="margin-top:6px;">HTML &bull; CSS &bull; JS &bull; Layout &bull; Rendering &mdash; all from scratch</p>
</div>
</body>
</html>)html";
    return html;
}

// ── shared helpers for page loading ──────────────────────────────────────────

inline std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

inline bool AttrContainsToken(const std::string& value, const std::string& token) {
    std::string lower = LowerAscii(value);
    size_t start = 0;
    while (start < lower.size()) {
        while (start < lower.size() && std::isspace((unsigned char)lower[start])) ++start;
        size_t end = start;
        while (end < lower.size() && !std::isspace((unsigned char)lower[end])) ++end;
        if (lower.substr(start, end - start) == token) return true;
        start = end;
    }
    return false;
}

inline bool StylesheetMediaApplies(const std::string& media) {
    std::string lower = LowerAscii(media);
    if (lower.empty()) return true;
    return lower.find("all") != std::string::npos
        || lower.find("screen") != std::string::npos
        || lower.find("projection") != std::string::npos;
}

inline Node* FindFirstElement(Node* root, const std::string& tag) {
    if (!root) return nullptr;
    std::vector<Node*> stack{ root };
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->type == NodeType::Element && n->tagName == tag) return n;
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
    return nullptr;
}

inline void LoadExternalStylesheets(const std::shared_ptr<Node>& dom, const std::string& pageUrl) {
    if (!dom) return;
    Node* attach = FindFirstElement(dom.get(), "head");
    if (!attach) attach = dom.get();
    std::vector<Node*> stack{ dom.get() };
    int loaded = 0;
    size_t loadedBytes = 0;
    while (!stack.empty() && loaded < 8 && loadedBytes < 512 * 1024) {
        Node* n = stack.back();
        stack.pop_back();
        if (n->type == NodeType::Element && n->tagName == "link"
            && AttrContainsToken(n->attr("rel"), "stylesheet")
            && StylesheetMediaApplies(n->attr("media"))) {
            std::string href = ResolveUrlAgainstBase(n->attr("href"), pageUrl);
            auto res = FetchUrl(href);
            if (res.success && !res.body.empty()) {
                loadedBytes += res.body.size();
                if (loadedBytes <= 512 * 1024) {
                    auto style = Node::makeElement("style");
                    style->appendChild(Node::makeText(DecodeTextToUtf8(res.body, res.contentType)));
                    attach->appendChild(style);
                    ++loaded;
                }
            }
        }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
}

#pragma once
//
// browser_core.h — platform-independent browser logic.
//
// Contains: Tab management, navigation, history, URL encoding, home page HTML,
// image fetch throttling. Used by all platform shells (Win32, macOS, Linux).
//
#include "network/fetcher.h"
#include "network/url.h"
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
<html><head><title>Helix</title>
<style>
body { font-family: 'Segoe UI', system-ui, sans-serif; margin: 0; padding: 40px; background: #fafafa; }
h1 { font-size: 28px; color: #333; margin-bottom: 4px; }
p { color: #666; line-height: 1.6; }
a { color: #0366d6; }
.shortcuts { margin-top: 32px; }
.shortcuts a { display: inline-block; background: #fff; border: 1px solid #ddd;
  border-radius: 8px; padding: 12px 20px; margin: 6px; text-decoration: none; color: #333; }
.shortcuts a:hover { border-color: #aaa; }
kbd { background: #eee; border: 1px solid #ccc; border-radius: 3px; padding: 2px 6px; font-size: 13px; }
</style></head><body>
<h1>Helix</h1>
<p>A web browser, from scratch.</p>
<div class="shortcuts">
<a href="https://en.wikipedia.org/wiki/Main_Page">Wikipedia</a>
<a href="https://news.ycombinator.com">Hacker News</a>
<a href="https://lite.cnn.com">CNN Lite</a>
<a href="helix://history">History</a>
</div>
<p style="margin-top:32px;font-size:13px;color:#999">
<strong>Ctrl+L</strong> &mdash; focus address bar &bull;
<strong>Ctrl+T</strong> &mdash; new tab &bull;
<strong>Ctrl+W</strong> &mdash; close tab<br>
<strong>Ctrl+F</strong> &mdash; find in page &bull;
<strong>Escape</strong> &mdash; stop loading / close find bar</p>
</body></html>)html";
    return html;
}

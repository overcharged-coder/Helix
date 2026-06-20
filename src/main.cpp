#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include "network/fetcher.h"
#include "network/url.h"
#include "html/parser.h"
#include "layout/scroll.h"
#include "render/renderer.h"
#include "js/engine.h"

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <condition_variable>

// A tiny counting semaphore (C++17 has no std::counting_semaphore). Caps how
// many image fetches run at once: firing ~50 simultaneous requests at a host
// like Wikimedia gets the burst rate-limited (HTTP 429), so most images fail.
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
// ~6 matches a typical browser's per-host connection limit.
static Semaphore g_imageFetchGate(6);

// ─── control IDs ─────────────────────────────────────────────────────────────
enum : int { IDC_BACK = 101, IDC_FWRD, IDC_REFR, IDC_STOP, IDC_HOME, IDC_URL, IDC_FIND };

// ─── custom messages ──────────────────────────────────────────────────────────
constexpr UINT WM_PAGE_READY  = WM_USER + 1;
constexpr UINT WM_IMAGE_READY = WM_USER + 2;
constexpr UINT WM_NEWTAB_NAVIGATE = WM_USER + 3;  // wParam=tabIdx, lParam=Page*

// ─── data types ──────────────────────────────────────────────────────────────
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
    std::string           url      = "helix://home";
    std::string           displayUrl;   // shown in URL bar (e.g. search query, not the bing URL)
    std::string           title    = "Helix";
    std::shared_ptr<Page> page;
    float                 scrollY  = 0.f;
    float                 docHeight= 600.f;
    bool                  loading  = false;
    std::string           pendingFragment;
    bool                  fragmentScrollPending = false;
    std::vector<std::string> history;
    int                   histIdx  = -1;
};

// ─── globals ─────────────────────────────────────────────────────────────────
static HWND     g_hwnd;
static HWND     g_hwndBack, g_hwndFwrd, g_hwndRefr, g_hwndStop, g_hwndHome, g_hwndUrl;
static HWND     g_hwndStatus;
static HWND     g_hwndFind;
static bool     g_findVisible = false;
static Renderer g_renderer;
static JsEngine g_js;

static std::vector<Tab> g_tabs;
static int      g_activeTab = 0;

static HCURSOR  g_cursorArrow, g_cursorHand;

// ─── layout constants ─────────────────────────────────────────────────────────
constexpr int TAB_H     = 36;   // tab strip height
constexpr int TOOLBAR_H = 44;   // toolbar (buttons + URL bar)
constexpr int STATUS_H  = 22;
constexpr int FIND_H    = 34;   // find bar height
constexpr int TOP_INSET = TAB_H + TOOLBAR_H;  // total above content
constexpr int BTN_W     = 38;
constexpr int BTN_H     = 28;
constexpr int MARGIN    =  6;

// ─── active tab helpers ───────────────────────────────────────────────────────
static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ─── string helpers ───────────────────────────────────────────────────────────
static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static std::string LowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static void SetUrlBar(const std::string& url) {
    SetWindowTextW(g_hwndUrl, ToWide(url).c_str());
}
static void SetUrlBarForTab(const Tab& tab) {
    SetUrlBar(tab.displayUrl.empty() ? tab.url : tab.displayUrl);
}
static void SetStatus(const std::string& s) {
    SetWindowTextW(g_hwndStatus, ToWide(s).c_str());
}
static void UpdateTitle() {
    std::wstring t = ToWide(CurTab().title);
    if (t.empty()) t = L"New Tab";
    SetWindowTextW(g_hwnd, (t + L" \x2014 Helix").c_str());
}

static bool AttrContainsToken(const std::string& value, const std::string& token) {
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

static bool StylesheetMediaApplies(const std::string& media) {
    std::string lower = LowerAscii(media);
    if (lower.empty()) return true;
    return lower.find("all") != std::string::npos
        || lower.find("screen") != std::string::npos
        || lower.find("projection") != std::string::npos;
}

static Node* FindFirstElement(Node* root, const std::string& tag) {
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

static void LoadExternalStylesheets(const std::shared_ptr<Node>& dom, const std::string& pageUrl) {
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
                    style->appendChild(Node::makeText(res.body));
                    attach->appendChild(style);
                    ++loaded;
                }
            }
        }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
            stack.push_back(it->get());
    }
}

// ─── scrollbar ───────────────────────────────────────────────────────────────
static int ViewportH() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    return rc.bottom - rc.top - TOP_INSET - STATUS_H;
}
static void UpdateScrollbar() {
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin  = 0;
    si.nMax  = (int)CurTab().docHeight;
    si.nPage = (UINT)std::max(0, ViewportH());
    si.nPos  = (int)CurTab().scrollY;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}
static void ClampScroll() {
    float maxY = std::max(0.f, CurTab().docHeight - (float)ViewportH());
    CurTab().scrollY = std::max(0.f, std::min(CurTab().scrollY, maxY));
}

// ─── built-in home page ───────────────────────────────────────────────────────
static const std::string kHomeHtml = R"html(<!DOCTYPE html>
<html>
<head><title>Helix</title></head>
<body>
<h1>Helix</h1>
<p>Your browser. Built from scratch in C++. No Chromium. No WebView. Everything is yours.</p>
<hr>
<h3>Keyboard shortcuts</h3>
<p><strong>Ctrl+L</strong> &mdash; focus address bar</p>
<p><strong>Ctrl+T</strong> &mdash; new tab</p>
<p><strong>Ctrl+W</strong> &mdash; close tab</p>
<p><strong>Ctrl+Tab</strong> &mdash; next tab</p>
<p><strong>Ctrl+1&ndash;9</strong> &mdash; switch to tab</p>
<p><strong>F5 / Ctrl+R</strong> &mdash; reload</p>
<p><strong>Alt+Left / Alt+Right</strong> &mdash; back / forward</p>
<p><strong>Ctrl+H</strong> &mdash; history</p>
<p><strong>Ctrl+= / Ctrl+-</strong> &mdash; zoom in / out</p>
<p><strong>Ctrl+0</strong> &mdash; reset zoom</p>
<p><strong>Ctrl+F</strong> &mdash; find in page</p>
<p><strong>Escape</strong> &mdash; stop loading / close find bar</p>
<hr>
<h3>What Helix has</h3>
<p>Custom HTML5 tokenizer &bull; DOM builder &bull; CSS cascade with combinators &bull;
Attribute selectors &bull; Inline &amp; block layout &bull; Direct2D renderer &bull;
WIC image loading &bull; Tabs &bull; Per-tab history &bull; Zoom</p>
</body>
</html>)html";

// ─── title extraction ────────────────────────────────────────────────────────
static std::string ExtractTitle(const Node* root) {
    if (!root) return {};
    std::function<std::string(const Node*)> find = [&](const Node* n) -> std::string {
        if (!n) return {};
        if (n->type == NodeType::Element && n->tagName == "title") {
            std::string t;
            for (auto& c : n->children)
                if (c->type == NodeType::Text) t += c->text;
            while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
            while (!t.empty() && isspace((unsigned char)t.back()))  t.pop_back();
            return t;
        }
        for (auto& c : n->children) {
            auto r = find(c.get());
            if (!r.empty()) return r;
        }
        return {};
    };
    return find(root);
}

// ─── address-bar input: URL vs. search query ──────────────────────────────────
// Percent-encode a search query for a URL.
static std::string UrlEncodeQuery(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += '+';
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
    }
    return out;
}

// Decide whether address-bar text is a URL to visit or a search query.
static bool LooksLikeUrl(const std::string& s) {
    if (s.empty()) return false;
    if (s.find("://") != std::string::npos) return true;          // has scheme
    if (s.rfind("helix://", 0) == 0 || s.rfind("about:", 0) == 0) return true;
    if (s.find(' ') != std::string::npos) return false;           // a space → search
    if (s == "localhost" || s.rfind("localhost:", 0) == 0) return true;
    size_t dot = s.find('.');
    if (dot == std::string::npos) return false;                   // no dot → search
    if (dot == 0 || dot == s.size() - 1) return false;            // ".x" / "x." → search
    return true;                                                  // looks like a domain
}

// ─── navigation ──────────────────────────────────────────────────────────────
static void Navigate(int tabIdx, const std::string& rawUrl, bool pushHistory = true);
static void Navigate(const std::string& rawUrl, bool push = true) {
    Navigate(g_activeTab, rawUrl, push);
}

static void TabPushHistory(Tab& tab, const std::string& url) {
    if (tab.histIdx + 1 < (int)tab.history.size())
        tab.history.erase(tab.history.begin() + tab.histIdx + 1, tab.history.end());
    tab.history.push_back(url);
    tab.histIdx = (int)tab.history.size() - 1;
}

static std::string UrlFragment(const std::string& url) {
    size_t hash = url.find('#');
    if (hash == std::string::npos || hash + 1 >= url.size()) return {};
    return url.substr(hash + 1);
}

static std::string UrlWithoutFragment(const std::string& url) {
    size_t hash = url.find('#');
    return hash == std::string::npos ? url : url.substr(0, hash);
}

static void Navigate(int tabIdx, const std::string& rawUrl, bool pushHistory) {
    if (tabIdx < 0 || tabIdx >= (int)g_tabs.size()) return;
    Tab& tab = g_tabs[tabIdx];
    if (tab.loading) return;

    std::string url = rawUrl;
    tab.displayUrl.clear();

    // ── built-in: home ──────────────────────────────────────────────────
    if (url.empty() || url == "helix://home" || url == "felix://home") {
        url = "helix://home";
        tab.page.reset(new Page{ url, ParseHtml(kHomeHtml), {} });
        tab.title   = "Helix";
        tab.scrollY = 0.f;
        tab.pendingFragment.clear();
        tab.fragmentScrollPending = false;
        if (pushHistory) TabPushHistory(tab, url);
        if (tabIdx == g_activeTab) {
            SetUrlBar(url);
            UpdateTitle();
            UpdateScrollbar();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        return;
    }

    // ── built-in: history ──────────────────────────────────────────────
    if (url == "helix://history" || url == "felix://history") {
        url = "helix://history";
        std::string html = "<html><body><h1>History</h1>";
        bool any = false;
        for (int i = (int)tab.history.size() - 1; i >= 0; i--) {
            const auto& h = tab.history[i];
            if (h == "helix://history") continue;
            html += "<p><a href=\"" + h + "\">" + h + "</a></p>";
            any = true;
        }
        if (!any) html += "<p>No history yet.</p>";
        html += "</body></html>";
        tab.page.reset(new Page{ url, ParseHtml(html), {} });
        tab.title   = "History";
        tab.scrollY = 0.f;
        tab.pendingFragment.clear();
        tab.fragmentScrollPending = false;
        if (pushHistory) TabPushHistory(tab, url);
        if (tabIdx == g_activeTab) {
            SetUrlBar(url);
            UpdateTitle();
            UpdateScrollbar();
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        return;
    }

    // If it's a URL, ensure it has a scheme; otherwise treat it as a search
    // query and route it to Bing's server-rendered results page.
    std::string displayUrl = url;
    if (LooksLikeUrl(url)) {
        if (url.find("://") == std::string::npos)
            url = "https://" + url;
        displayUrl = url;
    } else {
        displayUrl = url;
        url = "https://www.bing.com/search?q=" + UrlEncodeQuery(url);
    }

    tab.loading    = true;
    tab.url        = url;
    tab.displayUrl = displayUrl;
    tab.title      = "Loading…";
    tab.pendingFragment = UrlFragment(url);
    tab.fragmentScrollPending = !tab.pendingFragment.empty();
    if (pushHistory) TabPushHistory(tab, url);

    if (tabIdx == g_activeTab) {
        EnableWindow(g_hwndStop, TRUE);
        EnableWindow(g_hwndRefr, FALSE);
        SetUrlBar(displayUrl);
        UpdateTitle();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }

    HWND hwnd = g_hwnd;
    std::string fetchUrl = UrlWithoutFragment(url);
    std::thread([hwnd, url, fetchUrl, tabIdx]() {
        auto* p = new Page;
        p->url   = url;
        try {
            auto res = FetchUrl(fetchUrl);
            if (res.success) {
                p->dom = ParseHtml(res.body);
                if (!res.finalUrl.empty() && res.finalUrl != url)
                    p->url = res.finalUrl;
                LoadExternalStylesheets(p->dom, p->url);
            } else {
                p->error = res.error;
            }
        } catch (...) {
            p->dom.reset();
            p->error = "Failed to load page (internal error).";
        }
        auto* pm = new PageMsg{ tabIdx, p };
        PostMessageW(hwnd, WM_PAGE_READY, 0, (LPARAM)pm);
    }).detach();
}

static void GoBack() {
    Tab& tab = CurTab();
    if (tab.histIdx > 0) Navigate(g_activeTab, tab.history[--tab.histIdx], false);
}
static void GoForward() {
    Tab& tab = CurTab();
    if (tab.histIdx + 1 < (int)tab.history.size())
        Navigate(g_activeTab, tab.history[++tab.histIdx], false);
}

// ─── tab management ───────────────────────────────────────────────────────────
static void NewTab(const std::string& url = "helix://home") {
    g_tabs.emplace_back();
    int idx = (int)g_tabs.size() - 1;
    g_activeTab = idx;
    InvalidateRect(g_hwnd, NULL, FALSE);
    Navigate(idx, url);
}

static void CloseTab(int idx) {
    if (g_tabs.size() <= 1) {
        Navigate("helix://home");
        return;
    }
    g_tabs.erase(g_tabs.begin() + idx);
    if (g_activeTab >= (int)g_tabs.size())
        g_activeTab = (int)g_tabs.size() - 1;
    SetUrlBarForTab(CurTab());
    UpdateTitle();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void SwitchTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) return;
    g_activeTab = idx;
    const Tab& tab = CurTab();
    SetUrlBarForTab(tab);
    UpdateTitle();
    if (tab.loading) {
        EnableWindow(g_hwndStop, TRUE);
        EnableWindow(g_hwndRefr, FALSE);
    } else {
        EnableWindow(g_hwndStop, FALSE);
        EnableWindow(g_hwndRefr, TRUE);
    }
    ClampScroll();
    UpdateScrollbar();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ─── control layout ──────────────────────────────────────────────────────────
static void LayoutControls() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int btnY = TAB_H + (TOOLBAR_H - BTN_H) / 2;
    int x    = MARGIN;
    SetWindowPos(g_hwndBack, NULL, x,               btnY, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFwrd, NULL, x + BTN_W,       btnY, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndRefr, NULL, x + BTN_W * 2,   btnY, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndStop, NULL, x + BTN_W * 3,   btnY, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndHome, NULL, x + BTN_W * 4,   btnY, BTN_W, BTN_H, SWP_NOZORDER);
    int urlX = x + BTN_W * 5 + MARGIN;
    SetWindowPos(g_hwndUrl,    NULL, urlX, btnY, w - urlX - MARGIN, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFind,   NULL, 0, h - STATUS_H - FIND_H, w, FIND_H, SWP_NOZORDER);
}

// ─── URL bar subclass ─────────────────────────────────────────────────────────
LRESULT CALLBACK UrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        wchar_t buf[2048] = {};
        GetWindowTextW(hwnd, buf, 2048);
        Navigate(ToUtf8(buf));
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r') return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── find bar helpers ─────────────────────────────────────────────────────────
static void ShowFind(bool show) {
    g_findVisible = show;
    ShowWindow(g_hwndFind, show ? SW_SHOW : SW_HIDE);
    if (show) {
        SetFocus(g_hwndFind);
        SendMessageW(g_hwndFind, EM_SETSEL, 0, -1);
    } else {
        g_renderer.SetSearchQuery(L"");
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    // Re-layout status bar (find bar sits above it)
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFind,   NULL, 0, h - STATUS_H - FIND_H, w, FIND_H, SWP_NOZORDER);
}

LRESULT CALLBACK FindProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { ShowFind(false); return 0; }
        if (wp == VK_RETURN) { return 0; }
    }
    if (msg == WM_CHAR || (msg == WM_KEYDOWN && wp != VK_ESCAPE)) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        // Update search query from current text
        wchar_t buf[512] = {};
        GetWindowTextW(hwnd, buf, 512);
        g_renderer.SetSearchQuery(buf);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return r;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ─── build tab entries for renderer ──────────────────────────────────────────
static std::vector<TabEntry> BuildTabEntries() {
    std::vector<TabEntry> entries;
    entries.reserve(g_tabs.size());
    for (int i = 0; i < (int)g_tabs.size(); i++) {
        TabEntry e;
        e.title   = ToWide(g_tabs[i].title.empty() ? "New Tab" : g_tabs[i].title);
        e.active  = (i == g_activeTab);
        e.loading = g_tabs[i].loading;
        entries.push_back(std::move(e));
    }
    return entries;
}

// ─── WndProc ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_cursorArrow = LoadCursor(NULL, IDC_ARROW);
        g_cursorHand  = LoadCursor(NULL, IDC_HAND);

        HINSTANCE hi = GetModuleHandleW(NULL);
        auto btn = [&](LPCWSTR t, int id) {
            return CreateWindowW(L"BUTTON", t, WS_CHILD | WS_VISIBLE,
                0,0,0,0, hwnd, (HMENU)(intptr_t)id, hi, NULL);
        };
        g_hwndBack = btn(L"\x2190", IDC_BACK);
        g_hwndFwrd = btn(L"\x2192", IDC_FWRD);
        g_hwndRefr = btn(L"\x21BB", IDC_REFR);
        g_hwndStop = btn(L"\x2715", IDC_STOP);
        g_hwndHome = btn(L"\x2302", IDC_HOME);

        g_hwndUrl = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_URL, hi, NULL);
        g_hwndStatus = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
            0,0,0,0, hwnd, NULL, hi, NULL);

        g_hwndFind = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_FIND, hi, NULL);

        EnableWindow(g_hwndStop, FALSE);
        SetWindowSubclass(g_hwndUrl,  UrlProc,  1, 0);
        SetWindowSubclass(g_hwndFind, FindProc, 2, 0);

        g_renderer.SetImageRequestCallback([hwnd](std::string url) {
            std::thread([hwnd, url]() {
                g_imageFetchGate.acquire();
                auto res = FetchUrl(url);
                g_imageFetchGate.release();
                if (res.success && !res.body.empty()) {
                    auto* m = new ImageMsg;
                    m->url   = url;
                    m->bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
                    PostMessageW(hwnd, WM_IMAGE_READY, 0, (LPARAM)m);
                }
            }).detach();
        });

        g_renderer.Init(hwnd);

        // Start with one tab
        g_tabs.emplace_back();
        Navigate(std::string("helix://home"));
        return 0;
    }

    case WM_SIZE: {
        UINT w = LOWORD(lp), h = HIWORD(lp);
        LayoutControls();
        g_renderer.Resize(w, h);
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);

        auto tabs = BuildTabEntries();
        Tab& cur = CurTab();
        bool repaintForFragment = false;
        if (cur.page && cur.page->dom) {
            CurTab().docHeight = g_renderer.Paint(
                cur.page->dom, cur.scrollY, cur.page->url,
                (float)TOP_INSET, (float)TAB_H, &tabs);
            if (cur.fragmentScrollPending) {
                cur.fragmentScrollPending = false;
                float anchorY = 0.f;
                if (!cur.pendingFragment.empty()
                    && g_renderer.GetAnchorY(cur.pendingFragment, anchorY)) {
                    cur.docHeight = FragmentReachableDocumentHeight(
                        cur.docHeight, anchorY, (float)ViewportH());
                    cur.scrollY = std::max(0.f, anchorY - 16.f);
                    ClampScroll();
                    repaintForFragment = true;
                }
            }
        } else {
            g_renderer.Paint(nullptr, 0.f, {},
                (float)TOP_INSET, (float)TAB_H, &tabs);
        }
        UpdateScrollbar();
        EndPaint(hwnd, &ps);
        if (repaintForFragment) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_PAGE_READY: {
        auto* pm = reinterpret_cast<PageMsg*>(lp);
        int idx  = pm->tabIdx;
        Page* p  = pm->page;
        delete pm;

        if (idx >= 0 && idx < (int)g_tabs.size()) {
            Tab& tab   = g_tabs[idx];
            tab.page.reset(p);
            tab.scrollY = 0.f;
            tab.loading = false;
            if (p->dom) {
                tab.url = p->url;
                std::string title = ExtractTitle(p->dom.get());
                tab.title = title.empty() ? p->url : title;
            } else {
                std::string html = "<html><body><h2>Error</h2><p>"
                    + p->error + "</p></body></html>";
                tab.page->dom = ParseHtml(html);
                tab.title = "Error";
            }
        }

        if (idx == g_activeTab) {
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            SetUrlBarForTab(CurTab());
            UpdateTitle();
            ClampScroll();
            UpdateScrollbar();

        }

        // Run <script> tags in the loaded page
        if (idx >= 0 && idx < (int)g_tabs.size() && g_tabs[idx].page && g_tabs[idx].page->dom) {
            try {
                auto repaint = [hwnd]() { InvalidateRect(hwnd, NULL, FALSE); };
                g_js.setDocument(g_tabs[idx].page->dom, repaint);
                std::vector<const Node*> stack;
                stack.push_back(g_tabs[idx].page->dom.get());
                size_t scriptCount = 0;
                size_t totalScriptBytes = 0;
                while (!stack.empty()) {
                    const Node* n = stack.back();
                    stack.pop_back();
                    if (!n) continue;
                    if (n->type == NodeType::Element && n->tagName == "script") {
                        std::string src;
                        for (auto& c : n->children)
                            if (c->type == NodeType::Text) src += c->text;
                        totalScriptBytes += src.size();
                        // Page scripts are opt-in: heavy real-world JS (e.g. Bing's
                        // search page) running in our incomplete environment tends to
                        // rewrite/blank the DOM. Rendering the static HTML is far more
                        // useful. Set HELIX_JS=1 to re-enable script execution.
                        static bool jsEnabled = (getenv("HELIX_JS") != nullptr);
                        if (jsEnabled && !src.empty() && scriptCount < 128 && totalScriptBytes <= 256 * 1024) {
                            g_js.runScript(src, "inline");
                        }
                        ++scriptCount;
                    }
                    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                        stack.push_back(it->get());
                }
                // Set up timer for macrotasks / setTimeout
                SetTimer(hwnd, 1, 16, NULL);
            } catch (...) {
                OutputDebugStringA("[JS] Page script setup failed; continuing without page scripts\n");
            }
        }

        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_IMAGE_READY: {
        auto* m = reinterpret_cast<ImageMsg*>(lp);
        g_renderer.ReceiveImage(m->url, m->bytes);
        delete m;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP:     CurTab().scrollY -= 30.f;                    break;
        case SB_LINEDOWN:   CurTab().scrollY += 30.f;                    break;
        case SB_PAGEUP:     CurTab().scrollY -= (float)si.nPage;         break;
        case SB_PAGEDOWN:   CurTab().scrollY += (float)si.nPage;         break;
        case SB_THUMBTRACK: CurTab().scrollY  = (float)si.nTrackPos;     break;
        }
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        CurTab().scrollY -= GET_WHEEL_DELTA_WPARAM(wp) * 0.5f;
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        // Check tab strip
        if (py < TAB_H) {
            int closeIdx = -1;
            if (g_renderer.HitTestTabClose((float)px, (float)py, closeIdx)) {
                CloseTab(closeIdx);
            } else {
                int tidx = g_renderer.HitTestTab((float)px, (float)py);
                if (tidx == -1) {
                    NewTab();
                } else if (tidx >= 0) {
                    SwitchTab(tidx);
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py >= TOP_INSET) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            if (!href.empty()) Navigate(href);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py >= TOP_INSET) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            SetCursor(href.empty() ? g_cursorArrow : g_cursorHand);
            SetStatus(href);
        } else {
            SetCursor(g_cursorArrow);
            SetStatus({});
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BACK: GoBack();    break;
        case IDC_FWRD: GoForward(); break;
        case IDC_HOME: Navigate("helix://home"); break;
        case IDC_REFR:
            if (CurTab().page) Navigate(CurTab().url, false);
            break;
        case IDC_STOP:
            CurTab().loading = false;
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            break;
        }
        return 0;

    case WM_TIMER:
        try {
            g_js.runMacrotasks();
        } catch (...) {
            OutputDebugStringA("[JS] Macrotask pump failed; timer stopped\n");
            KillTimer(hwnd, 1);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ─── entry point ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc   = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"HelixBrowser";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0,
        L"HelixBrowser", L"Helix",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl    = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt     = (GetKeyState(VK_MENU)    & 0x8000) != 0;
            bool shift   = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool handled = false;

            if (ctrl) {
                if (msg.wParam == 'L') {
                    SetFocus(g_hwndUrl);
                    SendMessageW(g_hwndUrl, EM_SETSEL, 0, -1);
                    handled = true;
                } else if (msg.wParam == 'T') {
                    NewTab();
                    handled = true;
                } else if (msg.wParam == 'W') {
                    CloseTab(g_activeTab);
                    handled = true;
                } else if (msg.wParam == VK_TAB) {
                    int next = (g_activeTab + (shift ? -1 : 1) + (int)g_tabs.size())
                             % (int)g_tabs.size();
                    SwitchTab(next);
                    handled = true;
                } else if (msg.wParam >= '1' && msg.wParam <= '9') {
                    SwitchTab((int)(msg.wParam - '1'));
                    handled = true;
                } else if (msg.wParam == 'R' || msg.wParam == VK_F5) {
                    if (!CurTab().loading) Navigate(CurTab().url, false);
                    handled = true;
                } else if (msg.wParam == 'H') {
                    Navigate("helix://history");
                    handled = true;
                } else if (msg.wParam == 'F') {
                    ShowFind(!g_findVisible);
                    handled = true;
                } else if (msg.wParam == 'G') {
                    // Ctrl+G / Ctrl+Shift+G = find next / prev (stub for now)
                    handled = true;
                } else if (msg.wParam == VK_OEM_PLUS || msg.wParam == '=') {
                    g_renderer.SetZoom(g_renderer.GetZoom() + 0.1f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                } else if (msg.wParam == VK_OEM_MINUS) {
                    g_renderer.SetZoom(g_renderer.GetZoom() - 0.1f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                } else if (msg.wParam == '0') {
                    g_renderer.SetZoom(1.f);
                    InvalidateRect(g_hwnd, NULL, FALSE);
                    handled = true;
                }
            }

            if (!handled && msg.wParam == VK_F5) {
                if (!CurTab().loading) Navigate(CurTab().url, false);
                handled = true;
            }
            if (!handled && alt) {
                if (msg.wParam == VK_LEFT)  { GoBack();    handled = true; }
                if (msg.wParam == VK_RIGHT) { GoForward(); handled = true; }
            }
            if (!handled && msg.wParam == VK_ESCAPE) {
                if (g_findVisible) {
                    ShowFind(false);
                    handled = true;
                } else if (CurTab().loading) {
                    CurTab().loading = false;
                    CurTab().title   = CurTab().url;
                    EnableWindow(g_hwndStop, FALSE);
                    EnableWindow(g_hwndRefr, TRUE);
                    handled = true;
                }
            }

            if (handled) continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}

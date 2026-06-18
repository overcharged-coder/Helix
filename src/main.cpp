#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#include "network/fetcher.h"
#include "html/parser.h"
#include "render/renderer.h"

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

// ─── control IDs ─────────────────────────────────────────────────────────────
enum : int { IDC_BACK = 101, IDC_FWRD, IDC_REFR, IDC_STOP, IDC_HOME, IDC_URL };

// ─── custom window messages ───────────────────────────────────────────────────
constexpr UINT WM_PAGE_READY  = WM_USER + 1;  // lParam = Page*
constexpr UINT WM_IMAGE_READY = WM_USER + 2;  // lParam = ImageMsg*

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

// ─── globals ─────────────────────────────────────────────────────────────────
static HWND     g_hwnd;
static HWND     g_hwndBack, g_hwndFwrd, g_hwndRefr, g_hwndStop, g_hwndHome, g_hwndUrl;
static HWND     g_hwndStatus;
static Renderer g_renderer;
static float    g_scrollY   = 0.f;
static float    g_docHeight = 600.f;

static std::shared_ptr<Page> g_page;
static std::vector<std::string> g_history;
static int      g_histIdx = -1;

static std::atomic<bool> g_loading{ false };
static HCURSOR  g_cursorArrow, g_cursorHand;

// ─── layout constants ─────────────────────────────────────────────────────────
constexpr int TOOLBAR_H = 44;
constexpr int STATUS_H  = 22;
constexpr int BTN_W     = 38;
constexpr int BTN_H     = 28;
constexpr int MARGIN    =  6;

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
static void SetUrlBar(const std::string& url) {
    SetWindowTextW(g_hwndUrl, ToWide(url).c_str());
}
static void SetStatus(const std::string& s) {
    SetWindowTextW(g_hwndStatus, ToWide(s).c_str());
}
static void SetTitle(const std::wstring& t) {
    SetWindowTextW(g_hwnd, (t + L" — Felix").c_str());
}

// ─── scrollbar ───────────────────────────────────────────────────────────────
static int ViewportH() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    return rc.bottom - rc.top - TOOLBAR_H - STATUS_H;
}
static void UpdateScrollbar() {
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin  = 0;
    si.nMax  = (int)g_docHeight;
    si.nPage = (UINT)std::max(0, ViewportH());
    si.nPos  = (int)g_scrollY;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}
static void ClampScroll() {
    float maxY = std::max(0.f, g_docHeight - (float)ViewportH());
    g_scrollY  = std::max(0.f, std::min(g_scrollY, maxY));
}

// ─── built-in home page ───────────────────────────────────────────────────────
static const std::string kHomeHtml = R"html(<!DOCTYPE html>
<html>
<head><title>Felix</title></head>
<body>
<h1>Felix</h1>
<p>Your browser. Type a URL above and press <strong>Enter</strong>.</p>
<hr>
<h3>Keyboard shortcuts</h3>
<p><strong>Ctrl+L</strong> — focus address bar</p>
<p><strong>F5 / Ctrl+R</strong> — reload</p>
<p><strong>Alt+Left / Alt+Right</strong> — back / forward</p>
<p><strong>Escape</strong> — stop loading</p>
<hr>
<h3>About</h3>
<p>Felix is a hand-built C++ browser. It has its own HTML tokenizer,
DOM builder, layout engine, Direct2D renderer, and CSS color parser.
No Chromium. No WebView. Everything is yours.</p>
</body>
</html>)html";

// ─── navigation ──────────────────────────────────────────────────────────────
static void Navigate(const std::string& rawUrl, bool pushHistory = true);

static void PushHistory(const std::string& url) {
    if (g_histIdx + 1 < (int)g_history.size())
        g_history.erase(g_history.begin() + g_histIdx + 1, g_history.end());
    g_history.push_back(url);
    g_histIdx = (int)g_history.size() - 1;
}

static void Navigate(const std::string& rawUrl, bool pushHistory) {
    if (g_loading.load()) return;

    std::string url = rawUrl;

    // ── built-in pages ──────────────────────────────────────────────────
    if (url.empty() || url == "felix://home" || url == "felix:home") {
        url = "felix://home";
        auto* p  = new Page{ url, ParseHtml(kHomeHtml), {} };
        g_page.reset(p);
        g_scrollY = 0.f;
        if (pushHistory) PushHistory(url);
        SetUrlBar(url);
        SetWindowTextW(g_hwnd, L"Felix");
        UpdateScrollbar();
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    if (url.find("://") == std::string::npos)
        url = "https://" + url;

    g_loading = true;
    EnableWindow(g_hwndStop, TRUE);
    EnableWindow(g_hwndRefr, FALSE);
    SetUrlBar(url);
    SetWindowTextW(g_hwnd, L"Loading… — Felix");

    if (pushHistory) PushHistory(url);

    HWND hwnd = g_hwnd;
    std::thread([hwnd, url]() {
        auto* p  = new Page;
        p->url   = url;
        auto res = FetchUrl(url);
        if (res.success) {
            p->dom = ParseHtml(res.body);
            // Use final URL after redirects if different
            if (!res.finalUrl.empty() && res.finalUrl != url)
                p->url = res.finalUrl;
        } else {
            p->error = res.error;
        }
        PostMessageW(hwnd, WM_PAGE_READY, 0, (LPARAM)p);
    }).detach();
}

static void GoBack() {
    if (g_histIdx > 0) Navigate(g_history[--g_histIdx], false);
}
static void GoForward() {
    if (g_histIdx + 1 < (int)g_history.size()) Navigate(g_history[++g_histIdx], false);
}

// ─── control layout ──────────────────────────────────────────────────────────
static void LayoutControls() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int y = (TOOLBAR_H - BTN_H) / 2;
    int x = MARGIN;
    SetWindowPos(g_hwndBack, NULL, x,                  y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndFwrd, NULL, x + BTN_W,          y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndRefr, NULL, x + BTN_W * 2,      y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndStop, NULL, x + BTN_W * 3,      y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndHome, NULL, x + BTN_W * 4,      y, BTN_W, BTN_H, SWP_NOZORDER);
    int urlX = x + BTN_W * 5 + MARGIN;
    SetWindowPos(g_hwndUrl,    NULL, urlX, y, w - urlX - MARGIN, BTN_H, SWP_NOZORDER);
    SetWindowPos(g_hwndStatus, NULL, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
}

// ─── URL bar subclass (Enter key) ────────────────────────────────────────────
LRESULT CALLBACK UrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        wchar_t buf[2048] = {};
        GetWindowTextW(hwnd, buf, 2048);
        Navigate(ToUtf8(buf));
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
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
        g_hwndBack = btn(L"←", IDC_BACK);  // ←
        g_hwndFwrd = btn(L"→", IDC_FWRD);  // →
        g_hwndRefr = btn(L"↻", IDC_REFR);  // ↻
        g_hwndStop = btn(L"✕", IDC_STOP);  // ✕
        g_hwndHome = btn(L"⌂", IDC_HOME);  // ⌂

        g_hwndUrl = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_URL, hi, NULL);

        g_hwndStatus = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_SUNKEN,
            0,0,0,0, hwnd, NULL, hi, NULL);

        EnableWindow(g_hwndStop, FALSE);
        SetWindowSubclass(g_hwndUrl, UrlProc, 1, 0);

        // Wire up image loading
        g_renderer.SetImageRequestCallback([hwnd](std::string url) {
            std::thread([hwnd, url]() {
                auto res = FetchUrl(url);
                if (res.success && !res.body.empty()) {
                    auto* m = new ImageMsg;
                    m->url   = url;
                    m->bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
                    PostMessageW(hwnd, WM_IMAGE_READY, 0, (LPARAM)m);
                }
            }).detach();
        });

        g_renderer.Init(hwnd);
        Navigate("felix://home");
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
        if (g_page && g_page->dom)
            g_docHeight = g_renderer.Paint(g_page->dom, g_scrollY, g_page->url,
                                           (float)TOOLBAR_H);
        else
            g_renderer.Paint(nullptr, 0.f, {}, (float)TOOLBAR_H);
        UpdateScrollbar();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_PAGE_READY: {
        auto* p = reinterpret_cast<Page*>(lp);
        g_page.reset(p);
        g_scrollY = 0.f;
        g_loading = false;
        EnableWindow(g_hwndStop, FALSE);
        EnableWindow(g_hwndRefr, TRUE);
        if (p->dom) {
            SetUrlBar(p->url);
            SetTitle(ToWide(p->url));
        } else {
            std::string html = "<html><body><h2>Error</h2><p>" + p->error + "</p></body></html>";
            g_page->dom = ParseHtml(html);
            SetWindowTextW(hwnd, L"Error — Felix");
        }
        ClampScroll();
        UpdateScrollbar();
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
        case SB_LINEUP:     g_scrollY -= 30.f;               break;
        case SB_LINEDOWN:   g_scrollY += 30.f;               break;
        case SB_PAGEUP:     g_scrollY -= (float)si.nPage;    break;
        case SB_PAGEDOWN:   g_scrollY += (float)si.nPage;    break;
        case SB_THUMBTRACK: g_scrollY  = (float)si.nTrackPos; break;
        }
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        g_scrollY -= GET_WHEEL_DELTA_WPARAM(wp) * 0.5f;
        ClampScroll();
        UpdateScrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py > TOOLBAR_H && py < HIWORD(lp) - STATUS_H) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            SetCursor(href.empty() ? g_cursorArrow : g_cursorHand);
            SetStatus(href);
        } else {
            SetStatus({});
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int px = (int)(short)LOWORD(lp);
        int py = (int)(short)HIWORD(lp);
        if (py > TOOLBAR_H) {
            std::string href = g_renderer.HitTest((float)px, (float)py);
            if (!href.empty()) Navigate(href);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BACK: GoBack();    break;
        case IDC_FWRD: GoForward(); break;
        case IDC_HOME: Navigate("felix://home"); break;
        case IDC_REFR:
            if (g_page) Navigate(g_page->url, false);
            break;
        case IDC_STOP:
            g_loading = false;
            EnableWindow(g_hwndStop, FALSE);
            EnableWindow(g_hwndRefr, TRUE);
            break;
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

    WNDCLASSEXW wc  = { sizeof(wc) };
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.lpszClassName = L"FelixBrowser";
    wc.hCursor      = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon        = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0,
        L"FelixBrowser", L"Felix",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
        NULL, NULL, hInst, NULL);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        // ── global keyboard shortcuts ──────────────────────────────────────
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt  = (GetKeyState(VK_MENU)    & 0x8000) != 0;
            bool handled = false;

            if (ctrl && msg.wParam == 'L') {
                SetFocus(g_hwndUrl);
                SendMessageW(g_hwndUrl, EM_SETSEL, 0, -1);
                handled = true;
            } else if ((ctrl && msg.wParam == 'R') || msg.wParam == VK_F5) {
                if (g_page && !g_loading) Navigate(g_page->url, false);
                handled = true;
            } else if (alt && msg.wParam == VK_LEFT) {
                GoBack(); handled = true;
            } else if (alt && msg.wParam == VK_RIGHT) {
                GoForward(); handled = true;
            } else if (msg.wParam == VK_ESCAPE && g_loading) {
                g_loading = false;
                EnableWindow(g_hwndStop, FALSE);
                EnableWindow(g_hwndRefr, TRUE);
                handled = true;
            }

            if (handled) continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}

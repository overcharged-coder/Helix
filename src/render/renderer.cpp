#include "render/renderer.h"
#include "layout/layout_engine.h"
#include "css/stylesheet.h"
#include "render/webfont.h"
#include "render/transition.h"
#include "network/url.h"
#include "render/svg.h"
#include "third_party/stb_image.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#include <d2d1.h>
#include <dwrite.h>
#include <sstream>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cwchar>
#include <cctype>

// ─── helpers ─────────────────────────────────────────────────────────────────

static D2D1_COLOR_F ToD2D(const CssColor& c) { return { c.r, c.g, c.b, c.a }; }
static constexpr float kMarginX = 32.f;
static constexpr float kMarginY =  8.f;

static bool HasUrlScheme(const std::string& url) {
    size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    size_t stop = url.find_first_of("/?#");
    if (stop != std::string::npos && stop < colon) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = url[i];
        if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return true;
}

static bool LooksLikeImageUrl(const std::string& url) {
    std::string low;
    for (char c : url) low += (char)std::tolower((unsigned char)c);
    return low.rfind("data:image/", 0) == 0
        || low.find(".png") != std::string::npos
        || low.find(".jpg") != std::string::npos
        || low.find(".jpeg") != std::string::npos
        || low.find(".gif") != std::string::npos
        || low.find(".webp") != std::string::npos
        || low.find(".bmp") != std::string::npos
        || low.find(".svg") != std::string::npos;
}

static bool LooksLikeSvgUrl(const std::string& url) {
    std::string low;
    for (char c : url) low += (char)std::tolower((unsigned char)c);
    return low.find(".svg") != std::string::npos
        || low.find("image/svg+xml") != std::string::npos;
}

static bool IsBreakableWhitespace(wchar_t c) {
    return c != 0x00A0 && iswspace(c);
}

std::string Renderer::ResolveUrl(const std::string& href, const std::string& base) {
    if (href.empty()) return base;
    if (HasUrlScheme(href)) return href;
    if (href.size() >= 2 && href[0] == '/' && href[1] == '/')
        return "https:" + href;
    if (href[0] == '/') {
        size_t p = base.find("://");
        if (p == std::string::npos) return href;
        size_t sl = base.find('/', p + 3);
        return (sl == std::string::npos ? base : base.substr(0, sl)) + href;
    }
    size_t last = base.rfind('/');
    return (last == std::string::npos ? base : base.substr(0, last + 1)) + href;
}

ID2D1SolidColorBrush* Renderer::TempBrush(D2D1_COLOR_F color) {
    ID2D1SolidColorBrush* b = nullptr;
    if (m_rt) m_rt->CreateSolidColorBrush(color, &b);
    if (b) m_tempBrushes.push_back(b);
    return b;
}

// ─── init / teardown ─────────────────────────────────────────────────────────


void Renderer::CreateTabFont() {
    if (m_fmtTab) { m_fmtTab->Release(); m_fmtTab = nullptr; }
    if (!m_dwrite) return;
    m_dwrite->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.f, L"en-us", &m_fmtTab);
    if (m_fmtTab) {
        m_fmtTab->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_fmtTab->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_fmtTab->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

bool Renderer::Init(HWND hwnd) {
    m_hwnd = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_dwrite))))
        return false;
    // WIC removed — stb_image handles image decoding (cross-platform).
    CreateTabFont();
    return EnsureTarget();
}

bool Renderer::EnsureTarget() {
    if (m_rt) return true;
    RECT rc; GetClientRect(m_hwnd, &rc);
    m_width  = (UINT)(rc.right  - rc.left);
    m_height = (UINT)(rc.bottom - rc.top);
    // Force 96 DPI so Direct2D DIPs == physical pixels. The layout engine works
    // in physical pixels (m_width/m_height from GetClientRect), so the render
    // target must use the same coordinate space. Otherwise on a high-DPI display
    // (e.g. 125% scaling) the render target would be in scaled DIPs while layout
    // is in physical px, and everything centered in layout space drifts off.
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN),
        96.f, 96.f);
    if (FAILED(m_factory->CreateHwndRenderTarget(
            rtProps,
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(m_width, m_height)),
            &m_rt)))
        return false;
    return CreateBrushes();
}

bool Renderer::CreateBrushes() {
    auto mk = [&](D2D1_COLOR_F c, ID2D1SolidColorBrush** b) {
        return SUCCEEDED(m_rt->CreateSolidColorBrush(c, b));
    };
    return mk({0,0,0,1},             &m_textBrush)
        && mk({.1f,.1f,.8f,1},       &m_linkBrush)
        && mk({.97f,.97f,.97f,1},    &m_bgBrush)
        && mk({.7f,.7f,.7f,1},       &m_hrBrush)
        && mk({.96f,.96f,.96f,1},    &m_codeBgBrush)
        && mk({1.f,.949f,.463f,1},   &m_findBrush)
        && mk({.8f,.8f,.8f,1},       &m_quoteBrush)
        && mk({.91f,.91f,.91f,1},    &m_tabBgBrush)
        && mk({1,1,1,1},             &m_tabActBrush)
        && mk({.82f,.82f,.82f,1},    &m_tabInaBrush)
        && mk({.2f,.2f,.2f,1},       &m_tabTxtBrush)
        && mk({.53f,.53f,.53f,1},    &m_tabClsBrush);
}

void Renderer::ReleaseBrushes() {
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_textBrush); r(m_linkBrush); r(m_bgBrush); r(m_hrBrush);
    r(m_codeBgBrush); r(m_findBrush); r(m_quoteBrush);
    r(m_tabBgBrush); r(m_tabActBrush); r(m_tabInaBrush);
    r(m_tabTxtBrush); r(m_tabClsBrush);
}

void Renderer::ReleaseTarget() {
    ReleaseBrushes();
    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    for (auto& [url, bmp] : m_images) if (bmp) bmp->Release();
    m_images.clear();
    if (m_rt) { m_rt->Release(); m_rt = nullptr; }
}

void Renderer::DiscardTarget() { ReleaseTarget(); }

Renderer::~Renderer() {
    ReleaseTarget();
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_fmtTab);
    for (auto& [k, f] : m_fmtCache) if (f) f->Release();
    m_fmtCache.clear();
    r(m_dwrite); r(m_factory);
}

void Renderer::Resize(UINT w, UINT h) {
    m_width = w; m_height = h;
    if (m_rt) m_rt->Resize(D2D1::SizeU(w, h));
}

void Renderer::SetZoom(float z) {
    m_zoom = std::max(0.5f, std::min(3.f, z));
    // Cached text formats/measurements are size-dependent — invalidate them.
    for (auto& [k, f] : m_fmtCache) if (f) f->Release();
    m_fmtCache.clear();
    m_measureCache.clear();
}

// ─── image loading ────────────────────────────────────────────────────────────

void Renderer::ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes) {
    // Every completion, including fetch and decoder failures, must release the
    // in-flight slot. Otherwise one bad URL is stuck "loading" forever.
    m_loadingImages.erase(url);
    auto fail = [&]() { m_failedImages.insert(url); };
    if (!m_rt || bytes.empty()) { fail(); return; }

    // Decode external SVGs through Helix's own SVG rasterizer before falling
    // back to stb_image for raster formats.
    int w = 0, h = 0, channels = 0;
    unsigned char* stbiPixels = nullptr;
    std::vector<uint8_t> svgPixels;
    uint8_t* pixels = nullptr;
    bool fromStbi = false;
    if (LooksLikeSvgUrl(url) || svg::looksLikeSvgBytes(bytes)) {
        auto bmp = svg::renderSvgBytes(bytes, 2048);
        if (bmp.width > 0 && bmp.height > 0 && !bmp.pixels.empty()) {
            w = bmp.width;
            h = bmp.height;
            svgPixels = std::move(bmp.pixels);
            pixels = svgPixels.data();
        }
    }
    if (!pixels) {
        stbiPixels = stbi_load_from_memory(
            bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);  // force RGBA
        pixels = stbiPixels;
        fromStbi = true;
    }
    if (!pixels || w <= 0 || h <= 0) { if (stbiPixels) stbi_image_free(stbiPixels); fail(); return; }

    // stb_image outputs RGBA; Direct2D wants PBGRA (pre-multiplied, swizzled).
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = pixels + i * 4;
        unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
        float af = a / 255.f;
        p[0] = (unsigned char)(b * af + 0.5f);  // B
        p[1] = (unsigned char)(g * af + 0.5f);  // G
        p[2] = (unsigned char)(r * af + 0.5f);  // R
        p[3] = a;                                 // A
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap* bmp = nullptr;
    HRESULT hr = m_rt->CreateBitmap(
        D2D1::SizeU((UINT32)w, (UINT32)h), pixels, (UINT32)(w * 4), props, &bmp);
    if (fromStbi) stbi_image_free(stbiPixels);

    if (SUCCEEDED(hr) && bmp) {
        auto it = m_images.find(url);
        if (it != m_images.end() && it->second) it->second->Release();
        m_images[url] = bmp;
        InvalidateLayout();
        m_failedImages.erase(url);
    } else {
        fail();
    }
}

// ─── tab strip ───────────────────────────────────────────────────────────────

void Renderer::DrawTabStrip(const std::vector<TabEntry>& tabs, float h) {
    if (!m_rt || tabs.empty() || h < 4.f) return;
    m_tabHits.clear();

    float w      = (float)m_width;
    float newBtnW= h;
    float avail  = w - newBtnW - 4.f;
    int   n      = (int)tabs.size();
    float tabW   = std::min(200.f, std::max(80.f, avail / (float)n));
    float tabH   = h - 2.f;
    float closeW = 18.f, pad = 8.f;
    float x      = 0;

    m_rt->FillRectangle(D2D1::RectF(0, 0, w, h), m_tabBgBrush);

    for (int i = 0; i < n; i++) {
        const auto& t = tabs[i];
        auto* fill = t.active ? m_tabActBrush : m_tabInaBrush;
        m_rt->FillRectangle(D2D1::RectF(x + 1.f, 0, x + tabW - 1.f, tabH), fill);
        if (t.active)
            m_rt->DrawRectangle(D2D1::RectF(x + 1.f, 0, x + tabW - 1.f, tabH), m_hrBrush, 0.5f);

        float textW = tabW - pad * 2 - closeW;
        if (textW > 10.f && m_fmtTab) {
            std::wstring title = t.loading ? L"Loading…" : t.title;
            if (title.empty()) title = L"New Tab";
            m_rt->DrawText(title.c_str(), (UINT32)title.size(), m_fmtTab,
                D2D1::RectF(x + pad, 0, x + pad + textW, tabH),
                m_tabTxtBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        float cx = x + tabW - closeW, cy = (tabH - 14.f) / 2.f;
        if (m_fmtTab)
            m_rt->DrawText(L"×", 1, m_fmtTab,
                D2D1::RectF(cx, 0, cx + closeW, tabH), m_tabClsBrush);

        m_tabHits.push_back({ x, 0, tabW, tabH, i, false });
        m_tabHits.push_back({ cx, cy, closeW, 14.f, i, true });
        x += tabW;
    }

    float nx = avail + 4.f;
    m_rt->FillRectangle(D2D1::RectF(nx, 2.f, nx + newBtnW - 2.f, tabH - 2.f), m_tabInaBrush);
    if (m_fmtTab)
        m_rt->DrawText(L"+", 1, m_fmtTab, D2D1::RectF(nx, 0, nx + newBtnW, tabH), m_tabTxtBrush);
    m_tabHits.push_back({ nx, 0, newBtnW, tabH, -1, false });

    m_rt->DrawLine(D2D1::Point2F(0, h - 1.f), D2D1::Point2F(w, h - 1.f), m_hrBrush, 1.f);
}

int Renderer::HitTestTab(float x, float y) const {
    for (auto it = m_tabHits.rbegin(); it != m_tabHits.rend(); ++it)
        if (!it->isClose && x >= it->x && x < it->x + it->w
                         && y >= it->y && y < it->y + it->h)
            return it->idx;
    return -2;
}

bool Renderer::HitTestTabClose(float x, float y, int& outIdx) const {
    for (auto it = m_tabHits.rbegin(); it != m_tabHits.rend(); ++it)
        if (it->isClose && x >= it->x && x < it->x + it->w
                        && y >= it->y && y < it->y + it->h) {
            outIdx = it->idx; return true;
        }
    return false;
}


// ─── stylesheet collection + body bg ─────────────────────────────────────────

static std::string UrlDecodeSimple(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                c |= 0x20; if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                return 0;
            };
            out += (char)(hex(s[i+1]) * 16 + hex(s[i+2])); i += 2;
        } else { out += s[i]; }
    }
    return out;
}

Stylesheet Renderer::CollectStylesheet(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            std::string css;
            for (auto& c : n->children)
                if (c->type == NodeType::Text) css += c->text;
            Stylesheet part = ParseStylesheet(css);
            for (auto& r : part.rules) sheet.rules.push_back(r);
        } else if (n->type == NodeType::Element && n->tagName == "link") {
            std::string rel = n->attr("rel");
            std::string relLow; for (char c : rel) relLow += (char)std::tolower((unsigned char)c);
            if (relLow.find("stylesheet") != std::string::npos) {
                std::string href = n->attr("href");
                std::string hrefLow; for (char c : href) hrefLow += (char)std::tolower((unsigned char)c);
                const std::string prefix = "data:text/css,";
                if (hrefLow.rfind(prefix, 0) == 0) {
                    std::string css = UrlDecodeSimple(href.substr(prefix.size()));
                    Stylesheet part = ParseStylesheet(css);
                    for (auto& r : part.rules) sheet.rules.push_back(r);
                }
            }
        }
        for (auto& c : n->children) walk(c.get());
    };
    walk(root);
    return sheet;
}

CssColor Renderer::FindBodyBgColor(const Node* root, const Stylesheet& sheet) {
    if (!root) return {};
    std::function<CssColor(const Node*)> find = [&](const Node* n) -> CssColor {
        if (!n) return {};
        if (n->type == NodeType::Element
            && (n->tagName == "body" || n->tagName == "html")) {
            auto cs = sheet.resolve(n);
            if (cs.bgColor.valid) return cs.bgColor;
        }
        for (auto& c : n->children) {
            auto bg = find(c.get());
            if (bg.valid) return bg;
        }
        return {};
    };
    return find(root);
}

// ─── public paint ─────────────────────────────────────────────────────────────

float Renderer::Paint(const std::shared_ptr<Node>& doc,
                      float scrollY,
                      const std::string& baseUrl,
                      float topInset,
                      float tabStripH,
                      const std::vector<TabEntry>* tabs)
{
    if (!EnsureTarget()) return 0.f;

    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    m_hits.clear();
    // m_anchorY is rebuilt only when the layout tree is rebuilt (see below).

    m_rt->BeginDraw();

    Stylesheet sheet;
    CssColor   pageBg;
    if (doc) {
        sheet  = CollectStylesheet(doc.get());
        pageBg = FindBodyBgColor(doc.get(), sheet);
        // Load @font-face web fonts.
        if (!sheet.fontFaces.empty())
            WebFontLoader::instance().loadFonts(sheet, baseUrl, [this]() { InvalidateLayout(); });
    }

    // transparent body bg → default white (HwndRenderTarget clear with a=0 shows window black)
    D2D1_COLOR_F bgF = (pageBg.valid && pageBg.a > 0.001f)
        ? ToD2D(pageBg)
        : D2D1::ColorF(1.f, 1.f, 1.f);
    m_rt->Clear(bgF);

    // Chrome area (toolbar + tab strip)
    if (topInset > 0)
        m_rt->FillRectangle(D2D1::RectF(0, 0, (float)m_width, topInset), m_bgBrush);

    if (tabs && !tabs->empty() && tabStripH > 0)
        DrawTabStrip(*tabs, tabStripH);

    if (doc) {
        if (m_imageDocKey != doc.get()) {
            m_imageDocKey = doc.get();
            m_failedImages.clear();
        }
        m_curBaseUrl = baseUrl;
        float docH = 0.f;
        // A malformed or pathological page must never take down the browser.
        try {
            // The render target draws in physical pixels (pinned to 96 DPI), and
            // the layout works in those same pixels. To make content render at the
            // right physical size on a high-DPI display (instead of tiny), fold the
            // monitor's DPI scale into the effective zoom: a 125% display lays out
            // 25% larger. Centering still holds because layout + paint share one
            // coordinate space (physical px).
            UINT dpi = m_hwnd ? GetDpiForWindow(m_hwnd) : 96;
            float dpiScale = (dpi >= 48) ? (float)dpi / 96.f : 1.f;
            float effZoom = m_zoom * dpiScale;
            // Rebuild the layout tree only when something that affects geometry
            // changed; scrolling reuses the cached tree.
            extern const Node* g_hoverNode;
            static const Node* prevHover = nullptr;
            bool hoverChanged = (g_hoverNode != prevHover);
            std::map<const Node*, ComputedStyle> oldStyles;
            if (hoverChanged && m_layoutRoot) {
                std::function<void(const LayoutBox&)> collect = [&](const LayoutBox& b) {
                    if (b.node && oldStyles.find(b.node) == oldStyles.end())
                        oldStyles.emplace(b.node, b.style);
                    for (const auto& k : b.kids) collect(*k);
                };
                collect(*m_layoutRoot);
            }
            SetCssHoverNode(g_hoverNode);
            bool reuse = m_layoutRoot
                      && m_layoutDocKey  == doc.get()
                      && m_layoutWKey    == m_width
                      && m_layoutHKey    == m_height
                      && m_layoutZoomKey == effZoom
                      && !hoverChanged;
            if (!reuse) {
                LayoutInput in;
                in.document  = doc.get();
                in.sheet     = &sheet;
                in.measure   = this;
                in.viewportW = std::max(100.f, (float)m_width);
                in.viewportH = std::max(100.f, (float)m_height - topInset);
                in.zoom      = effZoom;
                in.baseUrl   = baseUrl;
                m_layoutRoot   = LayoutDocument(in);
                m_layoutDocKey = doc.get();
                m_layoutWKey   = m_width;
                m_layoutHKey   = m_height;
                m_layoutZoomKey= effZoom;
                m_anchorY.clear();
                if (m_layoutRoot) CollectAnchors(*m_layoutRoot);
            }
            if (hoverChanged && m_layoutRoot && !oldStyles.empty()) {
                std::set<const Node*> transitioned;
                std::function<void(const LayoutBox&)> startTransitions = [&](const LayoutBox& b) {
                    if (b.node && transitioned.insert(b.node).second) {
                        auto it = oldStyles.find(b.node);
                        if (it != oldStyles.end())
                            TransitionManager::instance().onStyleChange(b.node, it->second, b.style);
                    }
                    for (const auto& k : b.kids) startTransitions(*k);
                };
                startTransitions(*m_layoutRoot);
            }
            prevHover = g_hoverNode;
            if (m_layoutRoot) {
                PaintBox(*m_layoutRoot, scrollY, topInset, false);
                docH = m_layoutRoot->contentH + 32.f;
                // If transitions are active, schedule another repaint.
                if (TransitionManager::instance().hasActiveTransitions() && m_hwnd)
                    InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        } catch (const std::exception& ex) {
            FILE* f = fopen("C:/tmp/helix_crash.txt", "a");
            if (f) { fprintf(f, "LAYOUT/PAINT exception: %s\n", ex.what()); fclose(f); }
        } catch (...) {
            FILE* f = fopen("C:/tmp/helix_crash.txt", "a");
            if (f) { fprintf(f, "LAYOUT/PAINT unknown exception\n"); fclose(f); }
        }

        HRESULT hr = m_rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) ReleaseTarget();
        return docH;
    }

    m_rt->EndDraw();
    return 0.f;
}

std::string Renderer::HitTest(float x, float y) const {
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w
         && y >= it->y && y <= it->y + it->h)
            return UnwrapBingRedirect(it->href);
    return {};
}

bool Renderer::GetAnchorY(const std::string& anchor, float& outY) const {
    auto it = m_anchorY.find(anchor);
    if (it == m_anchorY.end()) return false;
    outY = it->second;
    return true;
}

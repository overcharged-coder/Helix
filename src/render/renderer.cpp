#include "render/renderer.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <sstream>
#include <algorithm>
#include <functional>

// ─── helpers ─────────────────────────────────────────────────────────────────

static D2D1_COLOR_F ToD2D(const CssColor& c) { return { c.r, c.g, c.b, c.a }; }

std::wstring Renderer::ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Renderer::ResolveUrl(const std::string& href, const std::string& base) {
    if (href.empty()) return base;
    if (href.find("://") != std::string::npos) return href;
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

IDWriteTextFormat* Renderer::TempFormat(float size, bool bold, bool mono, bool italic) {
    IDWriteTextFormat* f = nullptr;
    if (!m_dwrite) return nullptr;
    m_dwrite->CreateTextFormat(
        mono ? L"Consolas" : L"Segoe UI", nullptr,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        size * m_zoom, L"en-us", &f);
    if (f) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_tempFormats.push_back(f);
    }
    return f;
}

// ─── init / teardown ─────────────────────────────────────────────────────────

void Renderer::RecreateFormats() {
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_fmtBody); r(m_fmtBold); r(m_fmtItalic);
    r(m_fmtCode); r(m_fmtH1);   r(m_fmtH2); r(m_fmtH3);

    auto mkFmt = [&](float size, bool bold, bool mono, bool italic) -> IDWriteTextFormat* {
        IDWriteTextFormat* f = nullptr;
        if (!m_dwrite) return nullptr;
        m_dwrite->CreateTextFormat(
            mono ? L"Consolas" : L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size * m_zoom, L"en-us", &f);
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    };
    m_fmtBody   = mkFmt(15.f, false, false, false);
    m_fmtBold   = mkFmt(15.f, true,  false, false);
    m_fmtItalic = mkFmt(15.f, false, false, true);
    m_fmtCode   = mkFmt(13.f, false, true,  false);
    m_fmtH1     = mkFmt(32.f, true,  false, false);
    m_fmtH2     = mkFmt(26.f, true,  false, false);
    m_fmtH3     = mkFmt(20.f, true,  false, false);
}

bool Renderer::Init(HWND hwnd) {
    m_hwnd = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_factory)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_dwrite))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_wic));
    RecreateFormats();
    return EnsureTarget();
}

bool Renderer::EnsureTarget() {
    if (m_rt) return true;
    RECT rc; GetClientRect(m_hwnd, &rc);
    m_width  = (UINT)(rc.right  - rc.left);
    m_height = (UINT)(rc.bottom - rc.top);
    if (FAILED(m_factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(m_width, m_height)),
            &m_rt)))
        return false;
    return CreateBrushes();
}

bool Renderer::CreateBrushes() {
    auto mk = [&](D2D1_COLOR_F c, ID2D1SolidColorBrush** b) {
        return SUCCEEDED(m_rt->CreateSolidColorBrush(c, b));
    };
    return mk({0,0,0,1},          &m_textBrush)
        && mk({.1f,.1f,.8f,1},    &m_linkBrush)
        && mk({.97f,.97f,.97f,1}, &m_bgBrush)
        && mk({.7f,.7f,.7f,1},    &m_hrBrush);
}

void Renderer::ReleaseBrushes() {
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_textBrush); r(m_linkBrush); r(m_bgBrush); r(m_hrBrush);
}

void Renderer::ReleaseTarget() {
    ReleaseBrushes();
    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    for (auto* f : m_tempFormats) if (f) f->Release();
    m_tempFormats.clear();
    for (auto& [url, bmp] : m_images) if (bmp) bmp->Release();
    m_images.clear();
    if (m_rt) { m_rt->Release(); m_rt = nullptr; }
}

void Renderer::DiscardTarget() { ReleaseTarget(); }

Renderer::~Renderer() {
    ReleaseTarget();
    auto r = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    r(m_fmtBody); r(m_fmtBold); r(m_fmtItalic);
    r(m_fmtCode); r(m_fmtH1);   r(m_fmtH2); r(m_fmtH3);
    r(m_dwrite); r(m_wic); r(m_factory);
}

void Renderer::Resize(UINT w, UINT h) {
    m_width = w; m_height = h;
    if (m_rt) m_rt->Resize(D2D1::SizeU(w, h));
}

void Renderer::SetZoom(float z) {
    m_zoom = std::max(0.5f, std::min(3.f, z));
    if (m_dwrite) RecreateFormats();
}

// ─── image loading ────────────────────────────────────────────────────────────

void Renderer::ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes) {
    if (!m_wic || !m_rt || bytes.empty()) return;

    IWICStream* stream = nullptr;
    if (FAILED(m_wic->CreateStream(&stream))) return;
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size()))) {
        stream->Release(); return;
    }
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(m_wic->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) {
        stream->Release(); return;
    }
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(dec->GetFrame(0, &frame))) {
        dec->Release(); stream->Release(); return;
    }
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(m_wic->CreateFormatConverter(&conv))) {
        if (SUCCEEDED(conv->Initialize(frame,
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0,
                WICBitmapPaletteTypeCustom))) {
            ID2D1Bitmap* bmp = nullptr;
            if (SUCCEEDED(m_rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp))) {
                auto it = m_images.find(url);
                if (it != m_images.end() && it->second) it->second->Release();
                m_images[url] = bmp;
            }
        }
        conv->Release();
    }
    frame->Release(); dec->Release(); stream->Release();
    m_loadingImages.erase(url);
}

// ─── text layout ─────────────────────────────────────────────────────────────

IDWriteTextFormat* Renderer::FormatFor(const PaintCtx& ctx) const {
    if (ctx.fmtOverride)       return ctx.fmtOverride;
    if (ctx.headingLevel == 1) return m_fmtH1;
    if (ctx.headingLevel == 2) return m_fmtH2;
    if (ctx.headingLevel >= 3) return m_fmtH3;
    if (ctx.isCode)            return m_fmtCode;
    if (ctx.bold)              return m_fmtBold;
    if (ctx.italic)            return m_fmtItalic;
    return m_fmtBody;
}

float Renderer::DrawWrappedText(const std::wstring& text,
                                float x, float y,
                                float maxW,
                                IDWriteTextFormat* fmt,
                                ID2D1SolidColorBrush* brush,
                                bool underline,
                                const std::string& href,
                                float scrollY,
                                float topInset)
{
    if (text.empty() || !fmt || !m_dwrite) return y;

    float lineH  = fmt->GetFontSize() * 1.45f;
    float spaceW = fmt->GetFontSize() * 0.3f;

    {
        IDWriteTextLayout* sl = nullptr;
        if (SUCCEEDED(m_dwrite->CreateTextLayout(L" ", 1, fmt, 10000, 10000, &sl))) {
            DWRITE_TEXT_METRICS m{}; sl->GetMetrics(&m);
            if (m.width > 0) spaceW = m.width;
            sl->Release();
        }
    }

    std::vector<std::wstring> words;
    std::wistringstream ss(text);
    std::wstring w;
    while (ss >> w) words.push_back(w);
    if (words.empty()) return y;

    float cx = x;
    for (auto& word : words) {
        float ww = (float)word.size() * fmt->GetFontSize() * 0.55f;
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(m_dwrite->CreateTextLayout(
                word.c_str(), (UINT32)word.size(), fmt, 10000, 10000, &lay))) {
            DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m);
            ww = m.width;
            lay->Release();
        }

        if (cx > x && cx + ww > x + maxW) { cx = x; y += lineH; }

        float sy = y - scrollY + topInset;
        if (sy + lineH >= topInset && sy < (float)m_height) {
            IDWriteTextLayout* dl = nullptr;
            if (SUCCEEDED(m_dwrite->CreateTextLayout(
                    word.c_str(), (UINT32)word.size(), fmt, maxW, lineH * 2, &dl))) {
                if (underline) {
                    DWRITE_TEXT_RANGE all{ 0, (UINT32)word.size() };
                    dl->SetUnderline(TRUE, all);
                }
                m_rt->DrawTextLayout(D2D1::Point2F(cx, sy), dl, brush,
                    D2D1_DRAW_TEXT_OPTIONS_NONE);
                dl->Release();
            }
        }

        if (!href.empty()) {
            float sy2 = y - scrollY + topInset;
            m_hits.push_back({ cx, sy2, ww, lineH, href });
        }

        cx += ww + spaceW;
    }
    return y + lineH;
}

// ─── stylesheet collection ────────────────────────────────────────────────────

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
        }
        for (auto& c : n->children) walk(c.get());
    };
    walk(root);
    return sheet;
}

// ─── DOM walker ───────────────────────────────────────────────────────────────

static constexpr float kMarginX = 32.f;
static constexpr float kMarginY =  8.f;

float Renderer::WalkNode(const Node* node, PaintCtx& ctx) {
    if (!node) return ctx.y;

    // ── text node ─────────────────────────────────────────────────────────
    if (node->type == NodeType::Text) {
        if (node->text.empty()) return ctx.y;
        auto* fmt   = FormatFor(ctx);
        auto* brush = ctx.colorOverride
                        ? ctx.colorOverride
                        : (ctx.isLink ? m_linkBrush : m_textBrush);
        ctx.y = DrawWrappedText(
            ToWide(node->text),
            kMarginX, ctx.y, ctx.contentW,
            fmt, brush, ctx.isLink,
            ctx.isLink ? ctx.linkHref : "",
            ctx.scrollY, ctx.topInset);
        return ctx.y;
    }

    const std::string& tag = node->tagName;

    // ── CSS cascade ────────────────────────────────────────────────────────
    ComputedStyle cs;
    if (ctx.sheet) cs = ctx.sheet->resolve(node);
    if (cs.displayNone) return ctx.y;

    auto* prevColor  = ctx.colorOverride;
    auto* prevFmt    = ctx.fmtOverride;
    bool  prevBold   = ctx.bold;
    bool  prevItalic = ctx.italic;

    if (cs.color.valid)   ctx.colorOverride = TempBrush(ToD2D(cs.color));
    if (cs.boldSet)       ctx.bold   = cs.bold;
    if (cs.italicSet)     ctx.italic = cs.italic;
    if (cs.fontSize > 0)  ctx.fmtOverride = TempFormat(cs.fontSize, ctx.bold, ctx.isCode, ctx.italic);

    auto walkChildren = [&]() {
        for (auto& c : node->children) WalkNode(c.get(), ctx);
    };

    // ── inline elements ───────────────────────────────────────────────────
    if (tag == "a") {
        bool was = ctx.isLink; std::string wasHref = ctx.linkHref;
        ctx.isLink   = true;
        ctx.linkHref = ResolveUrl(node->attr("href"), ctx.baseUrl);
        walkChildren();
        ctx.isLink = was; ctx.linkHref = wasHref;
        goto restore;
    }
    if (tag == "strong" || tag == "b") {
        bool was = ctx.bold; ctx.bold = true;
        walkChildren();
        ctx.bold = was;
        goto restore;
    }
    if (tag == "em" || tag == "i" || tag == "cite") {
        bool was = ctx.italic; ctx.italic = true;
        walkChildren();
        ctx.italic = was;
        goto restore;
    }
    if (tag == "code" || tag == "tt" || tag == "kbd" || tag == "samp") {
        bool was = ctx.isCode; ctx.isCode = true;
        walkChildren();
        ctx.isCode = was;
        goto restore;
    }
    if (tag == "span"  || tag == "small" || tag == "abbr" || tag == "time"
     || tag == "mark"  || tag == "sup"   || tag == "sub"  || tag == "label") {
        walkChildren(); goto restore;
    }
    if (tag == "br") {
        ctx.y += FormatFor(ctx)->GetFontSize() * 1.45f;
        goto restore;
    }
    if (tag == "img") {
        std::string src = ResolveUrl(node->attr("src"), ctx.baseUrl);
        std::string alt = node->attr("alt");
        auto it = m_images.find(src);
        if (it != m_images.end() && it->second) {
            D2D1_SIZE_F bmpSz = it->second->GetSize();
            float scale = std::min(ctx.contentW / bmpSz.width, 1.f);
            float dw = bmpSz.width * scale, dh = bmpSz.height * scale;
            float sy = ctx.y - ctx.scrollY + ctx.topInset;
            if (sy + dh >= ctx.topInset && sy < ctx.winH)
                m_rt->DrawBitmap(it->second,
                    D2D1::RectF(kMarginX, sy, kMarginX + dw, sy + dh));
            ctx.y += dh + kMarginY;
        } else {
            if (!src.empty() && !m_loadingImages.count(src)) {
                m_loadingImages.insert(src);
                if (m_imageRequestCb) m_imageRequestCb(src);
            }
            std::wstring ph = ToWide(alt.empty() ? "[image]" : "[" + alt + "]");
            ctx.y = DrawWrappedText(ph, kMarginX, ctx.y, ctx.contentW,
                m_fmtBody, m_hrBrush, false, "", ctx.scrollY, ctx.topInset);
        }
        goto restore;
    }

    // ── skip non-visual elements ──────────────────────────────────────────
    if (tag == "head" || tag == "script" || tag == "style"
     || tag == "noscript" || tag == "svg" || tag == "canvas"
     || tag == "iframe" || tag == "template" || tag == "meta"
     || tag == "link"  || tag == "title") {
        goto restore;
    }

    // ── headings ──────────────────────────────────────────────────────────
    if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        ctx.y += kMarginY * 1.5f;
        int was = ctx.headingLevel;
        ctx.headingLevel = tag[1] - '0';
        walkChildren();
        ctx.headingLevel = was;
        ctx.y += kMarginY * 0.5f;
        goto restore;
    }

    // ── hr ────────────────────────────────────────────────────────────────
    if (tag == "hr") {
        ctx.y += kMarginY;
        float sy = ctx.y - ctx.scrollY + ctx.topInset;
        if (sy >= ctx.topInset && sy < ctx.winH)
            m_rt->DrawLine(
                D2D1::Point2F(kMarginX, sy),
                D2D1::Point2F(kMarginX + ctx.contentW, sy),
                m_hrBrush, 1.f);
        ctx.y += kMarginY;
        goto restore;
    }

    // ── list item ─────────────────────────────────────────────────────────
    if (tag == "li") {
        ctx.y += 2.f;
        float sy = ctx.y - ctx.scrollY + ctx.topInset;
        if (sy >= ctx.topInset && sy < ctx.winH)
            m_rt->DrawText(L"• ", 2, m_fmtBody,
                D2D1::RectF(kMarginX, sy, kMarginX + 20.f, sy + 22.f), m_textBrush);
        walkChildren();
        ctx.y += 2.f;
        goto restore;
    }

    // ── block elements ────────────────────────────────────────────────────
    {
        bool isBlock = (
            tag == "p"       || tag == "div"     || tag == "section"  || tag == "article"
         || tag == "main"    || tag == "header"  || tag == "aside"
         || tag == "ul"      || tag == "ol"      || tag == "dl"
         || tag == "blockquote" || tag == "pre"  || tag == "table"
         || tag == "tr"      || tag == "td"      || tag == "th"
         || tag == "figure"  || tag == "figcaption" || tag == "footer"
         || tag == "nav"     || tag == "form"    || tag == "fieldset"
         || tag == "body"    || tag == "#document" || tag == "html"
         || tag == "details" || tag == "summary"
        );
        bool notRoot = (tag != "body" && tag != "#document" && tag != "html");

        if (isBlock && notRoot) {
            ctx.y += (cs.marginTop >= 0) ? cs.marginTop : kMarginY * 0.5f;
        }

        walkChildren();

        if (isBlock && notRoot) {
            ctx.y += (cs.marginBottom >= 0) ? cs.marginBottom : kMarginY * 0.5f;
        }
    }

restore:
    ctx.colorOverride = prevColor;
    ctx.fmtOverride   = prevFmt;
    ctx.bold          = prevBold;
    ctx.italic        = prevItalic;
    return ctx.y;
}

// ─── public paint ─────────────────────────────────────────────────────────────

float Renderer::Paint(const std::shared_ptr<Node>& doc,
                      float scrollY,
                      const std::string& baseUrl,
                      float topInset)
{
    if (!EnsureTarget()) return 0.f;

    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    for (auto* f : m_tempFormats) if (f) f->Release();
    m_tempFormats.clear();

    m_hits.clear();

    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0.97f, 0.97f, 0.97f));

    if (topInset > 0)
        m_rt->FillRectangle(D2D1::RectF(0, 0, (float)m_width, topInset), m_bgBrush);

    if (doc) {
        Stylesheet sheet = CollectStylesheet(doc.get());

        PaintCtx ctx;
        ctx.y        = 16.f;
        ctx.contentW = std::max(100.f, (float)m_width - kMarginX * 2.f);
        ctx.scrollY  = scrollY;
        ctx.winH     = (float)m_height;
        ctx.topInset = topInset;
        ctx.baseUrl  = baseUrl;
        ctx.sheet    = &sheet;
        WalkNode(doc.get(), ctx);

        HRESULT hr = m_rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) ReleaseTarget();
        return ctx.y + 32.f;
    }

    m_rt->EndDraw();
    return 0.f;
}

std::string Renderer::HitTest(float x, float y) const {
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w
         && y >= it->y && y <= it->y + it->h)
            return it->href;
    return {};
}

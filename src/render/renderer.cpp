#include "render/renderer.h"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>

// ─── CSS color parsing ───────────────────────────────────────────────────────

static std::string strLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string strTrim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    return s;
}

static const std::map<std::string, D2D1_COLOR_F>& NamedColors() {
    static std::map<std::string, D2D1_COLOR_F> m = {
        {"black",      {0,0,0,1}},       {"white",    {1,1,1,1}},
        {"red",        {1,0,0,1}},       {"green",    {0,.502f,0,1}},
        {"blue",       {0,0,1,1}},       {"yellow",   {1,1,0,1}},
        {"orange",     {1,.647f,0,1}},   {"purple",   {.502f,0,.502f,1}},
        {"pink",       {1,.753f,.796f,1}},{"cyan",    {0,1,1,1}},
        {"magenta",    {1,0,1,1}},       {"gray",     {.502f,.502f,.502f,1}},
        {"grey",       {.502f,.502f,.502f,1}}, {"silver",{.753f,.753f,.753f,1}},
        {"teal",       {0,.502f,.502f,1}},{"navy",    {0,0,.502f,1}},
        {"maroon",     {.502f,0,0,1}},   {"olive",    {.502f,.502f,0,1}},
        {"lime",       {0,1,0,1}},       {"aqua",     {0,1,1,1}},
        {"fuchsia",    {1,0,1,1}},       {"brown",    {.647f,.165f,.165f,1}},
        {"coral",      {1,.498f,.314f,1}},{"crimson", {.863f,.078f,.235f,1}},
        {"darkblue",   {0,0,.545f,1}},   {"darkgreen",{0,.392f,0,1}},
        {"darkred",    {.545f,0,0,1}},   {"gold",     {1,.843f,0,1}},
        {"hotpink",    {1,.412f,.706f,1}},{"indigo",  {.294f,0,.510f,1}},
        {"lavender",   {.902f,.902f,.980f,1}},
        {"lightblue",  {.678f,.847f,.902f,1}},
        {"lightgray",  {.827f,.827f,.827f,1}},
        {"lightgrey",  {.827f,.827f,.827f,1}},
        {"lightgreen", {.565f,.933f,.565f,1}},
        {"salmon",     {.980f,.502f,.447f,1}},
        {"skyblue",    {.529f,.808f,.922f,1}},
        {"tomato",     {1,.388f,.278f,1}},
        {"transparent",{0,0,0,0}},
        {"violet",     {.933f,.510f,.933f,1}},
        {"deepskyblue",{0,.749f,1,1}},
        {"dimgray",    {.412f,.412f,.412f,1}},
        {"dimgrey",    {.412f,.412f,.412f,1}},
    };
    return m;
}

CssColor ParseCssColor(const std::string& raw) {
    std::string s = strLower(strTrim(raw));
    CssColor out;
    if (s.empty() || s == "inherit" || s == "initial" || s == "currentcolor")
        return out;

    // Named
    auto& nm = NamedColors();
    auto it = nm.find(s);
    if (it != nm.end()) {
        auto& c = it->second;
        out.valid = true; out.r = c.r; out.g = c.g; out.b = c.b; out.a = c.a;
        return out;
    }

    // #rgb / #rrggbb / #rrggbbaa
    if (!s.empty() && s[0] == '#') {
        std::string h = s.substr(1);
        if (h.size() == 3 || h.size() == 4) {
            std::string exp;
            for (char c : h) exp += { c, c };
            h = exp;
        }
        if (h.size() >= 6) {
            try {
                out.r = std::stoi(h.substr(0,2),nullptr,16) / 255.f;
                out.g = std::stoi(h.substr(2,2),nullptr,16) / 255.f;
                out.b = std::stoi(h.substr(4,2),nullptr,16) / 255.f;
                out.a = h.size() >= 8 ? std::stoi(h.substr(6,2),nullptr,16)/255.f : 1.f;
                out.valid = true;
            } catch (...) {}
        }
        return out;
    }

    // rgb(...) / rgba(...)
    if (s.substr(0,4) == "rgb(") {
        size_t end = s.find(')');
        if (end != std::string::npos) {
            std::istringstream ss(s.substr(4, end - 4));
            std::string tok; std::vector<float> vals;
            while (std::getline(ss, tok, ','))
                try { vals.push_back(std::stof(strTrim(tok))); } catch (...) {}
            if (vals.size() >= 3) {
                out.r = vals[0]/255.f; out.g = vals[1]/255.f; out.b = vals[2]/255.f;
                out.a = vals.size() >= 4 ? vals[3] : 1.f;
                out.valid = true;
            }
        }
    }
    return out;
}

// ─── helpers ────────────────────────────────────────────────────────────────

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

// ─── init / teardown ────────────────────────────────────────────────────────

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

    auto mkFmt = [&](float size, bool bold, bool mono) -> IDWriteTextFormat* {
        IDWriteTextFormat* f = nullptr;
        m_dwrite->CreateTextFormat(
            mono ? L"Consolas" : L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"en-us", &f);
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    };
    m_fmtBody = mkFmt(15.f, false, false);
    m_fmtBold = mkFmt(15.f, true,  false);
    m_fmtCode = mkFmt(13.f, false, true);
    m_fmtH1   = mkFmt(32.f, true,  false);
    m_fmtH2   = mkFmt(26.f, true,  false);
    m_fmtH3   = mkFmt(20.f, true,  false);

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
    return mk({0,0,0,1},             &m_textBrush)
        && mk({.1f,.1f,.8f,1},       &m_linkBrush)
        && mk({.97f,.97f,.97f,1},    &m_bgBrush)
        && mk({.7f,.7f,.7f,1},       &m_hrBrush);
}

void Renderer::ReleaseBrushes() {
    auto r = [](auto*& p){ if(p){ p->Release(); p=nullptr; } };
    r(m_textBrush); r(m_linkBrush); r(m_bgBrush); r(m_hrBrush);
}

void Renderer::ReleaseTarget() {
    ReleaseBrushes();
    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();
    for (auto& [url, bmp] : m_images) if (bmp) bmp->Release();
    m_images.clear();
    // Keep m_loadingImages so we don't re-request immediately
    if (m_rt) { m_rt->Release(); m_rt = nullptr; }
}

void Renderer::DiscardTarget() { ReleaseTarget(); }

Renderer::~Renderer() {
    ReleaseTarget();
    auto r = [](auto*& p){ if(p){ p->Release(); p=nullptr; } };
    r(m_fmtBody); r(m_fmtBold); r(m_fmtCode);
    r(m_fmtH1);   r(m_fmtH2);   r(m_fmtH3);
    r(m_dwrite);  r(m_wic);     r(m_factory);
}

void Renderer::Resize(UINT w, UINT h) {
    m_width = w; m_height = h;
    if (m_rt) m_rt->Resize(D2D1::SizeU(w, h));
}

// ─── image loading ───────────────────────────────────────────────────────────

void Renderer::ReceiveImage(const std::string& url,
                            const std::vector<uint8_t>& bytes) {
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

// ─── text layout ────────────────────────────────────────────────────────────

IDWriteTextFormat* Renderer::FormatFor(const PaintCtx& ctx) const {
    if (ctx.headingLevel == 1) return m_fmtH1;
    if (ctx.headingLevel == 2) return m_fmtH2;
    if (ctx.headingLevel >= 3) return m_fmtH3;
    if (ctx.isCode)            return m_fmtCode;
    if (ctx.bold)              return m_fmtBold;
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

    float lineH   = fmt->GetFontSize() * 1.45f;
    float spaceW  = fmt->GetFontSize() * 0.3f;

    // Measure space width properly
    {
        IDWriteTextLayout* sl = nullptr;
        if (SUCCEEDED(m_dwrite->CreateTextLayout(L" ", 1, fmt, 10000, 10000, &sl))) {
            DWRITE_TEXT_METRICS m{}; sl->GetMetrics(&m);
            if (m.width > 0) spaceW = m.width;
            sl->Release();
        }
    }

    // Split into words
    std::vector<std::wstring> words;
    std::wistringstream ss(text);
    std::wstring w;
    while (ss >> w) words.push_back(w);
    if (words.empty()) return y;

    float cx = x;
    for (auto& word : words) {
        // Measure word
        float ww = (float)word.size() * fmt->GetFontSize() * 0.55f; // approximate fallback
        IDWriteTextLayout* lay = nullptr;
        if (SUCCEEDED(m_dwrite->CreateTextLayout(
                word.c_str(), (UINT32)word.size(), fmt, 10000, 10000, &lay))) {
            DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m);
            ww = m.width;
            lay->Release();
        }

        // Line wrap
        if (cx > x && cx + ww > x + maxW) { cx = x; y += lineH; }

        // Screen Y = doc Y - scrollY + topInset
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

        // Hit region in physical window coordinates
        if (!href.empty()) {
            float sy2 = y - scrollY + topInset;
            m_hits.push_back({ cx, sy2, ww, lineH, href });
        }

        cx += ww + spaceW;
    }
    return y + lineH;
}

// ─── inline CSS parsing ──────────────────────────────────────────────────────

struct ParsedStyle {
    CssColor color, bgColor;
    float    fontSize   = 0;  // 0 = inherit
    int      fontWeight = 0;  // 0=inherit, 1=normal, 2=bold
};

static ParsedStyle ParseInlineStyle(const std::string& style) {
    ParsedStyle ps;
    std::istringstream ss(style);
    std::string decl;
    while (std::getline(ss, decl, ';')) {
        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string prop = strLower(strTrim(decl.substr(0, colon)));
        std::string val  = strTrim(decl.substr(colon + 1));

        if (prop == "color")
            ps.color = ParseCssColor(val);
        else if (prop == "background-color" || prop == "background")
            ps.bgColor = ParseCssColor(val);
        else if (prop == "font-size") {
            std::string v = strLower(val);
            if (v == "small")    ps.fontSize = 12;
            else if (v == "medium")   ps.fontSize = 15;
            else if (v == "large")    ps.fontSize = 18;
            else if (v == "x-large")  ps.fontSize = 22;
            else if (v == "xx-large") ps.fontSize = 28;
            else {
                try {
                    ps.fontSize = std::stof(v);  // strip units — px assumed
                } catch (...) {}
            }
        }
        else if (prop == "font-weight") {
            std::string v = strLower(val);
            if (v == "bold" || v == "700" || v == "800" || v == "900")
                ps.fontWeight = 2;
            else if (v == "normal" || v == "400")
                ps.fontWeight = 1;
        }
    }
    return ps;
}

// ─── DOM walker ─────────────────────────────────────────────────────────────

static constexpr float kMarginX = 32.f;
static constexpr float kMarginY =  8.f;

float Renderer::WalkNode(const Node* node, PaintCtx& ctx) {
    if (!node) return ctx.y;

    // ── text node ────────────────────────────────────────────────────────
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

    // ── apply inline CSS from style="" ───────────────────────────────────
    auto styleAttr = node->attr("style");
    ParsedStyle css;
    if (!styleAttr.empty()) css = ParseInlineStyle(styleAttr);

    auto* prevColor = ctx.colorOverride;
    auto* prevBrush = ctx.colorOverride;

    if (css.color.valid)
        ctx.colorOverride = TempBrush(css.color.toD2D());

    bool prevBold = ctx.bold;
    if (css.fontWeight == 2) ctx.bold = true;
    if (css.fontWeight == 1) ctx.bold = false;

    // ── inline elements ──────────────────────────────────────────────────
    auto walkChildren = [&]() {
        for (auto& c : node->children) WalkNode(c.get(), ctx);
    };

    if (tag == "a") {
        bool wasLink = ctx.isLink; std::string wasHref = ctx.linkHref;
        ctx.isLink   = true;
        ctx.linkHref = ResolveUrl(node->attr("href"), ctx.baseUrl);
        if (!ctx.colorOverride) ctx.colorOverride = nullptr; // let link brush show
        walkChildren();
        ctx.isLink = wasLink; ctx.linkHref = wasHref;
        goto restore;
    }
    if (tag == "strong" || tag == "b") {
        bool was = ctx.bold; ctx.bold = true;
        walkChildren();
        ctx.bold = was;
        goto restore;
    }
    if (tag == "em" || tag == "i" || tag == "cite") {
        walkChildren(); goto restore;
    }
    if (tag == "code" || tag == "tt" || tag == "kbd" || tag == "samp") {
        bool was = ctx.isCode; ctx.isCode = true;
        walkChildren();
        ctx.isCode = was;
        goto restore;
    }
    if (tag == "span" || tag == "small" || tag == "abbr" || tag == "time"
        || tag == "mark" || tag == "sup" || tag == "sub" || tag == "label") {
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
            if (sy + dh >= ctx.topInset && sy < ctx.winH) {
                m_rt->DrawBitmap(it->second,
                    D2D1::RectF(kMarginX, sy, kMarginX + dw, sy + dh));
            }
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

    // ── skip invisible / layout-irrelevant elements ───────────────────────
    if (tag == "head" || tag == "script" || tag == "style"
        || tag == "noscript" || tag == "svg" || tag == "canvas"
        || tag == "iframe" || tag == "template" || tag == "meta"
        || tag == "link"   || tag == "title") {
        goto restore;
    }

    // ── headings ─────────────────────────────────────────────────────────
    if (tag.size() == 2 && tag[0] == 'h'
        && tag[1] >= '1' && tag[1] <= '6') {
        ctx.y += kMarginY * 1.5f;
        int was = ctx.headingLevel;
        ctx.headingLevel = tag[1] - '0';
        walkChildren();
        ctx.headingLevel = was;
        ctx.y += kMarginY * 0.5f;
        goto restore;
    }

    // ── hr ───────────────────────────────────────────────────────────────
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

    // ── list item ────────────────────────────────────────────────────────
    if (tag == "li") {
        ctx.y += 2.f;
        float sy = ctx.y - ctx.scrollY + ctx.topInset;
        if (sy >= ctx.topInset && sy < ctx.winH) {
            m_rt->DrawText(L"• ", 2, m_fmtBody,
                D2D1::RectF(kMarginX, sy, kMarginX + 20.f, sy + 22.f),
                m_textBrush);
        }
        float savedX = kMarginX;
        // Indent text inside li
        (void)savedX;
        walkChildren();
        ctx.y += 2.f;
        goto restore;
    }

    // ── block elements ────────────────────────────────────────────────────
    {
        bool isBlock = (
            tag == "p" || tag == "div" || tag == "section" || tag == "article"
            || tag == "main" || tag == "header" || tag == "aside"
            || tag == "ul" || tag == "ol" || tag == "dl"
            || tag == "blockquote" || tag == "pre" || tag == "table"
            || tag == "tr" || tag == "td" || tag == "th"
            || tag == "figure" || tag == "figcaption" || tag == "footer"
            || tag == "nav" || tag == "form" || tag == "fieldset"
            || tag == "body" || tag == "#document" || tag == "html"
            || tag == "details" || tag == "summary"
        );

        // Draw background if specified
        if (css.bgColor.valid) {
            // We don't know height yet, but draw a rect over the children area
            // We'll approximate by just setting the bg before children
            // (full background rects require two passes; skip for now)
        }

        if (isBlock
            && tag != "body" && tag != "#document" && tag != "html")
            ctx.y += kMarginY * 0.5f;

        walkChildren();

        if (isBlock
            && tag != "body" && tag != "#document" && tag != "html")
            ctx.y += kMarginY * 0.5f;
    }

restore:
    ctx.colorOverride = prevColor;
    ctx.bold = prevBold;
    return ctx.y;
}

// ─── public paint ────────────────────────────────────────────────────────────

float Renderer::Paint(const std::shared_ptr<Node>& doc,
                      float scrollY,
                      const std::string& baseUrl,
                      float topInset)
{
    if (!EnsureTarget()) return 0.f;

    // Release temp brushes from last frame
    for (auto* b : m_tempBrushes) if (b) b->Release();
    m_tempBrushes.clear();

    m_hits.clear();

    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0.97f, 0.97f, 0.97f));

    // Draw toolbar background (clean separation)
    if (topInset > 0) {
        m_rt->FillRectangle(
            D2D1::RectF(0, 0, (float)m_width, topInset),
            m_bgBrush);
    }

    if (doc) {
        PaintCtx ctx;
        ctx.y         = 16.f;
        ctx.contentW  = std::max(100.f, (float)m_width - kMarginX * 2.f);
        ctx.scrollY   = scrollY;
        ctx.winH      = (float)m_height;
        ctx.topInset  = topInset;
        ctx.baseUrl   = baseUrl;
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

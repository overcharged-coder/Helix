// box_paint.cpp — paints the layout box tree produced by the layout engine,
// and implements the ITextMeasure interface the engine uses for measurement.
#include "render/renderer.h"
#include "layout/layout_engine.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <functional>
#include <cctype>

static D2D1_COLOR_F ToD2Dc(const CssColor& c) { return { c.r, c.g, c.b, c.a }; }

static std::string FontCacheKey(const FontKey& f) {
    std::string k = std::to_string((int)(f.size * 4));
    k += f.bold ? "b" : "-";
    k += f.italic ? "i" : "-";
    k += f.mono ? "m" : "-";
    k += f.family;
    return k;
}

// ─── ITextMeasure ─────────────────────────────────────────────────────────────

IDWriteTextFormat* Renderer::FormatForKey(const FontKey& f) {
    std::string key = FontCacheKey(f);
    auto it = m_fmtCache.find(key);
    if (it != m_fmtCache.end()) return it->second;
    if (!m_dwrite) return nullptr;

    std::wstring fam;
    if (!f.family.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, f.family.c_str(), -1, nullptr, 0);
        if (n > 0) { fam.resize(n - 1); MultiByteToWideChar(CP_UTF8, 0, f.family.c_str(), -1, fam.data(), n); }
    }
    if (fam.empty()) fam = f.mono ? L"Consolas" : L"Segoe UI";

    IDWriteTextFormat* fmt = nullptr;
    m_dwrite->CreateTextFormat(
        fam.c_str(), nullptr,
        f.bold   ? DWRITE_FONT_WEIGHT_BOLD  : DWRITE_FONT_WEIGHT_NORMAL,
        f.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, std::max(1.f, f.size), L"en-us", &fmt);
    if (fmt) fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_fmtCache[key] = fmt;
    return fmt;
}

float Renderer::MeasureText(const std::wstring& s, const FontKey& f) {
    if (s.empty() || !m_dwrite) return 0.f;
    std::wstring ck = std::wstring(FontCacheKey(f).begin(), FontCacheKey(f).end()) + L"\x1" + s;
    auto cit = m_measureCache.find(ck);
    if (cit != m_measureCache.end()) return cit->second;

    auto* fmt = FormatForKey(f);
    if (!fmt) return (float)s.size() * f.size * 0.5f;
    IDWriteTextLayout* lay = nullptr;
    float w = (float)s.size() * f.size * 0.5f;
    if (SUCCEEDED(m_dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 100000.f, 100000.f, &lay)) && lay) {
        DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m);
        w = m.widthIncludingTrailingWhitespace > 0 ? m.widthIncludingTrailingWhitespace : m.width;
        lay->Release();
    }
    m_measureCache[ck] = w;
    return w;
}

float Renderer::SpaceWidth(const FontKey& f) {
    return MeasureText(L" ", f) * 0.0f + MeasureText(L"x x", f) - 2.f * MeasureText(L"x", f);
}

bool Renderer::ImageIntrinsic(const std::string& url, float& w, float& h) {
    auto it = m_images.find(url);
    if (it != m_images.end() && it->second) {
        D2D1_SIZE_F sz = it->second->GetSize();
        w = sz.width; h = sz.height;
        return w > 0 && h > 0;
    }
    return false;
}

void Renderer::RequestImage(const std::string& url) {
    if (url.empty() || m_loadingImages.count(url)) return;
    m_loadingImages.insert(url);
    if (m_imageRequestCb) m_imageRequestCb(url);
}

// ─── anchors ──────────────────────────────────────────────────────────────────

void Renderer::CollectAnchors(const LayoutBox& box) {
    if (box.node && box.node->type == NodeType::Element) {
        std::string id = box.node->attr("id");
        std::string nm = box.node->attr("name");
        if (!id.empty() && !m_anchorY.count(id)) m_anchorY[id] = box.y;
        if (!nm.empty() && !m_anchorY.count(nm)) m_anchorY[nm] = box.y;
    }
    for (auto& k : box.kids) CollectAnchors(*k);
}

// ─── decorations (background + border + replaced image) ──────────────────────

void Renderer::PaintBoxDecorations(const LayoutBox& box, float scrollY, float topInset) {
    const ComputedStyle& s = box.style;
    float sx = box.x;
    float sy = box.y - scrollY + topInset;
    float bw = box.borderBoxW();
    float bh = box.borderBoxH();
    if (sy + bh < topInset || sy > (float)m_height) {
        // still need replaced image? cull entirely
        if (box.kind != BoxKind::Replaced) return;
        if (box.kind == BoxKind::Replaced && (sy + bh < topInset || sy > (float)m_height)) return;
    }

    // Background color.
    if (s.bgColor.valid && s.bgColor.a > 0.001f) {
        if (auto* b = TempBrush(ToD2Dc(s.bgColor)))
            m_rt->FillRectangle(D2D1::RectF(sx, sy, sx + bw, sy + bh), b);
    }
    // Background image (skip no-repeat fixed: anchored to viewport origin, invisible here).
    if (!s.backgroundImage.empty() && !(s.bgNoRepeat && s.bgFixed)) {
        std::string url = ResolveUrl(s.backgroundImage, m_curBaseUrl);
        auto it = m_images.find(url);
        if (it != m_images.end() && it->second) {
            ID2D1BitmapBrush* brush = nullptr;
            D2D1_EXTEND_MODE ext = s.bgNoRepeat ? D2D1_EXTEND_MODE_CLAMP : D2D1_EXTEND_MODE_WRAP;
            if (SUCCEEDED(m_rt->CreateBitmapBrush(it->second,
                    D2D1::BitmapBrushProperties(ext, ext,
                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), &brush)) && brush) {
                brush->SetTransform(D2D1::Matrix3x2F::Translation(sx, sy));
                m_rt->FillRectangle(D2D1::RectF(sx, sy, sx + bw, sy + bh), brush);
                brush->Release();
            }
        } else {
            const_cast<Renderer*>(this)->RequestImage(url);
        }
    }

    // Replaced image content.
    if (box.kind == BoxKind::Replaced && !box.replacedUrl.empty()) {
        auto it = m_images.find(box.replacedUrl);
        if (it != m_images.end() && it->second) {
            float cx = box.contentX();
            float cy = box.contentY() - scrollY + topInset;
            m_rt->DrawBitmap(it->second, D2D1::RectF(cx, cy, cx + box.contentW, cy + box.contentH));
        }
    }

    // Borders.
    auto side = [&](const CssColor& c) -> ID2D1SolidColorBrush* {
        if (c.valid) return TempBrush(ToD2Dc(c));
        if (s.borderColor.valid) return TempBrush(ToD2Dc(s.borderColor));
        if (s.color.valid) return TempBrush(ToD2Dc(s.color));
        return m_hrBrush;
    };
    float ex = sx + bw, ey = sy + bh;
    if (box.borderTop > 0)    if (auto* b = side(s.borderTopColor))    m_rt->FillRectangle(D2D1::RectF(sx, sy, ex, sy + box.borderTop), b);
    if (box.borderBottom > 0) if (auto* b = side(s.borderBottomColor)) m_rt->FillRectangle(D2D1::RectF(sx, ey - box.borderBottom, ex, ey), b);
    if (box.borderLeft > 0)   if (auto* b = side(s.borderLeftColor))   m_rt->FillRectangle(D2D1::RectF(sx, sy, sx + box.borderLeft, ey), b);
    if (box.borderRight > 0)  if (auto* b = side(s.borderRightColor))  m_rt->FillRectangle(D2D1::RectF(ex - box.borderRight, sy, ex, ey), b);

    // List-item marker.
    if (box.kind == BoxKind::ListItem && !(s.listStyleSet && s.listStyleNone)) {
        FontKey f; f.size = std::max(8.f, (s.fontSize > 0 ? s.fontSize : 16.f) * m_zoom);
        auto* fmt = FormatForKey(f);
        if (fmt && m_textBrush) {
            float mx = box.contentX() - 16.f;
            float my = box.contentY() - scrollY + topInset;
            m_rt->DrawText(L"•", 1, fmt, D2D1::RectF(mx, my, mx + 14.f, my + f.size * 1.4f),
                           m_textBrush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }
}

// ─── inline line painting ─────────────────────────────────────────────────────

void Renderer::PaintLines(const LayoutBox& box, float scrollY, float topInset, bool underFixed) {
    for (const auto& line : box.lines) {
        for (const auto& frag : line.frags) {
            if (!frag.src) continue;
            if (frag.src->kind == BoxKind::InlineBlock || frag.src->kind == BoxKind::Replaced) {
                PaintBox(*frag.src, scrollY, topInset, underFixed);
                continue;
            }
            // Text fragment.
            const ComputedStyle& fs = frag.src->style;
            if (fs.visibilitySet && fs.visibilityHidden) continue;
            float sy = frag.y - (underFixed ? 0.f : scrollY) + topInset;
            if (sy + frag.h < topInset || sy > (float)m_height) {
                if (!frag.src->href.empty())
                    m_hits.push_back({ frag.x, sy, frag.w, frag.h, frag.src->href });
                continue;
            }
            FontKey f;
            f.size = std::max(1.f, (fs.fontSize > 0 ? fs.fontSize : 16.f) * m_zoom);
            f.bold = fs.bold; f.italic = fs.italic;
            f.family = fs.fontFamily;
            std::string fl; for (char c : fs.fontFamily) fl += (char)std::tolower((unsigned char)c);
            f.mono = (fl.find("mono") != std::string::npos || fl.find("consol") != std::string::npos || fl.find("courier") != std::string::npos);
            auto* fmt = FormatForKey(f);
            if (!fmt) continue;
            ID2D1SolidColorBrush* brush = fs.color.valid ? TempBrush(ToD2Dc(fs.color))
                                        : (!frag.src->href.empty() ? m_linkBrush : m_textBrush);
            IDWriteTextLayout* lay = nullptr;
            if (SUCCEEDED(m_dwrite->CreateTextLayout(frag.text.c_str(), (UINT32)frag.text.size(),
                    fmt, frag.w + 4.f, frag.h * 2.f + 4.f, &lay)) && lay) {
                if (fs.underline || !frag.src->href.empty()) {
                    DWRITE_TEXT_RANGE all{ 0, (UINT32)frag.text.size() };
                    lay->SetUnderline(TRUE, all);
                }
                if (!m_searchQuery.empty() && m_findBrush) {
                    std::wstring low = frag.text, q = m_searchQuery;
                    std::transform(low.begin(), low.end(), low.begin(), ::towlower);
                    std::transform(q.begin(), q.end(), q.begin(), ::towlower);
                    if (low.find(q) != std::wstring::npos)
                        m_rt->FillRectangle(D2D1::RectF(frag.x - 1, sy, frag.x + frag.w + 1, sy + frag.h), m_findBrush);
                }
                m_rt->DrawTextLayout(D2D1::Point2F(frag.x, sy), lay, brush ? brush : m_textBrush);
                lay->Release();
            }
            if (!frag.src->href.empty())
                m_hits.push_back({ frag.x, sy, frag.w, frag.h, frag.src->href });
        }
    }
}

// ─── stacking-order box paint ─────────────────────────────────────────────────

void Renderer::PaintBox(const LayoutBox& box, float scrollY, float topInset, bool underFixed) {
    static thread_local int depth = 0;
    if (depth > 600) return;
    struct G { int& d; G(int& x):d(x){++d;} ~G(){--d;} } g(depth);
    if (box.style.isDisplayNone()) return;
    bool fixed = underFixed || box.style.positionMode == 3;
    float effScroll = fixed ? 0.f : scrollY;

    bool hidden = box.style.visibilitySet && box.style.visibilityHidden;

    // 1. This box's own background / borders / replaced content / marker.
    if (!hidden && box.kind != BoxKind::Text && box.kind != BoxKind::Inline
        && box.kind != BoxKind::Break)
        PaintBoxDecorations(box, effScroll, topInset);

    // 2. Children, in CSS2 stacking order (simplified):
    //    negative z-index positioned → in-flow → floats → z>=0 positioned.
    std::vector<const LayoutBox*> negZ, inflow, floats, posZ;
    for (auto& kptr : box.kids) {
        const LayoutBox* k = kptr.get();
        if (k->isOutOfFlow()) {
            if (k->style.zIndexSet && k->style.zIndex < 0) negZ.push_back(k);
            else posZ.push_back(k);
        } else if (k->isFloat()) {
            floats.push_back(k);
        } else if (k->style.positionMode == 1) {
            // relative: in normal flow but paints with positioned descendants on top
            posZ.push_back(k);
        } else {
            inflow.push_back(k);
        }
    }
    auto byZ = [](const LayoutBox* a, const LayoutBox* b) {
        int za = a->style.zIndexSet ? a->style.zIndex : 0;
        int zb = b->style.zIndexSet ? b->style.zIndex : 0;
        return za < zb;
    };
    std::stable_sort(negZ.begin(), negZ.end(), byZ);
    std::stable_sort(posZ.begin(), posZ.end(), byZ);

    for (auto* k : negZ)   PaintBox(*k, scrollY, topInset, fixed);

    // In-flow content of THIS box.
    if (box.establishesInline) {
        if (!hidden) PaintLines(box, effScroll, topInset, fixed);
    } else {
        for (auto* k : inflow) PaintBox(*k, scrollY, topInset, fixed);
    }
    for (auto* k : floats) PaintBox(*k, scrollY, topInset, fixed);
    for (auto* k : posZ)   PaintBox(*k, scrollY, topInset, fixed);
}

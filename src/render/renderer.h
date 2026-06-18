#pragma once
#include "html/dom.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>

struct HitRegion {
    float       x, y, w, h;
    std::string href;
};

// Parsed CSS color value
struct CssColor {
    bool  valid = false;
    float r = 0, g = 0, b = 0, a = 1;
    D2D1_COLOR_F toD2D() const { return { r, g, b, a }; }
};
CssColor ParseCssColor(const std::string& s);

class Renderer {
public:
    ~Renderer();

    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);

    // Paint the document. topInset = toolbar height so content
    // starts below the toolbar. Returns total document height.
    float Paint(const std::shared_ptr<Node>& doc,
                float scrollY,
                const std::string& baseUrl,
                float topInset = 0.f);

    std::string HitTest(float x, float y) const;
    void DiscardTarget();

    // Called on the UI thread when image bytes have arrived.
    void ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes);

    // main.cpp sets this to kick off async image fetches.
    void SetImageRequestCallback(std::function<void(std::string)> cb) {
        m_imageRequestCb = std::move(cb);
    }

private:
    ID2D1Factory*           m_factory   = nullptr;
    ID2D1HwndRenderTarget*  m_rt        = nullptr;
    IDWriteFactory*         m_dwrite    = nullptr;
    IWICImagingFactory*     m_wic       = nullptr;

    ID2D1SolidColorBrush*   m_textBrush = nullptr;
    ID2D1SolidColorBrush*   m_linkBrush = nullptr;
    ID2D1SolidColorBrush*   m_bgBrush   = nullptr;
    ID2D1SolidColorBrush*   m_hrBrush   = nullptr;

    IDWriteTextFormat*      m_fmtBody   = nullptr;
    IDWriteTextFormat*      m_fmtBold   = nullptr;
    IDWriteTextFormat*      m_fmtCode   = nullptr;
    IDWriteTextFormat*      m_fmtH1     = nullptr;
    IDWriteTextFormat*      m_fmtH2     = nullptr;
    IDWriteTextFormat*      m_fmtH3     = nullptr;

    // Image cache — device-dependent, cleared on target loss
    std::map<std::string, ID2D1Bitmap*> m_images;
    std::set<std::string>               m_loadingImages;
    std::function<void(std::string)>    m_imageRequestCb;

    // Per-Paint temporary brushes (CSS color overrides); released after Paint
    std::vector<ID2D1SolidColorBrush*>  m_tempBrushes;

    HWND  m_hwnd   = nullptr;
    UINT  m_width  = 800;
    UINT  m_height = 600;

    std::vector<HitRegion> m_hits;

    bool EnsureTarget();
    void ReleaseTarget();
    void ReleaseBrushes();
    bool CreateBrushes();

    struct PaintCtx {
        float y            = 0;
        float contentW     = 700;
        float scrollY      = 0;
        float winH         = 600;
        float topInset     = 0;
        bool  bold         = false;
        bool  isLink       = false;
        bool  isCode       = false;
        std::string linkHref;
        int   headingLevel = 0;
        std::string baseUrl;
        // nullptr = use default brush
        ID2D1SolidColorBrush* colorOverride = nullptr;
    };

    float WalkNode(const Node* node, PaintCtx& ctx);
    IDWriteTextFormat* FormatFor(const PaintCtx& ctx) const;

    float DrawWrappedText(const std::wstring& text,
                          float x, float y, float maxW,
                          IDWriteTextFormat* fmt,
                          ID2D1SolidColorBrush* brush,
                          bool underline,
                          const std::string& href,
                          float scrollY,
                          float topInset);

    // Creates a temporary brush for the current Paint call
    ID2D1SolidColorBrush* TempBrush(D2D1_COLOR_F color);

    std::wstring ToWide(const std::string& s);
    std::string  ResolveUrl(const std::string& href, const std::string& base);
};

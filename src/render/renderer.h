#pragma once
#include "html/dom.h"
#include "css/stylesheet.h"
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

class Renderer {
public:
    ~Renderer();

    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);

    // Returns total document height.
    float Paint(const std::shared_ptr<Node>& doc,
                float scrollY,
                const std::string& baseUrl,
                float topInset = 0.f);

    std::string HitTest(float x, float y) const;
    void DiscardTarget();

    void ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes);

    void SetImageRequestCallback(std::function<void(std::string)> cb) {
        m_imageRequestCb = std::move(cb);
    }

    void  SetZoom(float z);
    float GetZoom() const { return m_zoom; }

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
    IDWriteTextFormat*      m_fmtItalic = nullptr;
    IDWriteTextFormat*      m_fmtCode   = nullptr;
    IDWriteTextFormat*      m_fmtH1     = nullptr;
    IDWriteTextFormat*      m_fmtH2     = nullptr;
    IDWriteTextFormat*      m_fmtH3     = nullptr;

    float m_zoom = 1.f;

    std::map<std::string, ID2D1Bitmap*> m_images;
    std::set<std::string>               m_loadingImages;
    std::function<void(std::string)>    m_imageRequestCb;

    // Per-Paint temporary resources; released at end of Paint
    std::vector<ID2D1SolidColorBrush*>  m_tempBrushes;
    std::vector<IDWriteTextFormat*>     m_tempFormats;

    HWND  m_hwnd   = nullptr;
    UINT  m_width  = 800;
    UINT  m_height = 600;

    std::vector<HitRegion> m_hits;

    bool EnsureTarget();
    void ReleaseTarget();
    void ReleaseBrushes();
    bool CreateBrushes();
    void RecreateFormats();

    struct PaintCtx {
        float y            = 0;
        float contentW     = 700;
        float scrollY      = 0;
        float winH         = 600;
        float topInset     = 0;
        bool  bold         = false;
        bool  italic       = false;
        bool  isLink       = false;
        bool  isCode       = false;
        std::string linkHref;
        int   headingLevel = 0;
        std::string baseUrl;
        ID2D1SolidColorBrush* colorOverride = nullptr;
        IDWriteTextFormat*    fmtOverride   = nullptr;
        const Stylesheet*     sheet         = nullptr;
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

    ID2D1SolidColorBrush* TempBrush(D2D1_COLOR_F color);
    IDWriteTextFormat*    TempFormat(float size, bool bold, bool mono, bool italic);

    std::wstring ToWide(const std::string& s);
    std::string  ResolveUrl(const std::string& href, const std::string& base);

    static Stylesheet CollectStylesheet(const Node* root);
};

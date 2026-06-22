#pragma once
#include "html/dom.h"
#include "css/stylesheet.h"
#include "layout/box.h"
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

struct TabEntry {
    std::wstring title;
    bool         active  = false;
    bool         loading = false;
};

class Renderer : public ITextMeasure {
public:
    ~Renderer();

    bool Init(HWND hwnd);
    void Resize(UINT width, UINT height);

    // ── ITextMeasure (used by the layout engine) ──────────────────────────
    float MeasureText(const std::wstring& s, const FontKey& f) override;
    float SpaceWidth(const FontKey& f) override;
    bool  ImageIntrinsic(const std::string& url, float& w, float& h) override;
    void  RequestImage(const std::string& url) override;

    // Paint everything. Returns total document height (for scrollbar).
    float Paint(const std::shared_ptr<Node>& doc,
                float scrollY,
                const std::string& baseUrl,
                float topInset   = 0.f,
                float tabStripH  = 0.f,
                const std::vector<TabEntry>* tabs = nullptr);

    std::string HitTest(float x, float y) const;
    int  HitTestTab(float x, float y) const;
    bool HitTestTabClose(float x, float y, int& outIdx) const;
    bool GetAnchorY(const std::string& anchor, float& outY) const;

    void DiscardTarget();
    void ReceiveImage(const std::string& url, const std::vector<uint8_t>& bytes);

    void SetImageRequestCallback(std::function<void(std::string)> cb) {
        m_imageRequestCb = std::move(cb);
    }

    void  SetZoom(float z);
    float GetZoom() const { return m_zoom; }

    void SetSearchQuery(const std::wstring& q) { m_searchQuery = q; }
    const std::wstring& GetSearchQuery() const { return m_searchQuery; }

private:
    // ── D2D / DWrite / WIC ────────────────────────────────────────────────
    ID2D1Factory*           m_factory   = nullptr;
    ID2D1HwndRenderTarget*  m_rt        = nullptr;
    IDWriteFactory*         m_dwrite    = nullptr;
    IWICImagingFactory*     m_wic       = nullptr;

    // ── persistent brushes ────────────────────────────────────────────────
    ID2D1SolidColorBrush*   m_textBrush  = nullptr;
    ID2D1SolidColorBrush*   m_linkBrush  = nullptr;
    ID2D1SolidColorBrush*   m_bgBrush    = nullptr;
    ID2D1SolidColorBrush*   m_hrBrush    = nullptr;
    ID2D1SolidColorBrush*   m_codeBgBrush= nullptr;  // code block bg (#f4f4f4)
    ID2D1SolidColorBrush*   m_findBrush  = nullptr;  // search highlight (#fff176)
    ID2D1SolidColorBrush*   m_quoteBrush = nullptr;  // blockquote bar (#ccc)
    ID2D1SolidColorBrush*   m_tabBgBrush = nullptr;
    ID2D1SolidColorBrush*   m_tabActBrush= nullptr;
    ID2D1SolidColorBrush*   m_tabInaBrush= nullptr;
    ID2D1SolidColorBrush*   m_tabTxtBrush= nullptr;
    ID2D1SolidColorBrush*   m_tabClsBrush= nullptr;

    // ── text formats (zoom-scaled) ────────────────────────────────────────
    IDWriteTextFormat*      m_fmtBody   = nullptr;
    IDWriteTextFormat*      m_fmtBold   = nullptr;
    IDWriteTextFormat*      m_fmtItalic = nullptr;
    IDWriteTextFormat*      m_fmtCode   = nullptr;
    IDWriteTextFormat*      m_fmtH1     = nullptr;
    IDWriteTextFormat*      m_fmtH2     = nullptr;
    IDWriteTextFormat*      m_fmtH3     = nullptr;
    IDWriteTextFormat*      m_fmtTab    = nullptr;   // tab titles (not zoom-scaled)

    float m_zoom = 1.f;

    std::wstring m_searchQuery;
    std::string  m_curBaseUrl;   // base URL for the page currently being painted

    std::map<std::string, ID2D1Bitmap*> m_images;
    std::set<std::string>               m_loadingImages;
    std::set<std::string>               m_failedImages;
    const Node*                         m_imageDocKey = nullptr;
    std::function<void(std::string)>    m_imageRequestCb;

    std::vector<ID2D1SolidColorBrush*>  m_tempBrushes;
    std::vector<IDWriteTextFormat*>     m_tempFormats;

    HWND  m_hwnd   = nullptr;
    UINT  m_width  = 800;
    UINT  m_height = 600;

    std::vector<HitRegion> m_hits;
    std::map<std::string, float> m_anchorY;

    struct TabHit { float x, y, w, h; int idx; bool isClose; };
    std::vector<TabHit> m_tabHits;

    // ── internal helpers ──────────────────────────────────────────────────
    bool EnsureTarget();
    void ReleaseTarget();
    void ReleaseBrushes();
    bool CreateBrushes();
    void RecreateFormats();
    void CreateTabFont();

    void DrawTabStrip(const std::vector<TabEntry>& tabs, float h);

    // ── paint context ─────────────────────────────────────────────────────
    struct PaintCtx {
        float y            = 0;
        float x            = 32.f;
        float contentW     = 700;
        float scrollY      = 0;
        float winH         = 600;
        float topInset     = 0;
        bool  bold         = false;
        bool  italic       = false;
        bool  isLink       = false;
        bool  isCode       = false;
        bool  dryRun       = false;
        int   textAlign    = 0;       // 0=left, 1=center, 2=right
        int   textTransform= 0;       // 0=none, 1=upper, 2=lower, 3=cap
        bool  whiteSpaceNowrap = false;
        int   listStyle    = 0;       // 0=none, 1=unordered, 2=ordered
        int   listCounter  = 0;
        std::string fontFamily;
        std::string linkHref;
        int   headingLevel = 0;
        std::string baseUrl;
        ID2D1SolidColorBrush* colorOverride = nullptr;
        IDWriteTextFormat*    fmtOverride   = nullptr;
        const Stylesheet*     sheet         = nullptr;
        float lineHeightMul = 1.45f;
        int   floatMode         = 0;      // parent's effective float mode for inherit
        float floatBottom       = 0.f;
        float lastMarginBot     = 0.f;    // previous sibling's bottom margin (for collapsing)
        float containingBlockX  = 32.f;   // content-left of nearest positioned ancestor
        float containingBlockY  = 0.f;    // content-top  of nearest positioned ancestor
        float containingBlockW  = 700.f;  // content-width of nearest positioned ancestor
        float absMaxY           = 0.f;    // max bottom-edge of out-of-flow children (dry-run only)
    };

    float WalkNode(const Node* node, PaintCtx& ctx);
    IDWriteTextFormat* FormatFor(const PaintCtx& ctx) const;

    // ── box-tree painter (new layout engine) ──────────────────────────────
    void PaintBox(const LayoutBox& box, float scrollY, float topInset, bool underFixed);
    void PaintLines(const LayoutBox& box, float scrollY, float topInset, bool underFixed);
    void PaintBoxDecorations(const LayoutBox& box, float scrollY, float topInset);
    void CollectAnchors(const LayoutBox& box);
    IDWriteTextFormat* FormatForKey(const FontKey& f);
    std::map<std::string, IDWriteTextFormat*> m_fmtCache;
    std::map<std::wstring, float>             m_measureCache;

    // Cached layout tree — rebuilt only when the document, size, or zoom
    // changes, so scrolling/repainting is cheap.
    std::unique_ptr<LayoutBox> m_layoutRoot;
    const Node* m_layoutDocKey  = nullptr;
    UINT  m_layoutWKey = 0, m_layoutHKey = 0;
    float m_layoutZoomKey = 0.f;
    void InvalidateLayout() { m_layoutRoot.reset(); m_layoutDocKey = nullptr; }

    // Returns new y after drawing (or dry-run computing) wrapped text.
    float DrawWrappedText(const std::wstring& text,
                          float x, float y, float maxW,
                          IDWriteTextFormat* fmt,
                          ID2D1SolidColorBrush* brush,
                          bool underline,
                          const std::string& href,
                          float scrollY,
                          float topInset,
                          float lineHeightMul = 1.45f,
                          int   textAlign     = 0,
                          bool  dryRun        = false,
                          bool  nowrap        = false);

    // Draw a preformatted block (pre element), returns new y.
    float DrawPreBlock(const Node* node, PaintCtx& ctx);

    ID2D1SolidColorBrush* TempBrush(D2D1_COLOR_F color);
    IDWriteTextFormat*    TempFormat(float size, bool bold, bool mono, bool italic,
                                     const std::string& family = "");

    std::wstring ToWide(const std::string& s);
    std::string  ResolveUrl(const std::string& href, const std::string& base);

    static Stylesheet CollectStylesheet(const Node* root);
    static CssColor   FindBodyBgColor(const Node* root, const Stylesheet& sheet);
};

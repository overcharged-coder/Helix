#include "test/fixture.h"

#include "css/stylesheet.h"
#include "html/parser.h"
#include "layout/layout_engine.h"

namespace {
class FixedMeasure final : public ITextMeasure {
public:
    float MeasureText(const std::wstring& text, const FontKey&) override {
        return static_cast<float>(text.size()) * 8.f;
    }
    float SpaceWidth(const FontKey&) override { return 4.f; }
    bool ImageIntrinsic(const std::string&, float&, float&) override { return false; }
    void RequestImage(const std::string& url) override { requestedImages.push_back(url); }

    std::vector<std::string> requestedImages;
};

LayoutBox* FindEngineBoxById(LayoutBox* box, const std::string& id) {
    if (!box) return nullptr;
    if (box->node && box->node->attr("id") == id) return box;
    for (auto& child : box->kids)
        if (auto* found = FindEngineBoxById(child.get(), id)) return found;
    return nullptr;
}
} // namespace

TestResult RunLayoutEngineTests() {
    TestResult result;
    auto dom = ParseHtml(
        "<html><body><div id=\"row\"><div id=\"a\"></div><div id=\"b\"></div></div>"
        "<div id=\"column\"><div id=\"c\"></div><div id=\"d\"></div></div></body></html>");
    auto sheet = ParseStylesheet(
        "#row { display:flex; width:200px; gap:10px; }"
        "#row > div { flex:1; height:20px; }"
        "#column { display:flex; flex-direction:column; width:100px; gap:10px; }"
        "#column > div { height:20px; }");
    FixedMeasure measure;
    LayoutInput input;
    input.document = dom.get();
    input.sheet = &sheet;
    input.measure = &measure;
    input.viewportW = 320.f;
    input.viewportH = 480.f;
    auto layout = LayoutDocument(input);
    auto* a = FindEngineBoxById(layout.get(), "a");
    auto* b = FindEngineBoxById(layout.get(), "b");
    auto* c = FindEngineBoxById(layout.get(), "c");
    auto* d = FindEngineBoxById(layout.get(), "d");
    const bool found = a && b && c && d;
    const int rowDelta = found ? static_cast<int>(b->x - a->x + 0.5f) : -1;
    const int rowWidth = found ? static_cast<int>(a->contentW + 0.5f) : -1;
    const int columnDelta = found ? static_cast<int>(d->y - c->y + 0.5f) : -1;
    ExpectEqual("layout-engine/flex-row-grow-gap-and-column-gap",
        std::string(found ? "found " : "missing ") + "rowDelta=" + std::to_string(rowDelta)
            + " rowWidth=" + std::to_string(rowWidth)
            + " columnDelta=" + std::to_string(columnDelta) + "\n",
        "found rowDelta=105 rowWidth=95 columnDelta=30\n",
        result);

    {
        auto lazyDom = ParseHtml(
            "<html><body><img src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==\" "
            "data-src=\"/images/photo.jpg\"></body></html>");
        auto lazySheet = ParseStylesheet("");
        FixedMeasure lazyMeasure;
        LayoutInput lazyInput;
        lazyInput.document = lazyDom.get();
        lazyInput.sheet = &lazySheet;
        lazyInput.measure = &lazyMeasure;
        lazyInput.viewportW = 320.f;
        lazyInput.viewportH = 480.f;
        lazyInput.baseUrl = "https://example.test/articles/start.html";
        LayoutDocument(lazyInput);
        const std::string requested = lazyMeasure.requestedImages.empty()
            ? "none\n" : lazyMeasure.requestedImages.front() + "\n";
        ExpectEqual("layout-engine/lazy-image-prefers-real-data-src-over-data-placeholder",
            requested,
            "https://example.test/images/photo.jpg\n",
            result);
    }

    {
        auto pictureDom = ParseHtml(
            "<html><body><picture><source srcset=\"/images/cover.png 1x\">"
            "<img id=\"cover\"></picture></body></html>");
        auto pictureSheet = ParseStylesheet("");
        FixedMeasure pictureMeasure;
        LayoutInput pictureInput;
        pictureInput.document = pictureDom.get();
        pictureInput.sheet = &pictureSheet;
        pictureInput.measure = &pictureMeasure;
        pictureInput.viewportW = 320.f;
        pictureInput.viewportH = 480.f;
        pictureInput.baseUrl = "https://example.test/articles/start.html";
        LayoutDocument(pictureInput);
        const std::string requested = pictureMeasure.requestedImages.empty()
            ? "none\n" : pictureMeasure.requestedImages.front() + "\n";
        ExpectEqual("layout-engine/picture-source-provides-image-when-img-has-no-src",
            requested,
            "https://example.test/images/cover.png\n",
            result);
    }

    {
        auto gridDom = ParseHtml(
            "<html><body><div id=\"grid\"><div id=\"g1\"></div><div id=\"g2\"></div>"
            "<div id=\"g3\"></div></div></body></html>");
        auto gridSheet = ParseStylesheet(
            "#grid { display:grid; width:210px; grid-template-columns:1fr 2fr; gap:10px; }"
            "#grid > div { height:20px; }");
        LayoutInput gridInput;
        gridInput.document = gridDom.get();
        gridInput.sheet = &gridSheet;
        gridInput.measure = &measure;
        gridInput.viewportW = 320.f;
        gridInput.viewportH = 480.f;
        auto gridLayout = LayoutDocument(gridInput);
        auto* g1 = FindEngineBoxById(gridLayout.get(), "g1");
        auto* g2 = FindEngineBoxById(gridLayout.get(), "g2");
        auto* g3 = FindEngineBoxById(gridLayout.get(), "g3");
        const bool gridFound = g1 && g2 && g3;
        const int firstWidth = gridFound ? static_cast<int>(g1->contentW + .5f) : -1;
        const int secondX = gridFound ? static_cast<int>(g2->x - g1->x + .5f) : -1;
        const int nextRowY = gridFound ? static_cast<int>(g3->y - g1->y + .5f) : -1;
        ExpectEqual("layout-engine/grid-fr-tracks-and-row-gap",
            std::string(gridFound ? "found " : "missing ")
                + "firstWidth=" + std::to_string(firstWidth)
                + " secondX=" + std::to_string(secondX)
                + " nextRowY=" + std::to_string(nextRowY) + "\n",
            "found firstWidth=67 secondX=77 nextRowY=30\n",
            result);
    }

    {
        std::string giantText(256 * 1024, 'x');
        auto giantDom = ParseHtml("<html><body><p id=\"giant\">" + giantText + "</p></body></html>");
        auto giantSheet = ParseStylesheet("");
        FixedMeasure giantMeasure;
        LayoutInput giantInput;
        giantInput.document = giantDom.get();
        giantInput.sheet = &giantSheet;
        giantInput.measure = &giantMeasure;
        giantInput.viewportW = 640.f;
        giantInput.viewportH = 480.f;
        auto giantLayout = LayoutDocument(giantInput);
        auto* giant = FindEngineBoxById(giantLayout.get(), "giant");
        const bool bounded = giant && !giant->kids.empty()
            && giant->kids.front()->text.size() <= 16 * 1024;
        ExpectEqual("layout-engine/giant-text-is-bounded-before-measurement",
            bounded ? "bounded\n" : "unbounded\n",
            "bounded\n",
            result);
    }

    // Float text-wrap: paragraphs (even nested in a wrapper) must wrap their
    // line boxes to the left of a float:right sidebar, and the wrapper must not
    // stretch to the float's bottom.
    {
        auto fdom = ParseHtml(
            "<html><body><div id=\"box\">"
            "<div id=\"side\"></div>"
            "<div id=\"wrap\"><p id=\"para\">"
            "word word word word word word word word word word word word word word</p></div>"
            "</div></body></html>");
        auto fsheet = ParseStylesheet(
            "#box { width:400px; }"
            "#side { float:right; width:100px; height:200px; }"
            "#wrap, p { margin:0; }");
        FixedMeasure fmeasure;
        LayoutInput fin;
        fin.document = fdom.get();
        fin.sheet = &fsheet;
        fin.measure = &fmeasure;
        fin.viewportW = 800.f;
        fin.viewportH = 600.f;
        auto flayout = LayoutDocument(fin);
        auto* para = FindEngineBoxById(flayout.get(), "para");
        auto* wrap = FindEngineBoxById(flayout.get(), "wrap");
        bool fok = para && wrap && !para->lines.empty();
        // First line must end at or before the float's left edge (x=300).
        bool clamped = fok && (para->lines.front().x + para->lines.front().w) <= 305.f;
        // Wrapper height must reflect only the text (a few lines), not 200px.
        bool notStretched = wrap && wrap->contentH < 150.f;
        ExpectEqual("layout-engine/float-wrap-around-right-sidebar",
            std::string(fok ? "ok" : "missing")
                + " clamped=" + (clamped ? "1" : "0")
                + " notStretched=" + (notStretched ? "1" : "0") + "\n",
            "ok clamped=1 notStretched=1\n",
            result);
    }

    // Auto table layout: shared column widths align cells vertically, the table
    // shrinks to its content, and colspan spans multiple columns.
    {
        auto tdom = ParseHtml(
            "<html><body><table>"
            "<tr><th colspan=\"2\" id=\"hdr\">Header</th></tr>"
            "<tr><td id=\"k1\">Kingdom</td><td id=\"v1\">Animalia</td></tr>"
            "<tr><td id=\"k2\">Phy</td><td id=\"v2\">Chordata longvalue</td></tr>"
            "</table></body></html>");
        auto tsheet = ParseStylesheet(
            "table { border-spacing:0; } td,th { padding:0; margin:0; }");
        FixedMeasure tmeasure;
        LayoutInput tin;
        tin.document = tdom.get();
        tin.sheet = &tsheet;
        tin.measure = &tmeasure;
        tin.viewportW = 1200.f;
        tin.viewportH = 600.f;
        auto tlayout = LayoutDocument(tin);
        auto* k1 = FindEngineBoxById(tlayout.get(), "k1");
        auto* k2 = FindEngineBoxById(tlayout.get(), "k2");
        auto* v1 = FindEngineBoxById(tlayout.get(), "v1");
        auto* hdr = FindEngineBoxById(tlayout.get(), "hdr");
        bool tok = k1 && k2 && v1 && hdr;
        // Column 0 cells share x and width; column 1 starts after column 0.
        bool col0Aligned = tok && (int)(k1->x + .5f) == (int)(k2->x + .5f)
                                && (int)(k1->contentW + .5f) == (int)(k2->contentW + .5f);
        bool col1After = tok && v1->x > k1->x + k1->contentW - 1.f;
        // Header spans both columns: wider than a single column.
        bool spans = tok && hdr->contentW > k1->contentW + 1.f;
        ExpectEqual("layout-engine/table-shared-columns-and-colspan",
            std::string(tok ? "ok" : "missing")
                + " col0Aligned=" + (col0Aligned ? "1" : "0")
                + " col1After=" + (col1After ? "1" : "0")
                + " spans=" + (spans ? "1" : "0") + "\n",
            "ok col0Aligned=1 col1After=1 spans=1\n",
            result);
    }

    // align-items: center positions a short flex item in the middle of the
    // line's cross axis (defined by the tallest item).
    {
        auto adom = ParseHtml(
            "<html><body><div id=\"f\"><div id=\"tall\"></div><div id=\"short\"></div></div></body></html>");
        auto asheet = ParseStylesheet(
            "#f { display:flex; align-items:center; width:200px; }"
            "#tall { width:40px; height:100px; }"
            "#short { width:40px; height:20px; }");
        LayoutInput ain; ain.document = adom.get(); ain.sheet = &asheet;
        ain.measure = &measure; ain.viewportW = 320.f; ain.viewportH = 480.f;
        auto al = LayoutDocument(ain);
        auto* tall = FindEngineBoxById(al.get(), "tall");
        auto* shortBox = FindEngineBoxById(al.get(), "short");
        bool ok = tall && shortBox;
        // short (20px) centered in a 100px line → offset (100-20)/2 = 40px below tall's top.
        int delta = ok ? (int)(shortBox->y - tall->y + .5f) : -1;
        ExpectEqual("layout-engine/flex-align-items-center",
            std::string(ok ? "ok " : "missing ") + "delta=" + std::to_string(delta) + "\n",
            "ok delta=40\n",
            result);
    }

    // width:max-content shrinks a block to its content instead of filling.
    {
        auto mdom = ParseHtml(
            "<html><body><div id=\"mc\">hello</div></body></html>");
        auto msheet = ParseStylesheet("#mc { width:max-content; }");
        LayoutInput min; min.document = mdom.get(); min.sheet = &msheet;
        min.measure = &measure; min.viewportW = 800.f; min.viewportH = 480.f;
        auto ml = LayoutDocument(min);
        auto* mc = FindEngineBoxById(ml.get(), "mc");
        // "hello" = 5 chars * 8px (FixedMeasure) = 40px, not the 800px viewport.
        bool ok = mc && mc->contentW > 1.f && mc->contentW < 100.f;
        ExpectEqual("layout-engine/width-max-content-shrinks-to-content",
            std::string(ok ? "shrunk" : "filled") + "\n",
            "shrunk\n",
            result);
    }

    // display:contents removes the wrapper's own box but preserves children.
    {
        auto cdom = ParseHtml(
            "<html><body><div id=\"wrap\"><p id=\"child\">hello</p></div></body></html>");
        auto csheet = ParseStylesheet(
            "#wrap { display: contents; }"
            "#child { margin:0; height:20px; }");
        LayoutInput cin; cin.document = cdom.get(); cin.sheet = &csheet;
        cin.measure = &measure; cin.viewportW = 320.f; cin.viewportH = 480.f;
        auto cl = LayoutDocument(cin);
        auto* wrap = FindEngineBoxById(cl.get(), "wrap");
        auto* child = FindEngineBoxById(cl.get(), "child");
        ExpectEqual("layout-engine/display-contents-skips-wrapper-keeps-children",
            std::string(!wrap && child ? "contents\n" : "wrapped\n"),
            "contents\n",
            result);
    }

    // display:flow-root establishes a new BFC, so outside floats do not intrude.
    {
        auto frdom = ParseHtml(
            "<html><body><div id=\"box\">"
            "<div id=\"side\"></div>"
            "<div id=\"flow\"><p id=\"para\">word word word word word word word word word word word word</p></div>"
            "</div></body></html>");
        auto frsheet = ParseStylesheet(
            "#box { width:400px; }"
            "#side { float:right; width:100px; height:200px; }"
            "#flow { display: flow-root; margin:0; }"
            "#para { margin:0; }");
        LayoutInput frin; frin.document = frdom.get(); frin.sheet = &frsheet;
        frin.measure = &measure; frin.viewportW = 800.f; frin.viewportH = 600.f;
        auto frl = LayoutDocument(frin);
        auto* para = FindEngineBoxById(frl.get(), "para");
        bool ignoresOutsideFloat = para && !para->lines.empty()
            && (para->lines.front().x + para->lines.front().w) > 305.f;
        ExpectEqual("layout-engine/flow-root-starts-new-bfc",
            ignoresOutsideFloat ? "flow-root\n" : "intruded\n",
            "flow-root\n",
            result);
    }

    // position:sticky should parse as a relative-style fallback, not static.
    {
        auto sdom = ParseHtml("<html><body><div id=\"sticky\"></div></body></html>");
        auto ssheet = ParseStylesheet("#sticky { position: sticky; top: 12px; }");
        LayoutInput sin; sin.document = sdom.get(); sin.sheet = &ssheet;
        sin.measure = &measure; sin.viewportW = 320.f; sin.viewportH = 480.f;
        auto sl = LayoutDocument(sin);
        auto* sticky = FindEngineBoxById(sl.get(), "sticky");
        const bool positioned = sticky && sticky->style.positionMode == 1 && sticky->style.topSet;
        ExpectEqual("layout-engine/sticky-falls-back-to-relative",
            positioned ? "relative\n" : "static\n",
            "relative\n",
            result);
    }

    // Wikipedia-style portal links use a block title followed by an inline-block
    // article count. The count must be laid out on the next line, not flattened
    // onto the same inline baseline as the title.
    {
        auto ldom = ParseHtml(
            "<html><body><a id=\"link\" class=\"link-box\" href=\"#\">"
            "<strong id=\"title\">English</strong>"
            "<small id=\"count\">7,189,000+ <span>articles</span></small>"
            "</a></body></html>");
        auto lsheet = ParseStylesheet(
            ".link-box { display:block; width:156px; text-align:center; }"
            ".link-box strong { display:block; font-size:16px; line-height:20px; }"
            ".link-box small { display:inline-block; font-size:13px; line-height:20px; }");
        LayoutInput lin; lin.document = ldom.get(); lin.sheet = &lsheet;
        lin.measure = &measure; lin.viewportW = 320.f; lin.viewportH = 480.f;
        auto ll = LayoutDocument(lin);
        auto* link = FindEngineBoxById(ll.get(), "link");
        auto* title = FindEngineBoxById(ll.get(), "title");
        auto* count = FindEngineBoxById(ll.get(), "count");
        bool ok = link && title && count
            && count->y >= title->y + title->borderBoxH() - 0.5f
            && link->contentH >= title->marginBoxH() + count->marginBoxH() - 0.5f
            && !count->lines.empty()
            && !count->lines.front().frags.empty()
            && count->lines.front().frags.front().y >= count->contentY() - 0.5f;
        ExpectEqual("layout-engine/inline-block-count-follows-block-title",
            ok ? "stacked\n" : "overlap\n",
            "stacked\n",
            result);
    }
    return result;
}

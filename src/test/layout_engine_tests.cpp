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
    void RequestImage(const std::string&) override {}
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
    return result;
}

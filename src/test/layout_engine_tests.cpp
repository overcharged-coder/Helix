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

    // UA default: a <dialog> without the open attribute is not rendered.
    {
        auto ddom = ParseHtml(
            "<html><body><dialog id=\"closed\">hidden</dialog>"
            "<dialog id=\"open\" open>visible</dialog></body></html>");
        auto dsheet = ParseStylesheet("");
        LayoutInput din; din.document = ddom.get(); din.sheet = &dsheet;
        din.measure = &measure; din.viewportW = 480.f; din.viewportH = 320.f;
        auto dl = LayoutDocument(din);
        auto* closed = FindEngineBoxById(dl.get(), "closed");
        auto* open = FindEngineBoxById(dl.get(), "open");
        ExpectEqual("layout-engine/dialog-closed-is-display-none-by-default",
            std::string(!closed && open ? "hidden\n" : "visible\n"),
            "hidden\n",
            result);
    }

    // External SVGs should still be treated as requestable replaced images.
    {
        auto sdom = ParseHtml("<html><body><img id=\"mark\" src=\"/static/mark.svg\"></body></html>");
        auto ssheet = ParseStylesheet("");
        FixedMeasure svgMeasure;
        LayoutInput sin; sin.document = sdom.get(); sin.sheet = &ssheet;
        sin.measure = &svgMeasure; sin.viewportW = 320.f; sin.viewportH = 240.f;
        sin.baseUrl = "https://www.wikipedia.org/portal/index.html";
        LayoutDocument(sin);
        const std::string requested = svgMeasure.requestedImages.empty()
            ? "none\n" : svgMeasure.requestedImages.front() + "\n";
        ExpectEqual("layout-engine/external-svg-img-requests-image",
            requested,
            "https://www.wikipedia.org/static/mark.svg\n",
            result);
    }

    // Distilled Wikipedia portal: an absolute logo centered in a relative
    // language cluster and language links placed by percentage offsets.
    {
        auto pdom = ParseHtml(
            "<html><body><div id=\"portal\">"
            "<div id=\"logo\"></div><a id=\"lang1\">English</a><a id=\"lang2\">Deutsch</a>"
            "</div></body></html>");
        auto psheet = ParseStylesheet(
            "body { margin:0; }"
            "#portal { position:relative; width:600px; height:400px; margin-left:auto; margin-right:auto; }"
            "#logo { position:absolute; width:200px; height:180px; left:50%; top:20px; margin-left:-100px; }"
            "#lang1, #lang2 { position:absolute; width:160px; height:40px; }"
            "#lang1 { left:10%; top:120px; }"
            "#lang2 { right:10%; top:120px; }");
        LayoutInput pin; pin.document = pdom.get(); pin.sheet = &psheet;
        pin.measure = &measure; pin.viewportW = 1000.f; pin.viewportH = 600.f;
        auto pl = LayoutDocument(pin);
        auto* portal = FindEngineBoxById(pl.get(), "portal");
        auto* logo = FindEngineBoxById(pl.get(), "logo");
        auto* lang1 = FindEngineBoxById(pl.get(), "lang1");
        auto* lang2 = FindEngineBoxById(pl.get(), "lang2");
        bool ok = portal && logo && lang1 && lang2
            && (int)(portal->x + .5f) == 200
            && (int)(logo->x - portal->contentX() + .5f) == 200
            && (int)(lang1->x - portal->contentX() + .5f) == 60
            && (int)(portal->contentX() + portal->contentW - lang2->x - lang2->borderBoxW() + .5f) == 60;
        ExpectEqual("layout-engine/wikipedia-portal-absolute-cluster",
            ok ? "positioned\n" : "misplaced\n",
            "positioned\n",
            result);
    }

    // Search controls need tag-specific intrinsic sizes rather than every
    // control becoming a generic 140px block.
    {
        auto fdom = ParseHtml(
            "<html><body><form id=\"search\"><input id=\"q\"><button id=\"go\">Search</button>"
            "<select id=\"lang\"><option>EN</option></select></form></body></html>");
        auto fsheet = ParseStylesheet("#search { width:500px; margin:0; } #q { width:300px; }");
        LayoutInput fin; fin.document = fdom.get(); fin.sheet = &fsheet;
        fin.measure = &measure; fin.viewportW = 800.f; fin.viewportH = 400.f;
        auto fl = LayoutDocument(fin);
        auto* q = FindEngineBoxById(fl.get(), "q");
        auto* go = FindEngineBoxById(fl.get(), "go");
        auto* lang = FindEngineBoxById(fl.get(), "lang");
        bool ok = q && go && lang
            && (int)(q->contentW + .5f) == 300
            && go->contentW >= 56.f && go->contentW < 100.f
            && lang->contentW >= 48.f && lang->contentW < 100.f
            && go->contentH >= 28.f && lang->contentH >= 28.f;
        ExpectEqual("layout-engine/form-controls-have-useful-intrinsic-sizes",
            ok ? "sized\n" : "generic\n",
            "sized\n",
            result);
    }

    // Wikipedia's search form includes hidden inputs before/after the visible
    // search controls; they must not create anonymous white input boxes that
    // push the real input/button/select into each other.
    {
        auto hdom = ParseHtml(
            "<html><body><form id=\"search\">"
            "<input type=\"hidden\" id=\"family\" name=\"family\" value=\"wikipedia\">"
            "<div id=\"search-input\"><input id=\"q\" type=\"search\"><div id=\"picker\"><select id=\"lang\"><option>Afrikaans</option></select></div></div>"
            "<button id=\"go\"><i id=\"search-icon\" class=\"sprite svg-search-icon\">Search</i></button>"
            "<input type=\"hidden\" id=\"go-hidden\" value=\"Go\" name=\"go\">"
            "</form></body></html>");
        auto hsheet = ParseStylesheet(
            "#search { width:540px; margin:0; }"
            "#search-input { display:inline-block; position:relative; width:73%; vertical-align:top; }"
            "#q { width:100%; height:44px; }"
            "#picker { position:absolute; top:0; right:0; width:120px; height:44px; }"
            "#go { display:inline-block; width:23%; min-height:44px; vertical-align:top; }"
            ".sprite { display:inline-block; width:22px; height:22px; text-indent:-9999px; }");
        LayoutInput hin; hin.document = hdom.get(); hin.sheet = &hsheet;
        hin.measure = &measure; hin.viewportW = 800.f; hin.viewportH = 400.f;
        auto hl = LayoutDocument(hin);
        auto* family = FindEngineBoxById(hl.get(), "family");
        auto* hiddenGo = FindEngineBoxById(hl.get(), "go-hidden");
        auto* wrap = FindEngineBoxById(hl.get(), "search-input");
        auto* q = FindEngineBoxById(hl.get(), "q");
        auto* picker = FindEngineBoxById(hl.get(), "picker");
        auto* go = FindEngineBoxById(hl.get(), "go");
        auto* icon = FindEngineBoxById(hl.get(), "search-icon");
        bool rowOk = !family && !hiddenGo && wrap && q && picker && go
            && go->kind != BoxKind::Replaced
            && icon && icon->kind == BoxKind::InlineBlock
            && std::abs(wrap->x - q->x) < 0.5f
            && picker->x >= q->x + q->borderBoxW() - picker->borderBoxW() - 1.f
            && go->x >= wrap->x + wrap->borderBoxW() - 1.f
            && std::abs(go->y - wrap->y) < 1.f;
        ExpectEqual("layout-engine/hidden-inputs-do-not-mangle-search-row",
            rowOk ? "hidden\n" : "visible\n",
            "hidden\n",
            result);
    }

    // Dense language lists should wrap into multiple inline lines inside the
    // container instead of overflowing as one massive line.
    {
        auto wdom = ParseHtml(
            "<html><body><ul id=\"langs\">"
            "<li>Afar</li><li>Deutsch</li><li>English</li><li>Español</li><li>Français</li>"
            "<li>Italiano</li><li>Polski</li><li>Português</li><li>Русский</li><li>日本語</li>"
            "</ul></body></html>");
        auto wsheet = ParseStylesheet(
            "#langs { width:240px; margin:0; padding-left:0; }"
            "#langs li { display:inline; margin-right:12px; }");
        LayoutInput win; win.document = wdom.get(); win.sheet = &wsheet;
        win.measure = &measure; win.viewportW = 800.f; win.viewportH = 400.f;
        auto wl = LayoutDocument(win);
        auto* langs = FindEngineBoxById(wl.get(), "langs");
        bool wraps = langs && langs->establishesInline && langs->lines.size() >= 2;
        bool clamped = wraps;
        if (wraps) {
            for (const auto& line : langs->lines)
                clamped = clamped && line.w <= langs->contentW + 0.5f;
        }
        ExpectEqual("layout-engine/dense-inline-language-list-wraps",
            wraps && clamped ? "wrapped\n" : "overflow\n",
            "wrapped\n",
            result);
    }
    return result;
}

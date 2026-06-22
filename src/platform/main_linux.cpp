#ifdef __linux__
//
// main_linux.cpp — Linux application shell for Helix browser.
//
// Creates a GTK3 window with a toolbar (back/forward/reload/URL entry), a
// GtkDrawingArea for rendering, and drives the browser core.
//
#include <gtk/gtk.h>
#include "platform/platform.h"
#include "platform/browser_core.h"
#include "platform/box_painter.h"
#include "platform/plat_text_measure.h"
#include "third_party/stb_image.h"
#include <set>
#include "layout/layout_engine.h"
#include "css/stylesheet.h"

// ── globals ──────────────────────────────────────────────────────────────────

static GtkWidget* g_window;
static GtkWidget* g_urlEntry;
static GtkWidget* g_statusLabel;
static GtkWidget* g_drawingArea;

static std::vector<Tab> g_tabs;
static int g_activeTab = 0;
static JsEngine g_js;
static Semaphore g_imageFetchGate(6);

static std::unique_ptr<IPlatformRenderer> g_renderer;
static std::unique_ptr<PlatTextMeasure> g_measure;
static std::unique_ptr<LayoutBox> g_layoutRoot;
static std::map<std::string, PlatBitmap> g_images;
static std::set<std::string> g_loadingImages;
static std::set<std::string> g_failedImages;
static std::map<std::string, PlatFont> g_fontCache;

static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ── image loading pipeline ───────────────────────────────────────────────────

struct LinuxImageMsg {
    std::string url;
    std::vector<uint8_t> bytes;
};

static void ProcessImage(const std::string& url, const std::vector<uint8_t>& bytes) {
    if (!g_renderer || bytes.empty()) { g_failedImages.insert(url); return; }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) { if (pixels) stbi_image_free(pixels); g_failedImages.insert(url); return; }
    // stb_image outputs RGBA; Cairo wants ARGB32 (BGRA premultiplied on little-endian).
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = pixels + i * 4;
        unsigned char r = p[0], g = p[1], b = p[2], a = p[3];
        float af = a / 255.f;
        p[0] = (unsigned char)(b * af + 0.5f);
        p[1] = (unsigned char)(g * af + 0.5f);
        p[2] = (unsigned char)(r * af + 0.5f);
        p[3] = a;
    }
    PlatBitmap bmp = g_renderer->CreateBitmap(w, h, pixels);
    stbi_image_free(pixels);
    if (bmp) {
        auto it = g_images.find(url);
        if (it != g_images.end() && it->second) g_renderer->ReleaseBitmap(it->second);
        g_images[url] = bmp;
        g_measure->loadedImages[url] = { (float)w, (float)h };
        g_layoutRoot.reset();  // force relayout
        if (g_drawingArea) gtk_widget_queue_draw(g_drawingArea);
    } else {
        g_failedImages.insert(url);
    }
}

static void FetchImageAsync(const std::string& url) {
    if (g_loadingImages.count(url) || g_failedImages.count(url) || g_images.count(url)) return;
    g_loadingImages.insert(url);
    std::thread([url]() {
        g_imageFetchGate.acquire();
        auto res = FetchUrl(url);
        g_imageFetchGate.release();
        auto* msg = new LinuxImageMsg{ url, {} };
        if (res.success && !res.body.empty())
            msg->bytes = std::vector<uint8_t>(res.body.begin(), res.body.end());
        g_idle_add([](gpointer data) -> gboolean {
            auto* m = static_cast<LinuxImageMsg*>(data);
            g_loadingImages.erase(m->url);
            ProcessImage(m->url, m->bytes);
            delete m;
            return G_SOURCE_REMOVE;
        }, msg);
    }).detach();
}

static Stylesheet CollectCSS(const Node* root) {
    Stylesheet sheet;
    std::function<void(const Node*)> walk = [&](const Node* n) {
        if (!n) return;
        if (n->type == NodeType::Element && n->tagName == "style") {
            std::string css; for (auto& c : n->children) if (c->type == NodeType::Text) css += c->text;
            auto part = ParseStylesheet(css);
            for (auto& r : part.rules) sheet.rules.push_back(r);
        }
        for (auto& c : n->children) walk(c.get());
    };
    walk(root);
    return sheet;
}

// ── drawing ──────────────────────────────────────────────────────────────────

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    (void)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int w = alloc.width, h = alloc.height;

    if (!g_renderer) {
        g_renderer = CreatePlatformRenderer();
        g_renderer->Init(widget);
        g_measure = std::make_unique<PlatTextMeasure>(g_renderer.get());
        g_measure->onImageRequest = FetchImageAsync;
    }
    g_renderer->Resize(w, h);
    g_renderer->SetNativeContext(cr);
    g_renderer->Clear({1, 1, 1, 1});

    if (g_tabs.empty() || !CurTab().page || !CurTab().page->dom) {
        PlatFont font = g_renderer->CreateFont(16, false, false, false, "");
        g_renderer->DrawText(CurTab().loading ? L"Loading..." : L"Navigate to a URL",
                             20, 20, 800, 30, font, {0.5f, 0.5f, 0.5f, 1});
        g_renderer->ReleaseFont(font);
        return FALSE;
    }

    Tab& tab = CurTab();
    try {
        Stylesheet sheet = CollectCSS(tab.page->dom.get());
        LayoutInput in;
        in.document = tab.page->dom.get();
        in.sheet = &sheet;
        in.measure = g_measure.get();
        in.viewportW = (float)w;
        in.viewportH = (float)h;
        in.zoom = 1.f;
        in.baseUrl = tab.page->url;
        g_layoutRoot = LayoutDocument(in);
        if (g_layoutRoot) {
            std::vector<HitRegion> hits;
            PaintState ps;
            ps.r = g_renderer.get();
            ps.scrollY = tab.scrollY;
            ps.topInset = 0;
            ps.baseUrl = tab.page->url;
            ps.images = &g_images;
            ps.hits = &hits;
            ps.fontCache = &g_fontCache;
            PaintBoxTree(ps, *g_layoutRoot);
            tab.docHeight = g_layoutRoot->contentH + 32.f;
        }
    } catch (...) { /* keep the browser alive */ }
    return FALSE;
}

// ── URL bar ──────────────────────────────────────────────────────────────────

static void on_url_activate(GtkEntry* entry, gpointer data) {
    (void)data;
    const gchar* text = gtk_entry_get_text(entry);
    if (!text || g_tabs.empty()) return;
    std::string url(text);
    Tab& tab = CurTab();

    if (url == "helix://home") {
        tab.page = std::make_shared<Page>();
        tab.page->url = url;
        tab.page->dom = ParseHtml(HomePageHtml());
        tab.title = "Helix";
        tab.url = url;
        tab.scrollY = 0;
        gtk_widget_queue_draw(g_drawingArea);
        return;
    }

    if (LooksLikeUrl(url)) {
        if (url.find("://") == std::string::npos) url = "https://" + url;
    } else {
        url = "https://www.bing.com/search?q=" + UrlEncodeQuery(url);
    }

    tab.url = url;
    tab.title = "Loading...";
    tab.loading = true;
    tab.scrollY = 0;
    gtk_widget_queue_draw(g_drawingArea);

    // Fetch in background thread, update on main thread via g_idle_add
    std::string fetchUrl = url;
    std::thread([fetchUrl]() {
        auto res = FetchUrl(fetchUrl);
        auto* page = new Page();
        page->url = fetchUrl;
        if (res.success && !res.body.empty()) {
            page->dom = ParseHtml(res.body);
            LoadExternalStylesheets(page->dom, page->url);
        } else {
            page->error = res.error;
        }
        g_idle_add([](gpointer data) -> gboolean {
            auto* p = static_cast<Page*>(data);
            if (!g_tabs.empty()) {
                Tab& tab = CurTab();
                tab.page = std::shared_ptr<Page>(p);
                tab.loading = false;
                tab.title = "Page";
                // Extract title from DOM
                if (tab.page->dom) {
                    std::function<std::string(const Node*)> findTitle = [&](const Node* n) -> std::string {
                        if (n->tagName == "title") {
                            for (auto& c : n->children) if (c->type == NodeType::Text) return c->text;
                        }
                        for (auto& c : n->children) { auto t = findTitle(c.get()); if (!t.empty()) return t; }
                        return "";
                    };
                    std::string t = findTitle(tab.page->dom.get());
                    if (!t.empty()) tab.title = t;
                }
                gtk_window_set_title(GTK_WINDOW(g_window), tab.title.c_str());
                gtk_widget_queue_draw(g_drawingArea);
            } else {
                delete p;
            }
            return G_SOURCE_REMOVE;
        }, page);
    }).detach();
}

// ── toolbar buttons ──────────────────────────────────────────────────────────

static void on_back(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    if (g_tabs.empty()) return;
    Tab& tab = CurTab();
    if (tab.histIdx > 0) {
        tab.histIdx--;
        tab.url = tab.history[tab.histIdx];
        gtk_entry_set_text(GTK_ENTRY(g_urlEntry), tab.url.c_str());
        gtk_widget_queue_draw(g_drawingArea);
    }
}

static void on_forward(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    if (g_tabs.empty()) return;
    Tab& tab = CurTab();
    if (tab.histIdx + 1 < (int)tab.history.size()) {
        tab.histIdx++;
        tab.url = tab.history[tab.histIdx];
        gtk_entry_set_text(GTK_ENTRY(g_urlEntry), tab.url.c_str());
        gtk_widget_queue_draw(g_drawingArea);
    }
}

static void on_reload(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    gtk_widget_queue_draw(g_drawingArea);
}

static void on_home(GtkWidget* w, gpointer d) {
    (void)w; (void)d;
    if (g_tabs.empty()) return;
    Tab& tab = CurTab();
    tab.url = "helix://home";
    tab.page = std::make_shared<Page>();
    tab.page->url = "helix://home";
    tab.page->dom = ParseHtml(HomePageHtml());
    tab.title = "Helix";
    gtk_entry_set_text(GTK_ENTRY(g_urlEntry), "helix://home");
    gtk_widget_queue_draw(g_drawingArea);
}

// ── scroll ───────────────────────────────────────────────────────────────────

static gboolean on_scroll(GtkWidget* widget, GdkEventScroll* event, gpointer data) {
    (void)widget; (void)data;
    if (g_tabs.empty()) return FALSE;
    if (event->direction == GDK_SCROLL_UP)        CurTab().scrollY -= 60.f;
    else if (event->direction == GDK_SCROLL_DOWN) CurTab().scrollY += 60.f;
    else if (event->direction == GDK_SCROLL_SMOOTH) CurTab().scrollY -= (float)event->delta_y * 30.f;
    if (CurTab().scrollY < 0) CurTab().scrollY = 0;
    gtk_widget_queue_draw(g_drawingArea);
    return TRUE;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    // Window
    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "Helix");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1280, 800);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // Vertical box: toolbar + drawing area + status bar
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_window), vbox);

    // Toolbar
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(toolbar, 4);
    gtk_widget_set_margin_end(toolbar, 4);
    gtk_widget_set_margin_top(toolbar, 4);
    gtk_widget_set_margin_bottom(toolbar, 4);

    GtkWidget* backBtn   = gtk_button_new_with_label("←");
    GtkWidget* fwdBtn    = gtk_button_new_with_label("→");
    GtkWidget* reloadBtn = gtk_button_new_with_label("↻");
    GtkWidget* homeBtn   = gtk_button_new_with_label("⌂");
    g_signal_connect(backBtn,   "clicked", G_CALLBACK(on_back), NULL);
    g_signal_connect(fwdBtn,    "clicked", G_CALLBACK(on_forward), NULL);
    g_signal_connect(reloadBtn, "clicked", G_CALLBACK(on_reload), NULL);
    g_signal_connect(homeBtn,   "clicked", G_CALLBACK(on_home), NULL);

    g_urlEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_urlEntry), "Enter URL or search...");
    g_signal_connect(g_urlEntry, "activate", G_CALLBACK(on_url_activate), NULL);
    gtk_widget_set_hexpand(g_urlEntry, TRUE);

    gtk_box_pack_start(GTK_BOX(toolbar), backBtn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), fwdBtn,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), reloadBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), homeBtn,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), g_urlEntry, TRUE,  TRUE,  0);

    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    // Drawing area
    g_drawingArea = gtk_drawing_area_new();
    gtk_widget_set_vexpand(g_drawingArea, TRUE);
    gtk_widget_set_hexpand(g_drawingArea, TRUE);
    gtk_widget_add_events(g_drawingArea,
        GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(g_drawingArea, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(g_drawingArea, "scroll-event", G_CALLBACK(on_scroll), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_drawingArea, TRUE, TRUE, 0);

    // Status bar
    g_statusLabel = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_statusLabel), 0);
    gtk_widget_set_margin_start(g_statusLabel, 8);
    gtk_box_pack_start(GTK_BOX(vbox), g_statusLabel, FALSE, FALSE, 0);

    // Initial tab
    g_tabs.emplace_back();
    g_tabs[0].page = std::make_shared<Page>();
    g_tabs[0].page->url = "helix://home";
    g_tabs[0].page->dom = ParseHtml(HomePageHtml());

    gtk_widget_show_all(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
    gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
    // Release keep-above after a brief delay so the window lands on top once
    g_timeout_add(500, [](gpointer data) -> gboolean {
        gtk_window_set_keep_above(GTK_WINDOW(data), FALSE);
        return G_SOURCE_REMOVE;
    }, g_window);
    gtk_main();
    return 0;
}

#endif // __linux__

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

// ── globals ──────────────────────────────────────────────────────────────────

static GtkWidget* g_window;
static GtkWidget* g_urlEntry;
static GtkWidget* g_statusLabel;
static GtkWidget* g_drawingArea;

static std::vector<Tab> g_tabs;
static int g_activeTab = 0;
static JsEngine g_js;
static Semaphore g_imageFetchGate(6);

static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ── drawing ──────────────────────────────────────────────────────────────────

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    (void)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    // White background
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    if (g_tabs.empty()) return FALSE;
    Tab& tab = CurTab();

    if (!tab.page || !tab.page->dom) {
        // Draw loading/placeholder text
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 16);
        cairo_move_to(cr, 20, 30);
        cairo_show_text(cr, tab.loading ? "Loading..." : "Navigate to a URL");
        return FALSE;
    }

    // TODO: Full rendering integration.
    // 1. The LinuxRenderer's SetCairo(cr) must be called here.
    // 2. CollectStylesheet + LayoutDocument + PaintBox through the platform renderer.
    // This requires the Renderer class to be refactored to use IPlatformRenderer.

    // For now, show the page title as proof of life.
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    cairo_move_to(cr, 10, 24);
    std::string msg = "Page loaded: " + tab.title;
    cairo_show_text(cr, msg.c_str());

    return FALSE;
}

// ── URL bar ──────────────────────────────────────────────────────────────────

static void on_url_activate(GtkEntry* entry, gpointer data) {
    (void)data;
    const gchar* text = gtk_entry_get_text(entry);
    if (!text || g_tabs.empty()) return;
    std::string url(text);
    // TODO: call Navigate() from browser_core
    Tab& tab = CurTab();
    tab.url = url;
    tab.title = "Loading...";
    tab.loading = true;
    gtk_widget_queue_draw(g_drawingArea);
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
    gtk_main();
    return 0;
}

#endif // __linux__

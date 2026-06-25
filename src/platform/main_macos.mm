#ifdef __APPLE__
//
// main_macos.mm — macOS application shell for Helix browser.
//
// Creates an NSWindow with a toolbar (back/forward/reload/URL bar), a custom
// NSView for rendering, and drives the browser core (tabs, navigation, etc.).
//
#import <Cocoa/Cocoa.h>
#include "platform/platform.h"
#include "platform/browser_core.h"
#include "platform/box_painter.h"
#include "platform/plat_text_measure.h"
#include "platform/form_state.h"
#include "layout/layout_engine.h"
#include "css/stylesheet.h"

// ── forward declarations ─────────────────────────────────────────────────────

@class HelixView;
@class HelixWindowDelegate;

// ── globals ──────────────────────────────────────────────────────────────────

static NSWindow* g_window;
static NSTextField* g_urlField;
static NSTextField* g_statusField;
static HelixView* g_view;

static std::vector<Tab> g_tabs;
static int g_activeTab = 0;
static JsEngine g_js;
static Semaphore g_imageFetchGate(6);

static std::unique_ptr<IPlatformRenderer> g_renderer;
static std::unique_ptr<PlatTextMeasure> g_measure;
static std::unique_ptr<LayoutBox> g_layoutRoot;
static std::map<std::string, PlatBitmap> g_images;
static std::map<std::string, PlatFont> g_fontCache;
static FormState g_formState;

static Tab& CurTab() { return g_tabs[g_activeTab]; }

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

// ── HelixView (custom NSView for rendering) ──────────────────────────────────

@interface HelixView : NSView
@end

@implementation HelixView

- (BOOL)isFlipped { return YES; }  // top-left origin like Windows

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    NSRect bounds = [self bounds];
    int w = (int)bounds.size.width, h = (int)bounds.size.height;

    if (!g_renderer) {
        g_renderer = CreatePlatformRenderer();
        g_renderer->Init((__bridge void*)self);
        g_measure = std::make_unique<PlatTextMeasure>(g_renderer.get());
    }
    g_renderer->Resize(w, h);
    g_renderer->BeginFrame();
    g_renderer->Clear({1, 1, 1, 1});

    if (g_tabs.empty() || !CurTab().page || !CurTab().page->dom) {
        PlatFont font = g_renderer->CreateFont(16, false, false, false, "");
        g_renderer->DrawText(CurTab().loading ? L"Loading..." : L"Navigate to a URL",
                             20, 20, 800, 30, font, {0.5f, 0.5f, 0.5f, 1});
        g_renderer->ReleaseFont(font);
        g_renderer->EndFrame();
        return;
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
            ps.form = &g_formState;
            PaintBoxTree(ps, *g_layoutRoot);
            tab.docHeight = g_layoutRoot->contentH + 32.f;
        }
    } catch (...) { /* keep the browser alive */ }
    g_renderer->EndFrame();
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent*)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    if (g_layoutRoot && !g_tabs.empty()) {
        Node* input = FormState::hitTestInput(*g_layoutRoot, (float)pt.x, (float)pt.y, CurTab().scrollY, 0);
        if (input) {
            g_formState.focus(input);
            [self setNeedsDisplay:YES];
            return;
        }
        g_formState.blur();
        [self setNeedsDisplay:YES];
    }
}

- (void)keyDown:(NSEvent*)event {
    if (!g_formState.focusedInput) { [super keyDown:event]; return; }
    unsigned short kc = [event keyCode];
    if (kc == 36) { // Return
        std::string url = g_formState.buildFormQuery();
        if (!url.empty()) {
            g_formState.blur();
            if (!url.empty() && url[0] == '/') {
                std::string base = CurTab().page ? CurTab().page->url : "";
                size_t scheme = base.find("://");
                if (scheme != std::string::npos) {
                    size_t slash = base.find('/', scheme + 3);
                    url = base.substr(0, slash) + url;
                }
            }
            [g_urlField setStringValue:[NSString stringWithUTF8String:url.c_str()]];
            // TODO: trigger navigation
        }
        return;
    }
    if (kc == 51) { g_formState.backspace(); [self setNeedsDisplay:YES]; return; } // Backspace
    if (kc == 117) { g_formState.deleteChar(); [self setNeedsDisplay:YES]; return; } // Delete
    if (kc == 123 && g_formState.cursorPos > 0) { g_formState.cursorPos--; [self setNeedsDisplay:YES]; return; } // Left
    if (kc == 124) { // Right
        std::string v = g_formState.getValue(g_formState.focusedInput);
        if (g_formState.cursorPos < v.size()) g_formState.cursorPos++;
        [self setNeedsDisplay:YES]; return;
    }
    if (kc == 53) { g_formState.blur(); [self setNeedsDisplay:YES]; return; } // Escape
    NSString* chars = [event characters];
    if ([chars length] > 0) {
        unichar uc = [chars characterAtIndex:0];
        if (uc >= 32 && uc < 127) {
            g_formState.insertChar((char)uc);
            [self setNeedsDisplay:YES];
            return;
        }
    }
    [super keyDown:event];
}

- (void)scrollWheel:(NSEvent*)event {
    if (g_tabs.empty()) return;
    CurTab().scrollY -= (float)[event scrollingDeltaY] * 3.f;
    if (CurTab().scrollY < 0) CurTab().scrollY = 0;
    [self setNeedsDisplay:YES];
}

@end

// ── HelixWindowDelegate ──────────────────────────────────────────────────────

@interface HelixWindowDelegate : NSObject <NSWindowDelegate, NSTextFieldDelegate>
@end

@implementation HelixWindowDelegate

- (void)windowWillClose:(NSNotification*)notification {
    [NSApp terminate:nil];
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    // URL bar enter pressed
    NSTextField* field = [notification object];
    if (field == g_urlField) {
        std::string url = [[field stringValue] UTF8String];
        if (g_tabs.empty()) return;
        // TODO: call Navigate() from browser_core
        Tab& tab = CurTab();
        tab.url = url;
        tab.title = "Loading...";
        tab.loading = true;
        [g_view setNeedsDisplay:YES];
    }
}

@end

// ── toolbar actions ──────────────────────────────────────────────────────────

@interface ToolbarTarget : NSObject
- (void)goBack:(id)sender;
- (void)goForward:(id)sender;
- (void)reload:(id)sender;
- (void)goHome:(id)sender;
@end

@implementation ToolbarTarget

- (void)goBack:(id)sender {
    if (g_tabs.empty()) return;
    Tab& tab = CurTab();
    if (tab.histIdx > 0) {
        tab.histIdx--;
        tab.url = tab.history[tab.histIdx];
        // TODO: Navigate to tab.url
        [g_view setNeedsDisplay:YES];
    }
}

- (void)goForward:(id)sender {
    if (g_tabs.empty()) return;
    Tab& tab = CurTab();
    if (tab.histIdx + 1 < (int)tab.history.size()) {
        tab.histIdx++;
        tab.url = tab.history[tab.histIdx];
        // TODO: Navigate to tab.url
        [g_view setNeedsDisplay:YES];
    }
}

- (void)reload:(id)sender {
    // TODO: re-navigate current URL
    [g_view setNeedsDisplay:YES];
}

- (void)goHome:(id)sender {
    // TODO: Navigate to helix://home
    [g_view setNeedsDisplay:YES];
}

@end

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Create main menu (required for Cmd+Q to work)
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit Helix" action:@selector(terminate:)
                   keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];
        [NSApp setMainMenu:menubar];

        // Window
        NSRect frame = NSMakeRect(100, 100, 1280, 800);
        g_window = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered
            defer:NO];
        [g_window setTitle:@"Helix"];

        HelixWindowDelegate* delegate = [[HelixWindowDelegate alloc] init];
        [g_window setDelegate:delegate];

        // Content view layout
        NSView* contentView = [g_window contentView];

        // Toolbar area (URL bar + buttons)
        static ToolbarTarget* toolbarTarget = [[ToolbarTarget alloc] init];

        NSButton* backBtn = [NSButton buttonWithTitle:@"←" target:toolbarTarget action:@selector(goBack:)];
        NSButton* fwdBtn  = [NSButton buttonWithTitle:@"→" target:toolbarTarget action:@selector(goForward:)];
        NSButton* reloadBtn = [NSButton buttonWithTitle:@"↻" target:toolbarTarget action:@selector(reload:)];
        NSButton* homeBtn = [NSButton buttonWithTitle:@"⌂" target:toolbarTarget action:@selector(goHome:)];

        g_urlField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 800, 24)];
        [g_urlField setPlaceholderString:@"Enter URL or search..."];
        [g_urlField setDelegate:delegate];

        NSStackView* toolbar = [NSStackView stackViewWithViews:@[backBtn, fwdBtn, reloadBtn, homeBtn, g_urlField]];
        [toolbar setOrientation:NSUserInterfaceLayoutOrientationHorizontal];
        [toolbar setSpacing:4];
        [toolbar setTranslatesAutoresizingMaskIntoConstraints:NO];

        // Browser view
        g_view = [[HelixView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 760)];
        [g_view setTranslatesAutoresizingMaskIntoConstraints:NO];

        // Status bar
        g_statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 1280, 20)];
        [g_statusField setBezeled:NO];
        [g_statusField setEditable:NO];
        [g_statusField setSelectable:NO];
        [g_statusField setBackgroundColor:[NSColor windowBackgroundColor]];
        [g_statusField setFont:[NSFont systemFontOfSize:11]];
        [g_statusField setTextColor:[NSColor secondaryLabelColor]];
        [g_statusField setTranslatesAutoresizingMaskIntoConstraints:NO];

        [contentView addSubview:toolbar];
        [contentView addSubview:g_view];
        [contentView addSubview:g_statusField];

        // Auto Layout constraints
        NSDictionary* views = NSDictionaryOfVariableBindings(toolbar, g_view, g_statusField);
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|[toolbar]|"
            options:0 metrics:nil views:views]];
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|[g_view]|"
            options:0 metrics:nil views:views]];
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"H:|[g_statusField]|"
            options:0 metrics:nil views:views]];
        [contentView addConstraints:[NSLayoutConstraint constraintsWithVisualFormat:@"V:|[toolbar(==36)][g_view][g_statusField(==20)]|"
            options:0 metrics:nil views:views]];

        // Initial tab
        g_tabs.emplace_back();
        g_tabs[0].page = std::make_shared<Page>();
        g_tabs[0].page->url = "helix://home";
        g_tabs[0].page->dom = ParseHtml(HomePageHtml());

        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}

#endif // __APPLE__

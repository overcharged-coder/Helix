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
#include "render/renderer.h"

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

static Tab& CurTab() { return g_tabs[g_activeTab]; }

// ── HelixView (custom NSView for rendering) ──────────────────────────────────

@interface HelixView : NSView
@end

@implementation HelixView

- (BOOL)isFlipped { return YES; }  // top-left origin like Windows

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    // TODO: Drive the layout engine + paint pass here using the platform renderer.
    // The macOS renderer gets its CGContext from [NSGraphicsContext currentContext].
    // For now, fill with white background.
    [[NSColor whiteColor] setFill];
    NSRectFill(dirtyRect);

    if (g_tabs.empty()) return;
    Tab& tab = CurTab();
    if (!tab.page || !tab.page->dom) {
        // Draw loading or error text
        NSString* msg = tab.loading ? @"Loading..." : @"Navigate to a URL";
        NSDictionary* attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:16],
                                 NSForegroundColorAttributeName: [NSColor grayColor] };
        [msg drawAtPoint:NSMakePoint(20, 20) withAttributes:attrs];
        return;
    }

    // The full rendering integration would call:
    // 1. CollectStylesheet from the DOM
    // 2. LayoutDocument with the macOS ITextMeasure
    // 3. PaintBox through the macOS IPlatformRenderer
    // This requires porting the Renderer class to use IPlatformRenderer,
    // which is the next step after the window shells are in place.
}

- (void)mouseDown:(NSEvent*)event {
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    // TODO: hit-test links, dispatch click events
    (void)pt;
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

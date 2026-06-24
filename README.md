# Helix

![Release](https://img.shields.io/github/v/release/overcharged-coder/Helix?style=flat-square)
![Build](https://img.shields.io/github/actions/workflow/status/overcharged-coder/Helix/release.yml?style=flat-square)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20macOS%20%7C%20Linux-blue?style=flat-square)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Engine](https://img.shields.io/badge/engine-from%20scratch-ff69b4?style=flat-square)
![Stars](https://img.shields.io/github/stars/overcharged-coder/Helix?style=flat-square)

**Helix** is a web browser written from scratch in C++17 — its own HTML parser, DOM,
CSS engine, JavaScript engine, layout engine, and renderer. No Chromium, no WebView2,
no CEF, no QtWebEngine. Everything that makes it a browser is hand-built.

It runs on **Windows, macOS, and Linux** from a single shared engine, with a thin
native shell on each platform.

## What's built from scratch

- **HTML** — hand-written tokenizer, parser, and DOM
- **CSS** — cascade with combinators and attribute selectors, custom properties (`var()`),
  media queries, `calc()` / `clamp()` / `min()` / `max()`, viewport units, `box-sizing`,
  `object-fit`, flexbox, grid, tables, floats, positioning, `text-align` (incl. justify),
  `text-indent`, `text-decoration`, and more
- **JavaScript** — lexer → parser → bytecode compiler → VM, with live DOM bindings
- **Layout** — block / inline / float / table / flex / grid box-tree engine
- **Rendering** — per-platform painter over the OS rasterizer
- Tabs, per-tab history, zoom, find-in-page, async image loading

## Dependencies

Only three things are not hand-written:

- **libcurl** — HTTP/HTTPS and TLS (you don't hand-roll TLS)
- **stb_image** — JPEG/PNG/etc. decoding (vendored single header)
- **OS drawing APIs** — Direct2D + DirectWrite (Windows), Core Graphics + Core Text
  (macOS), GTK3 + Cairo + Pango (Linux). These only plot pixels, glyphs, and bitmaps;
  all layout and paint logic is Helix's own.

## Build

Requires **CMake 3.20+** and a C++17 compiler. libcurl is downloaded automatically
via CMake if a system copy isn't found.

### Windows
Visual Studio build tools with the x64 C++ toolchain.

```bat
build.bat
```
```bat
build\Release\Helix.exe
```

### macOS
Xcode command line tools.

```sh
cmake -B build
cmake --build build
open build/Helix.app
```

### Linux
GTK3 development headers (and ideally system libcurl):

```sh
sudo apt-get install -y build-essential cmake libgtk-3-dev libcurl4-openssl-dev pkg-config
cmake -B build
cmake --build build
./build/Helix
```

## Releases

Prebuilt binaries for all three platforms are published on the
[Releases page](https://github.com/overcharged-coder/Helix/releases), built by CI on
every tag.

## Running engine tests

```bat
build\Release\helix-tests.exe html
build\Release\helix-tests.exe css
build\Release\helix-tests.exe layout
build\Release\helix-tests.exe paint
```

Fixtures live under `tests/fixtures` — each has an input file and an expected text
snapshot. When engine behavior changes intentionally, change the implementation first,
inspect the failing output, then update the snapshot only once the new behavior is correct.

## Diagnostics

Two offline tools render without the GUI, useful for debugging:

```sh
build/dump_layout page.html [viewportWidth]   # prints the box tree + geometry
build/dump_js script.js                       # runs the JS engine
```

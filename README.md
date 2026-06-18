# Helix

Helix is a from-scratch C++ web browser experiment for Windows.

It currently has a Win32 shell, a small HTTP fetcher, a hand-built HTML tokenizer/parser, a small DOM, early CSS parsing/cascade support, and a Direct2D/DirectWrite renderer.

Helix does not use Chromium, WebView2, CEF, or QtWebEngine.

## Build

Requirements:

- Windows
- CMake 3.20+
- Visual Studio build tools with the x64 C++ toolchain

Build:

```bat
build.bat
```

Run:

```bat
build\Release\Helix.exe
```

## Running Engine Tests

Build first:

```bat
build.bat
```

Run all current suites:

```bat
build\Release\helix-tests.exe html
build\Release\helix-tests.exe css
build\Release\helix-tests.exe layout
build\Release\helix-tests.exe paint
```

The fixtures live under `tests/fixtures`. Each fixture has an input file and an expected text snapshot. When engine behavior intentionally changes, update the implementation first, inspect the failing output, and then update the expected snapshot only when the new behavior is correct.

## Current Limitations

- No JavaScript.
- No forms.
- No cookies or persistent storage.
- No browser tabs.
- No full HTML tree-construction algorithm.
- No standards-complete CSS parser.
- No flexbox, grid, canvas, media, or WebGL.
- Rendering is intentionally basic while the engine test harness comes online.

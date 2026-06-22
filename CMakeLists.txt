cmake_minimum_required(VERSION 3.20)
project(Helix VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── libcurl (cross-platform HTTP) ─────────────────────────────────────────────
include(FetchContent)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
# Per-OS TLS backend for libcurl
if (WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
elseif (APPLE)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
else ()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif ()
FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(curl)

# Use /EHa everywhere so catch(...) also catches structured exceptions
# (access violations etc.) — a faulting page must never kill the browser.
# Strip the default /EHsc first so it doesn't override /EHa on any target.
if (MSVC)
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/EHs" ""  CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    add_compile_options(/EHa)
    # Read source files as UTF-8 so literal chars (×, •, …) in L"" literals aren't
    # mangled to mojibake (e.g. "Ã—") under the system codepage.
    add_compile_options(/utf-8)
endif()

add_library(helix_engine STATIC
    src/network/fetcher.cpp
    src/network/text_decode.cpp
    src/network/url.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/html/resources.cpp
    src/html/serializer.cpp
    src/css/stylesheet.cpp
    src/layout/layout.cpp
    src/layout/layout_engine.cpp
    src/paint/display_list.cpp
    src/js/gc.cpp
    src/js/lexer.cpp
    src/js/parser.cpp
    src/js/compiler.cpp
    src/js/vm.cpp
    src/js/runtime.cpp
    src/js/dom_bridge.cpp
    src/js/engine.cpp
    src/third_party/stb_image_impl.cpp
)

target_include_directories(helix_engine PUBLIC src)
target_link_libraries(helix_engine PUBLIC CURL::libcurl)
target_compile_definitions(helix_engine PUBLIC CURL_STATICLIB)
if (WIN32)
    target_compile_definitions(helix_engine PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if (MSVC)
    # /EHa: catch SEH exceptions (stack overflow, access violation) in catch(...)
    target_compile_options(helix_engine PUBLIC /EHa)
endif()

# ── platform backends ─────────────────────────────────────────────────────────
# Each OS gets its own renderer + windowing backend. The engine library is
# platform-independent; only these files differ per OS.

if (WIN32)
    add_executable(Helix WIN32
        src/main.cpp
        src/render/renderer.cpp
        src/render/box_paint.cpp
        src/platform/platform_win32.cpp
    )
    target_include_directories(Helix PRIVATE src)
    target_link_libraries(Helix PRIVATE
        helix_engine d2d1 dwrite comctl32 ole32
    )
    if (MSVC)
        target_link_options(Helix PRIVATE /STACK:67108864)
    endif()
elseif (APPLE)
    add_executable(Helix MACOSX_BUNDLE
        src/platform/main_macos.mm
        src/platform/platform_macos.mm
    )
    target_include_directories(Helix PRIVATE src)
    target_link_libraries(Helix PRIVATE
        helix_engine
        "-framework Cocoa"
        "-framework CoreGraphics"
        "-framework CoreText"
    )
    set_target_properties(Helix PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.helix.browser"
        MACOSX_BUNDLE_BUNDLE_NAME "Helix"
    )
elseif (UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    add_executable(Helix
        src/platform/main_linux.cpp
        src/platform/platform_linux.cpp
    )
    target_include_directories(Helix PRIVATE src ${GTK3_INCLUDE_DIRS})
    target_link_libraries(Helix PRIVATE helix_engine ${GTK3_LIBRARIES})
    target_compile_options(Helix PRIVATE ${GTK3_CFLAGS_OTHER})
endif()

add_executable(helix-tests
    src/test/main.cpp
    src/test/fixture.cpp
    src/test/html_tests.cpp
    src/test/css_tests.cpp
    src/test/layout_tests.cpp
    src/test/paint_tests.cpp
    src/test/js_tests.cpp
    src/test/network_tests.cpp
)

target_include_directories(helix-tests PRIVATE src)

target_link_libraries(helix-tests PRIVATE
    helix_engine
)

add_executable(helix-layout-engine-tests
    src/test/layout_engine_main.cpp
    src/test/fixture.cpp
    src/test/layout_engine_tests.cpp
)
target_include_directories(helix-layout-engine-tests PRIVATE src)
target_link_libraries(helix-layout-engine-tests PRIVATE helix_engine)

add_executable(dump_layout tools/dump_layout.cpp)
target_include_directories(dump_layout PRIVATE src)
target_link_libraries(dump_layout PRIVATE helix_engine)

add_executable(dump_js tools/dump_js.cpp)
target_include_directories(dump_js PRIVATE src)
target_link_libraries(dump_js PRIVATE helix_engine)
if (MSVC)
    target_link_options(dump_js PRIVATE /DYNAMICBASE:NO /MAP)
endif()

set_target_properties(Helix helix-tests helix-layout-engine-tests dump_layout dump_js PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

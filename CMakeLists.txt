cmake_minimum_required(VERSION 3.20)
project(Helix VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/network/url.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
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
)

target_include_directories(helix_engine PUBLIC src)
target_compile_definitions(helix_engine PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
if (MSVC)
    # /EHa: catch SEH exceptions (stack overflow, access violation) in catch(...)
    target_compile_options(helix_engine PUBLIC /EHa)
endif()

add_executable(Helix WIN32
    src/main.cpp
    src/render/renderer.cpp
    src/render/box_paint.cpp
)

target_include_directories(Helix PRIVATE src)

target_link_libraries(Helix PRIVATE
    helix_engine d2d1 dwrite windowscodecs wininet comctl32 ole32
)

# Reserve a large stack (64 MB). Real pages can drive the JS engine and DOM
# registration into very deep recursion; the default 1 MB stack overflows and
# crashes (a stack overflow cannot be caught by C++ exception handling).
if (MSVC)
    target_link_options(Helix PRIVATE /STACK:67108864)
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
    helix_engine wininet
)

add_executable(helix-layout-engine-tests
    src/test/layout_engine_main.cpp
    src/test/fixture.cpp
    src/test/layout_engine_tests.cpp
)
target_include_directories(helix-layout-engine-tests PRIVATE src)
target_link_libraries(helix-layout-engine-tests PRIVATE helix_engine wininet)

add_executable(dump_layout tools/dump_layout.cpp)
target_include_directories(dump_layout PRIVATE src)
target_link_libraries(dump_layout PRIVATE helix_engine wininet)

add_executable(dump_js tools/dump_js.cpp)
target_include_directories(dump_js PRIVATE src)
target_link_libraries(dump_js PRIVATE helix_engine wininet)
if (MSVC)
    target_link_options(dump_js PRIVATE /DYNAMICBASE:NO /MAP)
endif()

set_target_properties(Helix helix-tests helix-layout-engine-tests dump_layout dump_js PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

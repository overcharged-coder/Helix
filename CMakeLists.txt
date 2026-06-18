cmake_minimum_required(VERSION 3.20)
project(Helix VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(helix_engine STATIC
    src/network/fetcher.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/html/serializer.cpp
    src/css/stylesheet.cpp
    src/layout/layout.cpp
    src/paint/display_list.cpp
)

target_include_directories(helix_engine PUBLIC src)
target_compile_definitions(helix_engine PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

add_executable(Helix WIN32
    src/main.cpp
    src/render/renderer.cpp
)

target_include_directories(Helix PRIVATE src)

target_link_libraries(Helix PRIVATE
    helix_engine d2d1 dwrite windowscodecs wininet comctl32 ole32
)

add_executable(helix-tests
    src/test/main.cpp
    src/test/fixture.cpp
    src/test/html_tests.cpp
    src/test/css_tests.cpp
    src/test/layout_tests.cpp
    src/test/paint_tests.cpp
)

target_include_directories(helix-tests PRIVATE src)

target_link_libraries(helix-tests PRIVATE
    helix_engine wininet
)

set_target_properties(Helix helix-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

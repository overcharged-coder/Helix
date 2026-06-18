cmake_minimum_required(VERSION 3.20)
project(Helix VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Helix WIN32
    src/main.cpp
    src/network/fetcher.cpp
    src/html/tokenizer.cpp
    src/html/parser.cpp
    src/render/renderer.cpp
)

target_include_directories(Helix PRIVATE src)

target_link_libraries(Helix PRIVATE
    d2d1 dwrite wininet comctl32 ole32
)

target_compile_definitions(Helix PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

set_target_properties(Helix PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

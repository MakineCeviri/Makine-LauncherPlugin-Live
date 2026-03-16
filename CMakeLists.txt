cmake_minimum_required(VERSION 3.25)
project(makineai-live LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_library(makineai-live SHARED
    src/plugin.cpp
    src/live_core.cpp
    src/capture.cpp
    src/ocr_rapid.cpp
    src/translator.cpp
)

target_include_directories(makineai-live PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(makineai-live PRIVATE gdi32 winhttp d3d11 dxgi)
endif()

set_target_properties(makineai-live PROPERTIES
    PREFIX ""
    OUTPUT_NAME "makineai-live"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
)

configure_file(${CMAKE_SOURCE_DIR}/manifest.json ${CMAKE_BINARY_DIR}/release/manifest.json COPYONLY)

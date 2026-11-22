cmake_minimum_required(VERSION 3.25)
project(makine-live VERSION 0.3.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prebuilt OCR runtime (RapidOcrOnnx.dll, onnxruntime.dll, models/). Not in git — see docs/MIMARI.md §6.
set(MAKINE_LIVE_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/third_party/rapidocr" CACHE PATH
    "Folder containing RapidOcrOnnx.dll, onnxruntime.dll and models/")

add_library(makine-live SHARED
    src/plugin.cpp
    src/live_core.cpp
    src/capture.cpp
    src/ocr_rapid.cpp
    src/translator.cpp
    src/settings.cpp
    src/text_fix.cpp
)

target_include_directories(makine-live PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(WIN32)
    target_compile_definitions(makine-live PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(makine-live PRIVATE gdi32 winhttp d3d11 dxgi crypt32)
endif()

if(MINGW)
    # Self-contained DLL: the launcher does not ship libstdc++/libgcc/libwinpthread.
    target_link_options(makine-live PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(makine-live PRIVATE -Wall -Wextra)
endif()

set_target_properties(makine-live PROPERTIES
    PREFIX ""
    OUTPUT_NAME "makine-live"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release"
)

configure_file(${CMAKE_SOURCE_DIR}/manifest.json ${CMAKE_BINARY_DIR}/release/manifest.json COPYONLY)
# Launcher plugin icon (generated from assets/logo_white-96.webp by tools/brand_assets.py)
configure_file(${CMAKE_SOURCE_DIR}/resources/icon.png ${CMAKE_BINARY_DIR}/release/icon.png COPYONLY)
# English word list for OCR space restoration (src/text_fix.cpp)
configure_file(${CMAKE_SOURCE_DIR}/resources/en_words.txt ${CMAKE_BINARY_DIR}/release/resources/en_words.txt COPYONLY)

# Unit test for the OCR space restoration: ctest --test-dir build
enable_testing()
add_executable(text_fix_test tests/text_fix_test.cpp src/text_fix.cpp)
target_include_directories(text_fix_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(MINGW)
    target_link_options(text_fix_test PRIVATE -static)
endif()
add_test(NAME text_fix COMMAND text_fix_test ${CMAKE_SOURCE_DIR}/resources/en_words.txt)

# Copy the OCR runtime next to the DLL so build/release is exactly the .makine package content.
if(EXISTS "${MAKINE_LIVE_RUNTIME_DIR}/RapidOcrOnnx.dll")
    add_custom_command(TARGET makine-live POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MAKINE_LIVE_RUNTIME_DIR}/RapidOcrOnnx.dll"
            "${MAKINE_LIVE_RUNTIME_DIR}/onnxruntime.dll"
            "${CMAKE_BINARY_DIR}/release"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${MAKINE_LIVE_RUNTIME_DIR}/models" "${CMAKE_BINARY_DIR}/release/models"
        COMMENT "Copying OCR runtime from ${MAKINE_LIVE_RUNTIME_DIR}")
else()
    message(WARNING "OCR runtime not found in ${MAKINE_LIVE_RUNTIME_DIR}; pack.py will refuse to package.")
endif()

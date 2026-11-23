cmake_minimum_required(VERSION 3.25)

# The version lives in manifest.json only.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json" MANIFEST_JSON)
string(JSON MANIFEST_VERSION GET "${MANIFEST_JSON}" version)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" PROJECT_NUMERIC_VERSION "${MANIFEST_VERSION}")
project(makine-live VERSION ${PROJECT_NUMERIC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Ninja, Makefiles) build unoptimised without a type.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link the C++ runtime into the DLL. The plugin host looks for DLLs only in the
# plugin's folder, the launcher's folder and System32, so a runtime DLL we do not
# ship would be the launcher's copy, in whatever version it has.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# nlohmann/json (MIT), header-only, pinned by hash.
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    SYSTEM)
FetchContent_MakeAvailable(json)

set(LIVE_DEFINES UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
    MAKINE_LIVE_VERSION="${MANIFEST_VERSION}")

function(live_warnings target)
    if(MINGW)
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)   # Turkish text in string literals
    endif()
endfunction()

# Text rules, subtitle timing, console geometry, local pool: ports of the Python harness,
# tested on their own (tests/cpp) before anything can talk to the shared pool.
add_library(live-logic STATIC
    src/unitext.cpp
    src/garbage.cpp
    src/subtitle_timing.cpp
    src/console_mode.cpp
    src/local_pool.cpp
    src/text_fix.cpp
)
target_include_directories(live-logic PUBLIC src)
target_compile_definitions(live-logic PUBLIC ${LIVE_DEFINES})
target_link_libraries(live-logic PUBLIC nlohmann_json::nlohmann_json user32 gdi32 dwmapi)
live_warnings(live-logic)

# ── makine-live.dll ─────────────────────────────────────────────────────────
# One DLL with two faces: makine_plugin_entry (ABI 2, the launcher; plugin.cpp + engine.cpp)
# and the ABI 1 exports the Python test harness loads with ctypes (abi1.cpp + live_core.cpp).
add_library(makine-live SHARED
    src/plugin.cpp
    src/engine.cpp
    src/server.cpp
    src/abi1.cpp
    src/live_core.cpp
    src/capture.cpp
    src/ocr_rapid.cpp
    src/translator.cpp
    src/settings.cpp
)
target_include_directories(makine-live PRIVATE include src)
target_link_libraries(makine-live PRIVATE live-logic d3d11 dxgi winhttp crypt32 bcrypt)
set_target_properties(makine-live PROPERTIES PREFIX "")   # makine-live.dll, not libmakine-live.dll
live_warnings(makine-live)
if(MINGW)
    # Self-contained: the plugin host never searches PATH, and the launcher's libstdc++ is not ours
    target_link_options(makine-live PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# build/package/ is the plugin as Makine bundles and installs it:
#   manifest.json, bin/makine-live.dll (the manifest's "entry"), bin/RapidOcrOnnx.dll,
#   models/, resources/en_words.txt, licenses/
# build/release/ is the flat layout the Python harness loads (makine_live/paths.py).
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
set(RELEASE_DIR "${CMAKE_BINARY_DIR}/release")
string(JSON PLUGIN_ENTRY GET "${MANIFEST_JSON}" entry)
configure_file(manifest.json "${PACKAGE_DIR}/manifest.json" COPYONLY)
configure_file(resources/en_words.txt "${PACKAGE_DIR}/resources/en_words.txt" COPYONLY)
configure_file(LICENSE "${PACKAGE_DIR}/licenses/Makine-Live-LICENSE.txt" COPYONLY)
configure_file(licenses/NOTICE.txt "${PACKAGE_DIR}/licenses/NOTICE.txt" COPYONLY)
configure_file("${json_SOURCE_DIR}/LICENSE.MIT" "${PACKAGE_DIR}/licenses/nlohmann-json-LICENSE.txt" COPYONLY)
configure_file(manifest.json "${RELEASE_DIR}/manifest.json" COPYONLY)
configure_file(resources/icon.png "${RELEASE_DIR}/icon.png" COPYONLY)
configure_file(resources/en_words.txt "${RELEASE_DIR}/resources/en_words.txt" COPYONLY)
add_custom_command(TARGET makine-live POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:makine-live>" "${PACKAGE_DIR}/${PLUGIN_ENTRY}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:makine-live>" "${RELEASE_DIR}/makine-live.dll"
    VERBATIM)

# The OCR runtime and models: python tools/fetch_ocr.py downloads them here.
set(LIVE_OCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ocr" CACHE PATH "Output of tools/fetch_ocr.py")
set(STAGE_OCR_SCRIPT -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_ocr.cmake")   # -D must come before -P
add_custom_target(stage-ocr ALL
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${LIVE_OCR_DIR}" "-DDST=${PACKAGE_DIR}" ${STAGE_OCR_SCRIPT}
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${LIVE_OCR_DIR}" "-DDST=${RELEASE_DIR}" -DFLAT=ON ${STAGE_OCR_SCRIPT}
    VERBATIM)

# cmake --build build --target pack  ->  build/<id>-<version>.mkplugin
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(pack
        COMMAND "${CMAKE_COMMAND}" "-DSRC=${LIVE_OCR_DIR}" "-DDST=${PACKAGE_DIR}" -DREQUIRE=ON ${STAGE_OCR_SCRIPT}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/plugin_registry.py"
                pack "${PACKAGE_DIR}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        VERBATIM)
    add_dependencies(pack makine-live)
else()
    message(STATUS "Python 3 not found: the pack target is not available")
endif()

# ── Tests: ctest --test-dir build ───────────────────────────────────────────
enable_testing()
add_executable(text_fix_test tests/text_fix_test.cpp)
target_link_libraries(text_fix_test PRIVATE live-logic)
add_executable(live_logic_test tests/cpp/test_logic.cpp)
target_link_libraries(live_logic_test PRIVATE live-logic)
live_warnings(live_logic_test)
if(MINGW)
    target_link_options(text_fix_test PRIVATE -static)
    target_link_options(live_logic_test PRIVATE -static)
endif()
add_test(NAME text_fix COMMAND text_fix_test ${CMAKE_SOURCE_DIR}/resources/en_words.txt)
foreach(case normalization garbage ocr_filter subtitle console local_pool)
    add_test(NAME ${case} COMMAND live_logic_test ${case} ${CMAKE_SOURCE_DIR})
endforeach()

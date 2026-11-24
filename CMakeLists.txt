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
    src/discord.cpp
    src/hotkeys.cpp
    src/scene.cpp
    src/subtitle_web.cpp
    src/tv_link.cpp
    src/adb.cpp
    src/gzip_store.cpp
    src/text_mask.cpp
    src/subtitle_finder.cpp
    src/fit.cpp
)
target_include_directories(live-logic PUBLIC src)
target_compile_definitions(live-logic PUBLIC ${LIVE_DEFINES})
target_link_libraries(live-logic PUBLIC nlohmann_json::nlohmann_json user32 gdi32 dwmapi ws2_32 bcrypt crypt32)
live_warnings(live-logic)

# The OCR runtime, models and the onnxruntime C API header: python tools/fetch_ocr.py puts them here.
set(LIVE_OCR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ocr" CACHE PATH "Output of tools/fetch_ocr.py")
if(NOT EXISTS "${LIVE_OCR_DIR}/include/onnxruntime_c_api.h")
    message(FATAL_ERROR "onnxruntime_c_api.h not in ${LIVE_OCR_DIR}/include: run `python tools/fetch_ocr.py`")
endif()

# ── makine-live.dll ─────────────────────────────────────────────────────────
# makine_plugin_entry (plugin ABI 2). OCR on onnxruntime's C API (ocr_ort.cpp): nothing links
# against onnxruntime, the plugin loads bin/onnxruntime.dll at run time.
add_library(makine-live SHARED
    src/plugin.cpp
    src/engine.cpp
    src/engine_outputs.cpp
    src/server.cpp
    src/capture.cpp
    src/ocr_ort.cpp
    src/finder_ocr.cpp
    src/settings.cpp
)
target_include_directories(makine-live PRIVATE include src)
target_include_directories(makine-live SYSTEM PRIVATE "${LIVE_OCR_DIR}/include")
target_link_libraries(makine-live PRIVATE live-logic d3d11 dxgi winhttp crypt32 bcrypt advapi32 shell32 ole32 shcore)
set_target_properties(makine-live PROPERTIES PREFIX "")   # makine-live.dll, not libmakine-live.dll
live_warnings(makine-live)
if(MINGW)
    # Self-contained: the plugin host never searches PATH, and the launcher's libstdc++ is not ours
    target_link_options(makine-live PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# build/package/ is the plugin as Makine bundles and installs it:
#   manifest.json, bin/makine-live.dll (the manifest's "entry"), bin/onnxruntime.dll (Microsoft-signed)
#   with the VC++ runtime it needs, models/, resources/, licenses/
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
string(JSON PLUGIN_ENTRY GET "${MANIFEST_JSON}" entry)
configure_file(manifest.json "${PACKAGE_DIR}/manifest.json" COPYONLY)
configure_file(resources/en_words.txt "${PACKAGE_DIR}/resources/en_words.txt" COPYONLY)
configure_file(spec/sample_quotes.json "${PACKAGE_DIR}/resources/sample_quotes.json" COPYONLY)   # "Örnek göster"
configure_file(assets/logo_white-96.webp "${PACKAGE_DIR}/resources/logo.webp" COPYONLY)   # phone / OBS pages
configure_file(LICENSE "${PACKAGE_DIR}/licenses/Makine-Live-LICENSE.txt" COPYONLY)
configure_file(licenses/NOTICE.txt "${PACKAGE_DIR}/licenses/NOTICE.txt" COPYONLY)
configure_file("${json_SOURCE_DIR}/LICENSE.MIT" "${PACKAGE_DIR}/licenses/nlohmann-json-LICENSE.txt" COPYONLY)
add_custom_command(TARGET makine-live POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:makine-live>" "${PACKAGE_DIR}/${PLUGIN_ENTRY}"
    VERBATIM)

set(STAGE_OCR_SCRIPT -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_ocr.cmake")   # -D must come before -P
add_custom_target(stage-ocr ALL
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${LIVE_OCR_DIR}" "-DDST=${PACKAGE_DIR}" ${STAGE_OCR_SCRIPT}
    VERBATIM)

# Makine TV packages for "TV'ye kur": the LG .ipk (tools/build_webos.py, pure Python, ms) and the
# Android TV APK (tv\gradlew.bat assembleRelease; release-signed only with MAKINE_TV_KEYSTORE set).
set(LIVE_TV_APK "${CMAKE_CURRENT_SOURCE_DIR}/tv/app/build/outputs/apk/release/app-release.apk" CACHE FILEPATH
    "Makine TV APK put into the package")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(stage-tv ALL
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_webos.py" "${PACKAGE_DIR}/resources/makine-tv.ipk"
        COMMAND "${CMAKE_COMMAND}" "-DSRC=${LIVE_TV_APK}" "-DDST=${PACKAGE_DIR}/resources/makine-tv.apk"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_file.cmake"
        VERBATIM)
endif()

# cmake --build build --target pack  ->  build/<id>-<version>.mkplugin
if(Python3_FOUND)
    add_custom_target(pack
        COMMAND "${CMAKE_COMMAND}" "-DSRC=${LIVE_OCR_DIR}" "-DDST=${PACKAGE_DIR}" -DREQUIRE=ON ${STAGE_OCR_SCRIPT}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/plugin_registry.py"
                pack "${PACKAGE_DIR}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        VERBATIM)
    add_dependencies(pack makine-live stage-tv)
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
foreach(case normalization garbage ocr_filter subtitle console local_pool speaker line_issues clipped_edge fuzzy_match scene)
    add_test(NAME ${case} COMMAND live_logic_test ${case} ${CMAKE_SOURCE_DIR})
endforeach()
add_executable(live_outputs_test tests/cpp/test_outputs.cpp)
target_link_libraries(live_outputs_test PRIVATE live-logic)
live_warnings(live_outputs_test)
if(MINGW)
    target_link_options(live_outputs_test PRIVATE -static)
endif()
foreach(case discord discord_pipe gzip adb_key web web_limits hotkeys tv_link)
    add_test(NAME ${case} COMMAND live_outputs_test ${case})
endforeach()
add_executable(live_fit_test tests/cpp/test_fit.cpp)
target_link_libraries(live_fit_test PRIVATE live-logic)
live_warnings(live_fit_test)
if(MINGW)
    target_link_options(live_fit_test PRIVATE -static)
endif()
foreach(case fit finder temp_grow)
    add_test(NAME ${case} COMMAND live_fit_test ${case})
endforeach()

# F1 gate: the plugin's OCR reads every benchmark frame exactly as the benchmark's ORT path did
# (OCR_KARAR; the frames and results live in the ocrbench repo's build/ocr_bench).
set(LIVE_OCR_BENCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Makine-LauncherPlugin-Live-ocrbench/build/ocr_bench" CACHE PATH
    "build/ocr_bench of the ocrbench repo (frames/, results/ort_v3ch_det.tsv)")
add_executable(live_ocr_gate tests/cpp/ocr_gate.cpp src/ocr_ort.cpp)
target_include_directories(live_ocr_gate PRIVATE src)
target_include_directories(live_ocr_gate SYSTEM PRIVATE "${LIVE_OCR_DIR}/include")
target_link_libraries(live_ocr_gate PRIVATE live-logic windowscodecs ole32)
live_warnings(live_ocr_gate)
if(MINGW)
    target_link_options(live_ocr_gate PRIVATE -static)
endif()
if(EXISTS "${LIVE_OCR_BENCH_DIR}/results/ort_v3ch_det.tsv")
    foreach(threads 1 2)
        add_test(NAME ocr_gate_${threads}t COMMAND live_ocr_gate "${LIVE_OCR_BENCH_DIR}" "${LIVE_OCR_DIR}" ${threads}
                 "${CMAKE_SOURCE_DIR}/resources/en_words.txt")
    endforeach()
else()
    message(STATUS "OCR gate skipped: no benchmark results in ${LIVE_OCR_BENCH_DIR}")
endif()

# Subtitle-finder gate on whole screens (python tools/make_scene_frames.py --bench <ocrbench>)
set(LIVE_SCENE_DIR "${CMAKE_BINARY_DIR}/scene_frames" CACHE PATH "Scene frames (tools/make_scene_frames.py)")
add_executable(live_finder_gate tests/cpp/finder_gate.cpp src/finder_ocr.cpp src/ocr_ort.cpp)
target_include_directories(live_finder_gate PRIVATE src)
target_include_directories(live_finder_gate SYSTEM PRIVATE "${LIVE_OCR_DIR}/include")
target_link_libraries(live_finder_gate PRIVATE live-logic windowscodecs ole32)
live_warnings(live_finder_gate)
if(MINGW)
    target_link_options(live_finder_gate PRIVATE -static)
endif()
if(EXISTS "${LIVE_SCENE_DIR}/truth.json")
    foreach(threads 1 2)
        add_test(NAME finder_gate_${threads}t COMMAND live_finder_gate "${LIVE_SCENE_DIR}" "${LIVE_OCR_DIR}" ${threads})
    endforeach()
else()
    message(STATUS "Finder gate skipped: no scene frames in ${LIVE_SCENE_DIR}")
endif()

# K3 gate: the console-mode text mask on D-C (python tools/make_dc_frames.py --bench <ocrbench>)
set(LIVE_OCR_DC_DIR "${CMAKE_BINARY_DIR}/ocr_dc" CACHE PATH "D-C frames (tools/make_dc_frames.py)")
add_executable(live_mask_gate tests/cpp/mask_gate.cpp)
target_link_libraries(live_mask_gate PRIVATE live-logic windowscodecs ole32)
live_warnings(live_mask_gate)
if(MINGW)
    target_link_options(live_mask_gate PRIVATE -static)
endif()
add_test(NAME mask_gate COMMAND live_mask_gate "${LIVE_OCR_DC_DIR}")

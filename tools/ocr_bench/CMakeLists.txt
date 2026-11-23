# OCR benchmark harness — standalone project (not part of the plugin build).
#   cmake -S tools/ocr_bench -B build/ocr_bench -G Ninja -DCMAKE_BUILD_TYPE=Release
#   bash tools/ocr_bench/run_all.sh
cmake_minimum_required(VERSION 3.25)
project(ocr_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(LIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
# onnxruntime_c_api.h from the official Microsoft.ML.OnnxRuntime.DirectML nupkg (fetch_models.sh); the DLLs are
# loaded at run time, nothing links against onnxruntime.
set(OCR_BENCH_ORT_INCLUDE "${LIVE_ROOT}/third_party/ocr_bench/include" CACHE PATH "onnxruntime headers")
if(NOT EXISTS "${OCR_BENCH_ORT_INCLUDE}/onnxruntime_c_api.h")
    message(FATAL_ERROR "onnxruntime_c_api.h not found — run: bash tools/ocr_bench/fetch_models.sh")
endif()

add_executable(ocr_bench
    src/main.cpp
    src/common.cpp
    src/ort_ocr.cpp
    src/rapid_dll.cpp
    src/report.cpp
    ${LIVE_ROOT}/src/text_fix.cpp)
target_include_directories(ocr_bench PRIVATE src ${LIVE_ROOT}/src)
target_include_directories(ocr_bench SYSTEM PRIVATE ${OCR_BENCH_ORT_INCLUDE})
target_compile_definitions(ocr_bench PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ocr_bench PRIVATE windowscodecs ole32 advapi32)
if(MINGW)
    target_link_options(ocr_bench PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ocr_bench PRIVATE -Wall -Wextra)
endif()

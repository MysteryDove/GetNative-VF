cmake_minimum_required(VERSION 3.20)

project(getnative_engine VERSION 0.2.1 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
option(GETNATIVE_BUILD_UPSTREAM_CONFORMANCE
       "Build test-only planner bitwise and output numerical comparisons against pinned descale and zimg" OFF)
option(GETNATIVE_ENABLE_METAL "Build the Metal analysis backend on macOS" ${APPLE})
option(GETNATIVE_ENABLE_CUDA
       "Build the new generic CUDA C++ baseline backend on Windows or Linux" OFF)
option(GETNATIVE_ENABLE_VULKAN
       "Build the Vulkan 1.2 compute analysis backend on Windows or Linux" OFF)
option(GETNATIVE_ENABLE_MEDIA
       "Build in-process FFmpeg 8 media indexing and decode support" ON)
set(GETNATIVE_FFMPEG_ROOT "" CACHE PATH
    "Root of a self-contained FFmpeg SDK (include/, lib/, and runtime libraries)")
if(NOT "${GETNATIVE_FFMPEG_DISCOVERY_ROOT}" STREQUAL "${GETNATIVE_FFMPEG_ROOT}")
    unset(GETNATIVE_FFMPEG_INCLUDE_DIR CACHE)
    foreach(GETNATIVE_FFMPEG_COMPONENT IN ITEMS AVFORMAT AVCODEC AVUTIL SWSCALE)
        unset(GETNATIVE_FFMPEG_${GETNATIVE_FFMPEG_COMPONENT}_LIBRARY CACHE)
    endforeach()
    set(GETNATIVE_FFMPEG_DISCOVERY_ROOT "${GETNATIVE_FFMPEG_ROOT}"
        CACHE INTERNAL "FFmpeg root used for the cached discovery results" FORCE)
endif()
option(GETNATIVE_BUILD_CUDA_AUTOTUNER
       "Build the standalone distributable CUDA launch-policy autotuner" OFF)
set(GETNATIVE_PLANNER_FP_MODE "fast" CACHE STRING
    "Floating-point mode for planner math (fast, strict, or precise; precise is MSVC-only)")
set_property(CACHE GETNATIVE_PLANNER_FP_MODE PROPERTY STRINGS fast strict precise)
string(TOLOWER "${GETNATIVE_PLANNER_FP_MODE}" GETNATIVE_PLANNER_FP_MODE)
if(NOT GETNATIVE_PLANNER_FP_MODE MATCHES "^(fast|strict|precise)$")
    message(FATAL_ERROR
        "GETNATIVE_PLANNER_FP_MODE must be fast, strict, or precise")
endif()
if(GETNATIVE_PLANNER_FP_MODE STREQUAL "precise" AND NOT MSVC)
    message(FATAL_ERROR
        "GETNATIVE_PLANNER_FP_MODE=precise currently requires MSVC")
endif()
set(GETNATIVE_PLANNER_STRUCTURE_MODE "optimized" CACHE STRING
    "Planner structure experiment (tap-recompute, baseline, geometry, scratch, transpose, profiled, workers, or optimized)")
set_property(CACHE GETNATIVE_PLANNER_STRUCTURE_MODE PROPERTY STRINGS
    tap-recompute baseline geometry scratch transpose profiled workers optimized)
string(TOLOWER "${GETNATIVE_PLANNER_STRUCTURE_MODE}"
       GETNATIVE_PLANNER_STRUCTURE_MODE)
if(NOT GETNATIVE_PLANNER_STRUCTURE_MODE MATCHES
   "^(tap-recompute|baseline|geometry|scratch|transpose|profiled|workers|optimized)$")
    message(FATAL_ERROR
        "GETNATIVE_PLANNER_STRUCTURE_MODE must be tap-recompute, baseline, geometry, scratch, transpose, profiled, workers, or optimized")
endif()

set(GETNATIVE_PLANNER_REUSE_TAPS 1)
set(GETNATIVE_PLANNER_FAST_INTERIOR 0)
set(GETNATIVE_PLANNER_REUSE_SCRATCH 0)
set(GETNATIVE_PLANNER_DIRECT_TRANSPOSE 0)
set(GETNATIVE_PLANNER_PERSISTENT_WORKERS 0)
if(GETNATIVE_PLANNER_STRUCTURE_MODE STREQUAL "tap-recompute")
    set(GETNATIVE_PLANNER_REUSE_TAPS 0)
elseif(GETNATIVE_PLANNER_STRUCTURE_MODE STREQUAL "geometry")
    set(GETNATIVE_PLANNER_FAST_INTERIOR 1)
elseif(GETNATIVE_PLANNER_STRUCTURE_MODE STREQUAL "scratch")
    set(GETNATIVE_PLANNER_REUSE_SCRATCH 1)
elseif(GETNATIVE_PLANNER_STRUCTURE_MODE STREQUAL "transpose")
    set(GETNATIVE_PLANNER_DIRECT_TRANSPOSE 1)
elseif(GETNATIVE_PLANNER_STRUCTURE_MODE MATCHES "^(profiled|workers|optimized)$")
    set(GETNATIVE_PLANNER_FAST_INTERIOR 1)
    set(GETNATIVE_PLANNER_REUSE_SCRATCH 1)
    set(GETNATIVE_PLANNER_DIRECT_TRANSPOSE 1)
    if(NOT GETNATIVE_PLANNER_STRUCTURE_MODE STREQUAL "profiled")
        set(GETNATIVE_PLANNER_PERSISTENT_WORKERS 1)
    endif()
endif()

set(GETNATIVE_CUDA_MIN_ARCHITECTURE "75" CACHE STRING
    "Minimum compute capability accepted by the CUDA backend")
set(GETNATIVE_CUDA_ARCHITECTURES
    "75;86;89;120" CACHE STRING
    "Native SM targets embedded in the CUDA fatbin")
set(GETNATIVE_CUDA_PTX_ARCHITECTURES "75;120" CACHE STRING
    "Virtual compute targets embedded as CUDA PTX fallbacks")
set(GETNATIVE_CUDA_ROOT "" CACHE PATH
    "CUDA Toolkit root (auto-detected from common environment variables and install paths when empty)")
if(GETNATIVE_ENABLE_CUDA
   AND NOT WIN32
   AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "GETNATIVE_ENABLE_CUDA currently requires Windows or Linux")
endif()
if(GETNATIVE_ENABLE_VULKAN
   AND NOT WIN32
   AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN currently requires Windows or Linux")
endif()
if(GETNATIVE_BUILD_CUDA_AUTOTUNER AND NOT GETNATIVE_ENABLE_CUDA)
    message(FATAL_ERROR
        "GETNATIVE_BUILD_CUDA_AUTOTUNER requires GETNATIVE_ENABLE_CUDA=ON")
endif()

if(GETNATIVE_ENABLE_MEDIA)
    if(GETNATIVE_FFMPEG_ROOT)
        find_path(GETNATIVE_FFMPEG_INCLUDE_DIR
            NAMES libavformat/avformat.h
            PATHS "${GETNATIVE_FFMPEG_ROOT}/include"
            NO_DEFAULT_PATH REQUIRED)
        foreach(GETNATIVE_FFMPEG_VERSION_CHECK IN ITEMS
                "libavformat/version_major.h;LIBAVFORMAT_VERSION_MAJOR;62"
                "libavcodec/version_major.h;LIBAVCODEC_VERSION_MAJOR;62"
                "libavutil/version.h;LIBAVUTIL_VERSION_MAJOR;60"
                "libswscale/version_major.h;LIBSWSCALE_VERSION_MAJOR;9")
            list(GET GETNATIVE_FFMPEG_VERSION_CHECK 0 GETNATIVE_FFMPEG_VERSION_HEADER)
            list(GET GETNATIVE_FFMPEG_VERSION_CHECK 1 GETNATIVE_FFMPEG_VERSION_MACRO)
            list(GET GETNATIVE_FFMPEG_VERSION_CHECK 2 GETNATIVE_FFMPEG_VERSION_EXPECTED)
            file(STRINGS
                "${GETNATIVE_FFMPEG_INCLUDE_DIR}/${GETNATIVE_FFMPEG_VERSION_HEADER}"
                GETNATIVE_FFMPEG_VERSION_LINE
                REGEX "^#define[ \t]+${GETNATIVE_FFMPEG_VERSION_MACRO}[ \t]+[0-9]+")
            if(NOT GETNATIVE_FFMPEG_VERSION_LINE MATCHES
               "[ \t]${GETNATIVE_FFMPEG_VERSION_EXPECTED}$")
                message(FATAL_ERROR
                    "${GETNATIVE_FFMPEG_VERSION_MACRO} must be ${GETNATIVE_FFMPEG_VERSION_EXPECTED}; found '${GETNATIVE_FFMPEG_VERSION_LINE}'")
            endif()
        endforeach()
        foreach(GETNATIVE_FFMPEG_COMPONENT avformat avcodec avutil swscale)
            string(TOUPPER "${GETNATIVE_FFMPEG_COMPONENT}"
                   GETNATIVE_FFMPEG_COMPONENT_UPPER)
            find_library(GETNATIVE_FFMPEG_${GETNATIVE_FFMPEG_COMPONENT_UPPER}_LIBRARY
                NAMES ${GETNATIVE_FFMPEG_COMPONENT} lib${GETNATIVE_FFMPEG_COMPONENT}
                PATHS "${GETNATIVE_FFMPEG_ROOT}/lib"
                NO_DEFAULT_PATH REQUIRED)
            list(APPEND GETNATIVE_FFMPEG_LIBRARIES
                "${GETNATIVE_FFMPEG_${GETNATIVE_FFMPEG_COMPONENT_UPPER}_LIBRARY}")
        endforeach()
        add_library(getnative_ffmpeg INTERFACE)
        target_include_directories(getnative_ffmpeg SYSTEM INTERFACE
            "${GETNATIVE_FFMPEG_INCLUDE_DIR}")
        target_link_libraries(getnative_ffmpeg INTERFACE
            ${GETNATIVE_FFMPEG_LIBRARIES})
        if(WIN32)
            set(GETNATIVE_FFMPEG_RUNTIME_OUTPUTS)
            foreach(GETNATIVE_FFMPEG_RUNTIME_LIBRARY IN ITEMS
                    avformat-62.dll avcodec-62.dll avutil-60.dll swscale-9.dll)
                set(GETNATIVE_FFMPEG_RUNTIME_SOURCE
                    "${GETNATIVE_FFMPEG_ROOT}/bin/${GETNATIVE_FFMPEG_RUNTIME_LIBRARY}")
                set(GETNATIVE_FFMPEG_RUNTIME_OUTPUT
                    "${CMAKE_CURRENT_BINARY_DIR}/${GETNATIVE_FFMPEG_RUNTIME_LIBRARY}")
                if(NOT EXISTS "${GETNATIVE_FFMPEG_RUNTIME_SOURCE}")
                    message(FATAL_ERROR
                        "FFmpeg runtime library is missing: ${GETNATIVE_FFMPEG_RUNTIME_SOURCE}")
                endif()
                add_custom_command(
                    OUTPUT "${GETNATIVE_FFMPEG_RUNTIME_OUTPUT}"
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                            "${GETNATIVE_FFMPEG_RUNTIME_SOURCE}"
                            "${GETNATIVE_FFMPEG_RUNTIME_OUTPUT}"
                    DEPENDS "${GETNATIVE_FFMPEG_RUNTIME_SOURCE}"
                    VERBATIM)
                list(APPEND GETNATIVE_FFMPEG_RUNTIME_OUTPUTS
                    "${GETNATIVE_FFMPEG_RUNTIME_OUTPUT}")
            endforeach()
            add_custom_target(getnative_ffmpeg_runtime
                DEPENDS ${GETNATIVE_FFMPEG_RUNTIME_OUTPUTS})
        endif()
        set(GETNATIVE_FFMPEG_TARGET getnative_ffmpeg)
        set(GETNATIVE_FFMPEG_FOUND ON)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(GETNATIVE_FFMPEG QUIET IMPORTED_TARGET
                libavformat>=62 libavformat<63
                libavcodec>=62 libavcodec<63
                libavutil>=60 libavutil<61
                libswscale>=9 libswscale<10)
        endif()
        if(GETNATIVE_FFMPEG_FOUND)
            set(GETNATIVE_FFMPEG_TARGET PkgConfig::GETNATIVE_FFMPEG)
        endif()
    endif()
    if(NOT GETNATIVE_FFMPEG_FOUND)
        message(WARNING
            "FFmpeg 8 development libraries were not found; verify_media_begin will be disabled")
        set(GETNATIVE_ENABLE_MEDIA OFF)
    endif()
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" GETNATIVE_SYSTEM_PROCESSOR_LOWER)
set(GETNATIVE_TARGET_X86_64 OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8
   AND GETNATIVE_SYSTEM_PROCESSOR_LOWER MATCHES "^(amd64|x86_64|x64)$")
    set(GETNATIVE_TARGET_X86_64 ON)
endif()
option(GETNATIVE_ENABLE_X86_SIMD
       "Build strict SSE2 and AVX2 CPU execution tiers on x86-64"
       ${GETNATIVE_TARGET_X86_64})
set(GETNATIVE_X86_AVX512_DEFAULT OFF)
if(WIN32 AND GETNATIVE_TARGET_X86_64)
    set(GETNATIVE_X86_AVX512_DEFAULT ON)
endif()
option(GETNATIVE_ENABLE_X86_AVX512
       "Build the strict AVX-512 CPU execution tier"
       ${GETNATIVE_X86_AVX512_DEFAULT})

if(GETNATIVE_ENABLE_X86_SIMD AND NOT GETNATIVE_TARGET_X86_64)
    message(FATAL_ERROR "GETNATIVE_ENABLE_X86_SIMD requires an x86-64 target")
endif()
if(GETNATIVE_ENABLE_X86_AVX512 AND NOT GETNATIVE_ENABLE_X86_SIMD)
    message(FATAL_ERROR
        "GETNATIVE_ENABLE_X86_AVX512 requires GETNATIVE_ENABLE_X86_SIMD")
endif()
if(GETNATIVE_ENABLE_X86_AVX512 AND MSVC AND MSVC_VERSION LESS 1914)
    message(FATAL_ERROR "This MSVC version does not support /arch:AVX512")
endif()

file(GLOB_RECURSE GETNATIVE_BENCHMARK_ID_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cu"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cuh"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.glsl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.metal"
)
list(SORT GETNATIVE_BENCHMARK_ID_FILES)
set(GETNATIVE_BENCHMARK_SOURCE_MATERIAL "")
foreach(GETNATIVE_BENCHMARK_ID_FILE IN LISTS GETNATIVE_BENCHMARK_ID_FILES)
    file(SHA256 "${GETNATIVE_BENCHMARK_ID_FILE}" GETNATIVE_BENCHMARK_FILE_SHA256)
    file(RELATIVE_PATH GETNATIVE_BENCHMARK_RELATIVE_PATH
         "${CMAKE_CURRENT_SOURCE_DIR}" "${GETNATIVE_BENCHMARK_ID_FILE}")
    string(APPEND GETNATIVE_BENCHMARK_SOURCE_MATERIAL
           "${GETNATIVE_BENCHMARK_RELATIVE_PATH}:${GETNATIVE_BENCHMARK_FILE_SHA256}\n")
endforeach()
string(SHA256 GETNATIVE_BENCHMARK_SOURCE_ID "${GETNATIVE_BENCHMARK_SOURCE_MATERIAL}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${GETNATIVE_BENCHMARK_ID_FILES})

if(MSVC)
    set(GETNATIVE_WARNING_OPTIONS /W4 /permissive- /utf-8)
    add_compile_definitions(NOMINMAX)
    set(GETNATIVE_STRICT_FP_OPTIONS /fp:strict)
    set(GETNATIVE_PRECISE_PLANNER_FP_OPTIONS /fp:precise /fp:contract-)
    set(GETNATIVE_FAST_PLANNER_FP_OPTIONS /fp:fast)
else()
    set(GETNATIVE_WARNING_OPTIONS
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    set(GETNATIVE_STRICT_FP_OPTIONS -ffp-contract=off)
    set(GETNATIVE_PRECISE_PLANNER_FP_OPTIONS -fno-fast-math -ffp-contract=off)
    set(GETNATIVE_FAST_PLANNER_FP_OPTIONS -ffast-math)
endif()
if(GETNATIVE_PLANNER_FP_MODE STREQUAL "precise")
    set(GETNATIVE_PLANNER_FP_OPTIONS ${GETNATIVE_PRECISE_PLANNER_FP_OPTIONS})
elseif(GETNATIVE_PLANNER_FP_MODE STREQUAL "fast")
    set(GETNATIVE_PLANNER_FP_OPTIONS ${GETNATIVE_FAST_PLANNER_FP_OPTIONS})
else()
    set(GETNATIVE_PLANNER_FP_OPTIONS ${GETNATIVE_STRICT_FP_OPTIONS})
endif()
string(JOIN " " GETNATIVE_WARNING_OPTIONS_TEXT ${GETNATIVE_WARNING_OPTIONS})
string(JOIN " " GETNATIVE_STRICT_FP_OPTIONS_TEXT ${GETNATIVE_STRICT_FP_OPTIONS})
string(JOIN " " GETNATIVE_PLANNER_FP_OPTIONS_TEXT ${GETNATIVE_PLANNER_FP_OPTIONS})
set(GETNATIVE_BENCHMARK_CONFIG_FLAGS
    "standard=c++23 | common=[${CMAKE_CXX_FLAGS}] | Debug=[${CMAKE_CXX_FLAGS_DEBUG}] | Release=[${CMAKE_CXX_FLAGS_RELEASE}] | RelWithDebInfo=[${CMAKE_CXX_FLAGS_RELWITHDEBINFO}] | MinSizeRel=[${CMAKE_CXX_FLAGS_MINSIZEREL}] | target=[${GETNATIVE_WARNING_OPTIONS_TEXT} ${GETNATIVE_STRICT_FP_OPTIONS_TEXT}] | planner_fp=${GETNATIVE_PLANNER_FP_MODE} [${GETNATIVE_PLANNER_FP_OPTIONS_TEXT}] | planner_structure=${GETNATIVE_PLANNER_STRUCTURE_MODE}")
string(REPLACE ";" "," GETNATIVE_BENCHMARK_CONFIG_FLAGS
       "${GETNATIVE_BENCHMARK_CONFIG_FLAGS}")
string(REPLACE "\\" "\\\\" GETNATIVE_BENCHMARK_CONFIG_FLAGS
       "${GETNATIVE_BENCHMARK_CONFIG_FLAGS}")
string(REPLACE "\"" "\\\"" GETNATIVE_BENCHMARK_CONFIG_FLAGS
       "${GETNATIVE_BENCHMARK_CONFIG_FLAGS}")

function(getnative_configure_benchmark_metadata target)
    target_compile_definitions(${target} PRIVATE
        "GETNATIVE_BENCHMARK_SOURCE_ID=\"${GETNATIVE_BENCHMARK_SOURCE_ID}\""
        "GETNATIVE_BENCHMARK_BUILD_TYPE=\"$<CONFIG>\""
        "GETNATIVE_BENCHMARK_CXX_FLAGS=\"${GETNATIVE_BENCHMARK_CONFIG_FLAGS}\""
        "GETNATIVE_BENCHMARK_SYSTEM_NAME=\"${CMAKE_SYSTEM_NAME}\""
        "GETNATIVE_BENCHMARK_SYSTEM_PROCESSOR=\"${CMAKE_SYSTEM_PROCESSOR}\""
        "GETNATIVE_BENCHMARK_PLANNER_FP_MODE=\"${GETNATIVE_PLANNER_FP_MODE}\""
        "GETNATIVE_BENCHMARK_PLANNER_STRUCTURE_MODE=\"${GETNATIVE_PLANNER_STRUCTURE_MODE}\""
    )
endfunction()

if(GETNATIVE_ENABLE_METAL)
    if(NOT APPLE)
        message(FATAL_ERROR "GETNATIVE_ENABLE_METAL requires an Apple platform")
    endif()
    find_program(GETNATIVE_XCRUN xcrun)
    if(GETNATIVE_XCRUN)
        execute_process(
            COMMAND "${GETNATIVE_XCRUN}" metal --version
            RESULT_VARIABLE GETNATIVE_METAL_COMPILER_RESULT
            OUTPUT_QUIET ERROR_QUIET
        )
        execute_process(
            COMMAND "${GETNATIVE_XCRUN}" metallib --version
            RESULT_VARIABLE GETNATIVE_METALLIB_RESULT
            OUTPUT_QUIET ERROR_QUIET
        )
    endif()
    if(NOT GETNATIVE_XCRUN
       OR NOT GETNATIVE_METAL_COMPILER_RESULT EQUAL 0
       OR NOT GETNATIVE_METALLIB_RESULT EQUAL 0)
        message(WARNING "MetalToolchain is unavailable; building the CPU backend only")
        set(GETNATIVE_ENABLE_METAL OFF)
    endif()
endif()

set(GETNATIVE_CORE_SOURCES
    src/backend/cpu/cpu_analysis.cpp
    src/backend/cpu/cpu_features_x86.cpp
    src/backend/cpu/inverse_columns.cpp
    src/backend/cpu/inverse_columns_neon.cpp
    src/geometry/candidate_grid.cpp
    src/geometry/crop_geometry.cpp
    src/geometry/profile.cpp
    src/planner/axis_plan.cpp
    src/planner/axis_planner.cpp
    src/planner/filter.cpp
    src/planner/plan_serialize.cpp
    src/planner/plan_store.cpp
)
if(NOT GETNATIVE_PLANNER_FP_MODE STREQUAL "strict")
    # Keep the target strict and relax only the translation units that construct
    # AxisPlan data. Source-specific options follow the target-wide strict flag.
    set_property(SOURCE
        src/planner/axis_plan.cpp
        src/planner/filter.cpp
        APPEND PROPERTY COMPILE_OPTIONS ${GETNATIVE_PLANNER_FP_OPTIONS}
    )
endif()
if(GETNATIVE_ENABLE_X86_SIMD)
    list(APPEND GETNATIVE_CORE_SOURCES
        src/backend/cpu/inverse_columns_sse2.cpp
        src/backend/cpu/inverse_columns_avx2.cpp
    )
    if(MSVC)
        set_source_files_properties(
            src/backend/cpu/inverse_columns_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "/arch:AVX2"
        )
    else()
        set_source_files_properties(
            src/backend/cpu/inverse_columns_sse2.cpp
            PROPERTIES COMPILE_OPTIONS "-msse2"
        )
        # AVX2 production path uses hardware FMA (_mm256_fmadd_ps).
        set_source_files_properties(
            src/backend/cpu/inverse_columns_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma"
        )
    endif()
    if(GETNATIVE_ENABLE_X86_AVX512)
        list(APPEND GETNATIVE_CORE_SOURCES
            src/backend/cpu/inverse_columns_avx512.cpp
        )
        if(MSVC)
            set_source_files_properties(
                src/backend/cpu/inverse_columns_avx512.cpp
                PROPERTIES COMPILE_OPTIONS "/arch:AVX512"
            )
        else()
            include(CheckCXXCompilerFlag)
            check_cxx_compiler_flag("-mavx512f" GETNATIVE_COMPILER_HAS_AVX512F)
            if(NOT GETNATIVE_COMPILER_HAS_AVX512F)
                message(FATAL_ERROR
                    "GETNATIVE_ENABLE_X86_AVX512 requires compiler AVX-512F support")
            endif()
            set_source_files_properties(
                src/backend/cpu/inverse_columns_avx512.cpp
                PROPERTIES COMPILE_OPTIONS "-mavx512f;-mfma"
            )
        endif()
    endif()
endif()

add_library(getnative_zstd STATIC third_party/zstd/zstd.c)
target_include_directories(getnative_zstd PUBLIC third_party/zstd)
set_target_properties(getnative_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(getnative_zstd PRIVATE -w)
endif()

add_library(getnative_lz4 STATIC third_party/lz4/lz4.c third_party/lz4/xxhash.c)
target_include_directories(getnative_lz4 PUBLIC third_party/lz4)
set_target_properties(getnative_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(getnative_lz4 PRIVATE -w)
endif()

add_library(getnative_core STATIC ${GETNATIVE_CORE_SOURCES})
target_compile_features(getnative_core PUBLIC cxx_std_23)
target_include_directories(getnative_core PUBLIC include)
target_link_libraries(getnative_core PUBLIC Threads::Threads getnative_zstd getnative_lz4)
target_compile_definitions(getnative_core PUBLIC
    GETNATIVE_PLANNER_REUSE_TAPS=${GETNATIVE_PLANNER_REUSE_TAPS}
    GETNATIVE_PLANNER_FAST_INTERIOR=${GETNATIVE_PLANNER_FAST_INTERIOR}
    GETNATIVE_PLANNER_REUSE_SCRATCH=${GETNATIVE_PLANNER_REUSE_SCRATCH}
    GETNATIVE_PLANNER_DIRECT_TRANSPOSE=${GETNATIVE_PLANNER_DIRECT_TRANSPOSE}
    GETNATIVE_PLANNER_PERSISTENT_WORKERS=${GETNATIVE_PLANNER_PERSISTENT_WORKERS}
)
if(GETNATIVE_PLANNER_FP_MODE STREQUAL "strict")
    target_compile_definitions(getnative_core PUBLIC GETNATIVE_PLANNER_FP_MODE_VALUE=0)
elseif(GETNATIVE_PLANNER_FP_MODE STREQUAL "fast")
    target_compile_definitions(getnative_core PUBLIC GETNATIVE_PLANNER_FP_MODE_VALUE=1)
else()
    target_compile_definitions(getnative_core PUBLIC GETNATIVE_PLANNER_FP_MODE_VALUE=2)
endif()
if(GETNATIVE_ENABLE_X86_SIMD)
    target_compile_definitions(getnative_core PRIVATE
        GETNATIVE_X86_SSE2_COMPILED=1
        GETNATIVE_X86_AVX2_COMPILED=1
    )
    if(GETNATIVE_ENABLE_X86_AVX512)
        target_compile_definitions(getnative_core PRIVATE
            GETNATIVE_X86_AVX512_COMPILED=1
        )
    endif()
endif()

if(GETNATIVE_ENABLE_CUDA)
    set(GETNATIVE_CUDA_ROOT_HINTS)
    if(GETNATIVE_CUDA_ROOT)
        list(APPEND GETNATIVE_CUDA_ROOT_HINTS "${GETNATIVE_CUDA_ROOT}")
    endif()
    foreach(GETNATIVE_CUDA_ROOT_ENV
            GETNATIVE_CUDA_ROOT CUDAToolkit_ROOT CUDA_PATH CUDA_HOME)
        if(DEFINED ENV{${GETNATIVE_CUDA_ROOT_ENV}}
           AND NOT "$ENV{${GETNATIVE_CUDA_ROOT_ENV}}" STREQUAL "")
            list(APPEND GETNATIVE_CUDA_ROOT_HINTS
                "$ENV{${GETNATIVE_CUDA_ROOT_ENV}}")
        endif()
    endforeach()
    if(CMAKE_HOST_UNIX)
        file(GLOB GETNATIVE_CUDA_VERSIONED_ROOT_HINTS
            LIST_DIRECTORIES true "/usr/local/cuda-*")
        list(SORT GETNATIVE_CUDA_VERSIONED_ROOT_HINTS
            COMPARE NATURAL ORDER DESCENDING)
        list(APPEND GETNATIVE_CUDA_ROOT_HINTS
            "/usr/local/cuda" ${GETNATIVE_CUDA_VERSIONED_ROOT_HINTS})
    endif()
    list(REMOVE_DUPLICATES GETNATIVE_CUDA_ROOT_HINTS)

    find_program(GETNATIVE_NVCC_EXECUTABLE
        NAMES nvcc.exe nvcc
        HINTS ${GETNATIVE_CUDA_ROOT_HINTS}
        PATH_SUFFIXES bin)
    find_program(GETNATIVE_CUOBJDUMP_EXECUTABLE
        NAMES cuobjdump.exe cuobjdump
        HINTS ${GETNATIVE_CUDA_ROOT_HINTS}
        PATH_SUFFIXES bin)
    find_path(GETNATIVE_CUDA_INCLUDE_DIR
        NAMES cuda.h
        HINTS ${GETNATIVE_CUDA_ROOT_HINTS}
        PATH_SUFFIXES include)
    if(NOT GETNATIVE_NVCC_EXECUTABLE OR NOT GETNATIVE_CUOBJDUMP_EXECUTABLE
       OR NOT GETNATIVE_CUDA_INCLUDE_DIR)
        message(FATAL_ERROR
            "GETNATIVE_ENABLE_CUDA requires a CUDA Toolkit with nvcc, cuobjdump, and cuda.h. "
            "Set GETNATIVE_CUDA_ROOT, CUDAToolkit_ROOT, CUDA_PATH, or CUDA_HOME to the toolkit root. "
            "Detected nvcc='${GETNATIVE_NVCC_EXECUTABLE}', cuobjdump='${GETNATIVE_CUOBJDUMP_EXECUTABLE}', "
            "include='${GETNATIVE_CUDA_INCLUDE_DIR}'.")
    endif()
    message(STATUS
        "GetNative CUDA Toolkit: nvcc=${GETNATIVE_NVCC_EXECUTABLE}, include=${GETNATIVE_CUDA_INCLUDE_DIR}")
    if(NOT GETNATIVE_CUDA_MIN_ARCHITECTURE MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "GETNATIVE_CUDA_MIN_ARCHITECTURE must be one numeric SM value such as 75")
    endif()
    if(GETNATIVE_CUDA_ARCHITECTURES STREQUAL ""
       OR GETNATIVE_CUDA_PTX_ARCHITECTURES STREQUAL "")
        message(FATAL_ERROR
            "CUDA native and PTX architecture lists must not be empty")
    endif()
    list(REMOVE_DUPLICATES GETNATIVE_CUDA_ARCHITECTURES)
    list(REMOVE_DUPLICATES GETNATIVE_CUDA_PTX_ARCHITECTURES)
    foreach(GETNATIVE_CUDA_ARCHITECTURE IN LISTS
            GETNATIVE_CUDA_ARCHITECTURES GETNATIVE_CUDA_PTX_ARCHITECTURES)
        if(NOT GETNATIVE_CUDA_ARCHITECTURE MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "CUDA architecture lists must contain numeric SM values")
        endif()
        if(GETNATIVE_CUDA_ARCHITECTURE LESS GETNATIVE_CUDA_MIN_ARCHITECTURE)
            message(FATAL_ERROR
                "CUDA architecture ${GETNATIVE_CUDA_ARCHITECTURE} is below the configured minimum ${GETNATIVE_CUDA_MIN_ARCHITECTURE}")
        endif()
    endforeach()
    list(FIND GETNATIVE_CUDA_ARCHITECTURES
         "${GETNATIVE_CUDA_MIN_ARCHITECTURE}" GETNATIVE_CUDA_MIN_NATIVE_INDEX)
    list(FIND GETNATIVE_CUDA_PTX_ARCHITECTURES
         "${GETNATIVE_CUDA_MIN_ARCHITECTURE}" GETNATIVE_CUDA_MIN_PTX_INDEX)
    if(GETNATIVE_CUDA_MIN_NATIVE_INDEX EQUAL -1
       OR GETNATIVE_CUDA_MIN_PTX_INDEX EQUAL -1)
        message(FATAL_ERROR
            "The minimum CUDA architecture must be present in both native and PTX target lists")
    endif()

    set(GETNATIVE_CUDA_GENERATE_CODE_ARGS)
    set(GETNATIVE_CUDA_NATIVE_TARGET_NAMES)
    foreach(GETNATIVE_CUDA_ARCHITECTURE IN LISTS GETNATIVE_CUDA_ARCHITECTURES)
        list(APPEND GETNATIVE_CUDA_GENERATE_CODE_ARGS
            "--generate-code=arch=compute_${GETNATIVE_CUDA_ARCHITECTURE},code=sm_${GETNATIVE_CUDA_ARCHITECTURE}")
        list(APPEND GETNATIVE_CUDA_NATIVE_TARGET_NAMES
            "sm_${GETNATIVE_CUDA_ARCHITECTURE}")
    endforeach()
    set(GETNATIVE_CUDA_PTX_TARGET_NAMES)
    foreach(GETNATIVE_CUDA_ARCHITECTURE IN LISTS GETNATIVE_CUDA_PTX_ARCHITECTURES)
        list(APPEND GETNATIVE_CUDA_GENERATE_CODE_ARGS
            "--generate-code=arch=compute_${GETNATIVE_CUDA_ARCHITECTURE},code=compute_${GETNATIVE_CUDA_ARCHITECTURE}")
        list(APPEND GETNATIVE_CUDA_PTX_TARGET_NAMES
            "compute_${GETNATIVE_CUDA_ARCHITECTURE}")
    endforeach()
    list(JOIN GETNATIVE_CUDA_NATIVE_TARGET_NAMES "," GETNATIVE_CUDA_NATIVE_TARGET_TEXT)
    list(JOIN GETNATIVE_CUDA_PTX_TARGET_NAMES "," GETNATIVE_CUDA_PTX_TARGET_TEXT)
    set(GETNATIVE_CUDA_ARTIFACT_TARGET
        "native=[${GETNATIVE_CUDA_NATIVE_TARGET_TEXT}]+ptx=[${GETNATIVE_CUDA_PTX_TARGET_TEXT}]")

    set(GETNATIVE_CUDA_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/cuda")
    set(GETNATIVE_CUDA_KERNEL_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda/getnative_cuda_baseline.cu")
    set(GETNATIVE_CUDA_KERNEL_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda/cuda_baseline.hpp")

    set(GETNATIVE_CUDA_FATBIN
        "${GETNATIVE_CUDA_GENERATED_DIR}/getnative_cuda_staged.fatbin")
    set(GETNATIVE_CUDA_FATBIN_SYMBOL "getnative_cuda_staged_fatbin")
    set(GETNATIVE_CUDA_FATBIN_HEADER
        "${GETNATIVE_CUDA_GENERATED_DIR}/${GETNATIVE_CUDA_FATBIN_SYMBOL}.hpp")

    set(GETNATIVE_CUDA_NVCC_HOST_OPTIONS)
    if(CMAKE_HOST_WIN32)
        list(APPEND GETNATIVE_CUDA_NVCC_HOST_OPTIONS -Xcompiler=/utf-8)
    endif()

    add_custom_command(
        OUTPUT "${GETNATIVE_CUDA_FATBIN}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${GETNATIVE_CUDA_GENERATED_DIR}"
        COMMAND "${GETNATIVE_NVCC_EXECUTABLE}"
                --fatbin --cudart=none --machine=64 --std=c++17 -O2 -lineinfo
                ${GETNATIVE_CUDA_NVCC_HOST_OPTIONS}
                ${GETNATIVE_CUDA_GENERATE_CODE_ARGS}
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda"
                "${GETNATIVE_CUDA_KERNEL_SOURCE}"
                -o "${GETNATIVE_CUDA_FATBIN}"
        DEPENDS "${GETNATIVE_CUDA_KERNEL_SOURCE}"
                "${GETNATIVE_CUDA_KERNEL_HEADER}"
        COMMENT "Building the staged generic CUDA C++ fatbin"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${GETNATIVE_CUDA_FATBIN_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
                "-DINPUT=${GETNATIVE_CUDA_FATBIN}"
                "-DOUTPUT=${GETNATIVE_CUDA_FATBIN_HEADER}"
                "-DSYMBOL=${GETNATIVE_CUDA_FATBIN_SYMBOL}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        DEPENDS "${GETNATIVE_CUDA_FATBIN}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        VERBATIM
    )

    add_custom_target(getnative_cuda_staged_fatbin
        DEPENDS "${GETNATIVE_CUDA_FATBIN}"
                "${GETNATIVE_CUDA_FATBIN_HEADER}")

    add_library(getnative_cuda STATIC
        src/backend/cuda/cuda_backend.cpp
        src/backend/cuda/cuda_driver.cpp
        "${GETNATIVE_CUDA_FATBIN_HEADER}"
    )
    add_dependencies(getnative_cuda getnative_cuda_staged_fatbin)
    target_compile_features(getnative_cuda PUBLIC cxx_std_23)
    target_include_directories(getnative_cuda
        PUBLIC include
        PRIVATE
            "${GETNATIVE_CUDA_INCLUDE_DIR}"
            "${GETNATIVE_CUDA_GENERATED_DIR}"
            src/backend/cuda
    )
    target_compile_definitions(getnative_cuda PRIVATE
        GETNATIVE_CUDA_MIN_ARCHITECTURE=${GETNATIVE_CUDA_MIN_ARCHITECTURE}
        "GETNATIVE_CUDA_ARTIFACT_TARGET=\"${GETNATIVE_CUDA_ARTIFACT_TARGET}\"")
    if(MSVC)
        target_compile_options(getnative_cuda PRIVATE /utf-8)
    endif()
    target_link_libraries(getnative_cuda PUBLIC getnative_core)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(getnative_cuda PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

if(GETNATIVE_ENABLE_VULKAN)
    find_package(Vulkan 1.2 QUIET)
    if(NOT Vulkan_FOUND)
        find_path(GETNATIVE_VULKAN_INCLUDE_DIR
            NAMES vulkan/vulkan.h
            HINTS "$ENV{VULKAN_SDK}"
            PATH_SUFFIXES include Include)
        if(WIN32)
            find_library(GETNATIVE_VULKAN_LOADER
                NAMES vulkan-1
                HINTS "$ENV{VULKAN_SDK}"
                PATH_SUFFIXES Lib lib)
        else()
            find_file(GETNATIVE_VULKAN_LOADER
                NAMES libvulkan.so.1 libvulkan.so.1.4.357
                HINTS "$ENV{VULKAN_SDK}"
                PATHS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib
                PATH_SUFFIXES lib lib64 VulkanLoader/lib)
        endif()
        if(NOT GETNATIVE_VULKAN_INCLUDE_DIR OR NOT GETNATIVE_VULKAN_LOADER)
            message(FATAL_ERROR
                "GETNATIVE_ENABLE_VULKAN requires Vulkan 1.2 headers and loader. "
                "Set VULKAN_SDK when they are not installed in standard paths.")
        endif()
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${GETNATIVE_VULKAN_LOADER}"
            INTERFACE_INCLUDE_DIRECTORIES "${GETNATIVE_VULKAN_INCLUDE_DIR}")
        message(STATUS
            "GetNative Vulkan fallback: include=${GETNATIVE_VULKAN_INCLUDE_DIR}, loader=${GETNATIVE_VULKAN_LOADER}")
    endif()
    set(GETNATIVE_VULKAN_SHADER_COMPILER "" CACHE FILEPATH
        "Path to glslc or glslangValidator used for Vulkan compute shaders")
    if(GETNATIVE_VULKAN_SHADER_COMPILER STREQUAL "")
        unset(GETNATIVE_VULKAN_SHADER_COMPILER CACHE)
        if(Vulkan_GLSLC_EXECUTABLE)
            set(GETNATIVE_VULKAN_SHADER_COMPILER "${Vulkan_GLSLC_EXECUTABLE}"
                CACHE FILEPATH
                "Path to glslc or glslangValidator used for Vulkan compute shaders")
        else()
            find_program(GETNATIVE_VULKAN_SHADER_COMPILER
                NAMES glslc.exe glslc glslangValidator.exe glslangValidator glslang
                HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
        endif()
    endif()
    if(NOT GETNATIVE_VULKAN_SHADER_COMPILER)
        message(FATAL_ERROR
            "GETNATIVE_ENABLE_VULKAN requires glslc or glslangValidator")
    endif()
    if(WIN32)
        find_file(GETNATIVE_VULKAN_RUNTIME
            NAMES vulkan-1.dll
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
        set(GETNATIVE_VULKAN_RUNTIME_NAME vulkan-1.dll)
    else()
        find_file(GETNATIVE_VULKAN_RUNTIME
            NAMES libvulkan.so.1
            HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/lib64"
            PATHS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
        set(GETNATIVE_VULKAN_RUNTIME_NAME libvulkan.so.1)
    endif()
    if(NOT GETNATIVE_VULKAN_RUNTIME)
        message(FATAL_ERROR
            "GETNATIVE_ENABLE_VULKAN requires a redistributable Vulkan loader runtime")
    endif()
    file(REAL_PATH "${GETNATIVE_VULKAN_RUNTIME}"
         GETNATIVE_VULKAN_RUNTIME_REAL)
    message(STATUS "GetNative Vulkan runtime: ${GETNATIVE_VULKAN_RUNTIME_REAL}")
    get_filename_component(GETNATIVE_VULKAN_SHADER_COMPILER_NAME
        "${GETNATIVE_VULKAN_SHADER_COMPILER}" NAME)
    if(GETNATIVE_VULKAN_SHADER_COMPILER_NAME MATCHES "^glslc(\\.exe)?$")
        set(GETNATIVE_VULKAN_SHADER_ARGUMENTS
            --target-env=vulkan1.2 -O)
    else()
        set(GETNATIVE_VULKAN_SHADER_ARGUMENTS
            -V --target-env vulkan1.2 -Os)
    endif()
    if(BUILD_TESTING)
        find_program(GETNATIVE_SPIRV_VAL_EXECUTABLE
            NAMES spirv-val.exe spirv-val
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
        if(NOT GETNATIVE_SPIRV_VAL_EXECUTABLE)
            message(FATAL_ERROR "Vulkan test builds require spirv-val")
        endif()
        set(GETNATIVE_VULKAN_VALIDATION_LAYER_DIR "" CACHE PATH
            "Directory containing VkLayer_khronos_validation.json for tests")
        if(NOT GETNATIVE_VULKAN_VALIDATION_LAYER_DIR
           AND EXISTS "$ENV{VULKAN_SDK}/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
            set(GETNATIVE_VULKAN_VALIDATION_LAYER_DIR
                "$ENV{VULKAN_SDK}/share/vulkan/explicit_layer.d")
        endif()
    endif()

    set(GETNATIVE_VULKAN_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
    set(GETNATIVE_VULKAN_SPIRV)
    set(GETNATIVE_VULKAN_GENERATED_HEADERS)
    function(getnative_compile_vulkan_shader name)
        set(source
            "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/vulkan/shaders/${name}.comp")
        set(spirv "${GETNATIVE_VULKAN_GENERATED_DIR}/${name}.spv")
        set(symbol "getnative_vulkan_${name}_spv")
        set(header "${GETNATIVE_VULKAN_GENERATED_DIR}/${symbol}.hpp")
        add_custom_command(
            OUTPUT "${spirv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${GETNATIVE_VULKAN_GENERATED_DIR}"
            COMMAND "${GETNATIVE_VULKAN_SHADER_COMPILER}"
                    ${GETNATIVE_VULKAN_SHADER_ARGUMENTS}
                    -o "${spirv}" "${source}"
            DEPENDS "${source}"
            COMMENT "Compiling GetNative Vulkan shader ${name}"
            VERBATIM)
        add_custom_command(
            OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DINPUT=${spirv}"
                    "-DOUTPUT=${header}"
                    "-DSYMBOL=${symbol}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
            DEPENDS "${spirv}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
            VERBATIM)
        set(GETNATIVE_VULKAN_SPIRV
            ${GETNATIVE_VULKAN_SPIRV} "${spirv}" PARENT_SCOPE)
        set(GETNATIVE_VULKAN_GENERATED_HEADERS
            ${GETNATIVE_VULKAN_GENERATED_HEADERS} "${header}" PARENT_SCOPE)
    endfunction()
    getnative_compile_vulkan_shader(transpose)
    getnative_compile_vulkan_shader(inverse)
    getnative_compile_vulkan_shader(forward)
    getnative_compile_vulkan_shader(metric)
    getnative_compile_vulkan_shader(luma)

    add_custom_target(getnative_vulkan_spirv
        DEPENDS ${GETNATIVE_VULKAN_SPIRV} ${GETNATIVE_VULKAN_GENERATED_HEADERS})
    add_library(getnative_vulkan STATIC
        src/backend/vulkan/vulkan_backend.cpp
        ${GETNATIVE_VULKAN_GENERATED_HEADERS})
    add_dependencies(getnative_vulkan getnative_vulkan_spirv)
    target_compile_features(getnative_vulkan PUBLIC cxx_std_23)
    target_include_directories(getnative_vulkan
        PUBLIC include
        PRIVATE "${GETNATIVE_VULKAN_GENERATED_DIR}" src/backend/vulkan)
    target_link_libraries(getnative_vulkan PUBLIC getnative_core Vulkan::Vulkan)
endif()

if(GETNATIVE_ENABLE_METAL)
    enable_language(OBJCXX)
    find_library(GETNATIVE_METAL_FRAMEWORK Metal REQUIRED)
    find_library(GETNATIVE_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(GETNATIVE_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(GETNATIVE_IMAGEIO_FRAMEWORK ImageIO REQUIRED)
    find_library(GETNATIVE_METAL_COREVIDEO_FRAMEWORK CoreVideo REQUIRED)

    set(GETNATIVE_METAL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/metal")
    set(GETNATIVE_METAL_AIR "${GETNATIVE_METAL_GENERATED_DIR}/getnative.air")
    set(GETNATIVE_METALLIB "${GETNATIVE_METAL_GENERATED_DIR}/getnative.metallib")
    set(GETNATIVE_METALLIB_HEADER "${GETNATIVE_METAL_GENERATED_DIR}/getnative_metallib.hpp")

    add_custom_command(
        OUTPUT "${GETNATIVE_METAL_AIR}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GETNATIVE_METAL_GENERATED_DIR}"
        COMMAND "${GETNATIVE_XCRUN}" -sdk macosx metal
                -Werror -fno-fast-math -ffp-contract=off
                -c "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/metal/getnative.metal"
                -o "${GETNATIVE_METAL_AIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/metal/getnative.metal"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${GETNATIVE_METALLIB}"
        COMMAND "${GETNATIVE_XCRUN}" -sdk macosx metallib
                "${GETNATIVE_METAL_AIR}" -o "${GETNATIVE_METALLIB}"
        DEPENDS "${GETNATIVE_METAL_AIR}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${GETNATIVE_METALLIB_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
                "-DINPUT=${GETNATIVE_METALLIB}"
                "-DOUTPUT=${GETNATIVE_METALLIB_HEADER}"
                -DSYMBOL=getnative_metallib
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        DEPENDS "${GETNATIVE_METALLIB}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
        VERBATIM
    )

    add_library(getnative_metal STATIC
        src/backend/metal/metal_backend.mm
        "${GETNATIVE_METALLIB_HEADER}"
    )
    target_compile_features(getnative_metal PUBLIC cxx_std_23)
    target_include_directories(getnative_metal PUBLIC include PRIVATE "${GETNATIVE_METAL_GENERATED_DIR}")
    target_compile_options(getnative_metal PRIVATE -fobjc-arc)
    target_link_libraries(getnative_metal PUBLIC getnative_core
        PRIVATE "${GETNATIVE_METAL_FRAMEWORK}" "${GETNATIVE_FOUNDATION_FRAMEWORK}"
                "${GETNATIVE_METAL_COREVIDEO_FRAMEWORK}")
endif()

if(GETNATIVE_ENABLE_MEDIA)
    add_library(getnative_media STATIC
        src/media/media_decode.cpp
        src/media/media_index.cpp)
    target_compile_features(getnative_media PUBLIC cxx_std_23)
    target_include_directories(getnative_media PUBLIC include)
    target_link_libraries(getnative_media PUBLIC ${GETNATIVE_FFMPEG_TARGET})
    if(TARGET getnative_ffmpeg_runtime)
        add_dependencies(getnative_media getnative_ffmpeg_runtime)
    endif()
    find_library(GETNATIVE_XXHASH_LIBRARY NAMES xxhash)
    if(GETNATIVE_XXHASH_LIBRARY)
        target_link_libraries(getnative_media PRIVATE "${GETNATIVE_XXHASH_LIBRARY}")
        target_compile_definitions(getnative_media PRIVATE GETNATIVE_USE_XXHASH=1)
    else()
        # Fall back to the in-tree copy so external LSMASH .lwi hash
        # validation works everywhere (getnative_lz4 exports xxhash.h).
        target_link_libraries(getnative_media PRIVATE getnative_lz4)
        target_compile_definitions(getnative_media PRIVATE GETNATIVE_USE_XXHASH=1)
    endif()
    if(GETNATIVE_ENABLE_CUDA)
        target_include_directories(getnative_media PRIVATE "${GETNATIVE_CUDA_INCLUDE_DIR}")
        target_compile_definitions(getnative_media PRIVATE GETNATIVE_HAS_CUDA=1)
    endif()
    if(GETNATIVE_ENABLE_VULKAN)
        target_link_libraries(getnative_media PRIVATE Vulkan::Vulkan)
        target_compile_definitions(getnative_media PRIVATE GETNATIVE_HAS_VULKAN=1)
    endif()
    if(APPLE)
        find_library(GETNATIVE_VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
        find_library(GETNATIVE_COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
        find_library(GETNATIVE_COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
        find_library(GETNATIVE_IOSURFACE_FRAMEWORK IOSurface REQUIRED)
        target_link_libraries(getnative_media PRIVATE
            "${GETNATIVE_VIDEOTOOLBOX_FRAMEWORK}"
            "${GETNATIVE_COREVIDEO_FRAMEWORK}"
            "${GETNATIVE_COREMEDIA_FRAMEWORK}"
            "${GETNATIVE_IOSURFACE_FRAMEWORK}")
        target_compile_definitions(getnative_media PRIVATE GETNATIVE_HAS_VIDEOTOOLBOX=1)
    endif()
endif()

function(getnative_enable_warnings target)
    target_compile_options(${target} PRIVATE ${GETNATIVE_WARNING_OPTIONS})
endfunction()

function(getnative_enable_strict_fp target)
    target_compile_options(${target} PRIVATE ${GETNATIVE_STRICT_FP_OPTIONS})
endfunction()

getnative_enable_warnings(getnative_core)
getnative_enable_strict_fp(getnative_core)
if(TARGET getnative_cuda)
    getnative_enable_warnings(getnative_cuda)
    getnative_enable_strict_fp(getnative_cuda)
endif()
if(TARGET getnative_vulkan)
    getnative_enable_warnings(getnative_vulkan)
    getnative_enable_strict_fp(getnative_vulkan)
endif()
if(TARGET getnative_metal)
    getnative_enable_warnings(getnative_metal)
    getnative_enable_strict_fp(getnative_metal)
endif()
if(TARGET getnative_media)
    getnative_enable_warnings(getnative_media)
    getnative_enable_strict_fp(getnative_media)
endif()

add_executable(getnative-engine
    src/cli/capabilities.cpp
    src/cli/json.cpp
    src/cli/main.cpp
    src/cli/worker.cpp)
if(NOT GETNATIVE_ENABLE_METAL)
    target_link_libraries(getnative-engine PRIVATE getnative_core)
endif()
if(GETNATIVE_ENABLE_MEDIA)
    target_link_libraries(getnative-engine PRIVATE getnative_media)
    target_compile_definitions(getnative-engine PRIVATE GETNATIVE_HAS_MEDIA=1)
endif()
if(GETNATIVE_ENABLE_METAL)
    target_link_libraries(getnative-engine PRIVATE getnative_metal)
    target_compile_definitions(getnative-engine PRIVATE GETNATIVE_HAS_METAL=1)
endif()
if(GETNATIVE_ENABLE_CUDA)
    target_link_libraries(getnative-engine PRIVATE getnative_cuda)
    target_compile_definitions(getnative-engine PRIVATE GETNATIVE_HAS_CUDA=1)
endif()
if(GETNATIVE_ENABLE_VULKAN)
    target_link_libraries(getnative-engine PRIVATE getnative_vulkan)
    target_compile_definitions(getnative-engine PRIVATE GETNATIVE_HAS_VULKAN=1)
endif()
if(GETNATIVE_ENABLE_MEDIA AND UNIX AND NOT APPLE)
    set_target_properties(getnative-engine PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
if(GETNATIVE_ENABLE_MEDIA AND APPLE AND GETNATIVE_FFMPEG_ROOT)
    # Build-tree tests resolve the SDK via BUILD_RPATH / absolute dylib IDs.
    # The install image uses @loader_path; staging rewrites the copied dylibs.
    set_property(TARGET getnative-engine APPEND PROPERTY
        BUILD_RPATH "${GETNATIVE_FFMPEG_ROOT}/lib")
    set_property(TARGET getnative-engine PROPERTY INSTALL_RPATH "@loader_path")
endif()
target_compile_features(getnative-engine PRIVATE cxx_std_23)
getnative_enable_warnings(getnative-engine)

install(TARGETS getnative-engine RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
        DESTINATION share/getnative RENAME LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
        DESTINATION share/getnative)
if(GETNATIVE_ENABLE_VULKAN)
    install(FILES "${GETNATIVE_VULKAN_RUNTIME_REAL}"
            DESTINATION bin RENAME "${GETNATIVE_VULKAN_RUNTIME_NAME}")
    install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-loader/LICENSE.txt"
            DESTINATION share/getnative/licenses/vulkan-loader)
endif()

if(BUILD_TESTING)
    add_executable(getnative_core_tests tests/core_test.cpp)
    target_link_libraries(getnative_core_tests PRIVATE getnative_core)
    target_compile_features(getnative_core_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_core_tests)
    getnative_enable_strict_fp(getnative_core_tests)
    add_test(NAME getnative_core_tests COMMAND getnative_core_tests)

    add_executable(getnative_cpu_column_simd_tests tests/cpu_column_simd_test.cpp)
    target_link_libraries(getnative_cpu_column_simd_tests PRIVATE getnative_core)
    target_include_directories(getnative_cpu_column_simd_tests PRIVATE src/backend/cpu)
    target_compile_features(getnative_cpu_column_simd_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_cpu_column_simd_tests)
    getnative_enable_strict_fp(getnative_cpu_column_simd_tests)
    add_test(NAME getnative_cpu_column_simd_tests COMMAND getnative_cpu_column_simd_tests)

    add_executable(getnative_geometry_tests tests/geometry_test.cpp)
    target_link_libraries(getnative_geometry_tests PRIVATE getnative_core)
    target_compile_features(getnative_geometry_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_geometry_tests)
    add_test(NAME getnative_geometry_tests COMMAND getnative_geometry_tests)

    add_executable(getnative_benchmark_support_tests
        tests/benchmark_support_test.cpp)
    target_include_directories(getnative_benchmark_support_tests PRIVATE bench)
    target_link_libraries(getnative_benchmark_support_tests PRIVATE getnative_core)
    target_compile_definitions(getnative_benchmark_support_tests PRIVATE
        "GETNATIVE_FORMAL_MATRIX_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/bench/fixtures/metal_kernel_matrix.json\"")
    target_compile_features(getnative_benchmark_support_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_benchmark_support_tests)
    add_test(NAME getnative_benchmark_support_tests
             COMMAND getnative_benchmark_support_tests)

    add_executable(getnative_axis_planner_tests tests/axis_planner_test.cpp)
    target_include_directories(getnative_axis_planner_tests PRIVATE src/planner)
    target_link_libraries(getnative_axis_planner_tests PRIVATE getnative_core)
    target_compile_features(getnative_axis_planner_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_axis_planner_tests)
    getnative_enable_strict_fp(getnative_axis_planner_tests)
    add_test(NAME getnative_axis_planner_tests COMMAND getnative_axis_planner_tests)

    add_executable(getnative_plan_store_tests tests/plan_store_test.cpp)
    target_include_directories(getnative_plan_store_tests PRIVATE src/planner)
    target_link_libraries(getnative_plan_store_tests PRIVATE getnative_core)
    target_compile_features(getnative_plan_store_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_plan_store_tests)
    getnative_enable_strict_fp(getnative_plan_store_tests)
    add_test(NAME getnative_plan_store_tests COMMAND getnative_plan_store_tests)

    add_executable(getnative_json_tests tests/json_test.cpp src/cli/json.cpp)
    target_include_directories(getnative_json_tests PRIVATE src/cli include)
    target_compile_features(getnative_json_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_json_tests)
    add_test(NAME getnative_json_tests COMMAND getnative_json_tests)

    add_executable(getnative_verify_resume_tests tests/verify_resume_test.cpp)
    target_include_directories(getnative_verify_resume_tests PRIVATE src/cli include)
    target_compile_features(getnative_verify_resume_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_verify_resume_tests)
    add_test(NAME getnative_verify_resume_tests COMMAND getnative_verify_resume_tests)

    add_executable(getnative_utf8_path_tests tests/utf8_path_test.cpp)
    target_link_libraries(getnative_utf8_path_tests PRIVATE getnative_core)
    target_compile_features(getnative_utf8_path_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_utf8_path_tests)
    add_test(NAME getnative_utf8_path_tests COMMAND getnative_utf8_path_tests)

    add_executable(getnative_backend_policy_tests tests/backend_policy_test.cpp)
    target_include_directories(getnative_backend_policy_tests PRIVATE src/cli)
    target_compile_features(getnative_backend_policy_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_backend_policy_tests)
    add_test(NAME getnative_backend_policy_tests COMMAND getnative_backend_policy_tests)

    if(TARGET getnative_media)
        add_executable(getnative_media_index_compat_tests
            tests/media_index_compat_test.cpp)
        target_link_libraries(getnative_media_index_compat_tests PRIVATE getnative_media)
        target_compile_features(getnative_media_index_compat_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_media_index_compat_tests)
        add_test(NAME getnative_media_index_compat_tests
                 COMMAND getnative_media_index_compat_tests)
    endif()

    find_program(GETNATIVE_PYTHON3 NAMES python3 python)
    if(GETNATIVE_PYTHON3)
        add_test(NAME getnative_worker_protocol_tests
            COMMAND "${GETNATIVE_PYTHON3}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_protocol_test.py"
                $<TARGET_FILE:getnative-engine>)
        set_tests_properties(getnative_worker_protocol_tests PROPERTIES
            RESOURCE_LOCK getnative_gpu
            RUN_SERIAL TRUE)
        if(TARGET getnative_media)
            add_test(NAME getnative_media_worker_tests
                COMMAND "${GETNATIVE_PYTHON3}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/media_worker_test.py"
                    $<TARGET_FILE:getnative-engine>)
            set_tests_properties(getnative_media_worker_tests PROPERTIES
                TIMEOUT 180
                RESOURCE_LOCK getnative_gpu)
        endif()
    endif()

    if(GETNATIVE_ENABLE_CUDA)
        add_test(NAME getnative_cuda_artifact_inventory
            COMMAND "${CMAKE_COMMAND}"
                "-DFATBIN=${GETNATIVE_CUDA_FATBIN}"
                "-DCUOBJDUMP=${GETNATIVE_CUOBJDUMP_EXECUTABLE}"
                "-DNATIVE_ARCHITECTURES=${GETNATIVE_CUDA_ARCHITECTURES}"
                "-DPTX_ARCHITECTURES=${GETNATIVE_CUDA_PTX_ARCHITECTURES}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_cuda_fatbin.cmake")
        add_executable(getnative_cuda_baseline_tests
            tests/cuda_baseline_test.cpp)
        target_link_libraries(getnative_cuda_baseline_tests PRIVATE getnative_cuda)
        target_include_directories(getnative_cuda_baseline_tests PRIVATE
            src/backend/cuda "${GETNATIVE_CUDA_INCLUDE_DIR}")
        target_compile_features(getnative_cuda_baseline_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_cuda_baseline_tests)
        getnative_enable_strict_fp(getnative_cuda_baseline_tests)
        add_test(NAME getnative_cuda_baseline_tests
                 COMMAND getnative_cuda_baseline_tests)
        set_tests_properties(getnative_cuda_baseline_tests PROPERTIES
            SKIP_REGULAR_EXPRESSION "^SKIP:"
            RESOURCE_LOCK getnative_gpu)
    endif()

    if(GETNATIVE_ENABLE_VULKAN)
        foreach(GETNATIVE_VULKAN_MODULE IN LISTS GETNATIVE_VULKAN_SPIRV)
            get_filename_component(GETNATIVE_VULKAN_MODULE_NAME
                "${GETNATIVE_VULKAN_MODULE}" NAME_WE)
            add_test(NAME "getnative_vulkan_spirv_${GETNATIVE_VULKAN_MODULE_NAME}"
                COMMAND "${GETNATIVE_SPIRV_VAL_EXECUTABLE}"
                    --target-env vulkan1.2 "${GETNATIVE_VULKAN_MODULE}")
        endforeach()
        add_executable(getnative_vulkan_analysis_tests
            tests/vulkan_analysis_test.cpp)
        target_link_libraries(getnative_vulkan_analysis_tests PRIVATE
            getnative_vulkan)
        target_compile_features(getnative_vulkan_analysis_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_vulkan_analysis_tests)
        getnative_enable_strict_fp(getnative_vulkan_analysis_tests)
        add_test(NAME getnative_vulkan_analysis_tests
                 COMMAND getnative_vulkan_analysis_tests)
        set_tests_properties(getnative_vulkan_analysis_tests PROPERTIES
            SKIP_REGULAR_EXPRESSION "^SKIP:"
            RESOURCE_LOCK getnative_gpu)
        if(GETNATIVE_VULKAN_VALIDATION_LAYER_DIR)
            set_tests_properties(getnative_vulkan_analysis_tests PROPERTIES
                ENVIRONMENT
                    "VK_LAYER_PATH=${GETNATIVE_VULKAN_VALIDATION_LAYER_DIR}")
        endif()
    endif()

    add_test(NAME getnative_cli_capabilities COMMAND getnative-engine capabilities)
    if(GETNATIVE_ENABLE_METAL)
        set(GETNATIVE_EXPECTED_METAL_CAPABILITY "\"id\":\"metal\",\"compiled\":true")
    else()
        set(GETNATIVE_EXPECTED_METAL_CAPABILITY "\"id\":\"metal\",\"compiled\":false")
    endif()
    if(GETNATIVE_ENABLE_CUDA)
        set(GETNATIVE_EXPECTED_CUDA_CAPABILITY "\"id\":\"cuda\",\"compiled\":true")
    else()
        set(GETNATIVE_EXPECTED_CUDA_CAPABILITY "\"id\":\"cuda\",\"compiled\":false")
    endif()
    if(GETNATIVE_ENABLE_VULKAN)
        set(GETNATIVE_EXPECTED_VULKAN_CAPABILITY
            "\"id\":\"vulkan\",\"compiled\":true")
    else()
        set(GETNATIVE_EXPECTED_VULKAN_CAPABILITY
            "\"id\":\"vulkan\",\"compiled\":false")
    endif()
    set_tests_properties(getnative_cli_capabilities PROPERTIES
        RESOURCE_LOCK getnative_gpu
        PASS_REGULAR_EXPRESSION "\\{\"schema_version\":2,\"engine\":\"getnative-engine\".*\"analyze\":false.*\"gui_min\":1,\"gui_max\":8,\"core_min\":1,\"core_max\":15.*\"id\":\"cpu\",\"compiled\":true,\"device_available\":true,\"analysis_command_available\":false.*\"selected_math_mode\":\"production\".*${GETNATIVE_EXPECTED_METAL_CAPABILITY}.*${GETNATIVE_EXPECTED_CUDA_CAPABILITY}.*${GETNATIVE_EXPECTED_VULKAN_CAPABILITY}.*\"muf-d278cd3\""
    )

    add_test(NAME getnative_cli_geometry
        COMMAND getnative-engine geometry
            --profile muf-d278cd3
            --source-width 1488 --source-height 837
            --active-width 1488 --active-height 837
            --base-height 838
    )
    set_tests_properties(getnative_cli_geometry PROPERTIES
        PASS_REGULAR_EXPRESSION "\"height\":838.*\"src_top\":0.5"
    )

    add_test(NAME getnative_cli_geometry_unknown_mode
        COMMAND getnative-engine geometry
            --mode nonsense
            --source-width 1488 --source-height 837
            --active-width 1488 --active-height 837
    )
    set_tests_properties(getnative_cli_geometry_unknown_mode PROPERTIES
        WILL_FAIL TRUE
    )

    if(GETNATIVE_ENABLE_METAL)
        add_executable(getnative_metal_tests tests/metal_conformance_test.cpp)
        target_link_libraries(getnative_metal_tests PRIVATE getnative_metal
            "${GETNATIVE_METAL_COREVIDEO_FRAMEWORK}")
        target_compile_features(getnative_metal_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_metal_tests)
        getnative_enable_strict_fp(getnative_metal_tests)
        add_test(NAME getnative_metal_tests COMMAND getnative_metal_tests)

        add_executable(getnative_metal_kernel_specialization_tests
            tests/metal_kernel_specialization_test.cpp
        )
        target_link_libraries(getnative_metal_kernel_specialization_tests PRIVATE getnative_metal)
        target_compile_features(getnative_metal_kernel_specialization_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_metal_kernel_specialization_tests)
        getnative_enable_strict_fp(getnative_metal_kernel_specialization_tests)
        add_test(NAME getnative_metal_kernel_specialization_tests
                 COMMAND getnative_metal_kernel_specialization_tests)
    endif()

    if(GETNATIVE_BUILD_UPSTREAM_CONFORMANCE)
        set(GETNATIVE_DESCALE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/descale")
        set(GETNATIVE_ZIMG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/zimg")
        if(NOT EXISTS "${GETNATIVE_DESCALE_ROOT}/src/descale.c"
           OR NOT EXISTS "${GETNATIVE_ZIMG_ROOT}/src/zimg/resize/filter.cpp")
            message(FATAL_ERROR "Pinned descale and zimg checkouts are required for conformance tests")
        endif()

        add_library(getnative_descale_reference STATIC
            "${GETNATIVE_DESCALE_ROOT}/src/descale.c"
        )
        target_include_directories(getnative_descale_reference PUBLIC
            "${GETNATIVE_DESCALE_ROOT}/include"
            "${GETNATIVE_DESCALE_ROOT}/src"
        )
        set_target_properties(getnative_descale_reference PROPERTIES C_STANDARD 11)
        getnative_enable_strict_fp(getnative_descale_reference)

        add_library(getnative_zimg_reference STATIC
            "${GETNATIVE_ZIMG_ROOT}/src/zimg/common/libm_wrapper.cpp"
            "${GETNATIVE_ZIMG_ROOT}/src/zimg/common/matrix.cpp"
            "${GETNATIVE_ZIMG_ROOT}/src/zimg/resize/filter.cpp"
        )
        target_include_directories(getnative_zimg_reference PUBLIC
            "${GETNATIVE_ZIMG_ROOT}/src/zimg"
        )
        target_compile_features(getnative_zimg_reference PUBLIC cxx_std_17)
        getnative_enable_strict_fp(getnative_zimg_reference)

        add_executable(getnative_upstream_conformance_tests
            tests/upstream_conformance_test.cpp
        )
        target_link_libraries(getnative_upstream_conformance_tests PRIVATE
            getnative_core
            getnative_descale_reference
            getnative_zimg_reference
        )
        target_compile_features(getnative_upstream_conformance_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_upstream_conformance_tests)
        getnative_enable_strict_fp(getnative_upstream_conformance_tests)
        add_test(NAME getnative_upstream_conformance_tests
                 COMMAND getnative_upstream_conformance_tests)
    endif()
endif()

add_executable(getnative_core_benchmark bench/core_benchmark.cpp)
target_link_libraries(getnative_core_benchmark PRIVATE getnative_core)
target_include_directories(getnative_core_benchmark PRIVATE src/planner)
target_compile_features(getnative_core_benchmark PRIVATE cxx_std_23)
getnative_enable_warnings(getnative_core_benchmark)
getnative_enable_strict_fp(getnative_core_benchmark)
getnative_configure_benchmark_metadata(getnative_core_benchmark)

find_library(GETNATIVE_ZSTD_LIBRARY NAMES zstd libzstd.so.1)
find_library(GETNATIVE_LZ4_LIBRARY NAMES lz4 liblz4.so.1)
find_path(GETNATIVE_COMPRESSION_INCLUDE_DIR NAMES zstd.h lz4.h
    HINTS "${GETNATIVE_COMPRESSION_INCLUDE_DIR}" /tmp/compression-headers)
if(GETNATIVE_ZSTD_LIBRARY AND GETNATIVE_LZ4_LIBRARY
   AND GETNATIVE_COMPRESSION_INCLUDE_DIR)
    add_executable(getnative_plan_compression_probe
        bench/plan_compression_probe.cpp)
    target_link_libraries(getnative_plan_compression_probe PRIVATE
        getnative_core
        "${GETNATIVE_ZSTD_LIBRARY}"
        "${GETNATIVE_LZ4_LIBRARY}")
    target_include_directories(getnative_plan_compression_probe PRIVATE
        "${GETNATIVE_COMPRESSION_INCLUDE_DIR}")
    target_compile_features(getnative_plan_compression_probe PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_plan_compression_probe)
    getnative_enable_strict_fp(getnative_plan_compression_probe)
endif()

add_executable(getnative_cpu_backend_benchmark
    bench/cpu_backend_benchmark.cpp)
target_link_libraries(getnative_cpu_backend_benchmark PRIVATE getnative_core)
target_include_directories(getnative_cpu_backend_benchmark PRIVATE
    bench
    src/backend/cpu
)
target_compile_features(getnative_cpu_backend_benchmark PRIVATE cxx_std_23)
getnative_enable_warnings(getnative_cpu_backend_benchmark)
getnative_enable_strict_fp(getnative_cpu_backend_benchmark)
getnative_configure_benchmark_metadata(getnative_cpu_backend_benchmark)

# Dual-arm memory-profile benchmark: frontend (plan cache x heights) +
# backend (fixed plan x random frames). Intended for AMD uProf assess/data_access/ibs.
add_executable(getnative_cpu_memory_profile_benchmark
    bench/cpu_memory_profile_benchmark.cpp)
target_link_libraries(getnative_cpu_memory_profile_benchmark PRIVATE getnative_core)
target_include_directories(getnative_cpu_memory_profile_benchmark PRIVATE
    bench
    src/backend/cpu
)
target_compile_features(getnative_cpu_memory_profile_benchmark PRIVATE cxx_std_23)
getnative_enable_warnings(getnative_cpu_memory_profile_benchmark)
getnative_enable_strict_fp(getnative_cpu_memory_profile_benchmark)
getnative_configure_benchmark_metadata(getnative_cpu_memory_profile_benchmark)

if(GETNATIVE_ENABLE_CUDA)
    add_executable(getnative_cuda_throughput_benchmark
        bench/cuda_throughput_benchmark.cpp)
    target_link_libraries(getnative_cuda_throughput_benchmark PRIVATE
        getnative_cuda)
    target_include_directories(getnative_cuda_throughput_benchmark PRIVATE bench)
    target_compile_features(getnative_cuda_throughput_benchmark PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_cuda_throughput_benchmark)
    getnative_enable_strict_fp(getnative_cuda_throughput_benchmark)
    getnative_configure_benchmark_metadata(getnative_cuda_throughput_benchmark)

    if(GETNATIVE_BUILD_CUDA_AUTOTUNER)
        add_executable(getnative_cuda_autotune
            bench/cuda_autotune_benchmark.cpp)
        set_target_properties(getnative_cuda_autotune PROPERTIES
            OUTPUT_NAME "getnative-cuda-autotune")
        target_link_libraries(getnative_cuda_autotune PRIVATE getnative_cuda)
        target_include_directories(getnative_cuda_autotune PRIVATE bench)
        target_compile_features(getnative_cuda_autotune PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_cuda_autotune)
        getnative_enable_strict_fp(getnative_cuda_autotune)
        getnative_configure_benchmark_metadata(getnative_cuda_autotune)
    endif()
endif()

if(GETNATIVE_ENABLE_METAL)
    add_executable(getnative_fixed_recipe_benchmark
        bench/fixed_recipe_benchmark.cpp)
    target_link_libraries(getnative_fixed_recipe_benchmark PRIVATE getnative_metal)
    target_compile_features(getnative_fixed_recipe_benchmark PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_fixed_recipe_benchmark)
    getnative_enable_strict_fp(getnative_fixed_recipe_benchmark)
    getnative_configure_benchmark_metadata(getnative_fixed_recipe_benchmark)

    add_executable(getnative_metal_benchmark bench/metal_benchmark.cpp)
    target_link_libraries(getnative_metal_benchmark PRIVATE getnative_metal)
    target_include_directories(getnative_metal_benchmark PRIVATE src/planner)
    target_compile_features(getnative_metal_benchmark PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_metal_benchmark)
    getnative_enable_strict_fp(getnative_metal_benchmark)
    getnative_configure_benchmark_metadata(getnative_metal_benchmark)

    add_executable(getnative_metal_kernel_benchmark bench/metal_kernel_benchmark.mm)
    target_link_libraries(getnative_metal_kernel_benchmark PRIVATE
        getnative_metal
        "${GETNATIVE_COREGRAPHICS_FRAMEWORK}"
        "${GETNATIVE_FOUNDATION_FRAMEWORK}"
        "${GETNATIVE_IMAGEIO_FRAMEWORK}"
    )
    target_compile_features(getnative_metal_kernel_benchmark PRIVATE cxx_std_23)
    target_compile_options(getnative_metal_kernel_benchmark PRIVATE -fobjc-arc)
    target_compile_definitions(getnative_metal_kernel_benchmark PRIVATE
        "GETNATIVE_METALLIB_PATH=\"${GETNATIVE_METALLIB}\""
    )
    target_include_directories(getnative_metal_kernel_benchmark PRIVATE
        src/backend/cpu
        src/planner
    )
    getnative_enable_warnings(getnative_metal_kernel_benchmark)
    getnative_enable_strict_fp(getnative_metal_kernel_benchmark)

    set(GETNATIVE_CPU0_EVALUATOR_ARGS
        --matrix "${CMAKE_CURRENT_SOURCE_DIR}/bench/fixtures/metal_kernel_matrix.json"
        --fixture "${CMAKE_CURRENT_SOURCE_DIR}/bench/fixtures/6.2-1.png"
        --samples 21 --case "bicubic-catrom@810" --cpu-columns --assert
        --artifact-root "${CMAKE_CURRENT_BINARY_DIR}/artifacts/backend-hotpath"
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        list(APPEND GETNATIVE_CPU0_EVALUATOR_ARGS --required-simd)
    endif()

    add_custom_target(getnative_backend_hotpath_evaluator
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
                --output-on-failure
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/artifacts/backend-hotpath"
        COMMAND $<TARGET_FILE:getnative_metal_kernel_benchmark>
                --matrix "${CMAKE_CURRENT_SOURCE_DIR}/bench/fixtures/metal_kernel_matrix.json"
                --fixture "${CMAKE_CURRENT_SOURCE_DIR}/bench/fixtures/6.2-1.png"
                --samples 21 --controls-only --required-specialized --assert
                --artifact-root "${CMAKE_CURRENT_BINARY_DIR}/artifacts/backend-hotpath"
        COMMAND $<TARGET_FILE:getnative_metal_kernel_benchmark>
                ${GETNATIVE_CPU0_EVALUATOR_ARGS}
        DEPENDS getnative_metal_kernel_benchmark
        USES_TERMINAL
        VERBATIM
    )
endif()

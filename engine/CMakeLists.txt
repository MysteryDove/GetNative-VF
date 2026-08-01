cmake_minimum_required(VERSION 3.20)

project(getnative_engine VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
option(GETNATIVE_BUILD_UPSTREAM_CONFORMANCE
       "Build test-only exact comparisons against pinned descale and zimg" OFF)
option(GETNATIVE_ENABLE_METAL "Build the Metal analysis backend on macOS" ${APPLE})
option(GETNATIVE_ENABLE_CUDA
       "Build the optional Windows CUDA Driver API analysis backend" OFF)
option(GETNATIVE_ENABLE_VULKAN
       "Build the optional Windows Vulkan Compute analysis backend" OFF)
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

set(GETNATIVE_CUDA_ARCHITECTURES "120" CACHE STRING
    "Semicolon-separated CUDA SM versions to embed as native cubins")
if(GETNATIVE_ENABLE_CUDA AND NOT WIN32)
    message(FATAL_ERROR "GETNATIVE_ENABLE_CUDA currently requires Windows")
endif()
if(GETNATIVE_ENABLE_VULKAN AND NOT WIN32)
    message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN currently requires Windows")
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
    set(GETNATIVE_WARNING_OPTIONS /W4 /permissive-)
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

add_library(getnative_core STATIC ${GETNATIVE_CORE_SOURCES})
target_compile_features(getnative_core PUBLIC cxx_std_23)
target_include_directories(getnative_core PUBLIC include)
target_link_libraries(getnative_core PUBLIC Threads::Threads)
target_compile_definitions(getnative_core PUBLIC
    GETNATIVE_PLANNER_REUSE_TAPS=${GETNATIVE_PLANNER_REUSE_TAPS}
    GETNATIVE_PLANNER_FAST_INTERIOR=${GETNATIVE_PLANNER_FAST_INTERIOR}
    GETNATIVE_PLANNER_REUSE_SCRATCH=${GETNATIVE_PLANNER_REUSE_SCRATCH}
    GETNATIVE_PLANNER_DIRECT_TRANSPOSE=${GETNATIVE_PLANNER_DIRECT_TRANSPOSE}
    GETNATIVE_PLANNER_PERSISTENT_WORKERS=${GETNATIVE_PLANNER_PERSISTENT_WORKERS}
)
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

add_library(getnative_gpu_common STATIC
    src/backend/gpu/gpu_batch.cpp
)
target_compile_features(getnative_gpu_common PUBLIC cxx_std_23)
target_include_directories(getnative_gpu_common PRIVATE src/backend/gpu)
target_link_libraries(getnative_gpu_common PUBLIC getnative_core)

if(GETNATIVE_ENABLE_CUDA)
    find_program(GETNATIVE_NVCC_EXECUTABLE
        NAMES nvcc.exe nvcc
        HINTS "$ENV{CUDA_PATH}/bin")
    find_path(GETNATIVE_CUDA_INCLUDE_DIR
        NAMES cuda.h
        HINTS "$ENV{CUDA_PATH}/include")
    if(NOT GETNATIVE_NVCC_EXECUTABLE OR NOT GETNATIVE_CUDA_INCLUDE_DIR)
        message(FATAL_ERROR
            "GETNATIVE_ENABLE_CUDA requires a CUDA Toolkit with nvcc and cuda.h")
    endif()
    execute_process(
        COMMAND "${GETNATIVE_NVCC_EXECUTABLE}" --version
        RESULT_VARIABLE GETNATIVE_NVCC_VERSION_RESULT
        OUTPUT_VARIABLE GETNATIVE_NVCC_VERSION_OUTPUT
        ERROR_VARIABLE GETNATIVE_NVCC_VERSION_ERROR)
    if(NOT GETNATIVE_NVCC_VERSION_RESULT EQUAL 0)
        message(FATAL_ERROR
            "nvcc --version failed: ${GETNATIVE_NVCC_VERSION_ERROR}")
    endif()
    string(REGEX MATCH "V([0-9]+\\.[0-9]+\\.[0-9]+)"
        GETNATIVE_NVCC_VERSION_MATCH "${GETNATIVE_NVCC_VERSION_OUTPUT}")
    if(NOT GETNATIVE_NVCC_VERSION_MATCH)
        set(GETNATIVE_CUDA_TOOLKIT_VERSION "unknown")
    else()
        set(GETNATIVE_CUDA_TOOLKIT_VERSION "${CMAKE_MATCH_1}")
    endif()
    if(NOT GETNATIVE_CUDA_ARCHITECTURES)
        message(FATAL_ERROR "GETNATIVE_CUDA_ARCHITECTURES must not be empty")
    endif()

    set(GETNATIVE_CUDA_GENCODE_ARGS)
    foreach(GETNATIVE_CUDA_ARCH IN LISTS GETNATIVE_CUDA_ARCHITECTURES)
        if(NOT GETNATIVE_CUDA_ARCH MATCHES "^[0-9]+$")
            message(FATAL_ERROR
                "Invalid CUDA architecture '${GETNATIVE_CUDA_ARCH}'; use values such as 120")
        endif()
        list(APPEND GETNATIVE_CUDA_GENCODE_ARGS
            "--generate-code=arch=compute_${GETNATIVE_CUDA_ARCH},code=sm_${GETNATIVE_CUDA_ARCH}")
    endforeach()
    list(GET GETNATIVE_CUDA_ARCHITECTURES -1 GETNATIVE_CUDA_PTX_ARCH)
    list(APPEND GETNATIVE_CUDA_GENCODE_ARGS
        "--generate-code=arch=compute_${GETNATIVE_CUDA_PTX_ARCH},code=compute_${GETNATIVE_CUDA_PTX_ARCH}")

    set(GETNATIVE_CUDA_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/cuda")
    set(GETNATIVE_CUDA_KERNEL_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda/getnative_cuda.cu")
    set(GETNATIVE_CUDA_KERNEL_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda/getnative_cuda_kernels.cuh")

    # Single production CUDA artifact: optimized path with FP32 FMA allowed.
    # Do not build parallel strict/fast-math fatbins or use -Xptxas=-O0.
    set(GETNATIVE_CUDA_FATBIN
        "${GETNATIVE_CUDA_GENERATED_DIR}/getnative_cuda.fatbin")
    set(GETNATIVE_CUDA_PTX
        "${GETNATIVE_CUDA_GENERATED_DIR}/getnative_cuda_compute_${GETNATIVE_CUDA_PTX_ARCH}.ptx")
    set(GETNATIVE_CUDA_FATBIN_SYMBOL "getnative_cuda_fatbin")
    set(GETNATIVE_CUDA_FATBIN_HEADER
        "${GETNATIVE_CUDA_GENERATED_DIR}/${GETNATIVE_CUDA_FATBIN_SYMBOL}.hpp")

    add_custom_command(
        OUTPUT "${GETNATIVE_CUDA_FATBIN}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${GETNATIVE_CUDA_GENERATED_DIR}"
        COMMAND "${GETNATIVE_NVCC_EXECUTABLE}"
                --fatbin --machine=64 --std=c++17 -O3 -lineinfo
                -Xptxas=-v
                ${GETNATIVE_CUDA_GENCODE_ARGS}
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda"
                "${GETNATIVE_CUDA_KERNEL_SOURCE}"
                -o "${GETNATIVE_CUDA_FATBIN}"
        DEPENDS "${GETNATIVE_CUDA_KERNEL_SOURCE}"
                "${GETNATIVE_CUDA_KERNEL_HEADER}"
        COMMENT "Building production CUDA native cubin and PTX fatbin"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${GETNATIVE_CUDA_PTX}"
        COMMAND "${GETNATIVE_NVCC_EXECUTABLE}"
                --ptx --machine=64 --std=c++17 -O3 -lineinfo
                "--gpu-architecture=compute_${GETNATIVE_CUDA_PTX_ARCH}"
                "-I${CMAKE_CURRENT_SOURCE_DIR}/src/backend/cuda"
                "${GETNATIVE_CUDA_KERNEL_SOURCE}"
                -o "${GETNATIVE_CUDA_PTX}"
        DEPENDS "${GETNATIVE_CUDA_KERNEL_SOURCE}"
                "${GETNATIVE_CUDA_KERNEL_HEADER}"
        COMMENT "Saving the production CUDA generic PTX fallback"
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

    add_custom_target(getnative_cuda_artifacts
        DEPENDS "${GETNATIVE_CUDA_FATBIN}"
                "${GETNATIVE_CUDA_PTX}"
                "${GETNATIVE_CUDA_FATBIN_HEADER}")

    add_library(getnative_cuda STATIC
        src/backend/cuda/cuda_backend.cpp
        src/backend/cuda/cuda_driver.cpp
        "${GETNATIVE_CUDA_FATBIN_HEADER}"
    )
    add_dependencies(getnative_cuda getnative_cuda_artifacts)
    target_compile_features(getnative_cuda PUBLIC cxx_std_23)
    target_include_directories(getnative_cuda
        PUBLIC include
        PRIVATE
            "${GETNATIVE_CUDA_INCLUDE_DIR}"
            "${GETNATIVE_CUDA_GENERATED_DIR}"
            src/backend/cuda
            src/backend/gpu
    )
    target_link_libraries(getnative_cuda PUBLIC getnative_gpu_common)
endif()

if(GETNATIVE_ENABLE_VULKAN)
    find_path(GETNATIVE_VULKAN_INCLUDE_DIR
        NAMES vulkan/vulkan.h
        HINTS "$ENV{VULKAN_SDK}/Include")
    find_program(GETNATIVE_GLSLC_EXECUTABLE
        NAMES glslc.exe glslc
        HINTS "$ENV{VULKAN_SDK}/Bin")
    find_program(GETNATIVE_SPIRV_VAL_EXECUTABLE
        NAMES spirv-val.exe spirv-val
        HINTS "$ENV{VULKAN_SDK}/Bin")
    if(NOT GETNATIVE_VULKAN_INCLUDE_DIR
       OR NOT GETNATIVE_GLSLC_EXECUTABLE
       OR NOT GETNATIVE_SPIRV_VAL_EXECUTABLE)
        message(FATAL_ERROR
            "GETNATIVE_ENABLE_VULKAN requires Vulkan-Headers, glslc, and spirv-val")
    endif()

    set(GETNATIVE_VULKAN_SHADER_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/vulkan/shaders")
    set(GETNATIVE_VULKAN_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
    set(GETNATIVE_VULKAN_SHADER_NAMES
        inverse_axis
        inverse_axis_matrix
        forward_axis_matrix
        metric_axis_p1)
    set(GETNATIVE_VULKAN_SPV_FILES)
    set(GETNATIVE_VULKAN_SPV_HEADERS)
    foreach(GETNATIVE_VULKAN_SHADER IN LISTS GETNATIVE_VULKAN_SHADER_NAMES)
        set(GETNATIVE_VULKAN_SPV
            "${GETNATIVE_VULKAN_GENERATED_DIR}/${GETNATIVE_VULKAN_SHADER}.spv")
        set(GETNATIVE_VULKAN_SYMBOL
            "getnative_vulkan_${GETNATIVE_VULKAN_SHADER}_spv")
        set(GETNATIVE_VULKAN_HEADER
            "${GETNATIVE_VULKAN_GENERATED_DIR}/${GETNATIVE_VULKAN_SYMBOL}.hpp")
        add_custom_command(
            OUTPUT "${GETNATIVE_VULKAN_SPV}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${GETNATIVE_VULKAN_GENERATED_DIR}"
            # Single production SPIR-V path: optimize for performance; no dual
            # strict/fast-math shader variants.
            COMMAND "${GETNATIVE_GLSLC_EXECUTABLE}"
                    --target-env=vulkan1.2 -fshader-stage=compute -Werror -O
                    "-I${GETNATIVE_VULKAN_SHADER_DIR}"
                    "${GETNATIVE_VULKAN_SHADER_DIR}/${GETNATIVE_VULKAN_SHADER}.comp"
                    -o "${GETNATIVE_VULKAN_SPV}"
            COMMAND "${GETNATIVE_SPIRV_VAL_EXECUTABLE}"
                    --target-env vulkan1.2 "${GETNATIVE_VULKAN_SPV}"
            DEPENDS
                "${GETNATIVE_VULKAN_SHADER_DIR}/${GETNATIVE_VULKAN_SHADER}.comp"
                "${GETNATIVE_VULKAN_SHADER_DIR}/getnative_common.glsl"
            COMMENT "Compiling and validating production ${GETNATIVE_VULKAN_SHADER}.comp"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${GETNATIVE_VULKAN_HEADER}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DINPUT=${GETNATIVE_VULKAN_SPV}"
                    "-DOUTPUT=${GETNATIVE_VULKAN_HEADER}"
                    "-DSYMBOL=${GETNATIVE_VULKAN_SYMBOL}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
            DEPENDS "${GETNATIVE_VULKAN_SPV}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
            VERBATIM
        )
        list(APPEND GETNATIVE_VULKAN_SPV_FILES "${GETNATIVE_VULKAN_SPV}")
        list(APPEND GETNATIVE_VULKAN_SPV_HEADERS "${GETNATIVE_VULKAN_HEADER}")
    endforeach()
    add_custom_target(getnative_vulkan_artifacts
        DEPENDS ${GETNATIVE_VULKAN_SPV_FILES} ${GETNATIVE_VULKAN_SPV_HEADERS})

    add_library(getnative_vulkan STATIC
        src/backend/vulkan/vulkan_backend.cpp
        src/backend/vulkan/vulkan_loader.cpp
        ${GETNATIVE_VULKAN_SPV_HEADERS}
    )
    add_dependencies(getnative_vulkan getnative_vulkan_artifacts)
    target_compile_features(getnative_vulkan PUBLIC cxx_std_23)
    target_include_directories(getnative_vulkan
        PUBLIC include
        PRIVATE
            "${GETNATIVE_VULKAN_INCLUDE_DIR}"
            "${GETNATIVE_VULKAN_GENERATED_DIR}"
            src/backend/vulkan
            src/backend/gpu
    )
    target_link_libraries(getnative_vulkan PUBLIC getnative_gpu_common)
endif()

if(GETNATIVE_ENABLE_METAL)
    enable_language(OBJCXX)
    find_library(GETNATIVE_METAL_FRAMEWORK Metal REQUIRED)
    find_library(GETNATIVE_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(GETNATIVE_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(GETNATIVE_IMAGEIO_FRAMEWORK ImageIO REQUIRED)

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
        PRIVATE "${GETNATIVE_METAL_FRAMEWORK}" "${GETNATIVE_FOUNDATION_FRAMEWORK}")
endif()

function(getnative_enable_warnings target)
    target_compile_options(${target} PRIVATE ${GETNATIVE_WARNING_OPTIONS})
endfunction()

function(getnative_enable_strict_fp target)
    target_compile_options(${target} PRIVATE ${GETNATIVE_STRICT_FP_OPTIONS})
endfunction()

getnative_enable_warnings(getnative_core)
getnative_enable_strict_fp(getnative_core)
getnative_enable_warnings(getnative_gpu_common)
getnative_enable_strict_fp(getnative_gpu_common)
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

add_executable(getnative-engine src/cli/main.cpp)
target_link_libraries(getnative-engine PRIVATE getnative_core)
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
target_compile_features(getnative-engine PRIVATE cxx_std_23)
getnative_enable_warnings(getnative-engine)

install(TARGETS getnative-engine RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
        DESTINATION share/getnative)

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

    add_executable(getnative_gpu_batch_tests tests/gpu_batch_test.cpp)
    target_link_libraries(getnative_gpu_batch_tests PRIVATE getnative_gpu_common)
    target_include_directories(getnative_gpu_batch_tests PRIVATE src/backend/gpu)
    target_compile_features(getnative_gpu_batch_tests PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_gpu_batch_tests)
    getnative_enable_strict_fp(getnative_gpu_batch_tests)
    add_test(NAME getnative_gpu_batch_tests COMMAND getnative_gpu_batch_tests)

    if(GETNATIVE_ENABLE_CUDA)
        add_executable(getnative_cuda_conformance_tests
            tests/cuda_conformance_test.cpp)
        target_link_libraries(getnative_cuda_conformance_tests PRIVATE getnative_cuda)
        target_compile_features(getnative_cuda_conformance_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_cuda_conformance_tests)
        getnative_enable_strict_fp(getnative_cuda_conformance_tests)
        add_test(NAME getnative_cuda_conformance_tests
                 COMMAND getnative_cuda_conformance_tests)

        add_executable(getnative_cuda_artifact_tests
            tests/cuda_artifact_test.cpp)
        add_dependencies(getnative_cuda_artifact_tests getnative_cuda_artifacts)
        target_compile_features(getnative_cuda_artifact_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_cuda_artifact_tests)
        set(GETNATIVE_CUDA_ARTIFACT_TEST_ARGS
            --fatbin "${GETNATIVE_CUDA_FATBIN}"
            --cuobjdump "${GETNATIVE_CUDA_INCLUDE_DIR}/../bin/cuobjdump.exe"
            --pe $<TARGET_FILE:getnative-engine>)
        foreach(GETNATIVE_CUDA_ARCH IN LISTS GETNATIVE_CUDA_ARCHITECTURES)
            list(APPEND GETNATIVE_CUDA_ARTIFACT_TEST_ARGS
                --expect-sm "sm_${GETNATIVE_CUDA_ARCH}")
        endforeach()
        list(APPEND GETNATIVE_CUDA_ARTIFACT_TEST_ARGS
            --expect-ptx "compute_${GETNATIVE_CUDA_PTX_ARCH}")
        add_test(NAME getnative_cuda_artifact_tests
            COMMAND getnative_cuda_artifact_tests
                    ${GETNATIVE_CUDA_ARTIFACT_TEST_ARGS})
    endif()

    if(GETNATIVE_ENABLE_VULKAN)
        add_executable(getnative_vulkan_conformance_tests
            tests/vulkan_conformance_test.cpp)
        target_link_libraries(getnative_vulkan_conformance_tests
            PRIVATE getnative_vulkan)
        target_compile_features(getnative_vulkan_conformance_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_vulkan_conformance_tests)
        getnative_enable_strict_fp(getnative_vulkan_conformance_tests)
        add_test(NAME getnative_vulkan_conformance_tests
                 COMMAND getnative_vulkan_conformance_tests)

        add_executable(getnative_vulkan_memory_tests
            tests/vulkan_memory_test.cpp)
        target_link_libraries(getnative_vulkan_memory_tests PRIVATE getnative_vulkan)
        target_include_directories(getnative_vulkan_memory_tests PRIVATE
            src/backend/vulkan "${GETNATIVE_VULKAN_INCLUDE_DIR}")
        target_compile_features(getnative_vulkan_memory_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_vulkan_memory_tests)
        getnative_enable_strict_fp(getnative_vulkan_memory_tests)
        add_test(NAME getnative_vulkan_memory_tests
                 COMMAND getnative_vulkan_memory_tests)

        foreach(GETNATIVE_VULKAN_SPV IN LISTS GETNATIVE_VULKAN_SPV_FILES)
            get_filename_component(GETNATIVE_VULKAN_SPV_NAME
                "${GETNATIVE_VULKAN_SPV}" NAME_WE)
            add_test(NAME "getnative_vulkan_spirv_${GETNATIVE_VULKAN_SPV_NAME}"
                COMMAND "${GETNATIVE_SPIRV_VAL_EXECUTABLE}"
                        --target-env vulkan1.2 "${GETNATIVE_VULKAN_SPV}")
        endforeach()
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
        set(GETNATIVE_EXPECTED_VULKAN_CAPABILITY "\"id\":\"vulkan\",\"compiled\":true")
    else()
        set(GETNATIVE_EXPECTED_VULKAN_CAPABILITY "\"id\":\"vulkan\",\"compiled\":false")
    endif()
    set_tests_properties(getnative_cli_capabilities PROPERTIES
        PASS_REGULAR_EXPRESSION "\\{\"schema_version\":3,\"engine\":\"getnative-engine\".*\"analyze\":false.*\"gui_min\":1,\"gui_max\":8,\"core_min\":1,\"core_max\":15.*\"id\":\"cpu\",\"compiled\":true,\"device_available\":true,\"analysis_command_available\":false.*\"selected_math_mode\":\"production\".*${GETNATIVE_EXPECTED_METAL_CAPABILITY}.*${GETNATIVE_EXPECTED_CUDA_CAPABILITY}.*${GETNATIVE_EXPECTED_VULKAN_CAPABILITY}.*\"muf-d278cd3\""
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
        target_link_libraries(getnative_metal_tests PRIVATE getnative_metal)
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
    add_executable(getnative_cuda_benchmark
        bench/cuda_matrix_benchmark.cpp)
    target_link_libraries(getnative_cuda_benchmark PRIVATE getnative_cuda)
    target_include_directories(getnative_cuda_benchmark PRIVATE
        bench
        src/planner)
    target_compile_features(getnative_cuda_benchmark PRIVATE cxx_std_23)
    target_compile_definitions(getnative_cuda_benchmark PRIVATE
        "GETNATIVE_CUDA_TOOLKIT_VERSION=\"${GETNATIVE_CUDA_TOOLKIT_VERSION}\""
        "GETNATIVE_CUDA_FATBIN_PATH=\"${GETNATIVE_CUDA_FATBIN}\"")
    getnative_enable_warnings(getnative_cuda_benchmark)
    getnative_enable_strict_fp(getnative_cuda_benchmark)
    getnative_configure_benchmark_metadata(getnative_cuda_benchmark)
endif()

if(GETNATIVE_ENABLE_VULKAN)
    add_executable(getnative_vulkan_benchmark
        bench/vulkan_benchmark.cpp)
    target_link_libraries(getnative_vulkan_benchmark PRIVATE getnative_vulkan)
    target_compile_features(getnative_vulkan_benchmark PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_vulkan_benchmark)
    getnative_enable_strict_fp(getnative_vulkan_benchmark)
    getnative_configure_benchmark_metadata(getnative_vulkan_benchmark)
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

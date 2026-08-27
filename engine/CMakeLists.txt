cmake_minimum_required(VERSION 3.20)

project(getnative_engine VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
option(GETNATIVE_BUILD_UPSTREAM_CONFORMANCE
       "Build test-only exact comparisons against pinned descale and zimg" OFF)
set(DSMVC_ROOT "" CACHE PATH "Optional dsmvc source root for blur fixture export")
option(GETNATIVE_ENABLE_METAL "Build the Metal analysis backend on macOS" ${APPLE})
option(GETNATIVE_ENABLE_VULKAN "Build the Vulkan compute backend on Windows or Linux" OFF)

if(GETNATIVE_ENABLE_VULKAN)
    if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN requires Windows or Linux")
    endif()
    find_path(GETNATIVE_VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include"
        PATH_SUFFIXES include Include)
    if(NOT GETNATIVE_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN requires Vulkan-Headers (vulkan/vulkan.h)")
    endif()
    # Linux distribution packages place the loader in an architecture-specific
    # multiarch directory.  We only probe it when Vulkan is enabled; CPU-only
    # configure must remain independent of any Vulkan SDK or loader.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_library(GETNATIVE_VULKAN_LOADER_LIBRARY
            NAMES vulkan libvulkan.so.1
            HINTS "$ENV{VULKAN_SDK}/lib" "$ENV{VULKAN_SDK}/Lib"
            PATHS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /usr/lib
            NO_DEFAULT_PATH)
        if(NOT GETNATIVE_VULKAN_LOADER_LIBRARY)
            find_library(GETNATIVE_VULKAN_LOADER_LIBRARY
                NAMES vulkan libvulkan.so.1)
        endif()
        if(NOT GETNATIVE_VULKAN_LOADER_LIBRARY)
            message(FATAL_ERROR
                "GETNATIVE_ENABLE_VULKAN requires the Vulkan loader (libvulkan.so.1)")
        endif()
        message(STATUS "GetNative Vulkan loader: ${GETNATIVE_VULKAN_LOADER_LIBRARY}")
    endif()
    find_program(GETNATIVE_GLSLC_EXECUTABLE NAMES glslc glslc.exe glslangValidator glslangValidator.exe
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(NOT GETNATIVE_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN requires glslc or glslangValidator")
    endif()
    find_program(GETNATIVE_SPIRV_VAL_EXECUTABLE NAMES spirv-val spirv-val.exe
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(NOT GETNATIVE_SPIRV_VAL_EXECUTABLE)
        message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN requires spirv-val")
    endif()
    find_program(GETNATIVE_SPIRV_DIS_EXECUTABLE NAMES spirv-dis spirv-dis.exe
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(NOT GETNATIVE_SPIRV_DIS_EXECUTABLE)
        message(FATAL_ERROR "GETNATIVE_ENABLE_VULKAN requires spirv-dis")
    endif()
    set(GETNATIVE_VULKAN_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
    set(GETNATIVE_VULKAN_SPIRV)
    set(GETNATIVE_VULKAN_GENERATED_HEADERS)
    get_filename_component(GETNATIVE_VULKAN_SHADER_COMPILER_NAME
        "${GETNATIVE_GLSLC_EXECUTABLE}" NAME)
    if(GETNATIVE_VULKAN_SHADER_COMPILER_NAME MATCHES "^glslangValidator(\\.exe)?$")
        set(GETNATIVE_VULKAN_SHADER_ARGUMENTS -V --target-env vulkan1.2 -Os)
    else()
        set(GETNATIVE_VULKAN_SHADER_ARGUMENTS --target-env=vulkan1.2 -O)
    endif()
    function(getnative_compile_vulkan_shader name)
        set(definitions ${ARGN})
        set(source_name "${name}")
        if(name MATCHES "^inverse_b[0-9]+$")
            set(source_name inverse)
        endif()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/vulkan/shaders/${source_name}.comp")
        set(spirv "${GETNATIVE_VULKAN_GENERATED_DIR}/${name}.spv")
        set(symbol "getnative_vulkan_${name}_spv")
        set(header "${GETNATIVE_VULKAN_GENERATED_DIR}/${symbol}.hpp")
        add_custom_command(OUTPUT "${spirv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${GETNATIVE_VULKAN_GENERATED_DIR}"
            COMMAND "${GETNATIVE_GLSLC_EXECUTABLE}" ${GETNATIVE_VULKAN_SHADER_ARGUMENTS}
                    ${definitions}
                    -o "${spirv}" "${source}"
            DEPENDS "${source}" VERBATIM)
        add_custom_command(OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spirv}" "-DOUTPUT=${header}"
                    "-DSYMBOL=${symbol}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
            DEPENDS "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
            VERBATIM)
        set(GETNATIVE_VULKAN_SPIRV ${GETNATIVE_VULKAN_SPIRV} "${spirv}" PARENT_SCOPE)
        set(GETNATIVE_VULKAN_GENERATED_HEADERS
            ${GETNATIVE_VULKAN_GENERATED_HEADERS} "${header}" PARENT_SCOPE)
    endfunction()
    getnative_compile_vulkan_shader(transpose)
    getnative_compile_vulkan_shader(inverse)
    foreach(GETNATIVE_VULKAN_BAND IN ITEMS 1 3 5 7 9 11)
        getnative_compile_vulkan_shader(
            inverse_b${GETNATIVE_VULKAN_BAND}
            -DGETNATIVE_FIXED_BANDWIDTH=${GETNATIVE_VULKAN_BAND})
    endforeach()
    getnative_compile_vulkan_shader(forward)
    getnative_compile_vulkan_shader(metric)
    getnative_compile_vulkan_shader(luma)
    add_custom_target(getnative_vulkan_artifacts
        DEPENDS ${GETNATIVE_VULKAN_SPIRV} ${GETNATIVE_VULKAN_GENERATED_HEADERS})
endif()

file(GLOB_RECURSE GETNATIVE_BENCHMARK_ID_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
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
else()
    set(GETNATIVE_WARNING_OPTIONS
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    set(GETNATIVE_STRICT_FP_OPTIONS -ffp-contract=off)
endif()
string(JOIN " " GETNATIVE_WARNING_OPTIONS_TEXT ${GETNATIVE_WARNING_OPTIONS})
string(JOIN " " GETNATIVE_STRICT_FP_OPTIONS_TEXT ${GETNATIVE_STRICT_FP_OPTIONS})
set(GETNATIVE_BENCHMARK_CONFIG_FLAGS
    "standard=c++23 | common=[${CMAKE_CXX_FLAGS}] | Debug=[${CMAKE_CXX_FLAGS_DEBUG}] | Release=[${CMAKE_CXX_FLAGS_RELEASE}] | RelWithDebInfo=[${CMAKE_CXX_FLAGS_RELWITHDEBINFO}] | MinSizeRel=[${CMAKE_CXX_FLAGS_MINSIZEREL}] | target=[${GETNATIVE_WARNING_OPTIONS_TEXT} ${GETNATIVE_STRICT_FP_OPTIONS_TEXT}]")
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

add_library(getnative_core STATIC
    src/backend/cpu/cpu_analysis.cpp
    src/backend/cpu/inverse_columns.cpp
    src/backend/cpu/inverse_columns_neon.cpp
    src/geometry/candidate_grid.cpp
    src/geometry/crop_geometry.cpp
    src/geometry/profile.cpp
    src/planner/axis_plan.cpp
    src/planner/axis_planner.cpp
    src/planner/filter.cpp
)
target_compile_features(getnative_core PUBLIC cxx_std_23)
target_include_directories(getnative_core PUBLIC include)
target_link_libraries(getnative_core PUBLIC Threads::Threads)

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
if(TARGET getnative_metal)
    getnative_enable_warnings(getnative_metal)
    getnative_enable_strict_fp(getnative_metal)
endif()

if(GETNATIVE_ENABLE_VULKAN)
    add_library(getnative_vulkan STATIC
        src/backend/vulkan/vulkan_backend.cpp
        src/backend/vulkan/vulkan_loader.cpp
        ${GETNATIVE_VULKAN_GENERATED_HEADERS})
    add_dependencies(getnative_vulkan getnative_vulkan_artifacts)
    target_compile_features(getnative_vulkan PUBLIC cxx_std_23)
    target_include_directories(getnative_vulkan PUBLIC include PRIVATE
        "${GETNATIVE_VULKAN_GENERATED_DIR}" "${GETNATIVE_VULKAN_INCLUDE_DIR}"
        src/backend/vulkan)
    target_link_libraries(getnative_vulkan PUBLIC getnative_core
        PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(getnative_vulkan
        PUBLIC GETNATIVE_HAS_VULKAN=1 PRIVATE VK_NO_PROTOTYPES=1)
    getnative_enable_warnings(getnative_vulkan)
    getnative_enable_strict_fp(getnative_vulkan)
endif()

add_executable(getnative-engine src/cli/main.cpp)
if(GETNATIVE_ENABLE_METAL)
    target_link_libraries(getnative-engine PRIVATE getnative_metal)
    target_compile_definitions(getnative-engine PRIVATE GETNATIVE_HAS_METAL=1)
else()
    target_link_libraries(getnative-engine PRIVATE getnative_core)
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

    add_test(NAME getnative_cli_capabilities COMMAND getnative-engine capabilities)
    if(GETNATIVE_ENABLE_METAL)
        set(GETNATIVE_EXPECTED_METAL_CAPABILITY "\"id\":\"metal\",\"compiled\":true")
    else()
        set(GETNATIVE_EXPECTED_METAL_CAPABILITY "\"id\":\"metal\",\"compiled\":false")
    endif()
    if(GETNATIVE_ENABLE_VULKAN)
        set(GETNATIVE_EXPECTED_VULKAN_CAPABILITY "\"id\":\"vulkan\",\"compiled\":true")
    else()
        set(GETNATIVE_EXPECTED_VULKAN_CAPABILITY "\"id\":\"vulkan\",\"compiled\":false,\"device_available\":false")
    endif()
    set_tests_properties(getnative_cli_capabilities PROPERTIES
        PASS_REGULAR_EXPRESSION "\\{\"schema_version\":2,\"engine\":\"getnative-engine\".*\"analyze\":false.*\"gui_min\":1,\"gui_max\":8,\"core_min\":1,\"core_max\":15.*\"blur\":\\{\"kind\":\"positive_scale\",\"default\":1.0,\"gui_min\":0.75\\}.*\"unsupported_features\":\\[\"spline32\"\\].*\"id\":\"cpu\",\"compiled\":true,\"device_available\":true,\"analysis_command_available\":false.*${GETNATIVE_EXPECTED_METAL_CAPABILITY}.*\"id\":\"cuda\",\"compiled\":false,\"device_available\":false,\"analysis_command_available\":false.*${GETNATIVE_EXPECTED_VULKAN_CAPABILITY}.*\"muf-d278cd3\""
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

    if(GETNATIVE_ENABLE_VULKAN)
        foreach(GETNATIVE_VULKAN_MODULE IN LISTS GETNATIVE_VULKAN_SPIRV)
            get_filename_component(GETNATIVE_VULKAN_MODULE_NAME
                "${GETNATIVE_VULKAN_MODULE}" NAME_WE)
            add_test(NAME "getnative_vulkan_spirv_${GETNATIVE_VULKAN_MODULE_NAME}"
                COMMAND "${GETNATIVE_SPIRV_VAL_EXECUTABLE}"
                    --target-env vulkan1.2 "${GETNATIVE_VULKAN_MODULE}")
        endforeach()
        foreach(GETNATIVE_VULKAN_STRICT_MODULE IN ITEMS
                inverse inverse_b1 inverse_b3 inverse_b5 inverse_b7 inverse_b9
                inverse_b11 forward metric)
            add_test(NAME "getnative_vulkan_strict_${GETNATIVE_VULKAN_STRICT_MODULE}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DSPIRV_DIS=${GETNATIVE_SPIRV_DIS_EXECUTABLE}"
                    "-DINPUT=${GETNATIVE_VULKAN_GENERATED_DIR}/${GETNATIVE_VULKAN_STRICT_MODULE}.spv"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_spirv_strict.cmake")
        endforeach()
        add_executable(getnative_vulkan_analysis_tests tests/vulkan_analysis_test.cpp)
        target_link_libraries(getnative_vulkan_analysis_tests PRIVATE getnative_vulkan)
        target_compile_features(getnative_vulkan_analysis_tests PRIVATE cxx_std_23)
        getnative_enable_warnings(getnative_vulkan_analysis_tests)
        getnative_enable_strict_fp(getnative_vulkan_analysis_tests)
        add_test(NAME getnative_vulkan_analysis_tests
                 COMMAND getnative_vulkan_analysis_tests)
        set_tests_properties(getnative_vulkan_analysis_tests PROPERTIES
            SKIP_REGULAR_EXPRESSION "^SKIP:" RESOURCE_LOCK getnative_gpu)
    endif()

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

if(DSMVC_ROOT)
    if(NOT EXISTS "${DSMVC_ROOT}/src/axis_plan.cpp"
       OR NOT EXISTS "${DSMVC_ROOT}/include/dsmvc/engine.hpp")
        message(FATAL_ERROR "DSMVC_ROOT must point to a dsmvc source checkout")
    endif()
    add_executable(dsmvc_blur_fixture_export
        tools/dsmvc_blur_fixture_export.cpp
        "${DSMVC_ROOT}/src/axis_plan.cpp")
    target_include_directories(dsmvc_blur_fixture_export PRIVATE
        "${DSMVC_ROOT}/include" "${DSMVC_ROOT}/src")
    target_link_libraries(dsmvc_blur_fixture_export PRIVATE getnative_core)
    target_compile_features(dsmvc_blur_fixture_export PRIVATE cxx_std_23)
    getnative_enable_warnings(dsmvc_blur_fixture_export)
    getnative_enable_strict_fp(dsmvc_blur_fixture_export)
endif()

if(GETNATIVE_ENABLE_VULKAN)
    add_executable(getnative_vulkan_kernel_benchmark
        bench/vulkan_kernel_benchmark.cpp)
    target_link_libraries(getnative_vulkan_kernel_benchmark PRIVATE getnative_vulkan)
    target_compile_features(getnative_vulkan_kernel_benchmark PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_vulkan_kernel_benchmark)
    getnative_enable_strict_fp(getnative_vulkan_kernel_benchmark)
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

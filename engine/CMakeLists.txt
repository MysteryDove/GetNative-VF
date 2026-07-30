cmake_minimum_required(VERSION 3.20)

project(getnative_engine VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
option(GETNATIVE_BUILD_UPSTREAM_CONFORMANCE
       "Build test-only exact comparisons against pinned descale and zimg" OFF)
option(GETNATIVE_ENABLE_METAL "Build the Metal analysis backend on macOS" ${APPLE})

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

add_executable(getnative-engine src/cli/main.cpp)
if(GETNATIVE_ENABLE_METAL)
    target_link_libraries(getnative-engine PRIVATE getnative_metal)
    target_compile_definitions(getnative-engine PRIVATE GETNATIVE_HAS_METAL=1)
else()
    target_link_libraries(getnative-engine PRIVATE getnative_core)
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
    set_tests_properties(getnative_cli_capabilities PROPERTIES
        PASS_REGULAR_EXPRESSION "\\{\"schema_version\":2,\"engine\":\"getnative-engine\".*\"analyze\":false.*\"gui_min\":1,\"gui_max\":8,\"core_min\":1,\"core_max\":15.*\"id\":\"cpu\",\"compiled\":true,\"device_available\":true,\"analysis_command_available\":false.*${GETNATIVE_EXPECTED_METAL_CAPABILITY}.*\"id\":\"cuda\",\"compiled\":false,\"device_available\":false,\"analysis_command_available\":false.*\"muf-d278cd3\""
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

if(GETNATIVE_ENABLE_METAL)
    add_executable(getnative_metal_benchmark bench/metal_benchmark.cpp)
    target_link_libraries(getnative_metal_benchmark PRIVATE getnative_metal)
    target_include_directories(getnative_metal_benchmark PRIVATE src/planner)
    target_compile_features(getnative_metal_benchmark PRIVATE cxx_std_23)
    getnative_enable_warnings(getnative_metal_benchmark)
    getnative_enable_strict_fp(getnative_metal_benchmark)
    getnative_configure_benchmark_metadata(getnative_metal_benchmark)
endif()

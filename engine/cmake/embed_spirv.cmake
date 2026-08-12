if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "INPUT, OUTPUT, and SYMBOL are required")
endif()

file(READ "${INPUT}" spirv_hex HEX)
string(LENGTH "${spirv_hex}" hex_length)
math(EXPR remainder "${hex_length} % 8")
if(NOT remainder EQUAL 0)
    message(FATAL_ERROR "SPIR-V byte length is not a multiple of four: ${INPUT}")
endif()

set(words "")
math(EXPR last "${hex_length} - 8")
if(last GREATER_EQUAL 0)
    foreach(offset RANGE 0 ${last} 8)
        string(SUBSTRING "${spirv_hex}" ${offset} 2 b0)
        math(EXPR next "${offset} + 2")
        string(SUBSTRING "${spirv_hex}" ${next} 2 b1)
        math(EXPR next "${offset} + 4")
        string(SUBSTRING "${spirv_hex}" ${next} 2 b2)
        math(EXPR next "${offset} + 6")
        string(SUBSTRING "${spirv_hex}" ${next} 2 b3)
        string(APPEND words "    0x${b3}${b2}${b1}${b0}U,\n")
    endforeach()
endif()

file(WRITE "${OUTPUT}"
    "#pragma once\n\n#include <cstdint>\n\n"
    "namespace getnative::vulkan_detail::embedded {\n"
    "inline constexpr std::uint32_t ${SYMBOL}[] = {\n${words}};\n"
    "} // namespace getnative::vulkan_detail::embedded\n")

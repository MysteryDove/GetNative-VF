if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "INPUT, OUTPUT, and SYMBOL are required")
endif()

file(READ "${INPUT}" binary_hex HEX)
string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," binary_bytes "${binary_hex}")
file(WRITE "${OUTPUT}"
    "#pragma once\n"
    "#include <cstddef>\n"
    "inline constexpr unsigned char ${SYMBOL}[] = {${binary_bytes}};\n"
    "inline constexpr std::size_t ${SYMBOL}_size = sizeof(${SYMBOL});\n")

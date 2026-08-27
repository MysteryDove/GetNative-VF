if(NOT DEFINED SPIRV_DIS OR NOT DEFINED INPUT)
    message(FATAL_ERROR "SPIRV_DIS and INPUT are required")
endif()

execute_process(
    COMMAND "${SPIRV_DIS}" "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE disassembly
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "spirv-dis failed for ${INPUT}: ${error}")
endif()

string(FIND "${disassembly}" "OpFma" fma_position)
if(NOT fma_position EQUAL -1)
    message(FATAL_ERROR "strict SPIR-V contains OpFma: ${INPUT}")
endif()

string(FIND "${disassembly}" "NoContraction" no_contraction_position)
if(no_contraction_position EQUAL -1)
    message(FATAL_ERROR "strict SPIR-V has no NoContraction decorations: ${INPUT}")
endif()

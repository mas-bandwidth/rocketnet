# Builds internal_header_probe, a program that includes rocketnet_internal.h
# through RocketNet::rocketnet, and passes only when that build fails because
# the header is not on the consumer's include path.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target internal_header_probe --config "${CONFIG}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL 0)
    message(FATAL_ERROR "a consumer compiled rocketnet_internal.h through RocketNet::rocketnet")
endif()
# The not-found diagnostic for rocketnet_internal.h itself (gcc, clang, MSVC),
# never a failure inside a copy of it that was found (its own includes
# missing): that copy would be on the consumer's include path.
if(NOT output MATCHES "rocketnet_internal\\.h: No such file|'rocketnet_internal\\.h' file not found|Cannot open include file: 'rocketnet_internal\\.h'")
    message(FATAL_ERROR "internal_header_probe failed for another reason:\n${output}")
endif()
message(STATUS "internal_header_refused: rocketnet_internal.h is not on a consumer's include path")

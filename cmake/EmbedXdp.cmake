file(MAKE_DIRECTORY "${OUTPUT_DIR}/source")
file(COPY "${SOURCE_DIR}/server_xdp/Makefile" "${SOURCE_DIR}/server_xdp/server_xdp.c"
    "${SOURCE_DIR}/sdk/rocketnet_wire.h" DESTINATION "${OUTPUT_DIR}/source")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${OUTPUT_DIR}/rocketnet_server_xdp.tar.gz" .
    WORKING_DIRECTORY "${OUTPUT_DIR}/source" RESULT_VARIABLE tar_result)
if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "XDP source archive failed: ${tar_result}")
endif()
execute_process(COMMAND "${XXD}" -i -n rocketnet_server_xdp_tar_gz "${OUTPUT_DIR}/rocketnet_server_xdp.tar.gz"
    OUTPUT_FILE "${OUTPUT_DIR}/rocketnet_server_xdp.h.tmp" RESULT_VARIABLE xxd_result)
if(NOT xxd_result EQUAL 0)
    message(FATAL_ERROR "XDP header embedding failed: ${xxd_result}")
endif()
file(RENAME "${OUTPUT_DIR}/rocketnet_server_xdp.h.tmp" "${OUTPUT_DIR}/rocketnet_server_xdp.h")

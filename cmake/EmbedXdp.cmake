include("${CMAKE_CURRENT_LIST_DIR}/XdpSources.cmake")

# The shared source list is staged flattened into a source archive. That
# archive stays a build artifact -- the packaging check and the archive
# verifier consume it, and check_xdp_archive regenerates it from this same
# list -- so nothing consumes the runtime tarball the audit finding described.
file(MAKE_DIRECTORY "${OUTPUT_DIR}/source")
set(archive_paths)
foreach(source IN LISTS ROCKETNET_XDP_SOURCES)
    get_filename_component(name "${source}" NAME)
    list(FIND archive_paths "./${name}" existing_index)
    if(NOT existing_index EQUAL -1)
        message(FATAL_ERROR "Duplicate XDP archive basename: ${name}")
    endif()
    configure_file("${SOURCE_DIR}/${source}" "${OUTPUT_DIR}/source/${name}" COPYONLY)
    list(APPEND archive_paths "./${name}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${OUTPUT_DIR}/rocketnet_server_xdp.tar.gz" --format=gnutar
    ${archive_paths}
    WORKING_DIRECTORY "${OUTPUT_DIR}/source" RESULT_VARIABLE tar_result)
if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "XDP source archive failed: ${tar_result}")
endif()
execute_process(COMMAND "${XXD}" -i -n rocketnet_server_xdp_tar_gz "${OUTPUT_DIR}/rocketnet_server_xdp.tar.gz"
    OUTPUT_FILE "${OUTPUT_DIR}/rocketnet_server_xdp.h.tmp" RESULT_VARIABLE xxd_result)
if(NOT xxd_result EQUAL 0)
    message(FATAL_ERROR "XDP header embedding failed: ${xxd_result}")
endif()
file(READ "${OUTPUT_DIR}/rocketnet_server_xdp.h.tmp" header_content)
string(REPLACE "unsigned char" "static const unsigned char" header_content "${header_content}")
string(REPLACE "unsigned int" "static const unsigned int" header_content "${header_content}")

# On Linux the object is compiled here, at build time, from the same shared
# source list, and embedded alongside the archive. The dataplane opens those
# bytes from memory with bpf_object__open_mem: nothing compiles at runtime and
# no shell runs. No other platform builds the XDP dataplane, so there is no
# object to embed there and the header needs only the archive.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(MAKE_DIRECTORY "${OUTPUT_DIR}/object/server_xdp")
    file(MAKE_DIRECTORY "${OUTPUT_DIR}/object/sdk")
    foreach(source IN LISTS ROCKETNET_XDP_SOURCES)
        configure_file("${SOURCE_DIR}/${source}" "${OUTPUT_DIR}/object/${source}" COPYONLY)
    endforeach()
    execute_process(COMMAND make
        WORKING_DIRECTORY "${OUTPUT_DIR}/object/server_xdp" RESULT_VARIABLE make_result)
    if(NOT make_result EQUAL 0)
        message(FATAL_ERROR "XDP object build failed: ${make_result}")
    endif()
    execute_process(COMMAND "${XXD}" -i -n server_xdp_o "${OUTPUT_DIR}/object/server_xdp/server_xdp.o"
        OUTPUT_FILE "${OUTPUT_DIR}/server_xdp_o.h.tmp" RESULT_VARIABLE xxd_object_result)
    if(NOT xxd_object_result EQUAL 0)
        message(FATAL_ERROR "XDP object embedding failed: ${xxd_object_result}")
    endif()
    file(READ "${OUTPUT_DIR}/server_xdp_o.h.tmp" object_content)
    string(REPLACE "unsigned char" "static const unsigned char" object_content "${object_content}")
    string(REPLACE "unsigned int" "static const unsigned int" object_content "${object_content}")
    set(header_content "${header_content}\n${object_content}")
endif()

file(WRITE "${OUTPUT_DIR}/rocketnet_server_xdp.h" "${header_content}")

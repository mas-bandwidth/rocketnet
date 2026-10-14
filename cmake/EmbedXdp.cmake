include("${CMAKE_CURRENT_LIST_DIR}/XdpSources.cmake")
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

# The archive is byte-reproducible: the member order follows
# ROCKETNET_XDP_SOURCES, --mtime fixes every member timestamp,
# SOURCE_DATE_EPOCH (set before compression below) fixes the gzip
# header timestamp, and the ustar header normalisation below fixes
# owner, group and mode. cmake -E tar offers no owner or group flags,
# so the headers of the uncompressed tar are rewritten directly and
# the result is compressed afterwards.
set(xdp_tar "${OUTPUT_DIR}/rocketnet_server_xdp.tar")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${xdp_tar}" --format=gnutar
    "--mtime=1970-01-01 00:00:00 UTC"
    ${archive_paths}
    WORKING_DIRECTORY "${OUTPUT_DIR}/source" RESULT_VARIABLE tar_result)
if(NOT tar_result EQUAL 0)
    message(FATAL_ERROR "XDP source archive failed: ${tar_result}")
endif()

function(rocketnet_xdp_replace_range input offset length replacement output)
    string(LENGTH "${input}" input_length)
    string(SUBSTRING "${input}" 0 "${offset}" before)
    math(EXPR after_start "${offset} + ${length}")
    math(EXPR after_length "${input_length} - ${after_start}")
    string(SUBSTRING "${input}" "${after_start}" "${after_length}" after)
    set("${output}" "${before}${replacement}${after}" PARENT_SCOPE)
endfunction()

function(rocketnet_xdp_hex_value digit output)
    string(FIND "0123456789abcdef" "${digit}" value)
    set("${output}" "${value}" PARENT_SCOPE)
endfunction()

file(READ "${xdp_tar}" archive_hex HEX)
set(header_offset 0)
foreach(path IN LISTS archive_paths)
    string(SUBSTRING "${path}" 2 -1 name)
    file(SIZE "${OUTPUT_DIR}/source/${name}" member_size)
    math(EXPR header_hex_offset "${header_offset} * 2")
    string(SUBSTRING "${archive_hex}" "${header_hex_offset}" 1024 header_hex)
    # Mode 0644, uid 0 and gid 0 as zero-padded octal, uname and gname
    # "root" over NUL padding, and the checksum field as spaces while
    # the checksum is computed.
    rocketnet_xdp_replace_range("${header_hex}" 200 16 "3030303036343400" header_hex)
    rocketnet_xdp_replace_range("${header_hex}" 216 16 "3030303030303000" header_hex)
    rocketnet_xdp_replace_range("${header_hex}" 232 16 "3030303030303000" header_hex)
    rocketnet_xdp_replace_range("${header_hex}" 530 64 "726f6f7400000000000000000000000000000000000000000000000000000000" header_hex)
    rocketnet_xdp_replace_range("${header_hex}" 594 64 "726f6f7400000000000000000000000000000000000000000000000000000000" header_hex)
    rocketnet_xdp_replace_range("${header_hex}" 296 16 "2020202020202020" header_hex)
    set(checksum 0)
    foreach(byte_index RANGE 0 511)
        math(EXPR byte_hex_offset "${byte_index} * 2")
        string(SUBSTRING "${header_hex}" "${byte_hex_offset}" 2 byte_hex)
        string(SUBSTRING "${byte_hex}" 0 1 high_digit)
        string(SUBSTRING "${byte_hex}" 1 1 low_digit)
        rocketnet_xdp_hex_value("${high_digit}" high_value)
        rocketnet_xdp_hex_value("${low_digit}" low_value)
        math(EXPR byte_value "${high_value} * 16 + ${low_value}")
        math(EXPR checksum "${checksum} + ${byte_value}")
    endforeach()
    set(checksum_octal "")
    foreach(digit_index RANGE 1 6)
        math(EXPR checksum_digit "${checksum} % 8")
        set(checksum_octal "${checksum_digit}${checksum_octal}")
        math(EXPR checksum "${checksum} / 8")
    endforeach()
    set(checksum_hex "")
    foreach(digit_index RANGE 0 5)
        string(SUBSTRING "${checksum_octal}" "${digit_index}" 1 checksum_digit)
        set(checksum_hex "${checksum_hex}3${checksum_digit}")
    endforeach()
    rocketnet_xdp_replace_range("${header_hex}" 296 16 "${checksum_hex}0020" header_hex)
    rocketnet_xdp_replace_range("${archive_hex}" "${header_hex_offset}" 1024 "${header_hex}" archive_hex)
    math(EXPR header_offset "${header_offset} + 512 + ((${member_size} + 511) / 512) * 512")
endforeach()
set(xdp_tar_hex "${OUTPUT_DIR}/rocketnet_server_xdp.tar.hex")
file(WRITE "${xdp_tar_hex}" "${archive_hex}")
execute_process(COMMAND "${XXD}" -r -p "${xdp_tar_hex}" "${xdp_tar}.normalized"
    RESULT_VARIABLE xxd_result)
if(NOT xxd_result EQUAL 0)
    message(FATAL_ERROR "XDP archive normalisation failed: ${xxd_result}")
endif()
set(ENV{SOURCE_DATE_EPOCH} "0")
file(ARCHIVE_CREATE OUTPUT "${OUTPUT_DIR}/rocketnet_server_xdp.tar.gz"
    PATHS "${xdp_tar}.normalized" FORMAT raw COMPRESSION GZip
    WORKING_DIRECTORY "${OUTPUT_DIR}")
file(REMOVE "${xdp_tar}" "${xdp_tar}.normalized" "${xdp_tar_hex}")

execute_process(COMMAND "${XXD}" -i -n rocketnet_server_xdp_tar_gz "${OUTPUT_DIR}/rocketnet_server_xdp.tar.gz"
    OUTPUT_FILE "${OUTPUT_DIR}/rocketnet_server_xdp.h.tmp" RESULT_VARIABLE xxd_result)
if(NOT xxd_result EQUAL 0)
    message(FATAL_ERROR "XDP header embedding failed: ${xxd_result}")
endif()
file(RENAME "${OUTPUT_DIR}/rocketnet_server_xdp.h.tmp" "${OUTPUT_DIR}/rocketnet_server_xdp.h")

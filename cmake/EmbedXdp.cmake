# Compiles sdk/xdp/server_xdp.c to the BPF object server_xdp.o at build
# time and embeds its bytes in OUTPUT_DIR/rocketnet_server_xdp.h as
# rocketnet_server_xdp_o and rocketnet_server_xdp_o_len. The library loads the
# object from those bytes (bpf_object__open_mem); the server host needs no
# compiler, no make and no temporary directory.
#
# Inputs: SOURCE_DIR, OUTPUT_DIR, XXD, CLANG (a clang with the BPF target, or
# empty) and MULTIARCH (the host triple whose /usr/include/<triple> holds
# asm/types.h), and optionally HISTORY_PACKETS: the library's
# ROCKETNET_HISTORY_PACKETS, which sets the kernel replay window, and
# MAX_CLIENTS: the library's ROCKETNET_MAX_CLIENTS, which sizes the kernel
# replay map (sdk/rocketnet_xdp_limits.h). Without either, the SDK headers'
# default applies (sdk/rocketnet_history.h, sdk/rocketnet.h): the default the
# library compiles with, so a build at the defaults passes neither. The values
# the program was compiled with are read back from the preprocessor and written
# into the generated header, which refuses to compile into a library built with
# different ones, so the kernel program and the library cannot disagree. The
# script runs only in a build with XDP (CMakeLists.txt): without a BPF-capable
# clang the XDP transport is compiled out instead.
#
# The object is byte-reproducible from the same sources and toolchain: the
# compile runs in SOURCE_DIR on relative paths, and -fdebug-compilation-dir
# and -ffile-prefix-map keep the checkout and build paths out of the debug
# and BTF strings.
include("${CMAKE_CURRENT_LIST_DIR}/XdpSources.cmake")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(xdp_object "${OUTPUT_DIR}/server_xdp.o")
set(xdp_header "${OUTPUT_DIR}/rocketnet_server_xdp.h")
file(REMOVE "${xdp_object}")

if(NOT CLANG)
    message(FATAL_ERROR "server_xdp.o: EmbedXdp.cmake needs CLANG, a clang with the BPF target")
endif()
# Clang may report a vendor component that the distribution's UAPI
# include directory omits (x86_64-pc-linux-gnu vs x86_64-linux-gnu).
if(NOT EXISTS "/usr/include/${MULTIARCH}/asm/types.h")
    string(REGEX REPLACE "^([^-]+)-(pc|unknown)-linux-" "\\1-linux-" MULTIARCH "${MULTIARCH}")
endif()
set(xdp_defines -D__linux__ -D__BPF__)
if(HISTORY_PACKETS)
    list(APPEND xdp_defines "-DROCKETNET_HISTORY_PACKETS=${HISTORY_PACKETS}")
endif()
if(MAX_CLIENTS)
    list(APPEND xdp_defines "-DROCKETNET_MAX_CLIENTS=${MAX_CLIENTS}")
endif()
execute_process(COMMAND "${CLANG}" ${xdp_defines} -ffreestanding -O2 -g
        "-I/usr/include/${MULTIARCH}" -Isdk
        -fdebug-compilation-dir=. "-ffile-prefix-map=${SOURCE_DIR}=." "-ffile-prefix-map=${OUTPUT_DIR}=."
        -target bpf -c sdk/xdp/server_xdp.c -o "${xdp_object}"
    WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE clang_result)
if(NOT clang_result EQUAL 0)
    message(FATAL_ERROR "server_xdp.o: the BPF compile failed (${clang_result})")
endif()
# The limits the object was compiled with, read with the compile's own defines
# and include paths, so an unset value reads as the headers' default.
set(xdp_limits_probe "${OUTPUT_DIR}/server_xdp_limits.c")
file(WRITE "${xdp_limits_probe}" "#include \"rocketnet_xdp_limits.h\"\nrocketnet_server_xdp_limits ROCKETNET_MAX_CLIENTS ROCKETNET_HISTORY_PACKETS\n")
execute_process(COMMAND "${CLANG}" ${xdp_defines} -ffreestanding
        "-I/usr/include/${MULTIARCH}" -Isdk -target bpf -E -P "${xdp_limits_probe}"
    WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE xdp_limits RESULT_VARIABLE probe_result)
file(REMOVE "${xdp_limits_probe}")
if(NOT probe_result EQUAL 0 OR NOT xdp_limits MATCHES "rocketnet_server_xdp_limits ([0-9]+) ([0-9]+)")
    message(FATAL_ERROR "server_xdp.o: reading ROCKETNET_MAX_CLIENTS and ROCKETNET_HISTORY_PACKETS failed (${probe_result})")
endif()
set(xdp_max_clients "${CMAKE_MATCH_1}")
set(xdp_history_packets "${CMAKE_MATCH_2}")

execute_process(COMMAND "${XXD}" -i server_xdp.o
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    OUTPUT_VARIABLE embedded RESULT_VARIABLE xxd_result)
if(NOT xxd_result EQUAL 0)
    message(FATAL_ERROR "server_xdp.o: embedding failed (${xxd_result})")
endif()
# Jammy's xxd has no -n option. A fixed basename gives a stable identifier
# on both old and new versions, then we name the private embedded symbols.
string(REPLACE "unsigned char server_xdp_o[]" "static const unsigned char rocketnet_server_xdp_o[]" embedded "${embedded}")
string(REPLACE "unsigned int server_xdp_o_len" "static const unsigned int rocketnet_server_xdp_o_len" embedded "${embedded}")

# The library includes this header; built with other limits than the program,
# it refuses to compile.
string(CONCAT xdp_limits_check
    "// server_xdp.o was compiled with these limits. The library that embeds it\n"
    "// must be built with the same: a build that sets ROCKETNET_MAX_CLIENTS or\n"
    "// ROCKETNET_HISTORY_PACKETS passes the value to cmake/EmbedXdp.cmake too.\n"
    "#include \"rocketnet_xdp_limits.h\"\n"
    "#define ROCKETNET_SERVER_XDP_MAX_CLIENTS ${xdp_max_clients}\n"
    "#define ROCKETNET_SERVER_XDP_HISTORY_PACKETS ${xdp_history_packets}\n"
    "#if ROCKETNET_MAX_CLIENTS != ROCKETNET_SERVER_XDP_MAX_CLIENTS\n"
    "#error server_xdp.o was compiled for another ROCKETNET_MAX_CLIENTS than this library: pass the library value to cmake/EmbedXdp.cmake as MAX_CLIENTS\n"
    "#endif\n"
    "#if ROCKETNET_HISTORY_PACKETS != ROCKETNET_SERVER_XDP_HISTORY_PACKETS\n"
    "#error server_xdp.o was compiled for another ROCKETNET_HISTORY_PACKETS than this library: pass the library value to cmake/EmbedXdp.cmake as HISTORY_PACKETS\n"
    "#endif\n")
file(WRITE "${xdp_header}.tmp" "// Generated by cmake/EmbedXdp.cmake: server_xdp.o, compiled at build time.\n${xdp_limits_check}${embedded}")
file(RENAME "${xdp_header}.tmp" "${xdp_header}")

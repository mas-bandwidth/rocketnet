# Refuses an embedded XDP object whose byte order is not the library's.
#
# cmake/EmbedXdp.cmake compiles server_xdp.o with clang's BPF target, and the
# bare "bpf" target takes the byte order of the machine clang runs on, not of
# the library being built. A cross build for a big-endian target on a
# little-endian build machine would embed a little-endian object that libbpf
# refuses to load on the target. This reads byte 5 of the object's ELF
# header (EI_DATA: 1 little endian, 2 big endian) and fails the build when it
# is not the target's, removing the generated header so the next build runs
# the compile again instead of keeping the wrong object.
#
# Inputs: OBJECT (server_xdp.o), HEADER (the generated header that embeds it)
# and BYTE_ORDER (CMAKE_C_BYTE_ORDER: LITTLE_ENDIAN or BIG_ENDIAN).

if(BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
    set(expected "01")
elseif(BYTE_ORDER STREQUAL "BIG_ENDIAN")
    set(expected "02")
else()
    file(REMOVE "${HEADER}")
    message(FATAL_ERROR "server_xdp.o: the target byte order is \"${BYTE_ORDER}\", not LITTLE_ENDIAN or BIG_ENDIAN, so the embedded object cannot be checked against it")
endif()

if(NOT EXISTS "${OBJECT}")
    file(REMOVE "${HEADER}")
    message(FATAL_ERROR "server_xdp.o: ${OBJECT} is missing")
endif()

file(READ "${OBJECT}" magic LIMIT 4 HEX)
file(READ "${OBJECT}" data OFFSET 5 LIMIT 1 HEX)
if(NOT magic STREQUAL "7f454c46")
    file(REMOVE "${HEADER}")
    message(FATAL_ERROR "server_xdp.o: ${OBJECT} is not an ELF object (magic ${magic})")
endif()
if(NOT data STREQUAL expected)
    file(REMOVE "${HEADER}")
    message(FATAL_ERROR "server_xdp.o: the object's ELF data encoding is ${data} (01 little endian, 02 big endian) but the library targets ${BYTE_ORDER}: the BPF compile took the build machine's byte order, and libbpf on the target would refuse the object")
endif()

# The inputs of server_xdp.o, the BPF object cmake/EmbedXdp.cmake compiles at
# build time and embeds in the library: CMake rebuilds the object when one of
# them changes, and tools/check_xdp_archive.py reads the same list.
set(ROCKETNET_XDP_SOURCES
    sdk/xdp/server_xdp.c
    sdk/rocketnet.h
    sdk/rocketnet_wire.h
    sdk/rocketnet_xdp_limits.h
    sdk/rocketnet_history.h)

# Let verification consume the same CMake list as generation and dependencies.
if(DEFINED ROCKETNET_XDP_SOURCE_LIST_OUTPUT)
    file(WRITE "${ROCKETNET_XDP_SOURCE_LIST_OUTPUT}" "")
    foreach(source IN LISTS ROCKETNET_XDP_SOURCES)
        file(APPEND "${ROCKETNET_XDP_SOURCE_LIST_OUTPUT}" "${source}\n")
    endforeach()
endif()

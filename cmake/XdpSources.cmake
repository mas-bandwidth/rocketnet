# Source paths are flattened to basenames in the runtime XDP source archive.
set(ROCKETNET_XDP_SOURCES
    server_xdp/Makefile
    server_xdp/server_xdp.c
    sdk/rocketnet_wire.h
    sdk/rocketnet_xdp_limits.h)

# Let verification consume the same CMake list as generation and dependencies.
if(DEFINED ROCKETNET_XDP_SOURCE_LIST_OUTPUT)
    file(WRITE "${ROCKETNET_XDP_SOURCE_LIST_OUTPUT}" "")
    foreach(source IN LISTS ROCKETNET_XDP_SOURCES)
        file(APPEND "${ROCKETNET_XDP_SOURCE_LIST_OUTPUT}" "${source}\n")
    endforeach()
endif()

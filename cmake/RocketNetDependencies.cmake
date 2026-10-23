# Resolved on the consuming machine: no producer's absolute dependency paths
# are written into the installed package.
find_package(Threads REQUIRED)
if(ROCKETNET_USE_SYSTEM_SODIUM AND NOT TARGET RocketNet::Sodium)
    add_library(RocketNet::Sodium INTERFACE IMPORTED)
    if(WIN32)
        find_path(ROCKETNET_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
        find_library(ROCKETNET_SODIUM_LIBRARY NAMES libsodium sodium REQUIRED)
        set_target_properties(RocketNet::Sodium PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_SODIUM_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${ROCKETNET_SODIUM_LIBRARY}")
        set(ROCKETNET_SODIUM_VERSION "")
        if(EXISTS "${ROCKETNET_SODIUM_INCLUDE_DIR}/sodium/version.h")
            file(STRINGS "${ROCKETNET_SODIUM_INCLUDE_DIR}/sodium/version.h" ROCKETNET_SODIUM_VERSION_LINE
                REGEX "^#define SODIUM_VERSION_STRING \"[0-9.]+\"")
            string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" ROCKETNET_SODIUM_VERSION "${ROCKETNET_SODIUM_VERSION_LINE}")
        endif()
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(ROCKETNET_SODIUM REQUIRED IMPORTED_TARGET libsodium)
        set_target_properties(RocketNet::Sodium PROPERTIES
            INTERFACE_LINK_LIBRARIES PkgConfig::ROCKETNET_SODIUM)
    endif()
    # The external libsodium floor; the vendored copy is 1.0.22 (sodium/sodium.h:1972).
    if(NOT ROCKETNET_SODIUM_VERSION OR ROCKETNET_SODIUM_VERSION VERSION_LESS 1.0.20)
        if(NOT ROCKETNET_SODIUM_VERSION)
            set(ROCKETNET_SODIUM_VERSION "of unknown version")
        endif()
        message(FATAL_ERROR "ROCKETNET_USE_SYSTEM_SODIUM: found libsodium ${ROCKETNET_SODIUM_VERSION}; "
            "rocketnet requires libsodium 1.0.20 or later. Install a newer libsodium, or "
            "configure with -DROCKETNET_USE_SYSTEM_SODIUM=OFF to build the vendored 1.0.22.")
    endif()
endif()
# New distributions keep AF_XDP in libxdp. Ubuntu 22.04 keeps it in
# libbpf 0.5 (bpf/xsk.h) and has no libxdp package. Installed packages must
# resolve the same provider they were built with, even if both are present.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ROCKETNET_XDP)
    if(NOT TARGET RocketNet::Bpf)
        find_library(ROCKETNET_Bpf_LIBRARY NAMES bpf)
        find_path(ROCKETNET_Bpf_INCLUDE_DIR NAMES bpf/libbpf.h)
        if(ROCKETNET_Bpf_LIBRARY AND ROCKETNET_Bpf_INCLUDE_DIR)
            add_library(RocketNet::Bpf UNKNOWN IMPORTED)
            set_target_properties(RocketNet::Bpf PROPERTIES
                IMPORTED_LOCATION "${ROCKETNET_Bpf_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_Bpf_INCLUDE_DIR}")
        endif()
    endif()
    if(NOT TARGET RocketNet::Xdp)
        if(NOT ROCKETNET_XDP_LEGACY_LIBBPF)
            find_library(ROCKETNET_Xdp_LIBRARY NAMES xdp)
            find_path(ROCKETNET_Xdp_INCLUDE_DIR NAMES xdp/libxdp.h)
        endif()
        if(NOT ROCKETNET_XDP_LEGACY_LIBBPF AND ROCKETNET_Xdp_LIBRARY AND ROCKETNET_Xdp_INCLUDE_DIR)
            add_library(RocketNet::Xdp UNKNOWN IMPORTED)
            set_target_properties(RocketNet::Xdp PROPERTIES
                IMPORTED_LOCATION "${ROCKETNET_Xdp_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_Xdp_INCLUDE_DIR}")
            set(ROCKETNET_XDP_LEGACY_LIBBPF OFF)
        elseif(TARGET RocketNet::Bpf AND
               (NOT ROCKETNET_XDP_REQUIRED OR ROCKETNET_XDP_LEGACY_LIBBPF) AND
               EXISTS "${ROCKETNET_Bpf_INCLUDE_DIR}/bpf/xsk.h")
            add_library(RocketNet::Xdp INTERFACE IMPORTED)
            set_target_properties(RocketNet::Xdp PROPERTIES
                INTERFACE_LINK_LIBRARIES RocketNet::Bpf
                INTERFACE_COMPILE_DEFINITIONS ROCKETNET_XDP_LEGACY_LIBBPF=1)
            set(ROCKETNET_XDP_LEGACY_LIBBPF ON)
            message(STATUS "XDP: using legacy AF_XDP APIs from libbpf (Ubuntu 22.04)")
        endif()
    endif()
    if(ROCKETNET_XDP_REQUIRED AND (NOT TARGET RocketNet::Bpf OR NOT TARGET RocketNet::Xdp))
        message(FATAL_ERROR "RocketNet requires the XDP provider used to build it (legacy libbpf=${ROCKETNET_XDP_LEGACY_LIBBPF})")
    endif()
endif()
# liburing for the io_uring server transport: optional when building (absent,
# the transport compiles to a create that refuses), required when the installed
# package was built with it (ROCKETNET_IO_URING_REQUIRED, set by the config).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ROCKETNET_IO_URING AND NOT TARGET RocketNet::Uring)
    if(ROCKETNET_IO_URING_REQUIRED)
        find_library(ROCKETNET_Uring_LIBRARY NAMES uring REQUIRED)
        find_path(ROCKETNET_Uring_INCLUDE_DIR NAMES liburing.h REQUIRED)
    else()
        find_library(ROCKETNET_Uring_LIBRARY NAMES uring)
        find_path(ROCKETNET_Uring_INCLUDE_DIR NAMES liburing.h)
    endif()
    if(ROCKETNET_Uring_LIBRARY AND ROCKETNET_Uring_INCLUDE_DIR)
        add_library(RocketNet::Uring UNKNOWN IMPORTED)
        set_target_properties(RocketNet::Uring PROPERTIES
            IMPORTED_LOCATION "${ROCKETNET_Uring_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_Uring_INCLUDE_DIR}")
    else()
        message(STATUS "liburing not found: the io_uring server transport is built to refuse")
    endif()
endif()

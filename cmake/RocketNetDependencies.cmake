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
# libbpf and libxdp for the XDP server transport: optional when building
# (absent, CMakeLists.txt compiles XDP out), required when the installed
# package was built with XDP (ROCKETNET_XDP_REQUIRED, set by the config).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ROCKETNET_XDP)
    foreach(dependency Bpf Xdp)
        if(NOT TARGET RocketNet::${dependency})
            string(TOLOWER "${dependency}" name)
            if(ROCKETNET_XDP_REQUIRED)
                find_library(ROCKETNET_${dependency}_LIBRARY NAMES ${name} REQUIRED)
                find_path(ROCKETNET_${dependency}_INCLUDE_DIR NAMES ${name}/lib${name}.h REQUIRED)
            else()
                find_library(ROCKETNET_${dependency}_LIBRARY NAMES ${name})
                find_path(ROCKETNET_${dependency}_INCLUDE_DIR NAMES ${name}/lib${name}.h)
            endif()
            if(ROCKETNET_${dependency}_LIBRARY AND ROCKETNET_${dependency}_INCLUDE_DIR)
                add_library(RocketNet::${dependency} UNKNOWN IMPORTED)
                set_target_properties(RocketNet::${dependency} PROPERTIES
                    IMPORTED_LOCATION "${ROCKETNET_${dependency}_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_${dependency}_INCLUDE_DIR}")
            endif()
        endif()
    endforeach()
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

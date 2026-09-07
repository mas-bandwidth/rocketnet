# Resolved on the consuming machine: no producer's absolute dependency paths
# are written into the installed package.
find_package(Threads REQUIRED)
if(NOT TARGET RocketNet::Sodium)
    add_library(RocketNet::Sodium INTERFACE IMPORTED)
    if(WIN32)
        find_path(ROCKETNET_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
        find_library(ROCKETNET_SODIUM_LIBRARY NAMES libsodium sodium REQUIRED)
        set_target_properties(RocketNet::Sodium PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_SODIUM_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${ROCKETNET_SODIUM_LIBRARY}")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(ROCKETNET_SODIUM REQUIRED IMPORTED_TARGET libsodium)
        set_target_properties(RocketNet::Sodium PROPERTIES
            INTERFACE_LINK_LIBRARIES PkgConfig::ROCKETNET_SODIUM)
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(dependency Bpf Xdp)
        if(NOT TARGET RocketNet::${dependency})
            string(TOLOWER "${dependency}" name)
            find_library(ROCKETNET_${dependency}_LIBRARY NAMES ${name} REQUIRED)
            find_path(ROCKETNET_${dependency}_INCLUDE_DIR NAMES ${name}/lib${name}.h REQUIRED)
            add_library(RocketNet::${dependency} UNKNOWN IMPORTED)
            set_target_properties(RocketNet::${dependency} PROPERTIES
                IMPORTED_LOCATION "${ROCKETNET_${dependency}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${ROCKETNET_${dependency}_INCLUDE_DIR}")
        endif()
    endforeach()
endif()

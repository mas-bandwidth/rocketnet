
cmake_minimum_required(VERSION 3.10)
add_compile_definitions(ROCKETNET_DEVELOPMENT=0)

# A no-op build prints nothing. The per-target "Built target" lines were the only
# output a rebuild-with-nothing-to-do produced, and they carry no information; a
# real build still prints its [ x%] progress lines, and errors are untouched.
set(CMAKE_TARGET_MESSAGES OFF)

# server transport driver selection is runtime (#106): one build carries every
# driver its platform has -- linux carries xdp, gso and portable -- and the
# transport walks the config's candidate list at create (config driver field,
# ROCKETNET_DRIVER env). the old ROCKETNET_SERVER_XDP / ROCKETNET_SERVER_GSO
# compile options are gone; passing them now errors in the transport source.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# server xdp — the XDP program source is packed as a tarball into a generated
# header; the SDK builds and loads it on the target kernel at runtime.
# regeneration needs bash/tar/xxd, which windows doesn't reliably have; the
# header is committed and the XDP dataplane is linux-only, so the windows
# build consumes the committed artifact as-is.

if(NOT WIN32)
add_custom_command(
    COMMENT "Generating rocketnet_server_xdp.h"
    OUTPUT ${CMAKE_SOURCE_DIR}/sdk/rocketnet_server_xdp.h
    COMMAND bash -c "mkdir -p rocketnet_server_xdp"
    COMMAND bash -c "cp ${CMAKE_SOURCE_DIR}/server_xdp/Makefile rocketnet_server_xdp"
    COMMAND bash -c "cp ${CMAKE_SOURCE_DIR}/server_xdp/server_xdp.c rocketnet_server_xdp"
    COMMAND bash -c "cp ${CMAKE_SOURCE_DIR}/sdk/rocketnet_wire.h rocketnet_server_xdp"
    COMMAND bash -c "tar -zcvf rocketnet_server_xdp.tar.gz -C rocketnet_server_xdp ." >/dev/null 2>&1
    COMMAND bash -c "rm -rf rocketnet_server_xdp"
    COMMAND bash -c "xxd -i rocketnet_server_xdp.tar.gz" > ${CMAKE_SOURCE_DIR}/sdk/rocketnet_server_xdp.h
    COMMAND bash -c "rm -f rocketnet_server_xdp.tar.gz"
    DEPENDS ${CMAKE_SOURCE_DIR}/server_xdp/Makefile ${CMAKE_SOURCE_DIR}/server_xdp/server_xdp.c ${CMAKE_SOURCE_DIR}/sdk/rocketnet_wire.h
)
endif()


# rocketnet

project(rocketnet)
enable_language(CXX)

# strict floating point. Estate policy for mas-bandwidth network libraries: "generally
# speaking, network libraries we work on require -ffp-contract=off" (Glenn, 2026-08-24).
#   - GCC/Clang: -ffp-contract=off. Not merely "no -ffast-math" -- GCC's default is
#     -ffp-contract=fast, which contracts across statements, and clang's default =on still
#     fuses within a single expression.
#   - MSVC: /fp:precise, which since VS 2022 does not imply contraction.
# rocketnet's float wire encoding is a bit copy (memcpy of the bits), so
# this is the family floor rather than a wire fix: the float arithmetic here is the RTT,
# jitter and bandwidth tracking. Contraction is architecture-dependent (FMA is in the
# aarch64 baseline and absent from the x86-64 one), so any float arithmetic that ever
# nears the wire diverges bit-wise between architectures without it. Set here, directly
# after the first project() call -- the MSVC variable is undefined before the compiler
# is detected, and testing it earlier silently handed every compiler the gcc/clang
# branch (the windows leg's D9002 "ignoring unknown option -ffp-contract=off" caught
# it). Every target in this file -- C and C++ -- is created after this line, so all
# of them build with the floor; CI builds through this file.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()

set(ROCKETNET_SOURCE
    sdk/rocketnet.cpp
    sdk/rocketnet_ack.cpp
    sdk/rocketnet_address.cpp
    sdk/rocketnet_base64.cpp
    sdk/rocketnet_client_protocol.cpp
    sdk/rocketnet_client_slots.cpp
    sdk/rocketnet_client_transport.cpp
    sdk/rocketnet_config.cpp
    sdk/rocketnet_connect_token.cpp
    sdk/rocketnet_counters.cpp
    sdk/rocketnet_encryption_map.cpp
    sdk/rocketnet_fragment.cpp
    sdk/rocketnet_frame_sync.cpp
    sdk/rocketnet_hash.cpp
    sdk/rocketnet_keys.cpp
    sdk/rocketnet_network_simulator.cpp
    sdk/rocketnet_platform.cpp
    sdk/rocketnet_protocol.cpp
    sdk/rocketnet_replay.cpp
    sdk/rocketnet_server_gso_linux.cpp
    sdk/rocketnet_server_protocol.cpp
    sdk/rocketnet_server_socket_linux.cpp
    sdk/rocketnet_server_transport.cpp
    sdk/rocketnet_token_tables.cpp
)
file(GLOB ROCKETNET_HEADERS sdk/*.h)
add_library(rocketnet ${ROCKETNET_SOURCE} ${ROCKETNET_HEADERS} sdk/rocketnet_server_xdp.h)
find_package(Threads REQUIRED)

# libsodium, per the crypto ruling in REQUIREMENTS.md. Interim: the system
# package (brew on mac, libsodium-dev on linux, vcpkg on windows) so
# construction moves quickly; the gutted two-file vendor replaces this before
# anything ships. Windows has no pkg-config, so the library is found directly
# there -- vcpkg's toolchain file puts its installed tree on the find path,
# and find_library returns a full path so SODIUM_LIBRARY_DIRS stays empty.
if(WIN32)
    find_path(SODIUM_INCLUDE_DIRS sodium.h REQUIRED)
    find_library(SODIUM_LIBRARIES NAMES libsodium sodium REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED libsodium)
endif()
target_include_directories(rocketnet PUBLIC ${SODIUM_INCLUDE_DIRS})
target_link_directories(rocketnet PUBLIC ${SODIUM_LIBRARY_DIRS})
target_link_libraries(rocketnet ${SODIUM_LIBRARIES})
if (APPLE)
    target_link_libraries(rocketnet Threads::Threads "-framework CoreFoundation" "-framework SystemConfiguration")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # every linux consumer inherits the XDP dataplane's libraries now that
    # the driver ladder is runtime and one build carries all three drivers
    target_link_libraries(rocketnet Threads::Threads bpf xdp)
else()
    target_link_libraries(rocketnet Threads::Threads)
endif()


# rocketnet shared -- the same sources built as a shared library, for language
# bindings that load librocketnet at runtime (the C# binding in csharp/).
# ROCKETNET_SHARED + ROCKETNET_EXPORT select the header's export machinery on
# the build side; consumers define only ROCKETNET_SHARED. The add_dependencies
# on the static target orders the generated rocketnet_server_xdp.h without
# attaching the custom command to two targets.

add_library(rocketnet_shared SHARED ${ROCKETNET_SOURCE})
add_dependencies(rocketnet_shared rocketnet)
set_target_properties(rocketnet_shared PROPERTIES
    OUTPUT_NAME rocketnet
    ARCHIVE_OUTPUT_NAME rocketnet_import
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_definitions(rocketnet_shared PRIVATE ROCKETNET_SHARED ROCKETNET_EXPORT)
target_include_directories(rocketnet_shared PUBLIC ${SODIUM_INCLUDE_DIRS})
target_link_directories(rocketnet_shared PUBLIC ${SODIUM_LIBRARY_DIRS})
target_link_libraries(rocketnet_shared ${SODIUM_LIBRARIES})
if (APPLE)
    target_link_libraries(rocketnet_shared Threads::Threads "-framework CoreFoundation" "-framework SystemConfiguration")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(rocketnet_shared Threads::Threads bpf xdp)
else()
    target_link_libraries(rocketnet_shared Threads::Threads)
endif()


# client

project(client)
enable_language(CXX)
add_executable(client client.c)
target_include_directories(client PRIVATE sdk)
add_dependencies(client rocketnet)
target_link_libraries(client rocketnet)


# server

project(server)
enable_language(CXX)
add_executable(server server.c)
target_include_directories(server PRIVATE sdk)
add_dependencies(server rocketnet)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(server rocketnet bpf xdp)
else()
    target_link_libraries(server rocketnet)
endif()


# test

project(test)
enable_language(CXX)
add_executable(test test.cpp)
target_include_directories(test PRIVATE sdk)
add_library(rocketnet_test_support STATIC sdk/rocketnet_tests.cpp)
target_link_libraries(rocketnet_test_support PUBLIC rocketnet)
target_link_libraries(test rocketnet_test_support)


# soak

project(soak)
enable_language(C)
# test_serialize -- the only build that sees the C++ stream surface

project(test_serialize)

add_executable(test_serialize test_serialize.cpp)
target_include_directories(test_serialize PRIVATE sdk test)
add_dependencies(test_serialize rocketnet)
target_link_libraries(test_serialize rocketnet)

add_executable(soak soak.c)
target_include_directories(soak PRIVATE sdk)
add_dependencies(soak rocketnet)
target_link_libraries(soak rocketnet)


# soak_sim

project(soak_sim)
enable_language(CXX)
add_executable(soak_sim soak_sim.cpp)
target_include_directories(soak_sim PRIVATE sdk)
add_dependencies(soak_sim rocketnet)
target_link_libraries(soak_sim rocketnet)


# keygen

project(keygen)
enable_language(CXX)
add_executable(keygen keygen.cpp)
target_include_directories(keygen PRIVATE sdk)
add_dependencies(keygen rocketnet)
target_link_libraries(keygen rocketnet)

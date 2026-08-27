
cmake_minimum_required(VERSION 3.10)
add_compile_definitions(ROCKETNET_DEVELOPMENT=0)

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
# nears the wire diverges bit-wise between architectures without it. Set globally, so
# every target in this file -- C and C++ -- builds with it; CI builds through this file.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# server xdp — the XDP program source is packed as a tarball into a generated
# header; the SDK builds and loads it on the target kernel at runtime.

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


# rocketnet

project(rocketnet)
enable_language(CXX)
file(GLOB ROCKETNET_SOURCE sdk/*.cpp)
file(GLOB ROCKETNET_HEADERS sdk/*.h)
add_library(rocketnet ${ROCKETNET_SOURCE} ${ROCKETNET_HEADERS} sdk/rocketnet_server_xdp.h)
find_package(Threads REQUIRED)

# libsodium, per the crypto ruling in REQUIREMENTS.md. Interim: the system
# package (brew on mac, libsodium-dev on linux) so construction moves quickly;
# the gutted two-file vendor replaces this before anything ships.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)
target_include_directories(rocketnet PUBLIC ${SODIUM_INCLUDE_DIRS})
target_link_directories(rocketnet PUBLIC ${SODIUM_LIBRARY_DIRS})
target_link_libraries(rocketnet ${SODIUM_LIBRARIES})
if (APPLE)
    target_link_libraries(rocketnet Threads::Threads "-framework CoreFoundation" "-framework SystemConfiguration")
else()
    target_link_libraries(rocketnet Threads::Threads)
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
if (LINUX)
    target_link_libraries(server rocketnet bpf xdp)
else()
    target_link_libraries(server rocketnet)
endif()


# test

project(test)
enable_language(CXX)
add_executable(test test.cpp)
target_include_directories(test PRIVATE sdk)
add_dependencies(test rocketnet)
target_link_libraries(test rocketnet)


# soak

project(soak)
enable_language(C)
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

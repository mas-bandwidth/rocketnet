
cmake_minimum_required(VERSION 3.10)
add_compile_definitions(ROCKETNET_DEVELOPMENT=0)

# strict floating point. Estate policy for mas-bandwidth network libraries: "generally
# speaking, network libraries we work on require -ffp-contract=off" (Glenn, 2026-08-24).
#   - GCC/Clang: -ffp-contract=off. Not merely "no -ffast-math" -- GCC's default is
#     -ffp-contract=fast, which contracts across statements, and clang's default =on still
#     fuses within a single expression.
#   - MSVC: /fp:precise, which since VS 2022 does not imply contraction.
# rocketnet's float wire encoding is a bit copy (shared_write_float32/64 memcpy the bits), so
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
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ../dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ../dist)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include_directories(
	"." 
)


# hydrogen

project(hydrogen)
enable_language(C)
file(GLOB HYDROGEN_SOURCE ../lib/hydrogen/*.c)
file(GLOB HYDROGEN_HEADERS ../lib/hydrogen/*.h)
add_library(hydrogen ${HYDROGEN_SOURCE} ${HYDROGEN_HEADERS})


# platform

project(platform)
enable_language(C)
file(GLOB PLATFORM_SOURCE ../lib/platform/*.c)
file(GLOB PLATFORM_HEADERS ../lib/platform/*.h)
add_library(platform ${PLATFORM_SOURCE} ${PLATFORM_HEADERS})
target_include_directories(platform PRIVATE ../lib)
add_dependencies(platform hydrogen)


# shared

project(shared)
enable_language(C)
file(GLOB SHARED_SOURCE ../lib/shared/*.c)
file(GLOB SHARED_HEADERS ../lib/shared/*.h)
add_library(shared ${SHARED_SOURCE} ${SHARED_HEADERS})
target_include_directories(shared PRIVATE ../lib)
add_dependencies(shared platform)
if (UNIX)
    target_link_libraries(shared hydrogen)
endif()


# client backend

add_custom_command(
    COMMENT "Copying proton.h for client backend"
    OUTPUT ../cmd/client_backend/proton.h
    COMMAND bash -c "cp ../lib/proton/proton.h ../cmd/client_backend/proton.h"
    DEPENDS ../lib/proton/proton.h
)

add_custom_command(
    COMMENT "Generating client_backend_xdp_source.h"
    OUTPUT ../cmd/client_backend/client_backend_xdp_source.h
    COMMAND bash -c "mkdir -p ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "cp ../cmd/client_backend/Makefile ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "cp ../cmd/client_backend/proton.h ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "cp ../cmd/client_backend/client_backend_xdp.c ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "cp ../cmd/client_backend/client_backend_shared.h ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "cp ../cmd/client_backend/client_backend_constants.h ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "tar -zcvf ../cmd/client_backend/client_backend_xdp_source.tar.gz -C ../cmd/client_backend/client_backend_xdp_source ." >/dev/null 2>&1
    COMMAND bash -c "rm -rf ../cmd/client_backend/client_backend_xdp_source"
    COMMAND bash -c "xxd -i ../cmd/client_backend/client_backend_xdp_source.tar.gz" > ../cmd/client_backend/client_backend_xdp_source.h
    COMMAND bash -c "rm -f ../cmd/client_backend/client_backend_xdp_source.tar.gz"
    DEPENDS ../cmd/client_backend/Makefile ../cmd/client_backend/client_backend_xdp.c ../cmd/client_backend/client_backend_shared.h ../cmd/client_backend/client_backend_constants.h ../cmd/client_backend/proton.h
)

project(client_backend)

file(GLOB CLIENT_BACKEND_SOURCE ../cmd/client_backend/*.c)
file(GLOB CLIENT_BACKEND_HEADERS ../cmd/client_backend/*.h)
add_executable(client_backend ${CLIENT_BACKEND_SOURCE} ${CLIENT_BACKEND_HEADERS} ../cmd/client_backend/client_backend_xdp_source.h)
target_include_directories(client_backend PRIVATE ../lib)
add_dependencies(client_backend platform shared)
find_package(Threads REQUIRED)
if (APPLE)
	target_link_libraries(client_backend platform shared Threads::Threads)
elseif (UNIX)
	target_link_libraries(client_backend platform shared bpf xdp Threads::Threads)
endif()


# server xdp

add_custom_command(
    COMMENT "Generating rocketnet_server_xdp.h"
    OUTPUT ../sdk/rocketnet_server_xdp.h
    COMMAND bash -c "mkdir -p rocketnet_server_xdp"
    COMMAND bash -c "cp ../cmd/server_xdp/Makefile rocketnet_server_xdp"
    COMMAND bash -c "cp ../cmd/server_xdp/server_xdp.c rocketnet_server_xdp"
    COMMAND bash -c "tar -zcvf rocketnet_server_xdp.tar.gz -C rocketnet_server_xdp ." >/dev/null 2>&1
    COMMAND bash -c "rm -rf rocketnet_server_xdp"
    COMMAND bash -c "xxd -i rocketnet_server_xdp.tar.gz" > ../sdk/rocketnet_server_xdp.h
    COMMAND bash -c "rm -f rocketnet_server_xdp.tar.gz"
    DEPENDS ../cmd/server_xdp/Makefile ../cmd/server_xdp/server_xdp.c
)


# ---------------------------------------------------------------------
#                              SDK BELOW HERE
# ---------------------------------------------------------------------


# rocketnet

project(rocketnet)
enable_language(CXX)
file(GLOB ROCKETNET_SOURCE ../sdk/*.cpp)
file(GLOB ROCKETNET_HEADERS ../sdk/*.h)
add_library(rocketnet ${ROCKETNET_SOURCE} ${ROCKETNET_HEADERS} ../sdk/rocketnet_server_xdp.h)
find_package(Threads REQUIRED)
if (APPLE)
    target_link_libraries(rocketnet shared Threads::Threads "-framework CoreFoundation" "-framework SystemConfiguration")
else()
    target_link_libraries(rocketnet shared Threads::Threads)
endif()


# client

project(client)
enable_language(CXX)
add_executable(client ../cmd/client/client.cpp)
target_include_directories(client PRIVATE ../sdk)
add_dependencies(client rocketnet)
target_link_libraries(client rocketnet)


# server

project(server)
enable_language(CXX)
add_executable(server ../cmd/server/server.cpp)
target_include_directories(server PRIVATE ../sdk)
add_dependencies(server rocketnet)
if (LINUX)
    target_link_libraries(server rocketnet bpf xdp)
else()
    target_link_libraries(server rocketnet)
endif()


# test

project(test)
enable_language(CXX)
add_executable(test ../cmd/test/test.cpp)
target_include_directories(test PRIVATE ../sdk)
add_dependencies(test rocketnet)
target_link_libraries(test rocketnet)


# keygen

project(keygen)
enable_language(CXX)
add_executable(keygen ../cmd/keygen/keygen.cpp)
target_include_directories(keygen PRIVATE ../sdk)
add_dependencies(keygen rocketnet)
target_link_libraries(keygen rocketnet)


if (LINUX)

# sender

project(sender)
enable_language(CXX)
add_executable(sender ../cmd/sender/sender.cpp)
target_include_directories(sender PRIVATE ../sdk)
add_dependencies(sender rocketnet)
target_link_libraries(sender rocketnet bpf xdp)

endif()

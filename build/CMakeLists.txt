
cmake_minimum_required(VERSION 3.10)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ../dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ../dist)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ../dist)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include_directories(
	".."
)

# strict floating point. Estate policy for mas-bandwidth network libraries: "generally
# speaking, network libraries we work on require -ffp-contract=off" (Glenn, 2026-08-24).
#   - GCC/Clang: -ffp-contract=off. Not merely "no -ffast-math" -- GCC's default is
#     -ffp-contract=fast, which contracts across statements, and clang's default =on still
#     fuses within a single expression.
#   - MSVC: /fp:precise, which since VS 2022 does not imply contraction.
# rocketnet's floats are connection config and stats (RTT, jitter, packet loss, bandwidth),
# so this is the family floor rather than a wire fix: contraction is architecture-dependent
# (FMA is in the aarch64 baseline and absent from the x86-64 one), so any float arithmetic
# that ever nears the wire diverges bit-wise between architectures without it. Set globally,
# so every target in this file -- the library, the C++ tests and the C examples -- builds
# with it; CI builds through this file.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()


# rocketnet — C-like C++ implementation behind the C header

project(rocketnet)
enable_language(CXX)
add_library(rocketnet
	../rocketnet.cpp
	../rocketnet_address.cpp
	../rocketnet_platform.cpp
	../rocketnet.h
	../rocketnet_internal.h
	../rocketnet_address.h
	../rocketnet_platform.h
)


# test — C++, so the header is exercised under C++ linkage

project(test)
enable_language(CXX)
add_executable(test ../test.cpp ../rocketnet_tests.cpp ../rocketnet_tests.h)
add_dependencies(test rocketnet)
target_link_libraries(test rocketnet)


# examples — C, so the header is proven to be a C header and the library
# is proven to link from plain C

project(simple_client)
enable_language(C)
add_executable(simple_client ../examples/simple_client.c)
add_dependencies(simple_client rocketnet)
target_link_libraries(simple_client rocketnet)

project(simple_server)
enable_language(C)
add_executable(simple_server ../examples/simple_server.c)
add_dependencies(simple_server rocketnet)
target_link_libraries(simple_server rocketnet)

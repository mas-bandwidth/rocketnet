
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

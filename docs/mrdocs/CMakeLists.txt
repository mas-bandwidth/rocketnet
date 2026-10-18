cmake_minimum_required(VERSION 3.16)
project(rocketnet_mrdocs LANGUAGES C CXX)

option(MRDOCS_BUILD "MrDocs compilation database build" OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ROCKETNET_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" REALPATH)

add_library(rocketnet_mrdocs OBJECT
    public.cpp
    public_c.c
)

target_include_directories(rocketnet_mrdocs PRIVATE
    "${ROCKETNET_SDK_DIR}"
)

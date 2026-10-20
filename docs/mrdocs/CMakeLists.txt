cmake_minimum_required(VERSION 3.16)
project(rocketnet_mrdocs LANGUAGES C CXX)

option(MRDOCS_BUILD "MrDocs compilation database build" OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ROCKETNET_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" REALPATH)
get_filename_component(ROCKETNET_TOOLS_DOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/docs" REALPATH)

# The counters are an X-macro table whose comments the preprocessor drops, so
# the docs build documents them from a documentation-only expansion of the
# rows (tools/docs/counters_doc.py), written here at configure time as
# gen/rocketnet_counters.h and included by public.cpp only. The library never
# sees it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ROCKETNET_DOCS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${ROCKETNET_TOOLS_DOCS_DIR}/counters_doc.py"
            "${ROCKETNET_SDK_DIR}/rocketnet_counters.h"
            "${ROCKETNET_DOCS_GEN_DIR}/rocketnet_counters.h"
    RESULT_VARIABLE ROCKETNET_COUNTERS_DOC_RESULT
)
if(NOT ROCKETNET_COUNTERS_DOC_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/docs/counters_doc.py failed: ${ROCKETNET_COUNTERS_DOC_RESULT}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ROCKETNET_SDK_DIR}/rocketnet_counters.h"
    "${ROCKETNET_TOOLS_DOCS_DIR}/counters_doc.py"
)

add_library(rocketnet_mrdocs OBJECT
    public.cpp
    public_c.c
)

# the generated directory first: public.cpp's rocketnet_counters.h is the
# documented expansion, and nothing in this build includes the real one
target_include_directories(rocketnet_mrdocs PRIVATE
    "${ROCKETNET_DOCS_GEN_DIR}"
    "${ROCKETNET_SDK_DIR}"
)

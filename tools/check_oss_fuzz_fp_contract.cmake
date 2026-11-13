cmake_minimum_required(VERSION 3.15)

# Checks the one build path that does not go through CMakeLists.txt.
#
# OSS-Fuzz builds the sdk, the vendored sodium and the fuzz targets through
# tests/oss-fuzz/build.sh with its own $CFLAGS/$CXXFLAGS. Those flags never carry
# -ffp-contract=off, and GCC's default is -ffp-contract=fast (clang's default
# =on still fuses within an expression), so without an explicit flag the fuzzed
# build contracts float arithmetic and diverges bit-wise from every CMake
# build. This pins -ffp-contract=off onto every compile line in the script
# so the OSS-Fuzz build computes what every CMake build computes.
#
# cmake -DROCKETNET_OSS_FUZZ_BUILD_SCRIPT=<path> -P tools/check_oss_fuzz_fp_contract.cmake

if(NOT ROCKETNET_OSS_FUZZ_BUILD_SCRIPT)
    message(FATAL_ERROR "ROCKETNET_OSS_FUZZ_BUILD_SCRIPT is required")
endif()

file(READ "${ROCKETNET_OSS_FUZZ_BUILD_SCRIPT}" build_script)

string(REPLACE "\n" ";" build_lines "${build_script}")

set(compile_lines 0)
set(failures "")

foreach(line IN LISTS build_lines)
    # a shell comment is not code: a flag in a trailing "# ..." does not count,
    # and a commented-out command is no compile line
    string(REGEX REPLACE "(^|[ \t])#.*$" "" code "${line}")
    # a $CC or $CXX compilation is one that passes -c, or one that names a C or
    # C++ source (a one-step compile-and-link); a link of objects alone does not
    # compile and needs no codegen flag.
    if(code MATCHES "\\$(CC|CXX) " AND
       (code MATCHES "[-]c " OR code MATCHES "\\.(c|cc|cpp|cxx)([ \t\"']|$)"))
        math(EXPR compile_lines "${compile_lines} + 1")
        if(NOT code MATCHES "[-]ffp-contract=off")
            list(APPEND failures "${line}")
        endif()
    endif()
endforeach()

if(compile_lines EQUAL 0)
    message(FATAL_ERROR "0 compile lines: no $CC or $CXX compile line found in ${ROCKETNET_OSS_FUZZ_BUILD_SCRIPT}")
endif()

if(failures)
    foreach(failure IN LISTS failures)
        message(SEND_ERROR "tests/oss-fuzz/build.sh compile line lacks -ffp-contract=off: ${failure}")
    endforeach()
    message(FATAL_ERROR "tests/oss-fuzz/build.sh does not build with -ffp-contract=off")
endif()

message(STATUS "tests/oss-fuzz/build.sh builds every compile line (${compile_lines}) with -ffp-contract=off")

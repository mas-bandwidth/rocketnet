cmake_minimum_required(VERSION 3.20)
# 12.0: the Xcode 27 build system refuses 11.0 (its supported range starts at
# 12.0), so it is the lowest target both the Xcode generator and the Command
# Line Tools build on both universal slices. Set before project(); -D overrides.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum supported macOS version" FORCE)
endif()
# CMake's VERSION holds numbers only, so it is the 1.0.0 of v1.0.0-preview-1;
# the release label, prerelease included, is ROCKETNET_VERSION_STRING below,
# read from sdk/rocketnet.h, and the installed package carries both.
project(rocketnet VERSION 1.0.0 LANGUAGES C CXX)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/rocketnet.h" ROCKETNET_VERSION_FULL_LINE
    REGEX "^ *#define ROCKETNET_VERSION_FULL +\"v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?\"$")
list(LENGTH ROCKETNET_VERSION_FULL_LINE ROCKETNET_VERSION_FULL_COUNT)
if(NOT ROCKETNET_VERSION_FULL_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected one shipping ROCKETNET_VERSION_FULL label beginning with v")
endif()
string(REGEX REPLACE ".*\"([^\"]+)\"$" "\\1" ROCKETNET_VERSION_STRING "${ROCKETNET_VERSION_FULL_LINE}")
string(REGEX REPLACE "^v([0-9]+\\.[0-9]+\\.[0-9]+).*" "\\1" ROCKETNET_VERSION_NUMERIC "${ROCKETNET_VERSION_STRING}")
string(REGEX REPLACE "^v[0-9]+\\.[0-9]+\\.[0-9]+-?" "" ROCKETNET_VERSION_PRERELEASE "${ROCKETNET_VERSION_STRING}")
if(NOT ROCKETNET_VERSION_NUMERIC STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "sdk/rocketnet.h ROCKETNET_VERSION_FULL \"${ROCKETNET_VERSION_STRING}\" "
        "does not match project VERSION ${PROJECT_VERSION}")
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

# Defaults preserve the checkout's make workflow; embedded consumers build only libraries.
set(ROCKETNET_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ROCKETNET_TOP_LEVEL ON)
endif()
option(ROCKETNET_USE_SYSTEM_SODIUM "Use an external libsodium instead of the pinned two-file vendor" OFF)
option(ROCKETNET_HARDEN "Enable hardening flags for release builds" ON)
option(ROCKETNET_BUILD_TESTS "Build the native tests and ABI fixtures" ${ROCKETNET_TOP_LEVEL})
option(ROCKETNET_BUILD_EXAMPLES "Build the client/server examples" ${ROCKETNET_TOP_LEVEL})
option(ROCKETNET_BUILD_TOOLS "Build keygen and soak tools" ${ROCKETNET_TOP_LEVEL})
option(ROCKETNET_XDP "Build the Linux XDP transport and embed its BPF program" ON)
option(ROCKETNET_IO_URING "Build the io_uring server transport against liburing when it is found (Linux)" ON)
option(ROCKETNET_SANITIZE_ADDRESS "Build every target with AddressSanitizer" OFF)
option(ROCKETNET_TEST_DEVELOPMENT "Build the unit test library with ROCKETNET_DEVELOPMENT=1, so ./bin/test runs the development paths" OFF)
if(ROCKETNET_SANITIZE_ADDRESS)
    if(MSVC)
        add_compile_options(/fsanitize=address /Zi)
        add_link_options(/DEBUG /INCREMENTAL:NO)
    else()
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
endif()
set(ROCKETNET_TRANSPORT_REGISTRY_HEADER "" CACHE FILEPATH "Additional native transport registry rows (.inc)")
set(ROCKETNET_TRANSPORT_SOURCES "" CACHE STRING "Additional native transport source files")
if(ROCKETNET_TRANSPORT_REGISTRY_HEADER)
    get_filename_component(ROCKETNET_TRANSPORT_REGISTRY_HEADER "${ROCKETNET_TRANSPORT_REGISTRY_HEADER}" ABSOLUTE)
    if(NOT EXISTS "${ROCKETNET_TRANSPORT_REGISTRY_HEADER}")
        message(FATAL_ERROR "Transport registry header does not exist: ${ROCKETNET_TRANSPORT_REGISTRY_HEADER}")
    endif()
endif()
set(CMAKE_TARGET_MESSAGES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(ROCKETNET_TOP_LEVEL)
    if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
    endif()
    if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
    endif()
endif()
if(ROCKETNET_SANITIZE_ADDRESS AND MSVC AND DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    get_filename_component(ROCKETNET_MSVC_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    if(EXISTS "${ROCKETNET_MSVC_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        add_custom_target(rocketnet_asan_runtime ALL
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ROCKETNET_MSVC_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll"
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/clang_rt.asan_dynamic-x86_64.dll"
            VERBATIM)
    endif()
endif()

set(ROCKETNET_HISTORY_PACKETS 4096 CACHE STRING "Per-connection sequence history memory budget (4096..32768, multiple of 64)")
if(NOT ROCKETNET_HISTORY_PACKETS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "ROCKETNET_HISTORY_PACKETS must be an integer")
endif()
math(EXPR ROCKETNET_HISTORY_REMAINDER "${ROCKETNET_HISTORY_PACKETS} % 64")
if(ROCKETNET_HISTORY_PACKETS LESS 4096 OR ROCKETNET_HISTORY_PACKETS GREATER 32768 OR NOT ROCKETNET_HISTORY_REMAINDER EQUAL 0)
    message(FATAL_ERROR "ROCKETNET_HISTORY_PACKETS must be a multiple of 64 between 4096 and 32768")
endif()

set(ROCKETNET_REASSEMBLY_MEMORY_MIB 512 CACHE STRING "Maximum reassembly component allocation in MiB")
set(ROCKETNET_XDP_MEMORY_MIB 4096 CACHE STRING "Maximum XDP queues plus UMEM allocation in MiB")
foreach(budget ROCKETNET_REASSEMBLY_MEMORY_MIB ROCKETNET_XDP_MEMORY_MIB)
    if(NOT ${budget} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${budget} must be an integer")
    endif()
    if(${budget} LESS 1 OR ${budget} GREATER 1048576)
        message(FATAL_ERROR "${budget} must be 1..1048576 MiB")
    endif()
endforeach()

# The tree's own build settings. Directory scope reaches every target this
# directory and its subdirectories create (the libraries, tests, examples and
# tools) and enters no target's INTERFACE, so neither the installed package
# nor a parent project that adds this directory receives them: the strict
# floating-point floor (BUILDING.md, "Floating point") and the memory budgets
# the internal headers read (sdk/rocketnet_history.h:13, sdk/rocketnet_memory.h:7-11).
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-ffp-contract=off)
endif()
# The warning floor, at the same directory scope so every target this tree
# builds carries it, the vendored sodium source included (sodium/NOTES.md):
# a customer compiles these sources inside their game with their own warning
# flags, so the tree ships with no warning at the floor and the floor fails
# the build. ROCKETNET_STRICT_WARNINGS adds the strict set a game is likely
# to use (not yet clean: a known issue in ROADMAP.md); ROCKETNET_WARNINGS_AS_ERRORS=OFF
# keeps the warnings and drops only the failure, for a compiler newer than
# the ones the tree is checked on.
option(ROCKETNET_WARNINGS_AS_ERRORS "Fail the build on any compiler warning" ON)
option(ROCKETNET_STRICT_WARNINGS "Add the strict warning set (conversion, shadow, sign, format, undef)" OFF)
if(MSVC)
    add_compile_options(/W4)
    if(ROCKETNET_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ROCKETNET_STRICT_WARNINGS)
        add_compile_options(-Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion)
    endif()
    if(ROCKETNET_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()
add_compile_definitions(
    ROCKETNET_HISTORY_PACKETS=${ROCKETNET_HISTORY_PACKETS}
    ROCKETNET_REASSEMBLY_MEMORY_MIB=${ROCKETNET_REASSEMBLY_MEMORY_MIB}
    ROCKETNET_XDP_MEMORY_MIB=${ROCKETNET_XDP_MEMORY_MIB})

include(cmake/RocketNetDependencies.cmake)

if(NOT ROCKETNET_USE_SYSTEM_SODIUM)
    set(ROCKETNET_SODIUM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/sodium/sodium.c")
    # the vendor has no configure step, so sodium_memzero (sodium/sodium.c:176)
    # falls to its volatile byte loop unless a HAVE_ macro names the platform
    # wipe; glibc and musl have explicit_bzero, the wipe libsodium's own
    # configure picks there. The byte loop spends ~125 ms on the 56 MB server
    # transport that rocketnet_server_transport_destroy wipes through
    # rocketnet_clear_and_free (sdk/rocketnet.cpp:488).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_property(SOURCE "${ROCKETNET_SODIUM_SOURCE}" APPEND PROPERTY COMPILE_DEFINITIONS HAVE_EXPLICIT_BZERO=1)
    endif()
endif()
function(rocketnet_use_sodium target)
    if(ROCKETNET_USE_SYSTEM_SODIUM)
        target_link_libraries(${target} PRIVATE RocketNet::Sodium)
    else()
        # Only this build owns the privately prefixed RNG state in sodium.c.
        # The lifecycle source must never close an application's system sodium.
        target_compile_definitions(${target} PRIVATE ROCKETNET_BUNDLED_SODIUM=1)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sodium")
    endif()
endfunction()

# server_xdp.o is compiled here, at build time, and embedded in the library as
# bytes (cmake/EmbedXdp.cmake); the server host needs no compiler. Only the
# Linux xdp server transport includes the generated header.
include(cmake/XdpSources.cmake)
set(ROCKETNET_XDP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/rocketnet_server_xdp.h")
set(ROCKETNET_XDP_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/generated/server_xdp.o")
#
# The build is tolerant: ROCKETNET_XDP=ON builds XDP only where a clang with
# the BPF target, libbpf and libxdp are all found (libbpf and libxdp are
# optional here, cmake/RocketNetDependencies.cmake). Short of any of them
# ROCKETNET_XDP turns OFF for this build, exactly as if configured OFF: no
# embedded program, no XDP code in the library, no libbpf or libxdp link, and
# a transport list naming xdp refuses it with TRANSPORT_NOT_BUILT.
set(ROCKETNET_BPF_CLANG "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(ROCKETNET_XDP)
        find_program(ROCKETNET_CLANG clang)
    endif()
    if(ROCKETNET_XDP AND ROCKETNET_CLANG)
        execute_process(COMMAND "${ROCKETNET_CLANG}" -print-targets
            OUTPUT_VARIABLE ROCKETNET_CLANG_TARGETS ERROR_QUIET)
        if(ROCKETNET_CLANG_TARGETS MATCHES "[ \t\n]bpf[ \t]")
            set(ROCKETNET_BPF_CLANG "${ROCKETNET_CLANG}")
        endif()
    endif()
    if(NOT ROCKETNET_XDP)
        message(STATUS "XDP off: ROCKETNET_XDP=OFF; the xdp transport is unavailable; BPF/libxdp are not required")
    elseif(NOT ROCKETNET_BPF_CLANG)
        message(STATUS "XDP off: no clang with the BPF target was found, so XDP is compiled out of this build")
        set(ROCKETNET_XDP OFF)
    elseif(NOT TARGET RocketNet::Bpf OR NOT TARGET RocketNet::Xdp)
        message(STATUS "XDP off: libbpf or libxdp was not found, so XDP is compiled out of this build")
        set(ROCKETNET_XDP OFF)
    endif()
endif()
# One value for every target in the tree: the library, the unit suite, the
# functional suite and the tools agree on whether the XDP transport exists.
add_compile_definitions($<$<PLATFORM_ID:Linux>:ROCKETNET_XDP=$<BOOL:${ROCKETNET_XDP}>>)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ROCKETNET_XDP)
    find_program(ROCKETNET_XXD xxd REQUIRED)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE ROCKETNET_MULTIARCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/xdp-build-settings.txt"
        CONTENT "${ROCKETNET_BPF_CLANG}\n${ROCKETNET_MULTIARCH}\n")
    add_custom_command(OUTPUT "${ROCKETNET_XDP_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/generated"
            "-DXXD=${ROCKETNET_XXD}"
            "-DCLANG=${ROCKETNET_BPF_CLANG}"
            "-DMULTIARCH=${ROCKETNET_MULTIARCH}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedXdp.cmake"
        BYPRODUCTS "${ROCKETNET_XDP_OBJECT}"
        DEPENDS ${ROCKETNET_XDP_SOURCES} cmake/XdpSources.cmake cmake/EmbedXdp.cmake
            "${CMAKE_CURRENT_BINARY_DIR}/generated/xdp-build-settings.txt"
        VERBATIM)
    add_custom_target(rocketnet_xdp_header DEPENDS "${ROCKETNET_XDP_HEADER}")
else()
    add_custom_target(rocketnet_xdp_header)
endif()

set(ROCKETNET_SOURCE
    sdk/rocketnet.cpp
    sdk/rocketnet_abi.cpp
    sdk/rocketnet_api.cpp
    sdk/rocketnet_ack.cpp
    sdk/rocketnet_allocator.cpp
    sdk/rocketnet_window_send.cpp
    sdk/rocketnet_address.cpp
    sdk/rocketnet_base64.cpp
    sdk/rocketnet_client_protocol.cpp
    sdk/rocketnet_client_slots.cpp
    sdk/rocketnet_client_transport.cpp
    sdk/rocketnet_config.cpp
    sdk/rocketnet_connection_reset.cpp
    sdk/rocketnet_driver.cpp
    sdk/rocketnet_driver_builtin.cpp
    sdk/rocketnet_driver_loopback.cpp
    sdk/rocketnet_connect_token.cpp
    sdk/rocketnet_counters.cpp
    sdk/rocketnet_encryption_map.cpp
    sdk/rocketnet_fragment.cpp
    sdk/rocketnet_frame_sync.cpp
    sdk/rocketnet_gateway_netlink_linux.cpp
    sdk/rocketnet_hash.cpp
    sdk/rocketnet_keys.cpp
    sdk/rocketnet_message.cpp
    sdk/rocketnet_message_client.cpp
    sdk/rocketnet_message_server.cpp
    sdk/rocketnet_endpoint.cpp
    sdk/rocketnet_slots.cpp
    sdk/rocketnet_channel.cpp
    sdk/rocketnet_channel_reliable.cpp
    sdk/rocketnet_channel_sequenced.cpp
    sdk/rocketnet_channel_receipts.cpp
    sdk/rocketnet_packet_builder.cpp
    sdk/rocketnet_window_receive.cpp
    sdk/rocketnet_window_ack.cpp
    sdk/rocketnet_network_simulator.cpp
    sdk/rocketnet_platform.cpp
    sdk/rocketnet_protocol.cpp
    sdk/rocketnet_replay.cpp
    sdk/rocketnet_server_replay_marks.cpp
    sdk/rocketnet_server_gso_linux.cpp
    sdk/rocketnet_server_uring_linux.cpp
    sdk/rocketnet_server_iocp_windows.cpp
    sdk/rocketnet_server_rio_windows.cpp
    sdk/rocketnet_server_protocol.cpp
    sdk/rocketnet_server_socket_linux.cpp
    sdk/rocketnet_server_transport.cpp
    sdk/rocketnet_token_tables.cpp
    ${ROCKETNET_TRANSPORT_SOURCES}
)

if(ROCKETNET_HARDEN)
    foreach(language C CXX)
        if(language STREQUAL "C")
            check_c_compiler_flag("-fstack-protector-strong" ROCKETNET_C_STACK_PROTECTOR)
            check_c_compiler_flag("-fcf-protection" ROCKETNET_C_CF_PROTECTION)
        else()
            check_cxx_compiler_flag("-fstack-protector-strong" ROCKETNET_CXX_STACK_PROTECTOR)
            check_cxx_compiler_flag("-fcf-protection" ROCKETNET_CXX_CF_PROTECTION)
        endif()
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        check_linker_flag(CXX "-Wl,-z,relro,-z,now" ROCKETNET_LINK_RELRO_NOW)
    endif()
endif()

# The warning floor is directory-wide (the add_compile_options above the
# dependencies), so it reaches every target, named here or not. The function
# stays as the place a target names the floor it is checked against; it adds
# nothing the directory scope has not already given the target.
function(rocketnet_warning_floor target)
endfunction()

# Explicit STATIC: a parent's BUILD_SHARED_LIBS must not change the target's meaning.
add_library(rocketnet STATIC ${ROCKETNET_SOURCE} ${ROCKETNET_SODIUM_SOURCE})
add_library(rocketnet_shared SHARED ${ROCKETNET_SOURCE} ${ROCKETNET_SODIUM_SOURCE})
# The development library: the same source compiled with ROCKETNET_DEVELOPMENT=1,
# so the dev flow (examples, functional suite, soak, serialize) keeps the
# development token minting and accepts the published development keys that the
# production library refuses. Never installed: it is a build-tree dev aid.
add_library(rocketnet_dev STATIC ${ROCKETNET_SOURCE} ${ROCKETNET_SODIUM_SOURCE})
# The development shared library: the same source with ROCKETNET_DEVELOPMENT=1,
# for native development-mode loopback and ABI checks. Never
# installed; the production shared library must not carry the minting symbol.
add_library(rocketnet_shared_dev SHARED ${ROCKETNET_SOURCE} ${ROCKETNET_SODIUM_SOURCE})
# The test hook library: the same source with the unit suite's seams compiled
# in (the message layer's payload copy count, the public link simulator's
# development switch, and the io_uring transport's fake completion source). Only
# the unit suite links it, so the installed rocketnet library carries no test hook.
set(rocketnet_test_hook_targets)
if(ROCKETNET_BUILD_TESTS)
    add_library(rocketnet_test_hooks STATIC EXCLUDE_FROM_ALL ${ROCKETNET_SOURCE} ${ROCKETNET_SODIUM_SOURCE})
    target_compile_definitions(rocketnet_test_hooks PRIVATE ROCKETNET_ACK_STATS_TEST_HOOKS=1 ROCKETNET_FILTER_EPOCH_TEST_HOOKS=1 ROCKETNET_LINK_SIMULATOR_TEST_HOOKS=1 ROCKETNET_MESSAGE_COPY_TEST_HOOKS=1 ROCKETNET_SECRET_WIPE_TEST_HOOKS=1 ROCKETNET_URING_REAP_TEST_HOOKS=1 ROCKETNET_RECEIVE_TEST_HOOKS=1 ROCKETNET_PORTABLE_TX_TEST_HOOKS=1)
    set(rocketnet_test_hook_targets rocketnet_test_hooks)
endif()
add_library(RocketNet::rocketnet ALIAS rocketnet)
add_library(RocketNet::rocketnet_shared ALIAS rocketnet_shared)

# The send-failure seam drives the send error arms from the functional suite.
# It is compiled into the development libraries only, never a production
# archive or a shared library, so no shipped symbol carries it.
target_compile_definitions(rocketnet_dev PRIVATE ROCKETNET_SEND_ERROR_TEST_HOOKS=1)
target_compile_definitions(rocketnet_shared_dev PRIVATE ROCKETNET_SEND_ERROR_TEST_HOOKS=1)

# The soname follows SDK layout/calling compatibility, independently of wire version.
# the shared library's soname follows the major version
set(ROCKETNET_ABI_SOVERSION ${PROJECT_VERSION_MAJOR})
set_target_properties(rocketnet_shared PROPERTIES
    OUTPUT_NAME rocketnet ARCHIVE_OUTPUT_NAME rocketnet_import
    VERSION ${PROJECT_VERSION} SOVERSION ${ROCKETNET_ABI_SOVERSION}
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
set_target_properties(rocketnet_shared_dev PROPERTIES
    OUTPUT_NAME rocketnet_dev ARCHIVE_OUTPUT_NAME rocketnet_dev_import
    VERSION ${PROJECT_VERSION} SOVERSION ${ROCKETNET_ABI_SOVERSION}
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)

# The effective archive/import library name a target writes: ARCHIVE_OUTPUT_NAME,
# else OUTPUT_NAME, else the target name.
function(rocketnet_effective_archive_name target outvar)
    get_target_property(archive_name ${target} ARCHIVE_OUTPUT_NAME)
    get_target_property(output_name ${target} OUTPUT_NAME)
    if(archive_name AND NOT archive_name STREQUAL "archive_name-NOTFOUND")
        set(${outvar} "${archive_name}" PARENT_SCOPE)
    elseif(output_name AND NOT output_name STREQUAL "output_name-NOTFOUND")
        set(${outvar} "${output_name}" PARENT_SCOPE)
    else()
        set(${outvar} "${target}" PARENT_SCOPE)
    endif()
endfunction()

# A static library and a shared library's import library must not share an
# archive/import name, or whichever writes last wins on Windows and every
# target that links the static library pulls the DLL's import stubs instead.
set(rocketnet_static_targets rocketnet rocketnet_dev)
set(rocketnet_shared_targets rocketnet_shared rocketnet_shared_dev)
foreach(static_target IN LISTS rocketnet_static_targets)
    foreach(shared_target IN LISTS rocketnet_shared_targets)
        rocketnet_effective_archive_name(${static_target} static_archive_name)
        rocketnet_effective_archive_name(${shared_target} shared_archive_name)
        if(static_archive_name STREQUAL shared_archive_name)
            message(FATAL_ERROR
                "static library ${static_target} archive \"${static_archive_name}\" and "
                "shared library ${shared_target} import archive \"${shared_archive_name}\" collide")
        endif()
    endforeach()
endforeach()

target_compile_definitions(rocketnet_shared PUBLIC ROCKETNET_SHARED PRIVATE ROCKETNET_BUILDING_DLL)
target_compile_definitions(rocketnet_shared_dev PUBLIC ROCKETNET_SHARED PRIVATE ROCKETNET_BUILDING_DLL)
foreach(target rocketnet rocketnet_dev rocketnet_shared rocketnet_shared_dev ${rocketnet_test_hook_targets})
    add_dependencies(${target} rocketnet_xdp_header)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(${target} PRIVATE cxx_std_11)
    rocketnet_warning_floor(${target})
    if(target STREQUAL "rocketnet_dev" OR target STREQUAL "rocketnet_shared_dev" OR
       (ROCKETNET_TEST_DEVELOPMENT AND target STREQUAL "rocketnet_test_hooks"))
        target_compile_definitions(${target} PUBLIC ROCKETNET_DEVELOPMENT=1)
    else()
        target_compile_definitions(${target} PRIVATE ROCKETNET_DEVELOPMENT=0)
    endif()
    if(ROCKETNET_TRANSPORT_REGISTRY_HEADER)
        target_compile_definitions(${target} PRIVATE "ROCKETNET_TRANSPORT_REGISTRY_HEADER=\"${ROCKETNET_TRANSPORT_REGISTRY_HEADER}\"")
    endif()
    target_compile_definitions(${target} PRIVATE
        ROCKETNET_HISTORY_PACKETS=${ROCKETNET_HISTORY_PACKETS}
        ROCKETNET_REASSEMBLY_MEMORY_MIB=${ROCKETNET_REASSEMBLY_MEMORY_MIB}
        ROCKETNET_XDP_MEMORY_MIB=${ROCKETNET_XDP_MEMORY_MIB})
    target_include_directories(${target} BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    rocketnet_use_sodium(${target})
    target_link_libraries(${target} PRIVATE Threads::Threads)
    # Family strict floating-point floor for the library's own compile; a
    # consumer's floating-point flags stay its own.
    if(MSVC)
        # Static compiler symbols must not race the shared library linker PDB
        # when both targets use the rocketnet output name in an embedded build.
        set_target_properties(${target} PROPERTIES COMPILE_PDB_NAME "${target}_compile")
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
    if(ROCKETNET_HARDEN)
        target_compile_definitions(${target} PRIVATE "$<$<CONFIG:Release>:_FORTIFY_SOURCE=2>")
        foreach(language C CXX)
            if(ROCKETNET_${language}_STACK_PROTECTOR)
                target_compile_options(${target} PRIVATE
                    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:${language}>>:-fstack-protector-strong>")
            endif()
            if(ROCKETNET_${language}_CF_PROTECTION)
                target_compile_options(${target} PRIVATE
                    "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:${language}>>:-fcf-protection>")
            endif()
        endforeach()
    endif()
    if(APPLE)
        target_link_libraries(${target} PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${target} PRIVATE ROCKETNET_XDP=$<BOOL:${ROCKETNET_XDP}>)
        if(ROCKETNET_XDP)
            target_link_libraries(${target} PRIVATE RocketNet::Bpf RocketNet::Xdp)
        endif()
        if(TARGET RocketNet::Uring)
            target_link_libraries(${target} PRIVATE RocketNet::Uring)
            target_compile_definitions(${target} PRIVATE ROCKETNET_IO_URING=1)
        endif()
    endif()
endforeach()
if(ROCKETNET_HARDEN AND ROCKETNET_LINK_RELRO_NOW)
    target_link_options(rocketnet_shared PRIVATE "$<$<CONFIG:Release>:-Wl,-z,relro,-z,now>")
endif()

# The public header at the strictest level each compiler has, in the default
# build: a translation unit holding only rocketnet.h, as C and as C++
# (tools/header_warnings). Every game that includes the header compiles it
# with its own flags, so the header must be clean under all of them. Angle
# bracket includes are the compiler's own headers, never ours, so MSVC reads
# them at /external:W0.
add_library(rocketnet_header_warnings OBJECT
    tools/header_warnings/header_warnings.c
    tools/header_warnings/header_warnings.cpp)
target_include_directories(rocketnet_header_warnings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # C4820 (padding added) is MSVC's -Wpadded: layout, not a defect.
    target_compile_options(rocketnet_header_warnings PRIVATE /Wall /wd4820 /external:anglebrackets /external:W0)
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(rocketnet_header_warnings PRIVATE -Weverything -Wno-padded)
    check_c_compiler_flag("-Wno-poison-system-directories" ROCKETNET_C_NO_POISON_SYSTEM_DIRECTORIES)
    if(ROCKETNET_C_NO_POISON_SYSTEM_DIRECTORIES)
        target_compile_options(rocketnet_header_warnings PRIVATE -Wno-poison-system-directories)
    endif()
else()
    target_compile_options(rocketnet_header_warnings PRIVATE
        -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wformat=2 -Wundef -Wdouble-promotion)
endif()

if(ROCKETNET_BUILD_EXAMPLES)
    add_executable(client client.c)
    target_link_libraries(client PRIVATE rocketnet_dev)
    add_executable(server server.c)
    target_link_libraries(server PRIVATE rocketnet_dev)
    add_executable(happy_server tools/happy_path/happy_server.c)
    target_link_libraries(happy_server PRIVATE rocketnet_dev)
    add_executable(happy_client tools/happy_path/happy_client.c)
    target_link_libraries(happy_client PRIVATE rocketnet_dev)
    add_subdirectory(examples)
endif()
if(ROCKETNET_BUILD_TESTS)
    # oss-fuzz/build.sh is the one build path that does not go through this
    # file, so the -ffp-contract=off floor has to be pinned there separately:
    # this holds every $CC/$CXX compile line in it to the flag. A build target,
    # not a ctest: the unit binary's target name "test" is reserved once CTest
    # is enabled. ci.yml runs the same script beside tools/check_fp_contract.sh.
    add_custom_target(oss_fuzz_fp_contract
        COMMAND ${CMAKE_COMMAND}
            -DROCKETNET_OSS_FUZZ_BUILD_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/oss-fuzz/build.sh
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_oss_fuzz_fp_contract.cmake
        VERBATIM)
    # A real C translation unit must link the internal token helpers used by
    # client.c and soak.c against their C++ implementation.
    add_executable(test_internal_c tools/internal_c/main.c)
    set_target_properties(test_internal_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_link_libraries(test_internal_c PRIVATE rocketnet_dev)
    rocketnet_warning_floor(test_internal_c)
    add_executable(test_sodium test_sodium.c)
    target_link_libraries(test_sodium PRIVATE rocketnet)
    rocketnet_use_sodium(test_sodium)
    rocketnet_warning_floor(test_sodium)
    # Isolated parity oracle only: never installed or linked into the SDK.
    add_library(rocketnet_sodium_oracle SHARED EXCLUDE_FROM_ALL sodium/sodium.c)
    set_target_properties(rocketnet_sodium_oracle PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_library(rocketnet_test_support STATIC sdk/rocketnet_tests.cpp)
    target_link_libraries(rocketnet_test_support PUBLIC rocketnet_test_hooks)
    rocketnet_use_sodium(rocketnet_test_support)
    rocketnet_warning_floor(rocketnet_sodium_oracle)
    rocketnet_warning_floor(rocketnet_test_support)
    # The message layer's payload copy count (test_unit_unreliable_in_place.cpp)
    # exists only for the unit suite: the test binary sees the declaration, and
    # the definition lives in rocketnet_test_hooks, never in rocketnet.
    # Every test_unit_<topic>.cpp at the repository root registers with
    # REGISTER_UNIT_TEST from test_unit_registry.h and is built into ./bin/test.
    file( GLOB ROCKETNET_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_unit_*.cpp" )
    # test_unit_driver_loopback.cpp compiles sdk/rocketnet_server_transport.cpp
    # into the suite, so its object goes first on the link line. lld-link loads
    # an archive member as soon as an earlier object references one of its
    # symbols, and the library's copy of the transport would then define every
    # transport symbol a second time; link.exe searches archives only after
    # every object, so it links in either order.
    set(ROCKETNET_UNIT_TRANSPORT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/test_unit_driver_loopback.cpp")
    list(REMOVE_ITEM ROCKETNET_UNIT_TEST_SOURCES "${ROCKETNET_UNIT_TRANSPORT_SOURCE}")
    # Included by the loopback translation unit, which compiles the server
    # transport, so the transport-result cases can swap that transport's
    # receive_batch. Not a second translation unit.
    set(ROCKETNET_UNIT_TRANSPORT_RESULTS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/test_unit_driver_results_checked.cpp")
    list(REMOVE_ITEM ROCKETNET_UNIT_TEST_SOURCES "${ROCKETNET_UNIT_TRANSPORT_RESULTS_SOURCE}")
    add_executable(test ${ROCKETNET_UNIT_TRANSPORT_SOURCE} test.cpp test_channels.cpp ${ROCKETNET_UNIT_TEST_SOURCES})
    target_compile_definitions(rocketnet_test_hooks PRIVATE ROCKETNET_ACK_COST_TEST_HOOKS=1 ROCKETNET_LOOPBACK_TEST_HOOKS=1)
    target_compile_definitions(test PRIVATE ROCKETNET_ACK_COST_TEST_HOOKS=1 ROCKETNET_LOOPBACK_TEST_HOOKS=1)
    target_compile_definitions(test PRIVATE ROCKETNET_ACK_STATS_TEST_HOOKS=1 ROCKETNET_FILTER_EPOCH_TEST_HOOKS=1 ROCKETNET_XDP_STAGING_TEST_HOOKS=1 ROCKETNET_LINK_SIMULATOR_TEST_HOOKS=1 ROCKETNET_MESSAGE_COPY_TEST_HOOKS=1 ROCKETNET_SECRET_WIPE_TEST_HOOKS=1 ROCKETNET_URING_REAP_TEST_HOOKS=1 ROCKETNET_RECEIVE_TEST_HOOKS=1)
    if(TARGET RocketNet::Uring)
        # the io_uring seam cases are built only where the driver is
        target_compile_definitions(test PRIVATE ROCKETNET_IO_URING=1)
    endif()
    if(WIN32)
        # The RIO overflow seam (test_unit_rio_overflow_seam.cpp) compiles into
        # the test hook library only, matching the link-simulator seam above, so
        # neither the installed static library nor the shared libraries carry it.
        target_compile_definitions(rocketnet_test_hooks PRIVATE ROCKETNET_RIO_OVERFLOW_TEST_HOOKS=1)
        target_compile_definitions(test PRIVATE ROCKETNET_RIO_OVERFLOW_TEST_HOOKS=1)
    endif()
    # The counter audit (test_unit_counter_classes.cpp) reads source files: the
    # tests that must name every counter, and the sdk files that count them.
    # Both lists reach it as compile definitions, relative to the source
    # directory, so the audit never walks a directory itself.
    file( GLOB ROCKETNET_COUNTER_TEST_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/functional/*.cpp" )
    list( APPEND ROCKETNET_COUNTER_TEST_SOURCES "sdk/rocketnet_tests.cpp" )
    file( GLOB ROCKETNET_COUNTER_SDK_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/*.inc" )
    list( REMOVE_ITEM ROCKETNET_COUNTER_SDK_SOURCES "sdk/rocketnet_tests.cpp" "sdk/rocketnet_tests.h"
        "sdk/rocketnet_counters.h" "sdk/rocketnet_counters.cpp" )
    string( REPLACE ";" "|" ROCKETNET_COUNTER_TEST_SOURCES_LIST "${ROCKETNET_COUNTER_TEST_SOURCES}" )
    string( REPLACE ";" "|" ROCKETNET_COUNTER_SDK_SOURCES_LIST "${ROCKETNET_COUNTER_SDK_SOURCES}" )
    set_property( SOURCE test_unit_counter_classes.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "ROCKETNET_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        "ROCKETNET_COUNTER_TEST_SOURCES=\"${ROCKETNET_COUNTER_TEST_SOURCES_LIST}\""
        "ROCKETNET_COUNTER_SDK_SOURCES=\"${ROCKETNET_COUNTER_SDK_SOURCES_LIST}\"" )
    # The message-vs-packet source scan (test_unit_client_server_say_messages.cpp)
    # reads sdk client/server files through ROCKETNET_SOURCE_DIR, the same way
    # the counter audit does.
    set_property( SOURCE test_unit_client_server_say_messages.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "ROCKETNET_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"" )
    # The env names audit (test_unit_config_env_names.cpp) reads sdk/rocketnet.h
    # through ROCKETNET_SOURCE_DIR for the variable names it documents.
    set_property( SOURCE test_unit_config_env_names.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "ROCKETNET_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"" )
    target_link_libraries(test PRIVATE rocketnet_test_support Threads::Threads)
    target_compile_features(test PRIVATE cxx_std_11)
    rocketnet_warning_floor(test)
    # The promotion case exercises the WIDE channel header (an index of 256 or
    # more) on channel 300, past the default build's cap, so this binary
    # compiles the SDK once more with ROCKETNET_MAX_CHANNELS=512 for every file
    # (the promotion rule: oversize unreliable sends promote to their reliable twin).
    add_executable(test_wide
        test.cpp test_channels.cpp test_unit_unreliable_promotion.cpp test_unit_channel_violation_counters.cpp
        sdk/rocketnet_tests.cpp ${ROCKETNET_SOURCE} ${ROCKETNET_SODIUM_SOURCE})
    add_dependencies(test_wide rocketnet_xdp_header)
    target_compile_definitions(test_wide PRIVATE ROCKETNET_MAX_CHANNELS=512
        ROCKETNET_LINK_SIMULATOR_TEST_HOOKS=1
        ROCKETNET_STATS_MANUAL_TEST_HOOKS=1
        $<TARGET_PROPERTY:rocketnet,COMPILE_DEFINITIONS>)
    target_include_directories(test_wide PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_features(test_wide PRIVATE cxx_std_11)
    target_compile_options(test_wide PRIVATE $<TARGET_PROPERTY:rocketnet,COMPILE_OPTIONS>)
    rocketnet_use_sodium(test_wide)
    rocketnet_warning_floor(test_wide)
    target_link_libraries(test_wide PRIVATE Threads::Threads)
    # The SDK sources link their platform libraries, as the library targets do.
    if(APPLE)
        target_link_libraries(test_wide PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(ROCKETNET_XDP)
            target_link_libraries(test_wide PRIVATE RocketNet::Bpf RocketNet::Xdp)
        endif()
        if(TARGET RocketNet::Uring)
            target_link_libraries(test_wide PRIVATE RocketNet::Uring)
            target_compile_definitions(test_wide PRIVATE ROCKETNET_IO_URING=1)
        endif()
    endif()
    # The endpoint's clock intercept (test_unit_endpoint.cpp): on Linux the
    # linker sends every call to the library's clock, rocketnet_platform_time,
    # from any object file of the test binary, to the test's counting wrapper.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(test PRIVATE "-Wl,--wrap=rocketnet_platform_time")
        set_property( SOURCE test_unit_endpoint.cpp APPEND PROPERTY COMPILE_DEFINITIONS ROCKETNET_TEST_CLOCK_WRAP=1 )
    endif()
    if(ROCKETNET_USE_SYSTEM_SODIUM)
        target_compile_definitions(test PRIVATE ROCKETNET_USE_SYSTEM_SODIUM=1)
        rocketnet_use_sodium(test)
    endif()
    # test_unit_driver_loopback.cpp compiles sdk/rocketnet_server_transport.cpp,
    # which includes <sodium.h>: the vendored header, as the transport's own
    # targets see it (a host with no system libsodium, such as Windows, has no other)
    if(NOT ROCKETNET_USE_SYSTEM_SODIUM)
        set_property( SOURCE test_unit_driver_loopback.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/sodium" )
    endif()
    # The XDP filter as host C (test/xdp_host/): server_xdp/server_xdp.c,
    # compiled unchanged with the BPF build's defines behind a shadow include
    # directory of minimal kernel and libbpf headers, so the unit suite judges
    # its decisions with no kernel, no root and no BPF syscall
    # (test_unit_xdp_filter_host.cpp). The shim directory is on this one C
    # target's include path only. The filter is GNU C and the shim models a
    # little-endian host, so the target is built for GCC or Clang on a
    # little-endian, non-Windows host; elsewhere the cases print SKIPPED.
    # ROCKETNET_XDP_FILTER_HOST_SOURCE names the filter source to compile; the
    # default is the shipped file, and another value is for reproducing a
    # historical filter.
    set(ROCKETNET_XDP_FILTER_HOST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/server_xdp/server_xdp.c" CACHE FILEPATH
        "XDP filter source the host-C unit target compiles")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_C_BYTE_ORDER STREQUAL "LITTLE_ENDIAN" AND NOT WIN32)
        get_filename_component(ROCKETNET_XDP_FILTER_HOST_SOURCE_PATH "${ROCKETNET_XDP_FILTER_HOST_SOURCE}" ABSOLUTE)
        add_library(rocketnet_xdp_filter_host STATIC test/xdp_host/server_xdp_host.c)
        set_source_files_properties(test/xdp_host/server_xdp_host.c PROPERTIES
            OBJECT_DEPENDS "${ROCKETNET_XDP_FILTER_HOST_SOURCE_PATH}")
        target_include_directories(rocketnet_xdp_filter_host BEFORE PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/test/xdp_host" "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
        target_compile_definitions(rocketnet_xdp_filter_host PRIVATE __linux__ __BPF__
            "ROCKETNET_XDP_FILTER_SOURCE=\"${ROCKETNET_XDP_FILTER_HOST_SOURCE_PATH}\"")
        target_compile_options(rocketnet_xdp_filter_host PRIVATE -ffreestanding -ffp-contract=off)
        rocketnet_warning_floor(rocketnet_xdp_filter_host)
        target_link_libraries(test PRIVATE rocketnet_xdp_filter_host)
        target_compile_definitions(test PRIVATE ROCKETNET_XDP_FILTER_HOST=1)
        # test_xdp_transport_host_every_counter_has_a_case reads sdk/rocketnet_wire.h and its
        # own source for the counters and the cases that name them.
        set_property( SOURCE test_unit_xdp_filter_host.cpp APPEND PROPERTY COMPILE_DEFINITIONS
            "ROCKETNET_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"" )
    else()
        message(STATUS "XDP filter host-C target not built: needs GCC or Clang on a little-endian, non-Windows host")
    endif()
    # Two probes load a shared library at run time, so the test target must
    # build both: CI legs build `test` alone, and without this the library is
    # missing and the probe goes red. test_dev_token_minting_absent_in_release
    # probes the release library; the connect-token vectors load the dev
    # library's token writer through test_dev_connect_token_timeout_is_the_named_constant.
    add_dependencies(test rocketnet_shared rocketnet_shared_dev)
    add_dependencies(test_wide rocketnet_shared)
    # Probe the exact shared targets built for this configuration, so the unit
    # suite passes when the output directory or working directory changes.
    # sdk/rocketnet_tests.cpp is compiled by these two targets, not by test.
    target_compile_definitions(rocketnet_test_support PRIVATE
        "ROCKETNET_RELEASE_LIBRARY_PATH=\"$<TARGET_FILE:rocketnet_shared>\"")
    target_compile_definitions(test_wide PRIVATE
        "ROCKETNET_RELEASE_LIBRARY_PATH=\"$<TARGET_FILE:rocketnet_shared>\"")
    target_compile_definitions(test PRIVATE
        "ROCKETNET_DEV_LIBRARY_PATH=\"$<TARGET_FILE:rocketnet_shared_dev>\"")
    # Keep the shared target directory in the executable's RUNPATH for other
    # runtime loads that use a bare soname.
    set_property(TARGET test APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:rocketnet_shared>")
    # Functional-tier binaries (BUILDING.md, "Test binaries and their tiers"):
    # test_lifecycle, test_transport, test_serialize, test_transport_control,
    # test_xdp, test_xdp_filter, test_msgx and test_iocp open real sockets,
    # start threads, sleep or read the wall clock, so none of them is a unit
    # test. ./bin/functional runs test_serialize, test_transport_control and
    # the platform's transport binary (the execs table in functional/main.cpp's
    # main); CI runs test_lifecycle and test_transport in steps named
    # "Functional: ...".
    foreach(test_target test_lifecycle)
        add_executable(${test_target} ${test_target}.cpp)
        target_include_directories(${test_target} PRIVATE test)
        target_link_libraries(${test_target} PRIVATE rocketnet)
        rocketnet_warning_floor(${test_target})
    endforeach()
    add_executable(test_transport test_driver.cpp)
    target_include_directories(test_transport PRIVATE test)
    target_link_libraries(test_transport PRIVATE rocketnet)
    rocketnet_warning_floor(test_transport)
    add_executable(test_serialize test_serialize.cpp)
    target_include_directories(test_serialize PRIVATE test)
    target_link_libraries(test_serialize PRIVATE rocketnet_dev)
    rocketnet_warning_floor(test_serialize)
    # Compiles the server transport itself and runs on development keys, so it needs the development library.
    add_executable(test_transport_control test_transport_control.cpp)
    target_include_directories(test_transport_control PRIVATE test)
    target_link_libraries(test_transport_control PRIVATE rocketnet_dev)
    rocketnet_use_sodium(test_transport_control)
    rocketnet_warning_floor(test_transport_control)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ROCKETNET_XDP)
        add_executable(test_xdp test_xdp.cpp sdk/rocketnet_server_socket_linux.cpp sdk/rocketnet_gateway_netlink_linux.cpp
            sdk/rocketnet.cpp sdk/rocketnet_platform.cpp sdk/rocketnet_address.cpp sdk/rocketnet_hash.cpp ${ROCKETNET_SODIUM_SOURCE})
        add_dependencies(test_xdp rocketnet_xdp_header)
        target_compile_definitions(test_xdp PRIVATE ROCKETNET_XDP_TESTS
            $<TARGET_PROPERTY:rocketnet,COMPILE_DEFINITIONS>)
        target_include_directories(test_xdp PRIVATE sdk "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_features(test_xdp PRIVATE cxx_std_11)
        target_compile_options(test_xdp PRIVATE $<TARGET_PROPERTY:rocketnet,COMPILE_OPTIONS>)
        rocketnet_use_sodium(test_xdp)
        target_link_libraries(test_xdp PRIVATE RocketNet::Bpf RocketNet::Xdp Threads::Threads)
        rocketnet_warning_floor(test_xdp)
        # test_xdp_filter loads server_xdp.o, the object the library embeds
        # (cmake/EmbedXdp.cmake), and runs crafted frames through it with
        # BPF_PROG_TEST_RUN (root or CAP_BPF + CAP_NET_ADMIN to run).
        if(ROCKETNET_BPF_CLANG)
            set(ROCKETNET_XDP_FILTER_OBJECT "${ROCKETNET_XDP_OBJECT}")
            add_executable(test_xdp_filter test_xdp_filter.cpp)
            add_dependencies(test_xdp_filter rocketnet_xdp_header)
            # xdp_on_link_reply_reaches_the_peer runs bin/server and bin/client
            if(TARGET server AND TARGET client)
                add_dependencies(test_xdp_filter server client)
            endif()
            target_compile_definitions(test_xdp_filter PRIVATE
                "ROCKETNET_XDP_FILTER_OBJECT=\"${ROCKETNET_XDP_FILTER_OBJECT}\"")
            target_include_directories(test_xdp_filter PRIVATE sdk)
            target_compile_features(test_xdp_filter PRIVATE cxx_std_11)
            target_link_libraries(test_xdp_filter PRIVATE RocketNet::Bpf)
            rocketnet_warning_floor(test_xdp_filter)
        else()
            # The configure message above explains why the object is absent.
        endif()
    endif()
    if(APPLE)
        add_executable(test_msgx test_msgx.cpp)
        target_link_libraries(test_msgx PRIVATE rocketnet)
        rocketnet_warning_floor(test_msgx)
    endif()
    if(WIN32)
        # Isolate the test-instrumented transports: never link another copy from
        # the production archive. Only their ordinary runtime dependencies
        # enter. The IOCP and RIO fault seams (ROCKETNET_IOCP_TESTS,
        # ROCKETNET_RIO_TESTS) compile only here, never into a library.
        add_executable(test_iocp test_iocp.cpp sdk/rocketnet_server_iocp_windows.cpp sdk/rocketnet_server_rio_windows.cpp
            sdk/rocketnet.cpp sdk/rocketnet_platform.cpp sdk/rocketnet_address.cpp sdk/rocketnet_hash.cpp ${ROCKETNET_SODIUM_SOURCE})
        target_compile_definitions(test_iocp PRIVATE ROCKETNET_IOCP_TESTS ROCKETNET_RIO_TESTS
            $<TARGET_PROPERTY:rocketnet,COMPILE_DEFINITIONS>)
        target_include_directories(test_iocp PRIVATE sdk)
        target_compile_features(test_iocp PRIVATE cxx_std_11)
        target_compile_options(test_iocp PRIVATE $<TARGET_PROPERTY:rocketnet,COMPILE_OPTIONS>)
        rocketnet_use_sodium(test_iocp)
        target_link_libraries(test_iocp PRIVATE Threads::Threads)
        rocketnet_warning_floor(test_iocp)
    endif()
    file( GLOB ROCKETNET_CHANNEL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/functional/channel_*.cpp" )
    add_executable(functional
        functional/main.cpp
        functional/rocketnet_functional_tests.cpp
        functional/platform_primitives.cpp
        functional/server_receive_backlog.cpp
        functional/lifecycle_driver.cpp
        functional/cpp_transports.cpp
        functional/live_matrix.cpp
        functional/windows_socket_errors.cpp
        functional/windows_connreset.cpp
        functional/posix_hard_killed_peer.cpp
        functional/posix_lifecycle_stress.cpp
        functional/posix_socket_errors.cpp
        ${ROCKETNET_CHANNEL_TEST_SOURCES})
    target_link_libraries(functional PRIVATE rocketnet_dev Threads::Threads)
    rocketnet_warning_floor(functional)
    target_compile_definitions(functional PRIVATE ROCKETNET_SEND_ERROR_TEST_HOOKS=1)
    target_compile_features(functional PRIVATE cxx_std_11)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_dependencies(functional test_serialize test_transport_control)
        if(ROCKETNET_XDP)
            add_dependencies(functional test_xdp)
        endif()
    elseif(APPLE)
        add_dependencies(functional test_msgx test_serialize test_transport_control)
    elseif(WIN32)
        add_dependencies(functional test_iocp test_serialize test_transport_control)
    else()
        add_dependencies(functional test_serialize test_transport_control)
    endif()
endif()
if(ROCKETNET_BUILD_TOOLS)
    add_executable(soak soak.c)
    add_executable(load load.c)
    add_executable(soak_sim soak_sim.cpp)
    # The deterministic simulation harness (docs/DST.md): one seed drives N
    # clients and a server over the in-memory wire, no socket and no clock.
    add_executable(dst dst.cpp)
    target_link_libraries(dst PRIVATE rocketnet_dev)
    rocketnet_use_sodium(dst)
    target_compile_features(dst PRIVATE cxx_std_11)
    add_executable(keygen keygen.cpp)
    add_executable(transport_memory bench/transport_memory.cpp)
    add_executable(transport_fill bench/transport_fill.cpp)
    add_executable(client_main_thread_cost bench/client_main_thread_cost.cpp)
    add_executable(instruction_count bench/instruction_count.cpp)
    add_executable(lease_cost bench/lease_cost.cpp)
    target_link_libraries(keygen PRIVATE rocketnet)
    foreach(tool soak load soak_sim transport_memory transport_fill client_main_thread_cost instruction_count lease_cost)
        target_link_libraries(${tool} PRIVATE rocketnet_dev)
    endforeach()
    target_compile_features(instruction_count PRIVATE cxx_std_11)
    if(CMAKE_BUILD_TYPE)
        target_compile_definitions(instruction_count PRIVATE "ROCKETNET_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
    endif()
    rocketnet_use_sodium(instruction_count)
    add_custom_command(TARGET instruction_count POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:instruction_count>"
            "${CMAKE_CURRENT_BINARY_DIR}/instruction_count")
    target_link_libraries(lease_cost PRIVATE Threads::Threads)
    rocketnet_use_sodium(soak_sim)
    rocketnet_use_sodium(keygen)
    # The corpus replay transports: fuzz/replay_main.cpp provides the main the
    # libFuzzer runtime would provide, reading each named file whole and
    # calling the harness's LLVMFuzzerTestOneInput once, so the committed
    # corpora replay on platforms with no libFuzzer runtime. Built against
    # the development library with the same includes as the NDEBUG fuzz
    # build in ci.yml.
    #
    # The pair harnesses (fuzz/rocketnet_fuzz_pair.h: the server and client
    # transports' real receive loops and the scripted session over the
    # loopback transport) compile sdk/rocketnet_server_transport.cpp and
    # sdk/rocketnet_driver_loopback.cpp into the harness, as the unit suite's
    # loopback case does, so the harness object goes first on the link line:
    # lld-link loads an archive member as soon as an earlier object
    # references one of its symbols (see ROCKETNET_UNIT_TRANSPORT_SOURCE).
    set(ROCKETNET_FUZZ_PAIR_HARNESSES rocketnet_fuzz_server_transport rocketnet_fuzz_client_transport rocketnet_fuzz_session)
    foreach(harness rocketnet_fuzz rocketnet_fuzz_message rocketnet_fuzz_connect_token rocketnet_fuzz_receive_structured rocketnet_fuzz_token_private rocketnet_fuzz_round_trip rocketnet_fuzz_config ${ROCKETNET_FUZZ_PAIR_HARNESSES})
        if(harness IN_LIST ROCKETNET_FUZZ_PAIR_HARNESSES)
            add_executable(fuzz_replay_${harness} fuzz/${harness}.cpp fuzz/replay_main.cpp)
        else()
            add_executable(fuzz_replay_${harness} fuzz/replay_main.cpp fuzz/${harness}.cpp)
        endif()
        target_compile_definitions(fuzz_replay_${harness} PRIVATE ROCKETNET_DEVELOPMENT=1)
        target_compile_features(fuzz_replay_${harness} PRIVATE cxx_std_17)
        target_include_directories(fuzz_replay_${harness} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/sodium"
            "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
            "${CMAKE_CURRENT_SOURCE_DIR}/test")
        rocketnet_use_sodium(fuzz_replay_${harness})
        target_link_libraries(fuzz_replay_${harness} PRIVATE rocketnet_dev Threads::Threads)
    endforeach()
endif()

install(TARGETS rocketnet rocketnet_shared EXPORT RocketNetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# rocketnet.h is the whole public C API.
install(FILES sdk/rocketnet.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/rocketnet)
install(FILES sodium/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/rocketnet/sodium)
set(ROCKETNET_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/RocketNet")
if(TARGET RocketNet::Uring)
    set(ROCKETNET_IO_URING_BUILT ON)
else()
    set(ROCKETNET_IO_URING_BUILT OFF)
endif()
configure_package_config_file(cmake/RocketNetConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfig.cmake"
    INSTALL_DESTINATION ${ROCKETNET_PACKAGE_DIR})
# find_package compares numbers, so the package version is 1.0.0; the label
# v1.0.0-preview-1 rides beside it as RocketNet_VERSION_STRING and
# RocketNet_VERSION_PRERELEASE (cmake/RocketNetConfig.cmake.in).
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(EXPORT RocketNetTargets NAMESPACE RocketNet:: DESTINATION ${ROCKETNET_PACKAGE_DIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfigVersion.cmake"
    cmake/RocketNetDependencies.cmake DESTINATION ${ROCKETNET_PACKAGE_DIR})

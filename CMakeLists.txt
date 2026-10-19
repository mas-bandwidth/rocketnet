cmake_minimum_required(VERSION 3.20)
project(rocketnet VERSION 1.0.0 LANGUAGES C CXX)
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
option(ROCKETNET_IO_URING "Build the io_uring server driver against liburing when it is found (Linux)" ON)
set(ROCKETNET_DRIVER_REGISTRY_HEADER "" CACHE FILEPATH "Additional native driver registry rows (.inc)")
set(ROCKETNET_DRIVER_SOURCES "" CACHE STRING "Additional native driver source files")
if(ROCKETNET_DRIVER_REGISTRY_HEADER)
    get_filename_component(ROCKETNET_DRIVER_REGISTRY_HEADER "${ROCKETNET_DRIVER_REGISTRY_HEADER}" ABSOLUTE)
    if(NOT EXISTS "${ROCKETNET_DRIVER_REGISTRY_HEADER}")
        message(FATAL_ERROR "Driver registry header does not exist: ${ROCKETNET_DRIVER_REGISTRY_HEADER}")
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


include(cmake/RocketNetDependencies.cmake)

if(NOT ROCKETNET_USE_SYSTEM_SODIUM)
    set(ROCKETNET_SODIUM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/sodium/sodium.c")
endif()
function(rocketnet_use_sodium target)
    if(ROCKETNET_USE_SYSTEM_SODIUM)
        target_link_libraries(${target} PRIVATE RocketNet::Sodium)
    else()
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sodium")
    endif()
endfunction()

# Generate into this build tree. Multiple builds never rewrite the source header.
include(cmake/XdpSources.cmake)
set(ROCKETNET_XDP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/rocketnet_server_xdp.h")
if(WIN32)
    configure_file(sdk/rocketnet_server_xdp.h "${ROCKETNET_XDP_HEADER}" COPYONLY)
else()
    find_program(ROCKETNET_XXD xxd REQUIRED)
    add_custom_command(OUTPUT "${ROCKETNET_XDP_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/generated"
            "-DXXD=${ROCKETNET_XXD}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedXdp.cmake"
        DEPENDS ${ROCKETNET_XDP_SOURCES} cmake/XdpSources.cmake cmake/EmbedXdp.cmake
        VERBATIM)
endif()
add_custom_target(rocketnet_xdp_header DEPENDS "${ROCKETNET_XDP_HEADER}")

set(ROCKETNET_SOURCE
    sdk/rocketnet.cpp
    sdk/rocketnet_abi.cpp
    sdk/rocketnet_ack.cpp
    sdk/rocketnet_allocator.cpp
    sdk/rocketnet_window_send.cpp
    sdk/rocketnet_address.cpp
    sdk/rocketnet_base64.cpp
    sdk/rocketnet_client_protocol.cpp
    sdk/rocketnet_client_slots.cpp
    sdk/rocketnet_client_transport.cpp
    sdk/rocketnet_config.cpp
    sdk/rocketnet_driver.cpp
    sdk/rocketnet_driver_builtin.cpp
    sdk/rocketnet_driver_loopback.cpp
    sdk/rocketnet_connect_token.cpp
    sdk/rocketnet_counters.cpp
    sdk/rocketnet_encryption_map.cpp
    sdk/rocketnet_fragment.cpp
    sdk/rocketnet_frame_sync.cpp
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
    ${ROCKETNET_DRIVER_SOURCES}
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
add_library(RocketNet::rocketnet ALIAS rocketnet)
add_library(RocketNet::rocketnet_shared ALIAS rocketnet_shared)

# The soname follows SDK layout/calling compatibility, independently of wire version.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sdk/rocketnet.h" ROCKETNET_ABI_LINE REGEX "^#define ROCKETNET_ABI_VERSION [0-9]+$")
string(REGEX REPLACE ".* ([0-9]+)$" "\\1" ROCKETNET_ABI_SOVERSION "${ROCKETNET_ABI_LINE}")
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

target_compile_definitions(rocketnet_shared PUBLIC ROCKETNET_SHARED PRIVATE ROCKETNET_EXPORT)
target_compile_definitions(rocketnet_shared_dev PUBLIC ROCKETNET_SHARED PRIVATE ROCKETNET_EXPORT)
foreach(target rocketnet rocketnet_dev rocketnet_shared rocketnet_shared_dev)
    add_dependencies(${target} rocketnet_xdp_header)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(${target} PRIVATE cxx_std_11)
    if(target STREQUAL "rocketnet_dev" OR target STREQUAL "rocketnet_shared_dev")
        target_compile_definitions(${target} PUBLIC ROCKETNET_DEVELOPMENT=1)
    else()
        target_compile_definitions(${target} PRIVATE ROCKETNET_DEVELOPMENT=0)
    endif()
    if(ROCKETNET_DRIVER_REGISTRY_HEADER)
        target_compile_definitions(${target} PRIVATE "ROCKETNET_DRIVER_REGISTRY_HEADER=\"${ROCKETNET_DRIVER_REGISTRY_HEADER}\"")
    endif()
    target_compile_definitions(${target} PUBLIC
        ROCKETNET_HISTORY_PACKETS=${ROCKETNET_HISTORY_PACKETS}
        ROCKETNET_REASSEMBLY_MEMORY_MIB=${ROCKETNET_REASSEMBLY_MEMORY_MIB}
        ROCKETNET_XDP_MEMORY_MIB=${ROCKETNET_XDP_MEMORY_MIB})
    target_include_directories(${target} BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sdk>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    rocketnet_use_sodium(${target})
    target_link_libraries(${target} PRIVATE Threads::Threads)
    # Family strict floating-point floor, including callers compiling SDK inline code.
    if(MSVC)
        # Static compiler symbols must not race the shared library linker PDB
        # when both targets use the rocketnet output name in an embedded build.
        set_target_properties(${target} PROPERTIES COMPILE_PDB_NAME "${target}_compile")
        target_compile_options(${target} PUBLIC /fp:precise)
    else()
        target_compile_options(${target} PUBLIC -ffp-contract=off)
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
        target_link_libraries(${target} PRIVATE RocketNet::Bpf RocketNet::Xdp)
        if(TARGET RocketNet::Uring)
            target_link_libraries(${target} PRIVATE RocketNet::Uring)
            target_compile_definitions(${target} PRIVATE ROCKETNET_IO_URING=1)
        endif()
    endif()
endforeach()
if(ROCKETNET_HARDEN AND ROCKETNET_LINK_RELRO_NOW)
    target_link_options(rocketnet_shared PRIVATE "$<$<CONFIG:Release>:-Wl,-z,relro,-z,now>")
endif()

if(ROCKETNET_BUILD_EXAMPLES)
    add_executable(client client.c)
    target_link_libraries(client PRIVATE rocketnet_dev)
    add_executable(server server.c)
    target_link_libraries(server PRIVATE rocketnet_dev)
endif()
if(ROCKETNET_BUILD_TESTS)
    add_executable(test_sodium test_sodium.c)
    target_link_libraries(test_sodium PRIVATE rocketnet)
    rocketnet_use_sodium(test_sodium)
    # Isolated parity oracle only: never installed or linked into the SDK.
    add_library(rocketnet_sodium_oracle SHARED EXCLUDE_FROM_ALL sodium/sodium.c)
    set_target_properties(rocketnet_sodium_oracle PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_library(rocketnet_test_support STATIC sdk/rocketnet_tests.cpp)
    target_link_libraries(rocketnet_test_support PUBLIC rocketnet)
    rocketnet_use_sodium(rocketnet_test_support)
    # The filter epoch window's MAC tally (test_unit_filter_epochs.cpp) exists
    # only for the unit suite.
    target_compile_definitions(rocketnet PRIVATE ROCKETNET_FILTER_EPOCH_TEST_HOOKS=1)
    # The message layer's payload copy count (test_unit_unreliable_in_place.cpp)
    # exists only for the unit suite, like the filter epoch tally above.
    target_compile_definitions(rocketnet PRIVATE ROCKETNET_MESSAGE_COPY_TEST_HOOKS=1)
    # Every test_unit_<topic>.cpp at the repository root registers with
    # REGISTER_UNIT_TEST from test_unit_registry.h and is built into ./bin/test.
    file( GLOB ROCKETNET_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_unit_*.cpp" )
    add_executable(test test.cpp test_channels.cpp ${ROCKETNET_UNIT_TEST_SOURCES})
    target_compile_definitions(test PRIVATE ROCKETNET_FILTER_EPOCH_TEST_HOOKS=1 ROCKETNET_MESSAGE_COPY_TEST_HOOKS=1)
    if(WIN32)
        # The RIO overflow seam (test_unit_rio_overflow_seam.cpp) compiles into
        # the library in test builds, matching the filter-epoch seam above, so
        # the production shared libraries never carry it.
        target_compile_definitions(rocketnet PRIVATE ROCKETNET_RIO_OVERFLOW_TEST_HOOKS=1)
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
    target_link_libraries(test PRIVATE rocketnet_test_support Threads::Threads)
    target_compile_features(test PRIVATE cxx_std_11)
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
        target_link_libraries(test PRIVATE rocketnet_xdp_filter_host)
        target_compile_definitions(test PRIVATE ROCKETNET_XDP_FILTER_HOST=1)
        # xdp_host_every_counter_has_a_case reads sdk/rocketnet_wire.h and its
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
    # library's token writer through dev_connect_token_timeout_is_the_named_constant.
    add_dependencies(test rocketnet_shared rocketnet_shared_dev)
    foreach(test_target test_lifecycle test_driver)
        add_executable(${test_target} ${test_target}.cpp)
        target_include_directories(${test_target} PRIVATE test)
        target_link_libraries(${test_target} PRIVATE rocketnet)
    endforeach()
    add_executable(test_serialize test_serialize.cpp)
    target_include_directories(test_serialize PRIVATE test)
    target_link_libraries(test_serialize PRIVATE rocketnet_dev)
    # Compiles the server transport itself and runs on development keys, so it needs the development library.
    add_executable(test_transport_control test_transport_control.cpp)
    target_include_directories(test_transport_control PRIVATE test)
    target_link_libraries(test_transport_control PRIVATE rocketnet_dev)
    rocketnet_use_sodium(test_transport_control)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_xdp test_xdp.cpp sdk/rocketnet_server_socket_linux.cpp
            sdk/rocketnet.cpp sdk/rocketnet_platform.cpp sdk/rocketnet_address.cpp sdk/rocketnet_hash.cpp ${ROCKETNET_SODIUM_SOURCE})
        add_dependencies(test_xdp rocketnet_xdp_header)
        target_compile_definitions(test_xdp PRIVATE ROCKETNET_XDP_TESTS
            $<TARGET_PROPERTY:rocketnet,COMPILE_DEFINITIONS>)
        target_include_directories(test_xdp PRIVATE sdk "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_features(test_xdp PRIVATE cxx_std_11)
        target_compile_options(test_xdp PRIVATE $<TARGET_PROPERTY:rocketnet,COMPILE_OPTIONS>)
        rocketnet_use_sodium(test_xdp)
        target_link_libraries(test_xdp PRIVATE RocketNet::Bpf RocketNet::Xdp Threads::Threads)
        # test_xdp_filter loads server_xdp/server_xdp.c and runs crafted frames
        # through it with BPF_PROG_TEST_RUN (root or CAP_BPF + CAP_NET_ADMIN to
        # run). The object is built with the command server_xdp/Makefile uses.
        find_program(ROCKETNET_CLANG clang)
        if(ROCKETNET_CLANG)
            execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE ROCKETNET_MULTIARCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            set(ROCKETNET_XDP_FILTER_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/server_xdp_filter_test.o")
            add_custom_command(OUTPUT "${ROCKETNET_XDP_FILTER_OBJECT}"
                COMMAND "${ROCKETNET_CLANG}" -D__linux__ -D__BPF__ -ffreestanding -O2 -g
                    "-I/usr/include/${ROCKETNET_MULTIARCH}" "-I${CMAKE_CURRENT_SOURCE_DIR}/sdk"
                    -target bpf -c "${CMAKE_CURRENT_SOURCE_DIR}/server_xdp/server_xdp.c"
                    -o "${ROCKETNET_XDP_FILTER_OBJECT}"
                DEPENDS ${ROCKETNET_XDP_SOURCES}
                VERBATIM)
            add_custom_target(rocketnet_xdp_filter_object DEPENDS "${ROCKETNET_XDP_FILTER_OBJECT}")
            add_executable(test_xdp_filter test_xdp_filter.cpp)
            add_dependencies(test_xdp_filter rocketnet_xdp_filter_object)
            target_compile_definitions(test_xdp_filter PRIVATE
                "ROCKETNET_XDP_FILTER_OBJECT=\"${ROCKETNET_XDP_FILTER_OBJECT}\"")
            target_include_directories(test_xdp_filter PRIVATE sdk)
            target_compile_features(test_xdp_filter PRIVATE cxx_std_11)
            target_link_libraries(test_xdp_filter PRIVATE RocketNet::Bpf)
        else()
            message(STATUS "clang not found: test_xdp_filter is not built")
        endif()
    endif()
    if(APPLE)
        add_executable(test_msgx test_msgx.cpp)
        target_link_libraries(test_msgx PRIVATE rocketnet)
    endif()
    if(WIN32)
        # Isolate the test-instrumented driver: never link another copy from
        # the production archive. Only its ordinary runtime dependencies enter.
        add_executable(test_iocp test_iocp.cpp sdk/rocketnet_server_iocp_windows.cpp
            sdk/rocketnet.cpp sdk/rocketnet_platform.cpp sdk/rocketnet_address.cpp sdk/rocketnet_hash.cpp ${ROCKETNET_SODIUM_SOURCE})
        target_compile_definitions(test_iocp PRIVATE ROCKETNET_IOCP_TESTS
            $<TARGET_PROPERTY:rocketnet,COMPILE_DEFINITIONS>)
        target_include_directories(test_iocp PRIVATE sdk)
        target_compile_features(test_iocp PRIVATE cxx_std_11)
        target_compile_options(test_iocp PRIVATE $<TARGET_PROPERTY:rocketnet,COMPILE_OPTIONS>)
        rocketnet_use_sodium(test_iocp)
        target_link_libraries(test_iocp PRIVATE Threads::Threads)
    endif()
    file( GLOB ROCKETNET_CHANNEL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/functional/channel_*.cpp" )
    add_executable(functional
        functional/main.cpp
        functional/rocketnet_functional_tests.cpp
        functional/lifecycle_driver.cpp
        functional/cpp_transports.cpp
        ${ROCKETNET_CHANNEL_TEST_SOURCES})
    target_link_libraries(functional PRIVATE rocketnet_dev Threads::Threads)
    target_compile_features(functional PRIVATE cxx_std_11)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_dependencies(functional test_xdp test_serialize test_transport_control)
    elseif(APPLE)
        add_dependencies(functional test_msgx test_serialize test_transport_control)
    elseif(WIN32)
        add_dependencies(functional test_iocp test_serialize test_transport_control)
    else()
        add_dependencies(functional test_serialize test_transport_control)
    endif()
    add_library(rocketnet_abi_mismatch SHARED EXCLUDE_FROM_ALL tools/abi_mismatch.cpp)
    target_compile_definitions(rocketnet_abi_mismatch PRIVATE ROCKETNET_SHARED ROCKETNET_EXPORT ROCKETNET_ABI_QUERIES_ONLY)
    target_compile_features(rocketnet_abi_mismatch PRIVATE cxx_std_11)
    set_target_properties(rocketnet_abi_mismatch PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    add_library(rocketnet_abi_legacy SHARED EXCLUDE_FROM_ALL tools/abi_legacy.cpp)
    target_compile_definitions(rocketnet_abi_legacy PRIVATE ROCKETNET_SHARED ROCKETNET_EXPORT)
endif()
if(ROCKETNET_BUILD_TOOLS)
    add_executable(soak soak.c)
    add_executable(load load.c)
    add_executable(soak_sim soak_sim.cpp)
    add_executable(keygen keygen.cpp)
    add_executable(transport_memory bench/transport_memory.cpp)
    add_executable(transport_fill bench/transport_fill.cpp)
    add_executable(client_main_thread_cost bench/client_main_thread_cost.cpp)
    add_executable(lease_cost bench/lease_cost.cpp)
    target_link_libraries(keygen PRIVATE rocketnet)
    foreach(tool soak load soak_sim transport_memory transport_fill client_main_thread_cost lease_cost)
        target_link_libraries(${tool} PRIVATE rocketnet_dev)
    endforeach()
    target_link_libraries(lease_cost PRIVATE Threads::Threads)
    rocketnet_use_sodium(soak_sim)
    rocketnet_use_sodium(keygen)
    # The corpus replay drivers: fuzz/replay_main.cpp provides the main the
    # libFuzzer runtime would provide, reading each named file whole and
    # calling the harness's LLVMFuzzerTestOneInput once, so the committed
    # corpora replay on platforms with no libFuzzer runtime. Built against
    # the development library with the same includes as the NDEBUG fuzz
    # build in ci.yml.
    foreach(harness rocketnet_fuzz rocketnet_fuzz_message rocketnet_fuzz_connect_token rocketnet_fuzz_receive_structured)
        add_executable(fuzz_replay_${harness} fuzz/replay_main.cpp fuzz/${harness}.cpp)
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
install(FILES sdk/rocketnet.h sdk/rocketnet.hpp sdk/rocketnet_address.h sdk/rocketnet_connect_token.h
    sdk/rocketnet_platform.h sdk/rocketnet_counters.h sdk/rocketnet_route.h
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
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(EXPORT RocketNetTargets NAMESPACE RocketNet:: DESTINATION ${ROCKETNET_PACKAGE_DIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfigVersion.cmake"
    cmake/RocketNetDependencies.cmake DESTINATION ${ROCKETNET_PACKAGE_DIR})

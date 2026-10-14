cmake_minimum_required(VERSION 3.20)
project(rocketnet VERSION 1.0.0 LANGUAGES C CXX)
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
option(ROCKETNET_BUILD_TESTS "Build the native tests and managed ABI fixtures" ${ROCKETNET_TOP_LEVEL})
option(ROCKETNET_BUILD_EXAMPLES "Build the client/server examples" ${ROCKETNET_TOP_LEVEL})
option(ROCKETNET_BUILD_TOOLS "Build keygen and soak tools" ${ROCKETNET_TOP_LEVEL})
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
    sdk/rocketnet_address.cpp
    sdk/rocketnet_base64.cpp
    sdk/rocketnet_client_protocol.cpp
    sdk/rocketnet_client_slots.cpp
    sdk/rocketnet_client_transport.cpp
    sdk/rocketnet_config.cpp
    sdk/rocketnet_driver.cpp
    sdk/rocketnet_driver_builtin.cpp
    sdk/rocketnet_connect_token.cpp
    sdk/rocketnet_counters.cpp
    sdk/rocketnet_encryption_map.cpp
    sdk/rocketnet_fragment.cpp
    sdk/rocketnet_frame_sync.cpp
    sdk/rocketnet_hash.cpp
    sdk/rocketnet_keys.cpp
    sdk/rocketnet_message.cpp
    sdk/rocketnet_unreliable.cpp
    sdk/rocketnet_network_simulator.cpp
    sdk/rocketnet_platform.cpp
    sdk/rocketnet_protocol.cpp
    sdk/rocketnet_replay.cpp
    sdk/rocketnet_server_gso_linux.cpp
    sdk/rocketnet_server_iocp_windows.cpp
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
# for the C# binding smoke, whose loopback connect mints its own token. Never
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
    OUTPUT_NAME rocketnet_dev
    VERSION ${PROJECT_VERSION} SOVERSION ${ROCKETNET_ABI_SOVERSION}
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
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
    endif()
endforeach()
if(ROCKETNET_HARDEN AND ROCKETNET_LINK_RELRO_NOW)
    target_link_options(rocketnet_shared PRIVATE "$<$<CONFIG:Release>:-Wl,-z,relro,-z,now>")
endif()

if(ROCKETNET_BUILD_EXAMPLES)
    add_executable(client client.c)
    target_link_libraries(client PRIVATE rocketnet_dev)
    add_executable(server server.c)
    target_link_libraries(server PRIVATE rocketnet)
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
    # Every test_unit_<topic>.cpp at the repository root registers with
    # REGISTER_UNIT_TEST from test_unit_registry.h and is built into ./bin/test.
    file( GLOB ROCKETNET_UNIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_unit_*.cpp" )
    add_executable(test test.cpp test_channels.cpp ${ROCKETNET_UNIT_TEST_SOURCES})
    target_link_libraries(test PRIVATE rocketnet_test_support Threads::Threads)
    target_compile_features(test PRIVATE cxx_std_11)
    # the driver-phase matrix compiles the server transport source (for its
    # fake-driver fixture) and so needs the vendored sodium header.
    rocketnet_use_sodium(test)
    # test_dev_token_minting_absent_in_release probes the shared release
    # library, so the test target must build it: CI legs build `test` alone,
    # and without this the shared library is missing and the probe goes red.
    add_dependencies(test rocketnet_shared)
    foreach(test_target test_lifecycle test_driver test_transport_control)
        add_executable(${test_target} ${test_target}.cpp)
        target_include_directories(${test_target} PRIVATE test)
        target_link_libraries(${test_target} PRIVATE rocketnet)
    endforeach()
    add_executable(test_serialize test_serialize.cpp)
    target_include_directories(test_serialize PRIVATE test)
    target_link_libraries(test_serialize PRIVATE rocketnet_dev)
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
    add_executable(functional
        functional/main.cpp
        functional/rocketnet_functional_tests.cpp
        functional/lifecycle_driver.cpp)
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
    target_compile_definitions(rocketnet_abi_mismatch PRIVATE ROCKETNET_SHARED ROCKETNET_EXPORT)
    target_compile_features(rocketnet_abi_mismatch PRIVATE cxx_std_11)
    set_target_properties(rocketnet_abi_mismatch PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    add_library(rocketnet_abi_legacy SHARED EXCLUDE_FROM_ALL tools/abi_legacy.cpp)
    target_compile_definitions(rocketnet_abi_legacy PRIVATE ROCKETNET_SHARED ROCKETNET_EXPORT)
endif()
if(ROCKETNET_BUILD_TOOLS)
    add_executable(soak soak.c)
    add_executable(soak_sim soak_sim.cpp)
    add_executable(keygen keygen.cpp)
    add_executable(transport_memory bench/transport_memory.cpp)
    add_executable(transport_fill bench/transport_fill.cpp)
    add_executable(lease_cost bench/lease_cost.cpp)
    foreach(tool keygen transport_memory)
        target_link_libraries(${tool} PRIVATE rocketnet)
    endforeach()
    foreach(tool soak soak_sim transport_fill lease_cost)
        target_link_libraries(${tool} PRIVATE rocketnet_dev)
    endforeach()
    target_link_libraries(lease_cost PRIVATE Threads::Threads)
    rocketnet_use_sodium(soak_sim)
    rocketnet_use_sodium(keygen)
endif()

install(TARGETS rocketnet rocketnet_shared EXPORT RocketNetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES sdk/rocketnet.h sdk/rocketnet_address.h sdk/rocketnet_connect_token.h
    sdk/rocketnet_platform.h sdk/rocketnet_counters.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/rocketnet)
install(FILES sodium/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/rocketnet/sodium)
set(ROCKETNET_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/RocketNet")
configure_package_config_file(cmake/RocketNetConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfig.cmake"
    INSTALL_DESTINATION ${ROCKETNET_PACKAGE_DIR})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(EXPORT RocketNetTargets NAMESPACE RocketNet:: DESTINATION ${ROCKETNET_PACKAGE_DIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/RocketNetConfigVersion.cmake"
    cmake/RocketNetDependencies.cmake DESTINATION ${ROCKETNET_PACKAGE_DIR})

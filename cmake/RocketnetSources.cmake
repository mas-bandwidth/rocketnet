# The RocketNet SDK library's source files: the one list. CMakeLists.txt
# builds every library target from it, and an application that vendors sdk/
# includes this file and compiles the same list, so a new SDK file reaches
# every build at once. tools/check_sdk_sources.py refuses an sdk/*.cpp or
# sdk/*.c that is neither listed here nor named as excluded.
#
# Paths are relative to the RocketNet root; a consumer prepends its vendored
# root (list(TRANSFORM ROCKETNET_SDK_SOURCES PREPEND <root>/)). sodium/ is
# separate: CMakeLists.txt compiles sodium/sodium.c unless the build uses a
# system libsodium.
#
# A platform file compiles to nothing off its platform
# (ROCKETNET_PLATFORM guards in each), so the library compiles every list on
# every platform: ROCKETNET_SDK_SOURCES is all of them, and a consumer
# compiles exactly that.

set(ROCKETNET_SDK_SOURCES_COMMON
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
    sdk/rocketnet_server_protocol.cpp
    sdk/rocketnet_server_transport.cpp
    sdk/rocketnet_token_tables.cpp)

set(ROCKETNET_SDK_SOURCES_LINUX
    sdk/rocketnet_client_gso_linux.cpp
    sdk/rocketnet_gateway_netlink_linux.cpp
    sdk/rocketnet_server_gso_linux.cpp
    sdk/rocketnet_server_uring_linux.cpp
    sdk/rocketnet_server_socket_linux.cpp)

# No Apple-only source today: macOS and iOS use the common sources.
set(ROCKETNET_SDK_SOURCES_APPLE)

set(ROCKETNET_SDK_SOURCES_WINDOWS
    sdk/rocketnet_server_iocp_windows.cpp
    sdk/rocketnet_server_rio_windows.cpp)

set(ROCKETNET_SDK_SOURCES
    ${ROCKETNET_SDK_SOURCES_COMMON}
    ${ROCKETNET_SDK_SOURCES_LINUX}
    ${ROCKETNET_SDK_SOURCES_APPLE}
    ${ROCKETNET_SDK_SOURCES_WINDOWS})

# sdk/ files that are not library sources: the unit suite's entry points,
# compiled into the test binary only.
set(ROCKETNET_SDK_SOURCES_EXCLUDED
    sdk/rocketnet_tests.cpp)

# Let tools/check_sdk_sources.py read the same lists the build compiles.
if(DEFINED ROCKETNET_SDK_SOURCE_LIST_OUTPUT)
    file(WRITE "${ROCKETNET_SDK_SOURCE_LIST_OUTPUT}" "")
    foreach(group COMMON LINUX APPLE WINDOWS EXCLUDED)
        foreach(source IN LISTS ROCKETNET_SDK_SOURCES_${group})
            file(APPEND "${ROCKETNET_SDK_SOURCE_LIST_OUTPUT}" "${group} ${source}\n")
        endforeach()
    endforeach()
endif()

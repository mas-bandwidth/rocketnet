# Every examples/*.c builds as example_<name>. Most use the development
# library to mint development tokens locally. The backend examples use the
# selected library mode and can connect with production backend keys.
file( GLOB ROCKETNET_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c" )
foreach( example_source IN LISTS ROCKETNET_EXAMPLE_SOURCES )
    get_filename_component( example_name "${example_source}" NAME_WE )
    add_executable( example_${example_name} "${example_source}" )
    if(example_name STREQUAL "backend_client" OR example_name STREQUAL "backend_server")
        # These examples accept real backend keys and exercise the selected build mode.
        target_link_libraries( example_${example_name} PRIVATE rocketnet )
    else()
        target_link_libraries( example_${example_name} PRIVATE rocketnet_dev )
    endif()
endforeach()

# The soak allocator telemetry uses a native lock across callback threads.
target_link_libraries( example_soak PRIVATE Threads::Threads )

# The server examples split off the second half of the network update onto a
# platform thread (examples/thread.h) and join it at the start of the next frame.
target_link_libraries( example_backend_server PRIVATE Threads::Threads )
target_link_libraries( example_client_server_server PRIVATE Threads::Threads )
target_link_libraries( example_game_loop PRIVATE Threads::Threads )

# backend_check_token runs the library's own token checks on a token one of
# the backends in examples/backend/ minted (tools/run_backend_examples.py). It
# is a test of the backends, not an example, so it may use the library's
# internal headers.
add_executable( backend_check_token "${CMAKE_CURRENT_SOURCE_DIR}/backend/check_token.cpp" )
target_link_libraries( backend_check_token PRIVATE rocketnet_dev )
